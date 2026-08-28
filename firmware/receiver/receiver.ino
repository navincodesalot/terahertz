#include <Arduino.h>
#include <ArduinoJson.h>
#include <mbedtls/sha256.h>

#include "../common/optical_protocol.h"

#define RECEIVER_PIN 5
#define BAUD_RATE 250000
#define RX_BUFFER_BYTES 8192
#define MAX_BYTES_PER_PUMP 2048
#define IMAGE_PROGRESS_LOG_INTERVAL 32
#define IMAGE_SESSION_TIMEOUT_MS 3000
#define MAX_IMAGE_ID_BYTES 64

OpticalFrameParser parser;
mbedtls_sha256_context imageHash;

bool imageActive = false;
bool imageFailed = false;
uint16_t expectedChunks = 0;
uint16_t nextChunk = 0;
uint32_t expectedBytes = 0;
uint32_t receivedBytes = 0;
unsigned long imageStartedAt = 0;
unsigned long imageLastActivityAt = 0;
char imageId[MAX_IMAGE_ID_BYTES + 1] = {0};
char expectedSha256[65] = {0};

struct LinkMetrics {
  uint32_t textSuccesses = 0;
  uint32_t textFailures = 0;
  uint32_t imagesStarted = 0;
  uint32_t imagesVerified = 0;
  uint32_t imagesFailed = 0;
  uint32_t headerCrcErrors = 0;
  uint32_t payloadCrcErrors = 0;
  uint32_t resyncs = 0;
  uint32_t parserTimeouts = 0;
} metrics;

void printLinkMetrics() {
  Serial.printf("LINK METRICS text_ok=%lu text_failed=%lu images_started=%lu "
                "images_verified=%lu images_failed=%lu header_crc=%lu "
                "payload_crc=%lu resyncs=%lu parser_timeouts=%lu\n",
                (unsigned long)metrics.textSuccesses,
                (unsigned long)metrics.textFailures,
                (unsigned long)metrics.imagesStarted,
                (unsigned long)metrics.imagesVerified,
                (unsigned long)metrics.imagesFailed,
                (unsigned long)metrics.headerCrcErrors,
                (unsigned long)metrics.payloadCrcErrors,
                (unsigned long)metrics.resyncs,
                (unsigned long)metrics.parserTimeouts);
}

void resetImageState() {
  mbedtls_sha256_free(&imageHash);
  mbedtls_sha256_init(&imageHash);
  imageActive = false;
  imageFailed = false;
  expectedChunks = 0;
  nextChunk = 0;
  expectedBytes = 0;
  receivedBytes = 0;
  imageStartedAt = 0;
  imageLastActivityAt = 0;
  imageId[0] = '\0';
  expectedSha256[0] = '\0';
}

void failImage(const char* reason) {
  if (!imageActive || imageFailed) return;
  imageFailed = true;
  metrics.imagesFailed++;
  Serial.printf("IMAGE FAILED id=%s reason=%s bytes=%lu/%lu chunks=%u/%u\n",
                imageId, reason, (unsigned long)receivedBytes,
                (unsigned long)expectedBytes, nextChunk, expectedChunks);
  printLinkMetrics();
}

void failImageWithoutSession(const char* reason) {
  metrics.imagesFailed++;
  Serial.printf("IMAGE FAILED id=unknown reason=%s\n", reason);
  printLinkMetrics();
}

void failText(uint16_t sequence, const char* reason) {
  metrics.textFailures++;
  Serial.printf("TEXT FAILED sequence=%u reason=%s\n", sequence, reason);
  printLinkMetrics();
}

bool copySha256(const char* value) {
  if (value == nullptr || strlen(value) != 64) return false;
  for (uint8_t index = 0; index < 64; index++) {
    char c = value[index];
    if (c >= 'A' && c <= 'F') c = (char)(c - 'A' + 'a');
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
      return false;
    }
    expectedSha256[index] = c;
  }
  expectedSha256[64] = '\0';
  return true;
}

void handleImageStart(uint16_t sequence, const uint8_t* payload,
                      uint16_t length) {
  JsonDocument document;
  const DeserializationError error = deserializeJson(document, payload, length);
  const char* id = document["id"] | "";
  const unsigned long totalBytes = document["totalBytes"] | 0UL;
  const unsigned long chunkCount = document["chunkCount"] | 0UL;

  if (imageActive) failImage("restarted");
  resetImageState();
  if (error || sequence != 0 || id[0] == '\0' ||
      strlen(id) > MAX_IMAGE_ID_BYTES || totalBytes == 0 ||
      chunkCount == 0 || chunkCount > UINT16_MAX ||
      chunkCount != ((totalBytes - 1) / OPTICAL_MAX_CHUNK) + 1) {
    failImageWithoutSession("start_metadata");
    return;
  }

  strncpy(imageId, id, MAX_IMAGE_ID_BYTES);
  imageId[MAX_IMAGE_ID_BYTES] = '\0';
  expectedBytes = (uint32_t)totalBytes;
  expectedChunks = (uint16_t)chunkCount;
  imageActive = true;
  imageStartedAt = millis();
  imageLastActivityAt = imageStartedAt;
  metrics.imagesStarted++;
  mbedtls_sha256_starts(&imageHash, 0);
  Serial.printf("IMAGE START id=%s bytes=%lu chunks=%u\n", imageId,
                (unsigned long)expectedBytes, expectedChunks);
}

void handleImageChunk(uint16_t sequence, const uint8_t* payload,
                      uint16_t length) {
  if (!imageActive) return;
  imageLastActivityAt = millis();
  if (imageFailed) return;
  if (sequence != nextChunk || nextChunk >= expectedChunks) {
    failImage("sequence");
    return;
  }
  if (receivedBytes > expectedBytes ||
      length > expectedBytes - receivedBytes) {
    failImage("size");
    return;
  }

  const uint32_t remaining = expectedBytes - receivedBytes;
  const uint16_t expectedLength =
      remaining > OPTICAL_MAX_CHUNK ? OPTICAL_MAX_CHUNK : (uint16_t)remaining;
  if (length != expectedLength) {
    failImage("chunk_length");
    return;
  }

  mbedtls_sha256_update(&imageHash, payload, length);
  receivedBytes += length;
  nextChunk++;
  if (nextChunk % IMAGE_PROGRESS_LOG_INTERVAL == 0 ||
      nextChunk == expectedChunks) {
    Serial.printf("IMAGE PROGRESS id=%s chunks=%u/%u bytes=%lu/%lu\n",
                  imageId, nextChunk, expectedChunks,
                  (unsigned long)receivedBytes,
                  (unsigned long)expectedBytes);
  }
}

void handleImageEnd(uint16_t sequence, const uint8_t* payload,
                    uint16_t length) {
  if (!imageActive) {
    Serial.println("FRAME FAILED reason=end_without_start");
    return;
  }

  imageLastActivityAt = millis();
  JsonDocument document;
  const DeserializationError error = deserializeJson(document, payload, length);
  const char* endId = document["id"] | "";
  const char* sha256 = document["sha256"] | "";
  if (error || strcmp(endId, imageId) != 0 || !copySha256(sha256))
    failImage("end_metadata");
  if (sequence != expectedChunks || nextChunk != expectedChunks)
    failImage("sequence");
  if (receivedBytes != expectedBytes) failImage("size");

  uint8_t actualDigest[32];
  char actualHex[65];
  mbedtls_sha256_finish(&imageHash, actualDigest);
  for (uint8_t index = 0; index < 32; index++) {
    snprintf(&actualHex[index * 2], 3, "%02x", actualDigest[index]);
  }
  actualHex[64] = '\0';
  if (strcmp(expectedSha256, actualHex) != 0) failImage("sha256");

  const unsigned long elapsed = millis() - imageStartedAt;
  const unsigned long rate =
      elapsed > 0 ? (unsigned long)((receivedBytes * 8000ULL) / elapsed) : 0;
  if (!imageFailed) {
    metrics.imagesVerified++;
    Serial.printf("IMAGE VERIFIED id=%s bytes=%lu chunks=%u sha256=ok "
                  "time_ms=%lu rate_bps=%lu\n",
                  imageId, (unsigned long)receivedBytes, nextChunk, elapsed,
                  rate);
    printLinkMetrics();
  } else {
    Serial.printf("IMAGE SUMMARY id=%s status=failed bytes=%lu/%lu "
                  "chunks=%u/%u time_ms=%lu rate_bps=%lu\n",
                  imageId, (unsigned long)receivedBytes,
                  (unsigned long)expectedBytes, nextChunk, expectedChunks,
                  elapsed, rate);
  }
  resetImageState();
}

void handleFrame() {
  switch (parser.frameType()) {
    case FRAME_TEXT:
      metrics.textSuccesses++;
      Serial.printf("TEXT OK sequence=%u bytes=%u crc=ok data=",
                    parser.sequence(), parser.length());
      Serial.write(parser.payload(), parser.length());
      Serial.println();
      if (metrics.textSuccesses % 100 == 0) printLinkMetrics();
      break;
    case FRAME_IMAGE_START:
      handleImageStart(parser.sequence(), parser.payload(), parser.length());
      break;
    case FRAME_IMAGE_CHUNK:
      handleImageChunk(parser.sequence(), parser.payload(), parser.length());
      break;
    case FRAME_IMAGE_END:
      handleImageEnd(parser.sequence(), parser.payload(), parser.length());
      break;
  }
}

void handleParserEvent(OpticalParserEvent event,
                       OpticalParserState previousState,
                       OpticalFrameType previousType) {
  switch (event) {
    case OPTICAL_NO_EVENT:
      break;
    case OPTICAL_FRAME_READY:
      handleFrame();
      break;
    case OPTICAL_HEADER_CRC_ERROR:
      metrics.headerCrcErrors++;
      if (imageActive) failImage("header_crc");
      break;
    case OPTICAL_HEADER_INVALID:
      if (imageActive) failImage("header");
      break;
    case OPTICAL_PAYLOAD_CRC_ERROR:
      metrics.payloadCrcErrors++;
      if (previousType == FRAME_TEXT)
        failText(parser.sequence(), "payload_crc");
      else if (imageActive)
        failImage("payload_crc");
      break;
    case OPTICAL_UNEXPECTED_SYNC:
      metrics.resyncs++;
      if (previousState == READ_PAYLOAD && previousType == FRAME_TEXT)
        failText(parser.sequence(), "unexpected_sync");
      else if (imageActive)
        failImage("unexpected_sync");
      break;
    case OPTICAL_INACTIVITY_TIMEOUT:
      metrics.parserTimeouts++;
      if (previousState == READ_PAYLOAD && previousType == FRAME_TEXT)
        failText(parser.sequence(), "timeout");
      else if (imageActive)
        failImage("timeout");
      break;
  }
}

void pumpOptical() {
  if (!Serial1.available()) {
    const OpticalParserState previousState = parser.state();
    const OpticalFrameType previousType = parser.frameType();
    handleParserEvent(parser.poll(millis()), previousState, previousType);
  }

  uint16_t budget = MAX_BYTES_PER_PUMP;
  while (budget-- > 0 && Serial1.available()) {
    const uint8_t incoming = (uint8_t)Serial1.read();
    const OpticalParserState previousState = parser.state();
    const OpticalFrameType previousType = parser.frameType();
    handleParserEvent(parser.push(incoming, millis()), previousState,
                      previousType);
  }
}

void checkImageSessionTimeout() {
  if (!imageActive ||
      (uint32_t)(millis() - imageLastActivityAt) <=
          IMAGE_SESSION_TIMEOUT_MS) {
    return;
  }

  if (!imageFailed) failImage("session_timeout");
  resetImageState();
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\nTERAHERTZ OPTICAL RECEIVER");
  Serial.printf("Protocol v%u baud=%u chunk=%u payload=%u timeout_ms=%u\n",
                OPTICAL_VERSION, BAUD_RATE, OPTICAL_MAX_CHUNK,
                OPTICAL_MAX_PAYLOAD, OPTICAL_INTERBYTE_TIMEOUT_MS);

  Serial1.setRxBufferSize(RX_BUFFER_BYTES);
  Serial1.begin(BAUD_RATE, SERIAL_8N1, RECEIVER_PIN, -1, true);
  mbedtls_sha256_init(&imageHash);
  resetImageState();
}

void loop() {
  pumpOptical();
  checkImageSessionTimeout();
}
