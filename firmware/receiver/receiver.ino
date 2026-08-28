#include <Arduino.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>

#include "secrets.h"
#include "../common/optical_protocol.h"

#define RECEIVER_PIN 5
#define BAUD_RATE 250000

// ---------------------------------------------------------------------------
// Image reassembly state
// ---------------------------------------------------------------------------

static String   imageId;
static uint16_t expectedImageChunks = 0;
static uint16_t nextImageChunk      = 0;
static uint32_t expectedImageBytes  = 0;
static uint32_t receivedImageBytes  = 0;
static bool     receivingImage      = false;
static unsigned long imageStartMs   = 0;
static unsigned long textStartMs    = 0;

// ---------------------------------------------------------------------------
// Telemetry POST — blocking, called once after a complete receive
// ---------------------------------------------------------------------------

static void postTelemetry(const String& body) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[TELEMETRY] No Wi-Fi; skipping POST");
    return;
  }

  WiFiClientSecure client;
  client.setInsecure();
  client.setTimeout(10000);

  HTTPClient https;
  https.begin(client, RECEIVE_API_URL);
  https.addHeader("Content-Type", "application/json");
  const int code = https.POST(body);
  if (code > 0)
    Serial.printf("[TELEMETRY] POST %d\n", code);
  else
    Serial.printf("[TELEMETRY] POST error: %s\n", https.errorToString(code).c_str());
  https.end();
}

// ---------------------------------------------------------------------------
// Base64 decode (needed for SHA-256 over raw image bytes)
// ---------------------------------------------------------------------------

static int base64Value(char c) {
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a' + 26;
  if (c >= '0' && c <= '9') return c - '0' + 52;
  if (c == '+') return 62;
  if (c == '/') return 63;
  return -1;
}

static uint8_t chunkBuf[IMAGE_CHUNK_BYTES + 4];

static size_t decodeBase64(const String& input, uint8_t* output, size_t limit) {
  size_t outLen = 0;
  int accum = 0, bits = 0;
  for (size_t i = 0; i < input.length(); i++) {
    const int v = base64Value(input[i]);
    if (v < 0) continue;
    accum = (accum << 6) | v;
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      if (outLen >= limit) return 0;
      output[outLen++] = (uint8_t)((accum >> bits) & 0xFF);
    }
  }
  return outLen;
}

// ---------------------------------------------------------------------------
// loop() — exactly the working original, extended to parse the protocol
// ---------------------------------------------------------------------------

void setup() {
  Serial.begin(115200);
  delay(1000);

  Serial.println("\n================================");
  Serial.println("  TERAHERTZ UART RECEIVER");
  Serial.println("================================\n");

  // A chunk line is ~1400 bytes. The default 256-byte RX buffer overflows
  // long before the line ends, silently dropping bytes mid-line.
  // Must be called BEFORE begin().
  Serial1.setRxBufferSize(2048);
  Serial1.begin(BAUD_RATE, SERIAL_8N1, RECEIVER_PIN, -1, true);

  Serial.printf("Connecting to Wi-Fi: %s\n", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.printf("\nWi-Fi connected: %s\n", WiFi.localIP().toString().c_str());

  Serial.println("Waiting for optical data...\n");
  textStartMs = millis(); // initialise so first text has a reference
}

void loop() {
  if (!Serial1.available()) return;

  // Static so the String keeps its capacity between calls instead of
  // reallocating from zero on every character of a long line.
  static String line;
  line = Serial1.readStringUntil('\n');
  line.trim();
  if (line.length() == 0) return;

  Serial.println("--------------------------------");
  Serial.print("RECEIVED: ");
  Serial.println(line);

  // ---- Text: THZTXT|<id>|<crc32hex>|<payload> ----
  const unsigned long lineReceivedMs = millis();
  if (line.startsWith("THZTXT|")) {
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

    const uint32_t expected = (uint32_t)strtoul(crcHex.c_str(), nullptr, 16);
    const uint32_t actual   = opticalCrc32((const uint8_t*)payload.c_str(), payload.length());
    const bool ok = (expected == actual);

    if (ok)
      Serial.printf("[TEXT] crc=OK id=%s\n", id.c_str());
    else
      Serial.printf("[ERROR] TEXT CRC fail id=%s expected=%s actual=%08lx\n",
                    id.c_str(), crcHex.c_str(), (unsigned long)actual);

    // Estimate transmission time: characters on the wire at 250000 baud 8N1
    // = 10 bits/byte, so bytes_per_sec = 25000. Use that for a cross-check;
    // actual measured time is available if you record when the first byte arrives.
    const uint32_t wireBytes = (uint32_t)(line.length() + 2); // +2 for \r\n
    const uint32_t txMs      = (wireBytes * 10u * 1000u) / BAUD_RATE;
    const uint32_t bps       = txMs > 0 ? (payload.length() * 1000u / txMs) : 0;

    JsonDocument doc;
    doc["id"]              = id;
    doc["status"]          = ok ? "success" : "failed";
    doc["type"]            = "text";
    doc["receivedBytes"]   = (uint32_t)payload.length();
    doc["checksumPassed"]  = ok;
    doc["receivedPayload"] = payload;
    doc["transmissionMs"]  = txMs;
    doc["bitsPerSecond"]   = (uint32_t)BAUD_RATE;
    String body;
    serializeJson(doc, body);
    postTelemetry(body);
    return;
  }

  // ---- Image start: THZIMG|S|<id>|<chunkCount>|<totalBytes>|<mimeType> ----
  // Also tolerate a startup glitch on byte 2 (e.g. TH^IMG|S|) same as sender.
  const bool isImgStart =
    line.startsWith("THZIMG|S|") ||
    (line.length() > 9 && line[0] == 'T' && line[1] == 'H' &&
     line.substring(3, 9) == "IMG|S|");
  if (isImgStart) {
    if (!line.startsWith("THZIMG|S|")) line = "THZIMG|S|" + line.substring(9);
    const int idEnd = line.indexOf('|', 9);
    const int p1    = line.indexOf('|', idEnd + 1);
    const int p2    = line.indexOf('|', p1 + 1);
    if (idEnd < 0 || p1 < 0 || p2 < 0) {
      Serial.println("[ERROR] Invalid image start");
      return;
    }
    imageId             = line.substring(9, idEnd);
    expectedImageChunks = (uint16_t)line.substring(idEnd + 1, p1).toInt();
    expectedImageBytes  = (uint32_t)line.substring(p1 + 1, p2).toInt();
    nextImageChunk      = 0;
    receivedImageBytes  = 0;
    receivingImage      = true;
    imageStartMs        = millis();
    Serial.printf("[IMAGE] START id=%s chunks=%u bytes=%lu\n",
                  imageId.c_str(), expectedImageChunks, (unsigned long)expectedImageBytes);
    return;
  }

  // ---- Image chunk: THZIMG|C|<id>|<index>|<base64>|<crc32hex> ----
  if (line.startsWith("THZIMG|C|") ||
      (line.length() > 9 && line[0] == 'T' && line[1] == 'H' &&
       line.substring(3, 9) == "IMG|C|")) {
    if (!line.startsWith("THZIMG|C|")) line = "THZIMG|C|" + line.substring(9);
    if (!receivingImage) { Serial.println("[WARN] Chunk before start; dropped"); return; }

    const int idEnd    = line.indexOf('|', 9);
    const int indexEnd = line.indexOf('|', idEnd + 1);
    const int crcSep   = line.lastIndexOf('|');
    if (idEnd < 0 || indexEnd < 0 || crcSep <= indexEnd) {
      Serial.println("[ERROR] Malformed image chunk");
      receivingImage = false;
      return;
    }

    const uint16_t index   = (uint16_t)line.substring(idEnd + 1, indexEnd).toInt();
    const String   encoded = line.substring(indexEnd + 1, crcSep);
    const String   crcHex  = line.substring(crcSep + 1);

    const uint32_t expected = (uint32_t)strtoul(crcHex.c_str(), nullptr, 16);
    const uint32_t actual   = opticalCrc32((const uint8_t*)encoded.c_str(), encoded.length());
    if (expected != actual) {
      Serial.printf("[ERROR] CHUNK CRC fail index=%u\n", index);
      receivingImage = false;
      return;
    }

    const size_t decoded = decodeBase64(encoded, chunkBuf, sizeof(chunkBuf));
    if (index != nextImageChunk || decoded == 0 ||
        receivedImageBytes + decoded > expectedImageBytes) {
      Serial.printf("[ERROR] Bad chunk index=%u expected=%u\n", index, nextImageChunk);
      receivingImage = false;
      return;
    }

    receivedImageBytes += decoded;
    nextImageChunk     += 1;
    Serial.printf("[IMAGE] CHUNK %u/%u bytes=%u total=%lu/%lu crc=OK\n",
                  index, expectedImageChunks, (unsigned)decoded,
                  (unsigned long)receivedImageBytes, (unsigned long)expectedImageBytes);
    return;
  }

  // ---- Image end: THZIMG|E|<id>|<sha256hex> ----
  if (line.startsWith("THZIMG|E|") ||
      (line.length() > 9 && line[0] == 'T' && line[1] == 'H' &&
       line.substring(3, 9) == "IMG|E|")) {
    if (!line.startsWith("THZIMG|E|")) line = "THZIMG|E|" + line.substring(9);
    const int idEnd = line.indexOf('|', 9);
    if (idEnd < 0) { Serial.println("[ERROR] Invalid image end"); return; }
    const String id          = line.substring(9, idEnd);
    const bool   complete    = receivingImage &&
                               nextImageChunk == expectedImageChunks &&
                               receivedImageBytes == expectedImageBytes;

    if (complete)
      Serial.printf("[IMAGE] END id=%s chunks=%u bytes=%lu\n",
                    id.c_str(), nextImageChunk, (unsigned long)receivedImageBytes);
    else
      Serial.printf("[ERROR] IMAGE incomplete chunks=%u/%u bytes=%lu/%lu\n",
                    nextImageChunk, expectedImageChunks,
                    (unsigned long)receivedImageBytes, (unsigned long)expectedImageBytes);

    JsonDocument doc;
    doc["id"]             = id;
    doc["status"]         = complete ? "success" : "failed";
    doc["type"]           = "image";
    doc["receivedBytes"]  = (uint32_t)receivedImageBytes;
    doc["chunksReceived"] = (uint16_t)nextImageChunk;
    doc["chunksExpected"] = (uint16_t)expectedImageChunks;
    doc["sha256Passed"]   = false; // SHA-256 check removed for simplicity; add mbedtls if needed
    const uint32_t imgTxMs = (uint32_t)(millis() - imageStartMs);
    doc["transmissionMs"] = imgTxMs;
    doc["bitsPerSecond"]  = (uint32_t)BAUD_RATE;
    String body;
    serializeJson(doc, body);
    receivingImage = false;
    postTelemetry(body);
    return;
  }
}
