// Renders the gesture pose bank (firmware/common/gestures.h) to an SVG contact
// sheet so the eye expressions can be reviewed without hardware.
//   make -C firmware/test gestures     # writes gestures.svg
//
// This is a preview approximation of the round GC9A01 eyes, not the on-device
// renderer — it exists to sanity-check that each pose reads as intended.
#include "../common/gestures.h"

#include <cmath>
#include <cstdio>
#include <sstream>
#include <string>

using namespace boombot;
using boombot::face::EyePose;
using boombot::face::EyeShape;
using boombot::face::FacePose;

namespace {

std::string rgb(uint8_t r, uint8_t g, uint8_t b) {
  char buf[16];
  std::snprintf(buf, sizeof buf, "#%02x%02x%02x", r, g, b);
  return buf;
}

std::string rgbDim(uint8_t r, uint8_t g, uint8_t b, float f) {
  return rgb(static_cast<uint8_t>(r * f), static_cast<uint8_t>(g * f),
             static_cast<uint8_t>(b * f));
}

// Draw one round eye. innerSign = +1 if the nose-side is to the right of this
// eye (left eye), -1 for the right eye — used to orient the brow.
void drawEye(std::ostringstream& s, const EyePose& e, double cx, double cy,
             double R, int innerSign, int uid) {
  const std::string tint = rgb(e.r, e.g, e.b);

  // Physical round module: bezel + black screen.
  s << "<circle cx='" << cx << "' cy='" << cy << "' r='" << R + 5
    << "' fill='#1a1a20' stroke='#3a3a44' stroke-width='2'/>";
  s << "<circle cx='" << cx << "' cy='" << cy << "' r='" << R
    << "' fill='#000'/>";

  s << "<clipPath id='clip" << uid << "'><circle cx='" << cx << "' cy='" << cy
    << "' r='" << R << "'/></clipPath>";
  s << "<g clip-path='url(#clip" << uid << ")'>";

  const double px = cx + e.pupilX * R * 0.42;
  const double py = cy + e.pupilY * R * 0.42;
  const bool closed = e.openness <= 0.03f || e.shape == EyeShape::Line;

  if (e.shape == EyeShape::Spiral) {
    // Dizzy spiral.
    std::ostringstream path;
    path << "M " << px << " " << py << " ";
    for (double t = 0; t < 6.28 * 3; t += 0.3) {
      double rr = (t / (6.28 * 3)) * R * 0.85;
      path << "L " << px + rr * std::cos(t) << " " << py + rr * std::sin(t)
           << " ";
    }
    s << "<path d='" << path.str() << "' fill='none' stroke='" << tint
      << "' stroke-width='4'/>";
  } else if (closed) {
    // Flat closed lid line.
    s << "<line x1='" << cx - R * 0.7 << "' y1='" << py << "' x2='"
      << cx + R * 0.7 << "' y2='" << py << "' stroke='" << tint
      << "' stroke-width='6' stroke-linecap='round'/>";
  } else {
    const double iris = R * 0.52 * e.pupilScale;
    const double pupil = R * 0.30 * e.pupilScale;
    s << "<circle cx='" << px << "' cy='" << py << "' r='" << iris
      << "' fill='" << rgbDim(e.r, e.g, e.b, 0.55f) << "'/>";
    s << "<circle cx='" << px << "' cy='" << py << "' r='" << pupil
      << "' fill='" << tint << "'/>";
    // Catch-light.
    s << "<circle cx='" << px - pupil * 0.4 << "' cy='" << py - pupil * 0.4
      << "' r='" << pupil * 0.28 << "' fill='#ffffff' opacity='0.9'/>";
  }

  // Eyelids: black shapes closing in from top (openness) and bottom (squint).
  if (!closed) {
    const double topCover = (1.0 - e.openness) * R * 1.15;
    if (topCover > 0.5)
      s << "<rect x='" << cx - R - 2 << "' y='" << cy - R - 2 << "' width='"
        << 2 * R + 4 << "' height='" << topCover << "' fill='#000'/>";
    const double botCover = e.squint * R * 0.9;
    if (botCover > 0.5)
      s << "<rect x='" << cx - R - 2 << "' y='" << cy + R - botCover
        << "' width='" << 2 * R + 4 << "' height='" << botCover + 4
        << "' fill='#000'/>";
  }
  s << "</g>";

  // Brow overlay (drawn above the eye), oriented by browAngle.
  if (std::fabs(e.browAngle) > 1.0f) {
    const double y0 = cy - R * 0.72;
    const double dx = R * 0.72;
    const double tilt = (e.browAngle / 45.0) * R * 0.5;
    const double xo = cx - innerSign * dx, yo = y0 - tilt * 0.4;  // outer
    const double xi = cx + innerSign * dx, yi = y0 + tilt;        // inner
    s << "<line x1='" << xo << "' y1='" << yo << "' x2='" << xi << "' y2='"
      << yi << "' stroke='" << tint
      << "' stroke-width='7' stroke-linecap='round'/>";
  }
}

}  // namespace

int main() {
  size_t n = 0;
  const proto::Gesture* ids = face::allGestures(n);

  const int cols = 3;
  const int rows = static_cast<int>((n + cols - 1) / cols);
  const double cellW = 300, cellH = 230, pad = 20, headerH = 70;
  const double W = cols * cellW + 2 * pad;
  const double H = headerH + rows * cellH + pad;

  std::ostringstream s;
  s << "<svg xmlns='http://www.w3.org/2000/svg' width='" << W << "' height='"
    << H << "' viewBox='0 0 " << W << " " << H << "'>";
  s << "<rect width='" << W << "' height='" << H << "' fill='#0e0e14'/>";
  s << "<text x='" << pad << "' y='42' fill='#e8e8f0' font-size='28' "
       "font-family='sans-serif' font-weight='700'>BoomBot — Eye Gesture Bank"
       "</text>";
  s << "<text x='" << pad << "' y='60' fill='#8a8a99' font-size='14' "
       "font-family='sans-serif'>2× GC9A01 round displays · "
    << n << " gestures</text>";

  int uid = 0;
  for (size_t i = 0; i < n; ++i) {
    const FacePose p = face::facePoseFor(ids[i]);
    const int c = static_cast<int>(i % cols), r = static_cast<int>(i / cols);
    const double x0 = pad + c * cellW, y0 = headerH + r * cellH;
    const double eyeR = 58;
    const double lcx = x0 + cellW * 0.34, rcx = x0 + cellW * 0.66;
    const double ecy = y0 + cellH * 0.42;

    s << "<rect x='" << x0 + 6 << "' y='" << y0 + 6 << "' width='" << cellW - 12
      << "' height='" << cellH - 12
      << "' rx='12' fill='#16161e' stroke='#26262e'/>";
    drawEye(s, p.left, lcx, ecy, eyeR, +1, uid++);
    drawEye(s, p.right, rcx, ecy, eyeR, -1, uid++);
    s << "<text x='" << x0 + cellW / 2 << "' y='" << y0 + cellH - 24
      << "' fill='#c8c8d4' font-size='18' font-family='monospace' "
         "text-anchor='middle'>"
      << face::gestureLabel(ids[i]) << "</text>";
  }
  s << "</svg>";

  FILE* f = std::fopen("gestures.svg", "w");
  if (!f) { std::perror("gestures.svg"); return 1; }
  std::fputs(s.str().c_str(), f);
  std::fclose(f);
  std::printf("wrote gestures.svg (%d gestures)\n", static_cast<int>(n));
  return 0;
}
