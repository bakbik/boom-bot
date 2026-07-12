// Gain tuner for the real BoomBot hardware (2026-07-12 CAD + bench data).
//   make -C firmware/test tune     # grid-searches PID gains, prints the best
//
// Plant parameters come from measurements, not guesses:
//   mass        580.57 g            (CAD mass properties, Assem1)
//   J_pitch     1.379e-3 kg*m^2     (CAD principal moment about the axle axis)
//   wheel r     32.5 mm             (65 mm wheels, BOM)
//   L           30 mm               (axle->CoM, CAD-verified)
//   drive       L298N at 5 V rail: ~3 V effective at the motors ->
//               ~0.045 N*m stall per motor, ~0.5 m/s no-load base speed
//
// Faithful to the firmware: angle-only control (no encoders yet, measured
// velocity input is 0), 200 Hz loop, motor deadband compensated away.
#include "../common/control.h"

#include <cmath>
#include <cstdio>

using namespace boombot;
using namespace boombot::control;

namespace {

struct RealPlant {
  // Measured constants
  static constexpr double kMass = 0.58057;    // kg
  static constexpr double kJc = 1.379e-3;     // kg*m^2 about CoM, pitch axis
  static constexpr double kL = 0.030;         // m, axle->CoM (CAD-verified 2026-07-12)
  static constexpr double kR = 0.0325;        // m wheel radius
  static constexpr double kG = 9.81;
  // L298N-limited drive: two N20s, ~3 V effective of a 6 V/0.09 N*m-ish spec
  static constexpr double kStallForce = 2.7;  // N at the contact patch (both wheels)
  static constexpr double kVmax = 0.5;        // m/s no-load at ~3 V
  static constexpr double kFric = 0.8;        // 1/s viscous on base velocity

  double theta = 0.0, omega = 0.0, vel = 0.0, pos = 0.0;

  // motor: -100..100 command (deadband already compensated in firmware)
  void step(double motor, double dt) {
    const double duty = motor / 100.0;
    // DC motor at the wheel: force falls off linearly with speed (back-EMF)
    double force = kStallForce * (duty - vel / kVmax);
    if (force > kStallForce) force = kStallForce;
    if (force < -kStallForce) force = -kStallForce;

    const double accel = force / kMass - kFric * vel;
    const double jPivot = kJc + kMass * kL * kL;
    const double thetaDd =
        (kMass * kG * kL * std::sin(theta) -
         kMass * kL * accel * std::cos(theta)) / jPivot;

    omega += thetaDd * dt;
    theta += omega * dt;
    vel += accel * dt;
    pos += vel * dt;
  }

  double angleDeg() const { return theta * 180.0 / M_PI; }
  double rateDeg() const { return omega * 180.0 / M_PI; }
};

struct Score {
  double survivedS = 0.0;
  double meanTiltDeg = 999.0;   // over the last half, if survived
  double meanAbsVel = 999.0;
  bool ok = false;
};

// Recover from an initial tilt and keep standing for `durS` seconds.
Score evaluate(const PidGains& g, double initialTiltDeg, double durS) {
  BalanceConfig cfg = defaultBalanceConfig();
  cfg.angle = g;
  BalanceController ctrl(cfg);
  ComplementaryFilter filt(0.98f);
  RealPlant p;
  p.theta = initialTiltDeg * M_PI / 180.0;
  filt.reset(p.angleDeg());

  // No encoders: pseudo-velocity from the motor command via a motor model.
  // Constants are deliberately ~25% off the plant's to prove robustness.
  VelocityEstimator vest(VelocityEstimatorConfig{
      static_cast<float>(RealPlant::kStallForce * 1.25 / RealPlant::kMass),
      static_cast<float>(RealPlant::kVmax * 0.8),
      static_cast<float>(RealPlant::kFric * 1.3)});

  const double dt = 0.005;
  const int steps = static_cast<int>(durS / dt);
  double tiltAcc = 0.0, velAcc = 0.0;
  int tail = 0;

  Score s;
  for (int i = 0; i < steps; ++i) {
    const double est = filt.update(p.angleDeg(), p.rateDeg(), dt);
    const double motor = ctrl.update(est, vest.velocity(), 0.0f, dt);
    vest.update(motor, dt);
    p.step(motor, dt);
    if (std::fabs(p.angleDeg()) > 35.0) {
      s.survivedS = i * dt;
      return s;  // fell
    }
    if (i >= steps / 2) {
      tiltAcc += std::fabs(p.angleDeg());
      velAcc += std::fabs(p.vel);
      ++tail;
    }
  }
  s.survivedS = durS;
  s.meanTiltDeg = tiltAcc / tail;
  s.meanAbsVel = velAcc / tail;
  s.ok = true;
  return s;
}

}  // namespace

int main() {
  std::printf("# tuning against measured plant: m=%.0f g, Jc=%.2e, L=%.0f mm, "
              "stall force=%.1f N, vmax=%.1f m/s\n",
              RealPlant::kMass * 1000, RealPlant::kJc, RealPlant::kL * 1000,
              RealPlant::kStallForce, RealPlant::kVmax);

  struct Result { PidGains g; double cost; Score s5; };
  Result best[5];
  int nBest = 0;

  for (double kp = 4.0; kp <= 40.0; kp += 2.0) {
    for (double kd = 0.0; kd <= 3.01; kd += 0.25) {
      for (double ki = 0.0; ki <= 10.01; ki += 5.0) {
        const PidGains g{static_cast<float>(kp), static_cast<float>(ki),
                         static_cast<float>(kd)};
        // Must survive both a small and a moderate disturbance.
        const Score s3 = evaluate(g, 3.0, 10.0);
        const Score s6 = evaluate(g, 6.0, 10.0);
        if (!s3.ok || !s6.ok) continue;
        const double cost = s6.meanTiltDeg + 0.5 * s6.meanAbsVel +
                            s3.meanTiltDeg + 0.5 * s3.meanAbsVel;
        if (nBest < 5) {
          best[nBest++] = {g, cost, s6};
        } else {
          int worst = 0;
          for (int i = 1; i < 5; ++i)
            if (best[i].cost > best[worst].cost) worst = i;
          if (cost < best[worst].cost) best[worst] = {g, cost, s6};
        }
      }
    }
  }

  if (nBest == 0) {
    std::printf("NO STABLE GAINS FOUND - drive authority insufficient for this plant\n");
    return 1;
  }
  // simple sort of the tiny list
  for (int i = 0; i < nBest; ++i)
    for (int j = i + 1; j < nBest; ++j)
      if (best[j].cost < best[i].cost) { Result t = best[i]; best[i] = best[j]; best[j] = t; }

  std::printf("# top gains (angle loop), 6 deg recovery stats:\n");
  for (int i = 0; i < nBest; ++i) {
    std::printf("  kp=%4.1f ki=%4.1f kd=%4.2f   mean tilt %.2f deg, mean |v| %.3f m/s, cost %.3f\n",
                best[i].g.kp, best[i].g.ki, best[i].g.kd,
                best[i].s5.meanTiltDeg, best[i].s5.meanAbsVel, best[i].cost);
  }

  // Also report the largest initial tilt the best gains can recover from.
  double maxTilt = 0.0;
  for (double t0 = 2.0; t0 <= 30.0; t0 += 1.0) {
    if (evaluate(best[0].g, t0, 10.0).ok) maxTilt = t0; else break;
  }
  std::printf("# best gains recover from up to ~%.0f deg initial tilt\n", maxTilt);
  return 0;
}
