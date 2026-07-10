// Host unit tests for the shared UART protocol (firmware/common/protocol.h).
// Compiles with any C++11 host compiler — no hardware or ESP32 toolchain.
//   make -C firmware/test        # build and run
#include "../common/protocol.h"

#include <cmath>
#include <cstdio>
#include <cstring>

using namespace boombot::proto;

static int g_fails = 0;
static int g_checks = 0;

#define CHECK(cond)                                                        \
  do {                                                                     \
    ++g_checks;                                                            \
    if (!(cond)) {                                                         \
      ++g_fails;                                                           \
      std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);         \
    }                                                                      \
  } while (0)

static bool nearly(float a, float b) { return std::fabs(a - b) < 0.01f; }

// ---- command parsing ------------------------------------------------------

static void test_parse_heartbeat() {
  Command c;
  CHECK(parseCommand("HB", c));
  CHECK(c.type == CmdType::Heartbeat);
  CHECK(parseCommand("HB\n", c));          // trailing newline tolerated
  CHECK(c.type == CmdType::Heartbeat);
  CHECK(parseCommand("HB\r\n", c));        // CRLF tolerated
  CHECK(c.type == CmdType::Heartbeat);
}

static void test_parse_drive() {
  Command c;
  CHECK(parseCommand("D:F:75", c));
  CHECK(c.type == CmdType::Drive);
  CHECK(c.dir == Direction::Forward);
  CHECK(c.speed == 75);

  CHECK(parseCommand("D:S:0", c));
  CHECK(c.dir == Direction::Stop);
  CHECK(c.speed == 0);

  CHECK(parseCommand("D:R:100", c));
  CHECK(c.dir == Direction::Right);
  CHECK(c.speed == 100);

  // out-of-range and malformed drive packets are rejected
  CHECK(!parseCommand("D:F:101", c));      // speed > 100
  CHECK(!parseCommand("D:F:-5", c));       // negative speed
  CHECK(!parseCommand("D:X:50", c));       // bad direction
  CHECK(!parseCommand("D:F:", c));         // no speed
  CHECK(!parseCommand("D:F:50x", c));      // trailing junk
  CHECK(!parseCommand("D:F", c));          // missing second colon
}

static void test_parse_gesture_and_mode() {
  Command c;
  CHECK(parseCommand("G:HAPPY", c));
  CHECK(c.type == CmdType::Gesture);
  CHECK(c.gesture == Gesture::Happy);
  CHECK(parseCommand("G:AVOID_L", c));
  CHECK(c.gesture == Gesture::AvoidL);
  CHECK(!parseCommand("G:NOPE", c));       // unknown gesture

  CHECK(parseCommand("M:FOLLOW", c));
  CHECK(c.type == CmdType::Mode);
  CHECK(c.mode == Mode::Follow);
  CHECK(!parseCommand("M:RUNNING", c));    // unknown mode
}

static void test_parse_command_rejects_junk() {
  Command c;
  CHECK(!parseCommand("", c));
  CHECK(!parseCommand("Z:1", c));          // unknown type
  CHECK(!parseCommand("T:2.3:45:4.9:0", c)); // telemetry is not a command
  CHECK(!parseCommand("HBB", c));          // near-miss on heartbeat
}

// ---- telemetry parsing ----------------------------------------------------

static void test_parse_telemetry() {
  Telemetry t;
  CHECK(parseTelemetry("T:2.30:45:4.92:0", t));
  CHECK(t.type == TelemType::Telemetry);
  CHECK(nearly(t.angle, 2.30f));
  CHECK(t.speed == 45);
  CHECK(nearly(t.battery, 4.92f));
  CHECK(t.fault == FAULT_NONE);

  CHECK(parseTelemetry("T:-179.99:100:3.20:7", t)); // negative angle, all faults
  CHECK(nearly(t.angle, -179.99f));
  CHECK(t.fault == (FAULT_FALLEN | FAULT_WATCHDOG | FAULT_OVERCURRENT));

  CHECK(!parseTelemetry("T:2.3:45:4.9", t));   // missing field
  CHECK(!parseTelemetry("T:2.3:200:4.9:0", t)); // speed out of range
  CHECK(!parseTelemetry("T::45:4.9:0", t));     // empty angle
}

static void test_parse_obstacle() {
  Telemetry t;
  CHECK(parseTelemetry("O:L:320", t));
  CHECK(t.type == TelemType::Obstacle);
  CHECK(t.side == Side::Left);
  CHECK(t.mm == 320);
  CHECK(parseTelemetry("O:R:65535", t));
  CHECK(t.mm == 65535);
  CHECK(!parseTelemetry("O:X:100", t));    // bad side
  CHECK(!parseTelemetry("O:L:99999", t));  // beyond uint16
}

// ---- serialization + round trip ------------------------------------------

static void test_format() {
  char b[kMaxLine];
  CHECK(formatHeartbeat(b, sizeof b) == 3);
  CHECK(strcmp(b, "HB\n") == 0);

  CHECK(formatDrive(b, sizeof b, Direction::Forward, 75) > 0);
  CHECK(strcmp(b, "D:F:75\n") == 0);

  CHECK(formatGesture(b, sizeof b, Gesture::AvoidR) > 0);
  CHECK(strcmp(b, "G:AVOID_R\n") == 0);

  CHECK(formatMode(b, sizeof b, Mode::Dance) > 0);
  CHECK(strcmp(b, "M:DANCE\n") == 0);

  CHECK(formatTelemetry(b, sizeof b, 2.3f, 45, 4.92f, FAULT_NONE) > 0);
  CHECK(strcmp(b, "T:2.30:45:4.92:0\n") == 0);

  CHECK(formatObstacle(b, sizeof b, Side::Left, 320) > 0);
  CHECK(strcmp(b, "O:L:320\n") == 0);

  // too-small buffer reports failure instead of truncating silently
  char tiny[4];
  CHECK(formatDrive(tiny, sizeof tiny, Direction::Forward, 100) == -1);
}

static void test_round_trip() {
  char b[kMaxLine];
  formatDrive(b, sizeof b, Direction::Backward, 60);
  Command c;
  CHECK(parseCommand(b, c));               // format output re-parses cleanly
  CHECK(c.dir == Direction::Backward && c.speed == 60);

  formatTelemetry(b, sizeof b, -12.50f, 30, 4.10f, FAULT_FALLEN);
  Telemetry t;
  CHECK(parseTelemetry(b, t));
  CHECK(nearly(t.angle, -12.50f) && t.speed == 30 &&
        nearly(t.battery, 4.10f) && t.fault == FAULT_FALLEN);

  for (int g = 0; g < 14; ++g) {           // every gesture survives a round trip
    formatGesture(b, sizeof b, static_cast<Gesture>(g));
    CHECK(parseCommand(b, c) && c.gesture == static_cast<Gesture>(g));
  }
}

// ---- streaming line reader ------------------------------------------------

static void test_line_reader() {
  LineReader r;
  const char* stream = "HB\nD:F:50\n";
  int lines = 0;
  Command c;
  for (const char* p = stream; *p; ++p) {
    if (r.push(*p)) {
      CHECK(parseCommand(r.line(), c));
      ++lines;
    }
  }
  CHECK(lines == 2);
  CHECK(c.type == CmdType::Drive && c.speed == 50);

  // an overlong line (missing newline) is dropped without overflowing
  r.reset();
  for (int i = 0; i < 200; ++i) CHECK(!r.push('X'));
  CHECK(r.push('\n') == true);             // reader still usable afterward
}

int main() {
  test_parse_heartbeat();
  test_parse_drive();
  test_parse_gesture_and_mode();
  test_parse_command_rejects_junk();
  test_parse_telemetry();
  test_parse_obstacle();
  test_format();
  test_round_trip();
  test_line_reader();

  std::printf("\n%d checks, %d failures\n", g_checks, g_fails);
  if (g_fails == 0) std::printf("ALL TESTS PASSED\n");
  return g_fails == 0 ? 0 : 1;
}
