#include <Arduino.h>
#include <ArduinoJson.h>
#include <Redis.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>

#include "secrets.h"
#include "../common/optical_protocol.h"

#define LASER_PIN 4
#define BAUD_RATE 230400
#define REDIS_CHANNEL "laser_commands"
#define REDIS_RECONNECT_MS 5000
#define MAX_OPTICAL_TEXT_BYTES OPTICAL_MAX_PAYLOAD
#define IMAGE_CHUNK_BUFFER_BYTES OPTICAL_MAX_PAYLOAD
#define IMAGE_PROGRESS_LOG_INTERVAL 32
// Queue a whole frame ahead of the wire so the UART never idles between the
// header and the payload.
#define TX_BUFFER_BYTES (OPTICAL_MAX_FRAME + 256)

WiFiClientSecure redisClient;
unsigned long nextRedisAttempt = 0;

uint8_t imageChunkBuffer[IMAGE_CHUNK_BUFFER_BYTES];
uint8_t opticalFrame[OPTICAL_MAX_FRAME];
uint16_t expectedImageChunks = 0;
JsonDocument commandDocument;

// One contiguous write instead of a dozen individually locked byte writes,
// then block until the frame has physically left the UART. That flush is the
// backpressure that keeps the TCP receive window closed while the laser is
// busy, which is what paces the whole transfer.
bool sendOpticalFrame(OpticalFrameType type, uint16_t sequence,
                      const uint8_t* payload, uint16_t length) {
  const size_t frameLength = opticalBuildFrame(opticalFrame, type, sequence, payload, length);
  if (frameLength == 0) return false;
  Serial1.write(opticalFrame, frameLength);
  Serial1.flush();
  return true;
}

void sendOpticalText(const String& payload) {
  if (payload.length() == 0 || payload.length() > MAX_OPTICAL_TEXT_BYTES) {
    Serial.println("[REJECTED] Text exceeds optical buffer limit");
    return;
  }

  const unsigned long startedAt = millis();
  if (!sendOpticalFrame(FRAME_TEXT, 0, (const uint8_t*)payload.c_str(),
                        (uint16_t)payload.length())) {
    Serial.println("[REJECTED] Text frame could not be built");
    return;
  }
  Serial.printf("UART frame sent: TEXT %u bytes in %lums\n",
                (unsigned)payload.length(), millis() - startedAt);
}

int base64Value(char value) {
  if (value >= 'A' && value <= 'Z') return value - 'A';
  if (value >= 'a' && value <= 'z') return value - 'a' + 26;
  if (value >= '0' && value <= '9') return value - '0' + 52;
  if (value == '+') return 62;
  if (value == '/') return 63;
  return -1;
}

size_t decodeBase64(const char* input, uint8_t* output, size_t outputLimit) {
  size_t outputLength = 0;
  uint32_t accumulator = 0;
  uint8_t bits = 0;

  for (size_t i = 0; input[i] != '\0'; i++) {
    const int value = base64Value(input[i]);
    if (value < 0) continue;

    // Keep only the bits needed for the next output byte. Without this
    // bound, the accumulator overflows after a few Base64 characters.
    accumulator = ((accumulator << 6) | (uint32_t)value) & 0xFFFFFF;
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      if (outputLength >= outputLimit) return 0;
      output[outputLength++] = (uint8_t)((accumulator >> bits) & 0xFF);
    }
  }

  return outputLength;
}

void sendImageCommand(JsonDocument& document) {
  const char* type = document["type"] | "";
  const uint16_t sequence = document["index"] | 0;
  if (strcmp(type, "image_start") == 0) {
    expectedImageChunks = document["chunkCount"] | 0;
  }

  if (strcmp(type, "image_chunk") == 0) {
    const size_t decoded = decodeBase64(document["data"] | "", imageChunkBuffer, sizeof(imageChunkBuffer));
    if (decoded == 0 ||
        !sendOpticalFrame(FRAME_IMAGE_CHUNK, sequence, imageChunkBuffer, (uint16_t)decoded)) {
      Serial.println("[REJECTED] Image chunk is too large or invalid");
      return;
    }
    // USB logging costs ~4 ms per line at 115200 and sits in the hot path, so
    // only report periodically.
    if (sequence % IMAGE_PROGRESS_LOG_INTERVAL == 0 || sequence + 1 == expectedImageChunks) {
      Serial.printf("UART frame sent: IMAGE_CHUNK %u/%u bytes=%u\n", sequence,
                    expectedImageChunks, (unsigned)decoded);
    }
    return;
  }

  String metadata;
  serializeJson(document, metadata);
  const OpticalFrameType frameType = strcmp(type, "image_start") == 0 ? FRAME_IMAGE_START : FRAME_IMAGE_END;
  if (!sendOpticalFrame(frameType, sequence, (const uint8_t*)metadata.c_str(),
                        (uint16_t)metadata.length())) {
    Serial.println("[REJECTED] Image metadata is too large");
    return;
  }
  Serial.printf("UART frame sent: %s\n", type);
  if (strcmp(type, "image_end") == 0) {
    expectedImageChunks = 0;
  }
}

void handleCommand(const String& json) {
  commandDocument.clear();
  const DeserializationError error = deserializeJson(commandDocument, json);
  JsonDocument& document = commandDocument;
  if (error) {
    Serial.printf("[REJECTED] Invalid command JSON: %s\n", error.c_str());
    return;
  }

  const char* id = document["id"] | "unknown";
  const char* type = document["type"] | "";
  if (strcmp(type, "image_chunk") != 0) {
    Serial.printf("Redis message received: id=%s type=%s\n", id, type);
  }

  if (strcmp(type, "text") == 0) {
    sendOpticalText(document["payload"].as<String>());
  } else if (strcmp(type, "image_start") == 0 || strcmp(type, "image_chunk") == 0 || strcmp(type, "image_end") == 0) {
    sendImageCommand(document);
  } else {
    Serial.println("[REJECTED] Unknown command type");
  }
}

void messageCallback(Redis* redis, String channel, String message) {
  if (channel != REDIS_CHANNEL) return;
  handleCommand(message);
}

void errorCallback(Redis* redis, RedisMessageError error) {
  Serial.printf("Redis subscription error: %d\n", (int)error);
}

bool connectAndSubscribe() {
  if (WiFi.status() != WL_CONNECTED) return false;

  if (strcmp(UPSTASH_REDIS_HOST, "your-database.upstash.io") == 0 ||
      strcmp(UPSTASH_REDIS_TOKEN, "your-upstash-token") == 0) {
    Serial.println("Redis credentials are still placeholders in sender/secrets.h");
    return false;
  }

  redisClient.setInsecure(); // Internal demo; pin a CA certificate before production use.
  redisClient.setTimeout(10000);
  redisClient.setHandshakeTimeout(10);
  Serial.printf("Connecting to Redis: %s:%d\n", UPSTASH_REDIS_HOST, UPSTASH_REDIS_PORT);

  if (!redisClient.connect(UPSTASH_REDIS_HOST, UPSTASH_REDIS_PORT)) {
    char errorBuffer[128] = {0};
    const int errorCode = redisClient.lastError(errorBuffer, sizeof(errorBuffer));
    Serial.printf("Redis TLS connection failed code=%d detail=%s\n", errorCode, errorBuffer);
    redisClient.stop();
    return false;
  }
  Serial.println("Connected to Redis over TLS");

  Redis redis(redisClient);
  if (redis.authenticate(UPSTASH_REDIS_TOKEN) != RedisSuccess) {
    Serial.println("Redis authentication failed");
    redisClient.stop();
    return false;
  }
  Serial.println("Authenticated successfully");

  if (!redis.subscribe(REDIS_CHANNEL)) {
    Serial.println("Redis channel subscription failed");
    redisClient.stop();
    return false;
  }
  Serial.println("Subscribed to laser_commands; waiting for commands");

  const RedisSubscribeResult result = redis.startSubscribing(messageCallback, errorCallback);
  Serial.printf("Redis subscription ended with result: %d\n", (int)result);
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
  Serial.printf("\nWi-Fi connected: %s\n", WiFi.localIP().toString().c_str());
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\nTERAHERTZ CLOUD COMMAND SENDER");

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
