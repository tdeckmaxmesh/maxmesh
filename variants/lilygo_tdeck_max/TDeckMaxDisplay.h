#pragma once

// E-paper driver for the T-Deck MAX's 3.1" GDEQ031T10 (UC8253, 240x320, portrait).
// Draws into an off-screen 1-bit canvas (so the UI can also dump screenshots over
// serial), then pushes it to the panel with fast partial refreshes and an
// occasional full refresh to clear ghosting.

#include <helpers/ui/DisplayDriver.h>
#include <GxEPD2_BW.h>
#include <Fonts/FreeSans9pt7b.h>
#include <Fonts/FreeSansBold9pt7b.h>
#include <Fonts/FreeSansBold12pt7b.h>
#include <Fonts/FreeSansBold18pt7b.h>

class TDeckMaxDisplay : public DisplayDriver {
  GxEPD2_310_GDEQ031T10 _epd;
  GFXcanvas1 _canvas;
  bool _init = false;
  bool _on = false;
  int _partials = 0;
  int _full_every = 25;
  bool _force_full = true;    // first refresh is always a full one
  uint32_t _refreshes = 0;

public:
  TDeckMaxDisplay();

  // ---- DisplayDriver (used by main.cpp's boot screen) ----
  bool begin();
  bool isOn() override { return _on; }
  bool isEink() override { return true; }
  void forceFullRefresh() override { _force_full = true; }
  void turnOn() override;
  void turnOff() override;
  void clear() override;
  void startFrame(ColorVal bkg = UIColor::window_bkg) override;
  void setTextSize(int sz) override;
  void setColor(ColorVal c) override;
  void setCursor(int x, int y) override;
  void print(const char* str) override;
  void fillRect(int x, int y, int w, int h) override;
  void drawRect(int x, int y, int w, int h) override;
  void drawXbm(int x, int y, const uint8_t* bits, int w, int h) override;
  uint16_t getTextWidth(const char* str) override;
  void endFrame() override { commit(); }

  // ---- direct access for the T-Deck MAX UI ----
  Adafruit_GFX& gfx() { return _canvas; }
  const uint8_t* buffer() { return _canvas.getBuffer(); }   // 240x320, 1 bit/px, 1 = white
  void commit();                          // push canvas to the panel
  void setFullRefreshEvery(int n) { _full_every = n; }
  uint32_t refreshCount() const { return _refreshes; }
};

// Called repeatedly while the panel is busy refreshing (keeps the mesh running).
void ui_display_busy_hook();
