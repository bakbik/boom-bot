// BoomBot MCU 1 — minimal balance firmware.
// Scope: MPU-6050 (I2C0) + L298N + WiFi bring-up console. No UART link,
// displays, or encoders yet. Pin map: firmware/common/pins.h.
//
// Wireless console: the board runs a WiFi AP (SSID "BoomBot", pass
// "boombot123"). Connect and run `nc 192.168.4.1 23` (or PuTTY, raw mode) —
// same commands and output as USB serial, both work simultaneously.
//
// Bring-up (wheels OFF the ground first — see firmware/mcu1/README.md):
//   1. Power on with the robot lying flat and still: gyro calibrates (~2 s).
//   2. Stand it upright and hold: when |angle| < 2 deg for 1 s it arms.
//   3. Tilt forward: wheels must spin forward (driving under the fall).
// Console keys:
//   'd' disarm  'a' allow arming  't' toggle angle stream  'g' print gains
//   'q'/'w' kp -/+1   'e'/'r' kd -/+0.1   'z'/'x' deadband -/+2%
//   '+'/'-' trim +/-0.5 deg   'b' toggle balance-point auto-trim

#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>

#include <stdarg.h>

#include "control.h"
#include "pins.h"

using namespace boombot;

// ---- orientation / sign configuration -------------------------------------
static const float kAngleSign = 1.0f;   // flip if reported angle has wrong sign
static const float kGyroSign  = 1.0f;   // flip if angle drifts when rotating
static const float kMotorSign = -1.0f;  // bench-verified 2026-07-12

// ---- console: USB serial + WiFi TCP, same interface ------------------------
static const char* kApSsid = "BoomBot";
static const char* kApPass = "boombot123";
static WiFiServer g_tcpServer(23);
static WiFiClient g_tcpClient;

static void conWrite(const char* s) {
  Serial.print(s);
  if (g_tcpClient && g_tcpClient.connected()) {
    // Never block the balance loop on a slow/stale TCP connection: write only
    // what the socket can take right now, drop the rest.
    const size_t n = strlen(s);
    if (static_cast<size_t>(g_tcpClient.availableForWrite()) >= n) {
      g_tcpClient.write(reinterpret_cast<const uint8_t*>(s), n);
    }
  }
}

static void conPrintln(const char* s) {
  conWrite(s);
  conWrite("\n");
}

static void conPrintf(const char* fmt, ...) {
  char buf[192];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof buf, fmt, ap);
  va_end(ap);
  conWrite(buf);
}

static void pollTcpClient() {
  if (g_tcpServer.hasClient()) {
    if (g_tcpClient) g_tcpClient.stop();  // newest connection wins
#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
    g_tcpClient = g_tcpServer.accept();
#else
    g_tcpClient = g_tcpServer.available();
#endif
    conPrintln("# console client connected");
  }
}

// Next command byte from USB or TCP; -1 when none. Skips CR/LF and telnet
// negotiation bytes so both `nc` and telnet clients work.
static int conRead() {
  int c = -1;
  if (Serial.available()) c = Serial.read();
  else if (g_tcpClient && g_tcpClient.connected() && g_tcpClient.available())
    c = g_tcpClient.read();
  if (c == '\r' || c == '\n' || c >= 0x80) return -1;
  return c;
}

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

static int i2cScan(int sda, int scl) {
  Wire.end();
  Wire.begin(sda, scl, 400000);
  delay(20);
  int found = 0;
  conPrintf("# scan SDA=%d SCL=%d:", sda, scl);
  for (uint8_t a = 1; a < 127; ++a) {
    if (i2cPing(a)) { conPrintf(" 0x%02X", a); ++found; }
  }
  conPrintln(found ? "" : " (nothing - check VCC/GND/wires)");
  return found;
}

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
        conPrintf("# MPU found: addr 0x%02X, SDA=%d SCL=%d%s\n",
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
// 5 kHz, not 20: the L298N's slow BJT switching eats a meaningful part of
// each period at high PWM frequency, cutting effective motor voltage further.
static const int kPwmFreq = 5000;
static const int kPwmRes  = 10;     // 0..1023

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

// Motor stiction deadband: below this duty (%) the motors don't move, so
// every nonzero command is remapped into [deadband, 100]. Tune live: z/x keys.
// 15% suits the 2S-fed rail; was 25% on the weak 5 V rail.
static float g_deadbandPct = 15.0f;

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
// Gains from firmware/test/tune_gains.cpp against the measured plant (CAD:
// m=581 g, J_pitch=1.38e-3 kg*m^2, axle->CoM 30 mm, 65 mm wheels; L298N-
// limited drive). Key finding: without velocity feedback this plant is
// UNSTABLE at any gains — the pseudo-velocity estimator below is what makes
// it balanceable, not the gain values.
static control::BalanceConfig benchConfig() {
  control::BalanceConfig c = control::defaultBalanceConfig();
  c.angle.kp = 40.0f;   // tuned for the 2S drive + head extension (L=68 mm)
  c.angle.ki = 10.0f;
  c.angle.kd = 1.0f;
  return c;
}

static control::ComplementaryFilter g_filter(0.98f);
static control::BalanceController g_ctrl(benchConfig());
static control::DriveMixer g_mixer;

// Encoder-less wheel-velocity estimate from the motor command (see control.h).
// Nominal constants for the 2S-fed L298N (~5 V at the motors) + 728 g robot
// (mass incl. 2S pack + head extension, CAD 2026-07-12).
static control::VelocityEstimator g_velEst(
    control::VelocityEstimatorConfig{7.15f, 0.95f, 0.8f});

static float g_gyroBias = 0.0f;
static float g_trimDeg = -8.0f;     // bench-measured starting balance point

// Auto-trim: if the motors persistently push one way, the balance setpoint is
// wrong that way (off-center CoM, e.g. battery not centered). Slowly steer the
// trim until average motor effort is zero. 'b' toggles.
static bool g_autoTrim = true;
static const float kAutoTrimRate = 0.02f;  // deg per (motor-unit * s)
static const float kTrimLimitDeg = 15.0f;

static bool g_armAllowed = true;
static bool g_armed = false;
static uint32_t g_uprightSinceMs = 0;

static const float kArmWithinDeg = 2.0f;
static const uint32_t kArmHoldMs = 1000;
static const float kLoopDt = 0.005f;  // 200 Hz

static bool g_telemetry = false;  // 't' toggles the angle stream (off: quiet)
static float g_lastMotor = 0.0f;  // last balance output, for telemetry
static control::PidGains g_angleGains = benchConfig().angle;

static void printGains() {
  conPrintf("# kp=%.1f ki=%.1f kd=%.2f deadband=%.0f%% trim=%.1f autotrim=%s wifi_clients=%d\n",
            g_angleGains.kp, g_angleGains.ki, g_angleGains.kd,
            g_deadbandPct, g_trimDeg, g_autoTrim ? "on" : "off",
            WiFi.softAPgetStationNum());
}

static void calibrateGyro() {
  conPrintln("# calibrating gyro - keep the robot STILL...");
  float sum = 0.0f;
  int got = 0;
  for (int i = 0; i < 400; ++i) {  // ~2 s
    MpuSample s;
    if (mpuRead(s)) { sum += s.gy; ++got; }
    delay(5);
  }
  g_gyroBias = (got > 0) ? sum / got : 0.0f;
  conPrintf("# gyro bias: %.3f deg/s (%d samples)\n", g_gyroBias, got);
}

void setup() {
  Serial.begin(115200);
#if defined(ARDUINO_USB_CDC_ON_BOOT) && ARDUINO_USB_CDC_ON_BOOT
  // Native-USB serial blocks when the host isn't draining the buffer, which
  // would stall the 200 Hz balance loop. Drop bytes instead of waiting.
  Serial.setTxTimeoutMs(0);
#endif
  delay(300);

  WiFi.mode(WIFI_AP);
  const bool apOk = WiFi.softAP(kApSsid, kApPass, /*channel=*/1,
                                /*hidden=*/0, /*max_conn=*/4);
  g_tcpServer.begin();
  g_tcpServer.setNoDelay(true);

  conPrintln("# BoomBot MCU1 - balance firmware (WiFi console)");
  conPrintf("# WiFi AP '%s' pass '%s' -> raw TCP %s:23 (PuTTY raw / nc)  [AP start: %s]\n",
            kApSsid, kApPass, WiFi.softAPIP().toString().c_str(),
            apOk ? "OK" : "FAILED");

  Wire.begin(pins::kI2c0Sda, pins::kI2c0Scl, 400000);
  motorsInit();
  motorsKill();

  if (!mpuInit()) {
    // Keep scanning and reporting so wiring can be fixed live, no reflash.
    while (true) {
      conPrintln("# ERROR: no MPU at 0x68/0x69 on either pin orientation.");
      i2cScan(pins::kI2c0Sda, pins::kI2c0Scl);
      i2cScan(pins::kI2c0Scl, pins::kI2c0Sda);
      conPrintln("# check: GY-521 power LED lit? VCC->3V3, GND->GND, SDA->11, SCL->10. retrying in 3 s...");
      pollTcpClient();
      delay(3000);
      if (mpuInit()) break;
    }
    conPrintln("# MPU recovered - continuing boot");
  }
  calibrateGyro();

  MpuSample s;
  if (mpuRead(s)) {
    g_filter.reset(kAngleSign * atan2f(s.ax, s.az) * RAD_TO_DEG);
  }
  printGains();
  conPrintln("# hold upright to arm (|angle| < 2 deg for 1 s)");
}

static void handleConsole() {
  for (int c = conRead(); c >= 0; c = conRead()) {
    bool gainsChanged = false;
    if (c == 'd') { g_armAllowed = false; g_armed = false; motorsKill(); conPrintln("# DISARMED"); }
    if (c == 'a') { g_armAllowed = true; conPrintln("# READY - hold upright to arm"); }
    if (c == 't') { g_telemetry = !g_telemetry; conPrintln(g_telemetry ? "# telemetry ON" : "# telemetry OFF"); }
    if (c == '+') { g_trimDeg += 0.5f; printGains(); }
    if (c == '-') { g_trimDeg -= 0.5f; printGains(); }
    if (c == 'b') { g_autoTrim = !g_autoTrim; printGains(); }
    if (c == 'q') { g_angleGains.kp -= 1.0f; gainsChanged = true; }
    if (c == 'w') { g_angleGains.kp += 1.0f; gainsChanged = true; }
    if (c == 'e') { g_angleGains.kd -= 0.1f; gainsChanged = true; }
    if (c == 'r') { g_angleGains.kd += 0.1f; gainsChanged = true; }
    if (c == 'z') { g_deadbandPct -= 2.0f; if (g_deadbandPct < 0) g_deadbandPct = 0; printGains(); }
    if (c == 'x') { g_deadbandPct += 2.0f; if (g_deadbandPct > 60) g_deadbandPct = 60; printGains(); }
    if (c == 'g') { printGains(); }
    if (c == 'm') {
      // Drive-train step test: discriminates electrical weakness from control
      // problems. If full duty spins up slowly, no gain tuning can help.
      if (g_armed) {
        conPrintln("# step test refused while ARMED - press 'd' first");
      } else {
        conPrintln("# STEP TEST: full duty 0.6 s - WHEELS UP!");
        control::WheelCommand full{100.0f, 100.0f};
        motorsApply(full);
        delay(600);
        motorsKill();
        conPrintln("# step test done (snappy strong spin-up = drive OK; sluggish = electrical)");
      }
    }
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
  // If something stalled us (WiFi event, TCP hiccup), resync instead of
  // replaying the missed ticks back-to-back with a wrong dt.
  if (static_cast<int32_t>(now - nextTickUs) > 50000) nextTickUs = now + 5000;

  // Network housekeeping is not worth doing 200x per second.
  static uint32_t lastNetMs = 0;
  const uint32_t msNow = millis();
  if (msNow - lastNetMs >= 100) {
    lastNetMs = msNow;
    pollTcpClient();
  }
  handleConsole();

  MpuSample s;
  if (!mpuRead(s)) {  // I2C hiccup: safest is motors off until data returns
    motorsKill();
    return;
  }

  const float accelAngle = kAngleSign * atan2f(s.ax, s.az) * RAD_TO_DEG;
  const float rate = kGyroSign * (s.gy - g_gyroBias);
  const float angle = g_filter.update(accelAngle, rate, kLoopDt);
  const float theta = angle - g_trimDeg;

  if (!g_armed) {
    motorsKill();
    const uint32_t ms = millis();
    if (g_armAllowed && fabsf(theta) < kArmWithinDeg) {
      if (g_uprightSinceMs == 0) {
        g_uprightSinceMs = ms;
        conPrintln("# READY - hold steady...");
      }
      if (ms - g_uprightSinceMs >= kArmHoldMs) {
        g_armed = true;
        g_ctrl.reset();
        g_velEst.reset();
        conPrintln("# ARMED - balancing");
      }
    } else {
      g_uprightSinceMs = 0;
    }
  } else {
    if (g_ctrl.fallen(theta)) {
      g_armed = false;
      g_uprightSinceMs = 0;
      motorsKill();
      conPrintln("# FALLEN - disarmed (stand upright to re-arm)");
    } else {
      // No encoders yet: pseudo-velocity from the motor model closes the
      // outer loop — without it this drive cannot stabilize the robot.
      const float motor =
          g_ctrl.update(theta, g_velEst.velocity(), 0.0f, kLoopDt);
      g_velEst.update(motor, kLoopDt);
      g_lastMotor = motor;
      motorsApply(g_mixer.mix(motor, proto::Direction::Stop, 0));

      // Balance-point learner: persistent backward drive means the setpoint
      // is behind the true equilibrium (raise trim), and vice versa. Learn
      // only near equilibrium — adapting during a big transient (push, catch,
      // pickup) erodes a good trim with garbage.
      if (g_autoTrim && fabsf(theta) < 10.0f && fabsf(rate) < 60.0f) {
        g_trimDeg -= kAutoTrimRate * motor * kLoopDt;
        g_trimDeg = control::clampf(g_trimDeg, -kTrimLimitDeg, kTrimLimitDeg);
      }
    }
  }

  // Loop-rate accounting (real control frequency, not nominal).
  static uint32_t loopCount = 0;
  static uint32_t lastRateMs = 0;
  static uint32_t loopHz = 0;
  ++loopCount;
  if (msNow - lastRateMs >= 1000) {
    loopHz = loopCount * 1000 / (msNow - lastRateMs);
    loopCount = 0;
    lastRateMs = msNow;
  }

  // Quiet by default; 't' toggles a 10 Hz stream for tuning:
  //   angle, last motor cmd, velocity estimate, actual loop Hz
  if (g_telemetry) {
    if (msNow - lastPrintMs >= 100) {
      lastPrintMs = msNow;
      conPrintf("%.2f,%.0f,%.2f,%u\n", theta, g_lastMotor,
                g_velEst.velocity(), loopHz);
    }
  }
}
