#include <Arduino.h>
#include <ArduinoJson.h>
#include <Redis.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>

#include "secrets.h"
#include "../common/optical_protocol.h"

#define LASER_PIN 4
#define BAUD_RATE 250000
#define REDIS_CHANNEL "laser_commands"
#define REDIS_RECONNECT_MS 5000
#define IMAGE_PROGRESS_LOG_INTERVAL 32
#define TX_BUFFER_BYTES (OPTICAL_MAX_FRAME + 256)
#define MAX_IMAGE_ID_BYTES 64

WiFiClientSecure redisClient;
unsigned long nextRedisAttempt = 0;

uint8_t imageChunkBuffer[OPTICAL_MAX_CHUNK];
uint8_t opticalFrame[OPTICAL_MAX_FRAME];
JsonDocument commandDocument;

bool imageSendActive = false;
uint16_t expectedImageChunks = 0;
uint16_t nextImageChunk = 0;
uint32_t expectedImageBytes = 0;
uint32_t sentImageBytes = 0;
uint16_t nextTextSequence = 0;
char currentImageId[MAX_IMAGE_ID_BYTES + 1] = {0};

bool validSha256(const char* value) {
  if (value == nullptr || strlen(value) != 64) return false;
  for (uint8_t index = 0; index < 64; index++) {
    const char c = value[index];
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
          (c >= 'A' && c <= 'F'))) {
      return false;
    }
  }
  return true;
}

bool sendOpticalFrame(OpticalFrameType type, uint16_t sequence,
                      const uint8_t* payload, uint16_t length) {
  const size_t frameLength =
      opticalBuildFrame(opticalFrame, type, sequence, payload, length);
  if (frameLength == 0) return false;

  const size_t written = Serial1.write(opticalFrame, frameLength);
  Serial1.flush();
  if (written != frameLength) {
    Serial.printf("FRAME FAILED reason=short_write wrote=%u expected=%u\n",
                  (unsigned)written, (unsigned)frameLength);
    return false;
  }
  return true;
}

void sendOpticalText(const String& payload) {
  if (payload.length() == 0 || payload.length() > OPTICAL_MAX_PAYLOAD) {
    Serial.println("TEXT FAILED reason=length");
    return;
  }

  const uint16_t sequence = nextTextSequence++;
  if (!sendOpticalFrame(FRAME_TEXT, sequence,
                        (const uint8_t*)payload.c_str(),
                        (uint16_t)payload.length())) {
    Serial.printf("TEXT FAILED sequence=%u reason=send\n", sequence);
    return;
  }
  Serial.printf("TEXT SENT sequence=%u bytes=%u\n", sequence,
                (unsigned)payload.length());
}

int base64Value(char value) {
  if (value >= 'A' && value <= 'Z') return value - 'A';
  if (value >= 'a' && value <= 'z') return value - 'a' + 26;
  if (value >= '0' && value <= '9') return value - '0' + 52;
  if (value == '+') return 62;
  if (value == '/') return 63;
  return -1;
}

// Accept only canonical RFC 4648 Base64: complete quartets, padding only in
// the final quartet, and zero unused bits. Whitespace is intentionally invalid.
bool decodeBase64Strict(const char* input, uint8_t* output,
                        size_t outputLimit, size_t& outputLength) {
  outputLength = 0;
  if (input == nullptr) return false;
  const size_t inputLength = strlen(input);
  if (inputLength == 0 || inputLength % 4 != 0) return false;

  for (size_t offset = 0; offset < inputLength; offset += 4) {
    const bool finalQuartet = offset + 4 == inputLength;
    const int a = base64Value(input[offset]);
    const int b = base64Value(input[offset + 1]);
    if (a < 0 || b < 0) return false;

    const char cChar = input[offset + 2];
    const char dChar = input[offset + 3];
    const int c = base64Value(cChar);
    const int d = base64Value(dChar);

    if (cChar == '=') {
      if (!finalQuartet || dChar != '=' || (b & 0x0F) != 0 ||
          outputLength + 1 > outputLimit) {
        return false;
      }
      output[outputLength++] = (uint8_t)((a << 2) | (b >> 4));
      continue;
    }

    if (c < 0) return false;
    if (dChar == '=') {
      if (!finalQuartet || (c & 0x03) != 0 ||
          outputLength + 2 > outputLimit) {
        return false;
      }
      output[outputLength++] = (uint8_t)((a << 2) | (b >> 4));
      output[outputLength++] = (uint8_t)((b << 4) | (c >> 2));
      continue;
    }

    if (d < 0 || outputLength + 3 > outputLimit) return false;
    output[outputLength++] = (uint8_t)((a << 2) | (b >> 4));
    output[outputLength++] = (uint8_t)((b << 4) | (c >> 2));
    output[outputLength++] = (uint8_t)((c << 6) | d);
  }
  return outputLength > 0;
}

void rejectImage(const char* reason) {
  Serial.printf("IMAGE SEND FAILED id=%s reason=%s bytes=%lu chunks=%u\n",
                currentImageId[0] ? currentImageId : "unknown", reason,
                (unsigned long)sentImageBytes, nextImageChunk);
}

bool beginImage(JsonDocument& document) {
  const char* id = document["id"] | "";
  const char* mimeType = document["mimeType"] | "";
  const unsigned long totalBytes = document["totalBytes"] | 0UL;
  const unsigned long chunkCount = document["chunkCount"] | 0UL;

  if (id[0] == '\0' || strlen(id) > MAX_IMAGE_ID_BYTES ||
      mimeType[0] == '\0' || strlen(mimeType) > 64 ||
      totalBytes == 0 || chunkCount == 0 || chunkCount > UINT16_MAX ||
      chunkCount != ((totalBytes - 1) / OPTICAL_MAX_CHUNK) + 1) {
    Serial.println("IMAGE SEND FAILED id=unknown reason=start_metadata");
    return false;
  }

  JsonDocument metadata;
  metadata["id"] = id;
  metadata["totalBytes"] = totalBytes;
  metadata["chunkCount"] = chunkCount;
  metadata["mimeType"] = mimeType;
  String payload;
  serializeJson(metadata, payload);
  if (payload.length() == 0 || payload.length() > OPTICAL_MAX_PAYLOAD ||
      !sendOpticalFrame(FRAME_IMAGE_START, 0,
                        (const uint8_t*)payload.c_str(),
                        (uint16_t)payload.length())) {
    Serial.printf("IMAGE SEND FAILED id=%s reason=start_send\n", id);
    return false;
  }

  strncpy(currentImageId, id, MAX_IMAGE_ID_BYTES);
  currentImageId[MAX_IMAGE_ID_BYTES] = '\0';
  expectedImageBytes = (uint32_t)totalBytes;
  expectedImageChunks = (uint16_t)chunkCount;
  sentImageBytes = 0;
  nextImageChunk = 0;
  imageSendActive = true;
  Serial.printf("IMAGE START SENT id=%s bytes=%lu chunks=%u\n",
                currentImageId, (unsigned long)expectedImageBytes,
                expectedImageChunks);
  return true;
}

void sendImageChunk(JsonDocument& document) {
  const unsigned long sequenceValue = document["index"] | ULONG_MAX;
  if (!imageSendActive || sequenceValue > UINT16_MAX ||
      (uint16_t)sequenceValue != nextImageChunk) {
    rejectImage("sequence");
    return;
  }

  size_t decodedLength = 0;
  if (!decodeBase64Strict(document["data"] | "", imageChunkBuffer,
                          sizeof(imageChunkBuffer), decodedLength)) {
    rejectImage("base64");
    return;
  }

  const uint32_t remaining = expectedImageBytes - sentImageBytes;
  const size_t expectedLength =
      remaining > OPTICAL_MAX_CHUNK ? OPTICAL_MAX_CHUNK : remaining;
  if (decodedLength != expectedLength ||
      !sendOpticalFrame(FRAME_IMAGE_CHUNK, nextImageChunk,
                        imageChunkBuffer, (uint16_t)decodedLength)) {
    rejectImage(decodedLength != expectedLength ? "chunk_length" : "chunk_send");
    return;
  }

  sentImageBytes += decodedLength;
  nextImageChunk++;
  if (nextImageChunk % IMAGE_PROGRESS_LOG_INTERVAL == 0 ||
      nextImageChunk == expectedImageChunks) {
    Serial.printf("IMAGE CHUNKS SENT id=%s chunks=%u/%u bytes=%lu/%lu\n",
                  currentImageId, nextImageChunk, expectedImageChunks,
                  (unsigned long)sentImageBytes,
                  (unsigned long)expectedImageBytes);
  }
}

void endImage(JsonDocument& document) {
  const char* id = document["id"] | "";
  const char* sha256 = document["sha256"] | "";
  if (!imageSendActive || strcmp(id, currentImageId) != 0 ||
      !validSha256(sha256) ||
      nextImageChunk != expectedImageChunks ||
      sentImageBytes != expectedImageBytes) {
    rejectImage("end_state");
    return;
  }

  JsonDocument metadata;
  metadata["id"] = currentImageId;
  metadata["sha256"] = sha256;
  String payload;
  serializeJson(metadata, payload);
  if (!sendOpticalFrame(FRAME_IMAGE_END, expectedImageChunks,
                        (const uint8_t*)payload.c_str(),
                        (uint16_t)payload.length())) {
    rejectImage("end_send");
    return;
  }

  Serial.printf("IMAGE END SENT id=%s bytes=%lu chunks=%u\n",
                currentImageId, (unsigned long)sentImageBytes,
                nextImageChunk);
  imageSendActive = false;
  currentImageId[0] = '\0';
  expectedImageChunks = 0;
  nextImageChunk = 0;
  expectedImageBytes = 0;
  sentImageBytes = 0;
}

void handleCommand(const String& json) {
  commandDocument.clear();
  const DeserializationError error = deserializeJson(commandDocument, json);
  if (error) {
    Serial.printf("COMMAND REJECTED reason=json detail=%s\n", error.c_str());
    return;
  }

  const char* type = commandDocument["type"] | "";
  if (strcmp(type, "text") == 0) {
    sendOpticalText(commandDocument["payload"].as<String>());
  } else if (strcmp(type, "image_start") == 0) {
    beginImage(commandDocument);
  } else if (strcmp(type, "image_chunk") == 0) {
    sendImageChunk(commandDocument);
  } else if (strcmp(type, "image_end") == 0) {
    endImage(commandDocument);
  } else {
    Serial.println("COMMAND REJECTED reason=type");
  }
}

void messageCallback(Redis* redis, String channel, String message) {
  if (channel == REDIS_CHANNEL) handleCommand(message);
}

void errorCallback(Redis* redis, RedisMessageError error) {
  Serial.printf("Redis subscription error: %d\n", (int)error);
}

bool connectAndSubscribe() {
  if (WiFi.status() != WL_CONNECTED) return false;

  if (strcmp(UPSTASH_REDIS_HOST, "your-database.upstash.io") == 0 ||
      strcmp(UPSTASH_REDIS_TOKEN, "your-upstash-token") == 0) {
    Serial.println("Redis credentials are placeholders in sender/secrets.h");
    return false;
  }

  redisClient.setInsecure();
  redisClient.setTimeout(10000);
  redisClient.setHandshakeTimeout(10);
  Serial.printf("Connecting to Redis: %s:%d\n", UPSTASH_REDIS_HOST,
                UPSTASH_REDIS_PORT);

  if (!redisClient.connect(UPSTASH_REDIS_HOST, UPSTASH_REDIS_PORT)) {
    char errorBuffer[128] = {0};
    const int errorCode =
        redisClient.lastError(errorBuffer, sizeof(errorBuffer));
    Serial.printf("Redis TLS connection failed code=%d detail=%s\n",
                  errorCode, errorBuffer);
    redisClient.stop();
    return false;
  }

  Redis redis(redisClient);
  if (redis.authenticate(UPSTASH_REDIS_TOKEN) != RedisSuccess) {
    Serial.println("Redis authentication failed");
    redisClient.stop();
    return false;
  }
  if (!redis.subscribe(REDIS_CHANNEL)) {
    Serial.println("Redis channel subscription failed");
    redisClient.stop();
    return false;
  }

  Serial.println("Redis subscribed channel=laser_commands");
  const RedisSubscribeResult result =
      redis.startSubscribing(messageCallback, errorCallback);
  Serial.printf("Redis subscription ended result=%d\n", (int)result);
  redisClient.stop();
  return false;
}

void connectWiFi() {
  if (WiFi.status() == WL_CONNECTED) return;

  Serial.printf("Connecting to Wi-Fi: %s\n", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.printf("\nWi-Fi connected: %s\n",
                WiFi.localIP().toString().c_str());
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\nTERAHERTZ OPTICAL SENDER");
  Serial.printf("Protocol v%u baud=%u chunk=%u payload=%u\n",
                OPTICAL_VERSION, BAUD_RATE, OPTICAL_MAX_CHUNK,
                OPTICAL_MAX_PAYLOAD);

  Serial1.setTxBufferSize(TX_BUFFER_BYTES);
  Serial1.begin(BAUD_RATE, SERIAL_8N1, -1, LASER_PIN, true);
  connectWiFi();
}

void loop() {
  if (WiFi.status() != WL_CONNECTED) {
    redisClient.stop();
    connectWiFi();
  }

  if (!redisClient.connected() && millis() >= nextRedisAttempt) {
    nextRedisAttempt = millis() + REDIS_RECONNECT_MS;
    connectAndSubscribe();
  }
  delay(10);
}
