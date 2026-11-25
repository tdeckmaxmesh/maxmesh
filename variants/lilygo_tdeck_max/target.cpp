#include <Arduino.h>
#include "target.h"

TDeckMaxBoard board;

RADIO_CLASS radio = new Module(P_LORA_NSS, P_LORA_DIO_1, P_LORA_RESET, P_LORA_BUSY, board.spi);
WRAPPER_CLASS radio_driver(radio, board);

ESP32RTCClock fallback_clock;
AutoDiscoverRTCClock rtc_clock(fallback_clock);
// GPS power is switched through the XL9555 by the board/UI, so no enable pin here
MicroNMEALocationProvider gps(Serial1, &rtc_clock);
EnvironmentSensorManager sensors(gps);

#ifdef DISPLAY_CLASS
  DISPLAY_CLASS display;
  MaxKeyboard keyboard(MAX_ADDR_KEYBOARD, &Wire);
  MaxTouch touch(MAX_TOUCH_INT, &Wire);
#endif

bool radio_init() {
  fallback_clock.begin();
  rtc_clock.begin(Wire);
  return radio.std_init(&board.spi);
}

mesh::LocalIdentity radio_new_identity() {
  RadioNoiseListener rng(radio);
  return mesh::LocalIdentity(&rng);
}
