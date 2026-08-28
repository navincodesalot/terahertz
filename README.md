# Terahertz

A web-controlled free-space optical link using two ESP32-S3 boards and inverted hardware UART.

## Current milestone

The browser sends text or images through:

```text
Browser -> Next.js -> Upstash Redis Pub/Sub -> sender ESP32
        -> 250000 baud inverted UART -> laser -> receiver ESP32 -> USB serial
```

Receiver results are intentionally read from USB serial. Neither text nor images are returned to the browser yet. Receiver-to-browser integration is the final step after the optical link has repeatable reliability measurements.

## UART and optical protocol

- Baud: `250000`
- Format: `8N1`
- Sender TX: GPIO `4`
- Receiver RX: GPIO `5`
- UART inversion: enabled so the laser is off while idle
- Image chunk: `1024` bytes
- Maximum optical payload: `2048` bytes
- Image upload limit: `500 KB`

Each protocol-v3 frame contains:

```text
4-byte sync
version + type + sequence + length
payload CRC32
header CRC16
whitened payload
```

Header integrity is verified before payload length is trusted. The receiver parser is non-blocking and continues looking for frame sync while receiving payload data so a truncated frame can recover at the next frame.

## Firmware setup

### Sender

1. Copy `firmware/sender/secrets.h.example` to `firmware/sender/secrets.h`.
2. Set Wi-Fi and Upstash Redis TCP/TLS credentials.
3. Install the ESP32 Arduino core and the libraries providing `ArduinoJson.h` and `Redis.h`.
4. Flash `firmware/sender/sender.ino` to the sender ESP32-S3.
5. Open its USB serial monitor at `115200` baud.

Expected ready output includes:

```text
TERAHERTZ OPTICAL SENDER
Protocol v3 baud=250000 chunk=1024 payload=2048
Redis subscribed channel=laser_commands
```

### Receiver

1. Flash `firmware/receiver/receiver.ino` to the receiver ESP32-S3.
2. Open its USB serial monitor at `115200` baud.

Expected ready output includes:

```text
TERAHERTZ OPTICAL RECEIVER
Protocol v3 baud=250000 chunk=1024 payload=2048 timeout_ms=50
```

Successful traffic is reported as:

```text
TEXT OK sequence=... bytes=... crc=ok data=...
IMAGE VERIFIED id=... bytes=... chunks=... sha256=ok time_ms=... rate_bps=...
LINK METRICS text_ok=... text_failed=... images_started=... images_verified=...
```

Failures report a concise reason such as `header_crc`, `payload_crc`, `sequence`, `size`, `sha256`, `unexpected_sync`, `timeout`, or `session_timeout`.

## Website setup

From `ui/`:

```sh
pnpm install
pnpm dev
```

Configure these server-side environment variables locally and in Vercel:

```text
UPSTASH_REDIS_REST_URL
UPSTASH_REDIS_REST_TOKEN
```

The website records command publication only. A `notified` status means Redis reported a connected subscriber for every command; it does not mean the optical receiver verified the transmission.

## Software validation

Run protocol and recovery tests from the repository root:

```sh
python -m unittest discover -s firmware/tests -v
```

Run web checks from `ui/`:

```sh
pnpm check
pnpm build
```

## Hardware validation order

1. Confirm the original text behavior over a direct sender-TX to receiver-RX wire.
2. Run the framed text path over direct UART at `250000` baud.
3. Repeat over a short, aligned optical link.
4. Send small images, then medium images, then near-limit images.
5. Record sender and receiver serial logs.
6. Require no parser cascades and clean recovery after an intentionally damaged frame.
7. Target at least 1000 measured text frames and 20 consecutive SHA-verified image transfers before adding receiver networking.

At `250000` baud with `8N1`, the gross wire rate is approximately `25 KB/s`. A 500 KB image therefore needs roughly 20.5 seconds plus framing and software overhead. The web API should return sooner because it publishes commands rather than waiting for optical completion.

## Reference

- `SIMPLIFICATION_PLAN.md` contains the comparison, design rationale, test matrix, and deferred final integration plan.
- `old code/` is reference material and is not part of the active implementation.
