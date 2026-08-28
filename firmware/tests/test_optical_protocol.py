# pyright: reportUnannotatedClassAttribute=false, reportUnknownMemberType=false, reportUnusedCallResult=false, reportMissingParameterType=false

import base64
import binascii
import struct
import unittest
import zlib
from pathlib import Path

SYNC = b"\x55\x55\x54\x48"
VERSION = 3
HEADER_DATA_BYTES = 10
HEADER_BYTES = 12
MAX_CHUNK = 1024
MAX_PAYLOAD = 2048
TEXT = 1
IMAGE_START = 2
IMAGE_CHUNK = 3
IMAGE_END = 4
TIMEOUT_MS = 50

ROOT = Path(__file__).resolve().parents[1]


def crc16(data: bytes) -> int:
    return binascii.crc_hqx(data, 0xFFFF)


def whiten(data: bytes) -> bytes:
    result = bytearray(data)
    lfsr = 0xACE1
    for index in range(len(result)):
        for _ in range(8):
            feedback = (lfsr ^ (lfsr >> 2) ^ (lfsr >> 3) ^ (lfsr >> 5)) & 1
            lfsr = ((lfsr >> 1) | (feedback << 15)) & 0xFFFF
        result[index] ^= lfsr & 0xFF
    return bytes(result)


def build_frame(frame_type: int, sequence: int, payload: bytes) -> bytes:
    limit = MAX_CHUNK if frame_type == IMAGE_CHUNK else MAX_PAYLOAD
    if not payload or len(payload) > limit:
        raise ValueError("invalid payload length")
    payload_crc = zlib.crc32(payload) & 0xFFFFFFFF
    header_data = struct.pack(">BBHHI", VERSION, frame_type, sequence,
                              len(payload), payload_crc)
    header = header_data + struct.pack(">H", crc16(header_data))
    return SYNC + header + whiten(payload)


class Parser:
    SEARCH_SYNC = 0
    READ_HEADER = 1
    READ_PAYLOAD = 2

    def __init__(self):
        self.state = self.SEARCH_SYNC
        self.sync_window = bytearray()
        self.header = bytearray()
        self.payload = bytearray()
        self.last_byte_at = 0
        self.frame_type = TEXT
        self.sequence = 0
        self.length = 0
        self.payload_crc = 0
        self.frames = []

    def _search(self, preserve_sync=False):
        self.state = self.SEARCH_SYNC
        if not preserve_sync:
            self.sync_window.clear()
        self.header.clear()
        self.payload.clear()
        self.last_byte_at = 0

    def _finish_payload(self):
        plain = whiten(bytes(self.payload))
        if zlib.crc32(plain) & 0xFFFFFFFF != self.payload_crc:
            self._search(preserve_sync=True)
            return "payload_crc"
        self.frames.append((self.frame_type, self.sequence, plain))
        self._search()
        return "frame"

    def poll(self, now: int):
        if self.state != self.SEARCH_SYNC and now - self.last_byte_at > TIMEOUT_MS:
            self._search()
            return "timeout"
        return None

    def push(self, incoming: int, now: int = 0):
        self.sync_window.append(incoming)
        self.sync_window = self.sync_window[-len(SYNC):]
        if bytes(self.sync_window) == SYNC:
            interrupted = self.state
            self.state = self.READ_HEADER
            self.sync_window.clear()
            self.header.clear()
            self.payload.clear()
            self.last_byte_at = now
            return None if interrupted == self.SEARCH_SYNC else "unexpected_sync"

        if self.state == self.SEARCH_SYNC:
            return None

        self.last_byte_at = now
        if self.state == self.READ_HEADER:
            self.header.append(incoming)
            if len(self.header) < HEADER_BYTES:
                return None
            header_data = bytes(self.header[:HEADER_DATA_BYTES])
            expected_crc = struct.unpack(">H", self.header[HEADER_DATA_BYTES:])[0]
            if crc16(header_data) != expected_crc:
                self._search()
                return "header_crc"

            version, frame_type, sequence, length, payload_crc = struct.unpack(
                ">BBHHI", header_data)
            limit = MAX_CHUNK if frame_type == IMAGE_CHUNK else MAX_PAYLOAD
            if (version != VERSION or frame_type not in range(TEXT, IMAGE_END + 1)
                    or length == 0 or length > limit):
                self._search()
                return "header_invalid"
            self.frame_type = frame_type
            self.sequence = sequence
            self.length = length
            self.payload_crc = payload_crc
            self.payload.clear()
            self.state = self.READ_PAYLOAD
            return None

        self.payload.append(incoming)
        if len(self.payload) == self.length:
            return self._finish_payload()
        return None

    def feed(self, data: bytes, now: int = 0):
        return [event for byte in data if (event := self.push(byte, now))]


class OpticalProtocolTests(unittest.TestCase):
    def test_deterministic_frame_vector(self):
        frame = build_frame(TEXT, 0x1234, b"HELLO")
        self.assertEqual(
            frame.hex(),
            "55555448030112340005c144643627dde4670b7b8b",
        )

    def test_whitening_is_self_inverse(self):
        payload = bytes(range(256)) * 4
        self.assertEqual(whiten(whiten(payload)), payload)

    def test_type_specific_payload_bounds(self):
        self.assertEqual(len(build_frame(IMAGE_CHUNK, 1, b"x" * 1024)), 1040)
        with self.assertRaises(ValueError):
            build_frame(IMAGE_CHUNK, 1, b"x" * 1025)
        self.assertEqual(len(build_frame(TEXT, 1, b"x" * 2048)), 2064)
        with self.assertRaises(ValueError):
            build_frame(TEXT, 1, b"x" * 2049)

    def test_header_crc_is_checked_before_corrupt_length(self):
        damaged = bytearray(build_frame(TEXT, 1, b"first"))
        damaged[8:10] = b"\xff\xff"
        parser = Parser()
        events = parser.feed(bytes(damaged) + build_frame(TEXT, 2, b"next"))
        self.assertEqual(events, ["header_crc", "frame"])
        self.assertEqual(parser.frames, [(TEXT, 2, b"next")])

    def test_plausible_bad_length_is_interrupted_by_full_sync(self):
        damaged = bytearray(build_frame(IMAGE_CHUNK, 4, b"abcdefgh"))
        header_data = bytearray(damaged[4:14])
        header_data[4:6] = struct.pack(">H", 100)
        damaged[4:14] = header_data
        damaged[14:16] = struct.pack(">H", crc16(bytes(header_data)))
        truncated = bytes(damaged[:19])

        parser = Parser()
        events = parser.feed(truncated + build_frame(TEXT, 9, b"recovered"))
        self.assertEqual(events, ["unexpected_sync", "frame"])
        self.assertEqual(parser.frames[-1], (TEXT, 9, b"recovered"))

    def test_valid_encoded_sync_prefix_tail_allows_immediate_next_frame(self):
        for prefix_length in (1, 2, 3):
            with self.subTest(prefix_length=prefix_length):
                encoded = b"\x10\x20\x30\x40\x50" + SYNC[:prefix_length]
                payload = whiten(encoded)
                first = build_frame(TEXT, 20 + prefix_length, payload)
                second = build_frame(TEXT, 30 + prefix_length, b"next")
                self.assertEqual(first[16:], encoded)

                parser = Parser()
                events = parser.feed(first + second)
                self.assertEqual(events, ["frame", "frame"])
                self.assertEqual(
                    parser.frames,
                    [
                        (TEXT, 20 + prefix_length, payload),
                        (TEXT, 30 + prefix_length, b"next"),
                    ],
                )

    def test_dropped_payload_byte_reacquires_immediate_next_frame(self):
        damaged = build_frame(IMAGE_CHUNK, 3, b"0123456789")
        damaged = damaged[:18] + damaged[19:]
        parser = Parser()
        events = parser.feed(damaged + build_frame(TEXT, 10, b"next"))
        self.assertEqual(events, ["payload_crc", "frame"])
        self.assertEqual(parser.frames[-1], (TEXT, 10, b"next"))

    def test_payload_corruption_reacquires_next_frame(self):
        damaged = bytearray(build_frame(IMAGE_CHUNK, 7, b"payload"))
        damaged[-1] ^= 0x80
        parser = Parser()
        events = parser.feed(bytes(damaged) + build_frame(TEXT, 11, b"good"))
        self.assertEqual(events, ["payload_crc", "frame"])
        self.assertEqual(parser.frames[-1], (TEXT, 11, b"good"))

    def test_interbyte_timeout_resets_partial_header(self):
        parser = Parser()
        parser.feed(SYNC + b"\x03\x01", now=100)
        self.assertEqual(parser.state, Parser.READ_HEADER)
        self.assertEqual(parser.poll(151), "timeout")
        self.assertEqual(parser.state, Parser.SEARCH_SYNC)
        self.assertEqual(parser.feed(build_frame(TEXT, 12, b"after"), 152), ["frame"])

    def test_strict_base64_expectations(self):
        valid = {"TQ==": b"M", "TWE=": b"Ma", "TWFu": b"Man"}
        for encoded, expected in valid.items():
            self.assertEqual(base64.b64decode(encoded, validate=True), expected)
        for invalid in ("", "TQ", "T Q==", "TQ=A", "TQ===", "!!!!"):
            with self.assertRaises((binascii.Error, ValueError)):
                if not invalid:
                    raise ValueError("empty chunks are invalid")
                base64.b64decode(invalid, validate=True)

    def test_firmware_configuration_uses_required_uart_and_decoder(self):
        protocol = (ROOT / "common" / "optical_protocol.h").read_text()
        sender = (ROOT / "sender" / "sender.ino").read_text()
        receiver = (ROOT / "receiver" / "receiver.ino").read_text()
        self.assertIn("{0x55, 0x55, 0x54, 0x48}", protocol)
        self.assertIn("#define OPTICAL_MAX_CHUNK 1024", protocol)
        self.assertIn("#define OPTICAL_MAX_PAYLOAD 2048", protocol)
        self.assertIn("#define BAUD_RATE 250000", sender)
        self.assertIn("#define BAUD_RATE 250000", receiver)
        self.assertIn("SERIAL_8N1, -1, LASER_PIN, true", sender)
        self.assertIn("SERIAL_8N1, RECEIVER_PIN, -1, true", receiver)
        self.assertIn("decodeBase64Strict", sender)

    def test_receiver_has_session_timeout_and_link_metrics(self):
        receiver = (ROOT / "receiver" / "receiver.ino").read_text()
        self.assertIn("#define IMAGE_SESSION_TIMEOUT_MS 3000", receiver)
        self.assertIn('failImage("session_timeout")', receiver)
        self.assertIn("imageLastActivityAt = imageStartedAt", receiver)
        self.assertIn("imageLastActivityAt = millis()", receiver)
        self.assertIn("LINK METRICS text_ok=", receiver)
        for field in (
            "text_failed=",
            "images_started=",
            "images_verified=",
            "images_failed=",
            "header_crc=",
            "payload_crc=",
            "resyncs=",
            "parser_timeouts=",
        ):
            self.assertIn(field, receiver)
        self.assertIn("if (!imageActive || imageFailed) return;", receiver)


if __name__ == "__main__":
    unittest.main()
