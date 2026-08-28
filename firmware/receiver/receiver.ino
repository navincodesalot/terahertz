#include <Arduino.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include "mbedtls/sha256.h"

#include "secrets.h"
#include "../common/optical_protocol.h"

#define RECEIVER_PIN 5
#define BAUD_RATE    250000
#define TELEMETRY_QUEUE_DEPTH 4
#define OPTICAL_LINE_OVERHEAD_BYTES 256

// ---------------------------------------------------------------------------
// Image reassembly state
// ---------------------------------------------------------------------------

String              imageId;
uint16_t            expectedImageChunks = 0;
uint16_t            nextImageChunk      = 0;
uint32_t            expectedImageBytes  = 0;
uint32_t            receivedImageBytes  = 0;
bool                receivingImage      = false;
unsigned long       imageStartMs        = 0;
mbedtls_sha256_context sha256Ctx;
QueueHandle_t       telemetryQueue;
String              rxBuffer;

static uint8_t chunkBuf[IMAGE_CHUNK_BYTES + 4];

// ---------------------------------------------------------------------------
// WiFi + telemetry POST
// ---------------------------------------------------------------------------

void connectWiFi() {
  if (WiFi.status() == WL_CONNECTED) return;
  Serial.printf("Connecting to Wi-Fi: %s\n", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  const unsigned long deadline = millis() + 15000;
  while (WiFi.status() != WL_CONNECTED && millis() < deadline) {
    delay(500);
    Serial.print(".");
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("\nWi-Fi connected: %s\n", WiFi.localIP().toString().c_str());
  } else {
    Serial.println("\nWi-Fi connection timed out; telemetry will be skipped");
  }
}

void postTelemetry(const String& body) {
  // Telemetry is sent once per completed receive. Reconnect here so a brief
  // Wi-Fi drop does not discard the final result permanently.
  if (WiFi.status() != WL_CONNECTED) connectWiFi();
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[TELEMETRY] Wi-Fi not connected; skipping POST");
    return;
  }

  WiFiClientSecure secureClient;
  secureClient.setInsecure(); // Internal demo — pin a CA cert before production
  secureClient.setTimeout(10000);

  HTTPClient https;
  if (!https.begin(secureClient, RECEIVE_API_URL)) {
    Serial.println("[TELEMETRY] HTTP begin failed");
    return;
  }

  https.addHeader("Content-Type", "application/json");
  const int code = https.POST(body);

  if (code > 0) {
    Serial.printf("[TELEMETRY] POST %d\n", code);
  } else {
    Serial.printf("[TELEMETRY] POST failed: %s\n", https.errorToString(code).c_str());
  }

  https.end();
}

void queueTelemetry(const String& body) {
  if (!telemetryQueue) {
    Serial.println("[TELEMETRY] Queue unavailable; report dropped");
    return;
  }

  String* report = new String(body);
  if (!report || xQueueSend(telemetryQueue, &report, 0) != pdTRUE) {
    delete report;
    Serial.println("[TELEMETRY] Queue full; report dropped");
  }
}

// HTTPS is deliberately handled outside the UART loop. Wi-Fi runs in the
// ESP32 background stack, and this worker performs the blocking POST while
// loop() remains available to receive optical bytes.
void telemetryTask(void* parameter) {
  while (true) {
    if (WiFi.status() != WL_CONNECTED) connectWiFi();

    String* report = nullptr;
    if (xQueueReceive(telemetryQueue, &report, pdMS_TO_TICKS(1000)) == pdTRUE) {
      if (report) {
        postTelemetry(*report);
        delete report;
      }
    }
  }
}

// ---------------------------------------------------------------------------
// Base64 decode
// ---------------------------------------------------------------------------

static int base64Value(char c) {
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a' + 26;
  if (c >= '0' && c <= '9') return c - '0' + 52;
  if (c == '+') return 62;
  if (c == '/') return 63;
  return -1;
}

static size_t decodeBase64(const String& input, uint8_t* output, size_t outputLimit) {
  size_t outLen = 0;
  int    accum  = 0;
  int    bits   = 0;
  for (size_t i = 0; i < input.length(); i++) {
    const int v = base64Value(input[i]);
    if (v < 0) continue;
    accum = (accum << 6) | v;
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      if (outLen >= outputLimit) return 0;
      output[outLen++] = (uint8_t)((accum >> bits) & 0xFF);
    }
  }
  return outLen;
}

// ---------------------------------------------------------------------------
// Text record handler  —  THZTXT|<id>|<crc32hex>|<payload>
// ---------------------------------------------------------------------------

static void handleTextLine(const String& line) {
  const int p1 = line.indexOf('|');
  const int p2 = line.indexOf('|', p1 + 1);
  const int p3 = line.indexOf('|', p2 + 1);
  if (p1 < 0 || p2 < 0 || p3 < 0) {
    Serial.println("[ERROR] Malformed text record");
    return;
  }

  const String id      = line.substring(p1 + 1, p2);
  const String crcHex  = line.substring(p2 + 1, p3);
  const String payload = line.substring(p3 + 1);

  const uint32_t expectedCrc =
    (uint32_t)strtoul(crcHex.c_str(), nullptr, 16);
  const uint32_t actualCrc =
    opticalCrc32((const uint8_t*)payload.c_str(), payload.length());

  const bool checksumPassed = (actualCrc == expectedCrc);
  if (!checksumPassed) {
    Serial.printf("[ERROR] TEXT CRC mismatch id=%s expected=%s actual=%08lx\n",
                  id.c_str(), crcHex.c_str(), (unsigned long)actualCrc);
  } else {
    Serial.println("--------------------------------");
    Serial.printf("[TEXT] id=%s bytes=%u crc=OK\n", id.c_str(), payload.length());
    Serial.println(payload);
  }

  // POST to Vercel regardless of CRC status so the dashboard reflects failures.
  JsonDocument doc;
  doc["id"]             = id;
  doc["status"]         = checksumPassed ? "success" : "failed";
  doc["type"]           = "text";
  doc["receivedBytes"]  = (uint32_t)payload.length();
  doc["checksumPassed"] = checksumPassed;
  // Preserve the decoded text even when CRC fails so the dashboard can show
  // what physically arrived, while checksumPassed still marks it untrusted.
  doc["receivedPayload"] = payload;

  String body;
  serializeJson(doc, body);
  queueTelemetry(body);
}

// ---------------------------------------------------------------------------
// Image record handler  —  THZIMG|<kind>|…
// ---------------------------------------------------------------------------

static void handleImageLine(const String& line) {
  if (!line.startsWith("THZIMG|")) return;

  const char kind    = line[7];
  const int  idStart = 9;
  const int  idEnd   = line.indexOf('|', idStart);
  if (idEnd < 0) {
    Serial.println("[ERROR] Invalid image record");
    return;
  }
  const String id = line.substring(idStart, idEnd);

  if (kind == 'S') {
    const int chunkCountEnd = line.indexOf('|', idEnd + 1);
    const int totalBytesEnd = line.indexOf('|', chunkCountEnd + 1);
    if (chunkCountEnd < 0 || totalBytesEnd < 0) {
      Serial.println("[ERROR] Invalid image start record");
      return;
    }

    imageId             = id;
    expectedImageChunks = (uint16_t)line.substring(idEnd + 1, chunkCountEnd).toInt();
    expectedImageBytes  = (uint32_t)line.substring(chunkCountEnd + 1, totalBytesEnd).toInt();
    nextImageChunk      = 0;
    receivedImageBytes  = 0;
    receivingImage      = expectedImageChunks > 0 && expectedImageBytes > 0;
    imageStartMs        = millis();

    mbedtls_sha256_init(&sha256Ctx);
    mbedtls_sha256_starts(&sha256Ctx, 0);

    Serial.printf("[IMAGE] START id=%s bytes=%lu chunks=%u\n",
                  imageId.c_str(),
                  (unsigned long)expectedImageBytes,
                  expectedImageChunks);
    return;
  }

  if (kind == 'C') {
    const int indexEnd = line.indexOf('|', idEnd + 1);
    if (indexEnd < 0) {
      Serial.println("[ERROR] Malformed image chunk record");
      return;
    }
    if (!receivingImage || id != imageId) {
      Serial.printf("[WARN] Dropping chunk — missed image_start (id=%s)\n",
                    id.c_str());
      return;
    }

    const uint16_t index = (uint16_t)line.substring(idEnd + 1, indexEnd).toInt();

    const int crcSep = line.lastIndexOf('|');
    if (crcSep <= indexEnd) {
      Serial.println("[ERROR] Missing checksum in image chunk");
      receivingImage = false;
      return;
    }
    const String encoded = line.substring(indexEnd + 1, crcSep);
    const String crcHex  = line.substring(crcSep + 1);

    const uint32_t expectedCrc =
      (uint32_t)strtoul(crcHex.c_str(), nullptr, 16);
    const uint32_t actualCrc =
      opticalCrc32((const uint8_t*)encoded.c_str(), encoded.length());
    if (actualCrc != expectedCrc) {
      Serial.printf("[ERROR] CHUNK CRC mismatch index=%u expected=%s actual=%08lx\n",
                    index, crcHex.c_str(), (unsigned long)actualCrc);
      receivingImage = false;
      return;
    }

    const size_t decoded = decodeBase64(encoded, chunkBuf, sizeof(chunkBuf));
    if (index != nextImageChunk || decoded == 0 ||
        receivedImageBytes + decoded > expectedImageBytes) {
      Serial.printf("[ERROR] Bad chunk index=%u expected=%u decoded=%u\n",
                    index, nextImageChunk, (unsigned)decoded);
      receivingImage = false;
      return;
    }

    mbedtls_sha256_update(&sha256Ctx, chunkBuf, decoded);

    receivedImageBytes += decoded;
    nextImageChunk     += 1;
    Serial.printf("[IMAGE] CHUNK index=%u/%u bytes=%u total=%lu/%lu crc=OK\n",
                  index, expectedImageChunks,
                  (unsigned)decoded,
                  (unsigned long)receivedImageBytes,
                  (unsigned long)expectedImageBytes);
    return;
  }

  if (kind == 'E') {
    const String expectedSha = line.substring(idEnd + 1);
    const bool   complete    = receivingImage          &&
                               id == imageId           &&
                               nextImageChunk == expectedImageChunks &&
                               receivedImageBytes == expectedImageBytes;

    bool sha256Passed = false;
    if (complete) {
      uint8_t hash[32];
      mbedtls_sha256_finish(&sha256Ctx, hash);
      char computedHex[65];
      for (int i = 0; i < 32; i++) {
        snprintf(computedHex + i * 2, 3, "%02x", hash[i]);
      }
      computedHex[64] = '\0';
      sha256Passed = (strcmp(computedHex, expectedSha.c_str()) == 0);

      if (sha256Passed) {
        Serial.printf("[SUCCESS] IMAGE id=%s bytes=%lu sha256=PASS\n",
                      id.c_str(), (unsigned long)receivedImageBytes);
      } else {
        Serial.printf("[ERROR] IMAGE sha256 FAIL id=%s\n  expected=%s\n  actual=%s\n",
                      id.c_str(), expectedSha.c_str(), computedHex);
      }
    } else {
      Serial.printf("[ERROR] IMAGE incomplete chunks=%u/%u bytes=%lu/%lu\n",
                    nextImageChunk, expectedImageChunks,
                    (unsigned long)receivedImageBytes,
                    (unsigned long)expectedImageBytes);
      mbedtls_sha256_free(&sha256Ctx);
    }

    const unsigned long transmissionMs = millis() - imageStartMs;

    JsonDocument doc;
    doc["id"]             = id;
    doc["status"]         = (complete && sha256Passed) ? "success" : "failed";
    doc["type"]           = "image";
    doc["receivedBytes"]  = (uint32_t)receivedImageBytes;
    doc["chunksReceived"] = (uint16_t)nextImageChunk;
    doc["chunksExpected"] = (uint16_t)expectedImageChunks;
    doc["sha256Passed"]   = sha256Passed;
    doc["transmissionMs"] = (uint32_t)transmissionMs;

    String body;
    serializeJson(doc, body);

    receivingImage = false;
    queueTelemetry(body);
    return;
  }

  Serial.println("[ERROR] Unknown image record kind");
}

// ---------------------------------------------------------------------------
// Line dispatcher
// ---------------------------------------------------------------------------

static void handleLine(String line) {
  if (line.endsWith("\r")) line.remove(line.length() - 1);
  if (line.length() == 0) return;

  const bool isText =
    line.startsWith("THZTXT|") ||
    (line.length() >= 7 && line[0] == 'T' && line[1] == 'H' &&
     line.substring(3, 7) == "TXT|");
  if (isText) {
    if (!line.startsWith("THZTXT|")) {
      line = "THZTXT|" + line.substring(7);
      Serial.println("[WARN] Startup glitch corrected on text record");
    }
    handleTextLine(line);
    return;
  }

  const bool isImage =
    line.startsWith("THZIMG|") ||
    (line.length() >= 7 && line[0] == 'T' && line[1] == 'H' &&
     line.substring(3, 7) == "IMG|");
  if (isImage) {
    if (!line.startsWith("THZIMG|")) {
      line = "THZIMG|" + line.substring(7);
      Serial.println("[WARN] Startup glitch corrected on image record");
    }
    handleImageLine(line);
    return;
  }
}

// ---------------------------------------------------------------------------
// Arduino entry points
// ---------------------------------------------------------------------------

void setup() {
  Serial.begin(115200);
  delay(1000);

  Serial.println("\n================================");
  Serial.println("  TERAHERTZ UART RECEIVER");
  Serial.println("================================\n");

  Serial1.begin(BAUD_RATE, SERIAL_8N1, RECEIVER_PIN, -1, true);

  telemetryQueue = xQueueCreate(TELEMETRY_QUEUE_DEPTH, sizeof(String*));
  if (!telemetryQueue) {
    Serial.println("[TELEMETRY] Queue allocation failed");
  } else {
    // No core pinning: the scheduler keeps this worker separate from the
    // Arduino UART loop while Wi-Fi/HTTPS are serviced in the background.
    xTaskCreate(telemetryTask, "Telemetry", 8192, nullptr, 1, nullptr);
  }

  Serial.println("Waiting for optical data...\n");
}

void loop() {
  static size_t bytesSinceYield = 0;

  while (Serial1.available()) {
    const char c = (char)Serial1.read();
    if (c == '\n') {
      if (rxBuffer.length() > 0) handleLine(rxBuffer);
      rxBuffer = "";
      bytesSinceYield = 0;
      continue;
    }

    rxBuffer += c;
    if (rxBuffer.length() > MAX_TRANSFER_BYTES + OPTICAL_LINE_OVERHEAD_BYTES) {
      Serial.println("[ERROR] Optical record exceeds the 250 KB transfer limit");
      rxBuffer = "";
      bytesSinceYield = 0;
      continue;
    }

    // Keep the Wi-Fi/telemetry task and ESP32 background services schedulable
    // while a large valid text record is arriving continuously.
    bytesSinceYield += 1;
    if (bytesSinceYield >= 256) {
      yield();
      bytesSinceYield = 0;
    }
  }
}
