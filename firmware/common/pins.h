// BoomBot MCU 1 (ESP32-S3 Lolin) pin map — single source of truth.
// Keep in sync with docs/wiring.md (the human-readable version of this file).
//
// ESP32-S3 pins deliberately NOT used here:
//   GPIO0, 3, 45, 46  strapping pins
//   GPIO19, 20        native USB D-/D+
//   GPIO26..32        SPI flash
//   GPIO33..37        octal PSRAM (present on the 8MB-PSRAM Lolin S3)
//   GPIO38            onboard RGB LED on the Lolin S3
//   GPIO43, 44        UART0 console (flashing/logs)
#ifndef BOOMBOT_PINS_H
#define BOOMBOT_PINS_H

namespace boombot {
namespace pins {

// ---- L298N motor driver (6 GPIOs) ----------------------------------------
// ENA/ENB jumpers removed; PWM via LEDC. Both EN low = hardware motor kill.
constexpr int kMotorEnLeft   = 5;   // ENA (PWM) — left motor speed
constexpr int kMotorIn1      = 6;   // IN1 — left motor direction A
constexpr int kMotorIn2      = 7;   // IN2 — left motor direction B
constexpr int kMotorEnRight  = 4;   // ENB (PWM) — right motor speed
constexpr int kMotorIn3      = 12;  // IN3 — right motor direction A
constexpr int kMotorIn4      = 13;  // IN4 — right motor direction B

// ---- I2C bus 0: MPU-6050 + 2x VL53L0X + left AS5600 -----------------------
constexpr int kI2c0Sda = 11;
constexpr int kI2c0Scl = 10;
// VL53L0X XSHUT lines (hold one in reset at boot to re-address the other).
constexpr int kTofXshutLeft  = 41;
constexpr int kTofXshutRight = 42;

// ---- I2C bus 1: right AS5600 ----------------------------------------------
// AS5600 has a fixed address (0x36), so the two encoders live on separate buses.
constexpr int kI2c1Sda = 1;
constexpr int kI2c1Scl = 2;

// ---- SPI: 2x GC9A01 round displays (shared bus, separate CS) --------------
constexpr int kDispMosi   = 8;   // shared SDA/MOSI
constexpr int kDispSclk   = 40;  // shared SCL/SCLK
constexpr int kDispCsLeft  = 9;
constexpr int kDispCsRight = 39;
constexpr int kDispDc     = 14;  // shared data/command
constexpr int kDispRst    = 21;  // shared reset
// BLK (backlight) pins tied to 3.3V rail, not a GPIO.

// ---- UART1: link to MCU 2 (ESP32-S3-CAM) -----------------------------------
constexpr int kUartTx = 17;  // -> CAM RX
constexpr int kUartRx = 18;  // <- CAM TX

// ---- Battery voltage sense --------------------------------------------------
// 5V rail -> 100k/100k divider -> ADC (reads rail/2, ~2.5V nominal).
// ADC2 channel: safe here because MCU 1 never enables WiFi (ADC2's only conflict).
constexpr int kVbatSense = 15;  // ADC2_CH4

// Spare, safe-to-use: GPIO 16, 47, 48.

}  // namespace pins
}  // namespace boombot

#endif  // BOOMBOT_PINS_H
