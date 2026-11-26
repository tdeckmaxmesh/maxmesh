#pragma once

// LilyGo T-Deck MAX (ESP32-S3, SX1262, 3.1" GDEQ031T10 e-paper, TCA8418 keyboard)
//
// Almost every peripheral rail and reset line is behind an XL9555 I2C GPIO
// expander, so this board class owns that expander and exposes simple
// power-control methods to the rest of the firmware.

#include <Arduino.h>
#include <Wire.h>
#include <SPI.h>
#include <helpers/ESP32Board.h>

// ---------------- direct GPIOs ----------------
#define MAX_I2C_SDA        13
#define MAX_I2C_SCL        14
#define MAX_SPI_SCK        36
#define MAX_SPI_MOSI       33
#define MAX_SPI_MISO       47
#define MAX_SD_CS          48
#define MAX_EPD_CS         34
#define MAX_EPD_DC         35
#define MAX_EPD_RST         9
#define MAX_EPD_BUSY       37
#define MAX_EPD_FRONTLIGHT 41
#define MAX_KB_INT         15
#define MAX_KB_LIGHT       42
#define MAX_TOUCH_INT      12
#define MAX_BOOT_BTN        0

// ---------------- I2C addresses ----------------
#define MAX_ADDR_XL9555   0x20
#define MAX_ADDR_KEYBOARD 0x34
#define MAX_ADDR_BQ27220  0x55
#define MAX_ADDR_DRV2605  0x5A

// ---------------- XL9555 pins (0..7 = port0, 8..15 = port1) ----------------
#define XL_MODEM_EN   0   // A7682E boost supply
#define XL_LORA_EN    1
#define XL_GPS_EN     2
#define XL_1V8_EN     3   // BHI260AP IMU rail
#define XL_LORA_SEL   4   // HIGH = internal antenna, LOW = external (u.FL)
#define XL_MOTOR_EN   5   // DRV2605 rail
#define XL_AMP_EN     6
#define XL_TOUCH_RST  7   // active low
#define XL_PWRKEY     8
#define XL_KEY_RST    9   // active low
#define XL_AUDIO_SEL 10

class TDeckMaxBoard : public ESP32Board {
  uint8_t _xl_out[2] = { 0, 0 };
  bool _xl_ok = false;
  bool _haptic_ok = false;
  uint8_t _frontlight = 0;

  bool xlWrite(uint8_t reg, uint8_t val);
  void hapticWrite(uint8_t reg, uint8_t val);
  uint8_t hapticRead(uint8_t reg);
  uint16_t gaugeRead16(uint8_t reg) const;

public:
  SPIClass spi { FSPI };   // ONE bus object shared by LoRa, e-paper and SD

  void begin();

  // XL9555 helpers
  void xlPin(uint8_t pin, bool high);
  bool xlState(uint8_t pin) const { return (_xl_out[pin >> 3] >> (pin & 7)) & 1; }
  bool expanderOk() const { return _xl_ok; }

  void setGpsPower(bool on)      { xlPin(XL_GPS_EN, on); }
  bool isGpsPowered() const      { return xlState(XL_GPS_EN); }
  void setExternalAntenna(bool ext) { xlPin(XL_LORA_SEL, !ext); }
  bool isExternalAntenna() const { return !xlState(XL_LORA_SEL); }

  // lights (0 = off .. 255 = full)
  void setFrontlight(uint8_t level);
  uint8_t getFrontlight() const { return _frontlight; }
  void setKeyboardLight(bool on);

  // haptics
  void buzz(uint8_t effect = 47);   // 47 = "buzz 1 - 100%"

  // battery (BQ27220 fuel gauge)
  uint16_t getBattMilliVolts() override;
  int  getBattPercent();             // -1 if unknown (voltage-curve estimate)
  int  getGaugeSoc();                // fuel gauge's own %, for diagnostics
  int  getBattCurrentMA();           // +charging / -discharging
  bool isCharging() { return getBattCurrentMA() > 5; }
  bool isUsbPowered();               // SY6970 charger "power good"

  // power
  void powerOff() override;          // deep sleep, wake on BOOT button
  void lightSleepUntilEvent(uint32_t max_ms);   // wake on LoRa / key / button / timer
  // Side (BOOT) button presses are latched by an interrupt so short taps are never missed
  // (e.g. during an e-paper refresh or while waking from light sleep).
  bool takeButtonPress();

  const char* getManufacturerName() const override { return "LilyGo T-Deck MAX"; }
};
