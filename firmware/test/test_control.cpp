// Host tests for the balance & drive control (firmware/common/control.h).
//
// The balance controller is exercised against a nonlinear inverted-pendulum-
// on-wheels plant model — the sim proves the robot recovers from a tilt, holds
// upright, and tracks a drive command without falling. No hardware needed.
//   make -C firmware/test        # builds and runs
#include "../common/control.h"

#include <cmath>
#include <cstdio>

using namespace boombot;
using namespace boombot::control;

static int g_fails = 0, g_checks = 0;
#define CHECK(cond)                                                        \
  do {                                                                     \
    ++g_checks;                                                            \
    if (!(cond)) {                                                         \
      ++g_fails;                                                           \
      std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);         \
    }                                                                      \
  } while (0)

// ---- plant model ----------------------------------------------------------
// Inverted pendulum whose pivot (the wheel axle) is driven horizontally.
//   theta'' = (g*sin(theta) - a*cos(theta)) / L      (a = pivot acceleration)
// with viscous friction on the base velocity. State is SI (radians, m).
struct Plant {
  double theta = 0.0;   // rad from vertical, + forward
  double omega = 0.0;   // rad/s
  double vel = 0.0;     // m/s base velocity
  double pos = 0.0;     // m base position

  static constexpr double kG = 9.81;
  static constexpr double kL = 0.09;   // effective pendulum length (m)
  static constexpr double kAmax = 30.0;  // base accel at |motor|=100 (m/s^2)
  static constexpr double kFric = 1.0;   // viscous friction (1/s)

  // Advance one step given a motor command in [-100, 100].
  void step(double motor, double dt) {
    const double a = (motor / 100.0) * kAmax;  // commanded pivot accel
    const double thetaDot2 =
        (kG * std::sin(theta) - a * std::cos(theta)) / kL;
    omega += thetaDot2 * dt;
    theta += omega * dt;
    vel += (a - kFric * vel) * dt;
    pos += vel * dt;
  }

  double angleDeg() const { return theta * 180.0 / M_PI; }
  double rateDeg() const { return omega * 180.0 / M_PI; }
};

// ---- scenario 1: recover from an initial tilt and hold upright ------------

static void test_balance_recovery() {
  Plant p;
  p.theta = 8.0 * M_PI / 180.0;  // start tilted 8 deg forward
  BalanceController ctrl;
  ComplementaryFilter filt;
  filt.reset(p.angleDeg());

  const double dt = 0.005;  // 200 Hz balance loop (matches spec)
  double maxTilt = 0.0;
  double settledTilt = 0.0, settledVel = 0.0;

  for (int i = 0; i < 800; ++i) {  // 4 s
    const double est = filt.update(p.angleDeg(), p.rateDeg(), dt);
    const double motor = ctrl.update(est, p.vel, /*targetVel=*/0.0, dt);
    p.step(motor, dt);
    const double a = std::fabs(p.angleDeg());
    if (a > maxTilt) maxTilt = a;
    if (i >= 500) {  // after 2.5 s, expect it settled
      settledTilt = a;
      settledVel = std::fabs(p.vel);
    }
  }

  CHECK(maxTilt < 25.0);       // never came close to falling
  CHECK(settledTilt < 1.5);    // upright at the end
  CHECK(settledVel < 0.2);     // roughly stationary
  std::printf("  recovery: maxTilt=%.2f deg, final tilt=%.2f deg, vel=%.3f m/s\n",
              maxTilt, settledTilt, settledVel);
}

// ---- scenario 2: track a forward drive command while balancing ------------

static void test_drive_tracking() {
  Plant p;
  BalanceController ctrl;
  ComplementaryFilter filt;
  DriveMixer mixer;

  const double dt = 0.005;
  const uint8_t speed = 50;  // half speed forward
  const double targetVel = mixer.targetVelocity(proto::Direction::Forward, speed);
  double maxTilt = 0.0, finalVel = 0.0;

  for (int i = 0; i < 1600; ++i) {  // 8 s
    const double est = filt.update(p.angleDeg(), p.rateDeg(), dt);
    const double motor = ctrl.update(est, p.vel, targetVel, dt);
    p.step(motor, dt);
    if (std::fabs(p.angleDeg()) > maxTilt) maxTilt = std::fabs(p.angleDeg());
    finalVel = p.vel;
  }

  CHECK(maxTilt < 15.0);                       // stayed up while accelerating
  CHECK(std::fabs(finalVel - targetVel) < 0.08);  // reached commanded speed
  std::printf("  drive: target=%.3f m/s, reached=%.3f m/s, maxTilt=%.2f deg\n",
              targetVel, finalVel, maxTilt);
}

// ---- unit checks: filter, PID clamp, drive mixer --------------------------

static void test_complementary_filter() {
  ComplementaryFilter f(0.98f);
  f.reset(0.0f);
  // Steady accel reading with zero gyro should pull the estimate toward it.
  for (int i = 0; i < 500; ++i) f.update(10.0f, 0.0f, 0.005f);
  CHECK(std::fabs(f.angle() - 10.0f) < 0.5f);
}

static void test_pid_clamp() {
  Pid pid(PidGains{100.0f, 50.0f, 0.0f}, -100.0f, 100.0f);
  // A huge sustained error must not wind the output past the clamp.
  float out = 0.0f;
  for (int i = 0; i < 1000; ++i) out = pid.update(1000.0f, 0.0f, 0.01f);
  CHECK(out <= 100.0f && out >= -100.0f);
  CHECK(out == 100.0f);
  // After the error flips, anti-windup lets it recover promptly (not stuck high).
  float out2 = out;
  for (int i = 0; i < 50; ++i) out2 = pid.update(-1000.0f, 0.0f, 0.01f);
  CHECK(out2 == -100.0f);
}

static void test_drive_mixer() {
  DriveMixer m;
  CHECK(m.targetVelocity(proto::Direction::Forward, 100) > 0.0f);
  CHECK(m.targetVelocity(proto::Direction::Backward, 100) < 0.0f);
  CHECK(m.targetVelocity(proto::Direction::Left, 100) == 0.0f);
  CHECK(m.targetVelocity(proto::Direction::Stop, 0) == 0.0f);

  // Pure forward: wheels share the balance term equally.
  WheelCommand f = m.mix(40.0f, proto::Direction::Forward, 80);
  CHECK(std::fabs(f.left - f.right) < 1e-4f);

  // Spin left: left wheel slower/reverse relative to right.
  WheelCommand l = m.mix(0.0f, proto::Direction::Left, 100);
  CHECK(l.left < l.right);

  // Balance term is preserved and clamped.
  WheelCommand r = m.mix(120.0f, proto::Direction::Stop, 0);
  CHECK(r.left == 100.0f && r.right == 100.0f);
}

int main() {
  test_complementary_filter();
  test_pid_clamp();
  test_drive_mixer();
  std::printf("balance simulation:\n");
  test_balance_recovery();
  test_drive_tracking();

  std::printf("\n%d checks, %d failures\n", g_checks, g_fails);
  if (g_fails == 0) std::printf("ALL TESTS PASSED\n");
  return g_fails == 0 ? 0 : 1;
}
