# Terahertz

A DIY free-space optical communication system inspired by the laser links used in modern satellite and terrestrial networks like **Starlink** and **Taara**.

It sends real data, including text and images, over a **650 nm laser link** between two ESP32-S3 nodes, with a cloud dashboard for sending data, receiving telemetry, verifying integrity, and visualizing transmission performance.

## Demo

<p align="center">
  <a href="https://ggl.link/terahertz-demo">
    <img src="https://img.youtube.com/vi/GarevacOfZU/hqdefault.jpg" alt="Terahertz Demo — Play on YouTube" width="720" />
  </a>
</p>

<p align="center">
  <a href="https://ggl.link/terahertz-demo">Watch on YouTube</a>
</p>

Detailed documentation and circuit images:

* **[Hardware & Circuit Documentation](./docs/HARDWARE.md)** — complete circuit, pinouts, component values, wiring, power, and breadboard replication
* **[Software Architecture](./docs/SOFTWARE.md)** — firmware, optical protocol, image transmission, cloud architecture, and dashboard
* **[Transmitter Image Gallery](./docs/transmitter-gallery.md)** — photos of the transmitter circuit and build
* **[Receiver Image Gallery](./docs/receiver-gallery.md)** — photos of the receiver circuit and build

**Fully open source:** firmware, cloud code, optical protocol, and complete circuit wiring are included.

---

## How It Works

```text
Browser Dashboard
       ↓
Upstash Redis
       ↓
ESP32-S3 Sender
       ↓
650 nm Laser
       ↓
Free Space
       ↓
BPW34 → MCP6292 → MAX941
       ↓
ESP32-S3 Receiver
       ↓
Vercel
       ↓
Browser Dashboard
```

A message or image is sent from the dashboard to the transmitter ESP32 through Upstash Redis. The ESP32 modulates the laser using hardware UART at **250 kbaud**.

The receiver detects the optical signal, reconstructs the data, verifies its integrity, and sends the result and transmission statistics back to the dashboard.

### Performance

- **Throughput**: Up to **0.25 MB/s** (2 Mbps) over free space
- **Modulation**: 250 kbaud UART with 8-bit frames
- **Range**: Limited by laser power (5 mW) and photodiode sensitivity; tested at ~1 meter in lab
- **Reliability**: CRC-32 per-record + SHA-256 end-to-end for image verification

---

## Hardware

### Transmitter

The transmitter uses an ESP32-S3 and 2N7000 MOSFET to modulate a **D6505I 650 nm, 5 mW laser** using hardware UART.

Further images of the transmitter circuit and physical build are available in **[Transmitter Build & Images](./docs/transmitter.md)**.

### Receiver

The receiver uses a **BPW34 photodiode**, **MCP6292 transimpedance amplifier**, and **MAX941 comparator** to reconstruct the digital signal for the receiving ESP32-S3.

Further images of the receiver circuit and physical build are available in **[Receiver Build & Images](./docs/receiver.md)**.

For the complete circuit, including exact pinouts, resistor and capacitor values, wiring, power, grounding, and breadboard replication, see **[Hardware & Circuit Documentation](./docs/HARDWARE.md)**.

Simplified BOM: https://ggl.link/terahertz-simplified-bom

---

## Software

The optical link uses the ESP32-S3's hardware UART at **250 kbaud, 8N1 with inverted logic**, keeping the laser off while idle.

A lightweight protocol handles text and chunked image transfers, with **CRC-32** for record integrity and **SHA-256** for end-to-end image verification.

The cloud pipeline connects the physical optical link to the web, with Redis handling pub/sub queueing and the dashboard displaying real-time transmission stats:

```text
Dashboard
   ↓
Upstash Redis Pub/Sub
   ↓
ESP32-S3 Sender
   ↓
650 nm Optical Link
   ↓
ESP32-S3 Receiver
   ↓
HTTPS
   ↓
Dashboard
```

The dashboard is built with **Next.js 16** and deployed on **Vercel**, allowing text and images to be transmitted while displaying received data, integrity checks, statistics, and transmission history.

For the protocol, firmware behavior, image transfer, and cloud architecture, see the **[Software Documentation](./docs/SOFTWARE.md)**.

---

## Tech Stack

| Layer             | Technology                           |
| ----------------- | ------------------------------------ |
| Web               | Next.js 16, TypeScript, Tailwind CSS |
| UI                | shadcn/ui                            |
| Cloud             | Vercel                               |
| Pub/Sub + Storage | Upstash Redis                        |
| MCU               | ESP32-S3                             |
| Firmware          | Arduino / C++                        |
| Optical Transport | Hardware UART, 250 kbaud             |
| Laser             | D6505I, 650 nm, 5 mW                 |
| Photodiode        | BPW34                                |
| TIA               | MCP6292                              |
| Comparator        | MAX941                               |
| Integrity         | CRC-32, SHA-256                      |

---

## Documentation

| Document                                         | Description                                                               |
| ------------------------------------------------ | ------------------------------------------------------------------------- |
| **[Hardware Layout](./docs/hardware_layout.md)** | Full circuit, pinouts, component values, wiring, power, and replication   |
| **[Transmitter](./docs/transmitter.md)**         | Transmitter circuit and build images                                      |
| **[Receiver](./docs/receiver.md)**               | Receiver circuit and build images                                         |
| **[Software](./docs/SOFTWARE.md)**               | Firmware, protocol, image transmission, cloud architecture, and dashboard |

---

## References

Terahertz was inspired in part by the engineering behind modern free-space optical communication systems, including **Starlink's optical inter-satellite links** and **Taara's terrestrial laser communication technology**.

* [Holly Jackson — Laser Comms](https://holly-jackson.com/laser-comms)
* [Upstash Redis Pub/Sub](https://upstash.com/docs/redis/features/pubsub)
