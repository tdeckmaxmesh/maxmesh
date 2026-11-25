#pragma once

// TCA8418 keyboard driver for the T-Deck MAX (same 35-key layout as the T-Deck Pro).
//
//   Row 1:  Q W E R T Y U I O P
//   Row 2:  A S D F G H J K L Bksp
//   Row 3:  Alt Z X C V B N M $ Enter
//   Row 4:  Shift Mic Space Sym Shift
//
// Modifiers:
//   Shift  - one-shot uppercase; double-tap for caps lock
//   Sym    - one-shot numbers/symbols as printed on the keys; double-tap for sym lock
//   Alt    - NAVIGATION modifier (this keyboard has no arrows/trackball):
//            Alt+W/S/A/D = up/down/left/right, Alt+Q = back, Alt+<other> = command
// Scan codes (key number = row*10 + col + 1) taken from the LilyGo/Meck reference.

#include <Arduino.h>
#include <Wire.h>

// special key codes returned by read()
enum : int {
  MK_NONE = 0,
  MK_BS = '\b',
  MK_ENTER = '\r',
  MK_UP = 0x100, MK_DOWN, MK_LEFT, MK_RIGHT,
  MK_BACK,          // Alt+Q
  MK_MIC,           // bare Mic key
  MK_DOLLAR,        // bare $ key (screen light)
  MK_TOP, MK_BOTTOM, // jump to first / last (long-press heart / plane, Alt+T / Alt+E)
  MK_ALT = 0x200   // MK_ALT + 'a'..'z' for other Alt+letter commands
};

class MaxKeyboard {
  TwoWire* _wire;
  uint8_t _addr;
  bool _ok = false;
  bool _shift = false, _caps = false, _sym = false, _symlock = false, _alt = false;
  bool _shift_held = false, _shift_used = false;
  uint32_t _last_shift = 0, _last_sym = 0, _sym_down = 0;
  bool _bs_held = false;
  uint32_t _bs_next = 0;
  int _queue[8];
  uint8_t _qh = 0, _qt = 0;

  uint8_t rd(uint8_t reg) {
    _wire->beginTransmission(_addr); _wire->write(reg); _wire->endTransmission(false);
    _wire->requestFrom(_addr, (uint8_t)1);
    return _wire->available() ? _wire->read() : 0;
  }
  void wr(uint8_t reg, uint8_t v) {
    _wire->beginTransmission(_addr); _wire->write(reg); _wire->write(v); _wire->endTransmission();
  }
  void push(int k) {
    uint8_t n = (_qt + 1) & 7;
    if (n != _qh) { _queue[_qt] = k; _qt = n; }
  }

  static char base(uint8_t c) {
    switch (c) {
      case 10: return 'q'; case 9: return 'w'; case 8: return 'e'; case 7: return 'r'; case 6: return 't';
      case 5: return 'y';  case 4: return 'u'; case 3: return 'i'; case 2: return 'o'; case 1: return 'p';
      case 20: return 'a'; case 19: return 's'; case 18: return 'd'; case 17: return 'f'; case 16: return 'g';
      case 15: return 'h'; case 14: return 'j'; case 13: return 'k'; case 12: return 'l';
      case 29: return 'z'; case 28: return 'x'; case 27: return 'c'; case 26: return 'v'; case 25: return 'b';
      case 24: return 'n'; case 23: return 'm';
      case 33: return ' ';
      default: return 0;
    }
  }
  static char sym(uint8_t c) {
    switch (c) {
      case 10: return '#'; case 9: return '1'; case 8: return '2'; case 7: return '3'; case 6: return '(';
      case 5: return ')';  case 4: return '_'; case 3: return '-'; case 2: return '+'; case 1: return '@';
      case 20: return '*'; case 19: return '4'; case 18: return '5'; case 17: return '6'; case 16: return '/';
      case 15: return ':'; case 14: return ';'; case 13: return '\''; case 12: return '"';
      case 29: return '7'; case 28: return '8'; case 27: return '9'; case 26: return '?'; case 25: return '!';
      case 24: return ','; case 23: return '.';
      case 34: return '0'; case 22: return '$'; case 33: return ' ';
      default: return 0;
    }
  }

  void handle(uint8_t code, bool pressed) {
    if (code == 35 || code == 31) {           // Shift (left / right)
      _mods_changed = true;
      if (pressed) {
        uint32_t now = millis();
        if (_shift && now - _last_shift < 400) { _caps = !_caps; _shift = false; }
        else _shift = true;
        _last_shift = now;
        _shift_held = true; _shift_used = false;
      } else {
        _shift_held = false;
        if (_shift_used) _shift = false;
      }
      return;
    }
    if (code == 11) {                          // Backspace (auto-repeat while held)
      _bs_held = pressed;
      if (pressed) { push(MK_BS); _bs_next = millis() + 450; }
      return;
    }
    if (code == 32 && !pressed) {             // Sym released: a long hold turns one-shot into sym lock
      if (_sym && millis() - _sym_down >= 500) { _symlock = true; _sym = false; _mods_changed = true; }
      return;
    }
    if (!pressed) return;

    _mods_changed = true;
    if (code == 30) { _alt = !_alt; _sym = false; return; }   // Alt (one-shot); cancels Sym
    if (code == 32) {                          // Sym (one-shot, double-tap = lock); cancels Alt
      uint32_t now = millis();
      _sym_down = now;
      _alt = false;
      if (_symlock) { _symlock = false; _sym = false; }
      else if (_sym && now - _last_sym < 400) { _symlock = true; _sym = false; }
      else _sym = !_sym;
      _last_sym = now;
      return;
    }
    if (code == 21) { _alt = false; _sym = false; push(MK_ENTER); return; }
    // Mic and $ are function keys (lights) unless Sym is armed: Sym+Mic = '0', Sym+$ = '$'
    if (code == 34) {
      bool s = _sym || _symlock; _sym = false; _alt = false;
      push(s ? '0' : MK_MIC); return;
    }
    if (code == 22) {
      bool s = _sym || _symlock; _sym = false; _alt = false;
      push(s ? '$' : MK_DOLLAR); return;
    }

    if (_alt) {
      _alt = false;
      char b = base(code);
      switch (b) {
        case 'w': push(MK_UP); return;
        case 's': push(MK_DOWN); return;
        case 'a': push(MK_LEFT); return;
        case 'd': push(MK_RIGHT); return;
        case 'q': push(MK_BACK); return;
        default:
          if (b >= 'a' && b <= 'z') push(MK_ALT + b);
          return;
      }
    }

    if (_sym || _symlock) {
      char s = sym(code);
      _sym = false;
      if (s) push(s);
      return;
    }
    if (code == 34) { push(MK_MIC); return; }
    if (code == 22) { push(MK_DOLLAR); return; }

    char c = base(code);
    if (!c) return;
    if (c >= 'a' && c <= 'z' && (_shift || _caps) && !(_shift && _caps)) c = c - 'a' + 'A';
    if (_shift) { if (_shift_held) _shift_used = true; else _shift = false; }
    push(c);
  }

public:
  MaxKeyboard(uint8_t addr = 0x34, TwoWire* wire = &Wire) : _wire(wire), _addr(addr) { }

  bool begin() {
    _wire->beginTransmission(_addr);
    if (_wire->endTransmission() != 0) return false;
    wr(0x01, 0x00);                            // CFG: scanner off while configuring
    for (int i = 0; i < 16 && (rd(0x03) & 0x0F); i++) rd(0x04);   // drain FIFO
    wr(0x02, 0x1F);                            // clear INT_STAT
    wr(0x20, 0); wr(0x21, 0); wr(0x22, 0);     // no GPI events
    wr(0x1D, 0xFF); wr(0x1E, 0xFF); wr(0x1F, 0x03);   // 8 rows x 10 cols keypad matrix
    wr(0x29, 0x03);                            // debounce
    wr(0x01, 0x11);                            // KE_IEN | INT_CFG: key events, INT re-asserts
    delay(5);
    for (int i = 0; i < 16 && (rd(0x03) & 0x0F); i++) rd(0x04);
    wr(0x02, 0x1F);
    _ok = true;
    return true;
  }

  bool isReady() const { return _ok; }

  // Read hardware FIFO; call often. Returns next key or MK_NONE.
  int read() {
    if (_ok) {
      int guard = 10;
      while (guard-- > 0) {
        uint8_t n = rd(0x03) & 0x0F;
        if (n == 0) break;
        uint8_t ev = rd(0x04);
        if (log_raw) Serial.printf("KEYRAW code=%d %s\n", ev & 0x7F, (ev & 0x80) ? "down" : "up");
        handle(ev & 0x7F, (ev & 0x80) != 0);
      }
      wr(0x02, 0x1F);
      if (_bs_held && (int32_t)(millis() - _bs_next) >= 0) {
        push(MK_BS);
        _bs_next = millis() + 70;
      }
    }
    if (_qh == _qt) return MK_NONE;
    int k = _queue[_qh];
    _qh = (_qh + 1) & 7;
    return k;
  }

  bool keyHeld() const { return _bs_held; }
  bool log_raw = false;   // print raw scan codes to Serial (diagnostics)
  bool _mods_changed = false;
  // true once after any modifier/key event, so the UI can redraw the ALT/SYM indicator
  bool modsChanged() { bool c = _mods_changed; _mods_changed = false; return c; }

  // modifier state for the on-screen indicator
  bool shiftOn() const { return _shift; }
  bool capsOn() const { return _caps; }
  bool symOn() const { return _sym || _symlock; }
  bool symLock() const { return _symlock; }
  bool altOn() const { return _alt; }
  void clearMods() { _shift = _caps = _sym = _symlock = _alt = false; }
};
