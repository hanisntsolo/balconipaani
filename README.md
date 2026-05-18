# BalconiPaani MVP2 (ESP8266 NodeMCU)

## 1) Architecture overview

A single-firmware local control system with deterministic fail-safe behavior:

- **Control plane**: ESP8266 local web server (`ESP8266WebServer`) serving a mobile-friendly dashboard.
- **Execution plane**: explicit state machine (`valveOn`, `startedByScheduler`, `autoMode`, daily schedule gate).
- **Persistence**: EEPROM config with magic + checksum validation; corrupt config resets to safe defaults.
- **Connectivity**:
  - Station mode when saved WiFi credentials are valid.
  - Automatic AP fallback (`BalconiPaani-Setup`) for local onboarding if WiFi is missing/failed.
- **Timing**: NTP-based local clock for daily schedule + independent hard runtime timeout via `millis()` safety guard.

Design priority is **reliability over features**: local-only, minimal dependencies, manual override always available.

## 2) Pin mapping

- **Relay IN**: `D2` on NodeMCU (`GPIO4`)
- **Relay logic**: LOW-trigger
  - `LOW` => relay ON => valve energized
  - `HIGH` => relay OFF => valve de-energized
- **Boot safety**: firmware drives D2 HIGH immediately in `setup()` before networking/web stack starts.

## 3) Required libraries

Install ESP8266 core in Arduino IDE (Boards Manager):

- **Board package**: `esp8266 by ESP8266 Community` (3.x)

Used headers (all from ESP8266 core):

- `ESP8266WiFi.h`
- `ESP8266WebServer.h`
- `DNSServer.h`
- `EEPROM.h`
- `time.h`

No cloud SDK, no MQTT, no extra external libraries.

## 4) Complete firmware code

Firmware file: `/home/runner/work/balconipaani/balconipaani/balconipaani.ino`

### Flash steps (30-minute path)

1. Open Arduino IDE.
2. Install **esp8266** board package.
3. Open `balconipaani.ino`.
4. Select board: **NodeMCU 1.0 (ESP-12E Module)**.
5. Upload via USB.
6. Open Serial Monitor @ `115200` baud.

## 5) Web dashboard explanation

Open device IP in mobile browser:

- In STA mode: use router-assigned IP (printed on serial)
- In AP mode: connect to `BalconiPaani-Setup` (password `balconi123`), open `http://192.168.4.1/`

Dashboard includes:

- Live status (mode, valve state, uptime, local time, IP)
- Manual valve ON/OFF
- Mode switch (MANUAL/AUTO)
- Scheduler + safety parameter editor
- WiFi credential update + reboot control

## 6) Scheduler explanation

- Single daily schedule: `hour:minute`
- Runs only when:
  - `AUTO` mode is active
  - scheduler is enabled
  - valve is currently OFF
  - NTP time is synced
- Daily run dedup uses day-key (`tm_year + tm_yday`) to avoid repeated triggers in same minute window.
- Schedule duration and max runtime are independently configurable.

## 7) Safety mechanisms

- **Safe boot**: relay forced OFF first.
- **Manual override**: web ON/OFF endpoints work regardless of mode.
- **Hard runtime cap**: valve is forcibly turned OFF when max runtime is reached.
- **Scheduler stop guarantee**: scheduled run auto-stops at configured duration.
- **Fail-safe defaults**:
  - default mode = MANUAL
  - schedule disabled by default
- **Config integrity**: checksum protection; invalid EEPROM content auto-resets to safe defaults.

## 8) Testing procedure

1. **Boot safety**
   - Reboot controller and confirm relay stays OFF during startup.
2. **Manual control**
   - Use dashboard ON/OFF repeatedly and verify deterministic actuation.
3. **Runtime timeout**
   - Set max runtime to 15s, turn valve ON manually, confirm forced OFF ~15s.
4. **Scheduler**
   - Enable AUTO and set schedule to current+1 minute with 20s duration.
   - Confirm one trigger only and auto OFF.
5. **WiFi onboarding fallback**
   - Save invalid SSID, reboot, confirm AP mode comes up and dashboard remains accessible.
6. **Power-cycle resilience**
   - Reboot multiple times and verify config persistence + safe initial relay state.

## 9) Future roadmap

- Multi-slot schedule table (weekday rules)
- Soil moisture sensor + dry-run prevention interlock
- Physical emergency-stop/manual switch input
- Optional local audit log ring-buffer in flash
- OTA update endpoint with signed firmware policy
