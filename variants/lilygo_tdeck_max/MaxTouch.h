#pragma once

// Minimal driver for the T-Deck MAX's Hynitron CST3530 touch controller (CST66xx
// protocol, I2C 0x1A, INT = GPIO12). Used for the 3 capacitive keys under the e-paper.
// Protocol (from LilyGo's HynTouch / hyn_cst66xx.c):
//   normal mode:  write D0 00 04 00 (x2), D0 00 00 00, D0 00 0C 00, D0 00 01 00
//   report:       write D0 07 00 00, read 9 bytes (+5 per extra point)
//                 buf[2] = 0xFF (position/key report), buf[3] = keys<<4 | fingers
//                 checksum: 0x55 + sum(buf[4 .. 4+n*5)) == buf[0] | buf[1]<<8
//                 key entry first: key_id = buf[8] & 0x0F, pressed = (buf[8] >> 4) != 0
//   ack:          write D0 00 02 AB

#include <Arduino.h>
#include <Wire.h>

#ifndef TOUCH_HOLD_MS
  #define TOUCH_HOLD_MS 1500   // hold heart/plane this long to jump to oldest/newest
#endif

namespace { volatile bool _max_touch_irq = false; void IRAM_ATTR _maxTouchIsr() { _max_touch_irq = true; } }

class MaxTouch {
  TwoWire* _w;
  int _int_pin;
  bool _ok = false;
  int8_t _key_down = -1;
  uint32_t _down_ms = 0;
  bool _long_sent = false;

  bool cmd4(uint8_t a, uint8_t b, uint8_t c, uint8_t d) {
    _w->beginTransmission(0x1A); _w->write(a); _w->write(b); _w->write(c); _w->write(d);
    return _w->endTransmission(true) == 0;
  }

public:
  bool log_raw = false;
  int16_t last_x = -1, last_y = -1;   // last finger position (if the glass reports any)

  MaxTouch(int int_pin = 12, TwoWire* w = &Wire) : _w(w), _int_pin(int_pin) { }

  bool begin() {
    pinMode(_int_pin, INPUT_PULLUP);
    _w->beginTransmission(0x1A);
    if (_w->endTransmission() != 0) return false;
    cmd4(0xD0, 0x00, 0x04, 0x00); delay(1);
    cmd4(0xD0, 0x00, 0x04, 0x00);
    cmd4(0xD0, 0x00, 0x00, 0x00);
    cmd4(0xD0, 0x00, 0x0C, 0x00);
    cmd4(0xD0, 0x00, 0x01, 0x00);
    attachInterrupt(_int_pin, _maxTouchIsr, FALLING);
    _ok = true;
    return true;
  }
  bool isReady() const { return _ok; }

  // Returns key id (0..2) on a new PRESS, -1 otherwise.
  int poll() {
    if (!_ok) return -1;
    // long press: a key held TOUCH_HOLD_MS reports once as 16 + id (fast jump)
    if (_key_down >= 0 && !_long_sent && millis() - _down_ms > TOUCH_HOLD_MS) { _long_sent = true; return 16 + _key_down; }
    if (!_max_touch_irq && digitalRead(_int_pin) == HIGH) return -1;
    _max_touch_irq = false;

    uint8_t b[32]; memset(b, 0, sizeof(b));
    if (!cmd4(0xD0, 0x07, 0x00, 0x00)) return -1;
    if (_w->requestFrom((uint8_t)0x1A, (uint8_t)9) != 9) return -1;
    for (int i = 0; i < 9; i++) b[i] = _w->read();
    int fingers = b[3] & 0x0F, keys = b[3] >> 4, n = fingers + keys;
    if (n > 1 && n <= 5) {
      int extra = (n - 1) * 5;
      if (_w->requestFrom((uint8_t)0x1A, (uint8_t)extra) == extra)
        for (int i = 0; i < extra; i++) b[9 + i] = _w->read();
    }
    cmd4(0xD0, 0x00, 0x02, 0xAB);   // ack the report

    if (log_raw) Serial.printf("TOUCHRAW %02X %02X %02X %02X %02X %02X %02X %02X %02X\n",
                               b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7], b[8]);
    if (b[2] != 0xFF || n == 0 || n > 5) { if (n == 0) _key_down = -1; return -1; }
    uint16_t sum = 0x55;
    for (int i = 4; i < 4 + n * 5; i++) sum += b[i];
    if (sum != (uint16_t)(b[0] | (b[1] << 8))) return -1;

    if (keys) {
      int id = b[8] & 0x0F;
      bool pressed = (b[8] >> 4) != 0;
      if (pressed && _key_down != id) { _key_down = id; _down_ms = millis(); _long_sent = false; return id; }
      if (!pressed) _key_down = -1;
      return -1;
    }
    _key_down = -1;
    last_x = b[4] | ((b[7] & 0x0F) << 8);
    last_y = b[5] | ((b[7] & 0xF0) << 4);
    return -1;
  }
};
