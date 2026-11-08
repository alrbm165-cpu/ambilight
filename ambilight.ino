// Ambilight ESP32-S3 : Adalight (Prismatik) + standalone modes + WiFi control
// FQBN: esp32:esp32:esp32s3:CDCOnBoot=default   (ESP32 core 2.0.17, FastLED)
#include <FastLED.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>

#define NUM_LEDS    60
#define DATA_PIN    5
#define MAX_MA      500
#define BAUD        115200
#define TIMEOUT_MS  5000      // after this without Prismatik data -> back to chosen mode
#define BYTE_TO_MS  200       // incomplete frame -> reset parser
#define BOOT_PIN    0
#define AP_SSID     "Ambilight"
#define AP_PASS     "12345678"

enum Mode : uint8_t { M_OFF = 0, M_SOLID, M_RAINBOW, M_BREATH, M_COMET, M_COUNT };

CRGB leds[NUM_LEDS];
WebServer server(80);
Preferences prefs;

uint8_t mode = M_OFF;
uint8_t bright = 150;
CRGB color = CRGB(255, 147, 41);

bool streaming = false;
uint32_t lastData = 0;

bool dirty = false;
uint32_t dirtyAt = 0;

// ---------- Adalight parser (non-blocking state machine) ----------
enum PState : uint8_t { P_A, P_D, P_a, P_HI, P_LO, P_CHK, P_DATA };
PState ps = P_A;
uint8_t pHi, pLo, rgbBuf[3], rgbIdx;
uint16_t expectedLeds, ledIdx;
uint32_t lastByteMs = 0;

void feed(uint8_t b) {
  switch (ps) {
    case P_A:  if (b == 'A') ps = P_D; break;
    case P_D:  ps = (b == 'd') ? P_a : (b == 'A' ? P_D : P_A); break;
    case P_a:  ps = (b == 'a') ? P_HI : (b == 'A' ? P_D : P_A); break;
    case P_HI: pHi = b; ps = P_LO; break;
    case P_LO: pLo = b; ps = P_CHK; break;
    case P_CHK:
      if (b == (uint8_t)(pHi ^ pLo ^ 0x55)) {
        expectedLeds = (((uint16_t)pHi << 8) | pLo) + 1;
        ledIdx = 0; rgbIdx = 0; ps = P_DATA;
      } else ps = P_A;
      break;
    case P_DATA:
      rgbBuf[rgbIdx++] = b;
      if (rgbIdx == 3) {
        rgbIdx = 0;
        if (ledIdx < NUM_LEDS) leds[ledIdx] = CRGB(rgbBuf[0], rgbBuf[1], rgbBuf[2]);
        ledIdx++;
        if (ledIdx >= expectedLeds) {
          for (uint16_t i = min<uint16_t>(expectedLeds, NUM_LEDS); i < NUM_LEDS; i++) leds[i] = CRGB::Black;
          FastLED.show();
          lastData = millis();
          streaming = true;
          ps = P_A;
        }
      }
      break;
  }
}

// ---------- Standalone modes ----------
void runMode() {
  static uint8_t hue = 0;
  static uint8_t pos = 0;
  switch (mode) {
    case M_OFF:
      fill_solid(leds, NUM_LEDS, CRGB::Black);
      break;
    case M_SOLID:
      fill_solid(leds, NUM_LEDS, color);
      break;
    case M_RAINBOW:
      fill_rainbow(leds, NUM_LEDS, hue++, 255 / NUM_LEDS + 1);
      break;
    case M_BREATH: {
      CRGB c = color;
      c.nscale8(beatsin8(12, 15, 255));
      fill_solid(leds, NUM_LEDS, c);
      break;
    }
    case M_COMET:
      fadeToBlackBy(leds, NUM_LEDS, 45);
      leds[pos] = color;
      pos = (pos + 1) % NUM_LEDS;
      break;
  }
}

// ---------- Saving (delayed, protects flash) ----------
void markDirty() { dirty = true; dirtyAt = millis(); }

void saveIfDirty() {
  if (dirty && millis() - dirtyAt > 2000) {
    prefs.putUChar("mode", mode);
    prefs.putUChar("bri", bright);
    prefs.putUChar("r", color.r);
    prefs.putUChar("g", color.g);
    prefs.putUChar("b", color.b);
    dirty = false;
  }
}

// ---------- BOOT button: next mode ----------
void handleButton() {
  static bool last = true;
  static uint32_t t = 0;
  bool now = digitalRead(BOOT_PIN);
  if (now != last && millis() - t > 50) {
    t = millis();
    last = now;
    if (!now) {
      mode = (mode + 1) % M_COUNT;
      markDirty();
    }
  }
}

// ---------- Web ----------
const char PAGE[] PROGMEM = R"rawliteral(<!DOCTYPE html>
<html lang="ar" dir="rtl"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Ambilight</title>
<style>
body{font-family:sans-serif;background:#111;color:#eee;max-width:420px;margin:0 auto;padding:16px}
h2{text-align:center}
button{width:48%;margin:1%;padding:14px;font-size:17px;border:0;border-radius:10px;background:#333;color:#eee}
button.on{background:#e8590c}
input[type=range]{width:100%}
input[type=color]{width:100%;height:50px;border:0;background:none}
label{display:block;margin-top:18px}
</style></head><body>
<h2>Ambilight</h2>
<div id="modes"></div>
<label>السطوع</label><input type="range" id="b" min="5" max="255">
<label>اللون</label><input type="color" id="c">
<script>
const names=["إطفاء","لون ثابت","قوس قزح","تنفّس","مذنّب"];
let cur=0;
function draw(){
  const d=document.getElementById("modes");d.innerHTML="";
  names.forEach((n,i)=>{const x=document.createElement("button");x.textContent=n;
    if(i==cur)x.className="on";x.onclick=()=>{cur=i;fetch("/set?m="+i);draw();};d.appendChild(x);});
}
fetch("/state").then(r=>r.json()).then(s=>{cur=s.m;b.value=s.b;c.value="#"+s.c;draw();});
let tm;
b.oninput=()=>{clearTimeout(tm);tm=setTimeout(()=>fetch("/set?b="+b.value),80);};
c.oninput=()=>{clearTimeout(tm);tm=setTimeout(()=>fetch("/set?c="+c.value.slice(1)),80);};
</script></body></html>)rawliteral";

void handleRoot() { server.send_P(200, "text/html; charset=utf-8", PAGE); }

void handleState() {
  char buf[64];
  snprintf(buf, sizeof(buf), "{\"m\":%u,\"b\":%u,\"c\":\"%02x%02x%02x\"}",
           mode, bright, color.r, color.g, color.b);
  server.send(200, "application/json", buf);
}

void handleSet() {
  if (server.hasArg("m")) {
    int m = server.arg("m").toInt();
    if (m >= 0 && m < M_COUNT) mode = m;
  }
  if (server.hasArg("b")) {
    bright = constrain(server.arg("b").toInt(), 5, 255);
    FastLED.setBrightness(bright);
  }
  if (server.hasArg("c")) {
    long v = strtol(server.arg("c").c_str(), NULL, 16);
    color = CRGB((v >> 16) & 255, (v >> 8) & 255, v & 255);
  }
  markDirty();
  server.send(200, "text/plain", "ok");
}

// ---------- Setup / loop ----------
void setup() {
  pinMode(BOOT_PIN, INPUT_PULLUP);
  Serial.setRxBufferSize(2048);
  Serial.begin(BAUD);

  prefs.begin("ambi", false);
  mode   = prefs.getUChar("mode", M_OFF);
  bright = prefs.getUChar("bri", 150);
  color  = CRGB(prefs.getUChar("r", 255), prefs.getUChar("g", 147), prefs.getUChar("b", 41));
  if (mode >= M_COUNT) mode = M_OFF;

  FastLED.addLeds<WS2812B, DATA_PIN, GRB>(leds, NUM_LEDS);
  FastLED.setBrightness(bright);
  FastLED.setMaxPowerInVoltsAndMilliamps(5, MAX_MA);

  // startup test: red, green, blue
  const CRGB cols[3] = {CRGB::Red, CRGB::Green, CRGB::Blue};
  for (int c = 0; c < 3; c++) {
    fill_solid(leds, NUM_LEDS, cols[c]);
    FastLED.show();
    delay(300);
  }
  FastLED.clear();
  FastLED.show();

  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASS);
  server.on("/", handleRoot);
  server.on("/state", handleState);
  server.on("/set", handleSet);
  server.begin();
}

void loop() {
  while (Serial.available()) {
    lastByteMs = millis();
    feed((uint8_t)Serial.read());
  }
  if (ps != P_A && millis() - lastByteMs > BYTE_TO_MS) ps = P_A;
  if (streaming && millis() - lastData > TIMEOUT_MS) streaming = false;

  server.handleClient();
  handleButton();
  saveIfDirty();

  if (!streaming) {
    EVERY_N_MILLISECONDS(20) {
      runMode();
      FastLED.show();
    }
  }
  delay(1);
}
