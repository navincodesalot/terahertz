# Terahertz Complete Hardware Layout

This section documents the physical transmitter and receiver circuits used by Terahertz.

Terahertz is a free-space optical communication system:

```text
ESP32-S3
    │
    ▼
Laser Driver
    │
    ▼
650 nm Laser
    │
    │ Free Space
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
ESP32-S3
```

The transmitter converts the ESP32's UART signal into rapid laser ON/OFF modulation.

The receiver converts those optical pulses back into a digital signal that can be read by the receiving ESP32-S3.

## Build Notes

- All fixed resistors used in the prototype are 5-band, 1% tolerance resistors unless otherwise noted.
- The transmitter and receiver are built on separate breadboards and powered separately.
- Each ESP32 shares a common ground with the breadboard circuitry on its own side.
- The transmitter uses ESP32-S3 GPIO4 for UART TX.
- The receiver uses ESP32-S3 GPIO5 for UART RX.
- The current transmitter does **not** use the older TL431 current-control circuit.
- The receiver TIA uses a **10 pF ceramic capacitor**.

---

# 1. Transmitter

## 1.1 Signal Flow

The final transmitter is a simple high-speed low-side MOSFET switch.

```text
ESP32-S3 GPIO4
       │
       ▼
     100 Ω
       │
       ▼
2N7000 Gate


Laser Power
    │
    ▼
D6505I Laser
    │
    ▼
   1 kΩ
    │
    ▼
2N7000 Drain
    │
2N7000 Source
    │
    ▼
   GND
```

The old TL431-based current regulator was removed because the simplified MOSFET driver provided much faster switching for the UART optical link.

---

## 1.2 D6505I 650 nm Laser Diode

The transmitter uses a **D6505I 650 nm, 5 mW laser diode**.

### Pinout

```text
5mW 650nm Laser Diode (D6505I)

                                2
                                |
                                |
                          .-----o-----.
                          |           |
                          |          ---
                         \|/         /|\
                     LD  -----      -----  / PD
                       ^   |           |   v
                      /    |           |
                           |           |
                           1           3
```

Pin functions:

| Pin | Function |
|---|---|
| 1 | Laser cathode |
| 2 | Common case |
| 3 | Monitor photodiode anode |

### Current Prototype Wiring

The current high-speed prototype uses:

```text
Laser Pin 2
    │
   1 kΩ
    │
    ▼
2N7000 Pin 3
   Drain
```

Laser Pin 3, the monitor photodiode connection, is unused.

```text
Laser Pin 3 → NC
```

> **Replication note:** The current breadboard wiring was recorded as **Laser Pin 2 → 1 kΩ → MOSFET Drain**. This differs from an earlier Terahertz wiring record that placed the series resistor on Laser Pin 1. Before producing a PCB or treating this as a manufacturing schematic, verify the physical D6505I lead orientation against the final breadboard/photo. The wiring documented here follows the final working prototype record.

---

## 1.3 2N7000 MOSFET

The transmitter uses one **2N7000TA N-channel MOSFET** as the laser switch.

### Pinout

```text
2N7000 MOSFET
          (TO-92 CASE 135AR)

               ______
              /      \
             |        |
             |________|
             /   |   \
            /    |    \
           |     |     |
           |     |     |
           |     |     |
           1     2     3

      1 - Source
      2 - Gate
      3 - Drain
```

### Connections

| Pin | Function | Connection |
|---|---|---|
| 1 | Source | GND |
| 2 | Gate | ESP32-S3 GPIO4 through 100 Ω |
| 2 | Gate | Also connected to GND through 10 kΩ pulldown |
| 3 | Drain | 1 kΩ resistor from Laser Pin 2 |

The gate circuit is:

```text
ESP32-S3 GPIO4
       │
      100 Ω
       │
       ├──────── 2N7000 Pin 2 / Gate
       │
      10 kΩ
       │
       ▼
      GND
```

The **100 Ω resistor** limits instantaneous gate-drive current and helps reduce switching noise.

The **10 kΩ pulldown** keeps the MOSFET OFF while GPIO4 is floating, such as while the ESP32 is booting.

---

## 1.4 Transmitter Power and Ground

The transmitter breadboard is powered from the transmitter battery/regulator system.

The important connections are:

```text
Battery / Regulator Power
        │
        └──────── Transmitter Power Rail


Battery Ground
        │
        └──────── Breadboard GND Rail


ESP32-S3 GND
        │
        └──────── Same Breadboard GND Rail
```

Therefore the ESP32, MOSFET driver, and transmitter power source all share the same electrical ground.

The ESP32 connection consists of:

```text
GPIO4 ───────── Transmit Signal
GND   ───────── Breadboard Ground
```

The breadboard power and ground rails were used as the common distribution points for the circuit.

---

## 1.5 Complete Transmitter Layout

| Component | Pin / Node | Connection |
|---|---|---|
| ESP32-S3 | GPIO4 | 100 Ω → 2N7000 Pin 2 |
| ESP32-S3 | GND | Breadboard GND |
| 2N7000 | Pin 1 / Source | GND |
| 2N7000 | Pin 2 / Gate | 100 Ω → GPIO4 |
| 2N7000 | Pin 2 / Gate | 10 kΩ → GND |
| 2N7000 | Pin 3 / Drain | 1 kΩ → Laser Pin 2 |
| D6505I | Pin 2 | 1 kΩ → MOSFET Pin 3 |
| D6505I | Pin 3 | NC |
| Battery / regulator | GND | Breadboard GND |
| Battery / regulator | Power | Breadboard power rail |

Overall:

```text
                    ESP32-S3
                     GPIO4
                       │
                      100 Ω
                       │
                       ├──────────────┐
                       │              │
                       ▼             10 kΩ
                2N7000 Pin 2          │
                     GATE             ▼
                       │             GND
                 ┌─────┴─────┐
                 │  2N7000   │
                 └─────┬─────┘
                       │
                 Pin 3 / DRAIN
                       │
                      1 kΩ
                       │
                       ▼
                D6505I Pin 2
                       │
                  LASER DIODE
                       │
                 Optical Output


                2N7000 Pin 1
                    SOURCE
                       │
                       ▼
                      GND
```

---

# 2. Receiver

## 2.1 Signal Flow

The receiver converts incoming laser light into a digital UART signal.

```text
650 nm Laser
     │
     │ Free Space
     ▼
   BPW34
     │
     ▼
MCP6292 TIA
     │
     ▼
MAX941 Comparator
     │
     ├──────── Indicator LED
     │
     └──────── ESP32-S3 GPIO5
```

The stages are:

```text
Light
  ↓
BPW34 Photocurrent
  ↓
MCP6292 Analog Voltage
  ↓
MAX941 Digital HIGH / LOW
  ↓
ESP32-S3 GPIO5
```

---

## 2.2 BPW34 Photodiode

### Pinout

```text
       BPW34
      ┌─────┐
      │     │
      └─────┘
       │   │
      Pin1 Pin2
       A    K
       │    │
    ANODE CATHODE
```

Pin assignments:

| Pin | Function | Connection |
|---|---|---|
| 1 | Anode | GND |
| 2 | Cathode | MCP6292 Pin 2 |

The cathode is the marked side of the BPW34.

```text
BPW34 Pin 1
    │
   GND


BPW34 Pin 2
    │
    ▼
MCP6292 Pin 2
```

The BPW34 converts changes in received laser intensity into a very small photocurrent.

---

# 2.3 MCP6292 TIA

The MCP6292 converts the BPW34's small photocurrent into a usable analog voltage.

### Pinout

```text
               MCP6292
          (PDIP, SOIC, MSOP)
              +---v---+
          [1]-|---|   |-[8]
              |  / \  |
          [2]-|-/___\-|-[7]
              | -   + |
          [3]-|---+-  |-[6]
              |  / \  |
          [4]-|-/___\-|-[5]
              +-------+
```

Pin functions:

| Pin | Name | Function |
|---|---|---|
| 1 | VOUTA | Output of Op Amp A |
| 2 | VINA− | Inverting input of Op Amp A |
| 3 | VINA+ | Non-inverting input of Op Amp A |
| 4 | VSS | Ground |
| 5 | VINB+ | Non-inverting input of Op Amp B |
| 6 | VINB− | Inverting input of Op Amp B |
| 7 | VOUTB | Output of Op Amp B |
| 8 | VDD | Positive supply |

### Exact Connections

| MCP6292 Pin | Connection |
|---|---|
| Pin 1 / OUT A | MAX941 Pin 2 and TIA feedback network |
| Pin 2 / −IN A | BPW34 Pin 2 and TIA feedback network |
| Pin 3 / +IN A | GND |
| Pin 4 / VSS | GND |
| Pin 5 / +IN B | GND |
| Pin 6 / −IN B | GND |
| Pin 7 / OUT B | NC |
| Pin 8 / VDD | +5.3 V |

The second op amp is unused. In the prototype:

```text
Pin 5 → GND
Pin 6 → GND
Pin 7 → NC
```

---

## 2.4 TIA Feedback Network

The MCP6292 TIA uses:

```text
Rf = 100 kΩ
Cf = 10 pF ceramic
```

The resistor and capacitor are connected **in parallel** between MCP6292 Pin 1 and Pin 2.

```text
                    100 kΩ
MCP Pin 1 ─────────/\/\/\/──────── MCP Pin 2
     │                                  │
     │                                  │
     └──────── 10 pF CERAMIC ───────────┘
```

The signal path is:

```text
BPW34 Pin 2
      │
      ▼
MCP6292 Pin 2
      │
      │ TIA
      ▼
MCP6292 Pin 1
      │
      ▼
MAX941 Pin 2
```

The primary analog measurement point is:

```text
MCP6292 Pin 1
```

This is the TIA output where the optical modulation appears as an analog voltage.

---

# 2.5 MAX941 Comparator

The MAX941 converts the TIA's analog voltage into a digital HIGH or LOW.

### Pinout

```text
               MAX941
          (PDIP, SO, µMAX)
             +---v---+
         [1]-| o     |-[8]
             | |---  |
         [2]-| | + \ |-[7]
             | |    >-|
         [3]-| | - / |-[6]
             | |---  |
         [4]-|       |-[5]
             +-------+
```

Physical pin layout:

```text
                 MAX941
             ┌───────────┐
 V+       1 ─┤           ├─ 8 NC
 IN+      2 ─┤           ├─ 7 OUT
 IN−      3 ─┤           ├─ 6 GND
 /SHDN    4 ─┤           ├─ 5 /LATCH
             └───────────┘
```

### Exact Connections

| MAX941 Pin | Function | Connection |
|---|---|---|
| 1 | V+ | +5.3 V |
| 2 | IN+ | MCP6292 Pin 1 |
| 3 | IN− | Trimpot Pin 2 / wiper |
| 4 | /SHDN | +5.3 V |
| 5 | /LATCH | +5.3 V |
| 6 | GND | GND |
| 7 | OUT | Directly to ESP32-S3 GPIO5 |
| 7 | OUT | Also through 1 kΩ resistor to indicator LED |
| 8 | NC | NC |

Pins 4 and 5 are held HIGH at +5.3 V.

```text
MAX941 Pin 4 → +5.3 V
MAX941 Pin 5 → +5.3 V
```

---

## 2.6 MAX941 Output and Indicator LED

There is **no 15 kΩ / 20 kΩ voltage divider in the current receiver**.

MAX941 Pin 7 splits into two paths:

```text
                         ┌──────────── ESP32-S3 GPIO5
                         │
MAX941 Pin 7 / OUT ──────┤
                         │
                         └── 1 kΩ ──► LED ──► GND
```

For the indicator LED:

```text
MAX941 Pin 7
     │
    1 kΩ
     │
     ▼
 LED Anode
     │
 LED Cathode
     │
     ▼
    GND
```

The LED provided a visual indication that the comparator was reacting to the incoming laser.

The comparator output was also wired **directly to ESP32-S3 GPIO5** in the working breadboard:

```text
MAX941 Pin 7
      │
      └──────── ESP32-S3 GPIO5
```

> **Prototype warning:** This documents the circuit as it was physically tested. The MAX941 is powered from approximately 5.3 V and can drive its HIGH output close to that supply voltage. A direct MAX941-to-ESP32 connection should therefore not automatically be copied into a production PCB without verifying the ESP32 input voltage or adding appropriate 3.3 V level protection.

---

# 2.7 Threshold Trimpot

The trimpot produces the comparator's adjustable reference voltage.

### Pinout

```text
          ┌─────────┐
          │   [2]   │ ← Wiper
          │         │
          │ [1] [3] │ ← Ends
          └─────────┘
```

Connections:

| Trimpot Pin | Connection |
|---|---|
| Pin 1 | GND |
| Pin 2 / Wiper | MAX941 Pin 3 / IN− |
| Pin 3 | +5.3 V |

```text
               +5.3 V
                  │
            Trimpot Pin 3
                  │
             Resistive
               Track
                  │
            Trimpot Pin 1
                  │
                 GND


            Trimpot Pin 2
                Wiper
                  │
                  ▼
           MAX941 Pin 3
                IN−
```

The trimpot is adjusted until the wiper produces a threshold of approximately:

```text
0.15 V
```

This threshold separates the laser OFF voltage from the laser ON voltage.

---

## 2.8 Receiver Power and Ground

The receiver breadboard used the power and ground rails on **both sides of the breadboard**.

The corresponding rails were connected together so that the entire breadboard shared the same supply.

```text
LEFT POWER RAIL  ───────── RIGHT POWER RAIL
     +5.3 V                    +5.3 V

LEFT GND RAIL    ───────── RIGHT GND RAIL
      GND                       GND
```

The receiver's +5.3 V rail powers:

```text
MCP6292 Pin 8
MAX941 Pin 1
MAX941 Pin 4
MAX941 Pin 5
Trimpot Pin 3
```

Ground is shared by:

```text
BPW34 Pin 1
MCP6292 Pin 3
MCP6292 Pin 4
MCP6292 Pin 5
MCP6292 Pin 6
MAX941 Pin 6
Trimpot Pin 1
LED Cathode
ESP32-S3 GND
Battery / regulator GND
```

The receiving ESP32 was therefore connected to the breadboard using at least:

```text
ESP32-S3 GPIO5 → MAX941 Pin 7
ESP32-S3 GND   → Breadboard GND
```

Having the ESP32 ground connected to the receiver circuit ground is important because GPIO5 must interpret the comparator output relative to the same ground reference.

---

## 2.9 Complete Receiver Layout

| Component | Pin / Node | Connection |
|---|---|---|
| BPW34 | Pin 1 / Anode | GND |
| BPW34 | Pin 2 / Cathode | MCP6292 Pin 2 |
| MCP6292 | Pin 1 / OUT A | MAX941 Pin 2 |
| MCP6292 | Pin 1 | 100 kΩ + 10 pF ceramic → Pin 2 |
| MCP6292 | Pin 2 / −IN A | BPW34 Pin 2 |
| MCP6292 | Pin 3 / +IN A | GND |
| MCP6292 | Pin 4 / VSS | GND |
| MCP6292 | Pin 5 / +IN B | GND |
| MCP6292 | Pin 6 / −IN B | GND |
| MCP6292 | Pin 7 / OUT B | NC |
| MCP6292 | Pin 8 / VDD | +5.3 V |
| Trimpot | Pin 1 | GND |
| Trimpot | Pin 2 / Wiper | MAX941 Pin 3 |
| Trimpot | Pin 3 | +5.3 V |
| MAX941 | Pin 1 / V+ | +5.3 V |
| MAX941 | Pin 2 / IN+ | MCP6292 Pin 1 |
| MAX941 | Pin 3 / IN− | Trimpot Pin 2 |
| MAX941 | Pin 4 / /SHDN | +5.3 V |
| MAX941 | Pin 5 / /LATCH | +5.3 V |
| MAX941 | Pin 6 / GND | GND |
| MAX941 | Pin 7 / OUT | Directly to ESP32-S3 GPIO5 |
| MAX941 | Pin 7 / OUT | 1 kΩ → LED anode |
| MAX941 | Pin 8 / NC | NC |
| LED | Cathode | GND |
| ESP32-S3 | GPIO5 | MAX941 Pin 7 |
| ESP32-S3 | GND | Breadboard GND |

Overall:

```text
                        650 nm LASER
                             │
                        FREE SPACE
                             │
                             ▼
                          BPW34
                       Pin 1 │ Pin 2
                        │    │
                       GND   ▼
                         MCP6292 Pin 2
                              │
                     ┌────────┴────────┐
                     │                 │
                   100 kΩ      10 pF CERAMIC
                     │                 │
                     └────────┬────────┘
                              │
                         MCP6292 Pin 1
                              │
                              ▼
                         MAX941 Pin 2
                              │
                         COMPARATOR
                              │
                         MAX941 Pin 7
                              │
                   ┌──────────┴───────────┐
                   │                      │
                   ▼                      ▼
            ESP32-S3 GPIO5              1 kΩ
                                          │
                                          ▼
                                         LED
                                          │
                                         GND


                  COMPARATOR THRESHOLD

                        +5.3 V
                           │
                    Trimpot Pin 3
                           │
                       Trimpot
                           │
                    Trimpot Pin 1
                           │
                          GND

                    Trimpot Pin 2
                        Wiper
                           │
                           ▼
                    MAX941 Pin 3
                        ≈ 0.15 V
```

---

# 3. Build and Replication Notes

### Keep Grounds Common Locally

The transmitter and receiver do not need to share a ground with **each other** because the communication link between them is optical.

However, each ESP32 must share ground with the circuitry it is connected to.

```text
TRANSMITTER:

Battery GND
     │
Breadboard GND
     │
ESP32 GND
     │
MOSFET Source


RECEIVER:

Battery GND
     │
Breadboard GND
     │
ESP32 GND
     │
MCP6292 / MAX941 GND
```

### Breadboard Rails

On the receiver, both sets of breadboard power rails were used.

Connect corresponding rails together:

```text
+ POWER LEFT  ↔ + POWER RIGHT

GND LEFT      ↔ GND RIGHT
```

This ensures components placed on either side of the breadboard see the same +5.3 V and ground.

### Measure Before Connecting

Useful receiver test points are:

```text
MCP6292 Pin 1
    → Analog optical signal

Trimpot Pin 2
    → Comparator threshold, approximately 0.15 V

MAX941 Pin 7
    → Digital comparator output

ESP32 GPIO5
    → Received UART waveform
```

### Optical Alignment

The receiver depends heavily on physical alignment.

The laser should strike the BPW34's active area cleanly and consistently. The black optical tube used in the prototype helps reduce ambient light reaching the photodiode.

---

# 4. Current Hardware Baseline

## Transmitter

```text
ESP32-S3 GPIO4
       ↓
     100 Ω
       ↓
2N7000 Gate

Laser
  ↓
1 kΩ
  ↓
2N7000 Drain
  ↓
2N7000 Source
  ↓
GND
```

## Receiver

```text
BPW34
   ↓
MCP6292 TIA
   │
   ├── Rf = 100 kΩ
   └── Cf = 10 pF ceramic
   ↓
MAX941
   │
   ├── Threshold ≈ 0.15 V
   ├── 1 kΩ → Indicator LED
   └── Direct → ESP32-S3 GPIO5
```

## Key Values

| Parameter | Value |
|---|---|
| Transmitter MCU | ESP32-S3 |
| Transmitter UART GPIO | GPIO4 |
| Laser | D6505I |
| Laser wavelength | 650 nm |
| Laser optical power | 5 mW |
| MOSFET | 2N7000TA |
| MOSFET gate resistor | 100 Ω |
| MOSFET gate pulldown | 10 kΩ |
| Laser series resistor | **1 kΩ** |
| Receiver MCU | ESP32-S3 |
| Receiver UART GPIO | **GPIO5** |
| Photodiode | BPW34 |
| TIA op amp | MCP6292 |
| TIA feedback resistor | 100 kΩ |
| TIA feedback capacitor | **10 pF ceramic** |
| Comparator | MAX941 |
| Comparator supply | ≈ 5.3 V |
| Comparator threshold | ≈ 0.15 V |
| MAX941 indicator resistor | 1 kΩ |
| Receiver voltage divider | **None** |
| MAX941 → ESP32 | Direct to GPIO5 |
| Fixed resistor tolerance | 1% |