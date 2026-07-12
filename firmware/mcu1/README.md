# MCU 1 firmware — balance-only bring-up

Minimal first firmware: **MPU-6050 + L298N + WiFi console**. The robot stands
up. No UART link, displays, ToF, or encoders yet — those come next.

## Wireless console (no USB cable needed)

The board runs its own WiFi access point so the robot can drive untethered,
powered from the power bank:

1. Power the Lolin from the power bank (USB-A → its USB-C port).
2. On a laptop/phone, join WiFi network **`BoomBot`**, password **`boombot123`**.
3. Connect to the console: `nc 192.168.4.1 23` (macOS/Linux) or PuTTY →
   host `192.168.4.1`, port `23`, connection type **Raw** (Windows).

Same output and same keys as USB serial; both work at the same time. The
newest TCP connection wins if you reconnect.

## Wiring needed for this test

From [`docs/wiring.md`](../../docs/wiring.md), only these:

| What | Pins |
|------|------|
| GY-521 | VCC→3V3, GND→GND, SCL→GPIO10, SDA→GPIO11 (AD0/INT/XDA/XCL unconnected) |
| L298N control | ENA→GPIO5, IN1→GPIO6, IN2→GPIO7, ENB→GPIO4, IN3→GPIO12, IN4→GPIO13 |
| L298N power | VS→5V rail (+ bridge VS→5V terminal, 5V-EN jumper OFF), GND→common |
| Motors | left→OUT1/OUT2, right→OUT3/OUT4 |
| Lolin power | USB (from computer while testing) |

⚠ Common ground between Lolin, L298N, and the 5 V rail is mandatory.

## Flash (PlatformIO — recommended)

```sh
pip install platformio          # or use the VSCode PlatformIO extension
cd firmware/mcu1
pio run -t upload               # builds and flashes over USB
pio device monitor              # 115200 baud
```

If the port isn't found: hold the Lolin's **BOOT (0)** button while plugging
in USB, then retry upload.

## Flash (Arduino IDE alternative)

1. Boards manager: install **esp32 by Espressif**; select **LOLIN S3**.
2. Copy `src/main.cpp` into a sketch, and copy `firmware/common/control.h`,
   `protocol.h`, `pins.h` next to it (Arduino IDE can't include outside the
   sketch folder).
3. Upload at 115200.

## Bring-up procedure — wheels OFF the ground first

1. **Lay the robot flat and still**, power on. The gyro calibrates for ~2 s
   (serial prints the bias).
2. **Hold it upright.** When |angle| < 2° for 1 s it arms and prints `# ARMED`.
3. **Tilt it forward slightly**: the wheels must spin **forward** (the robot
   drives *under* a fall, not away from it). If they spin backward, flip
   `kMotorSign` in `src/main.cpp` and re-flash. One wheel wrong → swap that
   motor's two wires at the L298N OUT terminals.
4. Only when direction is right: wheels on the ground, hold upright, let it
   arm, release gently.

## Serial commands (115200)

| Key | Action |
|-----|--------|
| `d` | disarm immediately (motors off) |
| `t` | toggle the 10 Hz angle stream (off by default) |
| `a` | re-allow arming |
| `+` / `-` | shift the balance point by ±0.5° |
| `b` | toggle auto-trim (ON by default — learns the balance point by steering trim until average motor effort is zero; handles an off-center battery/CoM) |
| `q` / `w` | angle kp −1 / +1 (response strength) |
| `e` / `r` | angle kd −0.1 / +0.1 (damping) |
| `z` / `x` | motor deadband −2% / +2% (min duty where motors actually move) |
| `g` | print current gains/deadband/trim |

Live-tuning workflow: get it balancing (even badly), then adjust while it runs.
Too weak/slow → `w` until it catches falls; buzzing/shaking → `e`,`r` to damp or
`q` to back off kp; motors hum but don't move on small tilts → `x` to raise the
deadband; jittery at rest → `z` to lower it. Note the values from `g` when it
feels right and tell them to the project so they become the new defaults.

Output is quiet by default — only state transitions print (READY / ARMED /
FALLEN / DISARMED), because streaming over native-USB serial can stall the
balance loop. Press `t` to toggle a 10 Hz `angle,armed` CSV stream when
tuning; the Arduino serial plotter graphs it directly.

## Tuning notes

- Gains live in `defaultBalanceConfig()` (`firmware/common/control.h`) —
  sim-tuned starting points. If it oscillates fast: lower `angle.kp`. If it
  falls slowly without fighting: raise `angle.kp`. Small overshoot wobble:
  raise `angle.kd` slightly.
- If it creeps/drifts on level ground, adjust trim (`+`/`-`) until neutral,
  then hard-code the value into `g_trimDeg`.
- The robot disarms automatically past 35° (`fallenDeg`) and must be held
  upright again to re-arm.

## Safety built in

- Arms only after being held upright and steady for 1 s.
- Motors cut instantly when fallen, on any I2C read failure, and on `d`.
- Boot halts loudly if the MPU isn't found (bad SDA/SCL wiring).
