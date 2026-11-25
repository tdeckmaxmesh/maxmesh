#include "TDeckMaxDisplay.h"
#include "TDeckMaxBoard.h"

extern TDeckMaxBoard board;

// UIColor palette for black/white e-paper
ColorVal UIColor::window_bkg = GxEPD_WHITE;
ColorVal UIColor::title_bkg = GxEPD_BLACK;
ColorVal UIColor::title_txt = GxEPD_WHITE;
ColorVal UIColor::primary_txt = GxEPD_BLACK;
ColorVal UIColor::secondary_txt = GxEPD_BLACK;
ColorVal UIColor::warning_txt = GxEPD_BLACK;
ColorVal UIColor::popup_bkg = GxEPD_WHITE;
ColorVal UIColor::popup_txt = GxEPD_BLACK;
ColorVal UIColor::corp_blue = GxEPD_BLACK;

__attribute__((weak)) void ui_display_busy_hook() { }

static void busyCallback(const void*) {
  ui_display_busy_hook();
}

TDeckMaxDisplay::TDeckMaxDisplay()
  : DisplayDriver(240, 320),
    _epd(MAX_EPD_CS, MAX_EPD_DC, MAX_EPD_RST, MAX_EPD_BUSY),
    _canvas(240, 320) { }

bool TDeckMaxDisplay::begin() {
  if (_init) return true;
  _epd.selectSPI(board.spi, SPISettings(10000000, MSBFIRST, SPI_MODE0));
  _epd.init(0, true, 2, false);   // no debug serial, initial full refresh, 2ms reset pulse
  _epd.setBusyCallback(busyCallback, nullptr);
  _canvas.setRotation(0);         // portrait: 240 wide, 320 tall, keyboard below
  _canvas.setTextWrap(false);
  _canvas.setTextColor(GxEPD_BLACK);
  _canvas.setFont(&FreeSans9pt7b);
  _canvas.fillScreen(GxEPD_WHITE);
  _init = true;
  _on = true;
  return true;
}

void TDeckMaxDisplay::turnOn() {
  if (!_init) begin();
  _on = true;
}

void TDeckMaxDisplay::turnOff() {
  // e-paper keeps its image with zero power; just put the controller to sleep
  if (_init) _epd.hibernate();
  _on = false;
}

void TDeckMaxDisplay::clear() { _canvas.fillScreen(GxEPD_WHITE); }

void TDeckMaxDisplay::startFrame(ColorVal bkg) {
  _canvas.fillScreen(bkg);
  _canvas.setTextColor(GxEPD_BLACK);
}

void TDeckMaxDisplay::setTextSize(int sz) {
  switch (sz) {
    case 1:  _canvas.setFont(&FreeSans9pt7b); break;
    case 2:  _canvas.setFont(&FreeSansBold12pt7b); break;
    default: _canvas.setFont(&FreeSansBold18pt7b); break;
  }
}

void TDeckMaxDisplay::setColor(ColorVal c) { _canvas.setTextColor(c); }
void TDeckMaxDisplay::setCursor(int x, int y) { _canvas.setCursor(x, y + 14); }
void TDeckMaxDisplay::print(const char* str) { _canvas.print(str); }
void TDeckMaxDisplay::fillRect(int x, int y, int w, int h) { _canvas.fillRect(x, y, w, h, GxEPD_BLACK); }
void TDeckMaxDisplay::drawRect(int x, int y, int w, int h) { _canvas.drawRect(x, y, w, h, GxEPD_BLACK); }
void TDeckMaxDisplay::drawXbm(int x, int y, const uint8_t* bits, int w, int h) {
  _canvas.drawXBitmap(x, y, bits, w, h, GxEPD_BLACK);
}

uint16_t TDeckMaxDisplay::getTextWidth(const char* str) {
  int16_t x1, y1; uint16_t w, h;
  _canvas.getTextBounds(str, 0, 0, &x1, &y1, &w, &h);
  return w;
}

void TDeckMaxDisplay::commit() {
  if (!_init) return;
  _on = true;
  const uint8_t* buf = _canvas.getBuffer();
  bool full = _force_full || (_full_every > 0 && _partials >= _full_every);
  if (full) {
    _epd.writeImageForFullRefresh(buf, 0, 0, 240, 320, false, false, false);
    _epd.refresh(false);          // full refresh: clears ghosting (~1.5s, flashes)
    _partials = 0;
    _force_full = false;
  } else {
    _epd.writeImage(buf, 0, 0, 240, 320, false, false, false);
    _epd.refresh(true);           // fast partial refresh
    _partials++;
  }
  if (_epd.hasFastPartialUpdate) {
    _epd.writeImageAgain(buf, 0, 0, 240, 320, false, false, false);   // sync "previous" buffer
  }
  _epd.powerOff();                // booster off between updates (image stays)
  _refreshes++;
}
