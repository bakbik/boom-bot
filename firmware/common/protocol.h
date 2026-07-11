// BoomBot UART protocol — shared between MCU 1 (ESP32-S3 Lolin) and
// MCU 2 (ESP32-S3-CAM). See docs/protocol.md for the wire spec.
//
// Header-only and free of Arduino/ESP-IDF dependencies so it compiles
// unchanged on both firmwares and on a host for unit testing. No dynamic
// allocation, no exceptions, no <string> — safe for the balance loop.
#ifndef BOOMBOT_PROTOCOL_H
#define BOOMBOT_PROTOCOL_H

#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

namespace boombot {
namespace proto {

// 115200 baud, 8N1, newline-terminated ASCII. Longest packet ("T:-179.99:100:12.34:7")
// stays well under this; leave headroom for a trailing "\r\n" and NUL.
static const size_t kMaxLine = 48;

enum class Direction : char {
  Forward  = 'F',
  Backward = 'B',
  Left     = 'L',
  Right    = 'R',
  Stop     = 'S',
};

enum class Mode : uint8_t { Idle, Follow, Avoid, Dance, Sleep };

enum class Side : char { Left = 'L', Right = 'R' };

// Gesture IDs, in the order listed in docs/protocol.md.
enum class Gesture : uint8_t {
  Happy, Sad, Curious, Surprised, Angry, Sleep, Blink,
  Scan, AvoidL, AvoidR, Dance, Boot, LowBat, Dazed,
};

// Fault bitmask carried in the telemetry packet.
enum Fault : uint8_t {
  FAULT_NONE        = 0,
  FAULT_FALLEN      = 1,  // angle > 45°
  FAULT_WATCHDOG    = 2,  // MCU 2 heartbeat lost
  FAULT_OVERCURRENT = 4,  // reserved — L298N has no fault pin (see docs/protocol.md)
};

// ---- MCU 2 -> MCU 1 : commands -------------------------------------------

enum class CmdType : uint8_t { None, Heartbeat, Drive, Gesture, Mode };

struct Command {
  CmdType   type    = CmdType::None;
  Direction dir     = Direction::Stop;  // Drive
  uint8_t   speed   = 0;                 // Drive, 0..100
  Gesture   gesture = Gesture::Blink;    // Gesture
  Mode      mode    = Mode::Idle;        // Mode
};

// ---- MCU 1 -> MCU 2 : telemetry ------------------------------------------

enum class TelemType : uint8_t { None, Telemetry, Obstacle };

struct Telemetry {
  TelemType type    = TelemType::None;
  float     angle   = 0.0f;   // degrees
  uint8_t   speed   = 0;      // 0..100
  float     battery = 0.0f;   // volts
  uint8_t   fault   = 0;      // Fault bitmask
  Side      side    = Side::Left;  // Obstacle
  uint16_t  mm      = 0;           // Obstacle distance
};

// ---- helpers --------------------------------------------------------------

namespace detail {

inline bool clampSpeed(long v, uint8_t& out) {
  if (v < 0 || v > 100) return false;
  out = static_cast<uint8_t>(v);
  return true;
}

// Copy `line` up to (but not including) the first CR/LF or NUL into `dst`,
// which must hold kMaxLine bytes. Returns false if the line is too long.
inline bool stripEol(const char* line, char* dst) {
  size_t i = 0;
  for (; line[i] && line[i] != '\r' && line[i] != '\n'; ++i) {
    if (i >= kMaxLine - 1) return false;
    dst[i] = line[i];
  }
  dst[i] = '\0';
  return true;
}

struct NamedGesture { const char* name; Gesture id; };
inline const NamedGesture* gestureTable(size_t& count) {
  static const NamedGesture kTable[] = {
    {"HAPPY", Gesture::Happy},   {"SAD", Gesture::Sad},
    {"CURIOUS", Gesture::Curious},{"SURPRISED", Gesture::Surprised},
    {"ANGRY", Gesture::Angry},   {"SLEEP", Gesture::Sleep},
    {"BLINK", Gesture::Blink},   {"SCAN", Gesture::Scan},
    {"AVOID_L", Gesture::AvoidL},{"AVOID_R", Gesture::AvoidR},
    {"DANCE", Gesture::Dance},   {"BOOT", Gesture::Boot},
    {"LOWBAT", Gesture::LowBat}, {"DAZED", Gesture::Dazed},
  };
  count = sizeof(kTable) / sizeof(kTable[0]);
  return kTable;
}

struct NamedMode { const char* name; Mode id; };
inline const NamedMode* modeTable(size_t& count) {
  static const NamedMode kTable[] = {
    {"IDLE", Mode::Idle},   {"FOLLOW", Mode::Follow},
    {"AVOID", Mode::Avoid}, {"DANCE", Mode::Dance},
    {"SLEEP", Mode::Sleep},
  };
  count = sizeof(kTable) / sizeof(kTable[0]);
  return kTable;
}

}  // namespace detail

inline bool gestureFromString(const char* s, Gesture& out) {
  size_t n; const detail::NamedGesture* t = detail::gestureTable(n);
  for (size_t i = 0; i < n; ++i)
    if (strcmp(s, t[i].name) == 0) { out = t[i].id; return true; }
  return false;
}

inline const char* gestureToString(Gesture g) {
  size_t n; const detail::NamedGesture* t = detail::gestureTable(n);
  for (size_t i = 0; i < n; ++i)
    if (t[i].id == g) return t[i].name;
  return "BLINK";
}

inline bool modeFromString(const char* s, Mode& out) {
  size_t n; const detail::NamedMode* t = detail::modeTable(n);
  for (size_t i = 0; i < n; ++i)
    if (strcmp(s, t[i].name) == 0) { out = t[i].id; return true; }
  return false;
}

inline const char* modeToString(Mode m) {
  size_t n; const detail::NamedMode* t = detail::modeTable(n);
  for (size_t i = 0; i < n; ++i)
    if (t[i].id == m) return t[i].name;
  return "IDLE";
}

inline bool isValidDirection(char c) {
  return c == 'F' || c == 'B' || c == 'L' || c == 'R' || c == 'S';
}

// ---- parsing --------------------------------------------------------------
// Both parsers accept a line with or without a trailing CR/LF and never read
// past it. Unknown or malformed input returns false; the caller ignores it
// per docs/protocol.md "Error Handling".

inline bool parseCommand(const char* line, Command& out) {
  char buf[kMaxLine];
  if (!detail::stripEol(line, buf)) return false;
  out = Command();

  if (strcmp(buf, "HB") == 0) { out.type = CmdType::Heartbeat; return true; }

  // Everything else is "<T>:<...>"; require a type char then ':'.
  if (buf[0] == '\0' || buf[1] != ':') return false;
  const char char0 = buf[0];
  const char* body = buf + 2;

  if (char0 == 'D') {
    // D:<dir>:<spd>
    if (!isValidDirection(body[0]) || body[1] != ':') return false;
    char* end = nullptr;
    long spd = strtol(body + 2, &end, 10);
    if (end == body + 2 || *end != '\0') return false;  // no digits / trailing junk
    if (!detail::clampSpeed(spd, out.speed)) return false;
    out.type = CmdType::Drive;
    out.dir = static_cast<Direction>(body[0]);
    return true;
  }
  if (char0 == 'G') {
    if (!gestureFromString(body, out.gesture)) return false;
    out.type = CmdType::Gesture;
    return true;
  }
  if (char0 == 'M') {
    if (!modeFromString(body, out.mode)) return false;
    out.type = CmdType::Mode;
    return true;
  }
  return false;
}

inline bool parseTelemetry(const char* line, Telemetry& out) {
  char buf[kMaxLine];
  if (!detail::stripEol(line, buf)) return false;
  out = Telemetry();

  if (buf[0] == '\0' || buf[1] != ':') return false;
  const char char0 = buf[0];
  const char* body = buf + 2;

  if (char0 == 'T') {
    // T:<ang>:<spd>:<bat>:<fault>
    char* end = nullptr;
    double ang = strtod(body, &end);
    if (end == body || *end != ':') return false;
    const char* p = end + 1;
    long spd = strtol(p, &end, 10);
    if (end == p || *end != ':') return false;
    p = end + 1;
    double bat = strtod(p, &end);
    if (end == p || *end != ':') return false;
    p = end + 1;
    long fault = strtol(p, &end, 10);
    if (end == p || *end != '\0') return false;
    if (!detail::clampSpeed(spd, out.speed)) return false;
    if (fault < 0 || fault > 255) return false;
    out.type = TelemType::Telemetry;
    out.angle = static_cast<float>(ang);
    out.battery = static_cast<float>(bat);
    out.fault = static_cast<uint8_t>(fault);
    return true;
  }
  if (char0 == 'O') {
    // O:<side>:<mm>
    if ((body[0] != 'L' && body[0] != 'R') || body[1] != ':') return false;
    char* end = nullptr;
    long mm = strtol(body + 2, &end, 10);
    if (end == body + 2 || *end != '\0') return false;
    if (mm < 0 || mm > 65535) return false;
    out.type = TelemType::Obstacle;
    out.side = static_cast<Side>(body[0]);
    out.mm = static_cast<uint16_t>(mm);
    return true;
  }
  return false;
}

// ---- serialization --------------------------------------------------------
// Each writes a newline-terminated packet and returns the number of bytes
// written (excluding the NUL), or -1 if the buffer was too small.

inline int formatHeartbeat(char* buf, size_t n) {
  int w = snprintf(buf, n, "HB\n");
  return (w < 0 || static_cast<size_t>(w) >= n) ? -1 : w;
}

inline int formatDrive(char* buf, size_t n, Direction dir, uint8_t spd) {
  if (spd > 100) return -1;
  int w = snprintf(buf, n, "D:%c:%u\n", static_cast<char>(dir), spd);
  return (w < 0 || static_cast<size_t>(w) >= n) ? -1 : w;
}

inline int formatGesture(char* buf, size_t n, Gesture g) {
  int w = snprintf(buf, n, "G:%s\n", gestureToString(g));
  return (w < 0 || static_cast<size_t>(w) >= n) ? -1 : w;
}

inline int formatMode(char* buf, size_t n, Mode m) {
  int w = snprintf(buf, n, "M:%s\n", modeToString(m));
  return (w < 0 || static_cast<size_t>(w) >= n) ? -1 : w;
}

inline int formatTelemetry(char* buf, size_t n, float angle, uint8_t spd,
                           float battery, uint8_t fault) {
  if (spd > 100) return -1;
  int w = snprintf(buf, n, "T:%.2f:%u:%.2f:%u\n", angle, spd, battery, fault);
  return (w < 0 || static_cast<size_t>(w) >= n) ? -1 : w;
}

inline int formatObstacle(char* buf, size_t n, Side side, uint16_t mm) {
  int w = snprintf(buf, n, "O:%c:%u\n", static_cast<char>(side), mm);
  return (w < 0 || static_cast<size_t>(w) >= n) ? -1 : w;
}

// ---- incremental line reader ---------------------------------------------
// UART arrives byte-by-byte. Feed each byte; push() returns true when a full
// line is ready in line()/length(). Overlong lines (a lost newline) are
// dropped and the buffer resets, matching the protocol's flush-on-malformed
// rule. CR is ignored so CRLF and LF both work.
class LineReader {
 public:
  bool push(char c) {
    if (c == '\r') return false;
    if (c == '\n') {
      buf_[len_] = '\0';
      ready_len_ = len_;
      len_ = 0;
      return true;
    }
    if (len_ >= kMaxLine - 1) { len_ = 0; return false; }  // overflow: drop
    buf_[len_++] = c;
    return false;
  }

  const char* line() const { return buf_; }
  size_t length() const { return ready_len_; }
  void reset() { len_ = 0; ready_len_ = 0; }

 private:
  char   buf_[kMaxLine] = {0};
  size_t len_ = 0;
  size_t ready_len_ = 0;
};

}  // namespace proto
}  // namespace boombot

#endif  // BOOMBOT_PROTOCOL_H
