# Terahertz Image-Transfer Simplification Plan

> Planning document only. This does not authorize or contain implementation changes.

## 1. Goal

Return the project to the simple idea that already worked:

```text
bytes -> inverted ESP32 hardware UART -> laser -> receiver UART -> bytes
```

Then add only the minimum structure required to move binary images reliably:

- one fixed UART configuration
- clear image boundaries
- bounded chunks
- reliable frame resynchronization
- binary line whitening
- corruption detection
- final whole-image verification

The first success target is deliberately local and measurable:

> Send text and images through the optical UART link at 250000 baud, observe all receiver results in the USB serial monitor, and establish repeatable reliability metrics. For images, the receiver must verify that the received SHA-256 equals the source SHA-256.

There will be **no receiver-to-browser path for either text or images during this milestone**. The receiver remains a serial-output device. Browser delivery, receiver Wi-Fi, `/api/receive`, and received-image storage are the final integration step only after the optical link is demonstrably reliable.

## 2. Ground truth from the old and current code

### 2.1 What the original text test proves

The supplied text-only sketches establish a valuable physical baseline:

- `Serial1` hardware UART works at `250000` baud.
- `SERIAL_8N1` works.
- GPIO 4 works for laser TX.
- GPIO 5 works for optical RX.
- `invert=true` correctly keeps the laser off while idle.
- Hardware UART is preferable to custom clocking/bit-banging.

The newline protocol does **not** prove that arbitrary image bytes will work. Newlines can occur inside binary data, text has a much friendlier bit distribution, and there is no corruption check.

### 2.2 Old firmware strengths

Files:

- `old code/firmware/common/optical_protocol.h`
- `old code/firmware/sender/sender.ino`
- `old code/firmware/receiver/receiver.ino`

Useful behavior to restore or preserve:

- `250000` baud on both devices.
- Simple `IMAGE_START`, ordered `IMAGE_CHUNK`, `IMAGE_END` lifecycle.
- One frame at a time with `Serial1.flush()` on the sender.
- 1 KB-class bounded chunks rather than buffering a complete image on the sender.
- CRC32 per frame.
- Sequence, declared byte count, declared chunk count, and final SHA-256 validation.

Unsafe behavior that must not return:

- `readOpticalFrame()` commits after one possible magic byte.
- `readExact()` blocks for up to two seconds using a length from an unverified header.
- A plausible corrupted length can consume following frames.
- The receiver cannot resynchronize while blocked in payload collection.
- Every image chunk is logged over the slower debug serial port.
- Raw image bytes are not whitened, so binary patterns can disturb the optical comparator baseline.

### 2.3 Current firmware strengths

Files:

- `firmware/common/optical_protocol.h`
- `firmware/sender/sender.ino`
- `firmware/receiver/receiver.ino`

Improvements worth retaining:

- Payload whitening through `opticalScramble()`.
- One contiguous frame buffer and one `Serial1.write()` call.
- A non-blocking receiver state machine.
- A larger RX ring buffer.
- Reduced hot-path logging.
- Explicit image failure state and measured transfer summary.

Current problems:

1. **Baud mismatch**
   - Current sender and receiver firmware use `230400`.
   - Current `ui/src/lib/protocol.ts` advertises `250000`.
   - The system has no single source of truth.

2. **The current parser still has a cascade case**
   - The sliding window is used only while searching for a header.
   - Once `headerValid()` accepts a plausible length, every following byte is consumed as payload.
   - A corrupted length that remains at or below 2048 can therefore swallow later frame bytes.
   - The comment claiming the sliding window alone prevents this is incorrect.

3. **The header is not protected**
   - Version and type have basic validation.
   - Sequence, length, and expected payload CRC can be corrupted without an independent header-integrity check.

4. **The two-second deadline is not the right model**
   - A UART frame is contiguous and a full 1 KB frame takes about 41 ms at 250000 baud.
   - A short inter-byte inactivity timeout is more meaningful than a long total-frame timeout.
   - The parser must remain non-blocking regardless of the timeout value.

5. **The current zero-length path bypasses payload CRC validation**
   - `beginPayload()` directly dispatches a zero-length frame.
   - Empty frames must either be explicitly valid and fully checked or rejected.

6. **The receiver verifies but does not retain an image**
   - Chunks are hashed and then discarded.
   - Optional Base64 debug printing is not image reassembly or API delivery.

### 2.4 Current API and UI findings

Files:

- `ui/src/lib/protocol.ts`
- `ui/src/app/api/send/route.ts`
- `ui/src/app/api/receive/route.ts`
- `ui/src/app/api/history/route.ts`
- `ui/src/app/api/health/route.ts`
- `ui/src/app/page.tsx`

What works:

- The browser can upload a bounded image in one request.
- `/api/send` splits it into bounded 1024-byte chunks.
- Source SHA-256 is calculated server-side.
- A record is stored before Redis Pub/Sub publication.
- The route no longer sleeps for every chunk, avoiding an artificial 20+ second server-function delay.
- History is bounded to 50 records.

What is inaccurate or incomplete:

- The web contract says `250000`, but the running firmware source says `230400`.
- The API supports 32 KB text while the current optical payload limit is 2 KB.
- A successful Redis `PUBLISH` is not proof of optical delivery.
- Upstash `PUBLISH` returns the listener count, but the current status model does not use it.
- Pub/Sub is at-most-once; commands published while the sender is disconnected are lost.
- Publishing hundreds of chunks quickly can queue roughly 680–750 KB of Base64/JSON data in network/server buffers for a 500 KB image. This must be measured, not assumed safe from `Serial1.flush()` alone.
- `/api/receive` accepts telemetry JSON but the receiver firmware never calls it.
- `/api/receive` cannot accept image bytes and cannot independently verify a received image.
- The page fetches history only once and never observes a later receiver update.
- The receive panel displays the newest submitted record, not the newest physically received image.
- “Cloud command link online” is hardcoded and only Redis health could currently be checked anyway.
- Fake progress values (`30`, `65`, `100`) do not represent measured optical progress.
- A published image is described using “received” language before receiver confirmation.

## 3. Complexity budget

### Keep: this complexity is required by binary optical transfer

1. Inverted hardware UART.
2. A short frame sync marker.
3. Frame type, sequence, and payload length.
4. Header integrity before trusting payload length.
5. Payload whitening for image bit balance.
6. CRC32 per payload.
7. 1024-byte chunks.
8. Start/chunk/end image session state.
9. Final byte-count, chunk-count, and SHA-256 verification.
10. Bounded buffers and sparse logging.

### Remove from the core implementation

1. 32 KB text support that firmware cannot transmit.
2. Fake transmission-stage progress.
3. Fake “online” state.
4. Unmeasured bit-error/retry/checksum telemetry fields.
5. Receiver API integration until local optical verification works repeatedly.
6. Browser realtime infrastructure.
7. Retry protocols requiring a reverse optical link.
8. Object storage, Redis Streams, and pending-job recovery in the first pass.
9. Any physical-layer redesign before the 250000-baud baseline is retested.

### Retain as a validation tool

Keep a small text path for `HELLO WORLD`, repeated counters, and fixed-pattern diagnostics. Text is useful for proving the basic UART/optical link before image testing. It may remain available in the send UI during validation, but it must not create a fake browser-side receive path or complicate the image protocol.

## 4. Proposed minimum architecture

```mermaid
flowchart TD
    UI[Browser transmit controls] -->|text or image POST| Send[/api/send]
    Send -->|save small submission record| History[Redis history]
    Send -->|text or image commands| PubSub[Redis Pub/Sub]
    PubSub --> Sender[ESP32 sender]
    Sender -->|250000 baud inverted UART| Laser[Optical link]
    Laser --> Receiver[ESP32 receiver]
    Receiver -->|all text/image results| USB[USB serial]
    Receiver -. no API or browser return yet .-> Stop[Serial reliability milestone]
```

This is the first milestone. It uses existing infrastructure and does not add storage services or a receiver network stack.

### Honest status model for this milestone

```text
publishing -> notified -> failed
```

Definitions:

- `publishing`: the API is validating/storing/publishing commands.
- `notified`: Redis reported at least one subscriber for the image commands. This means a subscriber was connected, not that the optical transfer succeeded.
- `failed`: validation, storage, or publication failed.

Do not expose `success`, `received`, or `verified` in the website until receiver results return to the server in the final integration phase.

The receiver’s USB output is the only source of optical truth during validation:

```text
TEXT OK sequence=... bytes=... crc=ok
TEXT FAILED reason=...
IMAGE VERIFIED id=... bytes=... chunks=... sha256=ok time_ms=...
IMAGE FAILED id=... reason=header_crc|payload_crc|sequence|size|sha256|timeout
```

These concise, machine-readable summaries are also how reliability will be measured before any receiver-to-browser work begins.

## 5. Proposed optical protocol

### 5.1 Wire format

Use one small versioned binary frame:

```text
[sync 4 bytes]
[version u8]
[type u8]
[sequence u16]
[length u16]
[payload CRC32 u32]
[header CRC16 u16]
[whitened payload: length bytes]
```

Recommended frame types:

```text
IMAGE_START
IMAGE_CHUNK
IMAGE_END
TEST (optional bench-only)
```

### 5.2 Why each field exists

- Four-byte sync: makes accidental in-payload sync extremely unlikely and provides a strong resynchronization target.
- Version: sender/receiver compatibility.
- Type: session dispatch.
- Sequence: missing/reordered chunk detection.
- Length: bounded payload collection.
- Header CRC16: length is not trusted until the complete header is validated.
- Payload CRC32: detects payload corruption after descrambling.
- Whitening: avoids long optical duty-cycle bias from image bytes.

### 5.3 Resynchronization rule

The sync detector must remain active for the entire byte stream, including while collecting a payload.

If a new full sync marker is detected before the current payload completes:

1. fail the current frame/session,
2. treat the marker as the beginning of the next frame,
3. continue without waiting for the corrupted length.

This rule addresses the important dropped-byte case. A header CRC alone prevents most false lengths, but continuous sync detection also allows the immediately following frame to interrupt a damaged/truncated frame.

The exact sync bytes should be fixed in the implementation and covered by test vectors. A short alternating prefix followed by the existing `TH` magic is a reasonable starting candidate because it also exercises the optical front end after idle. It must be validated on the actual comparator output rather than justified only in comments.

### 5.4 Parser states

Keep the receiver parser to three understandable states:

```text
SEARCH_SYNC -> READ_HEADER -> READ_PAYLOAD -> SEARCH_SYNC
```

Rules:

- Never call a blocking `readExact()`.
- Never trust `length` before header CRC16 passes.
- Reject invalid version, type, zero length where disallowed, and lengths above the type-specific maximum.
- Refresh a short inactivity deadline whenever a header/payload byte arrives.
- On timeout, CRC error, or unexpected sync, reset directly to sync/header collection.
- After an image fails, ignore its remaining chunks until `IMAGE_END` or allow a new `IMAGE_START` to replace it cleanly.

### 5.5 Image session metadata

Do not send the complete cloud command JSON blindly as optical metadata.

The optical start metadata needs only:

- image/transfer ID
- total bytes
- chunk count
- MIME type or compact format identifier
- expected SHA-256

`IMAGE_END` can remain a small explicit terminator. Avoid duplicate fields unless they are used as a consistency check.

For the first implementation, compact JSON is acceptable because ArduinoJson is already required for Redis commands. A fixed binary metadata struct can be considered only after the simple version works and measurements show JSON parsing is a real problem.

## 6. Firmware simplification plan

### Phase F0 — Preserve and establish the baseline

- Do not overwrite the current uncommitted protocol/receiver changes without reviewing them.
- Save the existing work as a comparison point.
- Restore `250000` consistently in sender and receiver.
- Confirm pins, inversion, idle-laser behavior, and `8N1` with the supplied text sketch behavior.
- Test direct wired UART before optical hardware.

**Gate:** 1000 repeated text/fixed-pattern messages pass over direct wire and then the short optical link at 250000 baud.

### Phase F1 — Replace only the frame layer

Update `firmware/common/optical_protocol.h` to contain:

- one protocol version,
- one max chunk/payload value,
- the frame layout,
- sync constants,
- CRC16 for the header,
- CRC32 for payload,
- whitening,
- one contiguous frame builder.

Remove stale comments, obsolete protocol variants, and unsupported claims.

**Gate:** deterministic test vectors produce identical frame bytes on sender-side and host-side calculations.

### Phase F2 — Simplify the sender hot path

Keep Redis/Wi-Fi connection handling, but reduce image dispatch to:

1. validate one cloud command,
2. decode one 1024-byte Base64 chunk,
3. build one optical frame,
4. write once,
5. flush once,
6. return to Redis processing.

Additional rules:

- Strictly reject malformed Base64 rather than silently skipping arbitrary characters.
- Keep only one chunk buffer and one frame buffer.
- Log image start/end and every 32 chunks, not every chunk.
- Check `Serial1.write()` result.
- Do not claim that flush alone proves end-to-end cloud pacing.

**Gate:** sender memory remains bounded and a 500 KB command stream does not reset, disconnect, or reject chunks.

### Phase F3 — Implement the robust non-blocking receiver

Replace both the old blocking parser and the current incomplete cascade fix with the state model in section 5.

Keep:

- RX ring buffer,
- sequence validation,
- byte/chunk totals,
- SHA-256,
- sparse diagnostics.

Remove:

- heartbeat/log state that does not help isolate a failure,
- duplicated counters,
- zero-length special cases that bypass integrity checks,
- long blocking-style timeouts.

Diagnostics should report one concise terminal reason and a final summary.

**Gate:** every injected header corruption, plausible bad length, payload corruption, missing byte, and missing chunk fails the current image and reacquires the next valid frame/image without an extended cascade.

### Phase F4 — Decide receiver image handling

For initial proof, choose one mode explicitly:

- **Verification mode:** stream chunks into SHA-256 and report success over USB without retaining the image.
- **Reconstruction mode:** retain the image in PSRAM, SD, or filesystem and make the exact bytes available after verification.

Verification mode proves transport integrity with the least code. Reconstruction mode is required before a received image can be previewed or uploaded elsewhere.

Do not assume a 500 KB buffer fits. Confirm the exact receiver board, PSRAM availability, free heap, and TLS memory needs first.

### Phase F5 — Serial reliability gate

Use the receiver USB output to calculate real reliability before any receiver-to-browser work.

Record at minimum:

- attempted text frames,
- valid text frames,
- header CRC failures,
- payload CRC failures,
- parser timeouts/resynchronizations,
- attempted images,
- SHA-verified images,
- image sequence/size/hash failures,
- ESP32 resets or RX overflows,
- bytes and elapsed milliseconds per image.

Initial pass criteria:

1. 1000 repeated text frames at 250000 baud with a documented success percentage and no parser cascade.
2. A representative image set containing small, medium, and near-limit PNG/JPEG files.
3. At least 20 consecutive complete image transfers with matching SHA-256 before calling the image path stable.
4. An intentionally corrupted/truncated frame fails cleanly and the next valid transmission succeeds.
5. No unexplained ESP32 reset, Redis disconnect, UART overflow, or multi-frame parser outage.
6. Measured throughput is reasonably close to the expected UART wire rate after framing overhead.

If the target is not met, preserve the logs and change one variable at a time: direct wire versus optical, baud, whitening, sync/header handling, chunk size, then physical alignment/front-end conditions. Do not add receiver networking while this gate is failing.

## 7. API simplification plan

### Phase A0 — Make the contract match firmware

In `ui/src/lib/protocol.ts`:

- set one `UART_BAUD = 250000`,
- set one `IMAGE_CHUNK_BYTES = 1024`,
- keep text support small and bounded if it is useful for serial validation,
- make images the primary product path,
- define only statuses the backend can prove,
- keep the 500 KB limit unless receiver storage testing requires a lower limit.

### Phase A1 — Keep one image send route

`POST /api/send` remains a multipart image upload because it is simple and 500 KB is comfortably below Vercel’s documented 4.5 MB request-body limit.

The route should:

1. validate that a file exists,
2. enforce size,
3. validate supported image signatures rather than trusting only browser MIME,
4. compute byte count and SHA-256,
5. store a small transmission record,
6. publish `IMAGE_START`, ordered 1024-byte chunks, and `IMAGE_END`,
7. inspect Redis `PUBLISH` listener counts,
8. return quickly without sleeping for optical wire time.

Do not publish the complete image as one ESP32 JSON value.

### Phase A2 — Treat Pub/Sub honestly

For the first pass:

- Pub/Sub remains a live transport, not a durable queue.
- If no subscriber is present, return/store a clear `sender_offline` or `not_notified` result.
- If the subscriber disconnects mid-burst, fail publication when later listener counts become zero if the API can observe it.
- Keep batches bounded, but do not add arbitrary delays in the Vercel request.
- Measure whether a 500 KB burst remains connected and ordered on the actual ESP32 Redis client.

If burst testing fails, do **not** immediately add random sleep values. The next options, in order, are:

1. reduce publish batch size,
2. lower maximum image size to the measured safe limit,
3. add a sender pull/download design using durable object storage,
4. use a durable queue/stream.

Options 3 and 4 are intentionally deferred because they add infrastructure and are unnecessary if the existing bounded burst works reliably.

### Phase A3 — Keep history small and truthful

`GET /api/history` may remain for recent submissions.

Stored records should contain metadata, never a Base64 image:

- ID
- filename
- MIME
- source bytes
- source SHA-256
- publish/notified status
- created/updated timestamps
- concrete error reason, if any

`DELETE /api/history` can remain for a local demo, but it should not be presented as production-safe without authentication.

### Phase A4 — Defer `/api/receive` for both text and images

The current route creates the appearance of telemetry without a receiver integration. During the serial-validation milestone:

- do not post received text to it,
- do not post image results or bytes to it,
- do not wire the browser to it,
- either deactivate it or leave it unused without claiming it is live.

Do not expand this route until text and image reliability have both been measured through receiver USB serial and the project is ready for the final receiver-to-browser integration step.

## 8. UI plan — preserve the current design

The current UI is visually good enough and is not a blocker. Do not redesign it, replace its layout, or make the project image-only before the protocol is reliable.

Text and image sending can both remain in the existing dashboard. During this milestone, both are transmit controls whose real receive results are read from the receiver USB serial monitor.

Only make small UI changes when they correct a real mismatch discovered during implementation:

- display `250000` everywhere once sender and receiver use it,
- make the text byte limit match the firmware limit,
- avoid calling a Redis-published command physically received or verified,
- avoid presenting Redis/API health as proof that the sender, receiver, or optical path is online,
- avoid numeric progress percentages unless they come from measured firmware progress,
- label history as submitted/published transmissions rather than received output,
- keep the current responsive layout, styling, shadcn components, and overall visual direction.

The existing receive panel does not need to be rebuilt now. If touched, its copy should simply state that receiver results are currently available through USB serial and that browser receive integration is deferred.

Refactoring the large page into smaller components is optional cleanup, not part of the protocol work. Do it only if a required UI correction would otherwise be difficult to maintain.

## 9. Final integration milestone: receiver to browser

This milestone applies to **both text and images** and starts only after the serial reliability gate passes. It is not part of the minimum reliable optical-transfer work.

It requires all of the following:

1. A reliable receiver Wi-Fi/TLS path that does not starve UART reception.
2. Persisting the active transfer ID on the receiver.
3. Posting verified text results to an authenticated receiver route.
4. Receiver image storage in PSRAM, SD, or filesystem.
5. Uploading verified image bytes through the authenticated receiver route.
6. Server-side recomputation of received size and SHA-256.
7. Object/blob storage for received image bytes.
8. Received text or image URLs/status in the small Redis history record.
9. UI polling or realtime updates until terminal status.

Only the server’s own comparison of uploaded received bytes against the source hash may set `verified`.

If this milestone is accepted later, a one-manifest/object-download sender architecture should also be reevaluated. It may reduce Pub/Sub traffic, but it introduces object storage and ESP32 HTTPS streaming, so it should not replace a working simpler path without measured need.

## 10. Validation matrix

### 10.1 Direct UART tests

- Known text baseline at 250000, inverted `8N1`.
- Empty payload.
- All `0x00` payload.
- All `0xFF` payload.
- Alternating `0x55`/`0xAA`.
- Pseudorandom payload.
- 1 byte, 1023 bytes, 1024 bytes, and maximum allowed payload.

### 10.2 Parser fault-injection tests

For a valid frame followed immediately by another valid frame, inject:

- every single-bit header corruption,
- bad but in-range length,
- oversized length,
- corrupted header CRC,
- corrupted payload CRC field,
- corrupted payload byte,
- one missing payload byte,
- one inserted byte,
- truncated frame,
- unexpected new sync inside a damaged frame,
- missing image start,
- missing/duplicate/reordered chunk,
- missing image end.

Expected result:

- the damaged image/frame fails,
- the receiver returns to sync search,
- the next complete valid frame/image can succeed,
- no multi-second blocking or RX-ring overflow occurs.

### 10.3 Optical tests

1. Short aligned optical link in controlled light.
2. Compare raw and whitened `0x00`/`0xFF` waveforms at comparator output.
3. Confirm idle laser state.
4. Confirm frame-start behavior after long idle.
5. Repeated small PNG/JPEG files.
6. Repeated 500 KB worst-case transfer if receiver handling supports it.
7. Increasing distance and ambient light only after the short-link test passes.

### 10.4 Cloud burst tests

- Sender online before upload.
- Sender offline before upload.
- Sender disconnects after start.
- Sender disconnects mid-image.
- Wi-Fi reconnect after a failed image.
- 1 KB, 100 KB, and 500 KB uploads.
- Verify command order and exact decoded byte totals.
- Monitor ESP32 heap, Redis connection stability, resets, and rejected chunks.

### 10.5 Timing expectations

At 250000 baud with 8N1, gross payload capacity is about 25000 wire bytes/s.

A 1024-byte payload plus a small frame header takes approximately 41–42 ms on the wire. A 500 KB image therefore has a physical minimum near 20.5 seconds before software and metadata overhead.

This duration is normal. The API should finish much sooner because it publishes commands rather than waiting for the laser transfer.

## 11. Implementation order and stop/go gates

1. **Freeze scope**
   - Receiver output remains USB serial for both text and images.
   - Core result is repeatable text delivery plus local SHA-verified image transfer.
   - All receiver-to-browser work is the final step.

2. **Reconfirm bare 250000-baud link**
   - Stop if the supplied simple baseline no longer works.

3. **Implement/test the frame protocol over direct wire**
   - Stop if fault injection causes cascades.

4. **Test binary whitening and frames over the optical link**
   - Stop and inspect waveforms before touching cloud code if errors remain.

5. **Align and simplify the API contract**
   - Test one image through Redis into sender USB logs before enabling the laser.

6. **Run full cloud-to-optical-to-receiver verification**
   - Start small, then increase image size.

7. **Record and evaluate the serial reliability metrics**
   - Do not proceed if text or image results are still intermittent or unexplained.

8. **Apply only necessary UI corrections**
   - Preserve the current design and text/image controls.
   - Correct baud, limits, labels, and status claims only where needed.
   - Keep all receiver output in the serial monitor.

9. **Implement receiver-to-browser as the final step**
   - Add the text result POST, image storage/upload, and browser receive display only after the core demo is repeatably reliable.

## 12. Files expected to change during implementation

Primary scope:

- `firmware/common/optical_protocol.h`
- `firmware/sender/sender.ino`
- `firmware/receiver/receiver.ino`
- `ui/src/lib/protocol.ts`
- `ui/src/app/api/send/route.ts`
- `ui/src/app/page.tsx`

Likely small supporting scope:

- `ui/src/app/api/history/route.ts`
- `ui/src/app/api/health/route.ts`
- selected existing `ui/src/components/ui/*` components only if needed
- focused test/vector files added during implementation

Deferred or removable from the active flow:

- `ui/src/app/api/receive/route.ts`

No changes should be made inside `old code/`; it is a reference snapshot.

## 13. Decisions to confirm before implementation

The recommended defaults are in bold.

1. Core completion target:
   - **receiver text output and image SHA verification over USB serial**.
   - Receiver-to-browser delivery is not included until the final step.

2. Product mode during validation:
   - **preserve the current text and image send UI**.
   - Text remains useful as a baseline/reliability diagnostic.
   - Neither mode has a browser receive path yet.

3. Receiver storage:
   - **verification-only first**.
   - PSRAM/SD reconstruction is deferred until image upload to the browser is being implemented.

4. Maximum image size:
   - **retain 500 KB and lower it only if measured hardware limits require it**, or
   - intentionally choose a smaller demo limit.

Unless these defaults are changed, implementation should follow the bold options.

## 14. External references checked

- Espressif Arduino UART documentation confirms configurable baud, inversion-capable hardware UART usage, and configurable RX/TX buffers: <https://docs.espressif.com/projects/arduino-esp32/en/latest/api/serial.html>
- Upstash `PUBLISH` returns the number of clients that received the message: <https://upstash.com/docs/redis/sdks/ts/commands/pubsub/publish>
- Upstash REST follows Redis command semantics: <https://upstash.com/docs/redis/features/restapi>
- Vercel documents a 4.5 MB function request/response payload limit; the current 500 KB upload is below it: <https://vercel.com/docs/errors/function_payload_too_large>
- Vercel function limits and duration configuration should be rechecked against the deployment plan before implementation: <https://vercel.com/docs/functions/limitations>
