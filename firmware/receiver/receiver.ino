#include <Arduino.h>
#include <ArduinoJson.h>
#include <mbedtls/sha256.h>

#include "../common/optical_protocol.h"

#define RECEIVER_PIN 5
#define BAUD_RATE 250000
#define FRAME_TIMEOUT_MS 2000

// Set to 1 when a serial capture of the received image is needed. The output
// between IMAGE_BASE64_BEGIN/END can be decoded by a host-side script.
#define DEBUG_EMIT_IMAGE_BASE64 0

uint8_t opticalPayload[OPTICAL_MAX_PAYLOAD];
mbedtls_sha256_context imageHash;
bool imageActive = false;
uint16_t expectedChunks = 0;
uint16_t nextChunk = 0;
uint32_t expectedBytes = 0;
uint32_t receivedBytes = 0;
char expectedSha256[65] = {0};

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

void resetImageState() {
  mbedtls_sha256_free(&imageHash);
  mbedtls_sha256_init(&imageHash);
  mbedtls_sha256_starts_ret(&imageHash, 0);
  imageActive = false;
  expectedChunks = 0;
  nextChunk = 0;
  expectedBytes = 0;
  receivedBytes = 0;
  expectedSha256[0] = '\0';
}

void printBase64(const uint8_t* data, size_t length) {
#if DEBUG_EMIT_IMAGE_BASE64
  static const char alphabet[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  Serial.print("IMAGE_DATA ");
  for (size_t index = 0; index < length; index += 3) {
    const uint32_t value = ((uint32_t)data[index] << 16) |
                           ((index + 1 < length ? data[index + 1] : 0) << 8) |
                           (index + 2 < length ? data[index + 2] : 0);
    Serial.print(alphabet[(value >> 18) & 0x3F]);
    Serial.print(alphabet[(value >> 12) & 0x3F]);
    Serial.print(index + 1 < length ? alphabet[(value >> 6) & 0x3F] : '=');
    Serial.print(index + 2 < length ? alphabet[value & 0x3F] : '=');
  }
  Serial.println();
#endif
}

bool copySha256(const char* value) {
  if (value == nullptr || strlen(value) != 64) return false;
  for (uint8_t index = 0; index < 64; index++) {
    const char character = value[index];
    const bool hexadecimal = (character >= '0' && character <= '9') ||
                             (character >= 'a' && character <= 'f') ||
                             (character >= 'A' && character <= 'F');
    if (!hexadecimal) return false;
    expectedSha256[index] = character;
  }
  expectedSha256[64] = '\0';
  return true;
}

void printDigest(const uint8_t* digest) {
  for (uint8_t index = 0; index < 32; index++) {
    if (digest[index] < 16) Serial.print('0');
    Serial.printf("%02x", digest[index]);
  }
}

void handleImageStart(const uint8_t* payload, uint16_t length) {
  JsonDocument document;
  if (deserializeJson(document, payload, length)) {
    Serial.println("[ERROR] Invalid image start metadata");
    resetImageState();
    return;
  }

  resetImageState();
  expectedBytes = document["totalBytes"] | 0;
  expectedChunks = document["chunkCount"] | 0;
  imageActive = expectedBytes > 0 && expectedChunks > 0;

#if DEBUG_EMIT_IMAGE_BASE64
  Serial.println("IMAGE_BASE64_BEGIN");
#endif
  Serial.printf("[IMAGE] START id=%s total=%lu chunks=%u mime=%s\n",
                document["id"] | "unknown", (unsigned long)expectedBytes,
                expectedChunks, document["mimeType"] | "unknown");
}

void handleImageChunk(uint16_t sequence, const uint8_t* payload, uint16_t length) {
  if (!imageActive) {
    Serial.println("[ERROR] Image chunk received without an active image");
    return;
  }
  if (sequence != nextChunk) {
    Serial.printf("[ERROR] Image chunk sequence expected=%u actual=%u\n",
                  nextChunk, sequence);
    imageActive = false;
    return;
  }
  if (receivedBytes + length > expectedBytes) {
    Serial.println("[ERROR] Image contains more bytes than declared");
    imageActive = false;
    return;
  }

  mbedtls_sha256_update_ret(&imageHash, payload, length);
  receivedBytes += length;
  nextChunk++;
  printBase64(payload, length);
  Serial.printf("[IMAGE] CHUNK index=%u/%u bytes=%u total=%lu/%lu\n", sequence,
                expectedChunks, length, (unsigned long)receivedBytes,
                (unsigned long)expectedBytes);
}

void handleImageEnd(const uint8_t* payload, uint16_t length) {
  JsonDocument document;
  if (!imageActive || deserializeJson(document, payload, length)) {
    Serial.println("[ERROR] Invalid image end or no active image");
    resetImageState();
    return;
  }

  const char* sha256 = document["sha256"] | "";
  const bool metadataValid = copySha256(sha256);
  const bool sizeValid = receivedBytes == expectedBytes;
  const bool countValid = nextChunk == expectedChunks;
  uint8_t actualDigest[32];
  mbedtls_sha256_finish_ret(&imageHash, actualDigest);

  char actualHex[65];
  for (uint8_t index = 0; index < 32; index++) {
    snprintf(&actualHex[index * 2], 3, "%02x", actualDigest[index]);
  }
  actualHex[64] = '\0';

  Serial.printf("[IMAGE] END receivedBytes=%lu/%lu chunks=%u/%u\n",
                (unsigned long)receivedBytes, (unsigned long)expectedBytes,
                nextChunk, expectedChunks);
  Serial.printf("[IMAGE] SHA256 expected=%s actual=%s\n",
                metadataValid ? expectedSha256 : "invalid", actualHex);

  if (metadataValid && sizeValid && countValid && strcmp(expectedSha256, actualHex) == 0) {
    Serial.println("[SUCCESS] IMAGE VERIFIED");
  } else {
    Serial.printf("[ERROR] IMAGE VERIFICATION FAILED size=%s count=%s sha=%s\n",
                  sizeValid ? "ok" : "mismatch", countValid ? "ok" : "mismatch",
                  metadataValid ? "mismatch" : "invalid");
  }
#if DEBUG_EMIT_IMAGE_BASE64
  Serial.println("IMAGE_BASE64_END");
#endif
  resetImageState();
}

void handleFrame(OpticalFrameType type, uint16_t sequence,
                 const uint8_t* payload, uint16_t length) {
  if (type == FRAME_TEXT) {
    Serial.printf("[SUCCESS] TEXT %u bytes: ", length);
    Serial.write(payload, length);
    Serial.println();
  } else if (type == FRAME_IMAGE_START) {
    handleImageStart(payload, length);
  } else if (type == FRAME_IMAGE_CHUNK) {
    handleImageChunk(sequence, payload, length);
  } else if (type == FRAME_IMAGE_END) {
    handleImageEnd(payload, length);
  } else {
    Serial.println("[ERROR] Unknown optical frame type");
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
  if (!readExact(opticalPayload, length)) {
    Serial.println("[ERROR] Optical frame timed out");
    return;
  }

  const uint32_t actualCrc = opticalCrc32(opticalPayload, length);
  if (actualCrc != expectedCrc) {
    Serial.printf("[ERROR] CRC mismatch expected=%08lX actual=%08lX\n",
                  expectedCrc, actualCrc);
    return;
  }
  handleFrame(type, sequence, opticalPayload, length);
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\nTERAHERTZ BINARY UART RECEIVER");
  Serial1.begin(BAUD_RATE, SERIAL_8N1, RECEIVER_PIN, -1, true);
  mbedtls_sha256_init(&imageHash);
  resetImageState();
}

void loop() { readOpticalFrame(); }
