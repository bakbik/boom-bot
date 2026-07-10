# BoomBot — Project Plan

Dual-MCU self-balancing robot with vision, obstacle avoidance, person following,
and an expressive gesture display.

---

## Architecture Decision

```
┌─────────────────────────────┐         ┌─────────────────────────────┐
│    MCU 1 — ESP32-S3 Lolin   │  UART   │    MCU 2 — ESP32-S3-CAM     │
│                             │◄───────►│                             │
│  • Balance PID (<5ms loop)  │         │  • Camera (ESP-WHO)         │
│  • Motor control (PWM)      │         │  • Obstacle detection       │
│  • IMU — MPU-6050           │         │  • Person/color following   │
│  • Wheel encoders           │         │  • Behavior state machine   │
│  • Gesture display driver   │         │  • WiFi user control        │
│  • Safety watchdog          │         │  • Telemetry aggregator     │
└─────────────────────────────┘         └─────────────────────────────┘
```

> MCU 2 was originally scoped as a Raspberry Pi Zero 2W; it is now an
> ESP32-S3-CAM (see M0). Both boards run 3.3V logic, so the UART link needs
> no level shifter. See `docs/architecture.md` for the current wiring.

**Watchdog rule:** if MCU 1 receives no heartbeat from MCU 2 for 500 ms,
it holds balance at 0° and cuts drive — vision crash can never topple the robot.

---

## Milestones

### M0 — Decisions ✅ resolved
- [x] **Display type** — Option A: 2× GC9A01 round TFT 1.28" (see §7)
- [x] Motor driver — TB6612FNG (replaces L298N)
- [x] MCU 2 platform — ESP32-S3-CAM (replaces RPi Zero 2W; keeps the stack all-ESP32)
- [x] Camera — OV2640 (bundled on the ESP32-S3-CAM)
- [ ] Camera mount — fixed forward vs pan/tilt (still open, see Open Questions)

### M1 — Hardware & BOM
See `docs/hardware.md` for the full BOM with part numbers and prices (~$78 total).
- [x] Finalize full component list with unit prices
- [x] MCU 1: ESP32-S3 Lolin (16MB flash, 8MB PSRAM)
- [x] MCU 2: ESP32-S3-CAM (OV2640 bundled)
- [x] Motor driver: TB6612FNG
- [x] Display: 2× GC9A01 round TFT (Option A)
- [x] 2× VL53L0X ToF sensors (obstacle flanks)
- [x] Wheel encoders: 2× AS5600 magnetic
- [x] Power: 5V 5000mAh always-on power bank (single rail)
- [x] No level shifter needed — both MCUs are 3.3V logic

### M2 — Chassis Redesign (CAD)
- [ ] Dual-layer top plate — RPi Zero 2W + camera mount
- [ ] Camera mount: fixed forward OR pan/tilt bracket (decide in M0)
- [ ] Display cutout/bezel on front face
- [ ] ToF sensor mounts (left 45°, right 45°)
- [ ] Cable routing channels between layers
- [ ] Updated battery bay for 2S LiPo
- [ ] All parts remain FDM-printable (no overhangs >45° without support)

### M3 — MCU 1 Firmware (ESP32)
- [ ] Complementary filter → upgrade to Madgwick for better angle estimate
- [ ] Cascade PID: outer loop (angle), inner loop (wheel speed via encoders)
- [ ] TB6612FNG motor driver API (replaces L298N)
- [ ] UART command receiver — parse speed/direction packets from MCU 2
- [ ] UART telemetry sender — angle, speed, battery voltage, fault flags
- [ ] Heartbeat watchdog (500 ms timeout → safe-mode)
- [ ] Display driver — SPI output to gesture display (post-M0)
- [ ] Unit tests on host (native ESP-IDF build or Unity framework)

### M4 — MCU 2 Firmware (ESP32-S3-CAM)
- [ ] Toolchain: ESP-IDF (or Arduino-ESP32) with ESP-WHO
- [ ] Camera stack: OV2640 via DVP, QVGA (320×240) capture
- [ ] UART command sender to MCU 1 (115200 baud, `firmware/common/protocol.h`)
- [ ] Heartbeat loop (sends `HB\n` every 200 ms)
- [ ] Behavior state machine

```
States: IDLE → FOLLOW → AVOID → DANCE → SLEEP
Triggers:
  IDLE     → FOLLOW  : person detected in frame
  FOLLOW   → AVOID   : obstacle within 40 cm (ToF)
  AVOID    → FOLLOW  : obstacle cleared
  any      → DANCE   : BT/WiFi command "dance"
  any      → SLEEP   : no motion/person for 5 min
```

- [ ] WiFi REST API + MJPEG stream for remote control (replaces HC-05 Bluetooth serial)
- [ ] Telemetry logger (NVS + optional MQTT push)

### M5 — Vision System
- [ ] **Obstacle detection** — VL53L0X ToF primary, camera secondary confirmation
- [ ] **Color following** — HSV blob tracking (configurable target color via app)
- [ ] **Person following** — ESP-WHO human/pedestrian detection model
  - Target lock: hold bounding box center within ±50px of frame center
  - Distance proxy: bounding box height → drive speed (bigger = slow down)
- [ ] **Face detection** — ESP-WHO face detector for close-range interaction trigger
- [ ] Calibration script — intrinsic camera calibration, ToF offset calibration

### M6 — Inter-MCU Protocol (UART)
Packet format (ASCII, newline-terminated):

```
MCU2 → MCU1  (commands)
  HB             heartbeat (every 200 ms)
  D:<dir>:<spd>  direction [F/B/L/R/S], speed [0-100]
  G:<id>         trigger gesture by ID
  M:<mode>       set mode [BALANCE/FOLLOW/AVOID/DANCE/SLEEP]

MCU1 → MCU2  (telemetry, every 50 ms)
  T:<angle>:<spd>:<bat>:<fault>
```

- [x] Protocol spec document — `docs/protocol.md`
- [x] Shared parser + serializer — `firmware/common/protocol.h` (used by both MCUs)
- [x] Host unit tests — `firmware/test/` (292 checks: parse, range clamp, round trips, line reader)
- [ ] On-device loopback integration test (UART echo between the two boards)

### M7 — Display & Gesture System

> **✅ Unblocked — M0 chose Option A (2× GC9A01 round TFTs). Pose design can start.**

#### Display Options (M0 chose Option A)

| Option | Display | Resolution | Feel | Driver |
|--------|---------|-----------|------|--------|
| A | 2× GC9A01 round TFT 1.28" | 240×240 each | Two eyes, most expressive | SPI, ST7789-compat |
| B | 1× ILI9341 2.4" rectangular | 320×240 | Single face panel | SPI |
| C | 2× SSD1306 OLED 0.96" | 128×64 each | Minimal / retro | I2C |
| D | MAX7219 8×8 LED matrix ×2 | 8×8 each | Pixelated, very readable | SPI |

**Chosen: Option A (2× GC9A01 round TFTs)** — gives the most lifelike
expression range and the round shape reads as eyes instantly.

#### Gesture Pose Bank (design AFTER display chosen)
Categories to design:

```
AMBIENT / IDLE
  • blink (slow, lazy)
  • look left / look right / look up
  • sleepy (half-closed, drooping)
  • scanning (eyes dart side to side)

EMOTIONAL
  • happy       (wide, curved-up pupils)
  • excited     (big round, small highlight dot)
  • sad         (drooped, angled inward)
  • angry       (angled down-in brows overlay)
  • surprised   (huge circles, small pupils)
  • confused    (one squint, one wide)
  • love        (heart pupils)

TASK-AWARE
  • following   (pupils track left/right matching target)
  • thinking    (eyes up-left, one blinks slowly)
  • avoiding    (side-eye toward obstacle)
  • lost        (spinning/searching pupils)

REACTIONS
  • dazed       (spiral pupils, for after collision)
  • booting     (loading bar inside eye)
  • low battery (red tint, drooping)
  • dance       (alternating winks + color flash)
```

Transition rules:
- All state changes use a 150 ms ease-in/out tween
- Blink triggers randomly every 3–8 s (perlin noise interval)
- Pupils track detected face/person position in frame

### M8 — Mobile App v2 (Flutter)
- [ ] Replace HC-05 Bluetooth with WiFi WebSocket connection
- [ ] Live camera feed (MJPEG stream from RPi)
- [ ] D-pad manual drive control
- [ ] Mode switcher (Balance / Follow / Dance / Sleep)
- [ ] Color target picker (for color-follow mode)
- [ ] Telemetry dashboard (angle, battery, mode)
- [ ] Gesture preview panel (trigger any gesture manually)

### M9 — Integration & Testing
- [ ] Hardware bring-up checklist
- [ ] Balance PID auto-tune procedure (Ziegler–Nichols)
- [ ] Vision latency test (measure MCU2→MCU1 command delay end-to-end)
- [ ] Obstacle avoidance stress test (random obstacle course)
- [ ] Person following test (track at varying distances 0.5m–3m)
- [ ] Drop/tilt recovery test (push robot, confirm recovery)
- [ ] Long-run stability test (30 min continuous balance)
- [ ] Gesture display review (all poses, correct feel)

---

## Open Questions

1. **Camera fixed or pan/tilt?** Pan/tilt adds expressive head movement but complicates
   the chassis and adds 2 servos + control complexity to MCU 1.
2. **Speaker?** Adding audio (DFPlayer Mini) would enable sound reactions alongside gestures.
3. **Touch sensor?** Capacitive touch on the chassis skin for pet/poke reactions.
4. **Encoder type?** Magnetic (AS5600, clean, no contact wear) vs optical disk (cheaper).

---

## Dependency Graph

```
M0 (Decisions)
  ├─► M1 (BOM)
  │     └─► M2 (Chassis CAD)
  ├─► M3 (ESP32 firmware)   ──┐
  ├─► M4 (RPi software)     ──┼─► M9 (Integration)
  ├─► M5 (Vision)           ──┤
  ├─► M6 (Protocol)         ──┤
  ├─► M7 (Display/Gestures) ──┘
  └─► M8 (App v2)
```

M0 unblocks everything. M3 + M4 + M6 must be complete before M9 starts.
M7 is independent once M0 display decision is made.
