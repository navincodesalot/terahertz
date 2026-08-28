#include <Arduino.h>
#include <ArduinoJson.h>
#include <Redis.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>

#include "secrets.h"
#include "../common/optical_protocol.h"

#define LASER_PIN          4
#define BAUD_RATE          250000
#define REDIS_CHANNEL      "laser_commands"
#define REDIS_RECONNECT_MS 5000

WiFiClientSecure redisClient;
unsigned long nextRedisAttempt = 0;
uint16_t expectedImageChunks = 0;

void sendOpticalText(const char* id, const String& payload) {
  if (payload.length() == 0 || payload.length() > MAX_TRANSFER_BYTES) {
    Serial.println("[REJECTED] Text exceeds the 250 KB transfer limit");
    return;
  }

  char crcHex[9];
  opticalCrc32Hex((const uint8_t*)payload.c_str(), payload.length(), crcHex);

  // The receiver uses the third separator to find the payload, so '|'
  // characters inside the text remain valid.
  String line = "THZTXT|";
  line += id;
  line += '|';
  line += crcHex;
  line += '|';
  line += payload;
  Serial1.println(line);

  Serial.printf("UART text sent: %u bytes crc=%s\n",
                payload.length(), crcHex);
}

void sendImageCommand(JsonDocument& document) {
  const char* type = document["type"] | "";
  const char* id = document["id"] | "unknown";

  if (strcmp(type, "image_start") == 0) {
    expectedImageChunks = document["chunkCount"] | 0;

    // Keep the proven optical startup workaround.
    Serial1.println();
    delay(20);

    String line = "THZIMG|S|";
    line += id;
    line += '|';
    line += (document["chunkCount"] | 0);
    line += '|';
    line += (document["totalBytes"] | 0);
    line += '|';
    line += (document["mimeType"] | "application/octet-stream");
    Serial1.println(line);
    Serial.printf("UART image start: chunks=%u bytes=%u\n",
                  expectedImageChunks,
                  (unsigned)(document["totalBytes"] | 0));
    return;
  }

  if (strcmp(type, "image_chunk") == 0) {
    const uint16_t sequence = document["index"] | 0;
    const char* encoded = document["data"] | "";
    char crcHex[9];
    opticalCrc32Hex((const uint8_t*)encoded, strlen(encoded), crcHex);

    String line = "THZIMG|C|";
    line += id;
    line += '|';
    line += sequence;
    line += '|';
    line += encoded;
    line += '|';
    line += crcHex;
    Serial1.println(line);
    Serial.printf("UART image chunk: index=%u/%u crc=%s\n",
                  sequence, expectedImageChunks, crcHex);
    return;
  }

  String line = "THZIMG|E|";
  line += id;
  line += '|';
  line += (document["sha256"] | "");
  Serial1.println(line);
  Serial.printf("UART image end: chunks=%u\n", expectedImageChunks);
  expectedImageChunks = 0;
}

void handleCommand(const String& json) {
  JsonDocument document;
  const DeserializationError error = deserializeJson(document, json);
  if (error) {
    Serial.printf("[REJECTED] Invalid JSON: %s\n", error.c_str());
    return;
  }

  const char* id = document["id"] | "unknown";
  const char* type = document["type"] | "";
  Serial.printf("Redis command: id=%s type=%s\n", id, type);

  if (strcmp(type, "text") == 0) {
    sendOpticalText(id, document["payload"].as<String>());
  } else if (strcmp(type, "image_start") == 0 ||
             strcmp(type, "image_chunk") == 0 ||
             strcmp(type, "image_end") == 0) {
    sendImageCommand(document);
  } else {
    Serial.println("[REJECTED] Unknown command type");
  }
}

// Keep the Redis callback simple and synchronous. This is the behavior that
// was already proven stable with the Arduino Redis library.
void messageCallback(Redis* redis, String channel, String message) {
  if (channel == REDIS_CHANNEL) handleCommand(message);
}

void errorCallback(Redis* redis, RedisMessageError error) {
  Serial.printf("Redis subscription error: %d\n", (int)error);
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

bool connectAndSubscribe() {
  if (WiFi.status() != WL_CONNECTED) return false;

  if (strcmp(UPSTASH_REDIS_HOST, "your-database.upstash.io") == 0 ||
      strcmp(UPSTASH_REDIS_TOKEN, "your-upstash-token") == 0) {
    Serial.println("Redis credentials are still placeholders in sender/secrets.h");
    return false;
  }

  redisClient.setInsecure();
  redisClient.setTimeout(10000);
  redisClient.setHandshakeTimeout(10);
  Serial.printf("Connecting to Redis: %s:%d\n",
                UPSTASH_REDIS_HOST, UPSTASH_REDIS_PORT);

  if (!redisClient.connect(UPSTASH_REDIS_HOST, UPSTASH_REDIS_PORT)) {
    char errorBuffer[128] = {0};
    const int errorCode = redisClient.lastError(errorBuffer, sizeof(errorBuffer));
    Serial.printf("Redis TLS connection failed code=%d detail=%s\n",
                  errorCode, errorBuffer);
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

  const RedisSubscribeResult result =
    redis.startSubscribing(messageCallback, errorCallback);
  Serial.printf("Redis subscription ended with result: %d\n", (int)result);
  redisClient.stop();
  return false;
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\nTERAHERTZ CLOUD COMMAND SENDER");

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
