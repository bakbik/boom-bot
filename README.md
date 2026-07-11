# BoomBot

Dual-MCU self-balancing robot with camera vision, obstacle avoidance, person following, and an expressive gesture display.

```
┌─────────────────────────────┐  UART  ┌──────────────────────────────┐
│     MCU 1 — ESP32-S3 Lolin  │◄──────►│  MCU 2 — ESP32-S3-CAM        │
│                             │        │                              │
│  • Balance PID (<5ms loop)  │        │  • Camera (ESP-WHO)          │
│  • Motor control (PWM)      │        │  • Obstacle detection        │
│  • IMU — MPU-6050           │        │  • Person / color following  │
│  • Wheel encoders           │        │  • Behavior state machine    │
│  • Gesture display (SPI)    │        │  • WiFi control API          │
│  • Safety watchdog          │        │  • Telemetry aggregator      │
└─────────────────────────────┘        └──────────────────────────────┘
        │                                          │
   L298N                                   Pi Camera (OV2640)
   2× DC motors                            2× VL53L0X ToF
   2× GC9A01 display (eyes)
```

## Hardware

| Component | Part | Notes |
|---|---|---|
| MCU 1 | ESP32-S3 Lolin | Balance, motors, display |
| MCU 2 | ESP32-S3-CAM | Vision, WiFi, behavior |
| IMU | MPU-6050 | I2C on MCU 1 |
| Motor driver | L298N | Note ~1.5–3 V dropout on the 5 V rail — see `docs/hardware.md` |
| Display | 2× GC9A01 round TFT 1.28" | SPI, driven from MCU 1 |
| Obstacle sensors | 2× VL53L0X ToF | I2C on MCU 1, left+right flanks |
| Encoders | 2× AS5600 magnetic | I2C on MCU 1 |
| Power | 5V 5000mAh (3A output, always-on) | Single rail |

## Repository Layout

```
boom-bot/
  firmware/
    common/        Shared code — UART protocol (protocol.h)
    mcu1/          ESP32-S3 Lolin — real-time controller
    mcu2/          ESP32-S3-CAM  — vision & behavior
    test/          Host unit tests (no hardware needed)
  app/             Flutter mobile app (WiFi WebSocket control)
  cad/             FDM-printable chassis (CadQuery)
  docs/
    architecture.md
    protocol.md    UART packet spec between MCU 1 and MCU 2
    hardware.md    Full BOM with part numbers and prices
  PLAN.md          Full milestone breakdown
```

## Milestones

| # | Milestone | Status |
|---|---|---|
| M0 | Decisions (display type, MCU 2 platform) | ✅ Resolved (camera mount still open) |
| M1 | Hardware BOM | ✅ Done — see `docs/hardware.md` |
| M2 | Chassis redesign (CAD) | ⬜ Not started |
| M3 | MCU 1 firmware (ESP32-S3 Lolin) | 🟢 Control lib + sim done; device wiring pending |
| M4 | MCU 2 software (ESP32-S3-CAM) | ⬜ Not started |
| M5 | Vision system | ⬜ Not started |
| M6 | UART protocol | 🟢 Shared lib + tests done; on-device loopback pending |
| M7 | Gesture pose bank | 🟢 Poses defined + SVG preview; device renderer pending |
| M8 | Flutter app v2 | ⬜ Not started |
| M9 | Integration & testing | ⬜ Not started |

## Power Architecture

```
[5V 5000mAh 3A power bank — always-on mode]
  ├── USB-A → ESP32-S3 Lolin (5V pin)
  │              └── onboard LDO → MPU-6050, ToF, encoders (3.3V)
  │              └── SPI → 2× GC9A01 display (3.3V)
  ├── USB-A → ESP32-S3-CAM (5V pin)
  └── 5V direct → L298N VS (motor supply) + VSS (logic; 5V-EN jumper removed)
```

## Quick Start

_Firmware and app are under active development — see individual READMEs in each subdirectory._

The shared UART protocol library has host unit tests that need only a C++
compiler (no hardware):

```sh
make -C firmware/test
```
