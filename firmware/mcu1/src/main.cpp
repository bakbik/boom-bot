// BoomBot MCU 1 — minimal balance firmware.
// Scope: MPU-6050 (I2C0) + L298N only. No UART link, no displays, no encoders
// yet — the robot just stands up. Pin map: firmware/common/pins.h.
//
// Bring-up (wheels OFF the ground first — see firmware/mcu1/README.md):
//   1. Power on with the robot lying flat and still: gyro calibrates (~2 s).
//   2. Stand it upright and hold: when |angle| < 2 deg for 1 s it arms.
//   3. Tilt forward: wheels must spin forward (driving under the fall).
//      If they spin backward, flip kMotorSign below.
// Serial (115200): streams "angle,motor" CSV at 20 Hz for tuning plots.
//   'd' = disarm now   'a' = allow arming   '+'/'-' = trim balance point 0.1 deg

#include <Arduino.h>
#include <Wire.h>

#include "control.h"
#include "pins.h"

using namespace boombot;

// ---- orientation / sign configuration -------------------------------------
// Assumes the GY-521 is mounted flat, X axis pointing forward. If your
// mounting differs, fix signs here, not in control.h.
static const float kAngleSign = 1.0f;   // flip if reported angle has wrong sign
static const float kGyroSign  = 1.0f;   // flip if angle drifts when rotating
static const float kMotorSign = -1.0f;  // bench-verified 2026-07-12: +1 drove away from the fall

// ---- MPU-6050 (raw I2C, no library) ----------------------------------------
static uint8_t g_mpuAddr = 0x68;  // discovered at boot (0x68 or 0x69)

static void mpuWrite(uint8_t reg, uint8_t val) {
  Wire.beginTransmission(g_mpuAddr);
  Wire.write(reg);
  Wire.write(val);
  Wire.endTransmission();
}

static bool i2cPing(uint8_t addr) {
  Wire.beginTransmission(addr);
  return Wire.endTransmission() == 0;
}

// Scan the whole bus and print every responding address.
static int i2cScan(int sda, int scl) {
  Wire.end();
  Wire.begin(sda, scl, 400000);
  delay(20);
  int found = 0;
  Serial.printf("# scan SDA=%d SCL=%d:", sda, scl);
  for (uint8_t a = 1; a < 127; ++a) {
    if (i2cPing(a)) { Serial.printf(" 0x%02X", a); ++found; }
  }
  Serial.println(found ? "" : " (nothing - check VCC/GND/wires)");
  return found;
}

// Try both pin orientations and both MPU addresses; adopt whatever answers.
static bool mpuProbe() {
  const int orient[2][2] = {{pins::kI2c0Sda, pins::kI2c0Scl},
                            {pins::kI2c0Scl, pins::kI2c0Sda}};
  for (int o = 0; o < 2; ++o) {
    Wire.end();
    Wire.begin(orient[o][0], orient[o][1], 400000);
    delay(20);
    for (uint8_t addr = 0x68; addr <= 0x69; ++addr) {
      if (i2cPing(addr)) {
        g_mpuAddr = addr;
        Serial.printf("# MPU found: addr 0x%02X, SDA=%d SCL=%d%s\n",
                      addr, orient[o][0], orient[o][1],
                      o == 1 ? "  (PINS SWAPPED vs pins.h - fix wiring or pins.h)"
                             : "");
        return true;
      }
    }
  }
  return false;
}

static bool mpuInit() {
  if (!mpuProbe()) return false;
  mpuWrite(0x6B, 0x01);  // PWR_MGMT_1: wake, clock = gyro X PLL
  mpuWrite(0x1A, 0x03);  // CONFIG: DLPF 44 Hz accel / 42 Hz gyro
  mpuWrite(0x1B, 0x08);  // GYRO_CONFIG: +/-500 dps  (65.5 LSB per dps)
  mpuWrite(0x1C, 0x00);  // ACCEL_CONFIG: +/-2 g     (16384 LSB per g)
  return true;
}

struct MpuSample {
  float ax, ay, az;  // g
  float gy;          // pitch rate, deg/s (rotation about Y)
};

static bool mpuRead(MpuSample& s) {
  Wire.beginTransmission(g_mpuAddr);
  Wire.write(0x3B);  // ACCEL_XOUT_H
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom(static_cast<int>(g_mpuAddr), 14) != 14) return false;
  int16_t raw[7];
  for (int i = 0; i < 7; ++i) {
    raw[i] = (Wire.read() << 8) | Wire.read();
  }
  s.ax = raw[0] / 16384.0f;
  s.ay = raw[1] / 16384.0f;
  s.az = raw[2] / 16384.0f;
  // raw[3] = temperature, raw[4..6] = gyro X/Y/Z
  s.gy = raw[5] / 65.5f;
  return true;
}

// ---- L298N ------------------------------------------------------------------
static const int kPwmFreq = 20000;  // above audible
static const int kPwmRes  = 10;     // 0..1023

// Arduino-ESP32 core 3.x renamed the LEDC API (ledcSetup/ledcAttachPin ->
// ledcAttach, channel-based write -> pin-based write). Support both.
#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
static void pwmInitPin(int pin, int /*channel*/) { ledcAttach(pin, kPwmFreq, kPwmRes); }
static void pwmWritePin(int pin, int /*channel*/, uint32_t duty) { ledcWrite(pin, duty); }
#else
static void pwmInitPin(int pin, int channel) {
  ledcSetup(channel, kPwmFreq, kPwmRes);
  ledcAttachPin(pin, channel);
}
static void pwmWritePin(int /*pin*/, int channel, uint32_t duty) { ledcWrite(channel, duty); }
#endif

static const int kPwmChanLeft = 0;
static const int kPwmChanRight = 1;

static void motorsInit() {
  pinMode(pins::kMotorIn1, OUTPUT);
  pinMode(pins::kMotorIn2, OUTPUT);
  pinMode(pins::kMotorIn3, OUTPUT);
  pinMode(pins::kMotorIn4, OUTPUT);
  pwmInitPin(pins::kMotorEnLeft, kPwmChanLeft);
  pwmInitPin(pins::kMotorEnRight, kPwmChanRight);
  pwmWritePin(pins::kMotorEnLeft, kPwmChanLeft, 0);
  pwmWritePin(pins::kMotorEnRight, kPwmChanRight, 0);
}

// L298N+N20 deadband: below this duty (%) the motors don't move at all, so
// every nonzero command is remapped into [deadband, 100]. Tune live: z/x keys.
static float g_deadbandPct = 25.0f;

// cmd in [-100, 100]; sign = direction, magnitude = duty.
static void motorSet(int enPin, int chan, int inA, int inB, float cmd) {
  const bool fwd = cmd >= 0.0f;
  digitalWrite(inA, fwd ? HIGH : LOW);
  digitalWrite(inB, fwd ? LOW : HIGH);
  float mag = fabsf(cmd);
  if (mag > 0.5f) {  // remap past the stiction deadband; tiny commands stay off
    mag = g_deadbandPct + mag * (100.0f - g_deadbandPct) / 100.0f;
    if (mag > 100.0f) mag = 100.0f;
  } else {
    mag = 0.0f;
  }
  pwmWritePin(enPin, chan, static_cast<uint32_t>(mag * 1023.0f / 100.0f));
}

static void motorsApply(const control::WheelCommand& w) {
  motorSet(pins::kMotorEnLeft, kPwmChanLeft, pins::kMotorIn1, pins::kMotorIn2,
           kMotorSign * w.left);
  motorSet(pins::kMotorEnRight, kPwmChanRight, pins::kMotorIn3, pins::kMotorIn4,
           kMotorSign * w.right);
}

static void motorsKill() {
  pwmWritePin(pins::kMotorEnLeft, kPwmChanLeft, 0);
  pwmWritePin(pins::kMotorEnRight, kPwmChanRight, 0);
}

// ---- state -------------------------------------------------------------------
// Bench gains: hotter than the sim defaults because the real L298N+N20 drive
// has far less authority than the simulated plant. Tune live: q/w e/r keys.
static control::BalanceConfig benchConfig() {
  control::BalanceConfig c = control::defaultBalanceConfig();
  c.angle.kp = 18.0f;
  c.angle.kd = 1.2f;
  return c;
}

static control::ComplementaryFilter g_filter(0.98f);
static control::BalanceController g_ctrl(benchConfig());
static control::DriveMixer g_mixer;


static float g_gyroBias = 0.0f;
static float g_trimDeg = 0.0f;      // balance-point offset (auto-learned + manual)

// Auto-trim: if the motors persistently push one way, the balance setpoint is
// wrong that way (off-center CoM, e.g. battery not centered). Slowly steer the
// trim until average motor effort is zero. 'b' toggles.
static bool g_autoTrim = true;
static const float kAutoTrimRate = 0.02f;  // deg per (motor-unit * s)
static const float kTrimLimitDeg = 10.0f;
static bool g_armAllowed = true;
static bool g_armed = false;
static uint32_t g_uprightSinceMs = 0;

static const float kArmWithinDeg = 2.0f;
static const uint32_t kArmHoldMs = 1000;
static const float kLoopDt = 0.005f;  // 200 Hz

static void calibrateGyro() {
  Serial.println("# calibrating gyro - keep the robot STILL...");
  float sum = 0.0f;
  int got = 0;
  for (int i = 0; i < 400; ++i) {  // ~2 s
    MpuSample s;
    if (mpuRead(s)) { sum += s.gy; ++got; }
    delay(5);
  }
  g_gyroBias = (got > 0) ? sum / got : 0.0f;
  Serial.printf("# gyro bias: %.3f deg/s (%d samples)\n", g_gyroBias, got);
}

void setup() {
  Serial.begin(115200);
#if defined(ARDUINO_USB_CDC_ON_BOOT) && ARDUINO_USB_CDC_ON_BOOT
  // Native-USB serial blocks when the host isn't draining the buffer, which
  // would stall the 200 Hz balance loop. Drop bytes instead of waiting.
  Serial.setTxTimeoutMs(0);
#endif
  delay(300);
  Serial.println("# BoomBot MCU1 - balance-only firmware");

  Wire.begin(pins::kI2c0Sda, pins::kI2c0Scl, 400000);
  motorsInit();
  motorsKill();

  if (!mpuInit()) {
    // Without an IMU there is nothing safe to do: keep scanning and reporting
    // so the wiring can be debugged live (rewire, watch, no reflash needed).
    while (true) {
      Serial.println("# ERROR: no MPU at 0x68/0x69 on either pin orientation.");
      i2cScan(pins::kI2c0Sda, pins::kI2c0Scl);
      i2cScan(pins::kI2c0Scl, pins::kI2c0Sda);
      Serial.println("# check: GY-521 power LED lit? VCC->3V3, GND->GND, SDA->11, SCL->10. retrying in 3 s...");
      delay(3000);
      if (mpuInit()) break;  // recovered after rewiring
    }
    Serial.println("# MPU recovered - continuing boot");
  }
  calibrateGyro();

  // Seed the filter from the accelerometer so it doesn't start at 0 by fiat.
  MpuSample s;
  if (mpuRead(s)) {
    g_filter.reset(kAngleSign * atan2f(s.ax, s.az) * RAD_TO_DEG);
  }
  Serial.println("# hold upright to arm (|angle| < 2 deg for 1 s). 'd'=disarm 'a'=allow +/-=trim");
}

static bool g_telemetry = false;  // 't' toggles the angle stream (off: quiet)
static control::PidGains g_angleGains = benchConfig().angle;

static void printGains() {
  Serial.printf("# kp=%.1f ki=%.1f kd=%.2f deadband=%.0f%% trim=%.1f\n",
                g_angleGains.kp, g_angleGains.ki, g_angleGains.kd,
                g_deadbandPct, g_trimDeg);
}

static void handleSerial() {
  while (Serial.available()) {
    const char c = Serial.read();
    bool gainsChanged = false;
    if (c == 'd') { g_armAllowed = false; g_armed = false; motorsKill(); Serial.println("# DISARMED"); }
    if (c == 'a') { g_armAllowed = true; Serial.println("# READY - hold upright to arm"); }
    if (c == 't') { g_telemetry = !g_telemetry; Serial.println(g_telemetry ? "# telemetry ON" : "# telemetry OFF"); }
    if (c == '+') { g_trimDeg += 0.5f; printGains(); }
    if (c == '-') { g_trimDeg -= 0.5f; printGains(); }
    if (c == 'b') { g_autoTrim = !g_autoTrim; Serial.println(g_autoTrim ? "# auto-trim ON" : "# auto-trim OFF"); }
    if (c == 'q') { g_angleGains.kp -= 1.0f; gainsChanged = true; }
    if (c == 'w') { g_angleGains.kp += 1.0f; gainsChanged = true; }
    if (c == 'e') { g_angleGains.kd -= 0.1f; gainsChanged = true; }
    if (c == 'r') { g_angleGains.kd += 0.1f; gainsChanged = true; }
    if (c == 'z') { g_deadbandPct -= 2.0f; if (g_deadbandPct < 0) g_deadbandPct = 0; printGains(); }
    if (c == 'x') { g_deadbandPct += 2.0f; if (g_deadbandPct > 60) g_deadbandPct = 60; printGains(); }
    if (c == 'g') { printGains(); }
    if (gainsChanged) {
      if (g_angleGains.kp < 0) g_angleGains.kp = 0;
      if (g_angleGains.kd < 0) g_angleGains.kd = 0;
      g_ctrl.setAngleGains(g_angleGains);
      printGains();
    }
  }
}

void loop() {
  static uint32_t nextTickUs = micros();
  static uint32_t lastPrintMs = 0;

  // Fixed-rate 200 Hz tick.
  const uint32_t now = micros();
  if (static_cast<int32_t>(now - nextTickUs) < 0) return;
  nextTickUs += 5000;

  handleSerial();

  MpuSample s;
  if (!mpuRead(s)) {  // I2C hiccup: safest is motors off until data returns
    motorsKill();
    return;
  }

  const float accelAngle = kAngleSign * atan2f(s.ax, s.az) * RAD_TO_DEG;
  const float rate = kGyroSign * (s.gy - g_gyroBias);
  const float angle = g_filter.update(accelAngle, rate, kLoopDt);
  const float theta = angle - g_trimDeg;

  // Arming: require the robot held upright and steady.
  if (!g_armed) {
    motorsKill();
    const uint32_t ms = millis();
    if (g_armAllowed && fabsf(theta) < kArmWithinDeg) {
      if (g_uprightSinceMs == 0) {
        g_uprightSinceMs = ms;
        Serial.println("# READY - hold steady...");
      }
      if (ms - g_uprightSinceMs >= kArmHoldMs) {
        g_armed = true;
        g_ctrl.reset();
        Serial.println("# ARMED - balancing");
      }
    } else {
      g_uprightSinceMs = 0;
    }
  } else {
    if (g_ctrl.fallen(theta)) {
      g_armed = false;
      g_uprightSinceMs = 0;
      motorsKill();
      Serial.println("# FALLEN - disarmed (stand upright to re-arm)");
    } else {
      // No encoders yet: measured velocity 0, target velocity 0 -> angle-only.
      const float motor = g_ctrl.update(theta, 0.0f, 0.0f, kLoopDt);
      motorsApply(g_mixer.mix(motor, proto::Direction::Stop, 0));

      // Balance-point learner: persistent backward drive means the setpoint
      // is behind the true equilibrium (raise trim), and vice versa.
      if (g_autoTrim) {
        g_trimDeg -= kAutoTrimRate * motor * kLoopDt;
        g_trimDeg = control::clampf(g_trimDeg, -kTrimLimitDeg, kTrimLimitDeg);
      }
    }
  }

  // Quiet by default; 't' toggles a 10 Hz angle stream for gain tuning.
  if (g_telemetry) {
    const uint32_t ms = millis();
    if (ms - lastPrintMs >= 100) {
      lastPrintMs = ms;
      Serial.printf("%.2f,%d\n", theta, g_armed ? 1 : 0);
    }
  }
}
