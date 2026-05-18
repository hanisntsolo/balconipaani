# BalconiPaani MVP2.1 (ESP8266 NodeMCU)

## 1) Architecture overview

A single-firmware local control system with deterministic fail-safe behavior:

- **Control plane**: ESP8266 local web server (`ESP8266WebServer`) serving a mobile-friendly dashboard.
- **Execution plane**: explicit state machine (`valveOn`, `startedByScheduler`, `autoMode`, daily schedule gate) — all safety decisions run in `runSchedulerAndSafety()`, never bypassed by HTTP handlers.
- **Persistence**: EEPROM config with magic + version + checksum validation; corrupt or version-mismatched config resets to safe defaults automatically.
- **Connectivity**:
  - Station mode when saved WiFi credentials are valid.
  - Automatic AP fallback (`BalconiPaani-Setup`) for local onboarding if WiFi is missing/failed.
  - Periodic WiFi reconnect (every 60 s) if link drops while in STA mode.
- **Timing**: NTP-based local clock for daily schedule + independent hard runtime timeout via `millis()` safety guard (millis-overflow-safe arithmetic).
- **Reboot safety**: `pendingReboot` flag defers `ESP.restart()` out of HTTP handler context so the HTTP response is flushed first; valve is driven OFF before every restart.

Design priority is **reliability over features**: local-only, minimal dependencies, manual override always available.

## 2) Pin mapping

| Signal | NodeMCU pin | GPIO | Note |
|--------|-------------|------|------|
| Relay IN | `D2` | GPIO4 | LOW-trigger |
| Relay logic | — | — | `LOW` = ON, `HIGH` = OFF |
| Boot safety | — | — | D2 driven HIGH first line of `setup()` |

Relay and solenoid are powered from a separate 24 V DC adapter. GND is shared between the ESP USB supply and the relay power supply.

## 3) Required libraries

Install the ESP8266 board package in Arduino IDE (Boards Manager → search "esp8266 by ESP8266 Community", version 3.x):

| Header | Source |
|--------|--------|
| `ESP8266WiFi.h` | ESP8266 core |
| `ESP8266WebServer.h` | ESP8266 core |
| `DNSServer.h` | ESP8266 core |
| `EEPROM.h` | ESP8266 core |
| `time.h` | ESP8266 core |

No cloud SDK, no MQTT, no extra external libraries.

## 4) Complete firmware code

Firmware file: `balconipaani.ino`

### Flash steps (≤ 30 min)

1. Open Arduino IDE.
2. Install **esp8266** board package (Boards Manager).
3. Open `balconipaani.ino`.
4. Select board: **NodeMCU 1.0 (ESP-12E Module)**.
5. Upload speed: 115200 (or 921600 for faster flashing).
6. Upload via USB.
7. Open Serial Monitor @ `115200` baud to observe boot log.

> **Note on first flash after upgrading from MVP2.0**: The config magic/version changed (`0xBADA2402`). The device will auto-reset EEPROM to safe defaults on first boot — re-enter your WiFi credentials via the dashboard.

## 5) Web dashboard

Open the device IP in a mobile browser:

- **STA mode**: router-assigned IP (printed on serial at boot, e.g. `IP=192.168.1.42`)
- **AP mode**: connect to `BalconiPaani-Setup` (password `balconi123`), open `http://192.168.4.1/`

Dashboard shows:

| Element | Description |
|---------|-------------|
| Mode pill | AUTO (green) / MANUAL (amber) |
| Valve pill | ON (green) / OFF (grey) |
| WiFi pill | STA (green) / AP (amber) |
| NTP pill | "NTP unsynced" (amber) until first sync |
| RSSI | Signal strength in dBm (STA mode only) |
| Runtime bar | Live progress bar, turns amber >55%, red >80% of cap |
| Runtime counter | Seconds elapsed / max cap when valve is ON |
| Last watered | Seconds ago valve last turned OFF (session) |
| IP / Uptime / Time | Device identity line |
| Daily schedule | Configured time + duration when enabled |

Controls: Mode switch, Valve ON/OFF, Scheduler editor, WiFi onboarding, Reboot.

## 6) Scheduler

- Single daily schedule: `hour:minute` (local time via NTP).
- **All conditions must pass before firing**:
  1. `autoMode` is `true`
  2. `scheduleEnabled` is `true`
  3. Valve is currently OFF
  4. NTP time is synced (epoch ≥ 2023)
  5. Current time matches `scheduleHour:scheduleMinute` within a 10-second window
  6. This calendar day has not already fired (day-key deduplication)
- Schedule duration and max runtime cap are independently configurable and validated (`duration ≤ maxRuntime` enforced server-side).
- Scheduled-run auto-stops at configured `scheduleDurationSec`.
- Hard cap (`maxRuntimeSec`) stops the valve regardless of how it was started.

## 7) Safety mechanisms

| Mechanism | Details |
|-----------|---------|
| **Safe boot** | `valveHardwareOff()` called as the very first statement in `setup()` — before Serial, EEPROM, or networking |
| **Hard runtime cap** | Checked every 500 ms; unconditional OFF when elapsed > `maxRuntimeSec`; takes priority over scheduler duration |
| **Scheduler duration limit** | Auto-OFF when scheduler-started valve exceeds `scheduleDurationSec` |
| **Manual override** | `/api/valve/off` works in any mode at any time |
| **Fail-safe defaults** | `autoMode=false`, `scheduleEnabled=false`, `maxRuntimeSec=300` |
| **Config integrity** | Magic + version + XOR checksum; any corruption auto-resets to defaults |
| **Valve OFF before reboot** | `pendingReboot` path drives relay OFF before `ESP.restart()` |
| **WiFi.persistent(false)** | Prevents SDK from writing credentials to its own flash area; EEPROM is the single source of truth |
| **millis() overflow safety** | All elapsed-time checks use unsigned subtraction: `(uint32_t)(millis() - since) >= interval` |

## 8) Testing procedure

1. **Boot safety** — power-cycle; confirm relay stays OFF, serial shows `=== BalconiPaani MVP2.1 ===`.
2. **Manual control** — use dashboard ON/OFF repeatedly; confirm deterministic actuation and valve runtime counter.
3. **Hard cap** — set max runtime to 15 s; turn ON manually; confirm forced OFF at ~15 s with `reason=hard_cap` in serial.
4. **Scheduler** — enable AUTO; set schedule to current+1 minute with 20 s duration; confirm single trigger and auto-OFF.
5. **WiFi onboarding fallback** — save invalid SSID; reboot; confirm AP mode comes up; dashboard accessible.
6. **WiFi reconnect** — disconnect router; wait 60 s; restore router; confirm device reconnects automatically and NTP re-syncs.
7. **Power-cycle resilience** — reboot multiple times; verify config persistence and safe initial relay state.
8. **NTP unsynced safety** — block NTP (no internet); confirm scheduler does not fire while "NTP unsynced" pill is shown.

## 9) Future roadmap

- **Multi-slot schedule table** — weekday rules, multiple daily windows
- **Soil moisture sensor interlock** — dry-run prevention, skip watering if soil already wet
- **Physical emergency-stop input** — hardware switch on a GPIO, edge-interrupt driven
- **Local audit log** — ring-buffer in flash (LittleFS) with last-N valve events
- **OTA update endpoint** — signed firmware policy with rollback on watchdog reset
- **mDNS** — `balconipaani.local` hostname for zero-config discovery
- **Flow meter input** — pulse-count based volume limiting
