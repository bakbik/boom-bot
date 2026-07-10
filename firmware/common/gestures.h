// BoomBot gesture / face-pose bank.
//
// Portable pose definitions for the 2× GC9A01 round eye displays. Each of the
// protocol's Gesture IDs (see protocol.h) maps to a FacePose describing both
// eyes as normalized parameters — the on-device renderer turns these into
// pixels, and the host tool in firmware/test renders them to SVG for review.
//
// The format is deliberately extensible: a MouthPose channel can be layered on
// for the planned rectangular lower-face display without changing existing
// poses (see docs/hardware.md "Planned addition"). No display/Arduino deps.
#ifndef BOOMBOT_GESTURES_H
#define BOOMBOT_GESTURES_H

#include <stdint.h>
#include "protocol.h"

namespace boombot {
namespace face {

// Outline treatment for an eye. Round is the default; the others swap the
// pupil/eye rendering for a stylized effect.
enum class EyeShape : uint8_t {
  Round,   // normal iris + pupil
  Spiral,  // dizzy spiral (dazed)
  Line,    // flat closed line
  Heart,   // heart pupil (reserved for a future LOVE gesture)
};

// One eye, all fields normalized so the renderer is resolution-independent.
struct EyePose {
  float openness;    // 0 = shut, 1 = wide open (lid position)
  float pupilX;      // -1 = hard left, 0 = center, +1 = hard right
  float pupilY;      // -1 = up,        0 = center, +1 = down (screen y is down)
  float pupilScale;  // pupil radius vs nominal (1.0). <1 tiny, >1 dilated
  float squint;      // 0 = none, 1 = lower lid fully raised (cheeks-up / glare)
  float browAngle;   // degrees; + inner-down (angry), - inner-up (sad/worried)
  EyeShape shape;
  uint8_t r, g, b;   // eye tint (iris/glow color)
};

// A complete face frame plus behavior hints for the animator.
struct FacePose {
  EyePose  left;
  EyePose  right;
  uint16_t holdMs;      // nominal hold before returning to ambient; 0 = sticky
  bool     autoBlink;   // permit random ambient blinks while in this pose
  bool     pupilsTrack; // pupils follow the tracked target (FOLLOW/AVOID)
  // Future: MouthPose mouth;  // lower-face display, added with the hardware.
};

namespace detail {

// Default friendly cyan glow used by most expressions.
static const uint8_t kCyanR = 0, kCyanG = 200, kCyanB = 255;

// Build a symmetric pose (both eyes identical) from one eye.
inline FacePose sym(const EyePose& e, uint16_t holdMs, bool autoBlink,
                    bool pupilsTrack = false) {
  FacePose p;
  p.left = e;
  p.right = e;
  p.holdMs = holdMs;
  p.autoBlink = autoBlink;
  p.pupilsTrack = pupilsTrack;
  return p;
}

inline EyePose eye(float openness, float px, float py, float pupilScale,
                   float squint, float browAngle, EyeShape shape,
                   uint8_t r = kCyanR, uint8_t g = kCyanG, uint8_t b = kCyanB) {
  EyePose e;
  e.openness = openness;
  e.pupilX = px;
  e.pupilY = py;
  e.pupilScale = pupilScale;
  e.squint = squint;
  e.browAngle = browAngle;
  e.shape = shape;
  e.r = r; e.g = g; e.b = b;
  return e;
}

}  // namespace detail

// The pose bank. One entry per Gesture ID from protocol.h.
inline FacePose facePoseFor(proto::Gesture id) {
  using detail::eye;
  using detail::sym;
  const EyeShape kR = EyeShape::Round;

  switch (id) {
    case proto::Gesture::Happy:
      // wide, cheeks up (squint from below), pupils centered
      return sym(eye(0.95f, 0.0f, -0.05f, 1.0f, 0.45f, 0.0f, kR), 0, true);

    case proto::Gesture::Sad:
      // half-open, inner brows up, looking down
      return sym(eye(0.60f, 0.0f, 0.30f, 0.9f, 0.0f, -22.0f, kR,
                     40, 140, 220), 0, true);

    case proto::Gesture::Curious:
      // one wide, one slightly squinted, glancing to the side
      {
        FacePose p;
        p.left  = eye(1.0f, 0.25f, -0.1f, 1.1f, 0.0f, -8.0f, kR);
        p.right = eye(0.75f, 0.25f, -0.1f, 1.0f, 0.25f, -8.0f, kR);
        p.holdMs = 1500; p.autoBlink = true; p.pupilsTrack = false;
        return p;
      }

    case proto::Gesture::Surprised:
      // huge eyes, tiny pupils
      return sym(eye(1.0f, 0.0f, 0.0f, 0.55f, 0.0f, -10.0f, kR), 800, false);

    case proto::Gesture::Angry:
      // narrowed, inner brows down, red glare
      return sym(eye(0.70f, 0.0f, -0.05f, 1.0f, 0.35f, 25.0f, kR,
                     255, 70, 40), 0, false);

    case proto::Gesture::Sleep:
      // nearly shut, drooping, dim
      return sym(eye(0.08f, 0.0f, 0.5f, 0.9f, 0.1f, 0.0f, EyeShape::Line,
                     0, 60, 90), 0, false);

    case proto::Gesture::Blink:
      // momentary full close
      return sym(eye(0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, EyeShape::Line), 120, false);

    case proto::Gesture::Scan:
      // open, pupils parked to one side (renderer sweeps them)
      return sym(eye(0.9f, 0.7f, 0.0f, 1.0f, 0.0f, 0.0f, kR), 0, true);

    case proto::Gesture::AvoidL:
      // side-eye toward a left obstacle
      return sym(eye(0.85f, -0.75f, 0.0f, 1.0f, 0.15f, 10.0f, kR), 0, false, true);

    case proto::Gesture::AvoidR:
      return sym(eye(0.85f, 0.75f, 0.0f, 1.0f, 0.15f, 10.0f, kR), 0, false, true);

    case proto::Gesture::Dance:
      // left wink, right wide, party colors (renderer flashes hue)
      {
        FacePose p;
        p.left  = eye(0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, EyeShape::Line,
                      255, 60, 200);
        p.right = eye(1.0f, 0.0f, -0.1f, 1.1f, 0.0f, 0.0f, kR,
                      255, 220, 0);
        p.holdMs = 0; p.autoBlink = false; p.pupilsTrack = false;
        return p;
      }

    case proto::Gesture::Boot:
      // small, forming pupils; loading feel
      return sym(eye(0.5f, 0.0f, 0.0f, 0.5f, 0.0f, 0.0f, kR), 0, false);

    case proto::Gesture::LowBat:
      // drooping, red, no blinking to save cycles
      return sym(eye(0.4f, 0.0f, 0.45f, 0.9f, 0.1f, -6.0f, kR,
                     255, 40, 40), 0, false);

    case proto::Gesture::Dazed:
      // spiral eyes after a collision / big tilt
      return sym(eye(1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, EyeShape::Spiral,
                     200, 160, 255), 1200, false);
  }
  // Unreachable for valid IDs; fall back to a neutral open eye.
  return sym(eye(0.9f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, kR), 0, true);
}

// Human-readable label for previews/logging.
inline const char* gestureLabel(proto::Gesture id) {
  return proto::gestureToString(id);
}

// All gesture IDs in display order, for iterating the bank.
inline const proto::Gesture* allGestures(size_t& count) {
  static const proto::Gesture kAll[] = {
    proto::Gesture::Happy,     proto::Gesture::Sad,
    proto::Gesture::Curious,   proto::Gesture::Surprised,
    proto::Gesture::Angry,     proto::Gesture::Sleep,
    proto::Gesture::Blink,     proto::Gesture::Scan,
    proto::Gesture::AvoidL,    proto::Gesture::AvoidR,
    proto::Gesture::Dance,     proto::Gesture::Boot,
    proto::Gesture::LowBat,    proto::Gesture::Dazed,
  };
  count = sizeof(kAll) / sizeof(kAll[0]);
  return kAll;
}

}  // namespace face
}  // namespace boombot

#endif  // BOOMBOT_GESTURES_H
