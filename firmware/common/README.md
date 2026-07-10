# firmware/common

Code shared between MCU 1 (ESP32-S3 Lolin) and MCU 2 (ESP32-S3-CAM).

## `protocol.h`

Header-only implementation of the UART protocol specified in
[`docs/protocol.md`](../../docs/protocol.md). It has no Arduino/ESP-IDF
dependencies, uses no dynamic allocation or exceptions, and compiles on both
firmwares and on a host for testing — so both sides of the link speak the wire
format from a single source of truth.

### What it provides

- **Types** — `Direction`, `Mode`, `Side`, `Gesture`, and the `Fault` bitmask.
- **Parsing** — `parseCommand()` (MCU 2 → MCU 1) and `parseTelemetry()`
  (MCU 1 → MCU 2). Both tolerate a trailing `\r\n`, reject malformed or
  out-of-range packets, and never read past the line.
- **Serialization** — `formatDrive()`, `formatHeartbeat()`, `formatGesture()`,
  `formatMode()`, `formatTelemetry()`, `formatObstacle()`. Each writes a
  newline-terminated packet and returns `-1` rather than truncating if the
  buffer is too small.
- **`LineReader`** — accumulates UART bytes and signals when a full line is
  ready, dropping overlong lines (a lost newline) instead of overflowing.

### Usage sketch

```cpp
#include "protocol.h"
using namespace boombot::proto;

// MCU 2: send a drive command
char out[kMaxLine];
int n = formatDrive(out, sizeof out, Direction::Forward, 75);  // "D:F:75\n"
Serial1.write(reinterpret_cast<uint8_t*>(out), n);

// MCU 1: receive commands byte-by-byte from the UART ISR/loop
LineReader reader;
while (Serial1.available()) {
  if (reader.push(Serial1.read())) {
    Command cmd;
    if (parseCommand(reader.line(), cmd) && cmd.type == CmdType::Drive) {
      applyDrive(cmd.dir, cmd.speed);
    }
  }
}
```

### Tests

Host unit tests live in [`../test`](../test). They need only a C++11 compiler:

```sh
make -C firmware/test        # build and run (292 checks)
```

The tests cover parse success/failure, range clamping, serialization,
format→parse round trips for every packet type, and the streaming
`LineReader`.
