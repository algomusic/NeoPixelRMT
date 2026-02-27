# NeoPixelRMT

Non-blocking NeoPixel driver for ESP32 using the RMT hardware peripheral with DMA.

## Why?

The Adafruit NeoPixel library disables interrupts during `show()` for ~720µs, which causes glitches in real-time audio and other timing-sensitive applications. NeoPixelRMT uses the ESP32's RMT peripheral with DMA to transmit LED data asynchronously — `show()` returns immediately while the hardware handles the rest, with zero CPU involvement during transmission.

### Why DMA?

Without DMA, the RMT peripheral has only 48 symbols of FIFO memory per channel. For a strip of 24 RGBW LEDs (768 symbols), the driver must refill the FIFO ~32 times via interrupts. If any interrupt is delayed (by WiFi, audio ISRs, etc.), the FIFO runs empty, the data line drops low, and the LEDs interpret this as a reset — causing visible flicker on LEDs further down the chain. DMA bypasses this entirely by feeding data directly from memory to the RMT peripheral.

## Features

- Drop-in replacement for Adafruit_NeoPixel API
- Non-blocking `show()` via ESP-IDF RMT driver with DMA
- Automatic fallback to non-DMA mode (4 memory blocks) if DMA is unavailable
- Supports RGB, GRB, RGBW, and GRBW color orders
- Dedicated `setPixelColorW()` for efficient white channel access on RGBW LEDs
- Header-only — no `.cpp` files to compile
- Brightness applied at show-time, buffer stores full color values
- Proper 280µs reset code appended to each transmission
- Thread-safe: waits for previous DMA transmission to complete before modifying the TX buffer

## Installation

Copy the `NeoPixelRMT` folder into your Arduino `libraries/` directory.

## Usage

```cpp
#include "NeoPixelRMT.h"

#define NUM_PIXELS 24
#define LED_PIN    48

NeoPixelRMT pixels(NUM_PIXELS, LED_PIN, NEO_GRB);

void setup() {
  pixels.begin();
  pixels.setBrightness(30);
}

void loop() {
  for (int i = 0; i < pixels.numPixels(); i++) {
    pixels.setPixelColor(i, pixels.Color(255, 0, 0));
  }
  pixels.show();
  delay(500);
}
```

### RGBW Example

```cpp
NeoPixelRMT pixels(NUM_PIXELS, LED_PIN, NEO_GRBW);

// Set a pixel using RGB channels
pixels.setPixelColor(0, pixels.Color(255, 0, 0));

// Set a pixel using the dedicated white LED (more efficient, lower power)
pixels.setPixelColorW(0, 150);

// Set a pixel with both RGB and white channels
pixels.setPixelColor(0, 100, 50, 0, 200); // r, g, b, w
```

## API

| Method | Description |
|--------|-------------|
| `NeoPixelRMT(numPixels, pin, colorOrder)` | Constructor. Color order: `NEO_RGB`, `NEO_GRB`, `NEO_RGBW`, `NEO_GRBW` |
| `begin()` | Initialize RMT DMA channel and allocate buffers |
| `show()` | Transmit pixel data (non-blocking via DMA) |
| `setPixelColor(i, r, g, b, w)` | Set pixel by RGB(W) components. `w` defaults to 0 |
| `setPixelColor(i, color)` | Set pixel by packed 32-bit color |
| `setPixelColorW(i, w)` | Set pixel to white-only (RGBW LEDs). Clears RGB, sets W channel |
| `Color(r, g, b, w)` | Pack RGB(W) into a 32-bit value. `w` defaults to 0 |
| `getPixelColor(i)` | Get packed color of a pixel |
| `setBrightness(b)` | Set global brightness (0-255) |
| `getBrightness()` | Get current brightness |
| `numPixels()` | Get pixel count |
| `clear()` | Set all pixels to black |

## Technical Details

- Uses the ESP-IDF 5.x RMT driver directly (not the Arduino `rmtWrite` wrapper)
- Custom NeoPixel encoder using `rmt_bytes_encoder` for bit-level encoding
- WS2812B/SK6812 compatible timing: T0H=400ns, T0L=800ns, T1H=800ns, T1L=400ns
- DMA buffer: 1024 symbols (Espressif recommended)
- Non-DMA fallback: 192 symbols (4 x 48-symbol blocks)
- 10MHz RMT clock resolution (100ns per tick)

## Requirements

- ESP32-S3 (or other ESP32 variant with RMT DMA support) with Arduino ESP32 core 3.x+
- WS2812B / SK6812 or compatible addressable LEDs

## License

MIT
