/*
  NeoPixelRMT.h - Non-blocking NeoPixel driver using ESP32 RMT hardware with DMA

  Uses the ESP32-S3's RMT peripheral with DMA for glitch-free async LED
  transmission. DMA bypasses the interrupt-driven FIFO ping-pong that causes
  LED flicker when interrupts are delayed (e.g., by WiFi, audio, or other ISRs).

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

// ---- Custom RMT encoder for NeoPixel byte data ----
// This encodes raw pixel bytes into RMT symbols on-the-fly,
// so the DMA feeds symbols directly without a pre-built buffer.

typedef struct {
  rmt_encoder_t base;
  rmt_encoder_t *bytes_encoder;
  rmt_encoder_t *copy_encoder;
  int state;
  rmt_symbol_word_t reset_code;
} neopixel_encoder_t;

static size_t neopixel_encode(rmt_encoder_t *encoder, rmt_channel_handle_t channel,
                               const void *primary_data, size_t data_size,
                               rmt_encode_state_t *ret_state) {
  neopixel_encoder_t *neo_enc = __containerof(encoder, neopixel_encoder_t, base);
  rmt_encode_state_t session_state = RMT_ENCODING_RESET;
  rmt_encode_state_t state = RMT_ENCODING_RESET;
  size_t encoded_symbols = 0;

  switch (neo_enc->state) {
    case 0: // encode pixel data
      encoded_symbols += neo_enc->bytes_encoder->encode(
        neo_enc->bytes_encoder, channel, primary_data, data_size, &session_state);
      if (session_state & RMT_ENCODING_COMPLETE) {
        neo_enc->state = 1; // move to reset code
      }
      if (session_state & RMT_ENCODING_MEM_FULL) {
        state = (rmt_encode_state_t)(state | RMT_ENCODING_MEM_FULL);
        *ret_state = state;
        return encoded_symbols;
      }
      // fall through to send reset
    case 1: // send reset code (low for >80us)
      encoded_symbols += neo_enc->copy_encoder->encode(
        neo_enc->copy_encoder, channel, &neo_enc->reset_code,
        sizeof(rmt_symbol_word_t), &session_state);
      if (session_state & RMT_ENCODING_COMPLETE) {
        neo_enc->state = RMT_ENCODING_RESET; // back to initial state
        state = (rmt_encode_state_t)(state | RMT_ENCODING_COMPLETE);
      }
      if (session_state & RMT_ENCODING_MEM_FULL) {
        state = (rmt_encode_state_t)(state | RMT_ENCODING_MEM_FULL);
      }
      break;
  }
  *ret_state = state;
  return encoded_symbols;
}

static esp_err_t neopixel_encoder_del(rmt_encoder_t *encoder) {
  neopixel_encoder_t *neo_enc = __containerof(encoder, neopixel_encoder_t, base);
  rmt_del_encoder(neo_enc->bytes_encoder);
  rmt_del_encoder(neo_enc->copy_encoder);
  free(neo_enc);
  return ESP_OK;
}

static esp_err_t neopixel_encoder_reset(rmt_encoder_t *encoder) {
  neopixel_encoder_t *neo_enc = __containerof(encoder, neopixel_encoder_t, base);
  rmt_encoder_reset(neo_enc->bytes_encoder);
  rmt_encoder_reset(neo_enc->copy_encoder);
  neo_enc->state = RMT_ENCODING_RESET;
  return ESP_OK;
}

class NeoPixelRMT {
public:
  NeoPixelRMT(uint16_t numPixels, int pin, uint8_t colorOrder = NEO_GRB)
    : _numPixels(numPixels), _pin(pin), _colorOrder(colorOrder),
      _brightness(255), _pixelBuffer(nullptr), _txBuffer(nullptr),
      _channel(nullptr), _encoder(nullptr), _txPending(false) {
    _bytesPerLed = (_colorOrder >= NEO_RGBW) ? 4 : 3;
  }

  ~NeoPixelRMT() {
    if (_encoder) rmt_del_encoder(_encoder);
    if (_channel) {
      rmt_disable(_channel);
      rmt_del_channel(_channel);
    }
    if (_pixelBuffer) free(_pixelBuffer);
    if (_txBuffer) free(_txBuffer);
  }

  void begin() {
    // Allocate pixel buffer (always 4 bytes per pixel for uniform indexing)
    _pixelBuffer = (uint8_t*)calloc(_numPixels * 4, sizeof(uint8_t));
    // TX buffer holds the reordered/brightness-adjusted bytes for transmission
    _txBuffer = (uint8_t*)calloc(_numPixels * _bytesPerLed, sizeof(uint8_t));

    // Configure RMT TX channel with DMA for glitch-free transmission
    rmt_tx_channel_config_t tx_cfg = {};
    tx_cfg.gpio_num = (gpio_num_t)_pin;
    tx_cfg.clk_src = RMT_CLK_SRC_DEFAULT;
    tx_cfg.resolution_hz = 10000000; // 10MHz = 100ns per tick
    tx_cfg.mem_block_symbols = 1024; // DMA buffer size (recommended by Espressif)
    tx_cfg.trans_queue_depth = 1;
    tx_cfg.flags.with_dma = true; // Enable DMA - bypasses interrupt-driven FIFO refill
    // tx_cfg.flags.invert_out = false;
    // tx_cfg.flags.io_loop_back = false;
    // tx_cfg.flags.io_od_mode = false;
    // tx_cfg.flags.allow_pd = false;

    esp_err_t err = rmt_new_tx_channel(&tx_cfg, &_channel);
    if (err != ESP_OK) {
      // DMA failed (maybe only 1 DMA channel available and already in use)
      // Fall back to non-DMA with maximum memory blocks
      tx_cfg.flags.with_dma = false;
      tx_cfg.mem_block_symbols = 192; // 4 blocks x 48 symbols
      rmt_new_tx_channel(&tx_cfg, &_channel);
    }

    // Create the NeoPixel encoder (bytes encoder + reset code)
    _createEncoder();

    // Enable the channel
    rmt_enable(_channel);
  }

  void show() {
    if (!_pixelBuffer || !_txBuffer || !_channel || !_encoder) return;

    // Wait for any previous transmission to complete BEFORE we touch _txBuffer
    if (_txPending) {
      rmt_tx_wait_all_done(_channel, portMAX_DELAY);
      _txPending = false;
    }

    // Build TX buffer: apply brightness and reorder bytes
    int txIdx = 0;
    for (int led = 0; led < _numPixels; led++) {
      uint8_t* px = &_pixelBuffer[led * 4];
      uint8_t r = (px[0] * _brightness) >> 8;
      uint8_t g = (px[1] * _brightness) >> 8;
      uint8_t b = (px[2] * _brightness) >> 8;
      uint8_t w = (px[3] * _brightness) >> 8;

      switch (_colorOrder) {
        case NEO_RGB:
          _txBuffer[txIdx++] = r; _txBuffer[txIdx++] = g; _txBuffer[txIdx++] = b;
          break;
        case NEO_GRB:
          _txBuffer[txIdx++] = g; _txBuffer[txIdx++] = r; _txBuffer[txIdx++] = b;
          break;
        case NEO_RGBW:
          _txBuffer[txIdx++] = r; _txBuffer[txIdx++] = g;
          _txBuffer[txIdx++] = b; _txBuffer[txIdx++] = w;
          break;
        case NEO_GRBW:
          _txBuffer[txIdx++] = g; _txBuffer[txIdx++] = r;
          _txBuffer[txIdx++] = b; _txBuffer[txIdx++] = w;
          break;
        default:
          _txBuffer[txIdx++] = g; _txBuffer[txIdx++] = r; _txBuffer[txIdx++] = b;
          break;
      }
    }

    // Transmit via RMT with DMA - non-blocking, returns immediately
    rmt_transmit_config_t tx_config = {};
    tx_config.loop_count = 0; // no loop
    rmt_transmit(_channel, _encoder, _txBuffer, _numPixels * _bytesPerLed, &tx_config);
    _txPending = true;
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

  // Helper for direct white channel access on RGBW, or RGB white on RGB LEDs
  void setPixelColorW(int i, uint8_t w) {
    if (i < 0 || i >= _numPixels || !_pixelBuffer) return;
    if (_bytesPerLed == 4) {
      // RGBW: use dedicated white channel
      _pixelBuffer[i * 4]     = 0;
      _pixelBuffer[i * 4 + 1] = 0;
      _pixelBuffer[i * 4 + 2] = 0;
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

private:
  uint16_t _numPixels;
  int _pin;
  uint8_t _colorOrder;
  uint8_t _brightness;
  uint8_t _bytesPerLed;
  uint8_t* _pixelBuffer;
  uint8_t* _txBuffer;
  rmt_channel_handle_t _channel;
  rmt_encoder_t* _encoder;
  bool _txPending;

  void _createEncoder() {
    neopixel_encoder_t *neo_enc = (neopixel_encoder_t*)calloc(1, sizeof(neopixel_encoder_t));

    neo_enc->base.encode = neopixel_encode;
    neo_enc->base.del = neopixel_encoder_del;
    neo_enc->base.reset = neopixel_encoder_reset;

    // Bytes encoder: converts each byte to 8 RMT symbols (MSB first)
    // WS2812B / SK6812 compatible timing at 10MHz (100ns/tick)
    rmt_bytes_encoder_config_t bytes_cfg = {};
    bytes_cfg.bit0.duration0 = 4;  // T0H: 400ns
    bytes_cfg.bit0.level0 = 1;
    bytes_cfg.bit0.duration1 = 8;  // T0L: 800ns
    bytes_cfg.bit0.level1 = 0;
    bytes_cfg.bit1.duration0 = 8;  // T1H: 800ns
    bytes_cfg.bit1.level0 = 1;
    bytes_cfg.bit1.duration1 = 4;  // T1L: 400ns
    bytes_cfg.bit1.level1 = 0;
    bytes_cfg.flags.msb_first = true;

    rmt_new_bytes_encoder(&bytes_cfg, &neo_enc->bytes_encoder);

    // Copy encoder for the reset code (low for 280us = 2800 ticks at 10MHz)
    rmt_copy_encoder_config_t copy_cfg = {};
    rmt_new_copy_encoder(&copy_cfg, &neo_enc->copy_encoder);

    neo_enc->reset_code.duration0 = 2800; // 280us low (reset signal)
    neo_enc->reset_code.level0 = 0;
    neo_enc->reset_code.duration1 = 0;
    neo_enc->reset_code.level1 = 0;

    neo_enc->state = RMT_ENCODING_RESET;

    _encoder = &neo_enc->base;
  }
};

#endif // NEOPIXELRMT_H
