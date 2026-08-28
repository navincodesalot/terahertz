#pragma once

// Shared newline-delimited optical transport protocol.
//
// Wire record formats (newline-terminated):
//   Text:        THZTXT|<id>|<crc32hex>|<payload>
//   Image start: THZIMG|S|<id>|<chunkCount>|<totalBytes>|<mimeType>
//   Image chunk: THZIMG|C|<id>|<index>|<base64data>|<crc32hex>
//   Image end:   THZIMG|E|<id>|<sha256hex>
//
// Constraints:
//   - Payloads must not contain '\n' or '\r'.
//   - '|' may appear in text payloads; parsers use field-count position splits.
//   - CRC-32 covers the payload/base64 string bytes, not the framing.

#define MAX_TRANSFER_BYTES (250 * 1024)
#define IMAGE_CHUNK_BYTES  1024

// CRC-32 (ISO 3309 / Ethernet polynomial).
inline uint32_t opticalCrc32(const uint8_t* data, size_t length) {
  uint32_t crc = 0xFFFFFFFF;
  while (length--) {
    crc ^= *data++;
    for (uint8_t bit = 0; bit < 8; bit++) {
      crc = (crc >> 1) ^ (0xEDB88320u & -(crc & 1));
    }
  }
  return ~crc;
}

// Write an 8-character lowercase hex CRC-32 into out[9] (including '\0').
inline void opticalCrc32Hex(const uint8_t* data, size_t length, char out[9]) {
  const uint32_t crc = opticalCrc32(data, length);
  snprintf(out, 9, "%08lx", (unsigned long)crc);
}
