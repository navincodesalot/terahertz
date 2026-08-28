# Terahertz Phase 2 — Cloud Integration PRD

## Vercel + Next.js 16 + Upstash Redis Pub/Sub + ESP32 FSO Link

---

# 1. Project Context

Terahertz is a free-space optical communication system.

The physical link is already working and should be treated as an existing subsystem.

Current physical architecture:

```text
SENDER ESP32
     │
     │ Hardware UART
     ▼
2N7000 MOSFET
     │
     ▼
650 nm Laser
     │
     │ FREE-SPACE OPTICAL LINK
     ▼
BPW34 Photodiode
     │
     ▼
MCP6292 TIA
     │
     ▼
MAX941 Comparator
     │
     ▼
RECEIVER ESP32
```

The project has moved from the physical/protocol development phase into the cloud/networking phase.

---

# 2. Existing Physical-Layer Baseline

DO NOT redesign or modify the physical layer unless explicitly necessary.

## Sender

Current laser driver:

```text
ESP32 GPIO
    │
    │ direct wire / ~100 Ω
    ▼
2N7000 Gate
    │
    ├── 10 kΩ → GND
    │
    ▼
2N7000 Drain
    │
    │
    └── 82 Ω resistor → Laser cathode
                             
Laser anode → supply
2N7000 Source → GND
```

The previous TL431 analog feedback/current-control loop was removed because it limited high-speed switching.

## Receiver

```text
BPW34
   │
   ▼
MCP6292 TIA
   │
   ├── feedback resistor = 100 kΩ
   └── feedback capacitor = 10 pF
   │
   ▼
MAX941 comparator
   │
   ▼
ESP32 receiver
```

The previous 100 pF TIA feedback capacitor was reduced to 10 pF because it excessively limited bandwidth.

## Optical performance baseline

* 650 nm laser
* 5 mW laser
* BPW34 photodiode
* MCP6292 TIA
* MAX941 comparator
* Hardware UART
* 115200 baud = earlier reliability baseline
* 230400 baud = current cloud-integration setting; now being validated with paced streaming

For the current cloud integration, use:

**230400 baud.**

Keep this setting fixed while implementing receiver validation. Re-test at 115200 only if the physical optical link proves unreliable; do not change baud and cloud behavior in the same experiment.

---

# 3. Current Firmware Architecture

The custom software bit-banging protocol was abandoned because of clock drift at higher speeds.

The system now uses:

**ESP32 Hardware UART.**

Current UART concept:

```cpp
Serial1.begin(
    BAUD_RATE,
    SERIAL_8N1,
    rxPin,
    txPin,
    true
);
```

The final `true` inverts the UART signal so that the electrical/laser line idles LOW.

This is important because an idle-high UART would otherwise leave the laser continuously ON.

The cloud integration uses a shared binary-safe optical frame envelope:

```text
[magic][version][type][sequence][payload length][CRC32][payload]
```

The sender emits one frame at a time over the existing inverted hardware UART. The receiver must:

1. synchronize on the frame magic bytes
2. validate the protocol version and payload length
3. read the sequence, CRC32, and payload
4. calculate and compare CRC32
5. reject corrupt or oversized frames
6. dispatch valid text/image-start/image-chunk/image-end frames

CRC32 protects each optical frame. Image-level SHA-256 verification, missing/duplicate chunk detection, and reassembly are receiver-layer work that remains to be implemented.

The cloud layer feeds jobs into this existing hardware UART path rather than changing the laser driver or physical circuit.

---

# 4. Cloud Product Vision

The final product is a polished web dashboard hosted on Vercel.

The UI should have a clear left/right layout.

```text
┌────────────────────────────────────────────────────────────────────┐
│                         TERAHERTZ                                 │
│                    FREE-SPACE OPTICAL LINK                        │
├──────────────────────────────┬─────────────────────────────────────┤
│                              │                                     │
│          SEND                │             RECEIVE                 │
│                              │                                     │
│   Message Type               │       Received Message              │
│   [ TEXT ▼ ]                 │                                     │
│                              │       HELLO WORLD                    │
│   ┌──────────────────────┐   │                                     │
│   │                      │   │       Transmission Stats             │
│   │   Enter message...   │   │                                     │
│   │                      │   │       115200 baud                    │
│   └──────────────────────┘   │       11 bytes                      │
│                              │       0 errors                      │
│   [ SEND ]                   │       142 ms                        │
│                              │                                     │
│                              │                                     │
├──────────────────────────────┴─────────────────────────────────────┤
│                         TRANSMISSION HISTORY                       │
│                                                                    │
│  HELLO WORLD        TEXT       SUCCESS       115200       142 ms  │
│  OPTICAL LINK       TEXT       SUCCESS       115200       138 ms  │
│  image.jpg          IMAGE      SUCCESS       115200       43.2 s  │
│                                                                    │
│                              [ CLEAR HISTORY ]                     │
└────────────────────────────────────────────────────────────────────┘
```

The UI should feel like a communications-system control panel, not a generic CRUD application.

---

# 5. Message Types

Initial supported message types:

```text
TEXT
IMAGE
```

## TEXT

User enters arbitrary text.

Example:

```text
HELLO WORLD
```

The cloud system sends the command to the ESP32 Sender.

## IMAGE

User uploads an image.

Maximum:

**500 KB**

The image should eventually be transmitted as binary/chunked data.

Do NOT design the system around putting a 500 KB base64 image directly into one ESP32 memory buffer.

---

# 6. Three Communication Layers

Keep the system conceptually separated into three independent layers.

## Layer A — Command Transport

```text
Vercel / Next.js
      │
      │ PUBLISH
      ▼
Upstash Redis Pub/Sub
      │
      │ persistent subscription
      ▼
ESP32 Sender
```

The command transport is implemented and hardware-tested for text and paced image streaming. The next priority is receiver-side validation and telemetry.

---

## Layer B — Optical Transport

```text
ESP32 Sender
      │
    UART
      │
    Laser
      │
     FSO
      │
  Photodiode
      │
     TIA
      │
  Comparator
      │
ESP32 Receiver
```

This already exists.

---

## Layer C — Dashboard Realtime / Telemetry

Eventually:

```text
ESP32 Receiver
      │
      │ HTTPS POST
      ▼
Next.js /api/receive
      │
      ▼
Upstash Redis
      │
      ▼
Realtime event
      │
      ▼
Browser
```

For browser-side live updates, Upstash Realtime or an equivalent Vercel streaming mechanism may be used. Before enabling this POST path, the receiver will first print and locally verify received text/images over USB serial. The receiver must report success only after frame CRC, ordering, byte count, and image SHA-256 checks pass.

DO NOT confuse this with the ESP32 command transport.

---

# 7. IMPORTANT: ESP32 COMMAND TRANSPORT DECISION

The chosen solution is:

# STANDARD UPSTASH REDIS PUB/SUB

NOT polling.

NOT periodic HTTP GET requests.

NOT browser-to-ESP32 direct communication.

The ESP32 Sender should maintain a persistent connection and subscribe to a Redis channel.

Conceptually:

```text
ESP32 Sender
      │
      │ Wi-Fi
      ▼
Upstash Redis
      │
      │ SUBSCRIBE laser_commands
      │
      ▼
waiting...
```

When Vercel publishes:

```text
PUBLISH laser_commands {...}
```

the Redis server pushes the message through the already-open connection.

The sender should process it immediately, while applying bounded memory use and UART backpressure so Redis delivery does not outrun the physical optical link.

Desired behavior:

```text
ESP32 powered on
       ↓
connect Wi-Fi
       ↓
connect to Upstash Redis
       ↓
SUBSCRIBE laser_commands
       ↓
WAIT
       ↓
Vercel publishes command
       ↓
ESP32 receives command immediately
       ↓
decode command
       ↓
send over existing UART/laser protocol
```

There must be NO polling loop like:

```text
every 500 ms:
    GET /api/message
```

The goal is true push-based command delivery.

---

# 8. Why Standard Redis Pub/Sub Is Being Used

For the ESP32 command channel, standard Redis Pub/Sub is preferred over Upstash Realtime/SSE.

Redis Pub/Sub gives:

```text
persistent TCP/TLS connection
        ↓
Redis RESP protocol
        ↓
SUBSCRIBE
        ↓
server pushes messages
```

SSE/Reatime would introduce:

```text
HTTP
 ↓
TLS
 ↓
SSE stream
 ↓
event framing
 ↓
text parsing
```

Both can achieve push-based delivery, but standard Redis Pub/Sub is the more direct fit for an always-connected ESP32 command subscriber.

Upstash Realtime remains useful for the **browser/dashboard realtime layer**.

---

# 9. First Development Milestone

DO NOT start by building the entire dashboard.

The first objective is to prove:

```text
Next.js
   ↓
Upstash Redis
   ↓
Redis Pub/Sub
   ↓
ESP32 Sender
```

with arbitrary messages.

The first test should literally be:

```text
Vercel / Next.js
      │
      │ PUBLISH
      │ "HELLO"
      ▼
Upstash Redis
      │
      │ laser_commands
      ▼
ESP32 Sender
      │
      ▼
Serial Monitor

RECEIVED: HELLO
```

Then:

```text
Vercel
   ↓
PUBLISH
"TEST 123"
   ↓
ESP32
   ↓
RECEIVED: TEST 123
```

Then test arbitrary payloads.

Only after this works should the command actually invoke the laser transmission.

---

# 10. First Command Schema

Use a structured command rather than publishing a raw string.

Initial text command:

```json
{
  "id": "msg_abc123",
  "type": "text",
  "payload": "HELLO WORLD",
  "timestamp": 1787870000000
}
```

The exact ID-generation mechanism is up to the implementation.

The important fields are:

```text
id
type
payload
timestamp
```

Eventually add:

```text
baud
encoding
chunk information
size
checksum
```

where appropriate.

The ESP32 should parse the JSON command received from Redis and dispatch based on `type`.

Example:

```text
type == "text"
    ↓
send text over optical link

type == "image"
    ↓
begin image/chunk transmission
```

---

# 11. Reliability Requirement for Pub/Sub

Important Redis behavior:

**Redis Pub/Sub is ephemeral / at-most-once.**

If the ESP32 is disconnected when:

```text
PUBLISH laser_commands ...
```

occurs, the ESP32 will not receive that message later.

Therefore:

```text
PUB/SUB
=
live, instant command delivery
```

while:

```text
Redis persistent storage / Streams
=
history + pending jobs + recovery
```

Do not attempt to turn Pub/Sub itself into the database.

The eventual system should save the command before publishing it.

Conceptually:

```text
POST /api/send
      │
      ├── 1. Save message to Redis
      │
      └── 2. PUBLISH to laser_commands
                    │
                    ▼
                 ESP32
```

This allows us to know that a command exists even if the ESP32 was temporarily offline.

Later we can add:

```text
ESP32 reconnects
      ↓
reports ONLINE
      ↓
server checks for pending jobs
      ↓
republishes pending job
```

This is a later reliability feature.

For the first prototype, prioritize getting live Pub/Sub working.

---

# 12. Do Not Overengineer Authentication Yet

This is a learning project.

The immediate priority is:

1. correctness
2. low latency
3. reliable delivery
4. speed
5. easy debugging

Do not let authentication architecture block the first proof-of-concept.

Use environment variables for credentials, but do not spend development time building elaborate authentication/authorization systems yet.

---

# 13. Eventual Text Transmission Flow

Once the Pub/Sub test works:

```text
User enters:

HELLO WORLD

       ↓

Click SEND

       ↓

POST /api/send

       ↓

Next.js creates message ID

       ↓

Redis stores message

       ↓

Redis PUBLISH laser_commands

       ↓

ESP32 Sender receives command

       ↓

ESP32 converts command to existing
optical transmission format

       ↓

UART

       ↓

LASER

       ↓

FSO

       ↓

RECEIVER ESP32

       ↓

decode packet

       ↓

checksum verification

       ↓

successful message
```

---

# 14. Receiver → Vercel

After the optical message succeeds, the Receiver ESP32 should eventually send telemetry to:

```text
POST /api/receive
```

Example:

```json
{
  "id": "msg_abc123",
  "status": "success",
  "type": "text",
  "input": {
    "bytes": 11
  },
  "output": {
    "text": "HELLO WORLD",
    "bytes": 11
  },
  "radio": {
    "baud": 115200,
    "uart": "8N1"
  },
  "performance": {
    "transmissionMs": 142,
    "bitErrors": 0,
    "checksumPassed": true,
    "retries": 0
  },
  "timestamp": 1787870000000
}
```

The Receiver should not simply report "success."

We want actual measured statistics.

---

# 15. Statistics to Track

Eventually store as much useful telemetry as the firmware can accurately measure.

## Message

```text
message ID
type
payload size
byte count
chunk count
```

## Optical/physical

```text
UART baud
UART format
packet size
checksum status
packet count
successful packets
failed packets
bit/byte errors if measurable
retries
```

## Timing

```text
queue latency
actual optical transmission duration
receiver processing duration
cloud POST latency
total end-to-end duration
```

## Derived

```text
raw bitrate
effective bitrate
success rate
packet loss
retry rate
error rate
```

Do not fabricate measurements.

If firmware cannot accurately measure something, leave it unavailable rather than inventing a number.

---

# 16. History

Every completed transmission should eventually be saved.

Example:

```text
message:abc123
message:def456
message:ghi789
```

Each record should contain:

```text
input
output
status
timestamp
type
size
stats
```

Redis persistent storage should be used for history.

Redis Pub/Sub itself should NOT be treated as historical storage.

Redis Streams may be useful for event/history storage, but choose the simplest implementation that satisfies the requirements.

---

# 17. Clear History

The dashboard must eventually provide:

```text
[ CLEAR HISTORY ]
```

with confirmation.

Example:

```text
Are you sure?

This will permanently delete
all transmission history.

       CANCEL       DELETE
```

Then:

```text
DELETE /api/history
```

The server deletes the relevant Redis history records.

---

# 18. Progressive Text Reveal

The final UI should support a visual progressive-receive effect.

For example:

```text
H
HE
HEL
HELL
HELLO
HELLO 
HELLO W
HELLO WO
HELLO WOR
HELLO WORL
HELLO WORLD
```

However:

**DO NOT POST one HTTP request per character.**

That would create unnecessary cloud traffic.

Instead the Receiver should buffer data and periodically report progress.

For example:

```text
Receiver
   ↓
bytes arrive
   ↓
buffer
   ↓
every ~20–50 ms or N bytes
   ↓
progress event
   ↓
Vercel
   ↓
browser
```

The exact interval should be configurable.

The visual animation does NOT need to correspond 1:1 with individual physical bytes.

---

# 19. Progressive Image Reveal

Images are capped at:

**500 KB**

The image should eventually be split into chunks.

Conceptual format:

```text
IMAGE_START

message_id
total_size
chunk_size
chunk_count

CHUNK 0000
CHUNK 0001
CHUNK 0002
...
CHUNK N

IMAGE_END
checksum
```

Do not put a 500 KB base64 image into one ESP32 memory buffer.

Do not require the ESP32 to make hundreds of HTTP requests to retrieve individual chunks.

The preferred architecture is to push the required data through the persistent command/data transport efficiently.

Exact chunking strategy should be designed after the basic text Pub/Sub path works.

---

# 20. Progressive Image UI

Eventually the dashboard should be able to show the image materializing.

Conceptually:

```text
0%
┌──────────────┐
│              │
│              │
│              │
└──────────────┘

25%
┌──────────────┐
│██████████████│
│██████████████│
│              │
└──────────────┘

50%
┌──────────────┐
│██████████████│
│██████████████│
│██████████████│
│              │
└──────────────┘

100%
┌──────────────┐
│   COMPLETE   │
│    IMAGE     │
└──────────────┘
```

This is a UI effect layered over actual transmission progress.

If true progressive JPEG/image decoding becomes unnecessarily complex, an easier fallback is to progressively update a visual representation based on received chunks and then display the completed image.

---

# 21. Animated Transmission Popup

When a user presses SEND, display an animated status popup.

State progression:

```text
IDLE
 ↓
QUEUED
 ↓
SENDING TO ESP32
 ↓
LASER ACTIVE
 ↓
RECEIVING
 ↓
CHECKSUM VERIFIED
 ↓
SUCCESS
```

Failure path:

```text
...
 ↓
CHECKSUM FAILED
 ↓
FAILED
```

The popup should eventually display useful stats.

Example:

```text
┌────────────────────────────┐
│       ✓ TRANSMITTED        │
│                            │
│       HELLO WORLD          │
│                            │
│       115200 baud          │
│       11 bytes             │
│       142 ms               │
│       0 errors             │
│                            │
└────────────────────────────┘
```

The animation should make the optical link feel visible and alive.

---

# 22. Final Dashboard Architecture

Target architecture:

```text
                         ┌─────────────────────────────┐
                         │       NEXT.JS 16 / VERCEL   │
                         │                             │
                         │        BEAUTIFUL UI         │
                         │                             │
                         │ ┌──────────┐ ┌────────────┐ │
                         │ │ SEND     │ │  RECEIVE   │ │
                         │ │          │ │            │ │
                         │ │ TEXT     │ │ progressive│ │
                         │ │ IMAGE    │ │ reveal     │ │
                         │ └────┬─────┘ └─────▲──────┘ │
                         │      │             │        │
                         │      ▼             │        │
                         │  /api/send         │        │
                         │      │             │        │
                         └──────┼─────────────┼────────┘
                                │             │
                                ▼             │
                         ┌────────────────────────┐
                         │      UPSTASH REDIS     │
                         │                        │
                         │  Commands              │
                         │  Message history       │
                         │  Telemetry             │
                         │  Streams               │
                         │                        │
                         │  + Realtime events     │
                         └───────────┬────────────┘
                                     │
                              command transport
                                     │
                                     ▼
                              ┌─────────────┐
                              │ ESP32       │
                              │ SENDER      │
                              └──────┬──────┘
                                     │
                                    UART
                                     │
                                   LASER
                                     │
                                  ═══════
                                   OPTICAL
                                   LINK
                                  ═══════
                                     │
                                  BPW34
                                     │
                                MCP6292
                                     │
                                 MAX941
                                     │
                                    UART
                                     │
                              ┌──────▼──────┐
                              │ ESP32       │
                              │ RECEIVER    │
                              └──────┬──────┘
                                     │
                                  HTTPS
                                     │
                                     ▼
                              /api/receive
                                     │
                                     ▼
                                  Redis
                                     │
                              realtime event
                                     │
                                     ▼
                                  Browser
```

---

# 23. Recommended Next.js 16 Structure

The eventual project can follow something like:

```text
app/
│
├── page.tsx
│
├── api/
│   ├── send/
│   │   └── route.ts
│   │
│   ├── receive/
│   │   └── route.ts
│   │
│   ├── history/
│   │   └── route.ts
│   │
│   └── realtime/
│       └── route.ts
│
├── components/
│   ├── SendPanel.tsx
│   ├── ReceivePanel.tsx
│   ├── TransmissionPopup.tsx
│   ├── History.tsx
│   ├── StatsPanel.tsx
│   └── ImageReveal.tsx
│
└── lib/
    ├── redis.ts
    ├── realtime.ts
    └── protocol.ts
```

This is a suggested organization, not a rigid requirement. Reuse the existing web application's structure where appropriate.

---

# 24. Development Order

DO NOT implement everything at once.

Follow this sequence.

## Phase 1 — Redis → ESP32

Build only:

```text
Next.js API
      ↓
Upstash PUBLISH
      ↓
laser_commands
      ↓
ESP32 SUBSCRIBE
      ↓
Serial Monitor
```

Test:

```text
HELLO
TEST 123
abcdefghijklmnopqrstuvwxyz
special characters
long text
```

Measure:

* delivery reliability
* latency
* reconnect behavior
* ESP32 memory usage
* connection stability

---

## Phase 2 — Redis command → existing laser

Once Phase 1 is stable:

```text
Redis command
      ↓
ESP32 Sender
      ↓
existing UART
      ↓
laser
```

Verify the cloud command produces exactly the same optical transmission as the current local firmware test.

Do not change the proven optical protocol unnecessarily.

---

## Phase 3 — Receiver telemetry

Add:

```text
Receiver
    ↓
POST /api/receive
```

Start with simple:

```json
{
  "id": "...",
  "status": "success",
  "payload": "HELLO WORLD"
}
```

Then progressively add measured statistics.

---

## Phase 4 — Basic dashboard

Build:

```text
left = send
right = receive
bottom = history
```

Support text first.

---

## Phase 5 — Live UI updates

Add Upstash Realtime or an appropriate Vercel streaming mechanism for:

```text
Receiver
    ↓
Vercel
    ↓
Browser
```

Do not use polling for the dashboard if realtime delivery is practical.

---

## Phase 6 — Animated transmission state

Implement:

```text
QUEUED
SENDING
LASER ACTIVE
RECEIVING
VERIFYING
SUCCESS
```

---

## Phase 7 — History + clear

Implement persistent Redis history and:

```text
CLEAR HISTORY
```

---

## Phase 8 — Images

Only after text transmission is stable:

```text
upload
 ↓
≤500 KB validation
 ↓
chunk
 ↓
cloud transport
 ↓
ESP32
 ↓
optical link
 ↓
receiver
 ↓
reassemble
 ↓
dashboard
```

---

## Phase 9 — Progressive reveal

Add:

* progressive text
* progressive image visualization
* transmission progress
* live statistics

---

## Phase 10 — Optimization

Only after the full system works:

* increase UART/optical baud rate
* optimize chunk size
* reduce latency
* measure effective bitrate
* improve retry/recovery
* optimize ESP32 memory
* optimize Redis traffic
* improve dashboard animations

---

# 25. First Task for the Coding Agent

**Ignore the final dashboard for now.**

The immediate task is:

> Implement and prove a persistent, non-polling Upstash Redis Pub/Sub connection between the existing Next.js 16 App Router application and the ESP32 Sender.

Expected result:

```text
Browser
   ↓
Next.js API
   ↓
Upstash Redis PUBLISH
   ↓
laser_commands
   ↓
persistent ESP32 subscription
   ↓
Serial Monitor
```

The first successful test should produce:

```text
[Vercel]
Published:
HELLO WORLD

[ESP32]
Redis message received:
HELLO WORLD
```

The ESP32 should remain subscribed indefinitely and receive new messages immediately whenever Vercel publishes them.

After this is proven, connect the received command to the existing UART/laser transmission function.

---

# 26. Core Engineering Principle

Keep the system modular:

```text
CLOUD COMMAND LAYER
        │
        ▼
    ESP32 JOB
        │
        ▼
OPTICAL TRANSPORT
        │
        ▼
RECEIVER TELEMETRY
        │
        ▼
CLOUD / UI
```

The cloud should not know how the laser works.

The laser firmware should not know how the Vercel UI works.

The Receiver should report measurements rather than fabricate them.

And the dashboard should visualize the actual state of the physical communication system.

The final product should feel like:

> **A web-controlled, real-time optical communications system.**

Not merely a web app that happens to send text through a laser.
