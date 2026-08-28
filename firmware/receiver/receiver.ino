#include <Arduino.h>
#include <ArduinoJson.h>

#include "../common/optical_protocol.h"

#define RECEIVER_PIN 5
#define BAUD_RATE 250000
#define FRAME_TIMEOUT_MS 2000

bool readExact(uint8_t* buffer, size_t length) {
  const unsigned long deadline = millis() + FRAME_TIMEOUT_MS;
  size_t received = 0;
  while (received < length && millis() < deadline) {
    if (Serial1.available()) buffer[received++] = (uint8_t)Serial1.read();
    else delay(1);
  }
  return received == length;
}

uint16_t readU16(const uint8_t* bytes) {
  return ((uint16_t)bytes[0] << 8) | bytes[1];
}

uint32_t readU32(const uint8_t* bytes) {
  return ((uint32_t)bytes[0] << 24) | ((uint32_t)bytes[1] << 16) |
         ((uint32_t)bytes[2] << 8) | bytes[3];
}

void reportImageFrame(OpticalFrameType type, uint16_t sequence, const uint8_t* payload, uint16_t length) {
  if (type == FRAME_IMAGE_START) {
    DynamicJsonDocument document(1024);
    if (deserializeJson(document, payload, length)) {
      Serial.println("[ERROR] Invalid image start metadata");
      return;
    }
    Serial.printf("[IMAGE] START id=%s total=%u chunks=%u\n",
                  document["id"] | "unknown", document["totalBytes"] | 0,
                  document["chunkCount"] | 0);
  } else if (type == FRAME_IMAGE_CHUNK) {
    Serial.printf("[IMAGE] CHUNK index=%u bytes=%u\n", sequence, length);
  } else {
    Serial.printf("[IMAGE] END bytes=%u\n", length);
  }
}

void readOpticalFrame() {
  if (!Serial1.available()) return;
  if ((uint8_t)Serial1.read() != OPTICAL_MAGIC_0) return;

  uint8_t header[OPTICAL_HEADER_BYTES - 1];
  if (!readExact(header, sizeof(header)) || header[0] != OPTICAL_MAGIC_1 ||
      header[1] != OPTICAL_VERSION) {
    Serial.println("[ERROR] Invalid optical frame header");
    return;
  }

  const OpticalFrameType type = (OpticalFrameType)header[2];
  const uint16_t sequence = readU16(&header[3]);
  const uint16_t length = readU16(&header[5]);
  const uint32_t expectedCrc = readU32(&header[7]);
  if (length > OPTICAL_MAX_PAYLOAD) {
    Serial.println("[ERROR] Optical payload exceeds limit");
    return;
  }

  static uint8_t payload[OPTICAL_MAX_PAYLOAD];
  if (!readExact(payload, length)) {
    Serial.println("[ERROR] Optical frame timed out");
    return;
  }
  const uint32_t actualCrc = opticalCrc32(payload, length);
  if (actualCrc != expectedCrc) {
    Serial.printf("[ERROR] CRC mismatch expected=%08lX actual=%08lX\n", expectedCrc, actualCrc);
    return;
  }

  if (type == FRAME_TEXT) {
    Serial.printf("[SUCCESS] TEXT %u bytes: ", length);
    Serial.write(payload, length);
    Serial.println();
  } else if (type == FRAME_IMAGE_START || type == FRAME_IMAGE_CHUNK || type == FRAME_IMAGE_END) {
    reportImageFrame(type, sequence, payload, length);
  } else {
    Serial.println("[ERROR] Unknown optical frame type");
  }
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\nTERAHERTZ BINARY UART RECEIVER");
  Serial1.begin(BAUD_RATE, SERIAL_8N1, RECEIVER_PIN, -1, true);
}

void loop() { readOpticalFrame(); }
