/*
  NeoPixelRMT.h - Non-blocking NeoPixel driver using ESP32 RMT hardware

  Uses the ESP32-S3's RMT peripheral for async LED transmission.
  Pre-builds the complete RMT symbol buffer before transmission to eliminate
  FIFO underrun gaps that can cause data corruption on marginal signal lines.

  Drop-in replacement for Adafruit_NeoPixel with compatible API.

  Created 2025, Andrew R. Brown
  MIT License
*/

#ifndef NEOPIXELRMT_H
#define NEOPIXELRMT_H

#include <Arduino.h>
#include "driver/rmt_tx.h"
#include "driver/rmt_encoder.h"

// Color order constants (Adafruit-compatible naming)
#define NEO_RGB  0
#define NEO_GRB  1
#define NEO_RGBW 2
#define NEO_GRBW 3

// ---- Simple copy encoder: sends a pre-built rmt_symbol_word_t buffer as-is ----

typedef struct {
  rmt_encoder_t base;
  rmt_encoder_t *copy_encoder;
} neopixel_copy_encoder_t;

static size_t neopixel_copy_encode(rmt_encoder_t *encoder, rmt_channel_handle_t channel,
                                    const void *primary_data, size_t data_size,
                                    rmt_encode_state_t *ret_state) {
  neopixel_copy_encoder_t *enc = __containerof(encoder, neopixel_copy_encoder_t, base);
  return enc->copy_encoder->encode(enc->copy_encoder, channel, primary_data, data_size, ret_state);
}

static esp_err_t neopixel_copy_encoder_del(rmt_encoder_t *encoder) {
  neopixel_copy_encoder_t *enc = __containerof(encoder, neopixel_copy_encoder_t, base);
  rmt_del_encoder(enc->copy_encoder);
  free(enc);
  return ESP_OK;
}

static esp_err_t neopixel_copy_encoder_reset(rmt_encoder_t *encoder) {
  neopixel_copy_encoder_t *enc = __containerof(encoder, neopixel_copy_encoder_t, base);
  rmt_encoder_reset(enc->copy_encoder);
  return ESP_OK;
}

class NeoPixelRMT {
public:
  NeoPixelRMT(uint16_t numPixels, int pin, uint8_t colorOrder = NEO_GRB)
    : _numPixels(numPixels), _pin(pin), _colorOrder(colorOrder),
      _brightness(255), _pixelBuffer(nullptr), _rmtSymbols(nullptr),
      _channel(nullptr), _encoder(nullptr), _txPending(false),
      _suppressWhite(false), _dynamicCap(false), _maxChannelVal(255),
      _capHeadroom(2.0f) {
    _bytesPerLed = (_colorOrder >= NEO_RGBW) ? 4 : 3;
  }

  ~NeoPixelRMT() {
    if (_encoder) rmt_del_encoder(_encoder);
    if (_channel) {
      rmt_disable(_channel);
      rmt_del_channel(_channel);
    }
    if (_pixelBuffer) free(_pixelBuffer);
    if (_rmtSymbols) free(_rmtSymbols);
  }

  void begin() {
    // Allocate pixel buffer (always 4 bytes per pixel for uniform indexing)
    _pixelBuffer = (uint8_t*)calloc(_numPixels * 4, sizeof(uint8_t));

    // Pre-allocate RMT symbol buffer: 8 symbols per byte + 1 reset symbol
    _numSymbols = _numPixels * _bytesPerLed * 8 + 1;
    _rmtSymbols = (rmt_symbol_word_t*)calloc(_numSymbols, sizeof(rmt_symbol_word_t));

    // Set up timing constants (SK6812 at 10MHz = 100ns/tick)
    // Slower period (1.4µs vs 1.2µs min) for signal settling on marginal voltage lines
    _bit0.duration0 = 4;   // T0H: 400ns
    _bit0.level0 = 1;
    _bit0.duration1 = 10;  // T0L: 1000ns
    _bit0.level1 = 0;
    _bit1.duration0 = 8;   // T1H: 800ns
    _bit1.level0 = 1;
    _bit1.duration1 = 6;   // T1L: 600ns
    _bit1.level1 = 0;

    // Configure RMT TX channel — non-DMA to avoid bus contention with I2S audio DMA
    // Still non-blocking: show() returns immediately, RMT transmits via FIFO refill
    rmt_tx_channel_config_t tx_cfg = {};
    tx_cfg.gpio_num = (gpio_num_t)_pin;
    tx_cfg.clk_src = RMT_CLK_SRC_DEFAULT;
    tx_cfg.resolution_hz = 10000000; // 10MHz = 100ns per tick
    tx_cfg.mem_block_symbols = 192;  // 4 blocks × 48 symbols
    tx_cfg.trans_queue_depth = 1;
    tx_cfg.flags.with_dma = false;

    esp_err_t err = rmt_new_tx_channel(&tx_cfg, &_channel);
    if (err != ESP_OK) {
      tx_cfg.mem_block_symbols = 48; // 1 block fallback
      rmt_new_tx_channel(&tx_cfg, &_channel);
    }

    // Create a simple copy encoder — just sends our pre-built symbol buffer
    _createCopyEncoder();

    rmt_enable(_channel);
  }

  void show() {
    if (!_pixelBuffer || !_rmtSymbols || !_channel || !_encoder) return;

    // Wait for any previous transmission to complete
    if (_txPending) {
      rmt_tx_wait_all_done(_channel, portMAX_DELAY);
      _txPending = false;
    }

    // Build the complete RMT symbol buffer from pixel data
    int symIdx = 0;
    for (int led = 0; led < _numPixels; led++) {
      uint8_t* px = &_pixelBuffer[led * 4];
      uint8_t r = min((uint8_t)((px[0] * _brightness) >> 8), _maxChannelVal);
      uint8_t g = min((uint8_t)((px[1] * _brightness) >> 8), _maxChannelVal);
      uint8_t b = min((uint8_t)((px[2] * _brightness) >> 8), _maxChannelVal);
      uint8_t w = _suppressWhite ? 0 : min((uint8_t)((px[3] * _brightness) >> 8), _maxChannelVal);

      // Encode bytes in color order
      switch (_colorOrder) {
        case NEO_RGB:
          _encodeByte(r, symIdx); _encodeByte(g, symIdx); _encodeByte(b, symIdx);
          break;
        case NEO_GRB:
          _encodeByte(g, symIdx); _encodeByte(r, symIdx); _encodeByte(b, symIdx);
          break;
        case NEO_RGBW:
          _encodeByte(r, symIdx); _encodeByte(g, symIdx);
          _encodeByte(b, symIdx); _encodeByte(w, symIdx);
          break;
        case NEO_GRBW:
          _encodeByte(g, symIdx); _encodeByte(r, symIdx);
          _encodeByte(b, symIdx); _encodeByte(w, symIdx);
          break;
        default:
          _encodeByte(g, symIdx); _encodeByte(r, symIdx); _encodeByte(b, symIdx);
          break;
      }
    }

    // Append reset code (280µs low)
    _rmtSymbols[symIdx].duration0 = 2800;
    _rmtSymbols[symIdx].level0 = 0;
    _rmtSymbols[symIdx].duration1 = 0;
    _rmtSymbols[symIdx].level1 = 0;
    symIdx++;

    // Transmit — non-blocking, returns immediately
    rmt_transmit_config_t tx_config = {};
    tx_config.loop_count = 0;
    rmt_transmit(_channel, _encoder, _rmtSymbols, symIdx * sizeof(rmt_symbol_word_t), &tx_config);
    _txPending = true;
  }

  void setBrightness(uint8_t b) {
    _brightness = b;
    if (_dynamicCap) _updateMaxChannel();
  }

  uint8_t getBrightness() const { return _brightness; }

  // Suppress white channel on RGBW/GRBW LEDs — forces W=0 in TX buffer
  void setSuppressWhite(bool suppress) { _suppressWhite = suppress; }

  // Cap per-channel output value (0-255) to limit max brightness in TX buffer
  void setMaxChannelValue(uint8_t maxVal) { _maxChannelVal = maxVal; _dynamicCap = false; }

  // Auto-cap: keeps _maxChannelVal at headroom above max possible output for current brightness
  // headroom is a multiplier (e.g. 2.0 = cap at 2x the max normal output)
  void setDynamicChannelCap(float headroom = 2.0f) {
    _dynamicCap = true;
    _capHeadroom = headroom;
    _updateMaxChannel();
  }

  void setPixelColor(int i, uint8_t r, uint8_t g, uint8_t b, uint8_t w = 0) {
    if (i < 0 || i >= _numPixels || !_pixelBuffer) return;
    _pixelBuffer[i * 4]     = r;
    _pixelBuffer[i * 4 + 1] = g;
    _pixelBuffer[i * 4 + 2] = b;
    _pixelBuffer[i * 4 + 3] = w;
  }

  // Helper for direct white channel access on RGBW, or RGB white on RGB LEDs
  void setPixelColorW(int i, uint8_t w) {
    if (i < 0 || i >= _numPixels || !_pixelBuffer) return;
    if (_bytesPerLed == 4) {
      // RGBW: set white channel, preserve existing RGB
      _pixelBuffer[i * 4 + 3] = w;
    } else {
      // RGB: set all channels to produce white
      _pixelBuffer[i * 4]     = w;
      _pixelBuffer[i * 4 + 1] = w;
      _pixelBuffer[i * 4 + 2] = w;
      _pixelBuffer[i * 4 + 3] = 0;
    }
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

  // Clear only the white channel on all pixels (preserves RGB)
  void clearWhite() {
    if (!_pixelBuffer) return;
    for (int i = 0; i < _numPixels; i++) {
      _pixelBuffer[i * 4 + 3] = 0;
    }
  }

  // Clear white channel on a range of pixels (preserves RGB)
  void clearWhite(int from, int to) {
    if (!_pixelBuffer) return;
    if (from < 0) from = 0;
    if (to > _numPixels) to = _numPixels;
    for (int i = from; i < to; i++) {
      _pixelBuffer[i * 4 + 3] = 0;
    }
  }

private:
  uint16_t _numPixels;
  int _pin;
  uint8_t _colorOrder;
  uint8_t _brightness;
  uint8_t _bytesPerLed;
  uint8_t* _pixelBuffer;
  rmt_symbol_word_t* _rmtSymbols;  // Pre-built symbol buffer
  uint32_t _numSymbols;
  rmt_symbol_word_t _bit0;         // Cached timing for bit 0
  rmt_symbol_word_t _bit1;         // Cached timing for bit 1
  rmt_channel_handle_t _channel;
  rmt_encoder_t* _encoder;
  bool _txPending;
  bool _suppressWhite;
  bool _dynamicCap;
  uint8_t _maxChannelVal;
  float _capHeadroom;

  void _updateMaxChannel() {
    uint16_t maxOut = (255 * (uint16_t)_brightness) >> 8;
    uint16_t cap = (uint16_t)(maxOut * _capHeadroom);
    _maxChannelVal = cap > 255 ? 255 : (uint8_t)cap;
  }

  // Encode one byte (8 bits, MSB first) into 8 RMT symbols
  inline void _encodeByte(uint8_t val, int &symIdx) {
    for (int bit = 7; bit >= 0; bit--) {
      _rmtSymbols[symIdx++] = (val & (1 << bit)) ? _bit1 : _bit0;
    }
  }

  void _createCopyEncoder() {
    neopixel_copy_encoder_t *enc = (neopixel_copy_encoder_t*)calloc(1, sizeof(neopixel_copy_encoder_t));
    enc->base.encode = neopixel_copy_encode;
    enc->base.del = neopixel_copy_encoder_del;
    enc->base.reset = neopixel_copy_encoder_reset;

    rmt_copy_encoder_config_t copy_cfg = {};
    rmt_new_copy_encoder(&copy_cfg, &enc->copy_encoder);

    _encoder = &enc->base;
  }
};

#endif // NEOPIXELRMT_H
