// BalconiPaani MVP2.1 — ESP8266 Irrigation Controller
// Production firmware: local-only, zero cloud dependency.
//
// Hardware:
//   NodeMCU ESP8266 + LOW-trigger relay on D2 → 24V solenoid valve.
//   Shared GND between ESP USB supply and relay 24V supply.

#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ESP8266mDNS.h>
#include <DNSServer.h>
#include <EEPROM.h>
#include <time.h>

// ── Firmware identity ──────────────────────────────────────────────────────
constexpr char     FIRMWARE_VERSION[]         = "MVP2.1";

// ── Hardware ───────────────────────────────────────────────────────────────
constexpr uint8_t  RELAY_PIN                  = D2;   // LOW-trigger relay

// ── EEPROM ─────────────────────────────────────────────────────────────────
constexpr uint16_t EEPROM_BYTES               = 512;
constexpr uint32_t CONFIG_MAGIC               = 0xBADA2402; // bump on struct change
constexpr uint8_t  CONFIG_VERSION             = 1;

// ── Timing ─────────────────────────────────────────────────────────────────
constexpr uint16_t WIFI_CONNECT_TIMEOUT_MS    = 20000;
constexpr uint32_t WIFI_RECONNECT_INTERVAL_MS = 60000UL; // retry lost connection
constexpr uint16_t SCHEDULER_TICK_MS          = 500;
constexpr uint8_t  SCHEDULE_WINDOW_SEC        = 10;     // fire window per trigger

// ── AP fallback ────────────────────────────────────────────────────────────
constexpr char AP_SSID[]     = "BalconiPaani-Setup";
constexpr char AP_PASSWORD[] = "balconi123";

ESP8266WebServer server(80);
DNSServer        dnsServer;

// ── Persistent config (EEPROM-backed) ─────────────────────────────────────
struct DeviceConfig {
  uint32_t magic;
  uint8_t  configVersion;
  char     ssid[33];
  char     password[65];
  bool     autoMode;
  bool     scheduleEnabled;
  uint8_t  scheduleHour;
  uint8_t  scheduleMinute;
  uint16_t scheduleDurationSec;
  uint16_t maxRuntimeSec;
  int16_t  timezoneOffsetMinutes;
  uint8_t  checksum;             // XOR over all bytes before this field
} __attribute__((packed));       // no compiler padding — EEPROM layout must be exact

static_assert(sizeof(DeviceConfig) < EEPROM_BYTES, "Config too large for EEPROM");

// ── Volatile runtime state (lost on power cycle) ───────────────────────────
struct RuntimeState {
  bool     valveOn;
  bool     startedByScheduler;
  uint32_t valveOnSinceMs;
  int32_t  lastScheduleDayKey;
  uint32_t lastTickMs;
  uint32_t lastWifiCheckMs;
  uint32_t lastValveOffMs;   // 0 = not used this session
  bool     ntpSynced;
  bool     pendingReboot;    // defers ESP.restart() out of HTTP handler
};

DeviceConfig config;
RuntimeState runtime{};
bool apMode = false;

const char INDEX_HTML[] PROGMEM = R"HTML(
<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>BalconiPaani</title>
<style>
:root{--bg:#0f172a;--card:#111827;--ok:#22c55e;--warn:#f59e0b;--bad:#ef4444;--text:#e5e7eb;--dim:#94a3b8}
*{box-sizing:border-box}
body{margin:0;font-family:system-ui,-apple-system,Segoe UI,Roboto,sans-serif;background:var(--bg);color:var(--text)}
.wrap{max-width:720px;margin:auto;padding:12px}
.card{background:var(--card);padding:14px;border-radius:14px;margin:10px 0}
h1{font-size:1.15rem;margin:0 0 10px}
h2{font-size:.9rem;margin:0 0 10px;color:var(--dim);text-transform:uppercase;letter-spacing:.05em}
.row{display:flex;gap:8px;flex-wrap:wrap;margin-top:8px}
button{cursor:pointer;background:#1e293b;color:#fff;border:1px solid #334155;border-radius:10px;padding:10px 18px;font-size:.95rem;min-width:110px;transition:opacity .15s}
button:active{opacity:.7}
button.ok{background:var(--ok);color:#052e16;border-color:var(--ok)}
button.bad{background:var(--bad);border-color:var(--bad)}
button.warn{background:var(--warn);color:#3b2201;border-color:var(--warn)}
input,select{border-radius:8px;border:1px solid #334155;padding:9px;font-size:.9rem;background:#0f172a;color:var(--text);width:100%}
label{display:block;font-size:.78rem;color:var(--dim);margin:8px 0 3px}
.grid{display:grid;grid-template-columns:1fr 1fr;gap:8px}
@media(max-width:560px){.grid{grid-template-columns:1fr}}
.pill{display:inline-block;padding:2px 9px;border-radius:999px;font-size:.8rem;background:#1e293b;margin:2px 3px 2px 0;vertical-align:middle}
.pill.ok{background:var(--ok);color:#052e16}
.pill.bad{background:var(--bad)}
.pill.warn{background:var(--warn);color:#3b2201}
.dim{color:var(--dim);font-size:.82rem}
#rtbar{height:5px;border-radius:3px;background:#1e293b;overflow:hidden;margin-top:8px;display:none}
#rtfill{height:100%;background:var(--ok);width:0%;transition:width .5s}
</style>
</head>
<body>
<div class="wrap">

  <div class="card">
    <h1>💧 BalconiPaani <span class="dim" id="ver" style="font-weight:400;font-size:.85rem"></span></h1>
    <div id="sb">Loading…</div>
    <div id="rtbar"><div id="rtfill"></div></div>
    <div class="dim" style="margin-top:8px">Manual OFF always works · Hard runtime cap enforced</div>
  </div>

  <div class="card">
    <h2>Mode</h2>
    <div class="row">
      <button class="warn" onclick="setMode('manual')">MANUAL</button>
      <button class="ok"   onclick="setMode('auto')">AUTO</button>
    </div>
  </div>

  <div class="card">
    <h2>Manual Valve</h2>
    <div class="row">
      <button class="ok"  onclick="valve('on')">Valve ON</button>
      <button class="bad" onclick="valve('off')">Valve OFF</button>
    </div>
  </div>

  <div class="card">
    <h2>Scheduler &amp; Safety</h2>
    <div class="grid">
      <div>
        <label>Schedule</label>
        <select id="schEnabled">
          <option value="0">Disabled</option>
          <option value="1">Enabled</option>
        </select>
      </div>
      <div><label>Start Time (HH:MM)</label><input id="schTime" type="time"></div>
      <div><label>Water Duration (sec)</label><input id="schDuration" type="number" min="5" max="3600"></div>
      <div><label>Max Runtime Cap (sec)</label><input id="maxRuntime" type="number" min="10" max="7200"></div>
      <div><label>Timezone Offset (min, e.g. IST=330)</label><input id="tzOffset" type="number" min="-720" max="840"></div>
    </div>
    <div class="row">
      <button onclick="saveSchedule()">Save Schedule</button>
      <button onclick="refresh()">Refresh</button>
    </div>
  </div>

  <div class="card" id="wifi-card">
    <h2>WiFi</h2>
    <div id="wifi-conn-row"></div>
    <div id="wifi-form">
      <label>SSID</label><input id="ssid" type="text" maxlength="32" autocomplete="off">
      <label>Password</label><input id="wfPass" type="password" maxlength="64" autocomplete="new-password">
      <div class="row">
        <button onclick="saveWifi()">Save &amp; Reboot</button>
      </div>
    </div>
    <div class="row" style="margin-top:8px">
      <button onclick="doReboot()">Reboot</button>
    </div>
  </div>

</div>
<script>
const two = n => String(n).padStart(2,'0');
const esc = s => String(s).replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/>/g,'&gt;');

function draw(s) {
  document.getElementById('ver').textContent = s.version || '';

  const mode = s.autoMode
    ? '<span class="pill ok">AUTO</span>'
    : '<span class="pill warn">MANUAL</span>';
  const valve = s.valveOn
    ? '<span class="pill ok">Valve ON</span>'
    : '<span class="pill">Valve OFF</span>';
  const net = s.wifiConnected
    ? `<span class="pill ok">STA</span>`
    : '<span class="pill warn">AP</span>';
  const ntp = s.ntpSynced ? '' : '<span class="pill warn">NTP unsynced</span>';
  const rssi = s.wifiConnected ? ` <span class="dim">${s.rssi} dBm</span>` : '';
  const rt = s.valveOn && s.valveRuntimeSec > 0
    ? ` <span class="dim">${s.valveRuntimeSec}s / ${s.maxRuntimeSec}s cap</span>`
    : '';
  const lw = s.lastWateredSec > 0
    ? `<br><span class="dim">Last watered: ${s.lastWateredSec}s ago</span>`
    : '';
  const sch = s.scheduleEnabled
    ? `<br><span class="dim">Daily: ${two(s.scheduleHour)}:${two(s.scheduleMinute)}, ${s.scheduleDurationSec}s</span>`
    : '';
  const ipLink = `<a href="http://${s.ip}" style="color:var(--ok)">${s.ip}</a>`;
  const mdnsLink = s.wifiConnected
    ? ` · <a href="http://balconipaani.local" style="color:var(--ok)">balconipaani.local</a>`
    : '';

  document.getElementById('sb').innerHTML =
    `${mode}${valve}${net}${ntp}${rssi}${rt}${lw}` +
    `<br><span class="dim">IP: ${ipLink}${mdnsLink} · Uptime: ${s.uptimeSec}s · ${s.localTime}</span>${sch}`;

  // Runtime progress bar
  const bar  = document.getElementById('rtbar');
  const fill = document.getElementById('rtfill');
  if (s.valveOn && s.valveRuntimeSec > 0) {
    bar.style.display = 'block';
    const pct = Math.min(100, (s.valveRuntimeSec / s.maxRuntimeSec) * 100);
    fill.style.width = pct + '%';
    fill.style.background = pct > 80 ? 'var(--bad)' : pct > 55 ? 'var(--warn)' : 'var(--ok)';
  } else {
    bar.style.display = 'none';
  }

  // WiFi section: hide credential form when already connected
  const connRow  = document.getElementById('wifi-conn-row');
  const wifiForm = document.getElementById('wifi-form');
  if (s.wifiConnected) {
    connRow.innerHTML =
      `<span class="dim">Connected: <b>${esc(s.connectedSsid)}</b></span> ` +
      `<a href="#" style="color:var(--ok);font-size:.82rem" onclick="document.getElementById('wifi-form').style.display='block';return false">Change</a>`;
    wifiForm.style.display = 'none';
  } else {
    connRow.innerHTML = `<span class="pill warn">Not connected — enter credentials below</span>`;
    wifiForm.style.display = 'block';
  }

  document.getElementById('schEnabled').value = s.scheduleEnabled ? '1' : '0';
  document.getElementById('schTime').value = `${two(s.scheduleHour)}:${two(s.scheduleMinute)}`;
  document.getElementById('schDuration').value = s.scheduleDurationSec;
  document.getElementById('maxRuntime').value = s.maxRuntimeSec;
  document.getElementById('tzOffset').value = s.timezoneOffsetMinutes;
}

async function req(path, body) {
  const r = await fetch(path, {
    method: 'POST',
    headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
    body: new URLSearchParams(body || {})
  });
  if (!r.ok) { alert('Error: ' + await r.text()); throw new Error(); }
  return r.json();
}

async function refresh() {
  try {
    const s = await (await fetch('/api/status')).json();
    draw(s);
  } catch (_) {
    document.getElementById('sb').innerHTML =
      '<span style="color:var(--bad)">⚠ Connection lost…</span>';
  }
}

async function valve(state) {
  try { await req('/api/valve/' + state, {}); await refresh(); } catch (_) {}
}
async function setMode(m) {
  try { await req('/api/mode', { mode: m }); await refresh(); } catch (_) {}
}
async function saveSchedule() {
  const t = document.getElementById('schTime').value || '06:00';
  const [hour, minute] = t.split(':');
  try {
    await req('/api/schedule', {
      enabled:    document.getElementById('schEnabled').value,
      hour, minute,
      duration:   document.getElementById('schDuration').value,
      maxRuntime: document.getElementById('maxRuntime').value,
      tzOffset:   document.getElementById('tzOffset').value,
    });
    await refresh();
  } catch (_) {}
}
async function saveWifi() {
  const ssid = document.getElementById('ssid').value.trim();
  if (!ssid) { alert('SSID cannot be empty'); return; }
  try {
    await req('/api/wifi', {
      ssid,
      password: document.getElementById('wfPass').value,
    });
    alert(
      'Credentials saved!\n\n' +
      '1. Reconnect YOUR device to your home WiFi network.\n' +
      '2. Open: http://balconipaani.local\n\n' +
      'If balconipaani.local does not work (Android), check your\n' +
      'router\'s DHCP client list for the "balconipaani" hostname.'
    );
  } catch (_) {}
}
async function doReboot() {
  if (!confirm('Reboot the controller?')) return;
  try { await req('/api/reboot', {}); } catch (_) {}
}

setInterval(refresh, 3000);
refresh();
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
  config.magic                 = CONFIG_MAGIC;
  config.configVersion         = CONFIG_VERSION;
  config.autoMode              = false;   // SAFETY: start in MANUAL
  config.scheduleEnabled       = false;   // SAFETY: schedule off
  config.scheduleHour          = 6;
  config.scheduleMinute        = 0;
  config.scheduleDurationSec   = 60;
  config.maxRuntimeSec         = 300;    // 5-minute hard cap default
  config.timezoneOffsetMinutes = 330;    // IST (UTC+5:30)
}

void persistConfig() {
  config.magic         = CONFIG_MAGIC;
  config.configVersion = CONFIG_VERSION;
  config.checksum      = 0;
  config.checksum      = configChecksum(config);
  EEPROM.put(0, config);
  if (!EEPROM.commit()) {
    Serial.println("EEPROM commit FAILED — credentials not saved!");
  }
}

void loadConfig() {
  EEPROM.get(0, config);
  if (config.magic != CONFIG_MAGIC || config.configVersion != CONFIG_VERSION) {
    Serial.println("Config: invalid magic/version — resetting to defaults");
    setDefaultConfig();
    persistConfig();
    return;
  }
  uint8_t saved = config.checksum;
  uint8_t calc  = configChecksum(config);
  if (saved != calc) {
    Serial.printf("Config: checksum mismatch (saved=%02X calc=%02X) — resetting\n", saved, calc);
    setDefaultConfig();
    persistConfig();
  }
}

// Relay wired to NC terminal: coil must be ENERGISED (LOW) to open NC contact and cut solenoid power.
// WARNING: if ESP loses power the pin goes Hi-Z, relay de-energises, NC closes, solenoid
// gets power and the valve opens. Wire to NO terminal for true fail-safe behaviour.
inline void valveHardwareOff() {
  digitalWrite(RELAY_PIN, LOW);   // energise relay → NC opens → solenoid OFF → valve CLOSED
}

inline void valveHardwareOn() {
  digitalWrite(RELAY_PIN, HIGH);  // de-energise relay → NC closes → solenoid ON → valve OPEN
}

void setValve(bool on, bool byScheduler, const char *reason) {
  if (on) {
    if (!runtime.valveOn) {
      valveHardwareOn();
      runtime.valveOn            = true;
      runtime.startedByScheduler = byScheduler;
      runtime.valveOnSinceMs     = millis();
      Serial.printf("[%lu] VALVE ON  reason=%s\n", millis(), reason);
    }
    return;
  }
  if (runtime.valveOn) {
    valveHardwareOff();
    uint32_t ranSec = static_cast<uint32_t>(millis() - runtime.valveOnSinceMs) / 1000;
    runtime.lastValveOffMs     = millis();
    runtime.valveOn            = false;
    runtime.startedByScheduler = false;
    Serial.printf("[%lu] VALVE OFF reason=%s ran=%us\n", millis(), reason, ranSec);
  }
}

bool elapsedMs(uint32_t since, uint32_t interval) {
  return static_cast<uint32_t>(millis() - since) >= interval;
}

String localTimeText() {
  time_t now = time(nullptr);
  if (now < 1700000000UL) return String("unsynced");
  struct tm tmNow;
  localtime_r(&now, &tmNow);
  char buf[24];
  strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tmNow);
  return String(buf);
}

bool checkNtpSynced() {
  if (runtime.ntpSynced) return true;
  if (time(nullptr) >= 1700000000UL) {
    runtime.ntpSynced = true;
    Serial.printf("[%lu] NTP synced: %s\n", millis(), localTimeText().c_str());
  }
  return runtime.ntpSynced;
}

void startTimeSync();  // forward declaration

bool connectStation() {
  if (strlen(config.ssid) == 0) return false;
  WiFi.persistent(false);  // don't let SDK clobber its own flash store
  WiFi.mode(WIFI_STA);
  WiFi.hostname("balconipaani");
  WiFi.begin(config.ssid, config.password);
  Serial.printf("[%lu] WiFi connecting to '%s'...\n", millis(), config.ssid);

  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && !elapsedMs(start, WIFI_CONNECT_TIMEOUT_MS)) {
    delay(250);
    yield();
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("[%lu] WiFi connected  IP=%s  RSSI=%d dBm\n",
                  millis(), WiFi.localIP().toString().c_str(), WiFi.RSSI());
    MDNS.begin("balconipaani");
    Serial.printf("[%lu] mDNS: http://balconipaani.local\n", millis());
    return true;
  }
  Serial.printf("[%lu] WiFi timeout — falling back to AP mode\n", millis());
  return false;
}

void startAccessPointMode() {
  apMode = true;
  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASSWORD);
  dnsServer.start(53, "*", WiFi.softAPIP());
  Serial.printf("[%lu] AP mode: SSID=%s  IP=%s\n",
                millis(), AP_SSID, WiFi.softAPIP().toString().c_str());
}

// Periodic reconnect when STA link drops (non-blocking check; retries every 60s).
void attemptWifiReconnect() {
  if (apMode) return;
  if (!elapsedMs(runtime.lastWifiCheckMs, WIFI_RECONNECT_INTERVAL_MS)) return;
  runtime.lastWifiCheckMs = millis();
  if (WiFi.status() == WL_CONNECTED) return;

  Serial.printf("[%lu] WiFi link lost — reconnecting...\n", millis());
  WiFi.disconnect();
  WiFi.begin(config.ssid, config.password);
  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && !elapsedMs(start, WIFI_CONNECT_TIMEOUT_MS)) {
    delay(250);
    yield();
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("[%lu] WiFi reconnected  IP=%s\n",
                  millis(), WiFi.localIP().toString().c_str());
    MDNS.begin("balconipaani");
    startTimeSync();  // re-sync NTP after link recovery
  } else {
    Serial.printf("[%lu] WiFi reconnect failed — will retry in %lus\n",
                  millis(), WIFI_RECONNECT_INTERVAL_MS / 1000);
  }
}

void startTimeSync() {
  configTime(config.timezoneOffsetMinutes * 60L, 0,
             "pool.ntp.org", "time.nist.gov", "time.google.com");
  runtime.ntpSynced = false;
}

String ipAddressForUi() {
  return apMode ? WiFi.softAPIP().toString() : WiFi.localIP().toString();
}

void sendStatusJson() {
  uint32_t valveRuntimeSec = runtime.valveOn
    ? static_cast<uint32_t>(millis() - runtime.valveOnSinceMs) / 1000
    : 0;
  uint32_t lastWateredSec = (runtime.lastValveOffMs > 0)
    ? static_cast<uint32_t>(millis() - runtime.lastValveOffMs) / 1000
    : 0;
  bool wifiOk = !apMode && (WiFi.status() == WL_CONNECTED);

  String j;
  j.reserve(256);
  j  = "{\"version\":\"";       j += FIRMWARE_VERSION;
  j += "\",\"autoMode\":";      j += config.autoMode        ? "true" : "false";
  j += ",\"valveOn\":";         j += runtime.valveOn        ? "true" : "false";
  j += ",\"valveRuntimeSec\":"; j += valveRuntimeSec;
  j += ",\"lastWateredSec\":";  j += lastWateredSec;
  j += ",\"scheduleEnabled\":"; j += config.scheduleEnabled ? "true" : "false";
  j += ",\"scheduleHour\":";    j += config.scheduleHour;
  j += ",\"scheduleMinute\":";  j += config.scheduleMinute;
  j += ",\"scheduleDurationSec\":"; j += config.scheduleDurationSec;
  j += ",\"maxRuntimeSec\":";   j += config.maxRuntimeSec;
  j += ",\"timezoneOffsetMinutes\":"; j += config.timezoneOffsetMinutes;
  j += ",\"wifiConnected\":";   j += wifiOk ? "true" : "false";
  j += ",\"rssi\":";            j += wifiOk ? WiFi.RSSI() : 0;
  j += ",\"ip\":\"";            j += ipAddressForUi();
  j += "\",\"uptimeSec\":";     j += millis() / 1000;
  j += ",\"localTime\":\"";     j += localTimeText();
  j += "\",\"ntpSynced\":";     j += runtime.ntpSynced ? "true" : "false";
  j += ",\"connectedSsid\":\""; j += wifiOk ? WiFi.SSID() : "";
  j += "\",\"configuredSsid\":\""; j += String(config.ssid);
  j += "\"}";

  server.send(200, "application/json", j);
}

bool parseIntArg(const String &name, int &out) {
  if (!server.hasArg(name)) return false;
  out = server.arg(name).toInt();
  return true;
}

void handleModeSet() {
  if (!server.hasArg("mode")) { server.send(400, "text/plain", "Missing mode"); return; }
  String mode = server.arg("mode");
  mode.toLowerCase();
  if      (mode == "auto")   config.autoMode = true;
  else if (mode == "manual") config.autoMode = false;
  else { server.send(400, "text/plain", "Invalid mode"); return; }
  persistConfig();
  Serial.printf("[%lu] Mode -> %s\n", millis(), mode.c_str());
  sendStatusJson();
}

void handleScheduleSet() {
  int enabled, hour, minute, duration, maxRuntime, tzOffset;
  if (!parseIntArg("enabled",    enabled)   ||
      !parseIntArg("hour",       hour)       ||
      !parseIntArg("minute",     minute)     ||
      !parseIntArg("duration",   duration)   ||
      !parseIntArg("maxRuntime", maxRuntime) ||
      !parseIntArg("tzOffset",   tzOffset)) {
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
  config.scheduleEnabled       = enabled > 0;
  config.scheduleHour          = static_cast<uint8_t>(hour);
  config.scheduleMinute        = static_cast<uint8_t>(minute);
  config.scheduleDurationSec   = static_cast<uint16_t>(duration);
  config.maxRuntimeSec         = static_cast<uint16_t>(maxRuntime);
  config.timezoneOffsetMinutes = static_cast<int16_t>(tzOffset);
  persistConfig();
  startTimeSync();  // re-apply TZ offset if changed
  Serial.printf("[%lu] Schedule: %02d:%02d dur=%us cap=%us tz=%+dmin enabled=%d\n",
                millis(), hour, minute, duration, maxRuntime, tzOffset, config.scheduleEnabled);
  sendStatusJson();
}

void handleWifiSet() {
  if (!server.hasArg("ssid") || !server.hasArg("password")) {
    server.send(400, "text/plain", "Missing wifi args");
    return;
  }
  String ssid = server.arg("ssid");
  String pass = server.arg("password");
  if (ssid.length() < 1 || ssid.length() > 32 || pass.length() > 64) {
    server.send(400, "text/plain", "Invalid wifi args");
    return;
  }
  memset(config.ssid,     0, sizeof(config.ssid));
  memset(config.password, 0, sizeof(config.password));
  ssid.toCharArray(config.ssid,     sizeof(config.ssid));
  pass.toCharArray(config.password, sizeof(config.password));
  persistConfig();
  sendStatusJson();
  runtime.pendingReboot = true;  // reboot after response is flushed
}

void handleReboot() {
  sendStatusJson();
  runtime.pendingReboot = true;
}

void setupRoutes() {
  server.on("/", HTTP_GET, []() {
    server.send_P(200, "text/html", INDEX_HTML);
  });
  server.on("/api/status",    HTTP_GET,  sendStatusJson);
  server.on("/api/valve/on",  HTTP_POST, []() { setValve(true,  false, "manual_web"); sendStatusJson(); });
  server.on("/api/valve/off", HTTP_POST, []() { setValve(false, false, "manual_web"); sendStatusJson(); });
  server.on("/api/mode",      HTTP_POST, handleModeSet);
  server.on("/api/schedule",  HTTP_POST, handleScheduleSet);
  server.on("/api/wifi",      HTTP_POST, handleWifiSet);
  server.on("/api/reboot",    HTTP_POST, handleReboot);
  server.onNotFound([]() {
    if (apMode) {
      server.sendHeader("Location",
        String("http://") + WiFi.softAPIP().toString() + "/", true);
      server.send(302, "text/plain", "");
      return;
    }
    server.send(404, "text/plain", "Not found");
  });
}

// ── Scheduler + safety watchdog ────────────────────────────────────────────
// Called every SCHEDULER_TICK_MS from loop(). All time-critical safety
// decisions live here; HTTP handlers only set intent, never bypass this.
void runSchedulerAndSafety() {
  if (!elapsedMs(runtime.lastTickMs, SCHEDULER_TICK_MS)) return;
  runtime.lastTickMs = millis();

  // ① Hard runtime cap — unconditional; cannot be overridden by any UI action.
  if (runtime.valveOn &&
      elapsedMs(runtime.valveOnSinceMs,
                static_cast<uint32_t>(config.maxRuntimeSec) * 1000UL)) {
    setValve(false, false, "hard_cap");
    return;
  }

  // ② Scheduled-run duration limit.
  if (runtime.valveOn && runtime.startedByScheduler &&
      elapsedMs(runtime.valveOnSinceMs,
                static_cast<uint32_t>(config.scheduleDurationSec) * 1000UL)) {
    setValve(false, false, "schedule_complete");
    return;
  }

  // ③ Scheduler gate: all conditions must pass before firing.
  if (!config.autoMode || !config.scheduleEnabled || runtime.valveOn) return;
  if (!checkNtpSynced()) return;

  time_t now = time(nullptr);
  struct tm tmNow;
  localtime_r(&now, &tmNow);

  int32_t dayKey = static_cast<int32_t>(tmNow.tm_year) * 1000 + tmNow.tm_yday;
  if (tmNow.tm_hour == config.scheduleHour   &&
      tmNow.tm_min  == config.scheduleMinute &&
      tmNow.tm_sec  <  SCHEDULE_WINDOW_SEC   &&
      dayKey        != runtime.lastScheduleDayKey) {
    runtime.lastScheduleDayKey = dayKey;
    setValve(true, true, "daily_schedule");
  }
}

// ── Setup ──────────────────────────────────────────────────────────────────
void setup() {
  // CRITICAL: Drive relay OFF BEFORE anything else — safe boot guarantee.
  pinMode(RELAY_PIN, OUTPUT);
  valveHardwareOff();

  Serial.begin(115200);
  delay(10);
  Serial.println();
  Serial.printf("=== BalconiPaani %s ===\n", FIRMWARE_VERSION);
  Serial.printf("Reset reason : %s\n", ESP.getResetReason().c_str());
  Serial.printf("Free heap    : %u bytes\n", ESP.getFreeHeap());

  EEPROM.begin(EEPROM_BYTES);
  loadConfig();

  if (!connectStation()) {
    startAccessPointMode();
  }
  startTimeSync();
  setupRoutes();
  server.begin();
  Serial.printf("[%lu] HTTP server ready\n", millis());
}

// ── Loop ───────────────────────────────────────────────────────────────────
void loop() {
  server.handleClient();
  if (apMode) dnsServer.processNextRequest();
  MDNS.update();
  attemptWifiReconnect();
  runSchedulerAndSafety();

  // Deferred reboot: response already flushed, safe to restart now.
  if (runtime.pendingReboot) {
    valveHardwareOff();  // safety: ensure valve off before restart
    delay(500);
    ESP.restart();
  }
}
