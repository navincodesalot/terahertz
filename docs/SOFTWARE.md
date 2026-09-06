# Software Architecture

This document covers each software component in depth — enough to understand, extend, or debug the system. For hardware specifics see [`./hardware_layout.md`](./HARDWARE.md).

---

## 1. Cloud Pipeline

```
┌─────────────────────────────────────────────────────────────────┐
│                       Browser Dashboard                         │
│   (Next.js client, polls /api/history every 5 s)                │
└───────────────────────────┬─────────────────────────────────────┘
                            │ POST /api/send
                            │ JSON: { type, payload, mimeType? }
                            ▼
┌─────────────────────────────────────────────────────────────────┐
│                   Next.js API — /api/send                       │
│   • Validates with Zod                                          │
│   • Generates UUID, writes MessageRecord to Redis               │
│   • For images: PUBLISHes image_start alone (+600 ms lead),     │
│     then chunks 4-at-a-time with 30 ms gaps                     │
│   • Stores original image bytes under image:<id> (24 h TTL)     │
└───────────────────────────┬─────────────────────────────────────┘
                            │ PUBLISH laser_commands
                            │ JSON command objects
                            ▼
┌─────────────────────────────────────────────────────────────────┐
│                   Upstash Redis (Pub/Sub)                       │
│   Channel: laser_commands                                       │
└───────────────────────────┬─────────────────────────────────────┘
                            │ TLS WebSocket (ArduinoRedis)
                            ▼
┌─────────────────────────────────────────────────────────────────┐
│              ESP32-S3 Sender (firmware/sender/)                 │
│   • Persistent subscriber, owns the Arduino loop               │
│   • Deserialises JSON command, formats optical record           │
│   • Serial1.println() → UART TX (GPIO4, 250 kbaud, inverted)   │
└───────────────────────────┬─────────────────────────────────────┘
                            │ 650 nm laser, free-space optical
                            ▼
┌─────────────────────────────────────────────────────────────────┐
│              ESP32 Receiver (firmware/receiver/)                │
│   • BPW34 → TIA → comparator → GPIO5 UART RX                   │
│   • Non-blocking accumulator, dispatches on \n                  │
│   • CRC-32 per line, SHA-256 over full image                    │
└───────────────────────────┬─────────────────────────────────────┘
                            │ POST /api/receive (HTTPS, setInsecure)
                            │ JSON telemetry + base64 image
                            ▼
┌─────────────────────────────────────────────────────────────────┐
│                 Next.js API — /api/receive                      │
│   • Merges telemetry into existing Redis MessageRecord          │
│   • Sets status → success / failed                              │
└─────────────────────────────────────────────────────────────────┘
                            ▲
                            │ GET /api/history (every 5 s)
                    Browser Dashboard
```

---

## 2. Sender Firmware (`firmware/sender/sender.ino`)

### Setup

1. Connect to WiFi (blocking retry loop).
2. Establish TLS connection to Upstash Redis endpoint (ArduinoRedis over `WiFiClientSecure`).
3. Authenticate with `AUTH <password>`.
4. `SUBSCRIBE laser_commands`.
5. Call `startSubscribing(messageCallback)` — this hands control of the loop to the library.

### `messageCallback`

Called by ArduinoRedis for each incoming message. Because `startSubscribing()` owns the Arduino `loop()`, all logic is synchronous inside the callback. It directly calls `handleCommand(message)` — no queue is needed.

### `handleCommand`

Deserialises the JSON payload with ArduinoJson v7. Dispatches on the `type` field:

| `type` field | Action |
|---|---|
| `text` | calls `sendOpticalText` |
| `image_start` | calls `sendImageCommand` |
| `image_chunk` | calls `sendImageCommand` |
| `image_end` | calls `sendImageCommand` |

### `sendOpticalText`

1. Validates payload length (rejects if over limit).
2. Computes CRC-32 (Ethernet polynomial) over the payload string.
3. Builds: `THZTXT|<msg_id>|<crc32hex>|<payload>`
4. `Serial1.println(line)` — appends `\r\n`; receiver strips `\r`.

### `sendImageCommand`

- **`image_start`**: sends a blank line (`Serial1.println("")`) + 20 ms delay to absorb optical startup transients, then emits `THZIMG|S|<id>|<chunkCount>|<totalBytes>|<mimeType>`.
- **`image_chunk`**: computes CRC-32 over the base64 data string, emits `THZIMG|C|<id>|<index>|<base64data>|<crc32hex>`. A 5 ms delay after each chunk prevents UART RX buffer overflow on the receiver.
- **`image_end`**: emits `THZIMG|E|<id>|<sha256hex>` where the SHA-256 was computed server-side over the original bytes and passed through in the Redis command.

### Reconnect Logic

After `startSubscribing()` returns (connection dropped):

1. Wait 5 s.
2. If WiFi is down, reconnect WiFi first.
3. Re-establish Redis TLS connection and re-subscribe.

### UART Configuration

```cpp
Serial1.begin(250000, SERIAL_8N1, -1, LASER_PIN, true);
//                                        ^           ^
//                                        RX unused   invert=true
```

`invert=true` means the TX line idles LOW (laser OFF) and pulses HIGH (laser ON) for data bits. This is intentional — the laser is off between transmissions.

---

## 3. Receiver Firmware (`firmware/receiver/receiver.ino`)

### Initialisation

```cpp
Serial1.setRxBufferSize(2048);          // must come before begin()
Serial1.begin(250000, SERIAL_8N1, RX_PIN, -1, true);
```

The 2048-byte buffer is critical. A single base64-encoded 1024-byte chunk expands to ≈ 1368 characters, plus protocol framing. The default 256-byte buffer overflows on long chunk lines.

### Main Loop

Non-blocking: reads one byte per `loop()` iteration and appends it to a static `String line`. On `\n`, calls `handleLine(line)` and clears the buffer. This avoids blocking reads that would stall other housekeeping.

### `handleLine`

Strips trailing `\r`. Checks the first 6 characters of the prefix:

```
THZTXT → handleTextLine
THZIMG → handleImageLine
```

**Startup glitch tolerance:** byte 2 of the prefix can arrive corrupted (e.g. `TH^TXT`, `TH^IMG`) due to optical transients at link establishment. The handler normalises byte 2 to `Z` before comparing, so a single-character corruption does not drop the record.

### `handleTextLine`

1. Splits on the first 3 pipe characters — the 4th field is the raw payload (which may itself contain `|`).
2. Recomputes CRC-32 over the payload; compares to the received hex.
3. Calls `postTelemetry` with pass/fail result.

### `handleImageLine`

Dispatches on sub-type (field 1):

**`S` (start)**
- Parses chunk count, total byte count, MIME type.
- Validates total ≤ 48 KB (heap safety limit).
- Allocates `uint8_t* imageBuffer` on the heap.
- Initialises SHA-256 context (`mbedtls_sha256_starts`).
- Resets chunk counters.

**`C` (chunk)**
- Splits off the trailing CRC field.
- Recomputes CRC-32 over the base64 string; records pass/fail.
- Base64-decodes the chunk into a temporary `chunkBuf`.
- `memcpy` into `imageBuffer` at the correct offset.
- Feeds decoded bytes into the SHA-256 context (`mbedtls_sha256_update`).

**`E` (end)**
- Finalises SHA-256 (`mbedtls_sha256_finish`), encodes as lowercase hex.
- Compares to the sender's hash from the `E` record.
- Re-encodes `imageBuffer` to base64 for telemetry.
- Builds JSON telemetry payload (see §4).
- Calls `postTelemetry`.
- `free(imageBuffer)`.

### `postTelemetry`

```cpp
WiFiClientSecure client;
client.setInsecure();           // no CA bundle on the device
HTTPClient http;
http.begin(client, VERCEL_URL "/api/receive");
http.addHeader("Content-Type", "application/json");
int code = http.POST(jsonBody);
```

Blocking call. This is acceptable because the sender stops transmitting after each complete message (text or image), so there is no ongoing UART stream to miss.

---

## 4. Next.js API Routes

All routes live under `ui/src/app/api/`. Validated with Zod at the boundary.

### `POST /api/send`

Request body (text):
```json
{ "type": "text", "payload": "hello world" }
```

Request body (image):
```json
{ "type": "image", "mimeType": "image/jpeg", "data": "<base64>" }
```

Processing:
1. Zod validates the union type.
2. Generates a UUID (`crypto.randomUUID()`).
3. Writes a `MessageRecord` to Redis: `SET message:<id> <json>`.
4. Pushes the ID onto the sorted set `transmission_history` (score = `Date.now()`).
5. For **text**: publishes one `{ type: "text", msgId, payload }` command.
6. For **image**:
   - Decodes base64, splits into 1024-byte raw chunks.
   - Publishes `image_start` command.
   - Waits 600 ms (absorbs optical warmup on the sender side).
   - Publishes chunks 4-at-a-time with `await sleep(30)` between batches.
   - Publishes `image_end` with the SHA-256 of the original bytes.
   - Stores original image under `image:<id>` with 24 h TTL.

### `POST /api/receive`

Accepts telemetry JSON from the receiver ESP32:

```json
{
  "msgId": "...",
  "type": "text" | "image",
  "crcPassed": true,
  "sha256Passed": true,
  "receivedPayload": "...",
  "receivedImageBase64": "...",
  "bytesReceived": 4096,
  "chunksReceived": 4,
  "transmissionMs": 320
}
```

Merges fields into the existing `message:<id>` Redis record and sets `status` to `"success"` or `"failed"`.

### `GET /api/history`

- Fetches last 50 IDs from `transmission_history` (sorted set, newest-first via `ZREVRANGE`).
- Fetches each `message:<id>`.
- Strips `receivedImageBase64` from all records except the most recent to keep the response payload small.

### `GET /api/image/[id]`

Returns the original uploaded image from `image:<id>` (stored base64 + MIME type). Used by the dashboard for the sent-vs-received side-by-side comparison.

### `GET /api/health`

Pings Redis with a `PING` command and returns `{ ok: true }` or a 500 error. Used for deployment health checks.

---

## 5. Optical Protocol Detail

### Wire Format

All records are ASCII, newline-terminated (`\n`). Fields are separated by `|`. The newline delimiter was chosen because:

- It's a single byte, easy to detect without buffering.
- It never appears in base64 or CRC hex strings, so no escaping is needed.
- The ESP32 `String` accumulator pattern (`readBytesUntil('\n')` or manual byte loop) is straightforward.

### CRC-32

Uses the standard Ethernet / ISO 3309 polynomial (`0xEDB88320` reflected). Computed over:
- **Text records**: the raw payload string (everything after the 3rd `|`).
- **Image chunk records**: the base64 data string (everything between the 4th and 5th `|`).

CRC is transmitted as 8 lowercase hex characters.

### Why Base64 for Image Chunks

Raw binary bytes would contain `\n` (0x0A), `\r` (0x0D), `|`, and null bytes — all of which conflict with the framing or field-split logic. Base64 produces printable ASCII only (`[A-Za-z0-9+/=]`), so the newline framing and pipe-split parsing are unambiguous.

### Startup Glitch Mitigation

When the laser first fires, the receiver's comparator may see a brief spurious edge before the optical path stabilises. Two mitigations:

1. **Sender**: sends a blank `\n` line before `image_start` (and waits 20 ms), so any glitch corrupts only the blank line.
2. **Receiver**: if byte 2 of the prefix is not `Z` (e.g. `TH^IMG`), it is normalised to `Z` before prefix matching. A single-byte corruption therefore still results in correct dispatch.

### Inter-Chunk Delay

The sender waits 5 ms after each `image_chunk` transmission. Without this delay, the sender's UART TX bursts chunk lines faster than the receiver can process them (CRC compute + heap copy + SHA-256 update), and the receiver's 2048-byte UART RX buffer overflows.

---

## 6. Redis Data Model

### `message:<id>` — MessageRecord

```typescript
{
  id: string,
  type: "text" | "image",
  status: "pending" | "success" | "failed",
  sentAt: number,            // Unix ms
  payload?: string,          // text only
  mimeType?: string,         // image only
  chunkCount?: number,
  totalBytes?: number,
  // — filled in by /api/receive —
  crcPassed?: boolean,
  sha256Passed?: boolean,
  receivedPayload?: string,
  receivedImageBase64?: string,
  bytesReceived?: number,
  chunksReceived?: number,
  transmissionMs?: number
}
```

Stored as JSON string. No TTL (history is kept indefinitely, pruned to 50 on read).

### `transmission_history` — Sorted Set

Members: message IDs. Score: `Date.now()` at send time. `ZREVRANGE` with limit 50 gives newest-first history. New entries added with `ZADD`.

### `image:<id>` — StoredImage

```typescript
{
  mimeType: string,
  base64: string     // original uploaded image
}
```

24-hour TTL (`EX 86400`). Only needed for the `/api/image/[id]` comparison endpoint; kept separate from the MessageRecord to avoid bloating the history response.

---

## 7. Dashboard UI

The dashboard is a single React client component (`"use client"`) that polls `/api/history` every 5 seconds using `setInterval`.

### Send Panel

- **Type toggle**: text or image. Switching clears the current input.
- **Text mode**: `<textarea>` for the payload. A CRC-32 of the typed text is computed in the browser and shown in real time — this lets the user verify on-screen that the value the sender will embed matches what the receiver will check. Rejects payloads that are too long before submission.
- **Image mode**: file input (accepts `image/*`, max 250 KB). Shows a preview thumbnail. On submit, the file is read as base64 and sent to `/api/send`.
- While awaiting the receiver, the send card shows an inline "Awaiting receiver…" status badge. Sonner toasts report send success or failure.

### Receive Panel

Displays the most recent completed transmission (latest entry in history with status `success` or `failed`).

- **Text**: shows the received payload string alongside the original. CRC pass/fail badge.
- **Image**: two images side-by-side — original (fetched from `/api/image/[id]`) and reassembled (base64 embedded in the history record). SHA-256 pass/fail badge. Stats cards show transmission speed (bytes/sec), total duration (ms), chunk count, and integrity result.

### History Log

Table of all records in `transmission_history` (newest first, max 50). Columns: type icon, message ID (truncated), status badge, size, sent time, transmission duration. Clicking a row sets it as the active receive panel entry.
