#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

// Wire format (multi-byte values are big-endian):
//   [sync 4][version 1][type 1][sequence 2][length 2]
//   [payload CRC32 4][header CRC16 2][whitened payload]

#define OPTICAL_SYNC_BYTES 4
#define OPTICAL_HEADER_DATA_BYTES 10
#define OPTICAL_HEADER_BYTES 12
#define OPTICAL_FRAME_OVERHEAD (OPTICAL_SYNC_BYTES + OPTICAL_HEADER_BYTES)
#define OPTICAL_MAX_CHUNK 1024
#define OPTICAL_MAX_PAYLOAD 2048
#define OPTICAL_MAX_FRAME (OPTICAL_FRAME_OVERHEAD + OPTICAL_MAX_PAYLOAD)
#define OPTICAL_VERSION 0x03
#define OPTICAL_INTERBYTE_TIMEOUT_MS 50

static const uint8_t OPTICAL_SYNC[OPTICAL_SYNC_BYTES] = {0x55, 0x55, 0x54, 0x48};

enum OpticalFrameType : uint8_t {
  FRAME_TEXT = 1,
  FRAME_IMAGE_START = 2,
  FRAME_IMAGE_CHUNK = 3,
  FRAME_IMAGE_END = 4,
};

inline bool opticalFrameTypeValid(uint8_t type) {
  return type >= FRAME_TEXT && type <= FRAME_IMAGE_END;
}

inline uint16_t opticalMaxPayloadForType(uint8_t type) {
  return type == FRAME_IMAGE_CHUNK ? OPTICAL_MAX_CHUNK : OPTICAL_MAX_PAYLOAD;
}

inline uint16_t opticalReadU16(const uint8_t* data) {
  return ((uint16_t)data[0] << 8) | data[1];
}

inline uint32_t opticalReadU32(const uint8_t* data) {
  return ((uint32_t)data[0] << 24) | ((uint32_t)data[1] << 16) |
         ((uint32_t)data[2] << 8) | data[3];
}

inline uint16_t opticalCrc16(const uint8_t* data, size_t length) {
  uint16_t crc = 0xFFFF;
  while (length--) {
    crc ^= (uint16_t)*data++ << 8;
    for (uint8_t bit = 0; bit < 8; bit++) {
      crc = (uint16_t)((crc << 1) ^ ((crc & 0x8000) ? 0x1021 : 0));
    }
  }
  return crc;
}

inline uint32_t opticalCrc32(const uint8_t* data, size_t length) {
  uint32_t crc = 0xFFFFFFFF;
  while (length--) {
    crc ^= *data++;
    for (uint8_t bit = 0; bit < 8; bit++) {
      crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)-(int32_t)(crc & 1));
    }
  }
  return ~crc;
}

// Self-inverse XOR whitening using a fixed LFSR keystream.
inline void opticalScramble(uint8_t* data, size_t length) {
  uint16_t lfsr = 0xACE1;
  for (size_t index = 0; index < length; index++) {
    for (uint8_t step = 0; step < 8; step++) {
      const uint16_t feedback =
          (lfsr ^ (lfsr >> 2) ^ (lfsr >> 3) ^ (lfsr >> 5)) & 1u;
      lfsr = (uint16_t)((lfsr >> 1) | (feedback << 15));
    }
    data[index] ^= (uint8_t)lfsr;
  }
}

// Builds one complete frame for one contiguous UART write.
inline size_t opticalBuildFrame(uint8_t* frame, OpticalFrameType type,
                                uint16_t sequence, const uint8_t* payload,
                                uint16_t length) {
  if (frame == nullptr || payload == nullptr || !opticalFrameTypeValid(type) ||
      length == 0 || length > opticalMaxPayloadForType(type)) {
    return 0;
  }

  memcpy(frame, OPTICAL_SYNC, OPTICAL_SYNC_BYTES);
  uint8_t* header = frame + OPTICAL_SYNC_BYTES;
  const uint32_t payloadCrc = opticalCrc32(payload, length);
  header[0] = OPTICAL_VERSION;
  header[1] = (uint8_t)type;
  header[2] = (uint8_t)(sequence >> 8);
  header[3] = (uint8_t)sequence;
  header[4] = (uint8_t)(length >> 8);
  header[5] = (uint8_t)length;
  header[6] = (uint8_t)(payloadCrc >> 24);
  header[7] = (uint8_t)(payloadCrc >> 16);
  header[8] = (uint8_t)(payloadCrc >> 8);
  header[9] = (uint8_t)payloadCrc;
  const uint16_t headerCrc = opticalCrc16(header, OPTICAL_HEADER_DATA_BYTES);
  header[10] = (uint8_t)(headerCrc >> 8);
  header[11] = (uint8_t)headerCrc;

  uint8_t* encodedPayload = frame + OPTICAL_FRAME_OVERHEAD;
  memcpy(encodedPayload, payload, length);
  opticalScramble(encodedPayload, length);
  return OPTICAL_FRAME_OVERHEAD + length;
}

enum OpticalParserState : uint8_t {
  SEARCH_SYNC,
  READ_HEADER,
  READ_PAYLOAD,
};

enum OpticalParserEvent : uint8_t {
  OPTICAL_NO_EVENT,
  OPTICAL_FRAME_READY,
  OPTICAL_HEADER_CRC_ERROR,
  OPTICAL_HEADER_INVALID,
  OPTICAL_PAYLOAD_CRC_ERROR,
  OPTICAL_UNEXPECTED_SYNC,
  OPTICAL_INACTIVITY_TIMEOUT,
};

class OpticalFrameParser {
 public:
  OpticalFrameParser() { reset(); }

  void reset() {
    state_ = SEARCH_SYNC;
    syncWindow_ = 0;
    syncFill_ = 0;
    headerFill_ = 0;
    payloadFill_ = 0;
    frameType_ = FRAME_TEXT;
    frameSequence_ = 0;
    frameLength_ = 0;
    frameCrc_ = 0;
    lastByteAt_ = 0;
  }

  OpticalParserState state() const { return state_; }
  OpticalFrameType frameType() const { return frameType_; }
  uint16_t sequence() const { return frameSequence_; }
  uint16_t length() const { return frameLength_; }
  const uint8_t* payload() const { return payload_; }

  OpticalParserEvent poll(uint32_t now) {
    if (state_ != SEARCH_SYNC &&
        (uint32_t)(now - lastByteAt_) > OPTICAL_INTERBYTE_TIMEOUT_MS) {
      startSearch();
      return OPTICAL_INACTIVITY_TIMEOUT;
    }
    return OPTICAL_NO_EVENT;
  }

  OpticalParserEvent push(uint8_t incoming, uint32_t now) {
    syncWindow_ = (syncWindow_ << 8) | incoming;
    if (syncFill_ < OPTICAL_SYNC_BYTES) syncFill_++;
    const bool foundSync =
        syncFill_ == OPTICAL_SYNC_BYTES && syncWindow_ == syncWord();

    if (foundSync) {
      const OpticalParserState interruptedState = state_;
      state_ = READ_HEADER;
      syncWindow_ = 0;
      syncFill_ = 0;
      headerFill_ = 0;
      payloadFill_ = 0;
      lastByteAt_ = now;
      return interruptedState == SEARCH_SYNC ? OPTICAL_NO_EVENT
                                             : OPTICAL_UNEXPECTED_SYNC;
    }

    if (state_ == SEARCH_SYNC) return OPTICAL_NO_EVENT;

    lastByteAt_ = now;
    if (state_ == READ_HEADER) {
      header_[headerFill_++] = incoming;
      if (headerFill_ < OPTICAL_HEADER_BYTES) return OPTICAL_NO_EVENT;

      // Header integrity is checked before version, type, or length is trusted.
      const uint16_t expectedHeaderCrc =
          opticalReadU16(header_ + OPTICAL_HEADER_DATA_BYTES);
      if (opticalCrc16(header_, OPTICAL_HEADER_DATA_BYTES) !=
          expectedHeaderCrc) {
        startSearch();
        return OPTICAL_HEADER_CRC_ERROR;
      }

      const uint8_t type = header_[1];
      const uint16_t length = opticalReadU16(header_ + 4);
      if (header_[0] != OPTICAL_VERSION || !opticalFrameTypeValid(type) ||
          length == 0 || length > opticalMaxPayloadForType(type)) {
        startSearch();
        return OPTICAL_HEADER_INVALID;
      }

      frameType_ = (OpticalFrameType)type;
      frameSequence_ = opticalReadU16(header_ + 2);
      frameLength_ = length;
      frameCrc_ = opticalReadU32(header_ + 6);
      payloadFill_ = 0;
      state_ = READ_PAYLOAD;
      return OPTICAL_NO_EVENT;
    }

    payload_[payloadFill_++] = incoming;
    if (payloadFill_ < frameLength_) return OPTICAL_NO_EVENT;
    return finishPayload();
  }

 private:
  static uint32_t syncWord() {
    return ((uint32_t)OPTICAL_SYNC[0] << 24) |
           ((uint32_t)OPTICAL_SYNC[1] << 16) |
           ((uint32_t)OPTICAL_SYNC[2] << 8) | OPTICAL_SYNC[3];
  }

  OpticalParserEvent finishPayload() {
    opticalScramble(payload_, frameLength_);
    const bool crcValid = opticalCrc32(payload_, frameLength_) == frameCrc_;
    // A dropped payload byte can make the first one to three bytes of the next
    // sync satisfy the declared length. Preserve the rolling window on CRC
    // failure so the remaining sync bytes can still reacquire that frame.
    startSearch(!crcValid);
    return crcValid ? OPTICAL_FRAME_READY : OPTICAL_PAYLOAD_CRC_ERROR;
  }

  void startSearch(bool preserveSync = false) {
    state_ = SEARCH_SYNC;
    if (!preserveSync) {
      syncWindow_ = 0;
      syncFill_ = 0;
    }
    headerFill_ = 0;
    payloadFill_ = 0;
    lastByteAt_ = 0;
  }

  OpticalParserState state_;
  uint32_t syncWindow_;
  uint8_t syncFill_;
  uint8_t header_[OPTICAL_HEADER_BYTES];
  uint8_t headerFill_;
  uint8_t payload_[OPTICAL_MAX_PAYLOAD];
  uint16_t payloadFill_;
  OpticalFrameType frameType_;
  uint16_t frameSequence_;
  uint16_t frameLength_;
  uint32_t frameCrc_;
  uint32_t lastByteAt_;
};
