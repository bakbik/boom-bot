// BoomBot balance & drive control — the real-time dynamics that run on MCU 1.
//
// Hardware-independent so it can be unit-tested against a plant model on a host
// (see firmware/test/test_control.cpp) and reused verbatim on the ESP32. No
// Arduino deps, no allocation, no floating-point surprises beyond basic math.
//
// Conventions:
//   angle  : degrees, tilt from vertical, POSITIVE = leaning forward
//   rate   : degrees/second (gyro)
//   vel    : meters/second, base translation, POSITIVE = forward
//   motor  : command in [-100, 100], POSITIVE = drive wheels forward
#ifndef BOOMBOT_CONTROL_H
#define BOOMBOT_CONTROL_H

#include <math.h>
#include <stdint.h>
#include "protocol.h"

namespace boombot {
namespace control {

inline float clampf(float v, float lo, float hi) {
  return v < lo ? lo : (v > hi ? hi : v);
}

// Complementary filter: fuse the accelerometer's absolute (but noisy) angle
// with the gyro's clean (but drifting) rate into a stable tilt estimate.
class ComplementaryFilter {
 public:
  explicit ComplementaryFilter(float alpha = 0.98f) : alpha_(alpha) {}

  // accelAngle in deg, gyroRate in deg/s, dt in seconds.
  float update(float accelAngle, float gyroRate, float dt) {
    angle_ = alpha_ * (angle_ + gyroRate * dt) + (1.0f - alpha_) * accelAngle;
    return angle_;
  }
  float angle() const { return angle_; }
  void reset(float a = 0.0f) { angle_ = a; }

 private:
  float alpha_;
  float angle_ = 0.0f;
};

struct PidGains { float kp, ki, kd; };

// PID with output clamping, integral anti-windup (integral is bounded so its
// contribution never exceeds the output range), and derivative-on-measurement
// to avoid a kick when the setpoint changes.
class Pid {
 public:
  Pid(PidGains g, float outMin, float outMax)
      : g_(g), outMin_(outMin), outMax_(outMax) {}

  void reset() { integ_ = 0.0f; prevMeas_ = 0.0f; first_ = true; }
  void setGains(PidGains g) { g_ = g; }

  float update(float setpoint, float measurement, float dt) {
    const float err = setpoint - measurement;

    // Integrate, then bound the integral term to the output range.
    integ_ += err * dt;
    if (g_.ki > 1e-6f) {
      const float iMax = outMax_ / g_.ki;
      const float iMin = outMin_ / g_.ki;
      integ_ = clampf(integ_, iMin, iMax);
    }

    const float deriv = first_ ? 0.0f : (measurement - prevMeas_) / dt;
    prevMeas_ = measurement;
    first_ = false;

    const float out = g_.kp * err + g_.ki * integ_ - g_.kd * deriv;
    return clampf(out, outMin_, outMax_);
  }

 private:
  PidGains g_;
  float outMin_, outMax_;
  float integ_ = 0.0f;
  float prevMeas_ = 0.0f;
  bool first_ = true;
};

struct BalanceConfig {
  PidGains angle;      // inner loop: tilt -> motor
  PidGains velocity;   // outer loop: speed error -> target tilt
  float maxLeanDeg;    // clamp on the outer loop's target tilt
  float motorLimit;    // |motor| ceiling
  float fallenDeg;     // beyond this tilt, balancing is hopeless -> report fallen
};

inline BalanceConfig defaultBalanceConfig() {
  BalanceConfig c;
  c.angle = PidGains{12.0f, 8.0f, 0.9f};
  c.velocity = PidGains{9.0f, 4.0f, 0.0f};
  c.maxLeanDeg = 6.0f;
  c.motorLimit = 100.0f;
  c.fallenDeg = 35.0f;
  return c;
}

// Cascade balance controller.
//   outer loop : drives base velocity toward target by biasing the tilt setpoint
//   inner loop : drives tilt toward that setpoint via the wheels
// A forward lean makes the base accelerate forward, so to go faster we lean in.
class BalanceController {
 public:
  explicit BalanceController(const BalanceConfig& c = defaultBalanceConfig())
      : cfg_(c),
        anglePid_(c.angle, -c.motorLimit, c.motorLimit),
        velPid_(c.velocity, -c.maxLeanDeg, c.maxLeanDeg) {}

  void reset() { anglePid_.reset(); velPid_.reset(); }

  // theta: estimated tilt (deg). vel: measured base velocity (m/s).
  // targetVel: desired base velocity (m/s). Returns the common motor term.
  float update(float theta, float vel, float targetVel, float dt) {
    if (fabsf(theta) > cfg_.fallenDeg) return 0.0f;  // fallen: cut drive
    const float targetAngle = velPid_.update(targetVel, vel, dt);
    // Negate: a positive forward tilt needs a positive (forward) motor command.
    return -anglePid_.update(targetAngle, theta, dt);
  }

  bool fallen(float theta) const { return fabsf(theta) > cfg_.fallenDeg; }
  const BalanceConfig& config() const { return cfg_; }

 private:
  BalanceConfig cfg_;
  Pid anglePid_;
  Pid velPid_;
};

// Per-wheel motor commands, each in [-100, 100].
struct WheelCommand { float left, right; };

struct DriveConfig {
  float maxSpeed;   // m/s corresponding to a full-speed F/B command
  float turnGain;   // motor-unit differential for a full-speed L/R spin
};

inline DriveConfig defaultDriveConfig() { return DriveConfig{0.6f, 60.0f}; }

// Translates a protocol Drive command into (a) a target velocity for the
// balance outer loop and (b) a left/right differential mixed onto the balance
// output. Turning is a pure differential — the balance term keeps the robot up.
class DriveMixer {
 public:
  explicit DriveMixer(const DriveConfig& c = defaultDriveConfig()) : cfg_(c) {}

  float targetVelocity(proto::Direction dir, uint8_t speed) const {
    const float s = clampf(speed / 100.0f, 0.0f, 1.0f);
    switch (dir) {
      case proto::Direction::Forward:  return  cfg_.maxSpeed * s;
      case proto::Direction::Backward: return -cfg_.maxSpeed * s;
      default:                          return 0.0f;  // L/R/S: no translation
    }
  }

  WheelCommand mix(float balanceMotor, proto::Direction dir,
                   uint8_t speed) const {
    const float s = clampf(speed / 100.0f, 0.0f, 1.0f);
    // Spin left = left wheel back, right wheel forward (turn counter-clockwise).
    float turn = 0.0f;
    if (dir == proto::Direction::Left)  turn =  cfg_.turnGain * s;
    if (dir == proto::Direction::Right) turn = -cfg_.turnGain * s;
    WheelCommand w;
    w.left  = clampf(balanceMotor - turn, -100.0f, 100.0f);
    w.right = clampf(balanceMotor + turn, -100.0f, 100.0f);
    return w;
  }

 private:
  DriveConfig cfg_;
};

}  // namespace control
}  // namespace boombot

#endif  // BOOMBOT_CONTROL_H
