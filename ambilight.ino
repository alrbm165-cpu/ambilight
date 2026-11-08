// Ambilight receiver for ESP32-S3 (Adalight protocol, works with Prismatik)
// Strip: WS2812B / RGBIC 5V, data on DATA_PIN
#include <FastLED.h>

// ---------------- settings ----------------
#define NUM_LEDS    30      // MUST equal the total LED count set in Prismatik
#define DATA_PIN    5       // GPIO 5 -> strip DIN (green wire)
#define BRIGHTNESS  150     // 0-255
#define MAX_MA      500     // power limit; raise to ~1500 with a phone charger
#define BAUD        115200  // same speed as in Prismatik
#define TIMEOUT_MS  5000    // strip turns off if no data arrives for this long
// ------------------------------------------

CRGB leds[NUM_LEDS];
uint32_t lastData = 0;
bool active = false;

// read one byte, return -1 on timeout
int readByte(uint16_t timeoutMs = 200) {
  uint32_t t = millis();
  while (!Serial.available()) {
    if (millis() - t > timeoutMs) return -1;
    yield();
  }
  return Serial.read();
}

void setup() {
  Serial.setRxBufferSize(2048);
  Serial.begin(BAUD);

  FastLED.addLeds<WS2812B, DATA_PIN, GRB>(leds, NUM_LEDS);
  FastLED.setBrightness(BRIGHTNESS);
  FastLED.setMaxPowerInVoltsAndMilliamps(5, MAX_MA);

  // startup self-test: red, green, blue (proves wiring works without any PC data)
  const CRGB colors[3] = {CRGB::Red, CRGB::Green, CRGB::Blue};
  for (int c = 0; c < 3; c++) {
    fill_solid(leds, NUM_LEDS, colors[c]);
    FastLED.show();
    delay(400);
  }
  FastLED.clear();
  FastLED.show();
}

void loop() {
  if (!Serial.available()) {
    if (active && millis() - lastData > TIMEOUT_MS) {
      FastLED.clear();
      FastLED.show();
      active = false;
    }
    return;
  }

  // header: 'A' 'd' 'a' hi lo chk
  if (Serial.read() != 'A') return;
  if (readByte() != 'd') return;
  if (readByte() != 'a') return;
  int hi = readByte(), lo = readByte(), chk = readByte();
  if (hi < 0 || lo < 0 || chk < 0) return;
  if (chk != (hi ^ lo ^ 0x55)) return;

  int n = ((hi << 8) | lo) + 1;   // LED count in this frame
  for (int i = 0; i < n; i++) {
    int r = readByte(), g = readByte(), b = readByte();
    if (r < 0 || g < 0 || b < 0) return;   // incomplete frame, drop it
    if (i < NUM_LEDS) leds[i] = CRGB(r, g, b);
  }
  FastLED.show();
  lastData = millis();
  active = true;
}
