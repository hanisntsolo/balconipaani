#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <DNSServer.h>
#include <EEPROM.h>
#include <time.h>

constexpr uint8_t RELAY_PIN = D2;          // LOW-trigger relay input
constexpr uint16_t EEPROM_BYTES = 1024;
constexpr uint32_t CONFIG_MAGIC = 0xBADA2401;
constexpr uint16_t WIFI_CONNECT_TIMEOUT_MS = 20000;
constexpr uint16_t STATUS_POLL_MS = 500;
constexpr char AP_SSID[] = "BalconiPaani-Setup";
constexpr char AP_PASSWORD[] = "balconi123";

ESP8266WebServer server(80);
DNSServer dnsServer;

struct DeviceConfig {
  uint32_t magic;
  char ssid[33];
  char password[65];
  bool autoMode;
  bool scheduleEnabled;
  uint8_t scheduleHour;
  uint8_t scheduleMinute;
  uint16_t scheduleDurationSec;
  uint16_t maxRuntimeSec;
  int16_t timezoneOffsetMinutes;
  uint8_t checksum;
};

static_assert(sizeof(DeviceConfig) < EEPROM_BYTES, "Config too large for EEPROM");

struct RuntimeState {
  bool valveOn;
  bool startedByScheduler;
  uint32_t valveOnSinceMs;
  int32_t lastScheduleDayKey;
  uint32_t lastStatusTickMs;
};

DeviceConfig config;
RuntimeState runtime{false, false, 0, -1, 0};
bool apMode = false;

const char INDEX_HTML[] PROGMEM = R"HTML(
<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>BalconiPaani Controller</title>
<style>
:root{--bg:#0f172a;--card:#111827;--ok:#22c55e;--warn:#f59e0b;--bad:#ef4444;--text:#e5e7eb}
*{box-sizing:border-box}body{margin:0;font-family:system-ui,-apple-system,Segoe UI,Roboto;background:var(--bg);color:var(--text)}
.wrap{max-width:720px;margin:auto;padding:14px}.card{background:var(--card);padding:14px;border-radius:12px;margin:10px 0}
h1{font-size:1.2rem;margin:0 0 10px}h2{font-size:1rem;margin:0 0 10px}.row{display:flex;gap:8px;flex-wrap:wrap}
button,input{border-radius:10px;border:1px solid #334155;padding:10px;font-size:1rem}
button{cursor:pointer;background:#1f2937;color:#fff;min-width:110px}
button.ok{background:var(--ok);color:#062b11}button.bad{background:var(--bad)}button.warn{background:var(--warn);color:#3b2201}
label{display:block;font-size:.85rem;margin:8px 0 4px}.grid{display:grid;grid-template-columns:1fr 1fr;gap:8px}
@media (max-width:560px){.grid{grid-template-columns:1fr}}
.status{font-size:.95rem;line-height:1.5}.pill{display:inline-block;padding:3px 8px;border-radius:999px;background:#1e293b;margin-right:6px}
small{opacity:.75}
</style>
</head>
<body>
<div class="wrap">
  <div class="card">
    <h1>BalconiPaani MVP2</h1>
    <div id="status" class="status">Loading…</div>
    <small>Manual OFF always works. Valve has hard runtime cap.</small>
  </div>

  <div class="card">
    <h2>Mode</h2>
    <div class="row">
      <button onclick="setMode('manual')" class="warn">MANUAL</button>
      <button onclick="setMode('auto')" class="ok">AUTO</button>
    </div>
  </div>

  <div class="card">
    <h2>Manual Valve Control</h2>
    <div class="row">
      <button onclick="valve('on')" class="ok">Valve ON</button>
      <button onclick="valve('off')" class="bad">Valve OFF</button>
    </div>
  </div>

  <div class="card">
    <h2>Scheduler & Safety</h2>
    <div class="grid">
      <div><label>Enabled (0/1)</label><input id="schEnabled" type="number" min="0" max="1"></div>
      <div><label>Start (HH:MM)</label><input id="schTime" type="time"></div>
      <div><label>Water Duration (sec)</label><input id="schDuration" type="number" min="5" max="3600"></div>
      <div><label>Max Runtime Cap (sec)</label><input id="maxRuntime" type="number" min="10" max="7200"></div>
      <div><label>Timezone Offset (min)</label><input id="tzOffset" type="number" min="-720" max="840"></div>
    </div>
    <div class="row" style="margin-top:10px">
      <button onclick="saveSchedule()">Save Schedule</button>
      <button onclick="refresh()">Refresh</button>
    </div>
  </div>

  <div class="card">
    <h2>WiFi Onboarding</h2>
    <label>SSID</label><input id="ssid" type="text" maxlength="32" style="width:100%">
    <label>Password</label><input id="password" type="password" maxlength="64" style="width:100%">
    <div class="row" style="margin-top:10px">
      <button onclick="saveWifi()">Save WiFi & Reboot</button>
      <button onclick="reboot()">Reboot</button>
    </div>
  </div>
</div>
<script>
async function req(path, body){
  const res = await fetch(path,{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:new URLSearchParams(body||{})});
  if(!res.ok) throw new Error(await res.text());
  return await res.json();
}
function two(n){return String(n).padStart(2,'0')}
function draw(s){
  document.getElementById('status').innerHTML =
    `<span class="pill">Mode: ${s.autoMode?'AUTO':'MANUAL'}</span>`+
    `<span class="pill">Valve: ${s.valveOn?'ON':'OFF'}</span>`+
    `<span class="pill">WiFi: ${s.wifiConnected?'STA':'AP'}</span><br>`+
    `IP: ${s.ip} | Uptime: ${s.uptimeSec}s<br>`+
    `Now: ${s.localTime} | Next schedule: ${two(s.scheduleHour)}:${two(s.scheduleMinute)} (${s.scheduleEnabled?'ENABLED':'DISABLED'})`;
  document.getElementById('schEnabled').value = s.scheduleEnabled?1:0;
  document.getElementById('schTime').value = `${two(s.scheduleHour)}:${two(s.scheduleMinute)}`;
  document.getElementById('schDuration').value = s.scheduleDurationSec;
  document.getElementById('maxRuntime').value = s.maxRuntimeSec;
  document.getElementById('tzOffset').value = s.timezoneOffsetMinutes;
}
async function refresh(){
  const s = await (await fetch('/api/status')).json();
  draw(s);
}
async function valve(state){ await req('/api/valve/'+state,{}); await refresh(); }
async function setMode(mode){ await req('/api/mode',{mode}); await refresh(); }
async function saveSchedule(){
  const t = document.getElementById('schTime').value || '06:00';
  const [hour, minute] = t.split(':');
  await req('/api/schedule',{
    enabled: document.getElementById('schEnabled').value,
    hour, minute,
    duration: document.getElementById('schDuration').value,
    maxRuntime: document.getElementById('maxRuntime').value,
    tzOffset: document.getElementById('tzOffset').value,
  });
  await refresh();
}
async function saveWifi(){
  await req('/api/wifi',{
    ssid: document.getElementById('ssid').value,
    password: document.getElementById('password').value,
  });
  alert('WiFi credentials saved. Device rebooting.');
}
async function reboot(){ await req('/api/reboot',{}); }
setInterval(refresh, 3000); refresh();
</script>
</body>
</html>
)HTML";

uint8_t configChecksum(const DeviceConfig &cfg) {
  const auto *raw = reinterpret_cast<const uint8_t *>(&cfg);
  uint8_t sum = 0;
  for (size_t i = 0; i < sizeof(DeviceConfig) - 1; i++) {
    sum ^= raw[i];
  }
  return sum;
}

void setDefaultConfig() {
  memset(&config, 0, sizeof(config));
  config.magic = CONFIG_MAGIC;
  config.autoMode = false;                 // fail-safe startup in MANUAL mode
  config.scheduleEnabled = false;
  config.scheduleHour = 6;
  config.scheduleMinute = 0;
  config.scheduleDurationSec = 60;
  config.maxRuntimeSec = 300;
  config.timezoneOffsetMinutes = 330;      // IST default; editable in UI
}

void persistConfig() {
  config.magic = CONFIG_MAGIC;
  config.checksum = 0;
  config.checksum = configChecksum(config);
  EEPROM.put(0, config);
  EEPROM.commit();
}

void loadConfig() {
  EEPROM.get(0, config);
  if (config.magic != CONFIG_MAGIC) {
    setDefaultConfig();
    persistConfig();
    return;
  }
  uint8_t saved = config.checksum;
  config.checksum = 0;
  uint8_t calc = configChecksum(config);
  config.checksum = saved;
  if (saved != calc) {
    setDefaultConfig();
    persistConfig();
  }
}

void valveHardwareOff() {
  digitalWrite(RELAY_PIN, HIGH);  // LOW-trigger relay: HIGH=OFF
}

void valveHardwareOn() {
  digitalWrite(RELAY_PIN, LOW);   // LOW-trigger relay: LOW=ON
}

void setValve(bool on, bool startedByScheduler, const char *reason) {
  if (on) {
    if (!runtime.valveOn) {
      valveHardwareOn();
      runtime.valveOn = true;
      runtime.startedByScheduler = startedByScheduler;
      runtime.valveOnSinceMs = millis();
      Serial.printf("VALVE ON (%s)\n", reason);
    }
    return;
  }

  if (runtime.valveOn) {
    valveHardwareOff();
    runtime.valveOn = false;
    runtime.startedByScheduler = false;
    Serial.printf("VALVE OFF (%s)\n", reason);
  }
}

bool elapsedMs(uint32_t since, uint32_t interval) {
  return static_cast<uint32_t>(millis() - since) >= interval;
}

String localTimeText() {
  time_t now = time(nullptr);
  if (now < 1700000000) {
    return String("unsynced");
  }
  struct tm tmNow;
  localtime_r(&now, &tmNow);
  char buf[24];
  strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tmNow);
  return String(buf);
}

bool connectStation() {
  if (strlen(config.ssid) == 0) {
    return false;
  }

  WiFi.mode(WIFI_STA);
  WiFi.hostname("balconipaani");
  WiFi.begin(config.ssid, config.password);
  Serial.printf("Connecting to WiFi SSID '%s'...\n", config.ssid);

  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && (millis() - start) < WIFI_CONNECT_TIMEOUT_MS) {
    delay(250);
    yield();
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("WiFi connected. IP=%s\n", WiFi.localIP().toString().c_str());
    return true;
  }

  Serial.println("WiFi connect timeout. Falling back to AP mode.");
  return false;
}

void startAccessPointMode() {
  apMode = true;
  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASSWORD);
  dnsServer.start(53, "*", WiFi.softAPIP());
  Serial.printf("AP mode active: SSID=%s IP=%s\n", AP_SSID, WiFi.softAPIP().toString().c_str());
}

void startTimeSync() {
  configTime(config.timezoneOffsetMinutes * 60, 0, "pool.ntp.org", "time.nist.gov", "time.google.com");
}

String ipAddressForUi() {
  return apMode ? WiFi.softAPIP().toString() : WiFi.localIP().toString();
}

void sendStatusJson() {
  String json = "{";
  json += "\"autoMode\":" + String(config.autoMode ? "true" : "false");
  json += ",\"valveOn\":" + String(runtime.valveOn ? "true" : "false");
  json += ",\"scheduleEnabled\":" + String(config.scheduleEnabled ? "true" : "false");
  json += ",\"scheduleHour\":" + String(config.scheduleHour);
  json += ",\"scheduleMinute\":" + String(config.scheduleMinute);
  json += ",\"scheduleDurationSec\":" + String(config.scheduleDurationSec);
  json += ",\"maxRuntimeSec\":" + String(config.maxRuntimeSec);
  json += ",\"timezoneOffsetMinutes\":" + String(config.timezoneOffsetMinutes);
  json += ",\"wifiConnected\":" + String(!apMode && WiFi.status() == WL_CONNECTED ? "true" : "false");
  json += ",\"ip\":\"" + ipAddressForUi() + "\"";
  json += ",\"uptimeSec\":" + String(millis() / 1000);
  json += ",\"localTime\":\"" + localTimeText() + "\"";
  json += "}";
  server.send(200, "application/json", json);
}

bool parseIntArg(const String &name, int &out) {
  if (!server.hasArg(name)) return false;
  out = server.arg(name).toInt();
  return true;
}

void handleModeSet() {
  if (!server.hasArg("mode")) {
    server.send(400, "text/plain", "Missing mode");
    return;
  }
  String mode = server.arg("mode");
  mode.toLowerCase();
  if (mode == "auto") {
    config.autoMode = true;
  } else if (mode == "manual") {
    config.autoMode = false;
  } else {
    server.send(400, "text/plain", "Invalid mode");
    return;
  }
  persistConfig();
  sendStatusJson();
}

void handleScheduleSet() {
  int enabled, hour, minute, duration, maxRuntime, tzOffset;
  if (!parseIntArg("enabled", enabled) ||
      !parseIntArg("hour", hour) ||
      !parseIntArg("minute", minute) ||
      !parseIntArg("duration", duration) ||
      !parseIntArg("maxRuntime", maxRuntime) ||
      !parseIntArg("tzOffset", tzOffset)) {
    server.send(400, "text/plain", "Missing schedule args");
    return;
  }

  if (hour < 0 || hour > 23 || minute < 0 || minute > 59 ||
      duration < 5 || duration > 3600 ||
      maxRuntime < 10 || maxRuntime > 7200 ||
      tzOffset < -720 || tzOffset > 840 ||
      duration > maxRuntime) {
    server.send(400, "text/plain", "Invalid schedule args");
    return;
  }

  config.scheduleEnabled = enabled > 0;
  config.scheduleHour = static_cast<uint8_t>(hour);
  config.scheduleMinute = static_cast<uint8_t>(minute);
  config.scheduleDurationSec = static_cast<uint16_t>(duration);
  config.maxRuntimeSec = static_cast<uint16_t>(maxRuntime);
  config.timezoneOffsetMinutes = static_cast<int16_t>(tzOffset);
  persistConfig();
  startTimeSync();
  sendStatusJson();
}

void handleWifiSet() {
  if (!server.hasArg("ssid") || !server.hasArg("password")) {
    server.send(400, "text/plain", "Missing wifi args");
    return;
  }

  String ssid = server.arg("ssid");
  String password = server.arg("password");
  if (ssid.length() < 1 || ssid.length() > 32 || password.length() > 64) {
    server.send(400, "text/plain", "Invalid wifi args");
    return;
  }

  memset(config.ssid, 0, sizeof(config.ssid));
  memset(config.password, 0, sizeof(config.password));
  ssid.toCharArray(config.ssid, sizeof(config.ssid));
  password.toCharArray(config.password, sizeof(config.password));

  persistConfig();
  sendStatusJson();
  delay(500);
  ESP.restart();
}

void handleReboot() {
  sendStatusJson();
  delay(500);
  ESP.restart();
}

void setupRoutes() {
  server.on("/", HTTP_GET, []() {
    server.send_P(200, "text/html", INDEX_HTML);
  });
  server.on("/api/status", HTTP_GET, sendStatusJson);
  server.on("/api/valve/on", HTTP_POST, []() {
    setValve(true, false, "manual_web");
    sendStatusJson();
  });
  server.on("/api/valve/off", HTTP_POST, []() {
    setValve(false, false, "manual_web");
    sendStatusJson();
  });
  server.on("/api/mode", HTTP_POST, handleModeSet);
  server.on("/api/schedule", HTTP_POST, handleScheduleSet);
  server.on("/api/wifi", HTTP_POST, handleWifiSet);
  server.on("/api/reboot", HTTP_POST, handleReboot);
  server.onNotFound([]() {
    if (apMode) {
      server.sendHeader("Location", String("http://") + WiFi.softAPIP().toString() + "/", true);
      server.send(302, "text/plain", "");
      return;
    }
    server.send(404, "text/plain", "Not found");
  });
}

void runSchedulerAndSafety() {
  if (!elapsedMs(runtime.lastStatusTickMs, STATUS_POLL_MS)) {
    return;
  }
  runtime.lastStatusTickMs = millis();

  if (runtime.valveOn && elapsedMs(runtime.valveOnSinceMs, static_cast<uint32_t>(config.maxRuntimeSec) * 1000UL)) {
    setValve(false, false, "max_runtime_timeout");
  }

  if (runtime.valveOn && runtime.startedByScheduler &&
      elapsedMs(runtime.valveOnSinceMs, static_cast<uint32_t>(config.scheduleDurationSec) * 1000UL)) {
    setValve(false, false, "schedule_duration_complete");
  }

  if (!config.autoMode || !config.scheduleEnabled || runtime.valveOn) {
    return;
  }

  time_t now = time(nullptr);
  if (now < 1700000000) {
    return;
  }

  struct tm tmNow;
  localtime_r(&now, &tmNow);
  int32_t dayKey = (tmNow.tm_year * 1000) + tmNow.tm_yday;
  if (tmNow.tm_hour == config.scheduleHour &&
      tmNow.tm_min == config.scheduleMinute &&
      tmNow.tm_sec < 5 &&
      dayKey != runtime.lastScheduleDayKey) {
    runtime.lastScheduleDayKey = dayKey;
    setValve(true, true, "daily_schedule");
  }
}

void setup() {
  pinMode(RELAY_PIN, OUTPUT);
  valveHardwareOff();            // SAFETY: keep valve OFF during boot

  Serial.begin(115200);
  delay(10);
  Serial.println();
  Serial.println("BalconiPaani MVP2 boot");

  EEPROM.begin(EEPROM_BYTES);
  loadConfig();

  if (!connectStation()) {
    startAccessPointMode();
  }
  startTimeSync();

  setupRoutes();
  server.begin();
  Serial.println("HTTP server started");
}

void loop() {
  server.handleClient();
  if (apMode) {
    dnsServer.processNextRequest();
  }
  runSchedulerAndSafety();
}
