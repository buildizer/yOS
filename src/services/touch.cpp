#include "services/touch.h"
#include <Arduino.h>
#include <Wire.h>

// GT911 capacitive controller on the Guition ESP32-4848S040.
#define TOUCH_SDA 19
#define TOUCH_SCL 45
#define TOUCH_W 480
#define TOUCH_H 480
#define TOUCH_FLIP_X false   // set if taps land mirrored relative to the display
#define TOUCH_FLIP_Y false

static const uint16_t REG_STATUS = 0x814E, REG_POINT1 = 0x8150;
static uint8_t addr = 0;

static bool readReg(uint16_t reg, uint8_t *buf, uint8_t len) {
  Wire.beginTransmission(addr);
  Wire.write(reg >> 8);
  Wire.write(reg & 0xFF);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom(addr, len) != len) return false;
  for (uint8_t i = 0; i < len; i++) buf[i] = Wire.read();
  return true;
}

static void writeReg(uint16_t reg, uint8_t value) {
  Wire.beginTransmission(addr);
  Wire.write(reg >> 8);
  Wire.write(reg & 0xFF);
  Wire.write(value);
  Wire.endTransmission();
}

bool touchStart() {
  Wire.begin(TOUCH_SDA, TOUCH_SCL, 400000);
  for (uint8_t a : {0x5D, 0x14}) {
    Wire.beginTransmission(a);
    if (Wire.endTransmission() == 0) {
      addr = a;
      Serial.printf("GT911 touch at 0x%02X\n", a);
      return true;
    }
  }
  Serial.println("GT911 touch not found");
  return false;
}

bool touchRead(int16_t &x, int16_t &y) {
  static bool down = false;
  static int16_t lx = 0, ly = 0;
  uint8_t status;
  if (addr && readReg(REG_STATUS, &status, 1) && (status & 0x80)) {
    uint8_t points = status & 0x0F;
    uint8_t p[4];
    if (points > 0 && points <= 5 && readReg(REG_POINT1, p, 4)) {
      lx = p[0] | (p[1] << 8);
      ly = p[2] | (p[3] << 8);
      if (TOUCH_FLIP_X) lx = TOUCH_W - 1 - lx;
      if (TOUCH_FLIP_Y) ly = TOUCH_H - 1 - ly;
      down = true;
    } else {
      down = false;
    }
    writeReg(REG_STATUS, 0);  // acknowledge so the controller reports the next frame
  }
  x = lx;
  y = ly;
  return down;
}
