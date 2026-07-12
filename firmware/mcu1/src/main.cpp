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
static const float kMotorSign = 1.0f;   // flip if robot drives away from the fall

// ---- MPU-6050 (raw I2C, no library) ----------------------------------------
static const uint8_t kMpuAddr = 0x68;

static void mpuWrite(uint8_t reg, uint8_t val) {
  Wire.beginTransmission(kMpuAddr);
  Wire.write(reg);
  Wire.write(val);
  Wire.endTransmission();
}

static bool mpuInit() {
  Wire.beginTransmission(kMpuAddr);
  if (Wire.endTransmission() != 0) return false;  // not responding
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
  Wire.beginTransmission(kMpuAddr);
  Wire.write(0x3B);  // ACCEL_XOUT_H
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom(static_cast<int>(kMpuAddr), 14) != 14) return false;
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

// cmd in [-100, 100]; sign = direction, magnitude = duty.
static void motorSet(int enPin, int chan, int inA, int inB, float cmd) {
  const bool fwd = cmd >= 0.0f;
  digitalWrite(inA, fwd ? HIGH : LOW);
  digitalWrite(inB, fwd ? LOW : HIGH);
  const float mag = fabsf(cmd);
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
static control::ComplementaryFilter g_filter(0.98f);
static control::BalanceController g_ctrl;   // defaultBalanceConfig()
static control::DriveMixer g_mixer;

static float g_gyroBias = 0.0f;
static float g_trimDeg = 0.0f;      // balance-point offset, tuned over serial
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
  delay(300);
  Serial.println("# BoomBot MCU1 - balance-only firmware");

  Wire.begin(pins::kI2c0Sda, pins::kI2c0Scl, 400000);
  motorsInit();
  motorsKill();

  if (!mpuInit()) {
    // Without an IMU there is nothing safe to do: report and halt.
    while (true) {
      Serial.println("# ERROR: MPU-6050 not found at 0x68 - check SDA=10 SCL=11 wiring");
      delay(1000);
    }
  }
  calibrateGyro();

  // Seed the filter from the accelerometer so it doesn't start at 0 by fiat.
  MpuSample s;
  if (mpuRead(s)) {
    g_filter.reset(kAngleSign * atan2f(s.ax, s.az) * RAD_TO_DEG);
  }
  Serial.println("# hold upright to arm (|angle| < 2 deg for 1 s). 'd'=disarm 'a'=allow +/-=trim");
}

static void handleSerial() {
  while (Serial.available()) {
    const char c = Serial.read();
    if (c == 'd') { g_armAllowed = false; g_armed = false; motorsKill(); Serial.println("# DISARMED"); }
    if (c == 'a') { g_armAllowed = true; Serial.println("# arming allowed"); }
    if (c == '+') { g_trimDeg += 0.1f; Serial.printf("# trim %.1f\n", g_trimDeg); }
    if (c == '-') { g_trimDeg -= 0.1f; Serial.printf("# trim %.1f\n", g_trimDeg); }
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
      if (g_uprightSinceMs == 0) g_uprightSinceMs = ms;
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
    }
  }

  // 20 Hz CSV telemetry: angle,armed (view with the Arduino serial plotter).
  const uint32_t ms = millis();
  if (ms - lastPrintMs >= 50) {
    lastPrintMs = ms;
    Serial.printf("%.2f,%d\n", theta, g_armed ? 1 : 0);
  }
}
