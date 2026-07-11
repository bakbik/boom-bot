# UART Protocol — MCU 1 ↔ MCU 2

**Transport:** Hardware UART, 115200 baud, 8N1, newline-terminated ASCII packets

---

## MCU 2 → MCU 1 (Commands)

| Packet | Format | Example | Notes |
|--------|--------|---------|-------|
| Heartbeat | `HB\n` | `HB` | Send every 200 ms — MCU 1 watchdog times out at 500 ms |
| Drive | `D:<dir>:<spd>\n` | `D:F:75` | dir: F/B/L/R/S, spd: 0–100 |
| Gesture | `G:<id>\n` | `G:HAPPY` | Trigger named gesture on display |
| Mode | `M:<mode>\n` | `M:FOLLOW` | IDLE / FOLLOW / AVOID / DANCE / SLEEP |
| Stop | `D:S:0\n` | `D:S:0` | Convenience alias for full stop |

### Direction Values

| Value | Meaning |
|-------|---------|
| `F` | Forward |
| `B` | Backward |
| `L` | Spin left |
| `R` | Spin right |
| `S` | Stop |

---

## MCU 1 → MCU 2 (Telemetry)

Sent every 50 ms unconditionally.

| Packet | Format | Example | Notes |
|--------|--------|---------|-------|
| Telemetry | `T:<ang>:<spd>:<bat>:<fault>\n` | `T:2.3:45:4.92:0` | ang: degrees (float), spd: 0–100, bat: volts (float), fault: 0/1 |
| ToF alert | `O:<side>:<mm>\n` | `O:L:320` | side: L/R, mm: distance |

### Fault Flags

`fault` field is a bitmask:
- `0` — nominal
- `1` — angle > 45° (fallen)
- `2` — watchdog triggered (MCU 2 silent)
- `4` — motor overcurrent (reserved — the L298N has no fault pin; may be
  implemented later via its SENSE resistors + ADC)

---

## Gesture IDs

| ID | Trigger condition |
|----|------------------|
| `HAPPY` | Person detected and following |
| `SAD` | Lost target |
| `CURIOUS` | New object in frame |
| `SURPRISED` | Sudden obstacle |
| `ANGRY` | Repeated obstacle |
| `SLEEP` | Idle timeout |
| `BLINK` | Random ambient (MCU 1 self-triggers) |
| `SCAN` | Searching for target |
| `AVOID_L` | Avoiding left obstacle |
| `AVOID_R` | Avoiding right obstacle |
| `DANCE` | Dance mode active |
| `BOOT` | Startup animation |
| `LOWBAT` | Battery < 3.5V |
| `DAZED` | Post-collision / large tilt event |

---

## Error Handling

- Unknown packet type: silently ignore, do not halt
- Malformed packet (no newline within 100 ms): flush buffer, reset parser
- MCU 1 watchdog: on timeout, send `M:IDLE\n` internally and enter safe-balance mode
- MCU 2 on MCU 1 fault bit set: stop sending drive commands until fault clears
