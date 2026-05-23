// BalconiPaani MVP3.0 — ESP8266 Irrigation Controller
// Production firmware: local-only, zero cloud dependency.
//
// Hardware:
//   NodeMCU ESP8266 + LOW-trigger relay on D2 → 24V solenoid valve.
//   Shared GND between ESP USB supply and relay 24V supply.
//   Optional: momentary button between D7 and GND for factory reset.

#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ESP8266mDNS.h>
#include <DNSServer.h>
#include <EEPROM.h>
#include <time.h>
#include <ArduinoOTA.h>
#ifndef UPDATE_SIZE_UNKNOWN
#define UPDATE_SIZE_UNKNOWN 0xFFFFFFFF
#endif

// ── Firmware identity ──────────────────────────────────────────────────────
constexpr char     FIRMWARE_VERSION[]         = "MVP3.0";

// ── Hardware ───────────────────────────────────────────────────────────────
constexpr uint8_t  RELAY_PIN                  = D2;   // LOW-trigger relay
constexpr uint8_t  RESET_BTN_PIN              = D7;   // hold LOW 5 s → factory reset
constexpr uint32_t RESET_HOLD_MS              = 5000UL;

// ── EEPROM ─────────────────────────────────────────────────────────────────
constexpr uint16_t EEPROM_BYTES               = 512;
constexpr uint32_t CONFIG_MAGIC               = 0xBADA2402; // unchanged; version bump handles migration
constexpr uint8_t  CONFIG_VERSION             = 2;

// ── Timing ─────────────────────────────────────────────────────────────────
constexpr uint16_t WIFI_CONNECT_TIMEOUT_MS    = 20000;
constexpr uint32_t WIFI_RECONNECT_INTERVAL_MS = 60000UL;
constexpr uint16_t SCHEDULER_TICK_MS          = 500;
constexpr uint8_t  SCHEDULE_WINDOW_SEC        = 10;

// ── AP fallback ────────────────────────────────────────────────────────────
constexpr char AP_SSID[]     = "BalconiPaani-Setup";
constexpr char AP_PASSWORD[] = "balconi123";

// ── Schedule slot (5 bytes packed) ─────────────────────────────────────────
struct ScheduleSlot {
  uint8_t  hour;
  uint8_t  minute;
  uint16_t durationSec;
  bool     enabled;
} __attribute__((packed));

// ── Persistent config (EEPROM-backed, CONFIG_VERSION = 2) ──────────────────
// Layout (199 bytes total, well within 512-byte EEPROM):
//   magic(4) + configVersion(1) + ssid(33) + password(65) + autoMode(1)
//   + slots[3](15) + maxRuntimeSec(2) + timezoneOffsetMinutes(2)
//   + otaPassword(33) + ntpServer(33) + skipToday(1) + _reserved(8)
//   + checksum(1)
struct DeviceConfig {
  uint32_t     magic;
  uint8_t      configVersion;
  char         ssid[33];
  char         password[65];
  bool         autoMode;
  ScheduleSlot slots[3];              // replaces single-schedule fields
  uint16_t     maxRuntimeSec;
  int16_t      timezoneOffsetMinutes;
  char         otaPassword[33];
  char         ntpServer[33];
  bool         skipToday;
  uint8_t      _reserved[8];          // future: zone-2 relay / second valve
  uint8_t      checksum;              // XOR over all bytes before this field
} __attribute__((packed));

static_assert(sizeof(DeviceConfig) < EEPROM_BYTES, "Config too large for EEPROM");

// ── Volatile runtime state (lost on power cycle) ───────────────────────────
struct RuntimeState {
  bool     valveOn;
  bool     startedByScheduler;
  uint16_t scheduledDurationSec;  // snapshot of slot durationSec at fire time
  uint32_t valveOnSinceMs;
  int32_t  lastScheduleDayKey;
  int32_t  lastSeenDayKey;        // for skipToday auto-clear on day rollover
  uint32_t lastTickMs;
  uint32_t lastWifiCheckMs;
  uint32_t lastValveOffMs;        // 0 = not used this session
  bool     ntpSynced;
  bool     pendingReboot;         // defers ESP.restart() out of HTTP handler
};

// ── Watering history (RAM circular buffer, most-recent-first on read) ───────
struct WaterHistoryEntry {
  time_t   ts;
  uint16_t durationSec;
  char     reason[16];
};
constexpr uint8_t HISTORY_SIZE = 10;
static WaterHistoryEntry historyBuf[HISTORY_SIZE];
static uint8_t historyHead  = 0;  // next write position
static uint8_t historyCount = 0;

ESP8266WebServer server(80);
DNSServer        dnsServer;

DeviceConfig config;
RuntimeState runtime{};
bool apMode = false;

// OTA upload abort flag (set when valve is ON at upload start)
static bool otaUploadAborted = false;

// ─────────────────────────────────────────────────────────────────────────────
// INDEX_HTML — served from flash (PROGMEM) to conserve RAM
// ─────────────────────────────────────────────────────────────────────────────
const char INDEX_HTML[] PROGMEM = R"HTML(<!doctype html>
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
button.sm{min-width:auto;font-size:.82rem;padding:7px 12px}
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
.slot-sep{border:0;border-top:1px solid #1e293b;margin:12px 0 6px}
.slot-lbl{font-size:.78rem;color:var(--dim);font-weight:600;margin-bottom:4px}
details>summary{cursor:pointer;color:var(--ok);font-size:.85rem;padding:2px 0;list-style:none}
details>summary::before{content:"▶ "}
details[open]>summary::before{content:"▼ "}
.hist-tbl{width:100%;border-collapse:collapse;font-size:.8rem;margin-top:8px}
.hist-tbl th{color:var(--dim);text-align:left;padding:4px 6px;border-bottom:1px solid #1e293b}
.hist-tbl td{padding:4px 6px;border-bottom:1px solid #1e293b33;color:var(--text)}
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
      <button id="skipBtn" class="sm" onclick="toggleSkip()">Skip Today</button>
    </div>
    <div id="skipStatus" class="dim" style="margin-top:6px"></div>
  </div>

  <div class="card">
    <h2>Scheduler &amp; Safety</h2>

    <div class="slot-lbl">Slot 1</div>
    <div class="grid">
      <div>
        <label>Schedule</label>
        <select id="s0e"><option value="0">Disabled</option><option value="1">Enabled</option></select>
      </div>
      <div><label>Start Time (HH:MM)</label><input id="s0t" type="time"></div>
      <div><label>Duration (sec)</label><input id="s0d" type="number" min="5" max="3600"></div>
    </div>

    <hr class="slot-sep">
    <div class="slot-lbl">Slot 2</div>
    <div class="grid">
      <div>
        <label>Schedule</label>
        <select id="s1e"><option value="0">Disabled</option><option value="1">Enabled</option></select>
      </div>
      <div><label>Start Time (HH:MM)</label><input id="s1t" type="time"></div>
      <div><label>Duration (sec)</label><input id="s1d" type="number" min="5" max="3600"></div>
    </div>

    <hr class="slot-sep">
    <div class="slot-lbl">Slot 3</div>
    <div class="grid">
      <div>
        <label>Schedule</label>
        <select id="s2e"><option value="0">Disabled</option><option value="1">Enabled</option></select>
      </div>
      <div><label>Start Time (HH:MM)</label><input id="s2t" type="time"></div>
      <div><label>Duration (sec)</label><input id="s2d" type="number" min="5" max="3600"></div>
    </div>

    <hr class="slot-sep">
    <div class="grid">
      <div><label>Max Runtime Cap (sec)</label><input id="maxRuntime" type="number" min="10" max="7200"></div>
      <div><label>Timezone Offset (min, IST=330)</label><input id="tzOffset" type="number" min="-720" max="840"></div>
      <div style="grid-column:1/-1"><label>NTP Server</label><input id="ntpServer" type="text" maxlength="32" placeholder="pool.ntp.org"></div>
    </div>
    <div class="row">
      <button onclick="saveSchedule()">Save Schedule</button>
      <button onclick="refresh()">Refresh</button>
    </div>
  </div>

  <div class="card">
    <h2>WiFi &amp; System</h2>
    <div id="wifi-conn-row"></div>
    <div id="wifi-form">
      <label>SSID</label><input id="ssid" type="text" maxlength="32" autocomplete="off">
      <label>Password</label><input id="wfPass" type="password" maxlength="64" autocomplete="new-password">
      <div class="row"><button onclick="saveWifi()">Save &amp; Reboot</button></div>
    </div>
    <div class="row" style="margin-top:8px">
      <button onclick="doReboot()">Reboot</button>
      <a href="/update" style="text-decoration:none"><button class="sm">🔧 Firmware Update</button></a>
    </div>
  </div>

  <div class="card">
    <h2>Security</h2>
    <label>OTA / Firmware Update Password</label>
    <input id="otaPw" type="password" maxlength="32" autocomplete="new-password" placeholder="(leave blank to keep current)">
    <div class="dim" style="margin-top:4px">Used for browser /update page and Arduino IDE OTA push.</div>
    <div class="row"><button onclick="saveOtaPassword()">Save OTA Password</button></div>
  </div>

  <div class="card">
    <h2>Watering History</h2>
    <details id="hist-det">
      <summary onclick="loadHistory(this)">Show last 10 sessions</summary>
      <div id="hist-body" class="dim" style="margin-top:8px">Loading…</div>
    </details>
  </div>

</div>
<script>
const two = n => String(n).padStart(2, '0');
const esc = s => String(s).replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;');
let _skipToday = false;

function draw(s) {
  document.getElementById('ver').textContent = 'v' + (s.version || '');

  const mode  = s.autoMode ? '<span class="pill ok">AUTO</span>' : '<span class="pill warn">MANUAL</span>';
  const vlv   = s.valveOn  ? '<span class="pill ok">Valve ON</span>' : '<span class="pill">Valve OFF</span>';
  const net   = s.wifiConnected ? '<span class="pill ok">STA</span>' : '<span class="pill warn">AP</span>';
  const ntp   = s.ntpSynced ? '' : '<span class="pill warn">NTP unsynced</span>';
  const rssi  = s.wifiConnected ? ' <span class="dim">' + s.rssi + ' dBm</span>' : '';
  const rt    = (s.valveOn && s.valveRuntimeSec > 0)
    ? ' <span class="dim">' + s.valveRuntimeSec + 's / ' + s.maxRuntimeSec + 's cap</span>' : '';
  const lw    = s.lastWateredSec > 0
    ? '<br><span class="dim">Last watered: ' + s.lastWateredSec + 's ago</span>' : '';
  const ipLink   = '<a href="http://' + s.ip + '" style="color:var(--ok)">' + s.ip + '</a>';
  const mdnsLink = s.wifiConnected
    ? ' &middot; <a href="http://balconipaani.local" style="color:var(--ok)">balconipaani.local</a>' : '';

  const slots = s.slots || [];
  const activeSlots = slots.filter(sl => sl.enabled);
  const schSum = activeSlots.length
    ? '<br><span class="dim">Schedules: ' +
      activeSlots.map(sl => two(sl.hour) + ':' + two(sl.minute) + ' (' + sl.durationSec + 's)').join(', ') +
      '</span>'
    : '';
  const skipPill = s.skipToday ? '<span class="pill warn">Skipping Today</span>' : '';

  document.getElementById('sb').innerHTML =
    mode + vlv + net + ntp + rssi + skipPill + rt + lw +
    '<br><span class="dim">IP: ' + ipLink + mdnsLink +
    ' &middot; Uptime: ' + s.uptimeSec + 's &middot; ' + esc(s.localTime) + '</span>' + schSum;

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

  const connRow  = document.getElementById('wifi-conn-row');
  const wifiForm = document.getElementById('wifi-form');
  if (s.wifiConnected) {
    connRow.innerHTML =
      '<span class="dim">Connected: <b>' + esc(s.connectedSsid) + '</b></span> ' +
      '<a href="#" style="color:var(--ok);font-size:.82rem" ' +
      'onclick="document.getElementById(\'wifi-form\').style.display=\'block\';return false">Change</a>';
    wifiForm.style.display = 'none';
  } else {
    connRow.innerHTML = '<span class="pill warn">Not connected — enter credentials below</span>';
    wifiForm.style.display = 'block';
  }

  const dSlots = slots.length === 3 ? slots : [{hour:6,minute:0,durationSec:60,enabled:false},{hour:0,minute:0,durationSec:60,enabled:false},{hour:0,minute:0,durationSec:60,enabled:false}];
  for (let i = 0; i < 3; i++) {
    document.getElementById('s' + i + 'e').value = dSlots[i].enabled ? '1' : '0';
    document.getElementById('s' + i + 't').value = two(dSlots[i].hour) + ':' + two(dSlots[i].minute);
    document.getElementById('s' + i + 'd').value = dSlots[i].durationSec;
  }
  document.getElementById('maxRuntime').value = s.maxRuntimeSec;
  document.getElementById('tzOffset').value   = s.timezoneOffsetMinutes;
  if (s.ntpServer) document.getElementById('ntpServer').value = s.ntpServer;

  _skipToday = !!s.skipToday;
  const skipBtn = document.getElementById('skipBtn');
  skipBtn.textContent = s.skipToday ? 'Resume Today' : 'Skip Today';
  skipBtn.className   = 'sm' + (s.skipToday ? ' warn' : '');
  document.getElementById('skipStatus').textContent =
    s.skipToday ? '\u26a0 Scheduled watering skipped for today' : '';
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
      '<span style="color:var(--bad)">\u26a0 Connection lost\u2026</span>';
  }
}

async function valve(state) {
  try { await req('/api/valve/' + state, {}); await refresh(); } catch (_) {}
}
async function setMode(m) {
  try { await req('/api/mode', { mode: m }); await refresh(); } catch (_) {}
}
async function toggleSkip() {
  try { await req('/api/skip-today', { skip: _skipToday ? '0' : '1' }); await refresh(); } catch (_) {}
}

async function saveSchedule() {
  const body = {
    maxRuntime: document.getElementById('maxRuntime').value,
    tzOffset:   document.getElementById('tzOffset').value,
    ntpServer:  document.getElementById('ntpServer').value.trim() || 'pool.ntp.org'
  };
  for (let i = 0; i < 3; i++) {
    const t = document.getElementById('s' + i + 't').value || '06:00';
    const parts = t.split(':');
    body['s' + i + 'e'] = document.getElementById('s' + i + 'e').value;
    body['s' + i + 'h'] = parts[0];
    body['s' + i + 'm'] = parts[1];
    body['s' + i + 'd'] = document.getElementById('s' + i + 'd').value;
  }
  try { await req('/api/schedule', body); await refresh(); } catch (_) {}
}

async function saveWifi() {
  const ssid = document.getElementById('ssid').value.trim();
  if (!ssid) { alert('SSID cannot be empty'); return; }
  try {
    await req('/api/wifi', { ssid, password: document.getElementById('wfPass').value });
    alert('Credentials saved!\n\n1. Reconnect YOUR device to your home WiFi network.\n2. Open: http://balconipaani.local\n\nIf balconipaani.local does not work (Android), check your router\'s DHCP client list for the "balconipaani" hostname.');
  } catch (_) {}
}

async function doReboot() {
  if (!confirm('Reboot the controller?')) return;
  try { await req('/api/reboot', {}); } catch (_) {}
}

async function saveOtaPassword() {
  const pw = document.getElementById('otaPw').value;
  if (!pw) { alert('Enter a new OTA password'); return; }
  try {
    await req('/api/ota-password', { password: pw });
    document.getElementById('otaPw').value = '';
    alert('OTA password saved.\nArduino IDE OTA will use the new password on next push.');
  } catch (_) {}
}

let _histLoaded = false;
async function loadHistory(summaryEl) {
  if (_histLoaded) return;
  _histLoaded = true;
  const body = document.getElementById('hist-body');
  try {
    const h = await (await fetch('/api/history')).json();
    if (!h.length) { body.innerHTML = 'No watering sessions yet.'; return; }
    let tbl = '<table class="hist-tbl"><thead><tr><th>Time</th><th>Duration</th><th>Reason</th></tr></thead><tbody>';
    for (const e of h) {
      const d = e.ts > 0 ? new Date(e.ts * 1000).toLocaleString() : '(no time)';
      tbl += '<tr><td>' + d + '</td><td>' + e.durationSec + 's</td><td>' + esc(e.reason) + '</td></tr>';
    }
    tbl += '</tbody></table>';
    body.innerHTML = tbl;
  } catch (_) {
    body.innerHTML = '<span style="color:var(--bad)">Failed to load history.</span>';
  }
}

setInterval(refresh, 3000);
refresh();
</script>
</body>
</html>)HTML";

uint8_t configChecksum(const DeviceConfig &cfg) {
  const auto *raw = reinterpret_cast<const uint8_t *>(&cfg);
  uint8_t sum = 0;
  for (size_t i = 0; i < sizeof(DeviceConfig) - 1; i++) sum ^= raw[i];
  return sum;
}

void setDefaultConfig() {
  memset(&config, 0, sizeof(config));
  config.magic                 = CONFIG_MAGIC;
  config.configVersion         = CONFIG_VERSION;
  config.autoMode              = false;              // SAFETY: start in MANUAL
  config.slots[0]              = {6, 0, 60, false};  // 06:00, 60 s, disabled
  config.slots[1]              = {0, 0, 60, false};
  config.slots[2]              = {0, 0, 60, false};
  config.maxRuntimeSec         = 300;               // 5-minute hard cap default
  config.timezoneOffsetMinutes = 330;               // IST (UTC+5:30)
  // OTA password: device-unique, chip-ID derived
  snprintf(config.otaPassword, sizeof(config.otaPassword), "bp%06X", ESP.getChipId());
  strncpy(config.ntpServer, "pool.ntp.org", sizeof(config.ntpServer) - 1);
  config.skipToday             = false;
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
    // Record to circular history buffer
    WaterHistoryEntry &e = historyBuf[historyHead];
    e.ts          = time(nullptr);
    e.durationSec = static_cast<uint16_t>(ranSec);
    strncpy(e.reason, reason, sizeof(e.reason) - 1);
    e.reason[sizeof(e.reason) - 1] = '\0';
    historyHead = (historyHead + 1) % HISTORY_SIZE;
    if (historyCount < HISTORY_SIZE) historyCount++;
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
             config.ntpServer, "time.nist.gov", "time.google.com");
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
  j.reserve(512);
  j  = "{\"version\":\"";             j += FIRMWARE_VERSION;
  j += "\",\"autoMode\":";            j += config.autoMode     ? "true" : "false";
  j += ",\"valveOn\":";               j += runtime.valveOn     ? "true" : "false";
  j += ",\"valveRuntimeSec\":";       j += valveRuntimeSec;
  j += ",\"lastWateredSec\":";        j += lastWateredSec;
  j += ",\"maxRuntimeSec\":";         j += config.maxRuntimeSec;
  j += ",\"timezoneOffsetMinutes\":"; j += config.timezoneOffsetMinutes;
  j += ",\"skipToday\":";             j += config.skipToday    ? "true" : "false";
  j += ",\"ntpServer\":\"";           j += String(config.ntpServer);
  j += "\",\"slots\":[";
  for (uint8_t i = 0; i < 3; i++) {
    if (i > 0) j += ",";
    j += "{\"hour\":";        j += config.slots[i].hour;
    j += ",\"minute\":";      j += config.slots[i].minute;
    j += ",\"durationSec\":"; j += config.slots[i].durationSec;
    j += ",\"enabled\":";     j += config.slots[i].enabled ? "true" : "false";
    j += "}";
  }
  j += "]";
  j += ",\"wifiConnected\":";         j += wifiOk ? "true" : "false";
  j += ",\"rssi\":";                  j += wifiOk ? WiFi.RSSI() : 0;
  j += ",\"ip\":\"";                  j += ipAddressForUi();
  j += "\",\"uptimeSec\":";           j += millis() / 1000;
  j += ",\"localTime\":\"";           j += localTimeText();
  j += "\",\"ntpSynced\":";           j += runtime.ntpSynced ? "true" : "false";
  j += ",\"connectedSsid\":\"";       j += wifiOk ? WiFi.SSID() : "";
  j += "\",\"configuredSsid\":\"";    j += String(config.ssid);
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
  // Parse 3 schedule slots
  for (uint8_t i = 0; i < 3; i++) {
    char ke[4], kh[4], km[4], kd[4];
    snprintf(ke, sizeof(ke), "s%ue", i);
    snprintf(kh, sizeof(kh), "s%uh", i);
    snprintf(km, sizeof(km), "s%um", i);
    snprintf(kd, sizeof(kd), "s%ud", i);
    int enabled, hour, minute, duration;
    if (!parseIntArg(ke, enabled) || !parseIntArg(kh, hour) ||
        !parseIntArg(km, minute)  || !parseIntArg(kd, duration)) {
      server.send(400, "text/plain", "Missing slot args");
      return;
    }
    if (hour < 0 || hour > 23 || minute < 0 || minute > 59 ||
        duration < 5 || duration > 3600) {
      server.send(400, "text/plain", "Invalid slot args");
      return;
    }
    config.slots[i].enabled     = enabled > 0;
    config.slots[i].hour        = static_cast<uint8_t>(hour);
    config.slots[i].minute      = static_cast<uint8_t>(minute);
    config.slots[i].durationSec = static_cast<uint16_t>(duration);
  }
  // Global settings
  int maxRuntime, tzOffset;
  if (!parseIntArg("maxRuntime", maxRuntime) || !parseIntArg("tzOffset", tzOffset)) {
    server.send(400, "text/plain", "Missing global args");
    return;
  }
  if (maxRuntime < 10 || maxRuntime > 7200 || tzOffset < -720 || tzOffset > 840) {
    server.send(400, "text/plain", "Invalid global args");
    return;
  }
  config.maxRuntimeSec         = static_cast<uint16_t>(maxRuntime);
  config.timezoneOffsetMinutes = static_cast<int16_t>(tzOffset);
  if (server.hasArg("ntpServer")) {
    String ntp = server.arg("ntpServer");
    ntp.trim();
    if (ntp.length() > 0 && ntp.length() < sizeof(config.ntpServer)) {
      memset(config.ntpServer, 0, sizeof(config.ntpServer));
      ntp.toCharArray(config.ntpServer, sizeof(config.ntpServer));
    }
  }
  persistConfig();
  startTimeSync();
  Serial.printf("[%lu] Schedule saved: 3 slots, maxRuntime=%us, tz=%+dmin, ntp=%s\n",
                millis(), config.maxRuntimeSec, config.timezoneOffsetMinutes, config.ntpServer);
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
  runtime.pendingReboot = true;
}

void handleReboot() {
  sendStatusJson();
  runtime.pendingReboot = true;
}

void handleSkipToday() {
  if (!server.hasArg("skip")) { server.send(400, "text/plain", "Missing skip arg"); return; }
  config.skipToday = server.arg("skip").toInt() != 0;
  persistConfig();
  Serial.printf("[%lu] skipToday -> %s\n", millis(), config.skipToday ? "true" : "false");
  sendStatusJson();
}

void handleOtaPassword() {
  if (!server.hasArg("password")) { server.send(400, "text/plain", "Missing password"); return; }
  String pw = server.arg("password");
  pw.trim();
  if (pw.length() < 4 || pw.length() >= sizeof(config.otaPassword)) {
    server.send(400, "text/plain", "Password must be 4-32 chars");
    return;
  }
  memset(config.otaPassword, 0, sizeof(config.otaPassword));
  pw.toCharArray(config.otaPassword, sizeof(config.otaPassword));
  persistConfig();
  ArduinoOTA.setPassword(config.otaPassword);  // apply to IDE OTA immediately
  Serial.printf("[%lu] OTA password updated\n", millis());
  sendStatusJson();
}

void handleHistory() {
  String j;
  j.reserve(512);
  j = "[";
  for (uint8_t i = 0; i < historyCount; i++) {
    // Walk backwards from historyHead → most-recent first
    uint8_t idx = (historyHead + HISTORY_SIZE - 1 - i) % HISTORY_SIZE;
    if (i > 0) j += ",";
    j += "{\"ts\":";          j += static_cast<uint32_t>(historyBuf[idx].ts);
    j += ",\"durationSec\":"; j += historyBuf[idx].durationSec;
    j += ",\"reason\":\"";    j += String(historyBuf[idx].reason);
    j += "\"}";
  }
  j += "]";
  server.send(200, "application/json", j);
}

// ── HTTP OTA handlers (valve-guarded, password-protected) ──────────────────
void handleOtaGet() {
  if (!server.authenticate("admin", config.otaPassword)) {
    return server.requestAuthentication();
  }
  if (runtime.valveOn) {
    server.send(409, "text/html",
      "<html><body style='font-family:sans-serif'>"
      "<h2 style='color:#ef4444'>Turn valve OFF before flashing!</h2>"
      "<a href='/'>Back</a></body></html>");
    return;
  }
  server.send(200, "text/html",
    "<html><head><meta charset='utf-8'>"
    "<style>body{font-family:sans-serif;max-width:480px;margin:40px auto;padding:0 16px}"
    "input[type=file]{display:block;margin:16px 0}"
    "input[type=submit]{padding:10px 24px;background:#22c55e;color:#052e16;border:0;border-radius:8px;cursor:pointer;font-size:1rem}"
    "</style></head><body>"
    "<h2>&#128295; BalconiPaani Firmware Update</h2>"
    "<p>Select a compiled <code>.bin</code> file from Arduino IDE (Sketch &rarr; Export Compiled Binary).</p>"
    "<form method='POST' enctype='multipart/form-data'>"
    "<input type='file' name='firmware' accept='.bin'>"
    "<input type='submit' value='Flash Firmware'>"
    "</form>"
    "<p><a href='/'>Cancel</a></p></body></html>");
}

void handleOtaPost() {
  if (!server.authenticate("admin", config.otaPassword)) {
    return server.requestAuthentication();
  }
  if (otaUploadAborted) {
    server.send(409, "text/plain", "ERR: Valve was ON — flash aborted for safety");
    return;
  }
  bool ok = !Update.hasError();
  server.send(ok ? 200 : 500, "text/html",
    ok ? "<html><body style='font-family:sans-serif'><h2>&#9989; Update successful! Rebooting\u2026</h2></body></html>"
       : "<html><body style='font-family:sans-serif'><h2 style='color:red'>&#10060; Update FAILED</h2><a href='/update'>Try again</a></body></html>");
  if (ok) {
    delay(500);
    ESP.restart();
  }
}

void handleOtaUpload() {
  HTTPUpload &upload = server.upload();
  if (upload.status == UPLOAD_FILE_START) {
    otaUploadAborted = runtime.valveOn;
    if (otaUploadAborted) {
      Serial.printf("[%lu] OTA HTTP blocked: valve is ON\n", millis());
      return;
    }
    valveHardwareOff();  // safety: ensure valve closed during flash
    Serial.printf("[%lu] OTA HTTP upload start: %s\n", millis(), upload.filename.c_str());
    if (!Update.begin(UPDATE_SIZE_UNKNOWN)) Update.printError(Serial);
  } else if (!otaUploadAborted && upload.status == UPLOAD_FILE_WRITE) {
    if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
      Update.printError(Serial);
    }
  } else if (!otaUploadAborted && upload.status == UPLOAD_FILE_END) {
    if (Update.end(true)) {
      Serial.printf("[%lu] OTA HTTP: %u bytes written OK\n", millis(), upload.totalSize);
    } else {
      Update.printError(Serial);
    }
  }
}

void setupRoutes() {
  server.on("/",                HTTP_GET,  []() { server.send_P(200, "text/html", INDEX_HTML); });
  server.on("/api/status",      HTTP_GET,  sendStatusJson);
  server.on("/api/valve/on",    HTTP_POST, []() { setValve(true,  false, "manual_web"); sendStatusJson(); });
  server.on("/api/valve/off",   HTTP_POST, []() { setValve(false, false, "manual_web"); sendStatusJson(); });
  server.on("/api/mode",        HTTP_POST, handleModeSet);
  server.on("/api/schedule",    HTTP_POST, handleScheduleSet);
  server.on("/api/wifi",        HTTP_POST, handleWifiSet);
  server.on("/api/reboot",      HTTP_POST, handleReboot);
  server.on("/api/skip-today",  HTTP_POST, handleSkipToday);
  server.on("/api/ota-password",HTTP_POST, handleOtaPassword);
  server.on("/api/history",     HTTP_GET,  handleHistory);
  server.on("/update",          HTTP_GET,  handleOtaGet);
  server.on("/update",          HTTP_POST, handleOtaPost, handleOtaUpload);
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

  // ── Status LED ────────────────────────────────────────────────────────
  static uint32_t lastLedToggleMs = 0;
  if (apMode) {
    if (elapsedMs(lastLedToggleMs, 250)) {
      lastLedToggleMs = millis();
      digitalWrite(LED_BUILTIN, !digitalRead(LED_BUILTIN));  // fast blink: AP mode
    }
  } else if (runtime.valveOn) {
    if (elapsedMs(lastLedToggleMs, 1000)) {
      lastLedToggleMs = millis();
      digitalWrite(LED_BUILTIN, !digitalRead(LED_BUILTIN));  // slow blink: valve ON
    }
  } else {
    digitalWrite(LED_BUILTIN, HIGH);  // off (LED_BUILTIN is active-LOW)
  }

  // ① Hard runtime cap — unconditional; cannot be overridden by any UI action.
  if (runtime.valveOn &&
      elapsedMs(runtime.valveOnSinceMs,
                static_cast<uint32_t>(config.maxRuntimeSec) * 1000UL)) {
    setValve(false, false, "hard_cap");
    return;
  }

  // ② Scheduled-run duration limit (snapshot taken at fire time).
  if (runtime.valveOn && runtime.startedByScheduler &&
      runtime.scheduledDurationSec > 0 &&
      elapsedMs(runtime.valveOnSinceMs,
                static_cast<uint32_t>(runtime.scheduledDurationSec) * 1000UL)) {
    setValve(false, false, "schedule_complete");
    return;
  }

  // ③ Scheduler gate: all conditions must pass before firing.
  if (!config.autoMode || runtime.valveOn) return;
  if (!checkNtpSynced()) return;

  time_t now = time(nullptr);
  struct tm tmNow;
  localtime_r(&now, &tmNow);
  int32_t dayKey = static_cast<int32_t>(tmNow.tm_year) * 1000 + tmNow.tm_yday;

  // Auto-clear skipToday on day rollover
  if (runtime.lastSeenDayKey != 0 && dayKey != runtime.lastSeenDayKey && config.skipToday) {
    config.skipToday = false;
    persistConfig();
    Serial.printf("[%lu] skipToday cleared for new day\n", millis());
  }
  runtime.lastSeenDayKey = dayKey;

  if (config.skipToday) return;

  // Loop all 3 slots; fire the first matching one per day
  for (uint8_t i = 0; i < 3; i++) {
    if (!config.slots[i].enabled) continue;
    if (tmNow.tm_hour == config.slots[i].hour   &&
        tmNow.tm_min  == config.slots[i].minute &&
        tmNow.tm_sec  <  SCHEDULE_WINDOW_SEC    &&
        dayKey        != runtime.lastScheduleDayKey) {
      runtime.lastScheduleDayKey   = dayKey;
      runtime.scheduledDurationSec = config.slots[i].durationSec;
      char label[20];
      snprintf(label, sizeof(label), "slot%u_schedule", i);
      setValve(true, true, label);
      break;  // only fire once per scheduler tick
    }
  }
}

// ── Setup ──────────────────────────────────────────────────────────────────
void setup() {
  // CRITICAL: Drive relay OFF BEFORE anything else — safe boot guarantee.
  pinMode(RELAY_PIN, OUTPUT);
  valveHardwareOff();

  // LED: active-LOW; HIGH = off.
  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN, HIGH);

  // Factory-reset button
  pinMode(RESET_BTN_PIN, INPUT_PULLUP);

  Serial.begin(115200);
  delay(10);
  Serial.println();
  Serial.printf("=== BalconiPaani %s ===\n", FIRMWARE_VERSION);
  Serial.printf("Reset reason : %s\n", ESP.getResetReason().c_str());
  Serial.printf("Free heap    : %u bytes\n", ESP.getFreeHeap());
  Serial.printf("Config size  : %u bytes\n", sizeof(DeviceConfig));

  // Neutralise ESP8266 SDK auto-connect BEFORE any WiFi call.
  WiFi.persistent(false);
  WiFi.setAutoConnect(false);
  WiFi.setAutoReconnect(false);
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  delay(100);

  EEPROM.begin(EEPROM_BYTES);
  loadConfig();

  if (!connectStation()) {
    startAccessPointMode();
  }
  startTimeSync();
  setupRoutes();
  server.begin();
  Serial.printf("[%lu] HTTP server ready\n", millis());

  // ArduinoOTA (Arduino IDE / espota.py push)
  ArduinoOTA.setHostname("balconipaani");
  ArduinoOTA.setPassword(config.otaPassword);
  ArduinoOTA.onStart([]() {
    setValve(false, false, "ota_start");  // safety: close valve before flashing
    Serial.printf("[%lu] ArduinoOTA start\n", millis());
  });
  ArduinoOTA.onEnd([]() {
    Serial.printf("[%lu] ArduinoOTA end — rebooting\n", millis());
  });
  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    Serial.printf("OTA %u%%\r", progress * 100 / total);
  });
  ArduinoOTA.onError([](ota_error_t e) {
    Serial.printf("OTA error[%u]: ", e);
    if      (e == OTA_AUTH_ERROR)    Serial.println("Auth Failed");
    else if (e == OTA_BEGIN_ERROR)   Serial.println("Begin Failed");
    else if (e == OTA_CONNECT_ERROR) Serial.println("Connect Failed");
    else if (e == OTA_RECEIVE_ERROR) Serial.println("Receive Failed");
    else if (e == OTA_END_ERROR)     Serial.println("End Failed");
  });
  ArduinoOTA.begin();
  Serial.printf("[%lu] ArduinoOTA ready (host: balconipaani, pass: chip-ID derived)\n", millis());
}

// ── Loop ───────────────────────────────────────────────────────────────────
void loop() {
  ArduinoOTA.handle();
  server.handleClient();
  if (apMode) dnsServer.processNextRequest();
  MDNS.update();
  attemptWifiReconnect();
  runSchedulerAndSafety();

  // Physical factory reset: hold D7 LOW for 5 s
  static uint32_t resetPressMs = 0;
  if (digitalRead(RESET_BTN_PIN) == LOW) {
    if (resetPressMs == 0) resetPressMs = millis();
    if (elapsedMs(resetPressMs, RESET_HOLD_MS)) {
      Serial.printf("[%lu] Factory reset triggered via D7 button!\n", millis());
      setDefaultConfig();
      persistConfig();
      delay(500);
      ESP.restart();
    }
  } else {
    resetPressMs = 0;
  }

  // Deferred reboot: response already flushed, safe to restart now.
  if (runtime.pendingReboot) {
    valveHardwareOff();  // safety: ensure valve off before restart
    delay(500);
    ESP.restart();
  }
}
