#pragma once

#include <Arduino.h>

// Binary-safe UART frame. All multi-byte values are big-endian.
// [TH 01][type][sequence u16][payload length u16][CRC32 u32][payload]
#define OPTICAL_MAGIC_0 0x54
#define OPTICAL_MAGIC_1 0x48
#define OPTICAL_VERSION 0x01
#define OPTICAL_HEADER_BYTES 12
#define OPTICAL_MAX_PAYLOAD 4096

enum OpticalFrameType : uint8_t {
  FRAME_TEXT = 1,
  FRAME_IMAGE_START = 2,
  FRAME_IMAGE_CHUNK = 3,
  FRAME_IMAGE_END = 4,
};

inline uint32_t opticalCrc32(const uint8_t* data, size_t length) {
  uint32_t crc = 0xFFFFFFFF;
  while (length--) {
    crc ^= *data++;
    for (uint8_t bit = 0; bit < 8; bit++) crc = (crc >> 1) ^ (0xEDB88320 & -(crc & 1));
  }
  return ~crc;
}

inline void writeU16(Stream& stream, uint16_t value) {
  stream.write((uint8_t)(value >> 8));
  stream.write((uint8_t)value);
}

inline void writeU32(Stream& stream, uint32_t value) {
  stream.write((uint8_t)(value >> 24));
  stream.write((uint8_t)(value >> 16));
  stream.write((uint8_t)(value >> 8));
  stream.write((uint8_t)value);
}

inline bool writeOpticalFrame(Stream& stream, OpticalFrameType type, uint16_t sequence, const uint8_t* payload, uint16_t length) {
  if (length > OPTICAL_MAX_PAYLOAD) return false;
  stream.write(OPTICAL_MAGIC_0);
  stream.write(OPTICAL_MAGIC_1);
  stream.write(OPTICAL_VERSION);
  stream.write((uint8_t)type);
  writeU16(stream, sequence);
  writeU16(stream, length);
  writeU32(stream, opticalCrc32(payload, length));
  stream.write(payload, length);
  return true;
}
