// Stand-up (swing-up) maneuver study for the real BoomBot plant.
//   make -C firmware/test standup
//
// The robot lies at ~80 deg (chassis edge resting, wheels touching). Static
// motor reaction torque (~0.17 N*m) cannot beat gravity (~0.28 N*m at 90 deg),
// so the maneuver is a momentum pump: spin the wheels AWAY from the face to
// build speed, then slam full reverse - back-EMF roughly doubles the
// deliverable force while the motor fights its own motion, the reaction
// torque kicks the body up, and the balance controller catches it.
//
// This sim extends the plant with wheel reaction torque (negligible near
// upright, dominant when lying) and a ground contact constraint, then
// searches the reverse-phase duration for reliable stand-up.
#include "../common/control.h"

#include <cmath>
#include <cstdio>

using namespace boombot;
using namespace boombot::control;

namespace {

struct Plant {
  // As-built constants (CAD 2026-07-12, incl. 2S pack)
  static constexpr double kMass = 0.68701;
  static constexpr double kJc = 2.065e-3;
  static constexpr double kL = 0.042;
  static constexpr double kR = 0.0325;
  static constexpr double kG = 9.81;
  static constexpr double kStallForce = 5.2;
  static constexpr double kVmax = 0.95;
  static constexpr double kFric = 0.8;
  static constexpr double kRestDeg = 55.0;  // design target: tail props it at <=55 deg

  double theta = kRestDeg * M_PI / 180.0;
  double omega = 0.0, vel = 0.0, pos = 0.0;

  void step(double motor, double dt) {
    const double duty = clampd(motor / 100.0, -1.0, 1.0);
    // Back-EMF: force scales with (duty - v/vmax); braking/reversing against
    // motion can deliver up to ~2x stall force.
    double force = kStallForce * (duty - vel / kVmax);
    force = clampd(force, -2.0 * kStallForce, 2.0 * kStallForce);
    // Traction ceiling: the wheels cannot push harder than friction allows,
    // regardless of motor torque (mu ~0.9, rubber on hard floor).
    const double tractionMax = 0.9 * kMass * kG;
    force = clampd(force, -tractionMax, tractionMax);

    const double accel = force / kMass - kFric * vel;
    const double jPivot = kJc + kMass * kL * kL;
    // Reaction torque of the wheel drive on the body: -force * r.
    const double thetaDd =
        (kMass * kG * kL * std::sin(theta) -
         kMass * kL * accel * std::cos(theta) - force * kR) / jPivot;

    omega += thetaDd * dt;
    theta += omega * dt;
    vel += accel * dt;
    pos += vel * dt;

    // Ground contact: cannot pitch past the resting angle.
    const double rest = kRestDeg * M_PI / 180.0;
    if (theta > rest) { theta = rest; if (omega > 0) omega = 0; }
    if (theta < -rest) { theta = -rest; if (omega < 0) omega = 0; }
  }

  static double clampd(double v, double lo, double hi) {
    return v < lo ? lo : (v > hi ? hi : v);
  }
  double angleDeg() const { return theta * 180.0 / M_PI; }
  double rateDeg() const { return omega * 180.0 / M_PI; }
};

struct Attempt {
  bool stood = false;      // reached the catch window
  bool balanced = false;   // still upright 3 s after handover
  double catchDeg = 0.0, catchRate = 0.0, peakVel = 0.0;
};

// Phase 1: full duty away from the face for revS seconds.
// Phase 2: full duty toward the face until the catch window or timeout.
// Catch:   |theta| < catchDeg -> hand over to the balance controller.
Attempt tryStandup(double revS, double catchDeg) {
  Plant p;
  BalanceConfig cfg = defaultBalanceConfig();
  cfg.angle = PidGains{40.0f, 10.0f, 0.75f};  // firmware defaults
  BalanceController ctrl(cfg);
  VelocityEstimator vest(VelocityEstimatorConfig{7.57f, 0.95f, 0.8f});
  ComplementaryFilter filt(0.98f);
  filt.reset(p.angleDeg());

  const double dt = 0.005;
  Attempt a;

  // dir: lying at +80 -> kick needs forward (+) force in phase 2.
  const double dir = p.theta > 0 ? 1.0 : -1.0;

  // Phase 1: build wheel speed away from the face.
  for (int i = 0; i < static_cast<int>(revS / dt); ++i) {
    p.step(-100.0 * dir, dt);
    filt.update(p.angleDeg(), p.rateDeg(), dt);
    if (std::fabs(p.vel) > a.peakVel) a.peakVel = std::fabs(p.vel);
  }
  // Phase 2: slam toward the face; wait for the catch window (max 1.5 s).
  bool caught = false;
  for (int i = 0; i < static_cast<int>(1.5 / dt); ++i) {
    p.step(100.0 * dir, dt);
    const double est = filt.update(p.angleDeg(), p.rateDeg(), dt);
    if (std::fabs(p.vel) > a.peakVel) a.peakVel = std::fabs(p.vel);
    if (std::fabs(est) < catchDeg) {
      a.stood = true;
      a.catchDeg = est;
      a.catchRate = p.rateDeg();
      caught = true;
      break;
    }
  }
  if (!caught) return a;

  // Handover: balance controller takes it from here.
  ctrl.reset();
  vest.reset();
  for (int i = 0; i < static_cast<int>(3.0 / dt); ++i) {
    const double est = filt.update(p.angleDeg(), p.rateDeg(), dt);
    const double motor = ctrl.update(est, vest.velocity(), 0.0f, dt);
    vest.update(motor, dt);
    p.step(motor, dt);
    if (std::fabs(p.angleDeg()) > 35.0) return a;  // fell after catch
  }
  a.balanced = true;
  return a;
}

}  // namespace

int main() {
  std::printf("# stand-up search: lying at %.0f deg, catch window sweep\n",
              Plant::kRestDeg);
  int successes = 0;
  double bestRev = -1, bestCatch = -1, bestRate = 1e9;
  for (double revS = 0.10; revS <= 0.85; revS += 0.05) {
    for (double catchDeg = 15.0; catchDeg <= 30.01; catchDeg += 5.0) {
      const Attempt a = tryStandup(revS, catchDeg);
      if (a.balanced) {
        ++successes;
        // prefer the gentlest catch (smallest body rate at handover)
        if (std::fabs(a.catchRate) < bestRate) {
          bestRate = std::fabs(a.catchRate);
          bestRev = revS;
          bestCatch = catchDeg;
        }
        std::printf("  rev=%.2fs catch<%2.0fdeg  -> stood, balanced (catch at %+.1f deg, %+.0f dps, peak v %.2f m/s)\n",
                    revS, catchDeg, a.catchDeg, a.catchRate, a.peakVel);
      }
    }
  }
  if (successes == 0) {
    std::printf("NO stand-up found - needs more torque or a multi-pump strategy\n");
    return 1;
  }
  std::printf("# %d working combos; RECOMMENDED: reverse %.2f s, catch window %.0f deg (handover rate %.0f dps)\n",
              successes, bestRev, bestCatch, bestRate);
  return 0;
}
