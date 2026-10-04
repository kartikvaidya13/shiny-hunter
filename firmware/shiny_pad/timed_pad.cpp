#include "timed_pad.h"

#include "src/switch_ESP32/switch_ESP32.h"

#if !CONFIG_TINYUSB_HID_ENABLED
#error "TinyUSB HID is not enabled in this core build."
#endif

// Global like upstream's GamepadDemo: the constructor must run before USB.begin().
static NSGamepad gPad;

void TimedPad::begin() {
  if (kScopePin >= 0) {
    pinMode(kScopePin, OUTPUT);
    digitalWrite(kScopePin, LOW);
  }
  gPad.begin();
  USB.begin();
  sendNeutral();
}

SendStamp TimedPad::sendState(uint16_t buttons, uint8_t dpad,
                              uint8_t lx, uint8_t ly, uint8_t rx, uint8_t ry) {
  HID_NSGamepadReport_Data_t report = {};
  report.buttons = buttons;
  report.dPad = dpad;
  report.leftXAxis = lx;
  report.leftYAxis = ly;
  report.rightXAxis = rx;
  report.rightYAxis = ry;

  SendStamp stamp;
  if (kScopePin >= 0) digitalWrite(kScopePin, HIGH);
  stamp.before_us = micros();
  stamp.ok = gPad.write(&report, sizeof(report));
  stamp.after_us = micros();
  if (kScopePin >= 0) digitalWrite(kScopePin, LOW);
  return stamp;
}

SendStamp TimedPad::sendNeutral() {
  gPad.end();  // only resets the local report; write() sends it
  SendStamp stamp;
  stamp.before_us = micros();
  stamp.ok = gPad.write();
  stamp.after_us = micros();
  return stamp;
}

void TimedPad::refresh() {
  gPad.loop();
}
