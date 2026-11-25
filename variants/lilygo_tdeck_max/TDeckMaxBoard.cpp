#include <Arduino.h>
#include "target.h"
#include <esp_sleep.h>
#include <driver/gpio.h>
#include <driver/rtc_io.h>

// XL9555 registers (same map as TCA9555/PCA9555)
#define XL_REG_OUT0 0x02
#define XL_REG_OUT1 0x03
#define XL_REG_CFG0 0x06
#define XL_REG_CFG1 0x07

// BQ27220 standard commands
#define BQ_REG_VOLTAGE 0x08
#define BQ_REG_CURRENT 0x0C
#define BQ_REG_SOC     0x2C

#define FRONTLIGHT_LEDC_CH 6

// DRV2605 registers
#define DRV_MODE     0x01
#define DRV_RTPIN    0x02
#define DRV_LIBRARY  0x03
#define DRV_SEQ1     0x04
#define DRV_SEQ2     0x05
#define DRV_GO       0x0C
#define DRV_FEEDBACK 0x1A
#define DRV_CONTROL3 0x1D

static bool i2cPresent(uint8_t addr) {
  Wire.beginTransmission(addr);
  return Wire.endTransmission() == 0;
}

bool TDeckMaxBoard::xlWrite(uint8_t reg, uint8_t val) {
  Wire.beginTransmission(MAX_ADDR_XL9555);
  Wire.write(reg);
  Wire.write(val);
  return Wire.endTransmission() == 0;
}

void TDeckMaxBoard::xlPin(uint8_t pin, bool high) {
  uint8_t port = pin >> 3, bit = 1 << (pin & 7);
  uint8_t v = high ? (_xl_out[port] | bit) : (_xl_out[port] & ~bit);
  if (v == _xl_out[port]) return;
  _xl_out[port] = v;
  if (_xl_ok) xlWrite(port ? XL_REG_OUT1 : XL_REG_OUT0, v);
}

void TDeckMaxBoard::begin() {
  // All three SPI devices share one bus: deselect everything before anything talks.
  pinMode(P_LORA_NSS, OUTPUT);  digitalWrite(P_LORA_NSS, HIGH);
  pinMode(MAX_SD_CS, OUTPUT);   digitalWrite(MAX_SD_CS, HIGH);
  pinMode(MAX_EPD_CS, OUTPUT);  digitalWrite(MAX_EPD_CS, HIGH);

  // lights off at boot
  pinMode(MAX_EPD_FRONTLIGHT, OUTPUT); digitalWrite(MAX_EPD_FRONTLIGHT, LOW);
  pinMode(MAX_KB_LIGHT, OUTPUT);       digitalWrite(MAX_KB_LIGHT, LOW);
  pinMode(MAX_BOOT_BTN, INPUT_PULLUP);
  pinMode(MAX_KB_INT, INPUT_PULLUP);

  // A panic/watchdog reboot only resets the CPU, not the GPIO block: light-sleep
  // wake-up settings (level interrupts on the keyboard/button/LoRa pins) can survive
  // it and, with a line held active, flood the chip with interrupts -> crash loop.
  // Always start from clean pin interrupt settings.
  esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
  const gpio_num_t wake_pins[] = { (gpio_num_t)MAX_KB_INT, (gpio_num_t)MAX_BOOT_BTN, (gpio_num_t)P_LORA_DIO_1 };
  for (gpio_num_t p : wake_pins) {
    gpio_wakeup_disable(p);
    gpio_set_intr_type(p, GPIO_INTR_DISABLE);
    gpio_intr_disable(p);
  }

  // Recover from a deep-sleep power-off (LoRa CS was held high)
  if (esp_reset_reason() == ESP_RST_DEEPSLEEP) {
    rtc_gpio_hold_dis((gpio_num_t)P_LORA_NSS);
  }

  ESP32Board::begin();               // Wire.begin(PIN_BOARD_SDA, PIN_BOARD_SCL)
  Wire.setClock(400000);

  // ---- XL9555: every rail starts in a known state ----
  _xl_ok = i2cPresent(MAX_ADDR_XL9555);
  if (_xl_ok) {
    xlWrite(XL_REG_CFG0, 0x00);      // all outputs
    xlWrite(XL_REG_CFG1, 0x00);
    // port0: LoRa on, internal antenna, touch out of reset. Modem, GPS, IMU, motor, amp OFF.
    _xl_out[0] = (1 << XL_LORA_EN) | (1 << XL_LORA_SEL) | (1 << XL_TOUCH_RST);
    // port1: keyboard in reset for a moment, PWRKEY low, audio -> codec
    _xl_out[1] = 0;
    xlWrite(XL_REG_OUT0, _xl_out[0]);
    xlWrite(XL_REG_OUT1, _xl_out[1]);
    delay(10);
    xlPin(XL_KEY_RST, true);         // release keyboard reset
    xlPin(XL_TOUCH_RST, false);      // touch controller reset pulse
    delay(20);
    xlPin(XL_TOUCH_RST, true);
    delay(60);
    xlPin(XL_MOTOR_EN, true);        // haptic rail (DRV2605 idles at ~uA in standby)
    delay(20);
  }

  // shared SPI bus (LoRa + EPD + SD)
  spi.begin(MAX_SPI_SCK, MAX_SPI_MISO, MAX_SPI_MOSI);

  // ---- DRV2605 haptic: ERM, open loop, internal trigger, library 1 ----
  _haptic_ok = i2cPresent(MAX_ADDR_DRV2605);
  if (_haptic_ok) {
    hapticWrite(DRV_MODE, 0x00);
    hapticWrite(DRV_RTPIN, 0x00);
    hapticWrite(DRV_FEEDBACK, hapticRead(DRV_FEEDBACK) & 0x7F);
    hapticWrite(DRV_CONTROL3, hapticRead(DRV_CONTROL3) | 0x20);
    hapticWrite(DRV_LIBRARY, 1);
    hapticWrite(DRV_MODE, 0x40);     // standby until needed
  }

  ledcSetup(FRONTLIGHT_LEDC_CH, 5000, 8);      // Arduino-ESP32 2.x API
  ledcAttachPin(MAX_EPD_FRONTLIGHT, FRONTLIGHT_LEDC_CH);
  ledcWrite(FRONTLIGHT_LEDC_CH, 0);
}

void TDeckMaxBoard::hapticWrite(uint8_t reg, uint8_t val) {
  Wire.beginTransmission(MAX_ADDR_DRV2605);
  Wire.write(reg);
  Wire.write(val);
  Wire.endTransmission();
}

uint8_t TDeckMaxBoard::hapticRead(uint8_t reg) {
  Wire.beginTransmission(MAX_ADDR_DRV2605);
  Wire.write(reg);
  Wire.endTransmission(false);
  Wire.requestFrom((uint8_t)MAX_ADDR_DRV2605, (uint8_t)1);
  return Wire.available() ? Wire.read() : 0;
}

void TDeckMaxBoard::buzz(uint8_t effect) {
  if (!_haptic_ok) return;
  hapticWrite(DRV_MODE, 0x00);       // out of standby, internal trigger
  hapticWrite(DRV_SEQ1, effect);
  hapticWrite(DRV_SEQ2, 0);
  hapticWrite(DRV_GO, 1);
  // DRV2605 returns to idle by itself after the sequence
}

void TDeckMaxBoard::setFrontlight(uint8_t level) {
  _frontlight = level;
  ledcWrite(FRONTLIGHT_LEDC_CH, level);
}

void TDeckMaxBoard::setKeyboardLight(bool on) {
  digitalWrite(MAX_KB_LIGHT, on ? HIGH : LOW);
}

uint16_t TDeckMaxBoard::gaugeRead16(uint8_t reg) const {
  Wire.beginTransmission(MAX_ADDR_BQ27220);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return 0xFFFF;
  if (Wire.requestFrom((uint8_t)MAX_ADDR_BQ27220, (uint8_t)2) != 2) return 0xFFFF;
  uint16_t lo = Wire.read();
  uint16_t hi = Wire.read();
  return (hi << 8) | lo;
}

uint16_t TDeckMaxBoard::getBattMilliVolts() {
  uint16_t mv = gaugeRead16(BQ_REG_VOLTAGE);
  return (mv == 0xFFFF || mv > 5000) ? 0 : mv;
}

// The BQ27220's own state-of-charge is unreliable on this board (it ships with
// generic capacity settings and was never calibrated for the 1400-1500 mAh cell),
// so we estimate from voltage on a Li-ion discharge curve, compensating for the
// voltage rise while charging and the sag under load.
int TDeckMaxBoard::getBattPercent() {
  int mv = getBattMilliVolts();
  if (mv == 0) return -1;
  int ma = getBattCurrentMA();
  if (ma > 0) mv -= min(150, ma / 4);       // charging lifts terminal voltage
  else mv += min(80, (-ma) / 8);            // load sags it
  if (ma >= -5 && ma <= 40 && isUsbPowered() && getBattMilliVolts() >= 4150) return 100;   // charge finished

  static const int16_t curve[][2] = {
    { 4180, 100 }, { 4100, 92 }, { 4000, 82 }, { 3950, 75 }, { 3900, 68 }, { 3850, 60 },
    { 3800, 50 }, { 3760, 40 }, { 3720, 30 }, { 3690, 22 }, { 3660, 15 }, { 3620, 10 },
    { 3560, 5 }, { 3450, 2 }, { 3300, 0 }
  };
  if (mv >= curve[0][0]) return 100;
  for (int i = 1; i < (int)(sizeof(curve) / sizeof(curve[0])); i++) {
    if (mv >= curve[i][0]) {
      int v0 = curve[i][0], v1 = curve[i - 1][0], p0 = curve[i][1], p1 = curve[i - 1][1];
      return p0 + (mv - v0) * (p1 - p0) / (v1 - v0);
    }
  }
  return 0;
}

int TDeckMaxBoard::getGaugeSoc() {
  uint16_t soc = gaugeRead16(BQ_REG_SOC);
  return soc <= 100 ? soc : -1;
}

int TDeckMaxBoard::getBattCurrentMA() {
  uint16_t raw = gaugeRead16(BQ_REG_CURRENT);
  if (raw == 0xFFFF) return 0;
  return (int16_t)raw;
}

bool TDeckMaxBoard::isUsbPowered() {
  // SY6970 (BQ25895-compatible) REG0B bit2 = PG_STAT (input power good)
  Wire.beginTransmission(0x6A);
  Wire.write(0x0B);
  if (Wire.endTransmission(false) == 0 && Wire.requestFrom((uint8_t)0x6A, (uint8_t)1) == 1) {
    return (Wire.read() & 0x04) != 0;
  }
  return getBattCurrentMA() > 0;
}

void TDeckMaxBoard::lightSleepUntilEvent(uint32_t max_ms) {
  // Don't sleep if something is already pending
  if (digitalRead(P_LORA_DIO_1) == HIGH) return;
  if (digitalRead(MAX_KB_INT) == LOW) return;
  if (digitalRead(MAX_BOOT_BTN) == LOW) return;

  Serial.flush();
  esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
  if (max_ms > 0) esp_sleep_enable_timer_wakeup((uint64_t)max_ms * 1000ULL);

  // Level-triggered wake-up pins also raise level interrupts. If interrupts are
  // live when we wake with a key still held, they fire continuously and the
  // interrupt watchdog resets the chip. So keep interrupts masked across the whole
  // sleep and restore the normal interrupt types before unmasking (same approach as
  // ESP32Board::sleep()).
  static portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
  portENTER_CRITICAL(&mux);
  if (gpio_get_level((gpio_num_t)P_LORA_DIO_1) == HIGH || gpio_get_level((gpio_num_t)MAX_KB_INT) == LOW ||
      gpio_get_level((gpio_num_t)MAX_BOOT_BTN) == LOW) {
    portEXIT_CRITICAL(&mux);     // something arrived while we were preparing
    return;
  }
  gpio_wakeup_enable((gpio_num_t)P_LORA_DIO_1, GPIO_INTR_HIGH_LEVEL);
  gpio_wakeup_enable((gpio_num_t)MAX_KB_INT, GPIO_INTR_LOW_LEVEL);
  gpio_wakeup_enable((gpio_num_t)MAX_BOOT_BTN, GPIO_INTR_LOW_LEVEL);
  esp_sleep_enable_gpio_wakeup();

  esp_light_sleep_start();

  gpio_wakeup_disable((gpio_num_t)P_LORA_DIO_1);
  gpio_wakeup_disable((gpio_num_t)MAX_KB_INT);
  gpio_wakeup_disable((gpio_num_t)MAX_BOOT_BTN);
  gpio_set_intr_type((gpio_num_t)MAX_KB_INT, GPIO_INTR_DISABLE);      // polled, no ISR
  gpio_set_intr_type((gpio_num_t)MAX_BOOT_BTN, GPIO_INTR_DISABLE);    // polled, no ISR
  gpio_set_intr_type((gpio_num_t)P_LORA_DIO_1, GPIO_INTR_POSEDGE);    // RadioLib's rising-edge ISR
  portEXIT_CRITICAL(&mux);
}

void TDeckMaxBoard::powerOff() {
  setFrontlight(0);
  setKeyboardLight(false);
#ifdef DISPLAY_CLASS
  display.turnOff();
#endif
  radio_driver.powerOff();
  digitalWrite(P_LORA_NSS, HIGH);
  rtc_gpio_hold_en((gpio_num_t)P_LORA_NSS);

  // cut every switchable rail
  xlPin(XL_GPS_EN, false);
  xlPin(XL_MODEM_EN, false);
  xlPin(XL_1V8_EN, false);
  xlPin(XL_AMP_EN, false);
  xlPin(XL_MOTOR_EN, false);
  xlPin(XL_LORA_EN, false);

  Serial.flush();
  delay(50);

  // wait for the BOOT button to be released, otherwise we'd wake instantly
  while (digitalRead(MAX_BOOT_BTN) == LOW) delay(10);
  delay(100);

  esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
  esp_sleep_enable_ext0_wakeup((gpio_num_t)MAX_BOOT_BTN, 0);
  rtc_gpio_pullup_en((gpio_num_t)MAX_BOOT_BTN);
  esp_deep_sleep_start();
}
