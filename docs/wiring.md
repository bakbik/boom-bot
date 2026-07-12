# Wiring — MCU 1 (ESP32-S3 Lolin), explicit pins

Machine-readable version: [`firmware/common/pins.h`](../firmware/common/pins.h)
— keep the two in sync. Diagram: [`wiring-diagram.svg`](wiring-diagram.svg).

> **Board assumption:** full-size Wemos LOLIN S3 (16 MB flash / 8 MB octal
> PSRAM). The GPIO numbers below avoid every ESP32-S3 chip-reserved pin
> (strapping 0/3/45/46, USB 19/20, flash 26–32, octal PSRAM 33–37, console
> 43/44) and the Lolin's onboard RGB LED (38), so they are safe on any S3
> board that exposes them. If your board revision doesn't expose one of these
> pins, remap it in `pins.h` — nothing else needs to change.

---

## The drive chain: wheel → motor → L298N → Lolin → power

```
 LEFT WHEEL                                RIGHT WHEEL
  65mm, press-fit                           65mm, press-fit
      │                                         │
  N20 gearbox shaft                         N20 gearbox shaft
      │                                         │
 ┌────┴─────┐                              ┌────┴─────┐
 │ N20 LEFT │                              │ N20 RIGHT│
 │  motor   │                              │  motor   │
 └─┬──────┬─┘                              └─┬──────┬─┘
   │ +    │ −                                │ +    │ −
   │      │                                  │      │
  OUT1   OUT2   ┌───────────────────────────────┐   OUT3   OUT4
   └──────┴─────┤             L298N             ├─────┴──────┘
                │                               │
                │ ENA  IN1  IN2  IN3  IN4  ENB  │  VS   VSS   GND
                └──┬────┬────┬────┬────┬────┬───┴───┬────┬─────┬──┘
                   │    │    │    │    │    │       │    │     │
      Lolin GPIO:  5    6    7    16   47   15      5V   5V   GND
                 (PWM)                    (PWM)    rail rail shared
```

| From | To | Wire |
|------|----|------|
| Left wheel | Left N20 output shaft | press-fit |
| Left N20 `+` terminal | L298N `OUT1` | motor wire |
| Left N20 `−` terminal | L298N `OUT2` | motor wire |
| Right N20 `+` terminal | L298N `OUT3` | motor wire |
| Right N20 `−` terminal | L298N `OUT4` | motor wire |
| L298N `ENA` (jumper removed) | Lolin `GPIO5` | PWM — left speed |
| L298N `IN1` | Lolin `GPIO6` | left direction |
| L298N `IN2` | Lolin `GPIO7` | left direction |
| L298N `ENB` (jumper removed) | Lolin `GPIO15` | PWM — right speed |
| L298N `IN3` | Lolin `GPIO16` | right direction |
| L298N `IN4` | Lolin `GPIO47` | right direction |
| L298N `VS` (VMOT, 12V terminal) | 5 V rail `+` | motor power |
| L298N `VSS` (5V terminal, **5V-EN jumper removed**) | 5 V rail `+` | logic power |
| L298N `GND` | common ground | shared with everything |

- Forward = `IN1 high, IN2 low` (left) and `IN3 high, IN4 low` (right), EN = PWM
  duty. If a wheel spins the wrong way on the bench, swap that motor's two wires
  at the OUT terminals (or invert its IN pair in firmware) — N20 `+`/`−` marking
  varies between batches.
- **Motor kill:** `GPIO5` and `GPIO15` low → both motors free-wheel regardless
  of IN pins. This is the safety watchdog's hardware cut.

## Power tree

```
 [5V 5000 mAh power bank, 3A, always-on]
   │
   ├── USB-A #1 → USB-C cable → Lolin USB-C port (powers the board; its
   │              onboard regulator feeds the 3.3 V rail below)
   │
   ├── USB-A #2 → cut USB cable / breakout: red = 5 V, black = GND
   │      ├── 5 V → L298N VS   (motor supply)
   │      ├── 5 V → L298N VSS  (logic — onboard 78M05 can't regulate from 5 V,
   │      │                     so remove the 5V-EN jumper and feed VSS direct)
   │      └── 5 V → 100k ── GPIO4 (ADC) ── 100k ── GND   (battery sense divider)
   │
   └── (MCU 2 / ESP32-S3-CAM powered from its own USB-A or a Y-split)

 Lolin 3V3 pin → MPU-6050 VCC, 2× VL53L0X VCC, 2× AS5600 VCC,
                 2× GC9A01 VCC + BLK (backlight tied high)

 ⚠ ALL grounds common: power bank GND ↔ Lolin GND ↔ L298N GND ↔ CAM GND
   ↔ every sensor/display GND. Without a shared ground none of the logic
   signals are valid.
```

## Full Lolin pin map

| GPIO | Direction | Connects to | Function |
|------|-----------|-------------|----------|
| 1 | I/O | AS5600 (right) SDA | I2C1 data |
| 2 | out | AS5600 (right) SCL | I2C1 clock |
| 4 | in (ADC1_CH3) | 100k/100k divider on 5 V rail | battery voltage (reads ≈2.5 V) |
| 5 | out (PWM) | L298N ENA | left motor speed |
| 6 | out | L298N IN1 | left motor dir |
| 7 | out | L298N IN2 | left motor dir |
| 8 | I/O | MPU-6050 + 2× VL53L0X + AS5600 (left) SDA | I2C0 data |
| 9 | out | same devices, SCL | I2C0 clock |
| 10 | out | GC9A01 left CS | display select L |
| 11 | out | both GC9A01 SDA/MOSI | SPI data (shared) |
| 12 | out | both GC9A01 SCL/SCLK | SPI clock (shared) |
| 13 | out | GC9A01 right CS | display select R |
| 14 | out | both GC9A01 DC | data/command (shared) |
| 15 | out (PWM) | L298N ENB | right motor speed |
| 16 | out | L298N IN3 | right motor dir |
| 17 | out | ESP32-S3-CAM RX | UART1 TX (115200) |
| 18 | in | ESP32-S3-CAM TX | UART1 RX |
| 21 | out | both GC9A01 RST | display reset (shared) |
| 41 | out | VL53L0X left XSHUT | ToF re-address at boot |
| 42 | out | VL53L0X right XSHUT | ToF re-address at boot |
| 47 | out | L298N IN4 | right motor dir |
| 3V3 | — | all sensor/display VCC | 3.3 V rail |
| GND | — | common ground | — |
| **spare** | | GPIO 39, 40, 48 | future (touch sensor, speaker, mouth display CS…) |

## I2C address plan

The two AS5600 encoders have a **fixed address (0x36)** — that's why they're
split across the two I2C controllers.

| Bus | Pins | Device | Address |
|-----|------|--------|---------|
| I2C0 | SDA 8 / SCL 9 | MPU-6050 | 0x68 |
| I2C0 | | VL53L0X left | 0x30 (re-addressed at boot via XSHUT 41) |
| I2C0 | | VL53L0X right | 0x29 (default; held in reset via XSHUT 42 while left is re-addressed) |
| I2C0 | | AS5600 left | 0x36 |
| I2C1 | SDA 1 / SCL 2 | AS5600 right | 0x36 (no conflict — separate bus) |

Boot sequence for the ToF pair: hold both XSHUT low → release left (41) →
re-address it to 0x30 → release right (42), which comes up at the default 0x29.

## Display SPI plan

Both GC9A01s share MOSI (11), SCLK (12), DC (14), RST (21); only CS differs
(10 = left eye, 13 = right eye). BLK (backlight) tied to 3V3. When the
lower-face "mouth" display is added later it joins the same bus with a spare
GPIO (39/40/48) as its CS.

## Bench bring-up order

1. **Grounds first** — verify continuity between power bank GND, Lolin GND,
   L298N GND before applying power.
2. Power the Lolin alone → flash a blink/hello sketch.
3. I2C0 scan → expect 0x68 (MPU); add ToF + encoder, re-scan.
4. Displays → init both GC9A01s, draw the BOOT gesture.
5. L298N with **wheels off the ground** → verify each wheel's direction
   matches `D:F` forward; fix any reversed motor at the OUT terminals.
6. UART loopback with the CAM board (M6 integration test).
7. Only then: balance tuning with the full stack.
