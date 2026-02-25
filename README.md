# NeoPixelRMT

Non-blocking NeoPixel driver for ESP32 using the RMT hardware peripheral.

## Why?

Other NeoPixel libraries can disable interrupts during `show()`, which causes glitches in real-time audio and other timing-sensitive applications. NeoPixelRMT uses the ESP32's RMT peripheral to transmit LED data asynchronously — `show()` returns immediately while the hardware handles the rest.

## Features

- Drop-in replacement for Adafruit_NeoPixel API
- Non-blocking `show()` via `rmtWriteAsync()`
- Supports RGB, GRB, RGBW, and GRBW color orders
- Brightness applied at show-time, buffer stores full color values

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

## API

| Method | Description |
|--------|-------------|
| `NeoPixelRMT(numPixels, pin, colorOrder)` | Constructor. Color order: `NEO_RGB`, `NEO_GRB`, `NEO_RGBW`, `NEO_GRBW` |
| `begin()` | Initialize RMT hardware and allocate buffers |
| `show()` | Transmit pixel data (non-blocking) |
| `setPixelColor(i, r, g, b)` | Set pixel by RGB components |
| `setPixelColor(i, color)` | Set pixel by packed 32-bit color |
| `Color(r, g, b, w)` | Pack RGB(W) into a 32-bit value |
| `getPixelColor(i)` | Get packed color of a pixel |
| `setBrightness(b)` | Set global brightness (0-255) |
| `getBrightness()` | Get current brightness |
| `numPixels()` | Get pixel count |
| `clear()` | Set all pixels to black |

## Requirements

- ESP32 board with Arduino ESP32 core (3.x+)
- WS2812B / SK6812 or compatible addressable LEDs

## License

MIT
