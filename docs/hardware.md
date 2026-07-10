# Hardware — Bill of Materials

> Status: draft — display decided (Option A, 2× GC9A01). Remaining open item: camera mount (fixed vs pan/tilt).

## Microcontrollers

| Qty | Part | Role | Est. Price |
|-----|------|------|-----------|
| 1 | ESP32-S3 Lolin (16MB flash, 8MB PSRAM) | MCU 1 — real-time controller | ~$8 |
| 1 | ESP32-S3-CAM (OV2640 included) | MCU 2 — vision & behavior | ~$10 |

## Sensors & Actuators

| Qty | Part | Notes | Est. Price |
|-----|------|-------|-----------|
| 1 | MPU-6050 breakout | I2C IMU, angle + gyro | ~$2 |
| 2 | VL53L0X ToF breakout | Obstacle flanks, I2C | ~$3 each |
| 2 | AS5600 magnetic encoder | Wheel speed feedback, I2C | ~$3 each |
| 2 | N20 geared DC motor 6V 300RPM | Main drive wheels | ~$4 each |
| 1 | TB6612FNG dual motor driver | Replaces L298N, lower dropout | ~$3 |

### IMU selection — MPU-6050 (GY-521)

Chosen board: **GY-521 (MPU-6050, 6-DOF: 3-axis accel + 3-axis gyro)**.

Considered alternatives on hand:

| Board | Chip(s) | Sensors | Verdict for a self-balancer |
|-------|---------|---------|-----------------------------|
| **GY-521** ✅ | MPU-6050 | accel + gyro (6-DOF) | Exactly what balancing needs; best-documented IMU for self-balancers |
| GY-9250 | MPU-9250 | accel + gyro + magnetometer (9-DOF) | Mag adds heading, but it's corrupted by the nearby motors, and the chip is heavily counterfeited |
| HW-612 (GY-87) | MPU-6050 + HMC5883L + BMP180 | accel + gyro + mag + barometer (10-DOF) | Contains the same MPU-6050; barometer is useless on a ground robot, mag has the same interference problem |

**Why the MPU-6050 wins here:**

1. Balancing only needs **pitch angle + pitch rate**, which come entirely from
   the accel + gyro. All three boards carry an MPU-6050-class accel/gyro (the
   HW-612 literally *contains* an MPU-6050), so all balance equally well — the
   extras don't improve the balance loop at all.
2. The barometer (HW-612) measures altitude — irrelevant for a ground robot.
3. The magnetometer (GY-9250 / HW-612) gives *heading*, not tilt, and a mag
   sitting centimetres from two DC motors and battery currents reads mostly
   noise without careful placement + hard/soft-iron calibration.
4. The MPU-6050 has the deepest ecosystem of self-balancer reference code; the
   MPU-9250 is widely cloned (many "9250" boards are actually an MPU-6500 with
   no magnetometer).

**Heading, if we need it later:** derive yaw from the **AS5600 wheel encoders**
(already in this BOM) rather than a magnetometer — encoders are immune to the
motor magnetic interference. Only add a dedicated magnetometer on a mast, away
from the motors, if encoder odometry proves insufficient.

## Display — decided: Option A (eyes)

| Option | Part | Qty | Notes | Est. Price |
|--------|------|-----|-------|-----------|
| **A ✅ chosen** | GC9A01 round TFT 1.28" 240×240 | 2 | SPI, the two eyes | ~$5 each |
| B | ILI9341 2.4" TFT 320×240 | 1 | Single face panel, color | ~$7 |
| C | SSD1306 OLED 128×64 0.96" | 2 | I2C, minimal/retro | ~$3 each |
| D | MAX7219 8×8 LED matrix | 2 | SPI, pixelated, very readable outdoors | ~$2 each |

### Planned addition — lower-face display (not yet spec'd)

A rectangular panel below the eyes for a mouth / lower face is on the roadmap.
Leading candidate is a wide rectangular TFT (e.g. a 2.4" ILI9341 or a bar-style
IPS) driven from the same SPI bus as the eyes. Deferred until the eye gesture
bank is proven — pin-budget and SPI bandwidth to be confirmed then.

## Power

| Qty | Part | Notes | Est. Price |
|-----|------|-------|-----------|
| 1 | 5V 5000mAh power bank, 3A output, always-on | Anker PowerCore or similar | ~$20 |
| — | *Alternative:* 2× 18650 + TP4056 + 5V boost | No auto-shutoff risk, DIY | ~$8 |

**Note on auto-shutoff:** USB power banks cut output below ~80–100 mA.
A balanced idle robot may trigger this. Either use an always-on bank or add
a 100 mA dummy load (resistor or LED array).

## Chassis & Mechanical

| Qty | Part | Notes | Est. Price |
|-----|------|-------|-----------|
| — | PLA/PETG filament | FDM printed chassis (see `cad/`) | ~$3 |
| 2 | 65mm rubber wheels | Press-fit on N20 shaft | ~$3/pair |
| 4 | M3×10 screws + nuts | Motor mount | ~$1 |
| 2 | M2.5×6 screws | PCB standoffs | ~$1 |
| 4 | Brass M3 heat-set inserts | Top/bottom plate join | ~$1 |

## Estimated Total

| Category | Cost |
|----------|------|
| MCUs | ~$18 |
| Sensors & actuators | ~$22 |
| Display (Option A) | ~$10 |
| Power | ~$20 |
| Chassis materials | ~$8 |
| **Total** | **~$78** |

## Wiring Notes

- ESP32-S3 Lolin ↔ ESP32-S3-CAM: UART on GPIO17/18, shared GND, no level shifter needed (both 3.3V)
- MPU-6050, VL53L0X (×2), AS5600 (×2): all share I2C bus on MCU 1. Assign unique I2C addresses or use I2C mux (TCA9548A) if address conflicts arise
- TB6612FNG: PWMA/PWMB from MCU 1 PWM-capable pins; STBY pin wired to GPIO (hardware motor kill)
- GC9A01 displays: share SPI MOSI/SCLK, separate CS pins; DC and RST can be shared
