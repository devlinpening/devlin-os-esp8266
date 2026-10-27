#include <Arduino.h>
#include <Wire.h>
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <EEPROM.h>
#include <Adafruit_GFX.h>
#if defined(DEVLIN_OLED_SH1106)
  #include <Adafruit_SH110X.h>
  #define SSD1306_WHITE SH110X_WHITE
  #define SSD1306_BLACK SH110X_BLACK
  #define SSD1306_INVERSE SH110X_INVERSE
#else
  #include <Adafruit_SSD1306.h>
#endif
#include <time.h>
#include <sys/time.h>
#include <math.h>
#include <cstring>
#include <user_interface.h>

// DEVLIN OS V2.1 - Wemos D1 mini ESP8266 ESP-12F, 4 MB flash
// Pin map carefully re-read from the supplied Nethercap schematic:
// OLED SDA=D2/GPIO4, SCL=D1/GPIO5 (standard Wemos D1 mini ESP8266 I2C pins).
// Confirm=D7/GPIO13, Down=D6/GPIO12, Up=D5/GPIO14,
// Back=D3/GPIO0, Start=D4/GPIO2, battery sense=A0.
// Buttons are active-low to GND and use INPUT_PULLUP.
// D3/GPIO0 and D4/GPIO2 are boot-strap pins: release Back and Start at power-up/reset/flash.
static constexpr int PIN_SDA = 4;        // D2 / GPIO4 -> OLED SDA
static constexpr int PIN_SCL = 5;        // D1 / GPIO5 -> OLED SCL
static constexpr int PIN_CONFIRM = 13;   // D7 / GPIO13
static constexpr int PIN_DOWN = 12;      // D6 / GPIO12
static constexpr int PIN_UP = 14;        // D5 / GPIO14
static constexpr int PIN_BACK = 0;       // D3 / GPIO0
static constexpr int PIN_START = 2;      // D4 / GPIO2
static constexpr int PIN_BATTERY = A0;   // midpoint of external 100k:100k battery divider

static constexpr int OLED_W = 128;
static constexpr int OLED_H = 64;
#if defined(DEVLIN_OLED_SH1106)
Adafruit_SH1106G display(OLED_W, OLED_H, &Wire, -1);
#else
Adafruit_SSD1306 display(OLED_W, OLED_H, &Wire, -1);
#endif
ESP8266WebServer server(80);

// Small Preferences-compatible EEPROM store for ESP8266. Config changes are
// committed immediately; high-score writes are batched to reduce flash wear.
class Preferences {
 public:
  bool begin(const char *, bool) {
    if (!ready) {
      EEPROM.begin(4096);
      for (int i = 0; i < MAX_RECORDS; ++i) {
        readRecord(i, records[i]);
        if ((uint8_t)records[i].key[0] == 0xFF) memset(&records[i], 0, sizeof(Record));
        records[i].key[sizeof(records[i].key)-1] = '\0';
        records[i].value[sizeof(records[i].value)-1] = '\0';
      }
      ready = true;
      lastFlush = millis();
    }
    return true;
  }
  String getString(const char *key, const char *fallback) { return getValue(key, String(fallback)); }
  String getString(const char *key, const String &fallback) { return getValue(key, fallback); }
  int getInt(const char *key, int fallback) { String v = getValue(key, String(fallback)); return v.toInt(); }
  uint32_t getUInt(const char *key, uint32_t fallback) { String v = getValue(key, String((unsigned long)fallback)); return (uint32_t)strtoul(v.c_str(), nullptr, 10); }
  bool getBool(const char *key, bool fallback) { String v = getValue(key, String(fallback ? 1 : 0)); return v.toInt() != 0; }
  size_t putString(const char *key, const String &value) { return setValue(key, value, true); }
  size_t putInt(const char *key, int value) { return setValue(key, String(value), true); }
  size_t putBool(const char *key, bool value) { return setValue(key, String(value ? 1 : 0), true); }
  size_t putUInt(const char *key, uint32_t value) { return setValue(key, String((unsigned long)value), strncmp(key, "hs", 2) != 0); }
  void tick() { if (ready && dirtyAny() && (uint32_t)(millis() - lastFlush) >= 10000UL) flush(); }
 private:
  struct Record { char key[16]; char value[64]; };
  static constexpr int MAX_RECORDS = 50;
  Record records[MAX_RECORDS] = {};
  bool dirty[MAX_RECORDS] = {};
  bool ready = false;
  uint32_t lastFlush = 0;
  void readRecord(int i, Record &r) { uint8_t *p = reinterpret_cast<uint8_t *>(&r); for (size_t j=0; j<sizeof(Record); ++j) p[j] = EEPROM.read(i * sizeof(Record) + j); }
  String getValue(const char *key, const String &fallback) { for (int i=0; i<MAX_RECORDS; ++i) if (records[i].key[0] && strcmp(records[i].key, key)==0) return String(records[i].value); return fallback; }
  int findSlot(const char *key) { int empty = -1; for (int i=0; i<MAX_RECORDS; ++i) { if (records[i].key[0] && strcmp(records[i].key,key)==0) return i; if (!records[i].key[0] && empty<0) empty=i; } return empty; }
  size_t setValue(const char *key, const String &value, bool immediate) { if (!ready) return 0; int i=findSlot(key); if (i<0 || strlen(key)>=sizeof(records[i].key)) return 0; memset(&records[i],0,sizeof(Record)); strncpy(records[i].key,key,sizeof(records[i].key)-1); String v=value; if (v.length() >= sizeof(records[i].value)) v=v.substring(0,sizeof(records[i].value)-1); strncpy(records[i].value,v.c_str(),sizeof(records[i].value)-1); dirty[i]=true; if (immediate) flush(); return v.length(); }
  bool dirtyAny() { for (int i=0;i<MAX_RECORDS;++i) if (dirty[i]) return true; return false; }
  void flush() { if (!ready || !dirtyAny()) return; for (int i=0;i<MAX_RECORDS;++i) if (dirty[i]) { const uint8_t *p=reinterpret_cast<const uint8_t *>(&records[i]); for (size_t j=0;j<sizeof(Record);++j) EEPROM.write(i*sizeof(Record)+j,p[j]); dirty[i]=false; } EEPROM.commit(); lastFlush=millis(); }
};
Preferences prefs;

const char *DEFAULT_AP_PASS = "devlin1234";
const char *DEFAULT_WEB_PASS = "devlin1234";
String apPass, webPass, staSsid, staPass;
String profiles[30];
int activeProfile = 0;
bool timeSyncRequested = false;
uint32_t lastTimeSyncAttempt = 0;
bool oledOk = false;
bool animationsEnabled = true;
bool autoClockEnabled = true;
bool wifiEnabled = true;
int settingsIndex = 0;
uint32_t lastClockCheckpoint = 0;
bool stopwatchRunning = false;
uint32_t stopwatchStartedAt = 0;
uint32_t stopwatchAccumulated = 0;
uint32_t stopwatchLaps = 0;

struct KeyState {
  uint8_t pin;
  bool lastRaw = HIGH;
  bool stable = HIGH;
  uint32_t lastChange = 0;
  uint32_t downAt = 0;
  bool pressedEvent = false;
  bool longEvent = false;
  bool longSent = false;
};
KeyState keys[] = {
  {PIN_CONFIRM}, {PIN_DOWN}, {PIN_UP}, {PIN_BACK}, {PIN_START}
};
enum KeyId { K_CONFIRM, K_DOWN, K_UP, K_BACK, K_START };
uint32_t lastInteraction = 0;
uint32_t lastRender = 0;
uint32_t lastBatteryRead = 0;
int batteryPercent = -1;
float batteryVoltage = 0.0f;

String htmlEscape(String s) {
  s.replace("&", "&amp;"); s.replace("<", "&lt;"); s.replace(">", "&gt;");
  s.replace("\"", "&quot;"); s.replace("'", "&#39;");
  return s;
}

String activeApSsid() {
  if (activeProfile < 0 || activeProfile > 29) activeProfile = 0;
  String s = profiles[activeProfile];
  s.trim();
  if (s.length() == 0) s = "DEVLIN-OS-V2.1";
  if (s.length() > 32) s = s.substring(0, 32);
  return s;
}

void loadSettings() {
  prefs.begin("devlin", false);
  apPass = prefs.getString("appass", DEFAULT_AP_PASS);
  webPass = prefs.getString("webpass", DEFAULT_WEB_PASS);
  staSsid = prefs.getString("stassid", "");
  staPass = prefs.getString("stapass", "");
  activeProfile = prefs.getInt("active", 0);
  animationsEnabled = prefs.getBool("anim", true);
  autoClockEnabled = prefs.getBool("autoclk", true);
  wifiEnabled = prefs.getBool("wifion", true);
  if (apPass.length() < 8) apPass = DEFAULT_AP_PASS;
  if (webPass.length() < 8) webPass = DEFAULT_WEB_PASS;
  for (int i = 0; i < 30; ++i) {
    char key[8]; snprintf(key, sizeof(key), "p%02d", i);
    String fallback;
    if (i == 0) fallback = "DEVLIN-OS-V2.1";
    else fallback = String("DEVLIN-NET-") + String(i + 1);
    profiles[i] = prefs.getString(key, fallback);
    if (profiles[i].length() > 32) profiles[i] = profiles[i].substring(0, 32);
  }
  if (activeProfile < 0 || activeProfile > 29) activeProfile = 0;
}

void saveProfile(int i, const String &name) {
  char key[8]; snprintf(key, sizeof(key), "p%02d", i);
  prefs.putString(key, name);
  profiles[i] = name;
}

void startWifi() {
  if (!wifiEnabled) {
    WiFi.disconnect(true);
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_OFF);
    return;
  }
  WiFi.mode(WIFI_AP_STA);
  WiFi.setSleepMode(WIFI_NONE_SLEEP);
  WiFi.setAutoReconnect(true);
  IPAddress ip(192, 168, 4, 1), gw(192, 168, 4, 1), mask(255, 255, 255, 0);
  WiFi.softAPConfig(ip, gw, mask);
  String ssid = activeApSsid();
  WiFi.softAP(ssid.c_str(), apPass.c_str(), 1, false, 4);
  if (staSsid.length() > 0) WiFi.begin(staSsid.c_str(), staPass.c_str());
}

void setWifiEnabled(bool enabled) {
  wifiEnabled = enabled;
  prefs.putBool("wifion", wifiEnabled);
  if (wifiEnabled) startWifi();
  else {
    WiFi.disconnect(true);
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_OFF);
  }
}

void requestTimeSync() {
  if (!wifiEnabled || WiFi.status() != WL_CONNECTED) return;
  const uint32_t nowMs = millis();
  const bool clockValid = time(nullptr) > 1700000000;
  const bool firstAttempt = !timeSyncRequested;
  const bool retryIfNoTime = !clockValid &&
      (lastTimeSyncAttempt == 0 || (uint32_t)(nowMs - lastTimeSyncAttempt) >= 60000UL);
  const bool periodicRefresh = clockValid &&
      (lastTimeSyncAttempt == 0 || (uint32_t)(nowMs - lastTimeSyncAttempt) >= 21600000UL);
  if (firstAttempt || retryIfNoTime || periodicRefresh) {
    configTzTime("WIB-7", "pool.ntp.org", "time.google.com", "time.cloudflare.com");
    timeSyncRequested = true;
    lastTimeSyncAttempt = nowMs;
  }
}

bool timeIsValid() {
  return time(nullptr) > 1700000000;
}

void setTimeFromEpoch(long epoch) {
  if (epoch < 1700000000L || epoch > 2145916800L) return;
  struct timeval tv;
  tv.tv_sec = epoch;
  tv.tv_usec = 0;
  settimeofday(&tv, nullptr);
  if (prefs.getUInt("clockep", 0) != (uint32_t)epoch) prefs.putUInt("clockep", (uint32_t)epoch);
  lastClockCheckpoint = millis();
}

void checkpointClock() {
  if (!timeIsValid()) return;
  uint32_t nowMs = millis();
  if (lastClockCheckpoint == 0 || (uint32_t)(nowMs - lastClockCheckpoint) >= 21600000UL) {
    time_t now = time(nullptr);
    if (now > 1700000000 && now < 2145916800LL) prefs.putUInt("clockep", (uint32_t)now);
    lastClockCheckpoint = nowMs;
  }
}

void handleSetTime() {
  if (!server.authenticate("admin", webPass.c_str())) return server.requestAuthentication();
  if (server.hasArg("epoch")) setTimeFromEpoch(server.arg("epoch").toInt());
  server.send(200, "text/plain", timeIsValid() ? "Clock synced" : "Invalid time");
}

bool requireAuth() {
  if (server.authenticate("admin", webPass.c_str())) return true;
  server.requestAuthentication();
  return false;
}

String makeWebPage() {
  String h;
  h.reserve(18000);
  h += F("<!doctype html><html lang='id'><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>");
  h += F("<title>DEVLIN OS V2.1 Control</title><style>:root{color-scheme:dark}*{box-sizing:border-box}body{margin:0;background:radial-gradient(ellipse at top,#172b40 0,#080d16 55%);color:#eaf3ff;font:15px system-ui,-apple-system,sans-serif}main{max-width:820px;margin:auto;padding:18px}h1{color:#7cf7d4;margin:0;font-size:clamp(28px,5vw,40px);letter-spacing:.04em}h2{font-size:18px;margin:0 0 10px;color:#f3f8ff}.brand{display:flex;align-items:center;gap:12px;padding:8px 0 16px}.logo{width:42px;height:42px;border:1px solid #7cf7d4;border-radius:13px;display:grid;place-items:center;color:#7cf7d4;font-weight:900;box-shadow:0 0 24px #7cf7d422}.muted,small{color:#9aabc0}.card{background:linear-gradient(145deg,#111d2d,#0c1421);border:1px solid #26384c;border-radius:18px;padding:17px;margin:14px 0;box-shadow:0 10px 30px #0002}label{display:block;margin:12px 0 6px;color:#b8c8dc}input:not([type=checkbox]),select{width:100%;padding:12px;border-radius:10px;border:1px solid #34485f;background:#07101b;color:#fff}input[type=checkbox]{width:18px;height:18px;vertical-align:middle;accent-color:#7cf7d4;margin-right:8px}button{padding:12px 16px;border:0;border-radius:10px;background:#7cf7d4;color:#07101b;font-weight:800;margin-top:14px;cursor:pointer}details{border-top:1px solid #26384c;padding:9px 0}summary{cursor:pointer;color:#7cf7d4;font-weight:650}.grid{display:grid;grid-template-columns:1fr 1fr;gap:12px}.status{border-color:#2e645e;background:linear-gradient(135deg,#112a2b,#101a29)}.pill{display:inline-block;border:1px solid #345b62;border-radius:999px;padding:4px 9px;color:#7cf7d4;font-size:12px}@media(max-width:520px){main{padding:12px}.grid{grid-template-columns:1fr}.card{padding:14px}}</style></head><body><main>");
  h += F("<div class='brand'><div class='logo'>D</div><div><h1>DEVLIN OS</h1><div class='muted'>VERSION 2.1 · CONTROL CENTER</div></div></div>");
  h += F("<div class='card status'><span class='pill'>DEVICE STATUS</span><p id='status'>Memuat status...</p><p class='muted'>Portal lokal: http://192.168.4.1/ · Jam dapat disetel dari browser HP tanpa internet saat terhubung ke portal lokal.</p></div>");
  h += F("<form method='POST' action='/save'><div class='card'><h2>Wi-Fi & Access Point</h2><p class='muted'>Saat Wi-Fi OFF, portal dan scanner tidak tersedia sampai radio dinyalakan lagi dari Settings OLED.</p><label><input style='width:auto' type='checkbox' name='wifiEnabled' value='1'");
  if (wifiEnabled) h += F(" checked");
  h += F("> Wi-Fi ON / OFF</label><p class='muted'>Tersedia 30 preset nama SSID, tetapi hanya preset terpilih yang dipancarkan.</p><label>Preset SSID aktif</label><select name='active'>");
  for (int i = 0; i < 30; ++i) h += "<option value='" + String(i) + "'" + (i == activeProfile ? " selected" : "") + ">" + String(i + 1) + " — " + htmlEscape(profiles[i]) + "</option>";
  h += F("</select><label>Password Wi-Fi perangkat (min. 8 karakter; kosongkan untuk tetap)</label><input type='password' name='apPass' minlength='8' placeholder='Tidak diubah jika kosong'><label>Password admin web (min. 8 karakter; kosongkan untuk tetap)</label><input type='password' name='webPass' minlength='8' placeholder='Tidak diubah jika kosong'><div class='grid'><div><label>Wi-Fi router untuk NTP (opsional)</label><input name='staSsid' maxlength='32' value='");
  h += htmlEscape(staSsid);
  h += F("' placeholder='Nama Wi-Fi rumah'></div><div><label>Password router (kosongkan untuk tetap)</label><input type='password' name='staPass' placeholder='Tidak diubah jika kosong'></div></div><label><input style='width:auto' type='checkbox' name='clearSta' value='1'> Hapus Wi-Fi router tersimpan</label></div>");
  h += F("<div class='card'><h2>30 preset nama SSID</h2><p class='muted'>Perangkat hanya memancarkan satu SSID aktif, bukan 30 SSID sekaligus.</p>");
  for (int i = 0; i < 30; ++i) {
    char key[8]; snprintf(key, sizeof(key), "p%02d", i);
    h += "<details" + String(i == activeProfile ? " open" : "") + "><summary>Preset " + String(i + 1) + (i == activeProfile ? " · AKTIF" : "") + "</summary><label>Nama SSID</label><input maxlength='32' name='" + String(key) + "' value='" + htmlEscape(profiles[i]) + "'></details>";
  }
  h += F("<button type='submit'>Simpan Pengaturan</button></form></div><div class='card'><h2>Jam & perangkat</h2><p>Jam tetap berjalan tanpa internet setelah pernah disetel. Halaman ini menyetel jam dari waktu HP saat dibuka tanpa memerlukan internet. Jika tidak ada waktu tersimpan, gunakan Settings OLED → Set clock offline. Karena perangkat tidak memiliki RTC baterai pada rancangan ini, waktu yang berlalu saat perangkat benar-benar mati tidak dapat dihitung dengan akurat.</p><p class='muted'>Wi-Fi scanner menampilkan jaringan yang terdeteksi. Firmware ini bukan repeater NAT internet.</p><button type='button' onclick=\"location.href='/restart'\">Restart perangkat</button></div>");
  h += F("<script>fetch('/status').then(r=>r.json()).then(s=>{document.getElementById('status').textContent='AP: '+s.ap+' · IP: '+s.ip+' · Router: '+s.sta+' · Wi-Fi: '+s.wifi.toUpperCase()+' · Baterai: '+s.battery+'%';}).catch(()=>{});fetch('/settime?epoch='+Math.floor(Date.now()/1000)).catch(()=>{});</script></main></body></html>");
  return h;
}

void handleRoot() {
  if (!requireAuth()) return;
  server.send(200, "text/html; charset=utf-8", makeWebPage());
}

void handleStatus() {
  if (!requireAuth()) return;
  String json = "{\"ap\":\"" + htmlEscape(activeApSsid()) + "\",\"ip\":\"" + WiFi.softAPIP().toString() + "\",\"sta\":\"" + String(WiFi.status() == WL_CONNECTED ? "connected" : "not connected") + "\",\"wifi\":\"" + String(wifiEnabled ? "on" : "off") + "\",\"battery\":" + String(batteryPercent < 0 ? 0 : batteryPercent) + "}";
  server.send(200, "application/json", json);
}

void handleSave() {
  if (!requireAuth()) return;
  for (int i = 0; i < 30; ++i) {
    char key[8]; snprintf(key, sizeof(key), "p%02d", i);
    if (server.hasArg(key)) {
      String value = server.arg(key); value.trim();
      if (value.length() > 32) value = value.substring(0, 32);
      saveProfile(i, value);
    }
  }
  if (server.hasArg("active")) {
    int a = server.arg("active").toInt();
    if (a >= 0 && a < 30) { activeProfile = a; prefs.putInt("active", activeProfile); }
  }
  if (server.hasArg("apPass") && server.arg("apPass").length() >= 8) {
    apPass = server.arg("apPass"); prefs.putString("appass", apPass);
  }
  if (server.hasArg("webPass") && server.arg("webPass").length() >= 8) {
    webPass = server.arg("webPass"); prefs.putString("webpass", webPass);
  }
  wifiEnabled = server.hasArg("wifiEnabled");
  prefs.putBool("wifion", wifiEnabled);
  if (server.hasArg("clearSta")) {
    staSsid = ""; staPass = ""; prefs.putString("stassid", ""); prefs.putString("stapass", "");
  } else {
    if (server.hasArg("staSsid")) { staSsid = server.arg("staSsid"); staSsid.trim(); prefs.putString("stassid", staSsid); }
    if (server.hasArg("staPass") && server.arg("staPass").length() > 0) { staPass = server.arg("staPass"); prefs.putString("stapass", staPass); }
  }
  server.send(200, "text/html; charset=utf-8", F("<!doctype html><meta name='viewport' content='width=device-width'><body style='background:#080d16;color:#eaf3ff;font:16px system-ui;padding:24px'><h2>Pengaturan tersimpan.</h2><p>Perangkat akan restart dalam beberapa saat. Sambungkan kembali ke Wi-Fi DEVLIN.</p></body>"));
  delay(350);
  ESP.restart();
}

void handleRestart() {
  if (!requireAuth()) return;
  server.send(200, "text/plain", "Restarting DEVLIN OS...");
  delay(250);
  ESP.restart();
}

void setupWeb() {
  server.on("/", HTTP_GET, handleRoot);
  server.on("/save", HTTP_POST, handleSave);
  server.on("/status", HTTP_GET, handleStatus);
  server.on("/settime", HTTP_GET, handleSetTime);
  server.on("/restart", HTTP_GET, handleRestart);
  server.onNotFound([]() {
    if (!requireAuth()) return;
    server.sendHeader("Location", "/", true);
    server.send(302, "text/plain", "Go to /.");
  });
  server.begin();
}

void updateKeys() {
  uint32_t now = millis();
  for (auto &k : keys) {
    bool raw = digitalRead(k.pin);
    if (raw != k.lastRaw) { k.lastRaw = raw; k.lastChange = now; }
    if ((now - k.lastChange) > 25 && raw != k.stable) {
      k.stable = raw;
      if (k.stable == LOW) {
        k.pressedEvent = true;
        k.downAt = now;
        k.longSent = false;
        lastInteraction = now;
      }
    }
    if (k.stable == LOW && !k.longSent && now - k.downAt > 800) {
      k.longEvent = true;
      k.longSent = true;
    }
  }
}

bool takePress(KeyId id) {
  bool v = keys[id].pressedEvent;
  keys[id].pressedEvent = false;
  return v;
}
bool takeLong(KeyId id) {
  bool v = keys[id].longEvent;
  keys[id].longEvent = false;
  return v;
}
bool takeConfirmOrStart() {
  bool c = takePress(K_CONFIRM);
  bool s = takePress(K_START);
  return c || s;
}
bool anyPressPending() {
  for (auto &k : keys) if (k.pressedEvent) return true;
  return false;
}

void updateBattery() {
  if (millis() - lastBatteryRead < 1500) return;
  lastBatteryRead = millis();
  uint32_t mv = (uint32_t)((analogRead(A0) / 1023.0f) * 3200.0f); // D1 mini A0 full-scale approx 3.2 V
  batteryVoltage = (mv / 1000.0f) * 2.0f; // 100k:100k divider
  if (batteryVoltage < 2.5f || batteryVoltage > 5.0f) { batteryPercent = -1; return; }
  float v = batteryVoltage;
  int p;
  if (v <= 3.30f) p = 0;
  else if (v < 3.50f) p = (int)(5 + (v - 3.30f) * 50.0f);
  else if (v < 3.70f) p = (int)(15 + (v - 3.50f) * 100.0f);
  else if (v < 3.85f) p = (int)(35 + (v - 3.70f) * 166.7f);
  else if (v < 4.00f) p = (int)(60 + (v - 3.85f) * 133.3f);
  else if (v < 4.20f) p = (int)(80 + (v - 4.00f) * 100.0f);
  else p = 100;
  batteryPercent = constrain(p, 0, 100);
}

void drawBattery() {
  display.setTextSize(1);
  if (batteryPercent >= 0) {
    display.setCursor(82, 0); display.print(batteryPercent); display.print('%');
    display.drawRect(105, 1, 19, 7, SSD1306_WHITE);
    display.fillRect(124, 3, 2, 3, SSD1306_WHITE);
    int fill = map(batteryPercent, 0, 100, 0, 15);
    if (fill > 0) display.fillRect(107, 3, fill, 3, SSD1306_WHITE);
  } else {
    display.setCursor(88, 0); display.print("BAT?");
  }
}

void drawHeader() {
  display.setTextSize(1); display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0); display.print("DEVLIN OS");
  drawBattery();
  display.drawFastHLine(0, 9, 128, SSD1306_WHITE);
}

void drawLogo(int x, int y, int color) {
  display.drawTriangle(x, y + 8, x + 8, y, x + 16, y + 8, color);
  display.drawLine(x + 4, y + 8, x + 8, y + 3, color);
  display.drawLine(x + 8, y + 3, x + 12, y + 8, color);
}

void drawLogo(int x, int y, int color);

void bootAnimation() {
  if (!oledOk) return;
  if (!animationsEnabled) {
    display.clearDisplay();
    drawLogo(56, 5, SSD1306_WHITE);
    display.setTextColor(SSD1306_WHITE); display.setTextSize(2);
    display.setCursor(15, 22); display.print("DEVLIN");
    display.setTextSize(1); display.setCursor(20, 42); display.print("LAUNCHER V2.1");
    display.setCursor(38, 55); display.print("READY");
    display.display(); delay(350); return;
  }
  for (int frame = 0; frame <= 34; ++frame) {
    display.clearDisplay();
    int bob = (frame % 8 < 4) ? frame % 4 : 7 - (frame % 8);
    int logoX = 55 + (int)(sin(frame * 0.34f) * 10.0f);
    drawLogo(logoX, 3 + bob, SSD1306_WHITE);
    // Tiny orbiting pixels give the monochrome OLED a playful boot animation.
    for (int i = 0; i < 5; ++i) {
      int px = (frame * 5 + i * 25) % 126;
      int py = 8 + ((frame + i * 3) % 7);
      display.drawPixel(px, py, SSD1306_WHITE);
    }
    display.setTextColor(SSD1306_WHITE); display.setTextSize(2);
    display.setCursor(15 + ((frame / 5) % 2), 20); display.print("DEVLIN");
    display.setTextSize(1); display.setCursor(20, 40); display.print("LAUNCHER V2.1");
    display.drawRect(15, 53, 98, 7, SSD1306_WHITE);
    int fill = (frame * 94) / 34;
    display.fillRect(17, 55, fill, 3, SSD1306_WHITE);
    if ((frame / 4) % 2 == 0) { display.setCursor(42, 45); display.print("BOOTING..."); }
    display.display();
    delay(35);
  }
  // A little smiley bounce before entering the main menu.
  for (int frame = 0; frame < 6; ++frame) {
    display.clearDisplay();
    drawLogo(56, 4 + (frame % 3), SSD1306_WHITE);
    display.setTextColor(SSD1306_WHITE); display.setTextSize(1);
    display.setCursor(20, 25); display.print("WELCOME TO DEVLIN");
    display.drawCircle(64, 45, 7, SSD1306_WHITE);
    display.fillCircle(61, 43, 1, SSD1306_WHITE); display.fillCircle(67, 43, 1, SSD1306_WHITE);
    display.drawLine(61, 47, 64, 49, SSD1306_WHITE); display.drawLine(64, 49, 67, 47, SSD1306_WHITE);
    display.display(); delay(45);
  }
}

enum Screen { SCR_MENU, SCR_CLOCK, SCR_WIFI_SCAN, SCR_GAMES, SCR_CALC, SCR_STOPWATCH, SCR_SETTINGS, SCR_NETWORK, SCR_SCORES, SCR_ABOUT, SCR_BLOCK, SCR_BREAKER, SCR_ONELINE, SCR_TTT, SCR_SNAKE, SCR_PONG, SCR_JUMPER, SCR_REACTION, SCR_SIMON, SCR_DODGE, SCR_ROULETTE, SCR_COIN, SCR_MINES, SCR_2048, SCR_RPS, SCR_DICE, SCR_GUESS, SCR_LIGHTS, SCR_PAIRS, SCR_CLOCK_EDIT };
Screen screen = SCR_MENU;
int menuIndex = 0, gameMenuIndex = 0;
uint32_t clockTransitionStart = 0;
bool transitionClock = false;

const char *mainItems[] = {"Wi-Fi Scanner", "Game Center", "Calculator", "Stopwatch", "Network Setup", "High Scores", "Settings", "About DEVLIN"};
const int MAIN_COUNT = sizeof(mainItems) / sizeof(mainItems[0]);
const char *gameItems[] = {"Block Blast", "Block Breaker", "One Line", "Tic-Tac-Toe 2P", "Snake", "Pong", "Pixel Jumper", "Reaction Tap", "Simon Memory", "Dodge Drop", "Roulette 0-36", "Coin Flip", "Minesweeper", "2048 Puzzle", "RPS vs CPU", "Dice Roller", "Number Guess", "Lights Out", "Memory Pairs", "Back"};
const int GAME_COUNT = sizeof(gameItems) / sizeof(gameItems[0]);
int scoresPage = 0;
void startRoulette();
void startCoin();
void startMines();
void start2048();
void startRps();
void startDice();
void startGuess();
void startLightsOut();
void startMemoryPairs();

void drawIcon(int idx, int x, int y, int color) {
  switch (idx % 6) {
    case 0:
      display.drawCircle(x + 6, y + 7, 1, color); display.drawLine(x + 2, y + 5, x + 4, y + 3, color); display.drawLine(x + 4, y + 3, x + 8, y + 3, color); display.drawLine(x + 8, y + 3, x + 10, y + 5, color); display.drawLine(x, y + 3, x + 3, y, color); display.drawLine(x + 3, y, x + 9, y, color); display.drawLine(x + 9, y, x + 12, y + 3, color); break;
    case 1:
      display.drawRoundRect(x, y + 1, 14, 10, 2, color); display.drawLine(x + 3, y + 4, x + 7, y + 4, color); display.drawLine(x + 5, y + 2, x + 5, y + 6, color); display.fillCircle(x + 10, y + 4, 1, color); display.fillCircle(x + 11, y + 7, 1, color); break;
    case 2:
      display.drawRoundRect(x + 1, y, 12, 12, 2, color); display.drawFastHLine(x + 4, y + 3, 6, color); display.drawFastHLine(x + 4, y + 6, 6, color); display.drawFastHLine(x + 4, y + 9, 6, color); break;
    case 3:
      display.drawRect(x + 1, y, 12, 12, color); display.drawLine(x + 4, y + 3, x + 10, y + 3, color); display.drawLine(x + 4, y + 6, x + 10, y + 6, color); display.drawLine(x + 4, y + 9, x + 8, y + 9, color); break;
    case 4:
      display.drawTriangle(x + 1, y + 11, x + 7, y, x + 13, y + 11, color); display.drawPixel(x + 7, y + 7, color); break;
    default:
      display.drawCircle(x + 7, y + 6, 5, color); display.drawCircle(x + 7, y + 6, 2, color); break;
  }
}

void drawMenuList(const char **items, int count, int selected, const char *titleText) {
  display.clearDisplay(); drawHeader();
  display.setTextColor(SSD1306_WHITE); display.setTextSize(1); display.setCursor(0, 11); display.print(titleText);
  display.setCursor(82, 11); display.print("OK=OPEN");
  int start = (selected / 4) * 4;
  for (int row = 0; row < 4; ++row) {
    int idx = start + row; if (idx >= count) break;
    int y = 21 + row * 10;
    bool sel = idx == selected;
    uint32_t anim = animationsEnabled ? millis() / 170 : 0;
    int bob = (animationsEnabled && sel) ? ((anim % 3 == 0) ? -1 : ((anim % 3 == 1) ? 0 : 1)) : 0;
    if (sel) display.fillRoundRect(0, y - 1, 128, 10, 2, SSD1306_WHITE);
    int col = sel ? SSD1306_BLACK : SSD1306_WHITE;
    drawIcon(idx, 2, y - 1 + bob, col);
    display.setTextColor(col); display.setCursor(21, y); display.print(items[idx]);
    if (sel) {
      display.fillTriangle(119, y + 2, 124, y + 4, 119, y + 7, SSD1306_BLACK);
      if (animationsEnabled) {
        int sx = 108 + (anim % 3) * 3;
        display.drawPixel(sx, y + 1, SSD1306_BLACK);
        if ((anim % 2) == 0) display.drawPixel(sx - 2, y + 7, SSD1306_BLACK);
      }
    }
  }
  display.setTextColor(SSD1306_WHITE);
  if (animationsEnabled && (millis() / 250) % 2 == 0) { display.drawPixel(123, 13, SSD1306_WHITE); display.drawPixel(119, 15, SSD1306_WHITE); }
  display.display();
}

void animateClockTransition() {
  if (!animationsEnabled) return;
  // Sliding monochrome curtain with a moving sparkle.
  for (int x = 0; x < 128; x += 16) {
    display.fillRect(x, 10, 16, 54, SSD1306_WHITE);
    display.drawCircle(min(126, x + 8), 36, 3, SSD1306_BLACK);
    display.display(); delay(14);
  }
  for (int x = 0; x < 128; x += 16) {
    display.fillRect(x, 10, 16, 54, SSD1306_BLACK);
    display.drawPixel((x + 13) % 128, 18 + (x % 30), SSD1306_WHITE);
    display.display(); delay(10);
  }
}

void drawClock() {
  display.clearDisplay(); drawHeader();
  display.setTextColor(SSD1306_WHITE);
  if (timeIsValid()) {
    time_t now = time(nullptr); struct tm t; localtime_r(&now, &t);
    char hm[8], sec[8], date[24];
    strftime(hm, sizeof(hm), "%H:%M", &t); strftime(sec, sizeof(sec), "%S", &t); strftime(date, sizeof(date), "%a, %d %b %Y", &t);
    display.setTextSize(2); display.setCursor(14, 18); display.print(hm);
    display.setTextSize(1); display.setCursor(91, 25); display.print(":"); display.print(sec);
    display.setCursor(16, 43); display.print(date);
    display.setCursor(29, 55); display.print("WIB  ·  DEVLIN");
    // Small animated mascot and twinkling star at the edge of the clock screen.
    int bounce = animationsEnabled ? (millis() / 220) % 2 : 0;
    display.drawCircle(7, 44 - bounce, 4, SSD1306_WHITE);
    display.drawPixel(5, 43 - bounce, SSD1306_WHITE); display.drawPixel(9, 43 - bounce, SSD1306_WHITE);
    display.drawLine(5, 46 - bounce, 7, 47 - bounce, SSD1306_WHITE); display.drawLine(7, 47 - bounce, 9, 46 - bounce, SSD1306_WHITE);
    if (animationsEnabled) {
      int starY = 13 + ((millis() / 90) % 48);
      display.drawPixel(125, starY, SSD1306_WHITE);
      if ((millis() / 300) % 2 == 0) display.drawPixel(121, starY + 2, SSD1306_WHITE);
    }
  } else {
    display.setTextSize(1); display.setCursor(15, 18); display.print("WIB CLOCK NOT SET");
    display.setCursor(8, 32); display.print("Settings > Set clock");
    display.setCursor(8, 44); display.print("works without Wi-Fi");
    display.setCursor(19, 56); display.print(wifiEnabled ? "192.168.4.1" : "OFFLINE MODE");
  }
  display.display();
}

void drawWifiScan() {
  display.clearDisplay(); drawHeader(); display.setTextSize(1); display.setTextColor(SSD1306_WHITE); display.setCursor(0, 11); display.print("WI-FI SCANNER");
  if (!wifiEnabled) { display.setCursor(8, 31); display.print("WI-FI IS OFF"); display.setCursor(0, 55); display.print("SETTINGS TO ENABLE"); display.display(); return; }
  int n = WiFi.scanComplete();
  if (n == WIFI_SCAN_RUNNING) { display.setCursor(0, 25); display.print("Scanning"); int dots = (millis() / 240) % 4; for (int i = 0; i < dots; ++i) display.print("."); }
  else if (n < 0) { display.setCursor(0, 25); display.print("Press OK to scan"); }
  else if (n == 0) { display.setCursor(0, 25); display.print("No networks found"); }
  else {
    int shown = min(n, 3);
    for (int i = 0; i < shown; ++i) {
      int y = 22 + i * 10;
      String s = WiFi.SSID(i); if (s.length() > 14) s = s.substring(0, 14);
      display.setCursor(0, y); display.print(s);
      display.setCursor(96, y); display.print(WiFi.RSSI(i));
    }
  }
  display.setCursor(0, 55); display.print("OK RESCAN   BACK EXIT"); display.display();
}

void doScan() {
  if (!wifiEnabled) return;
  WiFi.scanDelete();
  WiFi.scanNetworks(true, true);
}

void drawNetwork() {
  display.clearDisplay(); drawHeader(); display.setTextColor(SSD1306_WHITE); display.setTextSize(1); display.setCursor(0, 12); display.print("NETWORK SETUP");
  display.setCursor(0, 25); display.print("AP: "); String s = activeApSsid(); if (s.length() > 17) s = s.substring(0, 17); display.print(s);
  display.setCursor(0, 36); display.print("IP: "); display.print(wifiEnabled ? "192.168.4.1" : "WI-FI OFF");
  display.setCursor(0, 46); display.print("Router: "); display.print(!wifiEnabled ? "RADIO OFF" : (WiFi.status() == WL_CONNECTED ? "CONNECTED" : "OFFLINE"));
  int pulse = (millis() / 220) % 4;
  for (int i = 0; i < 4; ++i) display.fillRect(102 + i * 5, 44 - (i <= pulse ? (i + 1) * 3 : 0), 3, (i <= pulse ? (i + 1) * 3 : 2), SSD1306_WHITE);
  display.setCursor(0, 55); display.print("OK/BACK RETURN"); display.display();
}

void drawScores() {
  display.clearDisplay(); drawHeader(); display.setTextSize(1); display.setTextColor(SSD1306_WHITE); display.setCursor(0, 12); display.print("HIGH SCORES "); display.print(scoresPage + 1); display.print("/4");
  if (scoresPage == 0) {
    display.setCursor(0, 25); display.print("Block Blast: "); display.print(prefs.getUInt("hsblock", 0));
    display.setCursor(0, 36); display.print("Block Breaker: "); display.print(prefs.getUInt("hsbreak", 0));
    display.setCursor(0, 47); display.print("One Line: "); uint32_t best = prefs.getUInt("hsoneline", 0); if (best == 0) display.print("--"); else { display.print(best / 1000); display.print("s"); }
  } else if (scoresPage == 1) {
    display.setCursor(0, 25); display.print("Snake: "); display.print(prefs.getUInt("hssnake", 0));
    display.setCursor(0, 36); display.print("Pong hits: "); display.print(prefs.getUInt("hspong", 0));
    display.setCursor(0, 47); display.print("Pixel Jumper: "); display.print(prefs.getUInt("hsjumper", 0));
  } else if (scoresPage == 2) {
    display.setCursor(0, 25); display.print("Simon level: "); display.print(prefs.getUInt("hssimon", 0));
    display.setCursor(0, 36); display.print("Dodge score: "); display.print(prefs.getUInt("hsdodge", 0));
    display.setCursor(0, 47); display.print("Reaction: "); uint32_t best = prefs.getUInt("hsreact", 0); if (best) { display.print(best); display.print("ms"); } else display.print("--");
  } else {
    display.setCursor(0, 25); display.print("2048 best: "); display.print(prefs.getUInt("hs2048", 0));
    display.setCursor(0, 36); display.print("Guess best: "); uint32_t g = prefs.getUInt("hsguess", 0); if (g) display.print(g); else display.print("--");
    display.setCursor(0, 47); display.print("19 GAMES TOTAL");
  }
  if ((millis() / 260) % 2 == 0) display.drawPixel(122, 13, SSD1306_WHITE);
  display.setCursor(0, 55); display.print("UP/DN PAGE  BACK EXIT"); display.display();
}

void drawAbout() {
  display.clearDisplay(); drawHeader(); display.setTextSize(1); display.setTextColor(SSD1306_WHITE); display.setCursor(0, 13); display.print("DEVLIN OS V2.1");
  display.setCursor(0, 26); display.print("Board: ESP8266 4MB");
  display.setCursor(0, 36); display.print("OLED: SSD1306 128x64");
  display.setCursor(0, 46); display.print("19 GAMES + TOOLS");
  int bx = 106 + (int)((millis() / 140) % 5);
  display.drawCircle(bx, 31, 5, SSD1306_WHITE); display.drawPixel(bx - 2, 30, SSD1306_WHITE); display.drawPixel(bx + 2, 30, SSD1306_WHITE);
  display.setCursor(0, 55); display.print("BACK TO MENU"); display.display();
}

// Mix SDK random output with local timing/ADC noise for casual game outcomes.
// This is not a cryptographic RNG and must not be used for security or real-money play.
uint32_t gameRandom32() {
  uint32_t r = os_random();
  r ^= micros();
  r ^= ESP.getCycleCount();
  r ^= ((uint32_t)analogRead(A0) << 20);
  r ^= (uint32_t)ESP.getChipId() * 2654435761UL;
  return r;
}
uint32_t gameRandom(uint32_t upperExclusive) {
  if (!upperExclusive) return 0;
  // Rejection sampling avoids modulo bias (relevant for the 0-36 roulette).
  const uint32_t threshold = (uint32_t)(0UL - upperExclusive) % upperExclusive;
  uint32_t r;
  do { r = gameRandom32(); } while (r < threshold);
  return r % upperExclusive;
}

// ---------------- BLOCK PUZZLE (original line-clearing block game) ----------------
uint8_t blockBoard[8][8];
int blockX = 2, blockY = 2, blockPiece = 0;
uint32_t blockScore = 0;
int blockLastCleared = 0;
bool blockOver = false;
const int pieceCount = 7;
const int8_t pieceCells[pieceCount][4][2] = {
  {{0,0},{1,0},{2,0},{3,0}}, // line
  {{0,0},{1,0},{0,1},{1,1}}, // square
  {{0,0},{0,1},{0,2},{1,2}}, // L
  {{0,0},{1,0},{2,0},{1,1}}, // T
  {{0,0},{1,0},{1,1},{2,1}}, // Z
  {{1,0},{1,1},{1,2},{0,2}}, // reverse L
  {{1,0},{2,0},{0,1},{1,1}}  // S
};

bool resetBlockCursor();
void startBlockGame() {
  memset(blockBoard, 0, sizeof(blockBoard)); blockPiece = (int)gameRandom(pieceCount); blockScore = 0; blockLastCleared = 0; blockOver = false;
  resetBlockCursor();
}

int blockPieceWidth() {
  int w = 1;
  for (int i = 0; i < 4; ++i) w = max(w, (int)pieceCells[blockPiece][i][0] + 1);
  return w;
}
int blockPieceHeight() {
  int h = 1;
  for (int i = 0; i < 4; ++i) h = max(h, (int)pieceCells[blockPiece][i][1] + 1);
  return h;
}
bool canPlacePiece(int x, int y) {
  for (int i = 0; i < 4; ++i) {
    int xx = x + pieceCells[blockPiece][i][0], yy = y + pieceCells[blockPiece][i][1];
    if (xx < 0 || xx >= 8 || yy < 0 || yy >= 8 || blockBoard[yy][xx]) return false;
  }
  return true;
}
bool resetBlockCursor() {
  int bestX = 0, bestY = 0, bestDistance = 999;
  bool found = false;
  int maxX = 8 - blockPieceWidth(), maxY = 8 - blockPieceHeight();
  for (int y = 0; y <= maxY; ++y) for (int x = 0; x <= maxX; ++x) {
    if (!canPlacePiece(x, y)) continue;
    int distance = abs(x - maxX / 2) + abs(y - maxY / 2);
    if (distance < bestDistance) { bestDistance = distance; bestX = x; bestY = y; found = true; }
  }
  if (found) { blockX = bestX; blockY = bestY; blockOver = false; }
  else blockOver = true;
  return found;
}

void updateBlockHighScore() {
  uint32_t hs = prefs.getUInt("hsblock", 0);
  if (blockScore > hs) prefs.putUInt("hsblock", blockScore);
}

void placeBlockPiece() {
  if (blockOver) { startBlockGame(); return; }
  if (!canPlacePiece(blockX, blockY)) return;
  for (int i = 0; i < 4; ++i) blockBoard[blockY + pieceCells[blockPiece][i][1]][blockX + pieceCells[blockPiece][i][0]] = 1;
  blockScore += 4;
  bool fullRows[8] = {false,false,false,false,false,false,false,false};
  bool fullCols[8] = {false,false,false,false,false,false,false,false};
  int cleared = 0;
  for (int y = 0; y < 8; ++y) {
    fullRows[y] = true;
    for (int x = 0; x < 8; ++x) if (!blockBoard[y][x]) { fullRows[y] = false; break; }
    if (fullRows[y]) cleared++;
  }
  for (int x = 0; x < 8; ++x) {
    fullCols[x] = true;
    for (int y = 0; y < 8; ++y) if (!blockBoard[y][x]) { fullCols[x] = false; break; }
    if (fullCols[x]) cleared++;
  }
  // Clear every completed row/column at once; intersections are removed only once.
  for (int y = 0; y < 8; ++y) for (int x = 0; x < 8; ++x) if (fullRows[y] || fullCols[x]) blockBoard[y][x] = 0;
  blockLastCleared = cleared;
  blockScore += (uint32_t)cleared * 100UL + (cleared > 1 ? (uint32_t)(cleared - 1) * 50UL : 0UL);
  blockPiece = (int)gameRandom(pieceCount);
  resetBlockCursor();
  updateBlockHighScore();
}

void drawBlockGame() {
  display.clearDisplay(); drawHeader(); display.setTextSize(1); display.setTextColor(SSD1306_WHITE); display.setCursor(0, 11); display.print("BLOCK BLAST"); display.setCursor(80, 11); display.print("DIR/OK");
  const int cell = 5, ox = 2, oy = 22;
  for (int y = 0; y < 8; ++y) for (int x = 0; x < 8; ++x) {
    int px = ox + x * cell, py = oy + y * cell;
    if (blockBoard[y][x]) display.fillRect(px, py, 5, 5, SSD1306_WHITE); else display.drawRect(px, py, 5, 5, SSD1306_WHITE);
  }
  if (!blockOver) {
    for (int i = 0; i < 4; ++i) {
      int xx = blockX + pieceCells[blockPiece][i][0], yy = blockY + pieceCells[blockPiece][i][1];
      if (xx < 0 || xx >= 8 || yy < 0 || yy >= 8) continue;
      int px = ox + xx * cell, py = oy + yy * cell;
      if (blockBoard[yy][xx]) { display.drawLine(px + 1, py + 1, px + 3, py + 3, SSD1306_BLACK); display.drawLine(px + 3, py + 1, px + 1, py + 3, SSD1306_BLACK); }
      else { display.drawRect(px, py, 5, 5, SSD1306_WHITE); display.drawPixel(px + 2, py + 2, SSD1306_WHITE); }
    }
  }
  display.setCursor(54, 24); display.print("SCORE"); display.setCursor(95, 24); display.print("CLR");
  display.setCursor(54, 34); display.print(blockScore); display.setCursor(95, 34); display.print(blockLastCleared);
  if (blockOver) {
    display.setCursor(54, 54); display.print("OK RETRY");
  } else if (canPlacePiece(blockX, blockY)) {
    display.setCursor(54, 44); display.print("B/S=L/R"); display.setCursor(54, 54); display.print("OK=DROP");
  } else {
    display.setCursor(54, 44); display.print("NO FIT"); display.setCursor(54, 54); display.print("MOVE BLOCK");
  }
  if (blockOver) { display.fillRect(4, 28, 120, 17, SSD1306_BLACK); display.drawRect(4, 28, 120, 17, SSD1306_WHITE); display.setCursor(20, 33); display.print("NO MOVE: OK RETRY"); }
  display.display();
}

// ---------------- BLOCK BREAKER ----------------
bool bricks[5][10];
float ballX = 64, ballY = 48, ballVX = 1.3f, ballVY = -1.5f;
int paddleX = 53, lives = 3;
bool ballLaunched = false, breakerOver = false;
uint32_t breakerScore = 0, lastBallTick = 0;
void startBreaker() {
  for (int r = 0; r < 5; ++r) for (int c = 0; c < 10; ++c) bricks[r][c] = true;
  ballX = 64; ballY = 49; ballVX = 1.2f; ballVY = -1.5f; paddleX = 53; lives = 3; ballLaunched = false; breakerOver = false; breakerScore = 0;
}
void saveBreakerScore() { uint32_t hs = prefs.getUInt("hsbreak", 0); if (breakerScore > hs) prefs.putUInt("hsbreak", breakerScore); }
void tickBreaker() {
  if (screen != SCR_BREAKER || breakerOver || !ballLaunched || millis() - lastBallTick < 24) return;
  lastBallTick = millis();
  float oldX = ballX, oldY = ballY;
  ballX += ballVX; ballY += ballVY;
  if (ballX < 2) { ballX = 2; ballVX = fabs(ballVX); }
  if (ballX > 125) { ballX = 125; ballVX = -fabs(ballVX); }
  if (ballY < 19) { ballY = 19; ballVY = fabs(ballVY); }
  // Paddle collision: steer the rebound according to the impact point.
  if (ballVY > 0 && oldY <= 57 && ballY >= 57 && ballX + 1 >= paddleX && ballX - 1 <= paddleX + 22) {
    ballY = 56; ballVY = -fabs(ballVY);
    float hit = (ballX - (paddleX + 11.0f)) / 11.0f;
    ballVX = hit * 2.1f;
    if (fabs(ballVX) < 0.45f) ballVX = (ballVX < 0 ? -0.45f : 0.45f);
  }
  // Test actual brick rectangles, rather than a misaligned grid index. This
  // prevents the ball passing through bricks or deleting the wrong brick.
  bool hitBrick = false;
  for (int r = 0; r < 5 && !hitBrick; ++r) for (int c = 0; c < 10 && !hitBrick; ++c) {
    if (!bricks[r][c]) continue;
    int bx = 3 + c * 12, by = 21 + r * 5;
    if (ballX + 1 >= bx && ballX - 1 <= bx + 9 && ballY + 1 >= by && ballY - 1 <= by + 3) {
      bricks[r][c] = false; breakerScore += 10; hitBrick = true;
      if (oldY < by || oldY > by + 3) ballVY = -ballVY; else ballVX = -ballVX;
    }
  }
  if (ballY > 64) {
    lives--;
    if (lives <= 0) { breakerOver = true; saveBreakerScore(); }
    else { ballX = paddleX + 11; ballY = 54; ballLaunched = false; }
  }
  bool left = false;
  for (int r = 0; r < 5; ++r) for (int c = 0; c < 10; ++c) if (bricks[r][c]) left = true;
  if (!left) { breakerOver = true; saveBreakerScore(); }
}
void drawBreaker() {
  display.clearDisplay(); drawHeader(); display.setTextSize(1); display.setTextColor(SSD1306_WHITE); display.setCursor(0, 11); display.print("BLOCK BREAKER");
  for (int r = 0; r < 5; ++r) for (int c = 0; c < 10; ++c) if (bricks[r][c]) display.fillRect(3 + c * 12, 21 + r * 5, 9, 3, SSD1306_WHITE);
  display.fillRect(paddleX, 58, 22, 3, SSD1306_WHITE); display.fillCircle((int)ballX, (int)ballY, 1, SSD1306_WHITE);
  display.setCursor(0, 15); display.print("S:"); display.print(breakerScore); display.setCursor(82, 15); display.print("L:"); display.print(lives);
  if (!ballLaunched && !breakerOver) { display.fillRect(25, 39, 78, 10, SSD1306_BLACK); display.setCursor(27, 40); display.print("OK TO LAUNCH"); }
  if (breakerOver) { display.fillRect(10, 36, 108, 15, SSD1306_BLACK); display.drawRect(10, 36, 108, 15, SSD1306_WHITE); display.setCursor(17, 40); display.print("GAME OVER - OK AGAIN"); }
  display.display();
}

// ---------------- ONE LINE: PROGRESSIVE MULTI-LEVEL PATH PUZZLE ----------------
int onePos = 0, onePath[16], oneCount = 0, oneLevel = 1, oneW = 3, oneH = 3, oneTarget = 8;
bool oneVisited[16], oneWon = false, oneFailed = false;
uint32_t oneStart = 0, oneElapsed = 0;
void startOneLineLevel() {
  oneW = oneLevel == 1 ? 3 : 4;
  oneH = oneLevel <= 2 ? 3 : 4;
  memset(oneVisited, 0, sizeof(oneVisited));
  int corners[4] = {0, oneW - 1, (oneH - 1) * oneW, oneW * oneH - 1};
  onePos = corners[gameRandom(4)];
  const int total = oneW * oneH;
  const int startColor = (onePos % oneW + onePos / oneW) & 1;
  const int targetColor = (total & 1) ? startColor : (startColor ^ 1);
  int candidates[4], candidateCount = 0;
  for (int i = 0; i < 4; ++i) {
    if (corners[i] == onePos) continue;
    int color = (corners[i] % oneW + corners[i] / oneW) & 1;
    if (color == targetColor) candidates[candidateCount++] = corners[i];
  }
  oneTarget = candidateCount ? candidates[gameRandom(candidateCount)] : corners[(gameRandom(3) + 1) % 4];
  oneCount = 1; onePath[0] = onePos; oneVisited[onePos] = true;
  oneWon = false; oneFailed = false; oneElapsed = 0; oneStart = millis();
}
void startOneLine() { oneLevel = 1; startOneLineLevel(); }
void moveOneLine(int dx, int dy) {
  if (oneWon || oneFailed) return;
  int x = onePos % oneW, y = onePos / oneW, nx = x + dx, ny = y + dy;
  if (nx < 0 || nx >= oneW || ny < 0 || ny >= oneH) return;
  int next = ny * oneW + nx;
  if (oneVisited[next] || oneCount >= oneW * oneH) return;
  onePos = next; oneVisited[next] = true; onePath[oneCount++] = next;
  if (oneCount == oneW * oneH) {
    if (onePos == oneTarget) {
      oneWon = true; oneElapsed = millis() - oneStart;
      uint32_t best = prefs.getUInt("hsoneline", 0);
      if (best == 0 || oneElapsed < best) prefs.putUInt("hsoneline", oneElapsed);
    } else oneFailed = true;
  }
}
void drawOneLine() {
  display.clearDisplay(); drawHeader(); display.setTextSize(1); display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 11); display.print("ONE LINE LV:"); display.print(oneLevel);
  display.setCursor(0, 23); display.print("VISIT "); display.print(oneCount); display.print("/"); display.print(oneW * oneH);
  display.setCursor(0, 34); display.print("TIME "); display.print((oneWon ? oneElapsed : millis() - oneStart) / 1000); display.print("s");
  display.setCursor(0, 42); display.print("TARGET = RING");
  display.setCursor(0, 50); display.print("OK=UNDO");
  const int ox = 84, oy = 23, gap = 9;
  for (int i = 1; i < oneCount; ++i) {
    int a = onePath[i - 1], b = onePath[i];
    display.drawLine(ox + (a % oneW) * gap, oy + (a / oneW) * gap, ox + (b % oneW) * gap, oy + (b / oneW) * gap, SSD1306_WHITE);
  }
  for (int i = 0; i < oneW * oneH; ++i) {
    int x = ox + (i % oneW) * gap, y = oy + (i / oneW) * gap;
    if (oneVisited[i]) display.fillCircle(x, y, 2, SSD1306_WHITE); else display.drawCircle(x, y, 2, SSD1306_WHITE);
  }
  int tx = ox + (oneTarget % oneW) * gap, ty = oy + (oneTarget / oneW) * gap;
  display.drawCircle(tx, ty, 4, SSD1306_WHITE); display.drawCircle(tx, ty, 5, SSD1306_WHITE);
  display.drawCircle(ox + (onePos % oneW) * gap, oy + (onePos / oneW) * gap, 4, SSD1306_INVERSE);
  display.setCursor(0, 56);
  if (oneWon) display.print("OK NEXT LEVEL");
  else if (oneFailed) display.print("WRONG END: OK RETRY");
  else display.print("HOLD BACK EXIT");
  display.display();
}

// ---------------- TIC TAC TOE ----------------
int ttt[9], tttCursor = 0, tttPlayer = 1, tttWinner = 0;
void startTtt() { memset(ttt, 0, sizeof(ttt)); tttCursor = 0; tttPlayer = 1; tttWinner = 0; }
int checkTtt() {
  const int lines[8][3] = {{0,1,2},{3,4,5},{6,7,8},{0,3,6},{1,4,7},{2,5,8},{0,4,8},{2,4,6}};
  for (auto &ln : lines) if (ttt[ln[0]] && ttt[ln[0]] == ttt[ln[1]] && ttt[ln[1]] == ttt[ln[2]]) return ttt[ln[0]];
  for (int i = 0; i < 9; ++i) if (!ttt[i]) return 0;
  return 3;
}
void drawTtt() {
  display.clearDisplay(); drawHeader(); display.setTextSize(1); display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 11); display.print("TIC-TAC-TOE");
  const int ox = 2, oy = 21, cell = 11;
  for (int i = 1; i < 3; ++i) {
    display.drawFastVLine(ox + i * cell, oy, cell * 3, SSD1306_WHITE);
    display.drawFastHLine(ox, oy + i * cell, cell * 3, SSD1306_WHITE);
  }
  for (int i = 0; i < 9; ++i) {
    int x = ox + (i % 3) * cell, y = oy + (i / 3) * cell;
    if (ttt[i] == 1) {
      display.drawLine(x + 3, y + 3, x + 9, y + 9, SSD1306_WHITE);
      display.drawLine(x + 9, y + 3, x + 3, y + 9, SSD1306_WHITE);
    } else if (ttt[i] == 2) display.drawCircle(x + 6, y + 6, 3, SSD1306_WHITE);
  }
  int cx = ox + (tttCursor % 3) * cell, cy = oy + (tttCursor / 3) * cell;
  if ((millis() / 220) % 2 == 0) display.drawRect(cx + 1, cy + 1, cell - 2, cell - 2, SSD1306_INVERSE);
  else display.drawCircle(cx + 6, cy + 6, 5, SSD1306_INVERSE);
  display.setCursor(43, 24); display.print("2 PLAYERS");
  display.setCursor(43, 35);
  if (tttWinner == 1) display.print("X WINS!");
  else if (tttWinner == 2) display.print("O WINS!");
  else if (tttWinner == 3) display.print("DRAW!");
  else { display.print("TURN: "); display.print(tttPlayer == 1 ? 'X' : 'O'); }
  display.setCursor(43, 46); display.print("DIR MOVE");
  display.setCursor(0, 56); display.print(tttWinner ? "OK/START REPLAY" : "OK PLACE  HOLD B EXIT");
  display.display();
}

// ---------------- SNAKE ----------------
struct SnakePoint { int8_t x; int8_t y; };
SnakePoint snakeBody[112];
int snakeLen = 4, snakeDx = 1, snakeDy = 0, snakeFoodX = 11, snakeFoodY = 3;
bool snakeOver = false;
uint32_t snakeScore = 0, snakeLastTick = 0;
void spawnSnakeFood() {
  if (snakeLen >= 112) { snakeOver = true; return; }
  for (int attempt = 0; attempt < 400; ++attempt) {
    snakeFoodX = gameRandom(16); snakeFoodY = gameRandom(7);
    bool occupied = false;
    for (int i = 0; i < snakeLen; ++i) {
      if (snakeBody[i].x == snakeFoodX && snakeBody[i].y == snakeFoodY) { occupied = true; break; }
    }
    if (!occupied) return;
  }
  // Defensive fallback: never spin forever if the board has no free cell.
  snakeOver = true;
}
void startSnake() {
  snakeLen = 4; snakeDx = 1; snakeDy = 0; snakeScore = 0; snakeOver = false; snakeLastTick = millis();
  for (int i = 0; i < snakeLen; ++i) { snakeBody[i].x = 7 - i; snakeBody[i].y = 3; }
  spawnSnakeFood();
}
void tickSnake() {
  if (screen != SCR_SNAKE || snakeOver || millis() - snakeLastTick < 180) return;
  snakeLastTick = millis();
  int nx = snakeBody[0].x + snakeDx, ny = snakeBody[0].y + snakeDy;
  bool eat = (nx == snakeFoodX && ny == snakeFoodY);
  if (nx < 0 || nx >= 16 || ny < 0 || ny >= 7) snakeOver = true;
  int limit = snakeLen - (eat ? 0 : 1);
  for (int i = 0; i < limit; ++i) if (snakeBody[i].x == nx && snakeBody[i].y == ny) snakeOver = true;
  if (snakeOver) {
    uint32_t hs = prefs.getUInt("hssnake", 0); if (snakeScore > hs) prefs.putUInt("hssnake", snakeScore);
    return;
  }
  if (eat && snakeLen < 112) snakeLen++;
  for (int i = snakeLen - 1; i > 0; --i) snakeBody[i] = snakeBody[i - 1];
  snakeBody[0].x = nx; snakeBody[0].y = ny;
  if (eat) {
    snakeScore += 10;
    if (snakeLen >= 112) {
      snakeOver = true;
      uint32_t hs = prefs.getUInt("hssnake", 0); if (snakeScore > hs) prefs.putUInt("hssnake", snakeScore);
    } else {
      spawnSnakeFood();
      if (snakeOver) { uint32_t hs = prefs.getUInt("hssnake", 0); if (snakeScore > hs) prefs.putUInt("hssnake", snakeScore); }
    }
  }
}
void drawSnake() {
  display.clearDisplay(); drawHeader(); display.setTextSize(1); display.setTextColor(SSD1306_WHITE); display.setCursor(0, 11); display.print("SNAKE "); display.print(snakeScore); display.setCursor(82, 11); display.print("B< S>");
  display.drawRect(15, 17, 98, 44, SSD1306_WHITE);
  for (int i = 0; i < snakeLen; ++i) {
    int x = 17 + snakeBody[i].x * 6, y = 19 + snakeBody[i].y * 6;
    if (i == 0) { display.fillRoundRect(x, y, 5, 5, 1, SSD1306_WHITE); display.drawPixel(x + 1, y + 1, SSD1306_BLACK); }
    else display.fillRect(x, y, 4, 4, SSD1306_WHITE);
  }
  int fx = 17 + snakeFoodX * 6, fy = 19 + snakeFoodY * 6;
  if ((millis() / 180) % 2 == 0) display.fillCircle(fx + 2, fy + 2, 2, SSD1306_WHITE); else display.drawCircle(fx + 2, fy + 2, 2, SSD1306_WHITE);
  if (snakeOver) { display.fillRect(20, 31, 88, 17, SSD1306_BLACK); display.drawRect(20, 31, 88, 17, SSD1306_WHITE); display.setCursor(26, 36); display.print("OVER: OK REPLAY"); }
  display.display();
}
// ---------------- PONG ----------------
int pongPaddleY = 30;
float pongX = 28, pongY = 32, pongVX = 1.5f, pongVY = 1.1f;
bool pongLaunched = false, pongOver = false;
uint32_t pongScore = 0, pongLastTick = 0;
void startPong() { pongPaddleY = 28; pongX = 28; pongY = 32; pongVX = 1.6f; pongVY = 1.1f; pongLaunched = false; pongOver = false; pongScore = 0; pongLastTick = millis(); }
void tickPong() {
  if (screen != SCR_PONG || pongOver || !pongLaunched || millis() - pongLastTick < 22) return;
  pongLastTick = millis(); pongX += pongVX; pongY += pongVY;
  if (pongY < 18 || pongY > 53) pongVY = -pongVY;
  if (pongX < 2) pongVX = fabs(pongVX);
  if (pongX >= 116) {
    if (pongY >= pongPaddleY - 2 && pongY <= pongPaddleY + 12) { pongVX = -fabs(pongVX); pongScore++; pongVY += ((pongY - (pongPaddleY + 7)) * 0.035f); if (pongVY > 2.7f) pongVY = 2.7f; if (pongVY < -2.7f) pongVY = -2.7f; }
    else { pongOver = true; uint32_t hs = prefs.getUInt("hspong", 0); if (pongScore > hs) prefs.putUInt("hspong", pongScore); }
  }
}
void drawPong() {
  display.clearDisplay(); drawHeader(); display.setTextSize(1); display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 11); display.print("PONG HITS:"); display.print(pongScore);
  for (int y = 20; y < 54; y += 6) display.drawFastVLine(63, y, 3, SSD1306_WHITE);
  display.fillRoundRect(119, pongPaddleY, 4, 12, 1, SSD1306_WHITE);
  display.fillCircle((int)pongX, (int)pongY, 2, SSD1306_WHITE);
  if (!pongLaunched && !pongOver) { display.fillRect(20, 30, 88, 14, SSD1306_BLACK); display.setCursor(24, 34); display.print("OK/START TO SERVE"); }
  if (pongOver) { display.fillRect(17, 29, 95, 18, SSD1306_BLACK); display.drawRect(17, 29, 95, 18, SSD1306_WHITE); display.setCursor(23, 34); display.print("OUT! OK PLAY AGAIN"); }
  display.setCursor(0, 56); display.print("UP/DN MOVE OK SERVE");
  display.display();
}

// ---------------- PIXEL JUMPER (original mini platform runner) ----------------
float jumperY = 48, jumperVy = 0;
int jumperObstacleX = 128;
bool jumperGrounded = true, jumperOver = false;
uint32_t jumperScore = 0, jumperLastTick = 0;
void startJumper() { jumperY = 48; jumperVy = 0; jumperObstacleX = 128 + gameRandom(24); jumperGrounded = true; jumperOver = false; jumperScore = 0; jumperLastTick = millis(); }
void tickJumper() {
  if (screen != SCR_JUMPER || jumperOver || millis() - jumperLastTick < 32) return;
  jumperLastTick = millis();
  if (!jumperGrounded) { jumperY += jumperVy; jumperVy += 0.48f; if (jumperY >= 48) { jumperY = 48; jumperVy = 0; jumperGrounded = true; } }
  jumperObstacleX -= 2;
  if (jumperObstacleX < -8) { jumperObstacleX = 128 + (12 + (int)gameRandom(28)); jumperScore++; uint32_t hs = prefs.getUInt("hsjumper", 0); if (jumperScore > hs) prefs.putUInt("hsjumper", jumperScore); }
  if (jumperObstacleX < 27 && jumperObstacleX + 7 > 17 && jumperY >= 43) { jumperOver = true; uint32_t hs = prefs.getUInt("hsjumper", 0); if (jumperScore > hs) prefs.putUInt("hsjumper", jumperScore); }
}
void drawJumper() {
  display.clearDisplay(); drawHeader(); display.setTextSize(1); display.setTextColor(SSD1306_WHITE); display.setCursor(0, 11); display.print("JUMPER"); display.setCursor(42, 11); display.print("S:"); display.print(jumperScore); display.setCursor(94, 11); display.print("JUMP");
  int cloudX = 100 - ((millis() / 90) % 120); display.drawCircle(cloudX, 23, 2, SSD1306_WHITE); display.drawCircle(cloudX + 4, 22, 3, SSD1306_WHITE); display.drawCircle(cloudX + 8, 23, 2, SSD1306_WHITE);
  display.drawFastHLine(0, 58, 128, SSD1306_WHITE);
  int py = (int)jumperY;
  display.fillRoundRect(18, py, 8, 9, 2, SSD1306_WHITE); display.drawPixel(23, py + 2, SSD1306_BLACK); display.drawPixel(20, py + 6, SSD1306_BLACK);
  display.drawLine(20, py + 9, 18, py + 11, SSD1306_WHITE); display.drawLine(24, py + 9, 26, py + 11, SSD1306_WHITE);
  display.fillTriangle(jumperObstacleX, 58, jumperObstacleX + 4, 48, jumperObstacleX + 8, 58, SSD1306_WHITE);
  if (jumperOver) { display.fillRect(22, 29, 84, 18, SSD1306_BLACK); display.drawRect(22, 29, 84, 18, SSD1306_WHITE); display.setCursor(28, 34); display.print("BONK! OK TO RETRY"); }
  display.display();
}
// ---------------- REACTION TAP ----------------
int reactionMode = 0; // 0 wait, 1 go, 2 result, 3 false start
uint32_t reactionStart = 0, reactionGoAt = 0, reactionResult = 0;
uint32_t reactionWait = 2500;
void startReaction() { reactionMode = 0; reactionWait = (1600 + (int)gameRandom(3000)); reactionStart = millis(); reactionGoAt = 0; reactionResult = 0; }
void drawReaction() {
  display.clearDisplay(); drawHeader(); display.setTextColor(SSD1306_WHITE); display.setTextSize(1); display.setCursor(0, 12); display.print("REACTION TAP");
  if (reactionMode == 0) {
    int pulse = (millis() / 180) % 2;
    display.drawCircle(64, 34, 8 + pulse * 2, SSD1306_WHITE); display.drawCircle(64, 34, 3, SSD1306_WHITE);
    display.setCursor(23, 50); display.print("WAIT FOR GO...");
    if (millis() - reactionStart >= reactionWait) { reactionMode = 1; reactionGoAt = millis(); }
  } else if (reactionMode == 1) {
    display.fillCircle(64, 32, 15, SSD1306_WHITE); display.setTextColor(SSD1306_BLACK); display.setTextSize(2); display.setCursor(43, 25); display.print("GO!"); display.setTextColor(SSD1306_WHITE); display.setTextSize(1); display.setCursor(30, 53); display.print("PRESS OK NOW!");
  } else if (reactionMode == 2) {
    display.setTextSize(2); display.setCursor(20, 25); display.print(reactionResult); display.setTextSize(1); display.setCursor(88, 31); display.print("ms");
    uint32_t best = prefs.getUInt("hsreact", 0); display.setCursor(18, 47); display.print("BEST: "); if (best) { display.print(best); display.print("ms"); } else display.print("--"); display.setCursor(8, 56); display.print("OK/START TRY AGAIN");
  } else {
    display.drawCircle(64, 31, 11, SSD1306_WHITE); display.drawLine(57, 24, 71, 38, SSD1306_WHITE); display.drawLine(71, 24, 57, 38, SSD1306_WHITE); display.setCursor(28, 51); display.print("TOO SOON! TRY AGAIN");
  }
  display.display();
}
// ---------------- SIMON MEMORY ----------------
uint8_t simonSeq[20];
int simonLen = 1, simonInput = 0, simonMode = 0, simonLit = -1;
uint32_t simonStarted = 0;
void startSimon() { simonLen = 1; simonInput = 0; simonMode = 0; simonLit = -1; simonSeq[0] = gameRandom(4); simonStarted = millis(); }
void tickSimon() {
  if (screen != SCR_SIMON || simonMode != 0) return;
  uint32_t elapsed = millis() - simonStarted;
  int slot = elapsed / 620;
  if (slot >= simonLen) { simonMode = 1; simonLit = -1; simonInput = 0; }
  else simonLit = ((elapsed % 620) < 360) ? simonSeq[slot] : -1;
}
void drawSimon() {
  display.clearDisplay(); drawHeader(); display.setTextSize(1); display.setTextColor(SSD1306_WHITE); display.setCursor(0, 11); display.print("SIMON MEMORY  LV:"); display.print(simonLen);
  const char *names[] = {"UP", "DOWN", "LEFT", "RIGHT"};
  const int xs[] = {9, 67, 9, 67}; const int ys[] = {22, 22, 39, 39};
  for (int i = 0; i < 4; ++i) {
    bool lit = (simonLit == i);
    if (lit) display.fillRoundRect(xs[i], ys[i], 51, 13, 3, SSD1306_WHITE); else display.drawRoundRect(xs[i], ys[i], 51, 13, 3, SSD1306_WHITE);
    display.setTextColor(lit ? SSD1306_BLACK : SSD1306_WHITE); display.setCursor(xs[i] + 12, ys[i] + 3); display.print(names[i]);
  }
  display.setTextColor(SSD1306_WHITE); display.setCursor(0, 55);
  if (simonMode == 0) display.print("WATCH THE PATTERN..."); else if (simonMode == 1) { display.print("REPEAT: "); display.print(simonInput); display.print("/"); display.print(simonLen); } else { display.print("WRONG! OK/START RETRY"); }
  display.display();
}
// ---------------- DODGE DROP ----------------
int dodgePlayer = 5, dodgeX[4], dodgeY[4];
bool dodgeActive[4], dodgeOver = false;
uint32_t dodgeScore = 0, dodgeLastTick = 0;
void startDodge() { dodgePlayer = 5; for (int i=0;i<4;i++) { dodgeActive[i] = false; dodgeX[i] = 0; dodgeY[i] = 0; } dodgeOver = false; dodgeScore = 0; dodgeLastTick = millis(); }
void tickDodge() {
  if (screen != SCR_DODGE || dodgeOver || millis() - dodgeLastTick < 210) return;
  dodgeLastTick = millis();
  for (int i=0;i<4;i++) if (dodgeActive[i]) { dodgeY[i]++; if (dodgeY[i] >= 6) { if (dodgeX[i] == dodgePlayer) dodgeOver = true; dodgeActive[i] = false; dodgeScore++; } }
  if (gameRandom(100) < 62) for (int i=0;i<4;i++) if (!dodgeActive[i]) { dodgeActive[i] = true; dodgeX[i] = gameRandom(10); dodgeY[i] = 0; break; }
  if (dodgeOver) { uint32_t hs = prefs.getUInt("hsdodge", 0); if (dodgeScore > hs) prefs.putUInt("hsdodge", dodgeScore); }
}
void drawDodge() {
  display.clearDisplay(); drawHeader(); display.setTextSize(1); display.setTextColor(SSD1306_WHITE); display.setCursor(0, 11); display.print("DODGE "); display.print(dodgeScore); display.setCursor(82, 11); display.print("B< S>");
  display.drawRect(13, 18, 102, 43, SSD1306_WHITE);
  for (int i=0;i<4;i++) if (dodgeActive[i]) display.fillRoundRect(16 + dodgeX[i]*10, 20 + dodgeY[i]*6, 6, 5, 1, SSD1306_WHITE);
  int px = 16 + dodgePlayer*10; display.fillTriangle(px, 58, px+4, 52, px+8, 58, SSD1306_WHITE);
  if (dodgeOver) { display.fillRect(19, 31, 91, 17, SSD1306_BLACK); display.drawRect(19, 31, 91, 17, SSD1306_WHITE); display.setCursor(25, 36); display.print("CRASH: OK REPLAY"); }
  display.display();
}
// ---------------- CALCULATOR ----------------
// 5x4 keypad includes digits 0-9, decimal point, four operations, clear, delete, double-zero and sign toggle.
const char *calcKeys[20] = {
  "7", "8", "9", "/", "C",
  "4", "5", "6", "*", "DEL",
  "1", "2", "3", "-", ".",
  "0", "00", "=", "+", "+/-"
};
int calcCursor = 0;
String calcInput = "";
String calcResult = "";

int findCalcOperator(const String &expr) {
  for (int i = 1; i < (int)expr.length(); ++i) {
    char c = expr[i];
    if (c == '+' || c == '-' || c == '*' || c == '/') return i;
  }
  return -1;
}

void evaluateCalc() {
  int opAt = findCalcOperator(calcInput);
  if (opAt < 1 || opAt >= (int)calcInput.length() - 1) {
    calcResult = "Need A op B";
    return;
  }
  char op = calcInput[opAt];
  String left = calcInput.substring(0, opAt);
  String right = calcInput.substring(opAt + 1);
  if (left.length() == 0 || right.length() == 0) { calcResult = "Incomplete"; return; }
  double a = left.toDouble(), b = right.toDouble(), r = 0.0;
  if (op == '+') r = a + b;
  else if (op == '-') r = a - b;
  else if (op == '*') r = a * b;
  else if (op == '/') {
    if (fabs(b) < 0.0000001) { calcResult = "DIV BY ZERO"; return; }
    r = a / b;
  }
  if (!isfinite(r)) { calcResult = "OVERFLOW"; return; }
  calcResult = String(r, 4);
  while (calcResult.indexOf('.') >= 0 && calcResult.endsWith("0")) calcResult.remove(calcResult.length() - 1);
  if (calcResult.endsWith(".")) calcResult.remove(calcResult.length() - 1);
  calcInput = calcResult;
  if (calcInput.length() > 15) calcInput = calcInput.substring(0, 15);
}

bool isCalcOperator(char c) { return c == '+' || c == '-' || c == '*' || c == '/'; }

void toggleCalcSign() {
  if (calcInput.length() == 0) { calcInput = "-"; calcResult = ""; return; }
  int opAt = findCalcOperator(calcInput);
  int operandStart = opAt >= 0 ? opAt + 1 : 0;
  if (operandStart < (int)calcInput.length() && calcInput[operandStart] == '-') {
    calcInput.remove(operandStart, 1);
  } else {
    calcInput = calcInput.substring(0, operandStart) + "-" + calcInput.substring(operandStart);
  }
  calcResult = "";
}

void appendCalcKey(const char *key) {
  String k(key);
  if (k == "C") { calcInput = ""; calcResult = ""; return; }
  if (k == "DEL") { if (calcInput.length()) calcInput.remove(calcInput.length() - 1); calcResult = ""; return; }
  if (k == "+/-") { toggleCalcSign(); return; }
  if (k == "=") { if (calcResult.length() && calcInput == calcResult) return; evaluateCalc(); return; }
  char c = k[0];
  bool isDigit = c >= '0' && c <= '9';
  if (calcResult.length() && (isDigit || c == '.')) { calcInput = ""; calcResult = ""; }
  else if (calcResult.length() && isCalcOperator(c)) {
    bool validResult = calcResult != "DIV BY ZERO" && calcResult != "OVERFLOW" && calcResult != "Need A op B" && calcResult != "Incomplete";
    if (validResult) calcInput = calcResult;
    else calcInput = (c == '-') ? "-" : "";
    calcResult = "";
    if (calcInput == "-" || calcInput.length() == 0) return;
  }
  if (isDigit) {
    if (calcInput.length() < 15) calcInput += k;
    calcResult = "";
    return;
  }
  if (c == '.') {
    int opAt = findCalcOperator(calcInput);
    String operand = opAt >= 0 ? calcInput.substring(opAt + 1) : calcInput;
    if (operand.indexOf('.') >= 0) return;
    if (calcInput.length() == 0 || (calcInput.length() && isCalcOperator(calcInput[calcInput.length() - 1]))) calcInput += "0";
    if (calcInput.length() < 15) calcInput += '.';
    calcResult = "";
    return;
  }
  if (isCalcOperator(c)) {
    if (calcInput.length() == 0) {
      if (c == '-') calcInput = "-";
      return;
    }
    char last = calcInput[calcInput.length() - 1];
    if (isCalcOperator(last)) calcInput[calcInput.length() - 1] = c;
    else if (findCalcOperator(calcInput) < 0 && calcInput.length() < 15) calcInput += c;
    else if (findCalcOperator(calcInput) >= 0) {
      evaluateCalc();
      if (calcResult.length() && calcResult != "DIV BY ZERO" && calcResult != "OVERFLOW" && calcResult != "Need A op B" && calcResult != "Incomplete") {
        calcInput = calcResult + String(c);
        calcResult = "";
      }
    }
  }
}

void selectCalcKey() { appendCalcKey(calcKeys[calcCursor]); }

void drawCalc() {
  display.clearDisplay(); drawHeader();
  display.setTextSize(1); display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 11); display.print("CALCULATOR 5x4");
  display.drawRoundRect(1, 19, 126, 10, 2, SSD1306_WHITE);
  display.setCursor(4, 21);
  String val = calcResult.length() ? calcResult : calcInput;
  if (val.length() > 20) val = val.substring(val.length() - 20);
  display.print(val);
  const int cellW = 25, cellH = 8, gridY = 30;
  for (int i = 0; i < 20; ++i) {
    int col = i % 5, row = i / 5;
    int x = col * cellW, y = gridY + row * cellH;
    bool selected = i == calcCursor;
    if (selected) display.fillRect(x, y, cellW - 1, cellH, SSD1306_WHITE);
    else display.drawRect(x, y, cellW - 1, cellH, SSD1306_WHITE);
    display.setTextColor(selected ? SSD1306_BLACK : SSD1306_WHITE);
    int textW = (int)strlen(calcKeys[i]) * 6;
    display.setCursor(x + max(0, (cellW - textW) / 2), y);
    display.print(calcKeys[i]);
  }
  display.display();
}

// ---------------- STOPWATCH ----------------
uint32_t stopwatchElapsedMs() {
  return stopwatchAccumulated + (stopwatchRunning ? (uint32_t)(millis() - stopwatchStartedAt) : 0U);
}
void resetStopwatch() {
  stopwatchRunning = false; stopwatchStartedAt = 0; stopwatchAccumulated = 0; stopwatchLaps = 0;
}
void drawStopwatch() {
  display.clearDisplay(); drawHeader(); display.setTextSize(1); display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 12); display.print("STOPWATCH");
  uint32_t elapsed = stopwatchElapsedMs();
  uint32_t minutes = elapsed / 60000UL;
  uint32_t seconds = (elapsed / 1000UL) % 60UL;
  uint32_t centis = (elapsed / 10UL) % 100UL;
  char buf[16]; snprintf(buf, sizeof(buf), "%02lu:%02lu.%02lu", (unsigned long)minutes, (unsigned long)seconds, (unsigned long)centis);
  display.setTextSize(2); display.setCursor(8, 25); display.print(buf);
  display.setTextSize(1); display.setCursor(0, 47); display.print(stopwatchRunning ? "RUNNING" : "PAUSED"); display.setCursor(75, 47); display.print("LAPS:"); display.print(stopwatchLaps);
  display.setCursor(0, 56); display.print("OK RUN/PAUSE  ST RESET");
  display.display();
}
void processStopwatch() {
  if (takePress(K_BACK)) { screen = SCR_MENU; lastInteraction = millis(); return; }
  if (takePress(K_CONFIRM)) {
    if (stopwatchRunning) { stopwatchAccumulated += millis() - stopwatchStartedAt; stopwatchRunning = false; }
    else { stopwatchStartedAt = millis(); stopwatchRunning = true; }
  }
  if (takePress(K_START)) resetStopwatch();
  if (takePress(K_UP)) stopwatchLaps++;
}

// ---------------- OFFLINE CLOCK EDITOR ----------------
struct tm clockEdit = {};
int clockField = 0;
int daysInMonth(int year, int month) {
  static const uint8_t days[] = {31,28,31,30,31,30,31,31,30,31,30,31};
  if (month == 1) {
    bool leap = ((year % 4 == 0) && (year % 100 != 0)) || (year % 400 == 0);
    return leap ? 29 : 28;
  }
  return days[month];
}
void startClockEditor() {
  time_t now = time(nullptr);
  if (timeIsValid()) localtime_r(&now, &clockEdit);
  else {
    // The firmware build time is an editable starting value if no clock has ever been set.
    const char *date = __DATE__, *clockText = __TIME__;
    const char *months = "JanFebMarAprMayJunJulAugSepOctNovDec";
    char monthName[4] = {date[0], date[1], date[2], '\0'};
    int month = 0;
    for (int i = 0; i < 12; ++i) if (strncmp(monthName, months + i * 3, 3) == 0) { month = i; break; }
    memset(&clockEdit, 0, sizeof(clockEdit));
    clockEdit.tm_year = atoi(date + 7) - 1900; clockEdit.tm_mon = month; clockEdit.tm_mday = atoi(date + 4);
    clockEdit.tm_hour = atoi(clockText); clockEdit.tm_min = atoi(clockText + 3); clockEdit.tm_sec = atoi(clockText + 6);
    time_t approxLocal = mktime(&clockEdit) + 7 * 3600;
    localtime_r(&approxLocal, &clockEdit);
  }
  clockEdit.tm_sec = 0; clockField = 0;
}
void adjustClockField(int delta) {
  int year = clockEdit.tm_year + 1900;
  if (clockField == 0) { year = constrain(year + delta, 2024, 2037); clockEdit.tm_year = year - 1900; }
  else if (clockField == 1) clockEdit.tm_mon = (clockEdit.tm_mon + delta + 12) % 12;
  else if (clockField == 2) clockEdit.tm_mday = constrain(clockEdit.tm_mday + delta, 1, daysInMonth(year, clockEdit.tm_mon));
  else if (clockField == 3) clockEdit.tm_hour = (clockEdit.tm_hour + delta + 24) % 24;
  else clockEdit.tm_min = (clockEdit.tm_min + delta + 60) % 60;
  clockEdit.tm_mday = min(clockEdit.tm_mday, daysInMonth(year, clockEdit.tm_mon));
}
void drawClockEditor() {
  display.clearDisplay(); drawHeader(); display.setTextSize(1); display.setTextColor(SSD1306_WHITE); display.setCursor(0, 12); display.print("SET CLOCK OFFLINE");
  char date[16], clockText[8]; snprintf(date, sizeof(date), "%04d-%02d-%02d", clockEdit.tm_year + 1900, clockEdit.tm_mon + 1, clockEdit.tm_mday);
  snprintf(clockText, sizeof(clockText), "%02d:%02d", clockEdit.tm_hour, clockEdit.tm_min);
  display.setCursor(0, 25); display.print("DATE"); display.setCursor(32, 25); display.print(date);
  display.setCursor(0, 38); display.print("TIME"); display.setCursor(32, 38); display.print(clockText);
  int x = 31, y = 24, w = 26;
  if (clockField == 1) { x = 61; w = 14; }
  else if (clockField == 2) { x = 79; w = 14; }
  else if (clockField == 3) { x = 31; y = 37; w = 14; }
  else if (clockField == 4) { x = 49; y = 37; w = 14; }
  display.drawRect(x, y, w, 10, SSD1306_INVERSE);
  display.setCursor(0, 47); display.print("UP/DN CHANGE VALUE");
  display.setCursor(0, 56); display.print("B/S FIELD  OK SAVE"); display.display();
}
void processClockEditor() {
  if (takeLong(K_BACK)) { for (auto &k : keys) { k.pressedEvent = false; k.longEvent = false; } screen = SCR_SETTINGS; return; }
  bool up = takePress(K_UP), down = takePress(K_DOWN);
  if (up || down) adjustClockField(up ? 1 : -1);
  if (takePress(K_BACK)) clockField = (clockField + 4) % 5;
  if (takePress(K_START)) clockField = (clockField + 1) % 5;
  if (takePress(K_CONFIRM)) {
    clockEdit.tm_sec = 0; time_t epoch = mktime(&clockEdit);
    if (epoch > 1700000000 && epoch < 2145916800LL) setTimeFromEpoch((long)epoch);
    screen = SCR_SETTINGS;
  }
}

// ---------------- SETTINGS ----------------
void drawSettings() {
  display.clearDisplay(); drawHeader(); display.setTextSize(1); display.setTextColor(SSD1306_WHITE); display.setCursor(0, 12); display.print("SYSTEM SETTINGS");
  const char *labels[] = {"Animations", "Auto clock 10s", "Wi-Fi radio", "Set clock offline"};
  for (int i = 0; i < 4; ++i) {
    int y = 20 + i * 8; bool selected = settingsIndex == i;
    if (selected) display.fillRoundRect(0, y - 1, 128, 8, 2, SSD1306_WHITE);
    display.setTextColor(selected ? SSD1306_BLACK : SSD1306_WHITE); display.setCursor(4, y); display.print(labels[i]); display.setCursor(100, y);
    if (i == 0) display.print(animationsEnabled ? "ON" : "OFF");
    else if (i == 1) display.print(autoClockEnabled ? "ON" : "OFF");
    else if (i == 2) display.print(wifiEnabled ? "ON" : "OFF");
    else display.print("OPEN");
  }
  display.setTextColor(SSD1306_WHITE); display.setCursor(0, 55); display.print("UP/DN MOVE OK SELECT"); display.display();
}
void processSettings() {
  if (takePress(K_BACK)) { screen = SCR_MENU; lastInteraction = millis(); return; }
  bool up = takePress(K_UP), down = takePress(K_DOWN);
  if (up || down) settingsIndex = (settingsIndex + (up ? 3 : 1)) % 4;
  if (takeConfirmOrStart()) {
    if (settingsIndex == 0) { animationsEnabled = !animationsEnabled; prefs.putBool("anim", animationsEnabled); }
    else if (settingsIndex == 1) { autoClockEnabled = !autoClockEnabled; prefs.putBool("autoclk", autoClockEnabled); }
    else if (settingsIndex == 2) setWifiEnabled(!wifiEnabled);
    else { screen = SCR_CLOCK_EDIT; startClockEditor(); lastInteraction = millis(); }
  }
}

void enterScreen(Screen s) {
  screen = s; lastInteraction = millis();
  if (s == SCR_BLOCK) startBlockGame();
  if (s == SCR_BREAKER) startBreaker();
  if (s == SCR_ONELINE) startOneLine();
  if (s == SCR_TTT) startTtt();
  if (s == SCR_SNAKE) startSnake();
  if (s == SCR_PONG) startPong();
  if (s == SCR_JUMPER) startJumper();
  if (s == SCR_REACTION) startReaction();
  if (s == SCR_SIMON) startSimon();
  if (s == SCR_DODGE) startDodge();
  if (s == SCR_ROULETTE) startRoulette();
  if (s == SCR_COIN) startCoin();
  if (s == SCR_MINES) startMines();
  if (s == SCR_2048) start2048();
  if (s == SCR_RPS) startRps();
  if (s == SCR_DICE) startDice();
  if (s == SCR_GUESS) startGuess();
  if (s == SCR_LIGHTS) startLightsOut();
  if (s == SCR_PAIRS) startMemoryPairs();
  if (s == SCR_CLOCK_EDIT) startClockEditor();
  if (s == SCR_CALC) { calcInput = ""; calcResult = ""; calcCursor = 0; }
  if (s == SCR_STOPWATCH) resetStopwatch();
  if (s == SCR_WIFI_SCAN) doScan();
}

void processMenu() {
  if (takePress(K_UP)) { menuIndex = (menuIndex + MAIN_COUNT - 1) % MAIN_COUNT; }
  if (takePress(K_DOWN)) { menuIndex = (menuIndex + 1) % MAIN_COUNT; }
  if (takePress(K_BACK)) { screen = SCR_CLOCK; }
  if (takeConfirmOrStart()) {
    switch (menuIndex) {
      case 0: enterScreen(SCR_WIFI_SCAN); break;
      case 1: screen = SCR_GAMES; gameMenuIndex = 0; lastInteraction = millis(); break;
      case 2: enterScreen(SCR_CALC); break;
      case 3: enterScreen(SCR_STOPWATCH); break;
      case 4: enterScreen(SCR_NETWORK); break;
      case 5: enterScreen(SCR_SCORES); break;
      case 6: enterScreen(SCR_SETTINGS); break;
      default: enterScreen(SCR_ABOUT); break;
    }
  }
  if (autoClockEnabled && screen == SCR_MENU && millis() - lastInteraction > 10000) { animateClockTransition(); screen = SCR_CLOCK; }
}

void processGameMenu() {
  if (takePress(K_UP)) gameMenuIndex = (gameMenuIndex + GAME_COUNT - 1) % GAME_COUNT;
  if (takePress(K_DOWN)) gameMenuIndex = (gameMenuIndex + 1) % GAME_COUNT;
  if (takePress(K_BACK)) { screen = SCR_MENU; lastInteraction = millis(); }
  if (takeConfirmOrStart()) {
    switch (gameMenuIndex) {
      case 0: enterScreen(SCR_BLOCK); break;
      case 1: enterScreen(SCR_BREAKER); break;
      case 2: enterScreen(SCR_ONELINE); break;
      case 3: enterScreen(SCR_TTT); break;
      case 4: enterScreen(SCR_SNAKE); break;
      case 5: enterScreen(SCR_PONG); break;
      case 6: enterScreen(SCR_JUMPER); break;
      case 7: enterScreen(SCR_REACTION); break;
      case 8: enterScreen(SCR_SIMON); break;
      case 9: enterScreen(SCR_DODGE); break;
      case 10: enterScreen(SCR_ROULETTE); break;
      case 11: enterScreen(SCR_COIN); break;
      case 12: enterScreen(SCR_MINES); break;
      case 13: enterScreen(SCR_2048); break;
      case 14: enterScreen(SCR_RPS); break;
      case 15: enterScreen(SCR_DICE); break;
      case 16: enterScreen(SCR_GUESS); break;
      case 17: enterScreen(SCR_LIGHTS); break;
      case 18: enterScreen(SCR_PAIRS); break;
      default: screen = SCR_MENU; break;
    }
  }
}

void processBlock() {
  // Physical controls in games: UP/DOWN move vertically, BACK(short) left,
  // START right, CONFIRM places the selected shape; hold BACK to leave.
  if (takeLong(K_BACK)) { for (auto &k : keys) { k.pressedEvent = false; k.longEvent = false; } screen = SCR_GAMES; lastInteraction = millis(); return; }
  if (blockOver) { takePress(K_BACK); takePress(K_UP); takePress(K_DOWN); if (takeConfirmOrStart()) startBlockGame(); return; }
  if (takePress(K_BACK) && blockX > 0) blockX--;
  if (takePress(K_UP) && blockY > 0) blockY--;
  if (takePress(K_DOWN) && blockY < 8 - blockPieceHeight()) blockY++;
  if (takePress(K_START) && blockX < 8 - blockPieceWidth()) blockX++;
  if (takePress(K_CONFIRM)) placeBlockPiece();
}

void processBreaker() {
  if (takeLong(K_BACK)) { for (auto &k : keys) { k.pressedEvent = false; k.longEvent = false; } screen = SCR_GAMES; lastInteraction = millis(); return; }
  if (breakerOver) { takePress(K_BACK); takePress(K_UP); takePress(K_DOWN); if (takeConfirmOrStart()) startBreaker(); return; }
  if (takePress(K_BACK)) paddleX = max(0, paddleX - 7);
  if (takePress(K_START)) paddleX = min(106, paddleX + 7);
  bool launchConfirm = takePress(K_CONFIRM);
  bool launchUp = takePress(K_UP);
  bool launchDown = takePress(K_DOWN);
  bool launch = launchConfirm || launchUp || launchDown;
  if (launch && !ballLaunched) { ballLaunched = true; ballX = paddleX + 11; ballY = 55; lastBallTick = millis(); }
  tickBreaker();
}

void processOneLine() {
  if (takeLong(K_BACK)) { for (auto &k : keys) { k.pressedEvent = false; k.longEvent = false; } screen = SCR_GAMES; lastInteraction = millis(); return; }
  if (oneWon) { takePress(K_BACK); takePress(K_UP); takePress(K_DOWN); if (takeConfirmOrStart()) { oneLevel++; startOneLineLevel(); } return; }
  if (oneFailed) { takePress(K_BACK); takePress(K_UP); takePress(K_DOWN); if (takeConfirmOrStart()) startOneLineLevel(); return; }
  if (takePress(K_BACK)) moveOneLine(-1, 0);
  if (takePress(K_START)) moveOneLine(1, 0);
  if (takePress(K_UP)) moveOneLine(0, -1);
  if (takePress(K_DOWN)) moveOneLine(0, 1);
  if (takePress(K_CONFIRM) && oneCount > 1) {
    oneVisited[onePath[oneCount - 1]] = false;
    oneCount--;
    onePos = onePath[oneCount - 1];
  }
}

void processTtt() {
  if (takeLong(K_BACK)) { for (auto &k : keys) { k.pressedEvent = false; k.longEvent = false; } screen = SCR_GAMES; lastInteraction = millis(); return; }
  if (tttWinner) { takePress(K_BACK); takePress(K_UP); takePress(K_DOWN); if (takeConfirmOrStart()) startTtt(); return; }
  int row = tttCursor / 3, col = tttCursor % 3;
  if (takePress(K_UP)) row = (row + 2) % 3;
  if (takePress(K_DOWN)) row = (row + 1) % 3;
  if (takePress(K_BACK)) col = (col + 2) % 3;
  if (takePress(K_START)) col = (col + 1) % 3;
  tttCursor = row * 3 + col;
  if (takePress(K_CONFIRM) && !ttt[tttCursor]) {
    ttt[tttCursor] = tttPlayer; tttWinner = checkTtt();
    if (!tttWinner) tttPlayer = (tttPlayer == 1 ? 2 : 1);
  }
}

void processSnake() {
  if (takeLong(K_BACK)) { for (auto &k : keys) { k.pressedEvent = false; k.longEvent = false; } screen = SCR_GAMES; lastInteraction = millis(); return; }
  if (snakeOver) { takePress(K_BACK); takePress(K_UP); takePress(K_DOWN); if (takeConfirmOrStart()) startSnake(); return; }
  if (takePress(K_UP) && snakeDy != 1) { snakeDx = 0; snakeDy = -1; }
  if (takePress(K_DOWN) && snakeDy != -1) { snakeDx = 0; snakeDy = 1; }
  if (takePress(K_BACK) && snakeDx != 1) { snakeDx = -1; snakeDy = 0; }
  if (takePress(K_START) && snakeDx != -1) { snakeDx = 1; snakeDy = 0; }
}

void processPong() {
  if (takeLong(K_BACK)) { for (auto &k : keys) { k.pressedEvent = false; k.longEvent = false; } screen = SCR_GAMES; lastInteraction = millis(); return; }
  if (pongOver) { takePress(K_BACK); takePress(K_UP); takePress(K_DOWN); if (takeConfirmOrStart()) startPong(); return; }
  if (takePress(K_UP)) pongPaddleY = max(18, pongPaddleY - 5);
  if (takePress(K_DOWN)) pongPaddleY = min(41, pongPaddleY + 5);
  takePress(K_BACK); // Left is not needed for this vertical-paddle game.
  bool startPressed = takePress(K_START);
  bool confirmPressed = takePress(K_CONFIRM);
  if (startPressed || confirmPressed) {
    if (!pongLaunched) { pongLaunched = true; pongLastTick = millis(); }
  }
}

void processJumper() {
  if (takeLong(K_BACK)) { for (auto &k : keys) { k.pressedEvent = false; k.longEvent = false; } screen = SCR_GAMES; lastInteraction = millis(); return; }
  if (jumperOver) { takePress(K_BACK); takePress(K_UP); takePress(K_DOWN); if (takeConfirmOrStart()) startJumper(); return; }
  takePress(K_BACK); // Short BACK has no action in the auto-runner; hold BACK exits.
  bool up = takePress(K_UP), ok = takePress(K_CONFIRM), start = takePress(K_START);
  takePress(K_DOWN);
  if ((up || ok || start) && jumperGrounded) { jumperGrounded = false; jumperVy = -5.2f; }
}

void processReaction() {
  if (takeLong(K_BACK)) { for (auto &k : keys) { k.pressedEvent = false; k.longEvent = false; } screen = SCR_GAMES; lastInteraction = millis(); return; }
  takePress(K_BACK);
  bool ok = takePress(K_CONFIRM), start = takePress(K_START);
  takePress(K_UP); takePress(K_DOWN);
  if (start || ((reactionMode == 2 || reactionMode == 3) && ok)) { startReaction(); return; }
  if (ok && reactionMode == 0) reactionMode = 3;
  else if (ok && reactionMode == 1) {
    reactionResult = millis() - reactionGoAt; reactionMode = 2;
    uint32_t best = prefs.getUInt("hsreact", 0); if (!best || reactionResult < best) prefs.putUInt("hsreact", reactionResult);
  }
}

void processSimon() {
  if (takeLong(K_BACK)) { for (auto &k : keys) { k.pressedEvent = false; k.longEvent = false; } screen = SCR_GAMES; lastInteraction = millis(); return; }
  bool left = takePress(K_BACK);
  bool up = takePress(K_UP), down = takePress(K_DOWN), ok = takePress(K_CONFIRM), right = takePress(K_START);
  if (simonMode == 2) { if (ok || right) startSimon(); return; }
  if (simonMode != 1) return;
  int input = up ? 0 : (down ? 1 : (left ? 2 : (right ? 3 : -1)));
  if (input < 0) return;
  if (input != simonSeq[simonInput]) {
    simonMode = 2; uint32_t hs = prefs.getUInt("hssimon", 0); if ((uint32_t)(simonLen - 1) > hs) prefs.putUInt("hssimon", simonLen - 1); return;
  }
  simonInput++;
  if (simonInput >= simonLen) {
    uint32_t hs = prefs.getUInt("hssimon", 0); if ((uint32_t)simonLen > hs) prefs.putUInt("hssimon", simonLen);
    if (simonLen >= 19) { simonMode = 2; return; }
    simonSeq[simonLen++] = gameRandom(4); simonMode = 0; simonStarted = millis(); simonInput = 0;
  }
}

void processDodge() {
  if (takeLong(K_BACK)) { for (auto &k : keys) { k.pressedEvent = false; k.longEvent = false; } screen = SCR_GAMES; lastInteraction = millis(); return; }
  if (dodgeOver) { takePress(K_BACK); takePress(K_UP); takePress(K_DOWN); if (takeConfirmOrStart()) startDodge(); return; }
  if (takePress(K_BACK) && dodgePlayer > 0) dodgePlayer--;
  if (takePress(K_START) && dodgePlayer < 9) dodgePlayer++;
  takePress(K_UP); takePress(K_DOWN); takePress(K_CONFIRM);
}


// ---------------- ROULETTE 0-36 ----------------
int rouletteNumber = -1, rouletteStep = 0, rouletteFinal = 0;
uint32_t rouletteLastTick = 0, rouletteSpins = 0;
bool rouletteSpinning = false;
bool rouletteIsRed(int n) {
  const uint8_t red[] = {1,3,5,7,9,12,14,16,18,19,21,23,25,27,30,32,34,36};
  for (uint8_t i = 0; i < sizeof(red); ++i) if (red[i] == n) return true;
  return false;
}
void startRoulette() { rouletteNumber = -1; rouletteStep = 0; rouletteFinal = 0; rouletteSpinning = false; rouletteSpins = 0; rouletteLastTick = 0; }
void spinRoulette() { if (rouletteSpinning) return; rouletteSpinning = true; rouletteStep = 0; rouletteLastTick = millis(); rouletteFinal = (int)gameRandom(37); rouletteNumber = (int)gameRandom(37); }
void processRoulette() {
  if (takeLong(K_BACK)) { for (auto &k : keys) { k.pressedEvent = false; k.longEvent = false; } screen = SCR_GAMES; lastInteraction = millis(); return; }
  if (takePress(K_BACK)) { screen = SCR_GAMES; lastInteraction = millis(); return; }
  if (takeConfirmOrStart() && !rouletteSpinning) spinRoulette();
  if (rouletteSpinning && millis() - rouletteLastTick >= (uint32_t)(35 + rouletteStep * 5)) {
    rouletteLastTick = millis(); rouletteNumber = (int)gameRandom(37); rouletteStep++;
    if (rouletteStep >= 18) { rouletteSpinning = false; rouletteNumber = rouletteFinal; rouletteSpins++; }
  }
}
void drawRoulette() {
  display.clearDisplay(); drawHeader(); display.setTextSize(1); display.setTextColor(SSD1306_WHITE); display.setCursor(0, 11); display.print("EURO ROULETTE");
  display.drawCircle(64, 31, 17, SSD1306_WHITE); display.drawCircle(64, 31, 12, SSD1306_WHITE);
  for (int i = 0; i < 12; ++i) { float a = i * 0.523599f; display.drawPixel(64 + (int)(14 * cos(a)), 31 + (int)(14 * sin(a)), SSD1306_WHITE); }
  if (rouletteNumber < 0) { display.setTextSize(2); display.setCursor(58, 25); display.print("?"); }
  else { display.fillRoundRect(48, 24, 32, 16, 4, SSD1306_WHITE); display.setTextColor(SSD1306_BLACK); display.setTextSize(2); display.setCursor(rouletteNumber >= 10 ? 51 : 57, 24); display.print(rouletteNumber); display.setTextColor(SSD1306_WHITE); }
  display.setTextSize(1); display.setCursor(0, 48);
  if (rouletteSpinning) display.print("SPINNING...");
  else if (rouletteNumber < 0) display.print("0-36  OK=SPIN");
  else { display.print(rouletteNumber == 0 ? "GREEN" : (rouletteIsRed(rouletteNumber) ? "RED" : "BLACK")); display.setCursor(65, 48); display.print("OK=AGAIN"); }
  display.setCursor(0, 56); display.print("SPINS:"); display.print(rouletteSpins); display.setCursor(75, 56); display.print("HOLD B"); display.display();
}

// ---------------- COIN FLIP ----------------
bool coinDone = false, coinHeads = true, coinFinalHeads = true, coinFlipping = false;
uint8_t coinFrames = 0; uint32_t coinLastTick = 0, coinHeadsCount = 0, coinTailsCount = 0, coinStreak = 0;
int coinLastSide = -1;
void startCoin() { coinDone = false; coinHeads = true; coinFinalHeads = true; coinFlipping = false; coinFrames = 0; coinLastTick = 0; coinHeadsCount = coinTailsCount = coinStreak = 0; coinLastSide = -1; }
void flipCoin() { if (coinFlipping) return; coinFlipping = true; coinDone = false; coinFrames = 0; coinLastTick = millis(); coinFinalHeads = gameRandom(2) == 0; }
void processCoin() {
  if (takeLong(K_BACK)) { for (auto &k : keys) { k.pressedEvent = false; k.longEvent = false; } screen = SCR_GAMES; lastInteraction = millis(); return; }
  if (takePress(K_BACK)) { screen = SCR_GAMES; lastInteraction = millis(); return; }
  if (takeConfirmOrStart() && !coinFlipping) flipCoin();
  if (coinFlipping && millis() - coinLastTick >= 75UL) {
    coinLastTick = millis(); coinFrames++; coinHeads = gameRandom(2) == 0;
    if (coinFrames >= 12) {
      coinFlipping = false; coinDone = true; coinHeads = coinFinalHeads;
      int side = coinHeads ? 0 : 1; if (coinHeads) coinHeadsCount++; else coinTailsCount++;
      if (coinLastSide == side) coinStreak++; else coinStreak = 1; coinLastSide = side;
    }
  }
}
void drawCoin() {
  display.clearDisplay(); drawHeader(); display.setTextSize(1); display.setTextColor(SSD1306_WHITE); display.setCursor(0, 11); display.print("COIN FLIP");
  int r = coinFlipping ? ((coinFrames % 2) ? 8 : 16) : 17; display.drawCircle(64, 33, r, SSD1306_WHITE); display.drawCircle(64, 33, max(2, r - 3), SSD1306_WHITE);
  display.setTextSize(2); display.setCursor(58, 27); display.print(coinFlipping ? ((coinFrames % 2) ? "H" : "T") : (coinHeads ? "H" : "T"));
  if (!coinFlipping) {
    if (coinHeads) { display.drawLine(59, 21, 64, 17, SSD1306_WHITE); display.drawLine(64, 17, 69, 21, SSD1306_WHITE); display.drawPixel(64, 16, SSD1306_WHITE); }
    else { display.drawFastHLine(59, 19, 10, SSD1306_WHITE); display.drawFastHLine(59, 23, 10, SSD1306_WHITE); }
  }
  display.setTextSize(1); display.setCursor(0, 23); display.print("H:"); display.print(coinHeadsCount); display.setCursor(0, 34); display.print("T:"); display.print(coinTailsCount); display.setCursor(0, 45); display.print("STK:"); display.print(coinStreak);
  display.setCursor(0, 56); display.print(coinFlipping ? "FLIPPING..." : (coinDone ? (coinHeads ? "HEADS! OK FLIP" : "TAILS! OK FLIP") : "OK=FLIP")); display.display();
}

// ---------------- MINESWEEPER 4x4 ----------------
uint8_t mineMap[4][4], mineReveal[4][4], mineFlagged[4][4];
int mineX = 0, mineY = 0, mineCount = 3, mineSafeCount = 0;
bool minesOver = false, minesWin = false, mineConfirmPending = false;
void startMines() {
  memset(mineMap, 0, sizeof(mineMap)); memset(mineReveal, 0, sizeof(mineReveal)); memset(mineFlagged, 0, sizeof(mineFlagged));
  mineX = mineY = mineSafeCount = 0; minesOver = minesWin = mineConfirmPending = false;
  int placed = 0, guard = 0;
  while (placed < mineCount && guard++ < 100) { int q = 1 + (int)gameRandom(15), x = q % 4, y = q / 4; if (!mineMap[y][x]) { mineMap[y][x] = 1; placed++; } }
}
int mineNeighbors(int x, int y) {
  int n = 0;
  for (int dy = -1; dy <= 1; ++dy) for (int dx = -1; dx <= 1; ++dx) {
    int xx = x + dx, yy = y + dy;
    if ((dx || dy) && xx >= 0 && xx < 4 && yy >= 0 && yy < 4) n += mineMap[yy][xx];
  }
  return n;
}
void revealMineCell() {
  if (minesOver || mineReveal[mineY][mineX] || mineFlagged[mineY][mineX]) return;
  if (mineMap[mineY][mineX]) { minesOver = true; minesWin = false; return; }
  mineReveal[mineY][mineX] = 1;
  bool changed = true;
  while (changed) {
    changed = false;
    for (int y = 0; y < 4; ++y) for (int x = 0; x < 4; ++x) {
      if (!mineReveal[y][x] || mineMap[y][x] || mineNeighbors(x, y) != 0) continue;
      for (int dy = -1; dy <= 1; ++dy) for (int dx = -1; dx <= 1; ++dx) {
        int xx = x + dx, yy = y + dy;
        if (xx < 0 || xx >= 4 || yy < 0 || yy >= 4 || mineMap[yy][xx] || mineFlagged[yy][xx] || mineReveal[yy][xx]) continue;
        mineReveal[yy][xx] = 1; changed = true;
      }
    }
  }
  mineSafeCount = 0;
  for (int y = 0; y < 4; ++y) for (int x = 0; x < 4; ++x) if (mineReveal[y][x] && !mineMap[y][x]) mineSafeCount++;
  if (mineSafeCount >= 16 - mineCount) { minesOver = true; minesWin = true; }
}
void processMines() {
  if (takeLong(K_BACK)) { for (auto &k : keys) { k.pressedEvent = false; k.longEvent = false; } screen = SCR_GAMES; lastInteraction = millis(); return; }
  if (minesOver) { takePress(K_BACK); takePress(K_START); takePress(K_UP); takePress(K_DOWN); if (takePress(K_CONFIRM)) startMines(); return; }
  if (takePress(K_UP)) mineY = (mineY + 3) % 4;
  if (takePress(K_DOWN)) mineY = (mineY + 1) % 4;
  if (takePress(K_BACK)) mineX = (mineX + 3) % 4;
  if (takePress(K_START)) mineX = (mineX + 1) % 4;
  if (takeLong(K_CONFIRM)) {
    takePress(K_CONFIRM); mineConfirmPending = false;
    if (!mineReveal[mineY][mineX]) mineFlagged[mineY][mineX] = !mineFlagged[mineY][mineX];
    return;
  }
  if (takePress(K_CONFIRM)) mineConfirmPending = true;
  if (mineConfirmPending && keys[K_CONFIRM].stable == HIGH) { mineConfirmPending = false; revealMineCell(); }
}
void drawMines() {
  display.clearDisplay(); drawHeader(); display.setTextSize(1); display.setTextColor(SSD1306_WHITE); display.setCursor(0, 11); display.print("MINESWEEPER");
  display.setCursor(0, 23); display.print("MINES:"); display.print(mineCount); display.setCursor(0, 34); display.print("SAFE:"); display.print(mineSafeCount); display.print("/13");
  display.setCursor(0, 45); display.print(minesOver ? (minesWin ? "YOU WIN!" : "BOOM!") : "TAP=OPEN"); display.setCursor(0, 55); display.print(minesOver ? "OK RETRY" : "HOLD OK FLAG");
  const int ox = 74, oy = 19, cw = 12, ch = 9;
  for (int y = 0; y < 4; ++y) for (int x = 0; x < 4; ++x) {
    int px = ox + x * cw, py = oy + y * ch;
    if (minesOver || mineReveal[y][x]) {
      display.drawRect(px, py, 9, 8, SSD1306_WHITE);
      if (mineMap[y][x]) { display.fillCircle(px + 4, py + 3, 3, SSD1306_WHITE); display.drawPixel(px + 4, py + 3, SSD1306_BLACK); }
      else { int n = mineNeighbors(x, y); if (n) { display.setCursor(px + 2, py); display.print(n); } }
    } else {
      display.drawRect(px, py, 9, 8, SSD1306_WHITE);
      if (mineFlagged[y][x]) { display.setCursor(px + 2, py); display.print("F"); }
    }
  }
  display.drawRect(ox + mineX * cw - 1, oy + mineY * ch - 1, 11, 10, SSD1306_INVERSE); display.display();
}

// ---------------- 2048 PUZZLE ----------------
// Tiles store exponents: 1 means 2, 2 means 4, and so on.
uint8_t board2048[4][4];
int g2048x = 0, g2048y = 0;
bool game2048Over = false;
uint32_t score2048 = 0;
void spawn2048() {
  int empty[16], n = 0;
  for (int y = 0; y < 4; ++y) for (int x = 0; x < 4; ++x) if (!board2048[y][x]) empty[n++] = y * 4 + x;
  if (n) { int q = empty[gameRandom(n)]; board2048[q / 4][q % 4] = (gameRandom(10) == 0 ? 2 : 1); }
}
void start2048() { memset(board2048, 0, sizeof(board2048)); score2048 = 0; game2048Over = false; g2048x = g2048y = 0; spawn2048(); spawn2048(); }
void move2048(int dir) {
  if (game2048Over) return;
  bool changed = false;
  for (int line = 0; line < 4; ++line) {
    uint8_t a[4] = {0,0,0,0}; int n = 0;
    for (int j = 0; j < 4; ++j) {
      int x = (dir == 0 ? j : (dir == 1 ? 3 - j : line));
      int y = (dir == 2 ? j : (dir == 3 ? 3 - j : line));
      uint8_t v = board2048[y][x]; if (v) a[n++] = v;
    }
    for (int i = 0; i < n - 1; ++i) if (a[i] == a[i + 1]) {
      if (a[i] < 30) a[i]++;
      score2048 += (a[i] < 32 ? (1UL << a[i]) : 0);
      for (int k = i + 1; k < n - 1; ++k) a[k] = a[k + 1];
      a[n - 1] = 0; n--;
    }
    for (int j = 0; j < 4; ++j) {
      int x = (dir == 0 ? j : (dir == 1 ? 3 - j : line));
      int y = (dir == 2 ? j : (dir == 3 ? 3 - j : line));
      if (board2048[y][x] != a[j]) changed = true;
      board2048[y][x] = a[j];
    }
  }
  if (changed) {
    spawn2048(); uint32_t best = prefs.getUInt("hs2048", 0); if (score2048 > best) prefs.putUInt("hs2048", score2048);
    bool moves = false;
    for (int y = 0; y < 4; ++y) for (int x = 0; x < 4; ++x) {
      if (!board2048[y][x]) moves = true;
      if (x < 3 && board2048[y][x] == board2048[y][x + 1]) moves = true;
      if (y < 3 && board2048[y][x] == board2048[y + 1][x]) moves = true;
    }
    game2048Over = !moves;
  }
}
void process2048() {
  if (takeLong(K_BACK)) { for (auto &k : keys) { k.pressedEvent = false; k.longEvent = false; } screen = SCR_GAMES; lastInteraction = millis(); return; }
  bool left = takePress(K_BACK), right = takePress(K_START), up = takePress(K_UP), down = takePress(K_DOWN), ok = takePress(K_CONFIRM);
  if (game2048Over) { if (ok || right) start2048(); return; }
  if (left) move2048(0); else if (right) move2048(1); else if (up) move2048(2); else if (down) move2048(3);
}
void draw2048() {
  display.clearDisplay(); drawHeader(); display.setTextSize(1); display.setTextColor(SSD1306_WHITE); display.setCursor(0, 11); display.print("2048 SCORE:"); display.print(score2048);
  for (int y = 0; y < 4; ++y) for (int x = 0; x < 4; ++x) {
    int px = 4 + x * 30, py = 20 + y * 9; display.drawRect(px, py, 27, 8, SSD1306_WHITE); uint8_t v = board2048[y][x];
    if (v) { display.setCursor(px + 2, py); if (v <= 11) display.print(1UL << v); else display.print("MAX"); }
  }
  display.setCursor(0, 56); display.print(game2048Over ? "GAME OVER: OK RESET" : "ARROWS MOVE / HOLD B EXIT"); display.display();
}

// ---------------- ROCK / PAPER / SCISSORS VS CPU ----------------
int rpsChoice = 0, rpsCpu = -1, rpsOutcome = -1, rpsWins = 0, rpsLosses = 0, rpsDraws = 0;
const char *rpsNames[] = {"ROCK", "PAPER", "SCISSORS"};
void startRps() { rpsChoice = 0; rpsCpu = -1; rpsOutcome = -1; rpsWins = rpsLosses = rpsDraws = 0; }
void playRps() {
  rpsCpu = (int)gameRandom(3);
  if (rpsChoice == rpsCpu) { rpsOutcome = 0; rpsDraws++; }
  else if ((rpsChoice == 0 && rpsCpu == 2) || (rpsChoice == 1 && rpsCpu == 0) || (rpsChoice == 2 && rpsCpu == 1)) { rpsOutcome = 1; rpsWins++; }
  else { rpsOutcome = 2; rpsLosses++; }
}
void processRps() {
  if (takeLong(K_BACK)) { for (auto &k : keys) { k.pressedEvent = false; k.longEvent = false; } screen = SCR_GAMES; lastInteraction = millis(); return; }
  if (takePress(K_BACK)) rpsChoice = (rpsChoice + 2) % 3;
  if (takePress(K_START)) rpsChoice = (rpsChoice + 1) % 3;
  if (takePress(K_UP)) rpsChoice = (rpsChoice + 2) % 3;
  if (takePress(K_DOWN)) rpsChoice = (rpsChoice + 1) % 3;
  if (takePress(K_CONFIRM)) playRps();
}
void drawRps() {
  display.clearDisplay(); drawHeader(); display.setTextSize(1); display.setTextColor(SSD1306_WHITE); display.setCursor(0, 11); display.print("RPS VS CPU");
  display.setCursor(0, 23); display.print("YOU: "); display.print(rpsNames[rpsChoice]);
  display.setCursor(0, 34); display.print("CPU: "); if (rpsCpu < 0) display.print("--"); else display.print(rpsNames[rpsCpu]);
  display.setCursor(0, 45); if (rpsOutcome < 0) display.print("OK TO PLAY"); else if (rpsOutcome == 0) display.print("DRAW!"); else if (rpsOutcome == 1) display.print("YOU WIN!"); else display.print("CPU WINS!");
  display.setCursor(0, 56); display.print("W:"); display.print(rpsWins); display.print(" L:"); display.print(rpsLosses); display.print(" D:"); display.print(rpsDraws); display.display();
}

// ---------------- DICE ROLLER ----------------
int diceA = 1, diceB = 1, diceFinalA = 1, diceFinalB = 1;
bool diceRolling = false;
uint32_t diceRollStart = 0, diceLastTick = 0, diceRollCount = 0;
void startDice() { diceA = diceB = diceFinalA = diceFinalB = 1; diceRolling = false; diceRollStart = diceLastTick = diceRollCount = 0; }
void beginDiceRoll() { if (diceRolling) return; diceRolling = true; diceRollStart = millis(); diceLastTick = 0; diceFinalA = 1 + gameRandom(6); diceFinalB = 1 + gameRandom(6); }
void processDice() {
  if (takeLong(K_BACK)) { for (auto &k : keys) { k.pressedEvent = false; k.longEvent = false; } screen = SCR_GAMES; lastInteraction = millis(); return; }
  takePress(K_BACK); takePress(K_UP); takePress(K_DOWN);
  if (takeConfirmOrStart()) beginDiceRoll();
  if (diceRolling && millis() - diceLastTick >= 70UL) {
    diceLastTick = millis(); diceA = 1 + gameRandom(6); diceB = 1 + gameRandom(6);
    if (millis() - diceRollStart >= 600UL) { diceRolling = false; diceA = diceFinalA; diceB = diceFinalB; diceRollCount++; }
  }
}
void drawDie(int x, int y, int value) {
  display.drawRoundRect(x, y, 30, 28, 4, SSD1306_WHITE);
  const int px[7] = {x+7,x+22,x+7,x+15,x+22,x+7,x+22};
  const int py[7] = {y+6,y+6,y+14,y+14,y+14,y+22,y+22};
  bool pips[7] = {false,false,false,false,false,false,false};
  if (value % 2) pips[3] = true;
  if (value >= 2) { pips[0] = true; pips[6] = true; }
  if (value >= 4) { pips[1] = true; pips[5] = true; }
  if (value == 6) { pips[2] = true; pips[4] = true; }
  for (int i = 0; i < 7; ++i) if (pips[i]) display.fillCircle(px[i], py[i], 2, SSD1306_WHITE);
}
void drawDice() {
  display.clearDisplay(); drawHeader(); display.setTextSize(1); display.setTextColor(SSD1306_WHITE); display.setCursor(0, 11); display.print("DICE ROLLER");
  drawDie(28, 21, diceA); drawDie(70, 21, diceB);
  display.setCursor(0, 53); display.print("SUM:"); display.print(diceA + diceB); display.setCursor(42, 53); display.print(diceRolling ? "ROLLING" : "OK=ROLL"); display.setCursor(84, 53); display.print("#"); display.print(diceRollCount); display.display();
}

// ---------------- NUMBER GUESS ----------------
int guessTarget = 42, guessValue = 50, guessAttempts = 0, guessHint = 0;
bool guessWon = false;
void startGuess() { guessTarget = 1 + (int)gameRandom(99); guessValue = 50; guessAttempts = 0; guessHint = 0; guessWon = false; }
void submitGuess() {
  if (guessWon) { startGuess(); return; }
  guessAttempts++;
  if (guessValue == guessTarget) {
    guessWon = true; guessHint = 0; uint32_t best = prefs.getUInt("hsguess", 0);
    if (!best || (uint32_t)guessAttempts < best) prefs.putUInt("hsguess", (uint32_t)guessAttempts);
  } else guessHint = guessValue < guessTarget ? 1 : -1;
}
void processGuess() {
  if (takeLong(K_BACK)) { for (auto &k : keys) { k.pressedEvent = false; k.longEvent = false; } screen = SCR_GAMES; lastInteraction = millis(); return; }
  if (guessWon) { takePress(K_BACK); takePress(K_UP); takePress(K_DOWN); if (takeConfirmOrStart()) startGuess(); return; }
  if (takePress(K_UP)) guessValue = min(99, guessValue + 1);
  if (takePress(K_DOWN)) guessValue = max(1, guessValue - 1);
  if (takePress(K_BACK)) guessValue = max(1, guessValue - 10);
  if (takePress(K_START)) guessValue = min(99, guessValue + 10);
  if (takePress(K_CONFIRM)) submitGuess();
}
void drawGuess() {
  display.clearDisplay(); drawHeader(); display.setTextSize(1); display.setTextColor(SSD1306_WHITE); display.setCursor(0, 11); display.print("NUMBER GUESS 1-99");
  if (guessWon) {
    display.setCursor(0, 24); display.print("CORRECT! NUMBER: "); display.print(guessTarget);
    display.setCursor(0, 37); display.print("SOLVED IN "); display.print(guessAttempts); display.print(" TRIES");
    display.setCursor(0, 48); display.print("BEST: "); uint32_t best = prefs.getUInt("hsguess", 0); if (best) display.print(best); else display.print("--");
    display.setCursor(0, 56); display.print("OK/START NEW GAME");
  } else {
    display.setCursor(0, 23); display.print("YOUR GUESS"); display.setTextSize(2); display.setCursor(42, 30); if (guessValue < 10) display.print("0"); display.print(guessValue); display.setTextSize(1);
    display.setCursor(0, 47); display.print("TRIES: "); display.print(guessAttempts); display.setCursor(0, 56);
    if (guessHint > 0) display.print("TOO LOW: GO HIGHER"); else if (guessHint < 0) display.print("TOO HIGH: GO LOWER"); else display.print("UP/DN +/-1 B/S +/-10");
  }
  display.display();
}


// ---------------- LIGHTS OUT (SOLO LOGIC PUZZLE) ----------------
uint8_t lightsBoard[5][5];
int lightsX = 2, lightsY = 2, lightsMoves = 0;
bool lightsWon = false;
void toggleLightsCell(int x, int y) {
  if (x >= 0 && x < 5 && y >= 0 && y < 5) lightsBoard[y][x] ^= 1;
}
bool lightsAllOff() {
  for (int y = 0; y < 5; ++y) for (int x = 0; x < 5; ++x) if (lightsBoard[y][x]) return false;
  return true;
}
void startLightsOut() {
  memset(lightsBoard, 0, sizeof(lightsBoard)); lightsX = lightsY = 2; lightsMoves = 0; lightsWon = false;
  // Generate a guaranteed-solvable board by scrambling the solved board with legal moves.
  int scrambles = 10 + (int)gameRandom(9);
  for (int i = 0; i < scrambles; ++i) {
    int x = (int)gameRandom(5), y = (int)gameRandom(5);
    toggleLightsCell(x, y); toggleLightsCell(x - 1, y); toggleLightsCell(x + 1, y);
    toggleLightsCell(x, y - 1); toggleLightsCell(x, y + 1);
  }
  if (lightsAllOff()) { toggleLightsCell(2, 2); toggleLightsCell(1, 2); toggleLightsCell(3, 2); toggleLightsCell(2, 1); toggleLightsCell(2, 3); }
}
void pressLightsCell() {
  if (lightsWon) return;
  toggleLightsCell(lightsX, lightsY); toggleLightsCell(lightsX - 1, lightsY);
  toggleLightsCell(lightsX + 1, lightsY); toggleLightsCell(lightsX, lightsY - 1);
  toggleLightsCell(lightsX, lightsY + 1); lightsMoves++;
  lightsWon = lightsAllOff();
}
void processLightsOut() {
  if (takeLong(K_BACK)) { for (auto &k : keys) { k.pressedEvent = false; k.longEvent = false; } screen = SCR_GAMES; lastInteraction = millis(); return; }
  if (lightsWon) { takePress(K_BACK); takePress(K_UP); takePress(K_DOWN); if (takeConfirmOrStart()) startLightsOut(); return; }
  if (takePress(K_UP)) lightsY = (lightsY + 4) % 5;
  if (takePress(K_DOWN)) lightsY = (lightsY + 1) % 5;
  if (takePress(K_BACK)) lightsX = (lightsX + 4) % 5;
  if (takePress(K_START)) lightsX = (lightsX + 1) % 5;
  if (takePress(K_CONFIRM)) pressLightsCell();
}
void drawLightsOut() {
  display.clearDisplay(); drawHeader(); display.setTextSize(1); display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 11); display.print("LIGHTS OUT");
  display.setCursor(0, 23); display.print("MOVES:"); display.print(lightsMoves);
  display.setCursor(0, 34); display.print("GOAL: DARK");
  display.setCursor(0, 45); display.print(lightsWon ? "SOLVED!" : "OK TOGGLE");
  display.setCursor(0, 56); display.print(lightsWon ? "OK NEW GAME" : "HOLD B EXIT");
  const int ox = 72, oy = 20, cw = 10, ch = 7;
  for (int y = 0; y < 5; ++y) for (int x = 0; x < 5; ++x) {
    int px = ox + x * cw, py = oy + y * ch;
    if (lightsBoard[y][x]) display.fillRoundRect(px + 1, py + 1, 8, 6, 1, SSD1306_WHITE);
    else display.drawRoundRect(px + 1, py + 1, 8, 6, 1, SSD1306_WHITE);
  }
  display.drawRect(ox + lightsX * cw, oy + lightsY * ch, 10, 8, SSD1306_INVERSE);
  display.display();
}

// ---------------- MEMORY PAIRS (SOLO MEMORY GAME) ----------------
uint8_t pairValues[16], pairMatched[16], pairRevealed[16];
int pairX = 0, pairY = 0, pairFirst = -1, pairSecond = -1, pairFound = 0, pairMoves = 0;
bool pairHidePending = false, pairsWon = false;
uint32_t pairHideAt = 0;
void startMemoryPairs() {
  for (int i = 0; i < 16; ++i) { pairValues[i] = (uint8_t)(i % 8); pairMatched[i] = 0; pairRevealed[i] = 0; }
  for (int i = 15; i > 0; --i) { int j = (int)gameRandom((uint32_t)i + 1); uint8_t t = pairValues[i]; pairValues[i] = pairValues[j]; pairValues[j] = t; }
  pairX = pairY = 0; pairFirst = pairSecond = -1; pairFound = pairMoves = 0;
  pairHidePending = false; pairsWon = false; pairHideAt = 0;
}
void processMemoryPairs() {
  if (takeLong(K_BACK)) { for (auto &k : keys) { k.pressedEvent = false; k.longEvent = false; } screen = SCR_GAMES; lastInteraction = millis(); return; }
  if (pairHidePending && (int32_t)(millis() - pairHideAt) >= 0) {
    if (pairFirst >= 0 && !pairMatched[pairFirst]) pairRevealed[pairFirst] = 0;
    if (pairSecond >= 0 && !pairMatched[pairSecond]) pairRevealed[pairSecond] = 0;
    pairFirst = pairSecond = -1; pairHidePending = false;
  }
  if (pairHidePending) { takePress(K_UP); takePress(K_DOWN); takePress(K_BACK); takePress(K_START); takePress(K_CONFIRM); return; }
  if (pairsWon) { takePress(K_BACK); takePress(K_UP); takePress(K_DOWN); if (takeConfirmOrStart()) startMemoryPairs(); return; }
  if (takePress(K_UP)) pairY = (pairY + 3) % 4;
  if (takePress(K_DOWN)) pairY = (pairY + 1) % 4;
  if (takePress(K_BACK)) pairX = (pairX + 3) % 4;
  if (takePress(K_START)) pairX = (pairX + 1) % 4;
  if (takePress(K_CONFIRM)) {
    int idx = pairY * 4 + pairX;
    if (pairMatched[idx] || pairRevealed[idx]) return;
    pairRevealed[idx] = 1;
    if (pairFirst < 0) pairFirst = idx;
    else {
      pairSecond = idx; pairMoves++;
      if (pairValues[pairFirst] == pairValues[pairSecond]) {
        pairMatched[pairFirst] = pairMatched[pairSecond] = 1; pairFound++;
        pairFirst = pairSecond = -1;
        if (pairFound >= 8) pairsWon = true;
      } else { pairHidePending = true; pairHideAt = millis() + 750UL; }
    }
  }
}
void drawMemoryPairs() {
  display.clearDisplay(); drawHeader(); display.setTextSize(1); display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 11); display.print("MEMORY PAIRS");
  display.setCursor(0, 23); display.print("PAIRS:"); display.print(pairFound); display.print("/8");
  display.setCursor(0, 34); display.print("MOVES:"); display.print(pairMoves);
  display.setCursor(0, 45); display.print(pairsWon ? "ALL MATCHED!" : "OK REVEAL");
  display.setCursor(0, 56); display.print(pairsWon ? "OK NEW GAME" : "HOLD B EXIT");
  const int ox = 73, oy = 20, cw = 12, ch = 8;
  for (int i = 0; i < 16; ++i) {
    int x = ox + (i % 4) * cw, y = oy + (i / 4) * ch;
    bool faceUp = pairRevealed[i] || pairMatched[i];
    if (pairMatched[i]) display.fillRoundRect(x + 1, y + 1, 10, 7, 1, SSD1306_WHITE);
    else display.drawRoundRect(x + 1, y + 1, 10, 7, 1, SSD1306_WHITE);
    if (faceUp) {
      display.setTextColor(pairMatched[i] ? SSD1306_BLACK : SSD1306_WHITE);
      display.setCursor(x + 3, y + 1); display.print((int)pairValues[i] + 1);
      display.setTextColor(SSD1306_WHITE);
    } else display.drawPixel(x + 6, y + 5, SSD1306_WHITE);
  }
  int cx = ox + pairX * cw, cy = oy + pairY * ch;
  display.drawRect(cx, cy, cw, ch, SSD1306_INVERSE);
  display.display();
}

void processCalc() {
  if (takeLong(K_BACK)) { for (auto &k : keys) { k.pressedEvent = false; k.longEvent = false; } screen = SCR_MENU; lastInteraction = millis(); return; }
  if (takePress(K_UP)) calcCursor = (calcCursor + 15) % 20;
  if (takePress(K_DOWN)) calcCursor = (calcCursor + 5) % 20;
  if (takePress(K_BACK)) calcCursor = (calcCursor / 5) * 5 + ((calcCursor + 4) % 5);
  if (takePress(K_START)) calcCursor = (calcCursor / 5) * 5 + ((calcCursor + 1) % 5);
  if (takePress(K_CONFIRM)) selectCalcKey();
}

void processSimpleScreen() {
  if (screen == SCR_SCORES) {
    if (takePress(K_UP)) scoresPage = (scoresPage + 3) % 4;
    if (takePress(K_DOWN)) scoresPage = (scoresPage + 1) % 4;
    bool c = takePress(K_CONFIRM), b = takePress(K_BACK), st = takePress(K_START);
    if (c || b || st) { screen = SCR_MENU; lastInteraction = millis(); }
    return;
  }
  bool confirmPressed = takePress(K_CONFIRM);
  bool backPressed = takePress(K_BACK);
  bool startPressed = takePress(K_START);
  if (screen == SCR_WIFI_SCAN && confirmPressed && !backPressed && !startPressed) { doScan(); return; }
  if (backPressed || startPressed || confirmPressed) { screen = SCR_MENU; lastInteraction = millis(); }
}

void renderScreen() {
  switch (screen) {
    case SCR_MENU: drawMenuList(mainItems, MAIN_COUNT, menuIndex, "MAIN MENU"); break;
    case SCR_CLOCK: drawClock(); break;
    case SCR_WIFI_SCAN: drawWifiScan(); break;
    case SCR_GAMES: drawMenuList(gameItems, GAME_COUNT, gameMenuIndex, "GAME CENTER"); break;
    case SCR_CALC: drawCalc(); break;
    case SCR_STOPWATCH: drawStopwatch(); break;
    case SCR_SETTINGS: drawSettings(); break;
    case SCR_NETWORK: drawNetwork(); break;
    case SCR_SCORES: drawScores(); break;
    case SCR_ABOUT: drawAbout(); break;
    case SCR_BLOCK: drawBlockGame(); break;
    case SCR_BREAKER: drawBreaker(); break;
    case SCR_ONELINE: drawOneLine(); break;
    case SCR_TTT: drawTtt(); break;
    case SCR_SNAKE: drawSnake(); break;
    case SCR_PONG: drawPong(); break;
    case SCR_JUMPER: drawJumper(); break;
    case SCR_REACTION: drawReaction(); break;
    case SCR_SIMON: drawSimon(); break;
    case SCR_DODGE: drawDodge(); break;
    case SCR_ROULETTE: drawRoulette(); break;
    case SCR_COIN: drawCoin(); break;
    case SCR_MINES: drawMines(); break;
    case SCR_2048: draw2048(); break;
    case SCR_RPS: drawRps(); break;
    case SCR_DICE: drawDice(); break;
    case SCR_GUESS: drawGuess(); break;
    case SCR_LIGHTS: drawLightsOut(); break;
    case SCR_PAIRS: drawMemoryPairs(); break;
    case SCR_CLOCK_EDIT: drawClockEditor(); break;
  }
}

void setup() {
  // All five buttons are wired active-low to GND and have internal pull-ups enabled.
  // Release Back (D3/GPIO0) and Start (D4/GPIO2) while booting/resetting/flashing.
  randomSeed(ESP.getChipId() ^ micros() ^ analogRead(A0));
  for (auto &k : keys) pinMode(k.pin, INPUT_PULLUP);
  Wire.begin(PIN_SDA, PIN_SCL);
#if defined(DEVLIN_OLED_SH1106)
  oledOk = display.begin(0x3C, true);
  if (!oledOk) oledOk = display.begin(0x3D, true);
#else
  oledOk = display.begin(SSD1306_SWITCHCAPVCC, 0x3C);
  if (!oledOk) oledOk = display.begin(SSD1306_SWITCHCAPVCC, 0x3D);
#endif
  loadSettings();
  setenv("TZ", "WIB-7", 1); tzset();
  uint32_t savedEpoch = prefs.getUInt("clockep", 0);
  if (savedEpoch > 1700000000UL && savedEpoch < 2145916800UL) setTimeFromEpoch((long)savedEpoch);
  if (oledOk) bootAnimation();
  startWifi();
  setupWeb();
  requestTimeSync();
  lastInteraction = millis();
  startBlockGame();
  startBreaker();
  startOneLine();
  startTtt();
  screen = SCR_MENU;
}

void loop() {
  server.handleClient();
  updateKeys();
  const bool holdBackScreen = screen == SCR_CALC || screen == SCR_BLOCK || screen == SCR_BREAKER || screen == SCR_ONELINE || screen == SCR_TTT || screen == SCR_SNAKE || screen == SCR_PONG || screen == SCR_JUMPER || screen == SCR_REACTION || screen == SCR_SIMON || screen == SCR_DODGE || screen == SCR_ROULETTE || screen == SCR_COIN || screen == SCR_MINES || screen == SCR_2048 || screen == SCR_RPS || screen == SCR_DICE || screen == SCR_GUESS || screen == SCR_LIGHTS || screen == SCR_PAIRS || screen == SCR_CLOCK_EDIT;
  if (!holdBackScreen) for (auto &k : keys) k.longEvent = false;
  updateBattery();
  prefs.tick();
  requestTimeSync();
  checkpointClock();
  if (screen == SCR_CLOCK) {
    if (anyPressPending()) {
      for (auto &k : keys) { k.pressedEvent = false; k.longEvent = false; }
      screen = SCR_MENU; menuIndex = 0; lastInteraction = millis();
    }
  } else {
    switch (screen) {
      case SCR_MENU: processMenu(); break;
      case SCR_GAMES: processGameMenu(); break;
      case SCR_BLOCK: processBlock(); break;
      case SCR_BREAKER: processBreaker(); break;
      case SCR_ONELINE: processOneLine(); break;
      case SCR_TTT: processTtt(); break;
      case SCR_SNAKE: processSnake(); break;
      case SCR_PONG: processPong(); break;
      case SCR_JUMPER: processJumper(); break;
      case SCR_REACTION: processReaction(); break;
      case SCR_SIMON: processSimon(); break;
      case SCR_DODGE: processDodge(); break;
      case SCR_ROULETTE: processRoulette(); break;
      case SCR_COIN: processCoin(); break;
      case SCR_MINES: processMines(); break;
      case SCR_2048: process2048(); break;
      case SCR_RPS: processRps(); break;
      case SCR_DICE: processDice(); break;
      case SCR_GUESS: processGuess(); break;
      case SCR_LIGHTS: processLightsOut(); break;
      case SCR_PAIRS: processMemoryPairs(); break;
      case SCR_CLOCK_EDIT: processClockEditor(); break;
      case SCR_CALC: processCalc(); break;
      case SCR_STOPWATCH: processStopwatch(); break;
      case SCR_SETTINGS: processSettings(); break;
      case SCR_WIFI_SCAN: case SCR_NETWORK: case SCR_SCORES: case SCR_ABOUT: processSimpleScreen(); break;
      default: break;
    }
  }
  if (screen == SCR_BREAKER) tickBreaker();
  if (screen == SCR_SNAKE) tickSnake();
  if (screen == SCR_PONG) tickPong();
  if (screen == SCR_JUMPER) tickJumper();
  if (screen == SCR_SIMON) tickSimon();
  if (screen == SCR_DODGE) tickDodge();
  if (oledOk && millis() - lastRender > 100) { lastRender = millis(); renderScreen(); }
  delay(2);
}
