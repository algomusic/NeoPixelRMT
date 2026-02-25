/*
  BasicColors - NeoPixelRMT example

  Cycles through red, green, blue, and a rainbow pattern
  on a NeoPixel ring/strip using non-blocking RMT output.

  Wiring: NeoPixel DIN to GPIO 47 (change LED_PIN below)

  For ESP32 boards only.
*/

#include "NeoPixelRMT.h"

#define NUM_PIXELS 24
#define LED_PIN    47

NeoPixelRMT pixels(NUM_PIXELS, LED_PIN, NEO_GRB);

void setup() {
  pixels.begin();
  pixels.setBrightness(30);
}

void loop() {
  // Solid red
  colorWipe(pixels.Color(255, 0, 0), 50);
  delay(500);

  // Solid green
  colorWipe(pixels.Color(0, 255, 0), 50);
  delay(500);

  // Solid blue
  colorWipe(pixels.Color(0, 0, 255), 50);
  delay(500);

  // Rainbow cycle
  for (int j = 0; j < 256; j++) {
    for (int i = 0; i < pixels.numPixels(); i++) {
      pixels.setPixelColor(i, wheel((i * 256 / pixels.numPixels() + j) & 255));
    }
    pixels.show();
    delay(10);
  }
}

// Fill pixels one by one with a color
void colorWipe(uint32_t color, int wait) {
  for (int i = 0; i < pixels.numPixels(); i++) {
    pixels.setPixelColor(i, color);
    pixels.show();
    delay(wait);
  }
}

// Generate a color from a position on a 256-step color wheel
uint32_t wheel(byte pos) {
  if (pos < 85) {
    return pixels.Color(pos * 3, 255 - pos * 3, 0);
  } else if (pos < 170) {
    pos -= 85;
    return pixels.Color(255 - pos * 3, 0, pos * 3);
  } else {
    pos -= 170;
    return pixels.Color(0, pos * 3, 255 - pos * 3);
  }
}
