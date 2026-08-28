#include <Arduino.h>
#include <ArduinoJson.h>
#include <mbedtls/sha256.h>

#include "../common/optical_protocol.h"

#define RECEIVER_PIN 5
#define BAUD_RATE 230400
#define RX_BUFFER_BYTES 8192
// A frame is contiguous on the wire. If the rest of a payload has not arrived
// within this window the transmitter stopped mid-frame or bytes were lost.
#define PAYLOAD_TIMEOUT_MS 150
// Bound the work done per loop() so the task watchdog is always fed during a
// sustained image stream.
#define MAX_BYTES_PER_PUMP 2048
#define IMAGE_PROGRESS_LOG_INTERVAL 32

// Set to 1 when a serial capture of the received image is needed. The output
// between IMAGE_BASE64_BEGIN/END can be decoded by a host-side script.
#define DEBUG_EMIT_IMAGE_BASE64 0

// ---------------------------------------------------------------------------
// Frame parser state
// ---------------------------------------------------------------------------

// Sliding window over the byte stream. Every incoming byte shifts the window
// by one and the header is re-tested, so a false magic match or a corrupted
// header costs exactly zero bytes of the following frame.
uint8_t headerWindow[OPTICAL_HEADER_BYTES];
uint8_t headerFill = 0;

uint8_t opticalPayload[OPTICAL_MAX_PAYLOAD];
uint16_t payloadFill = 0;
bool payloadActive = false;
unsigned long payloadDeadline = 0;

OpticalFrameType frameType = FRAME_TEXT;
uint16_t frameSequence = 0;
uint16_t frameLength = 0;
uint32_t frameCrc = 0;

uint32_t crcErrors = 0;
uint32_t framingErrors = 0;

// ---------------------------------------------------------------------------
// Image session state
// ---------------------------------------------------------------------------

mbedtls_sha256_context imageHash;
bool imageActive = false;
bool imageFailed = false;
uint16_t expectedChunks = 0;
uint16_t nextChunk = 0;
uint32_t expectedBytes = 0;
uint32_t receivedBytes = 0;
unsigned long imageStartedAt = 0;
char expectedSha256[65] = {0};

void resetImageState() {
  mbedtls_sha256_free(&imageHash);
  mbedtls_sha256_init(&imageHash);
  mbedtls_sha256_starts(&imageHash, 0);
  imageActive = false;
  imageFailed = false;
  expectedChunks = 0;
  nextChunk = 0;
  expectedBytes = 0;
  receivedBytes = 0;
  imageStartedAt = 0;
  expectedSha256[0] = '\0';
}

// A corrupted image cannot be repaired without retransmission, so fail the
// session once and stay quiet until image_end rather than logging every
// remaining chunk.
void failImage(const char* reason) {
  if (imageFailed) return;
  imageFailed = true;
  Serial.printf("[ERROR] Image aborted at chunk %u/%u: %s\n", nextChunk,
                expectedChunks, reason);
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
  imageStartedAt = millis();
  crcErrors = 0;
  framingErrors = 0;

#if DEBUG_EMIT_IMAGE_BASE64
  Serial.println("IMAGE_BASE64_BEGIN");
#endif
  Serial.printf("[IMAGE] START id=%s total=%lu chunks=%u mime=%s\n",
                document["id"] | "unknown", (unsigned long)expectedBytes,
                expectedChunks, document["mimeType"] | "unknown");
}

void handleImageChunk(uint16_t sequence, const uint8_t* payload, uint16_t length) {
  if (!imageActive) {
    // Either a stray chunk or a session that already failed. Stay silent so
    // USB logging never becomes the reason the RX ring overflows.
    return;
  }
  if (imageFailed) return;
  if (sequence != nextChunk) {
    failImage("sequence gap");
    return;
  }
  if (receivedBytes + length > expectedBytes) {
    failImage("byte count overrun");
    return;
  }

  mbedtls_sha256_update(&imageHash, payload, length);
  receivedBytes += length;
  nextChunk++;
  printBase64(payload, length);

  if (sequence % IMAGE_PROGRESS_LOG_INTERVAL == 0 || sequence + 1 == expectedChunks) {
    Serial.printf("[IMAGE] CHUNK %u/%u bytes=%lu/%lu\n", sequence, expectedChunks,
                  (unsigned long)receivedBytes, (unsigned long)expectedBytes);
  }
}

void handleImageEnd(const uint8_t* payload, uint16_t length) {
  JsonDocument document;
  if (!imageActive || deserializeJson(document, payload, length)) {
    Serial.println("[ERROR] Image end without an active image");
    resetImageState();
    return;
  }

  const unsigned long elapsed = millis() - imageStartedAt;
  const bool metadataValid = copySha256(document["sha256"] | "");
  const bool sizeValid = receivedBytes == expectedBytes;
  const bool countValid = nextChunk == expectedChunks;

  uint8_t actualDigest[32];
  mbedtls_sha256_finish(&imageHash, actualDigest);
  char actualHex[65];
  for (uint8_t index = 0; index < 32; index++) {
    snprintf(&actualHex[index * 2], 3, "%02x", actualDigest[index]);
  }
  actualHex[64] = '\0';

  const bool hashValid = metadataValid && strcmp(expectedSha256, actualHex) == 0;
  const unsigned long throughput =
      elapsed > 0 ? (unsigned long)((receivedBytes * 8000ULL) / elapsed) : 0;

  Serial.printf("[IMAGE] END bytes=%lu/%lu chunks=%u/%u time=%lums rate=%lubps\n",
                (unsigned long)receivedBytes, (unsigned long)expectedBytes,
                nextChunk, expectedChunks, elapsed, throughput);
  Serial.printf("[LINK] crcErrors=%lu framingErrors=%lu\n",
                (unsigned long)crcErrors, (unsigned long)framingErrors);

  if (!imageFailed && sizeValid && countValid && hashValid) {
    Serial.println("[SUCCESS] IMAGE VERIFIED");
  } else {
    Serial.printf("[ERROR] IMAGE FAILED size=%s count=%s sha=%s\n",
                  sizeValid ? "ok" : "mismatch", countValid ? "ok" : "mismatch",
                  hashValid ? "ok" : "mismatch");
    Serial.printf("[IMAGE] SHA256 expected=%s actual=%s\n",
                  metadataValid ? expectedSha256 : "invalid", actualHex);
  }

#if DEBUG_EMIT_IMAGE_BASE64
  Serial.println("IMAGE_BASE64_END");
#endif
  resetImageState();
}

void handleFrame() {
  switch (frameType) {
    case FRAME_TEXT:
      Serial.printf("[SUCCESS] TEXT %u bytes: ", frameLength);
      Serial.write(opticalPayload, frameLength);
      Serial.println();
      break;
    case FRAME_IMAGE_START:
      handleImageStart(opticalPayload, frameLength);
      break;
    case FRAME_IMAGE_CHUNK:
      handleImageChunk(frameSequence, opticalPayload, frameLength);
      break;
    case FRAME_IMAGE_END:
      handleImageEnd(opticalPayload, frameLength);
      break;
  }
}

bool headerValid(const uint8_t* header) {
  if (header[0] != OPTICAL_MAGIC_0) return false;
  if (header[1] != OPTICAL_MAGIC_1) return false;
  if (header[2] != OPTICAL_VERSION) return false;
  if (!opticalFrameTypeValid(header[3])) return false;
  if ((((uint16_t)header[6] << 8) | header[7]) > OPTICAL_MAX_PAYLOAD) return false;
  return header[OPTICAL_HEADER_BYTES - 1] == opticalHeaderCheck(header);
}

void beginPayload() {
  frameType = (OpticalFrameType)headerWindow[3];
  frameSequence = ((uint16_t)headerWindow[4] << 8) | headerWindow[5];
  frameLength = ((uint16_t)headerWindow[6] << 8) | headerWindow[7];
  frameCrc = ((uint32_t)headerWindow[8] << 24) | ((uint32_t)headerWindow[9] << 16) |
             ((uint32_t)headerWindow[10] << 8) | headerWindow[11];
  headerFill = 0;
  payloadFill = 0;

  if (frameLength == 0) {
    handleFrame();
    return;
  }
  payloadActive = true;
  payloadDeadline = millis() + PAYLOAD_TIMEOUT_MS;
}

void abortPayload(const char* reason) {
  payloadActive = false;
  payloadFill = 0;
  headerFill = 0;
  framingErrors++;
  if (frameType == FRAME_IMAGE_CHUNK || frameType == FRAME_IMAGE_END) {
    failImage(reason);
  } else {
    Serial.printf("[ERROR] Frame discarded: %s\n", reason);
  }
}

void pumpOptical() {
  if (payloadActive && (long)(millis() - payloadDeadline) >= 0) {
    abortPayload("payload timeout");
  }

  uint16_t budget = MAX_BYTES_PER_PUMP;
  while (budget-- > 0 && Serial1.available()) {
    const uint8_t incoming = (uint8_t)Serial1.read();

    if (payloadActive) {
      opticalPayload[payloadFill++] = incoming;
      if (payloadFill < frameLength) continue;

      payloadActive = false;
      opticalScramble(opticalPayload, frameLength);
      if (opticalCrc32(opticalPayload, frameLength) != frameCrc) {
        crcErrors++;
        if (frameType == FRAME_IMAGE_CHUNK || frameType == FRAME_IMAGE_END) {
          failImage("CRC mismatch");
        } else {
          Serial.println("[ERROR] CRC mismatch on text frame");
        }
        continue;
      }
      handleFrame();
      continue;
    }

    if (headerFill == OPTICAL_HEADER_BYTES) {
      memmove(headerWindow, headerWindow + 1, OPTICAL_HEADER_BYTES - 1);
      headerWindow[OPTICAL_HEADER_BYTES - 1] = incoming;
    } else {
      headerWindow[headerFill++] = incoming;
      if (headerFill < OPTICAL_HEADER_BYTES) continue;
    }

    if (headerValid(headerWindow)) beginPayload();
  }
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\nTERAHERTZ BINARY UART RECEIVER");
  // Hold roughly 350 ms of line data so USB logging and loop scheduling can
  // never starve the parser during a sustained image stream.
  Serial1.setRxBufferSize(RX_BUFFER_BYTES);
  Serial1.begin(BAUD_RATE, SERIAL_8N1, RECEIVER_PIN, -1, true);
  mbedtls_sha256_init(&imageHash);
  resetImageState();
}

void loop() { pumpOptical(); }
