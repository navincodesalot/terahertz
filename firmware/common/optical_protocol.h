#pragma once

#include <Arduino.h>

// Binary-safe UART frame, version 2. Multi-byte values are big-endian.
//
//   [0x54 0x48]   magic
//   [0x02]        version
//   [type  u8]
//   [seq   u16]
//   [len   u16]
//   [crc32 u32]   CRC32 of the plain (unscrambled) payload
//   [payload]     whitened with opticalScramble()
//
// The receiver uses a sliding-window sync: every incoming byte shifts the
// 12-byte window and re-tests. A corrupt length byte therefore cannot cause
// the receiver to swallow subsequent frames — it will simply slide past the
// bad header and re-lock on the next valid one. The header check byte from
// the earlier draft was removed because it required all 13 header bytes to
// be clean before accepting a header. On a marginal optical link that raised
// the effective header failure rate ~5x compared to the original protocol,
// which only checked the 3 magic/version bytes. The sliding window already
// handles the cascade problem without the extra strictness.
//
// Payload whitening (opticalScramble) keeps the mark/space ratio near 50%
// so AC-coupled comparator thresholds do not drift during long runs of 0x00
// or 0xFF in image data.

#define OPTICAL_MAGIC_0 0x54
#define OPTICAL_MAGIC_1 0x48
#define OPTICAL_VERSION 0x02
#define OPTICAL_HEADER_BYTES 12
#define OPTICAL_MAX_PAYLOAD 2048
#define OPTICAL_MAX_FRAME (OPTICAL_HEADER_BYTES + OPTICAL_MAX_PAYLOAD)

enum OpticalFrameType : uint8_t {
  FRAME_TEXT = 1,
  FRAME_IMAGE_START = 2,
  FRAME_IMAGE_CHUNK = 3,
  FRAME_IMAGE_END = 4,
};

inline bool opticalFrameTypeValid(uint8_t type) {
  return type >= FRAME_TEXT && type <= FRAME_IMAGE_END;
}

inline uint32_t opticalCrc32(const uint8_t* data, size_t length) {
  uint32_t crc = 0xFFFFFFFF;
  while (length--) {
    crc ^= *data++;
    for (uint8_t bit = 0; bit < 8; bit++) crc = (crc >> 1) ^ (0xEDB88320 & -(crc & 1));
  }
  return ~crc;
}

// Self-inverse XOR whitening using a fixed LFSR keystream.
// Applying the same function on both ends scrambles then descrambles the
// payload, costing zero bytes of overhead on the wire.
inline void opticalScramble(uint8_t* data, size_t length) {
  uint16_t lfsr = 0xACE1;
  for (size_t index = 0; index < length; index++) {
    for (uint8_t step = 0; step < 8; step++) {
      const uint16_t feedback = (lfsr ^ (lfsr >> 2) ^ (lfsr >> 3) ^ (lfsr >> 5)) & 1u;
      lfsr = (uint16_t)((lfsr >> 1) | (feedback << 15));
    }
    data[index] ^= (uint8_t)lfsr;
  }
}

// Builds a complete frame into `frame` (must be at least OPTICAL_MAX_FRAME
// bytes). Returns the total frame length, or 0 when the payload is too large.
// Serialising to a buffer allows a single Serial1.write() call rather than
// many individual locked byte writes.
inline size_t opticalBuildFrame(uint8_t* frame, OpticalFrameType type, uint16_t sequence,
                                const uint8_t* payload, uint16_t length) {
  if (length > OPTICAL_MAX_PAYLOAD) return 0;
  const uint32_t crc = opticalCrc32(payload, length);
  frame[0]  = OPTICAL_MAGIC_0;
  frame[1]  = OPTICAL_MAGIC_1;
  frame[2]  = OPTICAL_VERSION;
  frame[3]  = (uint8_t)type;
  frame[4]  = (uint8_t)(sequence >> 8);
  frame[5]  = (uint8_t)sequence;
  frame[6]  = (uint8_t)(length >> 8);
  frame[7]  = (uint8_t)length;
  frame[8]  = (uint8_t)(crc >> 24);
  frame[9]  = (uint8_t)(crc >> 16);
  frame[10] = (uint8_t)(crc >> 8);
  frame[11] = (uint8_t)crc;
  memcpy(&frame[OPTICAL_HEADER_BYTES], payload, length);
  opticalScramble(&frame[OPTICAL_HEADER_BYTES], length);
  return OPTICAL_HEADER_BYTES + length;
}
