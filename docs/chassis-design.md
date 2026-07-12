# Chassis Design — Control-Relevant Dimensions (M2 input)

> **As built (CAD-measured, 2026-07-12):** mass **581 g**, pitch inertia about
> the CoM **1.38×10⁻³ kg·m²**, axle→CoM **L ≈ 30 mm**, wheels **65 mm**
> (confirmed). Balance-point trim settled at ≈ −8°, which at L = 30 mm means
> the CoM sits only ~4 mm off the axle horizontally — no urgent battery move.
> Although L is far below the 80–120 mm target below, the fall timescale is
> still comfortable: the large inertia relative to m·L² gives an *effective*
> pendulum length J_pivot/(m·L) ≈ 109 mm, so τ ≈ 0.10 s (≈20 control cycles).
> Gains for this exact plant: `make -C firmware/test tune`. Key result: with
> the L298N-limited drive the robot is **only stabilizable with velocity
> feedback** (pseudo-velocity estimator now; AS5600 encoders properly).

This is the bridge between the balance/drive controller (`firmware/common/control.h`,
verified in `firmware/test/test_control.cpp`) and the physical chassis. The
controller was tuned in simulation against an assumed geometry; the closer the
real chassis matches it, the less re-tuning is needed on hardware (M9 auto-tune).

> **The single most important number is `L` — the height of the center of mass
> (CoM) above the wheel axle.** The controller's stability and gains depend on it
> more than on anything else. The sim assumes **L ≈ 90 mm**.

## Parameters the controller assumes

| Symbol | Meaning | Value in sim/code | Where |
|--------|---------|-------------------|-------|
| `L` | CoM height above the wheel axle ("pendulum length") | **0.09 m** | `kL`, test_control.cpp |
| `r_wheel` | Wheel radius | 0.0325 m (65 mm wheels) | BOM |
| `a_max` | Base acceleration at full motor command | 30 m/s² (optimistic) | `kAmax` |
| `v_max` | Top translational speed | 0.6 m/s | `defaultDriveConfig().maxSpeed` |
| `loop rate` | Balance loop frequency | 200 Hz (dt = 5 ms) | test_control.cpp |
| `maxLean` | Outer loop's tilt-setpoint clamp | 6° | `defaultBalanceConfig` |
| `fallen` | Give-up tilt (drive cut) | 35° | `defaultBalanceConfig` |
| `g` | Gravity | 9.81 m/s² | `kG` |

Angle PID gains `{kp=12, ki=8, kd=0.9}`, velocity PID `{kp=9, ki=4}`. These are
**simulation-tuned starting points**, not final — they will be re-tuned once the
real mass, motor torque, and `L` are measured.

## The key physics — and the counterintuitive part

An upright balancing robot is an **inverted pendulum**. Its tilt diverges with a
characteristic time constant:

```
τ = sqrt(L / g)  ≈ sqrt(0.09 / 9.81) ≈ 0.096 s
```

τ is roughly how fast it falls. At 200 Hz you get ~20 control cycles per τ — enough
headroom. This leads to a result that surprises people:

- **A HIGHER center of mass is EASIER to balance, not harder.** Longer `L` → larger
  τ → the robot falls more slowly → more time to react. (Balancing a broom on your
  palm is easy; a pencil is hard.)
- **Burying all the weight at the bottom is the wrong instinct here.** That is right
  for a *statically stable* vehicle (a car, a low racer), but this robot is
  *actively* balanced. A very low CoM (short `L`) makes it fall fast and demands a
  faster loop and stronger motors.

So do **not** optimize for "lowest possible CoG." Aim for a **moderate CoM height
around 80–120 mm above the axle** (near the sim's 90 mm). That is the sweet spot:
slow enough to control comfortably, without being so tall that disturbances get big
leverage or the motors can't catch a large tilt.

### The competing limit
Taller isn't free: a taller robot gives disturbances more leverage and needs the
wheels to travel further to get back under the CoM. And `a_max` must be big enough
to catch a lean — which is why the robot must stay **light** (see below).

## Concrete chassis recommendations

**Center of mass**
- Target **CoM ~90 mm above the axle** (matches the tuned model).
- Put the **heaviest item (the battery) mid-height, not at the very bottom.**
- Make the battery mount **position-adjustable** (a slot or a couple of screw
  positions fore/aft). Shifting the battery is how you trim the true balance point
  and tune `L` on the bench without touching firmware.

**Fore/aft (pitch) balance**
- At rest, the CoM must sit **directly above the axle** so the balance angle is a
  true vertical (θ = 0). If the battery sits forward, the neutral lean is offset and
  you waste control range. Adjustable battery position fixes this.

**Left/right (yaw) symmetry — important**
- Keep mass **symmetric left-to-right.** Uneven L/R mass makes the robot
  continuously veer/turn because one wheel needs more effort. Center everything on
  the wheelbase centerline.

**IMU (MPU-6050) placement**
- Mount it **rigidly** to the chassis — flex or a wobbly standoff injects fake
  acceleration and ruins the tilt estimate.
- Mount it **on the centerline, as close to the wheel-axle height/axis as
  practical.** When the body rotates about the axle, an accelerometer far from that
  axis reads tangential/centripetal acceleration (∝ distance from axle) that
  pollutes the angle. Close to the axle = cleaner signal.
- Mount it **flat and axis-aligned** (know which IMU axis is pitch), away from motor
  wires. Its exact ground clearance is not a control constant — proximity to the
  axle line is what matters.

**Wheels & motors**
- 65 mm wheels + N20 300 rpm → no-load top speed ≈ π·0.065·(300/60) ≈ **1.0 m/s**;
  under load the effective `v_max ≈ 0.6 m/s` used in the mixer is realistic.
- Bigger wheels → more speed but less acceleration/torque. Keep 65 mm unless you
  change the gains.
- **`a_max = 30 m/s²` in the sim is optimistic** (that's ~3g at the contact patch;
  real N20s deliver less and will slip before that). It's further reduced by the
  **L298N driver's ~1.5–3 V internal dropout** — on the 5 V rail the motors see
  only ~2–3.5 V, roughly halving drive authority vs. an ideal driver. The
  practical consequence: **keep the robot light** so the motors can still
  accelerate the base fast enough to catch a lean. Every extra gram lowers real
  `a_max`. (Swapping to a MOSFET driver later recovers most of this margin.)

**Size & structure**
- Small and compact: think **~150–250 mm tall**; track width is flexible (see below).
- **Track width is NOT a control-loop constraint — it can be narrow.** The
  controller only balances *pitch*; **roll is passive**, so the track sets a
  static sideways tip limit:

  ```
  roll tip angle ≈ atan( (track/2) / h_CoM_above_ground )
  ```

  With h ≈ 122 mm (90 mm CoM above axle + 32.5 mm wheel radius):
  100 mm → ~22°, 80 mm → ~18°, 60 mm → ~14°, 40 mm → ~9°.
  Curves (centrifugal tilt), single-wheel bumps, floor slope, and dance gestures
  all eat into this margin. **~70–80 mm is still comfortable; below ~60 mm gets
  tippy** outside flat smooth floors. If you go narrow, don't also go tall —
  narrow + high CoM is what falls over sideways. Narrower track also spins
  faster for the same wheel differential (just a `turnGain` recalibration).
  In practice the widest component between the wheels (battery, ~65–70 mm for a
  power bank) usually sets the minimum anyway.
- **Rigid frame.** Structural flex is the enemy of balance — dual plates with solid
  standoffs, no rattly joints.
- Compact mass near the CoM = lower rotational inertia = snappier response (but
  faster fall); more spread-out mass = more inertia = slower, more forgiving but
  sluggish. Compact-and-moderate-height is the target.

## What to send back for exact re-tuning

Once the chassis exists, give me these measured values and I'll re-derive the gains:
1. **`L`** — CoM height above the axle (balance the assembled robot on a knife-edge
   at the axle, or measure/estimate from the CAD mass model).
2. **Total mass** (kg).
3. **Wheel radius** (m).
4. **Motor stall torque & no-load rpm** at the drive voltage (for a realistic
   `a_max`).
5. **Track width** (L/R wheel spacing) — for turn-rate calibration.

See `docs/chassis-dimensions.svg` for a labeled diagram of these dimensions.
