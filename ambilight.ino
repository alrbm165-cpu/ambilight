// Ambilight ESP32-S3 : Adalight (USB) + WiFi UDP (DRGB / DNRGB) + standalone modes + web control
// FQBN: esp32:esp32:esp32s3:CDCOnBoot=default   (ESP32 core 2.0.17, FastLED)
// Rename this file to ambilight.ino when you put it in the repo.
#include <FastLED.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <WebServer.h>
#include <Preferences.h>

#define NUM_LEDS    60
#define DATA_PIN    5
#define MAX_MA      500
#define BAUD        115200
#define TIMEOUT_MS  5000      // no data for this long -> back to chosen mode
#define BYTE_TO_MS  200       // incomplete serial frame -> reset parser
#define BOOT_PIN    0
#define AP_SSID     "Ambilight"
#define AP_PASS     "12345678"
#define UDP_PORT    21324     // WLED realtime port (Prismatik default)

enum Mode : uint8_t { M_OFF = 0, M_SOLID, M_RAINBOW, M_BREATH, M_COMET, M_COUNT };

CRGB leds[NUM_LEDS];
WebServer server(80);
WiFiUDP udp;
Preferences prefs;

uint8_t mode = M_OFF;
uint8_t bright = 150;
CRGB color = CRGB(255, 147, 41);

bool streaming = false;
uint32_t lastData = 0;

bool dirty = false;
uint32_t dirtyAt = 0;

// ---------- Adalight parser over USB serial ----------
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

// ---------- UDP realtime (WLED protocol: 2 = DRGB, 4 = DNRGB) ----------
uint8_t udpBuf[1472];

void handleUdp() {
  bool got = false;
  int sz;
  while ((sz = udp.parsePacket()) > 0) {
    int len = udp.read(udpBuf, sizeof(udpBuf));
    if (len < 5) continue;
    uint8_t proto = udpBuf[0];
    int offset = 0;
    uint16_t start = 0;
    if (proto == 2) {            // DRGB
      offset = 2;
    } else if (proto == 4) {     // DNRGB
      start = ((uint16_t)udpBuf[2] << 8) | udpBuf[3];
      offset = 4;
    } else continue;
    int n = (len - offset) / 3;
    for (int i = 0; i < n; i++) {
      int idx = start + i;
      if (idx >= NUM_LEDS) break;
      leds[idx] = CRGB(udpBuf[offset + i * 3], udpBuf[offset + i * 3 + 1], udpBuf[offset + i * 3 + 2]);
    }
    if (proto == 2) {            // DRGB updates every LED -> blank the rest
      for (int i = n; i < NUM_LEDS; i++) leds[i] = CRGB::Black;
    }
    got = true;
  }
  if (got) {
    FastLED.show();
    lastData = millis();
    streaming = true;
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
button.full{width:98%}
input[type=range]{width:100%}
input[type=color]{width:100%;height:50px;border:0;background:none}
input[type=text],input[type=password]{width:100%;padding:10px;margin-top:6px;box-sizing:border-box;font-size:16px}
label{display:block;margin-top:18px}
#st{margin-top:10px;padding:10px;background:#222;border-radius:8px}
</style></head><body>
<h2>Ambilight</h2>
<div id="modes"></div>
<label>السطوع</label><input type="range" id="b" min="5" max="255">
<label>اللون</label><input type="color" id="c">
<hr style="margin-top:24px;border-color:#333">
<label>واي فاي البيت (2.4GHz)</label>
<input type="text" id="s" placeholder="اسم الشبكة">
<input type="password" id="p" placeholder="الباسورد">
<button class="full" style="margin-top:10px" onclick="saveW()">حفظ وإعادة تشغيل</button>
<div id="st"></div>
<script>
const names=["إطفاء","لون ثابت","قوس قزح","تنفّس","مذنّب"];
let cur=0;
function draw(){
  const d=document.getElementById("modes");d.innerHTML="";
  names.forEach((n,i)=>{const x=document.createElement("button");x.textContent=n;
    if(i==cur)x.className="on";x.onclick=()=>{cur=i;fetch("/set?m="+i);draw();};d.appendChild(x);});
}
function status(s){
  st.textContent=s.w?("متصل بالواي فاي. العنوان: "+s.ip):"غير متصل بواي فاي البيت";
}
function load(){fetch("/state").then(r=>r.json()).then(s=>{cur=s.m;b.value=s.b;c.value="#"+s.c;if(!s.first){}status(s);draw();});}
load();
setInterval(()=>fetch("/state").then(r=>r.json()).then(status),3000);
let tm;
b.oninput=()=>{clearTimeout(tm);tm=setTimeout(()=>fetch("/set?b="+b.value),80);};
c.oninput=()=>{clearTimeout(tm);tm=setTimeout(()=>fetch("/set?c="+c.value.slice(1)),80);};
function saveW(){
  fetch("/wifi?s="+encodeURIComponent(s.value)+"&p="+encodeURIComponent(p.value)).then(()=>{
    st.textContent="تم الحفظ. البورد بيعيد التشغيل، ارجع اتصل بشبكة Ambilight وافتح الصفحة بعد 15 ثانية لتشوف العنوان.";
  });
}
</script></body></html>)rawliteral";

void handleRoot() { server.send_P(200, "text/html; charset=utf-8", PAGE); }

void handleState() {
  char buf[160];
  bool w = (WiFi.status() == WL_CONNECTED);
  snprintf(buf, sizeof(buf), "{\"m\":%u,\"b\":%u,\"c\":\"%02x%02x%02x\",\"w\":%s,\"ip\":\"%s\"}",
           mode, bright, color.r, color.g, color.b,
           w ? "true" : "false", w ? WiFi.localIP().toString().c_str() : "");
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

void handleWifi() {
  prefs.putString("ssid", server.arg("s"));
  prefs.putString("pass", server.arg("p"));
  server.send(200, "text/plain", "ok");
  delay(500);
  ESP.restart();
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

  // WiFi: own access point always on (fallback) + home network if configured
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(AP_SSID, AP_PASS);
  WiFi.setSleep(false);                 // lower latency for realtime
  WiFi.setAutoReconnect(true);
  String ssid = prefs.getString("ssid", "");
  String pass = prefs.getString("pass", "");
  if (ssid.length() > 0) WiFi.begin(ssid.c_str(), pass.c_str());   // non-blocking

  udp.begin(UDP_PORT);

  server.on("/", handleRoot);
  server.on("/state", handleState);
  server.on("/set", handleSet);
  server.on("/wifi", handleWifi);
  server.begin();
}

void loop() {
  while (Serial.available()) {
    lastByteMs = millis();
    feed((uint8_t)Serial.read());
  }
  if (ps != P_A && millis() - lastByteMs > BYTE_TO_MS) ps = P_A;

  handleUdp();
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
