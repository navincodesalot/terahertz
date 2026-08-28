#pragma once

#include <Arduino.h>

// Binary-safe UART frame, version 2. Multi-byte values are big-endian.
//
//   [0x55 0x55]   preamble  - a perfect square wave on the line. Lets an
//                             AC-coupled photodiode front end settle before
//                             the first real bit and gives the UART something
//                             clean to lock onto after an idle gap.
//   [0x54 0x48]   magic
//   [version]
//   [type]
//   [sequence u16]
//   [length   u16]
//   [crc32    u32]          - CRC32 of the plain (unscrambled) payload
//   [check    u8]           - rotate-xor over the 12 header bytes above
//   [payload]               - scrambled, see opticalScramble()
//
// The header check byte is what stops one corrupted bit from wrecking the
// whole stream: without it the receiver trusts a garbage length field and
// swallows the frames that follow it.

#define OPTICAL_PREAMBLE 0x55
#define OPTICAL_PREAMBLE_BYTES 2
#define OPTICAL_MAGIC_0 0x54
#define OPTICAL_MAGIC_1 0x48
#define OPTICAL_VERSION 0x02
#define OPTICAL_HEADER_BYTES 13
#define OPTICAL_MAX_PAYLOAD 2048
#define OPTICAL_MAX_FRAME \
  (OPTICAL_PREAMBLE_BYTES + OPTICAL_HEADER_BYTES + OPTICAL_MAX_PAYLOAD)

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

// Rotate-and-xor rather than a plain sum so that swapped or shifted header
// bytes are caught as well as flipped ones.
inline uint8_t opticalHeaderCheck(const uint8_t* header) {
  uint8_t check = 0xA5;
  for (uint8_t index = 0; index < OPTICAL_HEADER_BYTES - 1; index++) {
    check = (uint8_t)(((check << 1) | (check >> 7)) ^ header[index]);
  }
  return check;
}

// Self-inverse XOR whitening. Image payloads contain long runs of 0x00 and
// 0xFF; over an inverted optical UART those runs hold the laser at a 90% or
// 10% duty cycle, which drags an AC-coupled comparator threshold off centre
// and produces bit errors. Whitening keeps the duty cycle near 50% and costs
// zero bytes on the wire. Applied to the payload only, so the plain magic
// bytes remain searchable for resynchronisation.
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

// Serialises a complete frame into `frame`, which must hold OPTICAL_MAX_FRAME
// bytes. Returns the frame length, or 0 when the payload is too large.
// Building the frame contiguously lets the caller issue a single UART write
// instead of a dozen locked single-byte writes.
inline size_t opticalBuildFrame(uint8_t* frame, OpticalFrameType type, uint16_t sequence,
                                const uint8_t* payload, uint16_t length) {
  if (length > OPTICAL_MAX_PAYLOAD) return 0;

  size_t offset = 0;
  for (uint8_t index = 0; index < OPTICAL_PREAMBLE_BYTES; index++) {
    frame[offset++] = OPTICAL_PREAMBLE;
  }

  const uint32_t crc = opticalCrc32(payload, length);
  uint8_t* header = &frame[offset];
  header[0] = OPTICAL_MAGIC_0;
  header[1] = OPTICAL_MAGIC_1;
  header[2] = OPTICAL_VERSION;
  header[3] = (uint8_t)type;
  header[4] = (uint8_t)(sequence >> 8);
  header[5] = (uint8_t)sequence;
  header[6] = (uint8_t)(length >> 8);
  header[7] = (uint8_t)length;
  header[8] = (uint8_t)(crc >> 24);
  header[9] = (uint8_t)(crc >> 16);
  header[10] = (uint8_t)(crc >> 8);
  header[11] = (uint8_t)crc;
  header[12] = opticalHeaderCheck(header);
  offset += OPTICAL_HEADER_BYTES;

  memcpy(&frame[offset], payload, length);
  opticalScramble(&frame[offset], length);
  return offset + length;
}
