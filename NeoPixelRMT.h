/*
  NeoPixelRMT.h - Non-blocking NeoPixel driver using ESP32 RMT hardware

  Uses the ESP32's RMT peripheral for async LED data transmission,
  avoiding the interrupt-disabling behavior of Adafruit_NeoPixel::show()
  which causes audio glitches in real-time audio applications.

  Drop-in replacement for Adafruit_NeoPixel with compatible API.

  Created 2025, Andrew R. Brown
  MIT License
*/

#ifndef NEOPIXELRMT_H
#define NEOPIXELRMT_H

#include <Arduino.h>
#include "driver/rmt_tx.h"

// Color order constants (Adafruit-compatible naming)
#define NEO_RGB  0
#define NEO_GRB  1
#define NEO_RGBW 2
#define NEO_GRBW 3

class NeoPixelRMT {
public:
  NeoPixelRMT(uint16_t numPixels, int pin, uint8_t colorOrder = NEO_GRB)
    : _numPixels(numPixels), _pin(pin), _colorOrder(colorOrder),
      _brightness(255), _rmtData(nullptr), _pixelBuffer(nullptr) {
    _bytesPerLed = (_colorOrder >= NEO_RGBW) ? 4 : 3;
    _bitsPerLed = _bytesPerLed * 8;
  }

  ~NeoPixelRMT() {
    if (_rmtData) free(_rmtData);
    if (_pixelBuffer) free(_pixelBuffer);
  }

  void begin() {
    // Allocate buffers
    _rmtData = (rmt_data_t*)calloc(_numPixels * _bitsPerLed, sizeof(rmt_data_t));
    _pixelBuffer = (uint8_t*)calloc(_numPixels * 4, sizeof(uint8_t)); // Always 4 bytes (RGBW), W unused for 3-byte types
    // 10MHz = 100ns ticks
    rmtInit(_pin, RMT_TX_MODE, RMT_MEM_NUM_BLOCKS_1, 10000000);
  }

  void show() {
    if (!_rmtData || !_pixelBuffer) return;
    int i = 0;
    for (int led = 0; led < _numPixels; led++) {
      uint8_t* px = &_pixelBuffer[led * 4];
      // Apply brightness
      uint8_t r = (px[0] * _brightness) >> 8;
      uint8_t g = (px[1] * _brightness) >> 8;
      uint8_t b = (px[2] * _brightness) >> 8;
      uint8_t w = (px[3] * _brightness) >> 8;
      // Arrange bytes per color order
      uint8_t colors[4];
      switch (_colorOrder) {
        case NEO_RGB:
          colors[0] = r; colors[1] = g; colors[2] = b;
          break;
        case NEO_GRB:
          colors[0] = g; colors[1] = r; colors[2] = b;
          break;
        case NEO_RGBW:
          colors[0] = r; colors[1] = g; colors[2] = b; colors[3] = w;
          break;
        case NEO_GRBW:
          colors[0] = g; colors[1] = r; colors[2] = b; colors[3] = w;
          break;
        default:
          colors[0] = g; colors[1] = r; colors[2] = b; // default GRB
          break;
      }
      for (int col = 0; col < _bytesPerLed; col++) {
        for (int bit = 7; bit >= 0; bit--) {
          if (colors[col] & (1 << bit)) {
            _rmtData[i] = {8, 1, 4, 0};  // "1": 800ns high, 400ns low
          } else {
            _rmtData[i] = {4, 1, 8, 0};  // "0": 400ns high, 800ns low
          }
          i++;
        }
      }
    }
    // Non-blocking: returns immediately, RMT hardware handles transmission
    if (rmtTransmitCompleted(_pin)) {
      rmtWriteAsync(_pin, _rmtData, _numPixels * _bitsPerLed);
    }
  }

  void setBrightness(uint8_t b) { _brightness = b; }

  uint8_t getBrightness() const { return _brightness; }

  void setPixelColor(int i, uint8_t r, uint8_t g, uint8_t b, uint8_t w = 0) {
    if (i < 0 || i >= _numPixels || !_pixelBuffer) return;
    _pixelBuffer[i * 4]     = r;
    _pixelBuffer[i * 4 + 1] = g;
    _pixelBuffer[i * 4 + 2] = b;
    _pixelBuffer[i * 4 + 3] = w;
  }

  void setPixelColor(int i, uint32_t c) {
    setPixelColor(i, (c >> 16) & 0xFF, (c >> 8) & 0xFF, c & 0xFF, (c >> 24) & 0xFF);
  }

  uint32_t Color(uint8_t r, uint8_t g, uint8_t b, uint8_t w = 0) {
    return ((uint32_t)w << 24) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
  }

  uint32_t getPixelColor(int i) const {
    if (i < 0 || i >= _numPixels || !_pixelBuffer) return 0;
    return ((uint32_t)_pixelBuffer[i * 4 + 3] << 24) |
           ((uint32_t)_pixelBuffer[i * 4] << 16) |
           ((uint32_t)_pixelBuffer[i * 4 + 1] << 8) |
           _pixelBuffer[i * 4 + 2];
  }

  uint16_t numPixels() const { return _numPixels; }

  void clear() {
    if (_pixelBuffer) memset(_pixelBuffer, 0, _numPixels * 4);
  }

private:
  uint16_t _numPixels;
  int _pin;
  uint8_t _colorOrder;
  uint8_t _brightness;
  uint8_t _bytesPerLed;
  uint8_t _bitsPerLed;
  rmt_data_t* _rmtData;
  uint8_t* _pixelBuffer;
};

#endif // NEOPIXELRMT_H
