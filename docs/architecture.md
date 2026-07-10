# Architecture

## Dual-MCU Split

The robot separates real-time control from high-level vision/behavior into two independent processors communicating over UART.

### MCU 1 — ESP32-S3 Lolin (Real-Time Controller)

Owns everything that requires deterministic timing:

- **Balance PID** — 200 Hz loop, complementary filter → Madgwick filter upgrade
- **Motor control** — PWM to TB6612FNG, direction + speed
- **IMU** — MPU-6050 over I2C (400 kHz)
- **Encoders** — 2× AS5600 magnetic encoders for closed-loop speed control
- **Display** — 2× GC9A01 round TFT via SPI, renders gesture animations
- **ToF sensors** — 2× VL53L0X at 45° flanks for near-range obstacle data
- **Watchdog** — if no heartbeat from MCU 2 for 500 ms → safe mode (balance only, zero drive)

### MCU 2 — ESP32-S3-CAM (Vision & Behavior)

Owns everything that can tolerate variable latency:

- **Camera** — OV2640 via DVP, frames captured at QVGA (320×240) for inference
- **Vision** — ESP-WHO library: person detection, color blob tracking, face detection
- **Behavior FSM** — decides mode (IDLE / FOLLOW / AVOID / DANCE / SLEEP)
- **WiFi** — serves REST API + MJPEG stream for Flutter app
- **Telemetry** — aggregates MCU 1 data, logs to NVS / pushes to MQTT

### Communication

See `docs/protocol.md` for full packet spec.

UART wiring:
```
ESP32-S3 Lolin GPIO17 (TX) → ESP32-S3-CAM RX
ESP32-S3 Lolin GPIO18 (RX) → ESP32-S3-CAM TX
GND ←→ GND (shared ground required)
```
Both boards run at 3.3V logic — no level shifter needed.

## Behavior State Machine

```
         ┌──────────────────────────────────┐
         │              IDLE                │
         │   eyes: slow blink + scan        │
         └──┬───────────────────────────────┘
            │ person detected
            ▼
         ┌──────────────────────────────────┐
    ┌───►│             FOLLOW               │◄──┐
    │    │   eyes: pupils track target      │   │
    │    └──┬───────────────────────────────┘   │
    │       │ obstacle < 40cm (ToF)              │ obstacle cleared
    │       ▼                                    │
    │    ┌──────────────────────────────────┐    │
    │    │              AVOID               │────┘
    │    │   eyes: side-eye toward obstacle │
    │    └──────────────────────────────────┘
    │
    │    ┌──────────────────────────────────┐
    └────│              SLEEP               │
         │   (no motion/person for 5 min)   │
         │   eyes: closed, dim              │
         └──────────────────────────────────┘

         ┌──────────────────────────────────┐
         │              DANCE               │ (triggered by app command)
         │   eyes: wink + color flash       │
         └──────────────────────────────────┘
```

## Safety Rules

1. MCU 1 **never** waits on MCU 2 for balance decisions
2. MCU 2 sends heartbeat every 200 ms; MCU 1 triggers safe mode after 500 ms silence
3. ToF obstacle data is processed on MCU 1 directly — no vision latency in the avoidance path
4. Motor driver has hardware enable pin tied to MCU 1 GPIO — can cut motors instantly
