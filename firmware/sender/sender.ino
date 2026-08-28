#include <Arduino.h>
#include <ArduinoJson.h>
#include <Redis.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>

#include "secrets.h"
#include "../common/optical_protocol.h"

#define LASER_PIN         4
#define BAUD_RATE         250000
#define REDIS_CHANNEL     "laser_commands"
#define REDIS_RECONNECT_MS 5000

// Maximum size of a single command JSON (image_chunk with 1 024 B of base64
// encodes to ~1 450 chars; 2 048 gives comfortable headroom).
#define COMMAND_JSON_MAX  2048
// Queue depth: at 4 chunks/batch × 30 ms spacing and ~6 ms/chunk UART TX,
// 32 slots absorbs several batches of bursting without dropping messages.
#define COMMAND_QUEUE_DEPTH 32

struct CommandMessage {
  char json[COMMAND_JSON_MAX];
};

WiFiClientSecure redisClient;
QueueHandle_t    commandQueue;
uint16_t         expectedImageChunks = 0;

// ---------------------------------------------------------------------------
// Optical TX helpers
// ---------------------------------------------------------------------------

void sendOpticalText(const char* id, const String& payload) {
  if (payload.length() == 0) {
    Serial.println("[REJECTED] Text payload is empty");
    return;
  }

  char crcHex[9];
  opticalCrc32Hex((const uint8_t*)payload.c_str(), payload.length(), crcHex);

  // Format: THZTXT|<id>|<crc32hex>|<payload>
  // The payload is 3rd field; parsers find it by splitting on the third '|',
  // so '|' characters inside the payload are safe.
  String line = "THZTXT|";
  line += id;
  line += '|';
  line += crcHex;
  line += '|';
  line += payload;

  Serial1.println(line);
  Serial.printf("UART text sent: %u bytes crc=%s\n", payload.length(), crcHex);
}

void sendImageCommand(JsonDocument& document) {
  const char* type = document["type"] | "";
  const char* id   = document["id"]   | "unknown";

  if (strcmp(type, "image_start") == 0) {
    expectedImageChunks = document["chunkCount"] | 0;

    // Prime the optical link: a blank line absorbs startup transients so the
    // first real byte of the image record is not corrupted.
    Serial1.println();
    delay(20);

    String line = "THZIMG|S|";
    line += id;
    line += '|';
    line += (document["chunkCount"] | 0);
    line += '|';
    line += (document["totalBytes"]  | 0);
    line += '|';
    line += (document["mimeType"]    | "application/octet-stream");
    Serial1.println(line);
    Serial.printf("UART image start: chunks=%u bytes=%u\n",
                  expectedImageChunks, (unsigned)(document["totalBytes"] | 0));
    return;
  }

  if (strcmp(type, "image_chunk") == 0) {
    const uint16_t sequence = document["index"] | 0;
    const char*    encoded  = document["data"]  | "";

    char crcHex[9];
    opticalCrc32Hex((const uint8_t*)encoded, strlen(encoded), crcHex);

    // Format: THZIMG|C|<id>|<index>|<base64data>|<crc32hex>
    // base64 chars (A-Z a-z 0-9 + / =) never include '|', so lastIndexOf('|')
    // on the receiver always finds the checksum separator cleanly.
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

  // image_end — no checksum needed; the SHA-256 of the full image is the
  // integrity proof.
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

  const char* id   = document["id"]   | "unknown";
  const char* type = document["type"] | "";
  Serial.printf("Redis command: id=%s type=%s\n", id, type);

  if (strcmp(type, "text") == 0) {
    sendOpticalText(id, document["payload"].as<String>());
  } else if (strcmp(type, "image_start") == 0 ||
             strcmp(type, "image_chunk") == 0  ||
             strcmp(type, "image_end")   == 0) {
    sendImageCommand(document);
  } else {
    Serial.println("[REJECTED] Unknown command type");
  }
}

// ---------------------------------------------------------------------------
// Redis subscription
// ---------------------------------------------------------------------------

// Fix #1 — the callback enqueues only; it never transmits optically.
// All Serial1 writes happen in loop() on core 1, well outside the Redis
// socket-read loop running on core 0.
void messageCallback(Redis* redis, String channel, String message) {
  if (channel != REDIS_CHANNEL) return;
  CommandMessage msg;
  message.toCharArray(msg.json, sizeof(msg.json));
  if (xQueueSend(commandQueue, &msg, 0) != pdTRUE) {
    Serial.println("[WARN] Command queue full; message dropped");
  }
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
    vTaskDelay(pdMS_TO_TICKS(500));
    Serial.print(".");
  }
  Serial.printf("\nWi-Fi connected: %s\n", WiFi.localIP().toString().c_str());
}

void connectAndSubscribe() {
  if (strcmp(UPSTASH_REDIS_HOST, "your-database.upstash.io") == 0 ||
      strcmp(UPSTASH_REDIS_TOKEN, "your-upstash-token") == 0) {
    Serial.println("Redis credentials are still placeholders in sender/secrets.h");
    return;
  }

  redisClient.setInsecure();
  redisClient.setTimeout(10000);
  redisClient.setHandshakeTimeout(10);
  Serial.printf("Connecting to Redis: %s:%d\n", UPSTASH_REDIS_HOST, UPSTASH_REDIS_PORT);

  if (!redisClient.connect(UPSTASH_REDIS_HOST, UPSTASH_REDIS_PORT)) {
    char errorBuffer[128] = {0};
    const int errorCode = redisClient.lastError(errorBuffer, sizeof(errorBuffer));
    Serial.printf("Redis TLS failed code=%d detail=%s\n", errorCode, errorBuffer);
    redisClient.stop();
    return;
  }
  Serial.println("Connected to Redis over TLS");

  Redis redis(redisClient);
  if (redis.authenticate(UPSTASH_REDIS_TOKEN) != RedisSuccess) {
    Serial.println("Redis authentication failed");
    redisClient.stop();
    return;
  }
  Serial.println("Authenticated successfully");

  if (!redis.subscribe(REDIS_CHANNEL)) {
    Serial.println("Redis channel subscription failed");
    redisClient.stop();
    return;
  }
  Serial.println("Subscribed to laser_commands; waiting for commands");

  // Blocks until the connection drops; callbacks enqueue into commandQueue.
  const RedisSubscribeResult result = redis.startSubscribing(messageCallback, errorCallback);
  Serial.printf("Redis subscription ended: %d\n", (int)result);
  redisClient.stop();
}

// ---------------------------------------------------------------------------
// FreeRTOS tasks
// ---------------------------------------------------------------------------

// Core 0 — manages Wi-Fi and the persistent Redis subscription.
// Runs independently of optical TX so a slow or idle optical link never
// stalls the subscription socket read loop.
void redisTask(void* param) {
  while (true) {
    if (WiFi.status() != WL_CONNECTED) {
      redisClient.stop();
      connectWiFi();
    } else {
      connectAndSubscribe();
    }
    vTaskDelay(pdMS_TO_TICKS(REDIS_RECONNECT_MS));
  }
}

// ---------------------------------------------------------------------------
// Arduino entry points
// ---------------------------------------------------------------------------

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\nTERAHERTZ CLOUD COMMAND SENDER");

  Serial1.begin(BAUD_RATE, SERIAL_8N1, -1, LASER_PIN, true);

  commandQueue = xQueueCreate(COMMAND_QUEUE_DEPTH, sizeof(CommandMessage));

  // Pin the Redis task to core 0; loop() (optical TX) runs on core 1 as
  // the default Arduino task.
  xTaskCreatePinnedToCore(redisTask, "Redis", 12288, nullptr, 2, nullptr, 0);
}

// Core 1 — dequeues and transmits one command at a time.
// Blocking Serial1 writes here cannot starve the Redis subscription.
void loop() {
  CommandMessage msg;
  if (xQueueReceive(commandQueue, &msg, pdMS_TO_TICKS(100)) == pdTRUE) {
    handleCommand(String(msg.json));
  }
}
