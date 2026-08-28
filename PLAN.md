# Terahertz Implementation Plan

This is the working source of truth for the Phase 2 cloud integration. Update the checkboxes and decision notes as the system is implemented and verified.

## Constraints and decisions

- [ ] Use the existing Next.js 16 App Router application in `ui/`.
- [ ] Use Upstash Redis standard Pub/Sub for the ESP32 command channel.
- [ ] Use the `laser_commands` channel.
- [ ] Keep the ESP32-S3 sender subscribed persistently; do not use polling.
- [ ] Keep the current optical protocol and firmware files unchanged during the cloud/application phases.
- [ ] Keep the current optical baud at `250000` for now.
- [ ] Use environment variables for Upstash credentials; never commit secrets.
- [ ] Add firmware integration only after the cloud/application contract is working end-to-end.
- [ ] Add image transport only after text transport is stable.

## Target architecture

```text
Browser dashboard
    |
    | POST /api/send
    v
Next.js server route
    |
    | validate + create command + persist + publish
    v
Redis command/history data       Redis Pub/Sub: laser_commands
                                        |
                                        | persistent TLS RESP subscription
                                        v
                                  ESP32-S3 sender
                                        |
                                        | existing Serial1 optical packet format
                                        v
                                  laser / FSO link
                                        |
                                        v
                                  ESP32 receiver
                                        |
                                        | later POST /api/receive
                                        v
                                  Next.js telemetry route
                                        |
                                        v
                                  Redis + browser realtime updates
```

## Delivery strategy

Each phase below must leave the repository in a buildable state and have an explicit acceptance test. We will not build dashboard polish, image support, or firmware changes before the underlying command lifecycle is useful.

### Phase 0 — Application foundation

**Goal:** establish shared contracts and server-side infrastructure without involving hardware.

- [ ] Add the required Upstash Redis server dependency using pnpm.
- [ ] Add a server-only Redis client module in `ui/src/lib/redis.ts`.
- [ ] Define environment-variable validation for the Upstash REST URL/token without exposing values to client code.
- [ ] Define shared command types and validation in `ui/src/lib/protocol.ts`.
- [ ] Define the initial command schema:
  - `id`
  - `type: "text"`
  - `payload`
  - `timestamp`
- [ ] Define stable Redis key/channel constants in one place.
- [ ] Add a health/readiness endpoint that verifies application configuration without leaking credentials.

**Acceptance:** `pnpm typecheck` and `pnpm build` pass; invalid/missing configuration produces an actionable server error.

### Phase 1 — Real command creation and Redis persistence

**Goal:** make the application create a durable command record before attempting live delivery.

- [ ] Implement `POST /api/send`.
- [ ] Validate JSON request bodies with the shared protocol schema.
- [ ] Support arbitrary text, including empty/oversized/invalid input rejection with clear status codes.
- [ ] Generate a server-side message ID and timestamp.
- [ ] Persist the command in Redis before publishing.
- [ ] Store enough status metadata to distinguish `queued`, `published`, `delivered/unknown`, `success`, and `failed` as the system grows.
- [ ] Return the created command ID and publish result to the caller.
- [ ] Make the operation safe to retry at the API boundary where practical; do not claim optical delivery before receiver telemetry exists.

**Acceptance:** a request to `/api/send` returns a validated command, a Redis record exists, and the route has tests or a repeatable local verification procedure.

### Phase 2 — Live Pub/Sub delivery contract

**Goal:** prove that a published command is available immediately to a persistent subscriber, independently of optical transmission.

- [ ] Finalize the `laser_commands` message envelope and document it.
- [ ] Publish the complete structured JSON command, not a raw text payload.
- [ ] Add structured server logging for command ID, type, and publish result.
- [ ] Create a local command-publish test path through the real Next.js route.
- [ ] Define the ESP32 subscriber behavior contract:
  - connect Wi-Fi
  - connect to Upstash over TLS
  - authenticate
  - subscribe to `laser_commands`
  - parse RESP Pub/Sub messages
  - validate JSON
  - print received command ID/type/payload
  - reconnect and resubscribe after connection loss
- [ ] Do not modify firmware yet; this phase establishes the contract and server side only.

**Acceptance:** publishing `HELLO WORLD`, `TEST 123`, special characters, and long text through the API produces the exact structured messages expected by a persistent subscriber. Offline/disconnected Pub/Sub loss is documented as expected behavior.

### Phase 3 — Sender firmware adapter (deferred until now)

**Goal:** connect the proven cloud command stream to the existing sender without redesigning the optical protocol.

- [ ] Add Wi-Fi configuration through non-committed firmware configuration/secrets.
- [ ] Add a persistent Upstash TLS/RESP Pub/Sub client suitable for ESP32-S3.
- [ ] Add reconnect/backoff behavior and resubscription.
- [ ] Parse and validate the Phase 2 command schema within bounded memory.
- [ ] Initially print received commands to USB serial.
- [ ] Then dispatch `type: "text"` to a dedicated optical transmission function.
- [ ] Preserve the current inverted `Serial1`, GPIO 4, `SERIAL_8N1`, checksum, and `250000` baud settings.
- [ ] Avoid large `String` allocations and unbounded payload buffering on the ESP32.

**Acceptance:** the ESP32-S3 stays subscribed, receives repeated cloud commands without polling, reconnects after Wi-Fi/Redis interruption, and produces the same optical packet envelope as the current local sender behavior.

### Phase 4 — Receiver telemetry API

**Goal:** make optical delivery observable by the cloud without inventing measurements.

- [ ] Implement `POST /api/receive`.
- [ ] Define a telemetry schema beginning with `id`, `status`, `type`, and `payload`.
- [ ] Add optional measured fields only when the receiver can accurately provide them.
- [ ] Validate receiver reports and reject malformed/unknown messages.
- [ ] Persist completed transmission records in Redis.
- [ ] Correlate receiver telemetry with the original command ID.
- [ ] Define behavior for checksum failures, duplicate reports, unknown IDs, and late reports.

**Acceptance:** a valid receiver report updates one transmission record; invalid or unverifiable measurements are not stored as fabricated values.

### Phase 5 — Useful dashboard vertical slice

**Goal:** replace the starter page with a functional text-only control panel backed by real routes.

- [ ] Build the send panel for text commands.
- [ ] Build the receive panel for the latest received message/status.
- [ ] Build a transmission history list from Redis.
- [ ] Add clear loading, disabled, success, and failure states.
- [ ] Add a transmission status model: `idle`, `queued`, `published`, `sending`, `receiving`, `verifying`, `success`, `failed`.
- [ ] Use the existing shadcn component system and semantic theme tokens.
- [ ] Keep server components server-side; isolate interactive controls in small client components.
- [ ] Do not present “success” until receiver telemetry confirms it.

**Acceptance:** a user can submit text, see it queued/published, see receiver-confirmed success or failure, and view the record in history.

### Phase 6 — Browser realtime updates

**Goal:** update the dashboard without polling when the backend has new state.

- [ ] Choose and document the browser realtime mechanism: Upstash Realtime or an appropriate Vercel streaming approach.
- [ ] Publish normalized transmission events when command/telemetry state changes.
- [ ] Add a client subscription for latest status, received content, and progress.
- [ ] Handle reconnects and stale client state.
- [ ] Keep ESP32 command Pub/Sub separate from browser realtime transport.

**Acceptance:** an open dashboard reflects command and receiver state changes without periodic API polling.

### Phase 7 — History management and measured statistics

- [ ] Implement history retrieval with bounded pagination/limits.
- [ ] Implement confirmed `DELETE /api/history` with explicit confirmation in the UI.
- [ ] Store input/output/status/type/timestamps and available stats.
- [ ] Add baud, UART format, byte counts, packet counts, checksum status, and measured durations only when supplied by firmware.
- [ ] Clearly distinguish unavailable measurements from zero values.

**Acceptance:** history survives reloads, can be cleared intentionally, and displays only real measurements.

### Phase 8 — Images and chunked optical transport

- [ ] Validate uploads at a maximum of 500 KB.
- [ ] Define binary/chunk metadata and checksum strategy.
- [ ] Store or stream chunks without putting a 500 KB base64 image in one ESP32 buffer.
- [ ] Avoid one HTTP request per chunk from the ESP32.
- [ ] Add sender/receiver reassembly and retry behavior.
- [ ] Add completed-image storage and dashboard rendering.

**Acceptance:** a supported image can be transmitted, reassembled, verified, and displayed without exceeding ESP32 memory constraints.

### Phase 9 — Progressive reveal and UX polish

- [ ] Add buffered progressive text updates; never send one cloud request per character.
- [ ] Add chunk-based image progress/reveal.
- [ ] Add animated transmission popup/state timeline.
- [ ] Add useful stats and error explanations.
- [ ] Add responsive communications-control-panel visual design.

**Acceptance:** UI animation reflects actual backend/receiver state and does not imply physical progress that was not measured.

### Phase 10 — Optimization

- [ ] Measure end-to-end latency and effective bitrate.
- [ ] Evaluate baud-rate changes only after the system is reliable.
- [ ] Tune chunk size and memory usage.
- [ ] Add robust retry/recovery and pending-job handling.
- [ ] Evaluate Redis Streams/persistent job recovery for commands missed while offline.
- [ ] Review Redis traffic, bundle size, and client rendering performance.

## Intentionally deferred

- [ ] Firmware modifications before the cloud contract and API behavior are settled.
- [ ] Any change to the physical circuit or optical protocol.
- [ ] Authentication/authorization architecture beyond protecting environment secrets.
- [ ] Images and base64 transport.
- [ ] Redis Streams/job recovery until live Pub/Sub is proven.
- [ ] Higher baud-rate optimization beyond the current `250000` setting.

## Current status

- [x] PRD reviewed.
- [x] Existing sender and receiver firmware reviewed.
- [x] Existing Next.js/shadcn application reviewed.
- [x] ESP32-S3 target confirmed.
- [x] Current `250000` baud confirmed for now.
- [x] Firmware changes explicitly deferred.
- [ ] Phase 0 implementation started.
