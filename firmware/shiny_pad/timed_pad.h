// Thin wrapper over the vendored NSGamepad. The only file that includes
// switch_ESP32.h, which keeps src/switch_ESP32/ untouched.

#ifndef SHINY_PAD_TIMED_PAD_H_
#define SHINY_PAD_TIMED_PAD_H_

#include <Arduino.h>

// Wrong Tools settings flash fine but never enumerate as a gamepad.
#if !defined(ARDUINO_ARCH_ESP32)
#error "Tools > Board must be an ESP32-S3 board (ESP32S3 Dev Module)."
#endif
#if ARDUINO_USB_MODE
#error "Tools > USB Mode must be \"USB-OTG (TinyUSB)\"."
#endif
#if ARDUINO_USB_CDC_ON_BOOT
#error "Tools > USB CDC On Boot must be \"Disabled\"."
#endif

// High for the duration of each sendState() write, for the logic analyser. -1 disables.
constexpr int kScopePin = 4;

struct SendStamp {
  uint32_t before_us;
  uint32_t after_us;
  bool ok;
};

class TimedPad {
 public:
  void begin();
  // Writes immediately. Blocks until the transfer completes (up to one host poll).
  SendStamp sendState(uint16_t buttons, uint8_t dpad,
                      uint8_t lx, uint8_t ly, uint8_t rx, uint8_t ry);
  SendStamp sendNeutral();
  // 1 kHz keep-alive resend. Also blocks, so don't call it near a scheduled send.
  void refresh();
};

#endif  // SHINY_PAD_TIMED_PAD_H_
