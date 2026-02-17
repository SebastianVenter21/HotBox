# HotBox
A 3D printer enclosure controller built on an ESP32 microcontroller, integrated with OctoPrint running on a Raspberry Pi. The system maintains enclosure temperature via a PID control loop driving a heater and exhaust fan, with full remote control through the OctoPrint web interface.

---

## Table of Contents

1. [System Architecture](#1-system-architecture)
2. [Hardware](#2-hardware)
3. [Repository Structure](#3-repository-structure)
4. [How Each Component Works](#4-how-each-component-works)
   - [ESP32 Firmware](#41-esp32-firmware-maincpp)
   - [Pi Daemon](#42-pi-daemon-enclosure_daemonpy)
   - [OctoPrint Plugin](#43-octoprint-plugin-enclosure_proxy)
5. [File Locations on the Pi](#5-file-locations-on-the-pi)
6. [Initial Setup Guide](#6-initial-setup-guide)
7. [Deploying Updates](#7-deploying-updates)
8. [Systemd Service Reference](#8-systemd-service-reference)
9. [Serial Command Reference](#9-serial-command-reference)
10. [Daemon HTTP API Reference](#10-daemon-http-api-reference)
11. [Troubleshooting](#11-troubleshooting)

---

## 1. System Architecture

Data flows through three layers from browser to physical hardware:

```
Browser (laptop / phone)
        |
        |  HTTP  —  OctoPrint web interface (port 80)
        |
OctoPrint Plugin  [enclosure_proxy/__init__.py]
        |
        |  HTTP  —  localhost only (port 8070)
        |
Pi Daemon  [enclosure_daemon.py]
        |
        |  USB Serial  —  /dev/ttyUSB0  @  115200 baud
        |
ESP32 Firmware  [main.cpp]
        |
        |  GPIO / PWM / I2C
        |
BME280 Temp Sensor  /  Heater PWM  /  Exhaust Fan PWM  /  Tachometer
```

**Why this layered approach?**

- The OctoPrint plugin runs inside OctoPrint's Python process. It cannot safely access serial ports directly without risking conflicts with OctoPrint itself.
- The daemon owns the serial port exclusively. Everything else talks to it over HTTP on localhost — clean, conflict-free, and easy to debug with curl.
- The browser never talks to the daemon directly. It goes through OctoPrint, which handles authentication and serves the UI.

---

## 2. Hardware

| Component | Pin | Notes |
|-----------|-----|-------|
| BME280 temperature sensor | I2C (0x76) | Temp + humidity. Only temp is used for PID. |
| Heater PWM output | GPIO 33 | Via optocoupler for isolation |
| Heater cooling fan | GPIO 25 | Keeps the heater element cool when heater is off |
| Exhaust fan PWM | GPIO 26 | Controls fan speed |
| Exhaust fan MOSFET | GPIO 27 | Power switch for the exhaust fan |
| Tachometer input | GPIO 35 | RPM feedback from exhaust fan |
| Onboard LED | GPIO 2 | High = system ready |

**Safety limits defined in firmware:**

| Parameter | Value |
|-----------|-------|
| Max temperature | 60°C (hard cutoff — triggers fault) |
| Setpoint range | 10°C – 50°C |
| Min heater duty (when heating) | 5% |
| Min exhaust duty (when cooling) | 25% |
| PID loop rate | Every 500ms |

---

## 3. Repository Structure

```
enclosure-control/
│
├── firmware/                          # PlatformIO project — flash to ESP32
│   ├── src/
│   │   ├── main.cpp                   # Main loop, PID controller, command parser
│   │   ├── pinControl.cpp / .h        # HeaterControl + ExhaustFanControl drivers
│   │   └── tachometer.cpp / .h        # RPM measurement
│   └── platformio.ini                 # Board config, upload settings, libraries
│
├── daemon/
│   └── enclosure_daemon.py            # Flask HTTP server + serial bridge
│
├── octoprint-plugin/
│   └── enclosure_proxy/
│       ├── __init__.py                # Plugin logic — API proxy
│       ├── templates/
│       │   └── enclosure_proxy_sidebar.jinja2   # Sidebar HTML
│       └── static/
│           ├── js/enclosure_proxy.js  # Frontend polling + controls
│           └── css/enclosure_proxy.css
│
├── systemd/
│   └── enclosure-daemon.service       # Systemd unit — runs daemon on boot
│
└── README.md                          # This file
```

---

## 4. How Each Component Works

### 4.1 ESP32 Firmware (`main.cpp`)

The firmware runs a continuous `loop()` on the ESP32. Every iteration does the following in order:

#### Sensor Reading
- Reads temperature from the BME280 over I2C every loop cycle.
- If temperature exceeds `MAX_TEMP_C` (60°C), `enterError()` is called immediately — heater shuts off, exhaust runs at 100%, `safetyTripped = true`. AUTO mode cannot be re-enabled until `RESET_FAULT` is sent.
- Reads fan RPM from the tachometer on GPIO 35. The tachometer returns NaN between pulses (especially at low speeds or when the fan is stopped). The firmware handles this by falling back to the last valid RPM reading — but **only if the fan is actually commanded on**. If `exhaustDuty == 0`, RPM is reported as 0 regardless of any previous reading, preventing stale boot-noise values from persisting.

#### Serial Command Receiver
Commands arrive over USB serial (115200 baud). Characters are accumulated into a buffer until a newline `\n` is received, then `handleCommandLine()` is called.

Every command optionally starts with a message ID prefix `@N` which is echoed back in the reply. This lets the Pi daemon match replies to the correct outstanding request even if multiple commands are in flight.

```
Pi sends:    @42 GET STATUS\n
ESP replies: @42 OK TEMP 24.6 RPM 0 HEATER 0.0 EXHAUST 0.0 SETPOINT 25.0 MODE OFF CONTROL 0 SAFETY 0
```

#### PID Controller
Runs every 500ms when `controlEnabled == true` (AUTO mode).

```
error = setpoint - currentTemp

P = KP * error                          (KP = 25.0)
I = KI * integral                       (KI = 0.15,  clamped ±20)
D = KD * (error - previousError) / dt   (KD = 40.0)

pidOutput = P + I + D   (clamped ±100)
```

**Anti-windup:** The integral only accumulates when `|error| < 3.0°C`. If the error is large (e.g. just switched to AUTO from cold), the integral is zeroed. This prevents massive overshoot when the system is far from setpoint.

**Output mapping:**
- `pidOutput > 0.2` → heating: heater duty mapped 5%–100%, exhaust off
- `pidOutput < -0.2` → cooling: exhaust duty mapped 25%–100%, heater off
- `|pidOutput| ≤ 0.2` → deadband: both off

#### Control Modes

| Mode | Behaviour |
|------|-----------|
| `OFF` | Everything shut down. Responds to serial commands but does not control anything. |
| `AUTO` | PID runs every 500ms. Heater and exhaust driven automatically. Manual heater/exhaust commands are ignored. Cannot be entered if `safetyTripped`. |
| `MANUAL` | PID disabled. Operator sets heater and exhaust duty directly. Entering MANUAL resets both outputs to 0 as a known safe starting point. Outputs hold their value until changed. |

---

### 4.2 Pi Daemon (`enclosure_daemon.py`)

A Python Flask application that runs as a background service on the Pi. It is the **only** process that talks to the serial port.

#### Startup Sequence
1. `connect_serial()` opens `/dev/ttyUSB0` and sends `PING` commands until the ESP32 replies. Retries indefinitely — this handles the case where the Pi boots faster than the ESP32 initialises.
2. A background thread starts running `poller()`.
3. Flask HTTP server starts on `127.0.0.1:8070`.

#### Serial Locking
A `threading.Lock()` (`_lock`) wraps every serial operation. This prevents the background poller thread and an incoming HTTP request from both trying to write to the serial port at the same time, which would corrupt the data stream.

#### Background Poller
Sends `GET STATUS` every 2 seconds and caches the response in `_last_status`. This means:
- The `/status` HTTP endpoint responds instantly (returns cached data, no serial I/O).
- The serial port isn't hammered by every browser poll.

#### Reconnection Logic
If a serial write or read fails (e.g. ESP32 reset, USB cable briefly disconnected), the daemon:
1. Calls `_close_serial()` to forcefully close and discard the broken handle.
2. On the next `send_ser_command()` call, `open_serial()` detects `_ser is None` and reopens the port.
3. The poller backs off for `RECONNECT_DELAY` (3 seconds) before retrying.

This means the system self-heals after a USB glitch **without requiring a reboot**.

#### Flask Threading
`app.run(threaded=True)` is set so each incoming HTTP request is handled in its own thread. Without this, a slow serial command (waiting up to 4 seconds for a reply) would block all other HTTP requests, making the UI appear frozen.

---

### 4.3 OctoPrint Plugin (`enclosure_proxy/`)

The plugin lives inside OctoPrint's Python process and adds an enclosure control panel to the OctoPrint sidebar.

#### Plugin Mixins Used

| Mixin | Purpose |
|-------|---------|
| `SimpleApiPlugin` | Creates `GET /api/plugin/enclosure_proxy` and `POST /api/plugin/enclosure_proxy` endpoints |
| `StartupPlugin` | Runs `on_after_startup()` — used for logging on load |
| `TemplatePlugin` | Registers the sidebar HTML template so OctoPrint injects it into the web UI |
| `AssetPlugin` | Registers the JS and CSS files so OctoPrint serves them to the browser |

#### API Flow (browser → plugin → daemon → ESP32)

```
Browser JS          OctoPrint Plugin         Pi Daemon          ESP32
    |                      |                     |                 |
    |-- POST /api/plugin -->|                     |                 |
    |   enclosure_proxy     |                     |                 |
    |   {"command":"mode",  |                     |                 |
    |    "mode":"AUTO"}     |                     |                 |
    |                       |-- POST /mode ------>|                 |
    |                       |   {"mode":"AUTO"}   |                 |
    |                       |                     |-- @N SET MODE-->|
    |                       |                     |    AUTO\n       |
    |                       |                     |<-- @N OK MODE --| 
    |                       |                     |    AUTO         |
    |                       |<-- {"reply":"@N OK"}|                 |
    |<-- 200 JSON ----------|                     |                 |
```

#### Frontend JavaScript (`enclosure_proxy.js`)

- Polls `GET /api/plugin/enclosure_proxy` every **3 seconds** to update the status display.
- `startPolling()` always calls `stopPolling()` first — this prevents multiple overlapping intervals from stacking up if the page is refreshed or the OctoPrint UI reinitialises.
- `currentMode` starts as `null` so the very first status poll always triggers a full UI render regardless of what mode the ESP32 is currently in.
- The tab visibility API (`visibilitychange` event) is used to pause polling when the browser tab is hidden and resume it when the tab is brought back into focus — reduces unnecessary load on the Pi.
- Slider `input` events update the displayed percentage live as you drag. The actual command is only sent on the `change` event (when you release the slider) — this prevents flooding the ESP32 with serial commands while dragging.

---

## 5. File Locations on the Pi

| File | Path on Pi |
|------|-----------|
| Daemon script | `/home/pi/enclosure_daemon/enclosure_daemon.py` |
| Daemon Python venv | `/home/pi/enclosure_daemon/venv/` |
| Systemd service file | `/etc/systemd/system/enclosure-daemon.service` |
| Plugin root | `/home/pi/.octoprint/plugins/enclosure_proxy/` |
| Plugin main file | `/home/pi/.octoprint/plugins/enclosure_proxy/__init__.py` |
| Sidebar template | `/home/pi/.octoprint/plugins/enclosure_proxy/templates/enclosure_proxy_sidebar.jinja2` |
| JavaScript | `/home/pi/.octoprint/plugins/enclosure_proxy/static/js/enclosure_proxy.js` |
| CSS | `/home/pi/.octoprint/plugins/enclosure_proxy/static/css/enclosure_proxy.css` |
| OctoPrint log | `/home/pi/.octoprint/logs/octoprint.log` |

---

## 6. Initial Setup Guide

### 6.1 Flash the ESP32
Open the `firmware/` folder in PlatformIO (VS Code extension or CLI). Connect the ESP32 to your **development machine** via USB and run:
```
pio run --target upload
```
The ESP32 is not flashed from the Pi — the Pi connects to it only as a serial client.

### 6.2 Set Up the Daemon on the Pi
SSH into the Pi:
```bash
ssh pi@octopi.local
```

Create the daemon directory and virtual environment:
```bash
mkdir -p ~/enclosure_daemon
python3 -m venv ~/enclosure_daemon/venv
source ~/enclosure_daemon/venv/bin/activate
pip install flask pyserial
deactivate
```

Copy the daemon script:
```bash
# From your machine:
scp daemon/enclosure_daemon.py pi@octopi.local:/home/pi/enclosure_daemon/
```

### 6.3 Install the Systemd Service
```bash
# Copy the service file to the Pi
scp systemd/enclosure-daemon.service pi@octopi.local:/tmp/

# On the Pi:
sudo mv /tmp/enclosure-daemon.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable enclosure-daemon    # start on every boot
sudo systemctl start enclosure-daemon
sudo systemctl status enclosure-daemon --no-pager
```

### 6.4 Install the OctoPrint Plugin
```bash
# Copy the plugin folder to the Pi
scp -r octoprint-plugin/enclosure_proxy pi@octopi.local:/home/pi/.octoprint/plugins/

# Restart OctoPrint to load the plugin
sudo systemctl restart octoprint
```

### 6.5 Verify Everything is Working
```bash
# 1. Check daemon is running and polling
sudo journalctl -u enclosure-daemon -f

# 2. Hit the daemon directly
curl -s http://127.0.0.1:8070/status ; echo

# 3. Hit the OctoPrint plugin API (replace YOUR_KEY with your OctoPrint API key)
curl -s http://127.0.0.1/api/plugin/enclosure_proxy \
  -H "X-Api-Key: YOUR_KEY" ; echo

Your OctoPrint API key is found at: **OctoPrint Settings → Application Keys**
```
### 6.6 Accessing OctoPrint
```bash
Access the OctoPrint UI via: https://octopi.local or https://( PI's IP ); The enclosure sidebar can be found on the bottom left of the UI
```
---

## 7. Deploying Updates

### Updating the Firmware
Make changes in `firmware/src/`, then from your development machine:
```bash
pio run --target upload
```
The Pi daemon will automatically reconnect to the ESP32 after it resets from the flash.

### Updating the Daemon
```bash
scp daemon/enclosure_daemon.py pi@octopi.local:/home/pi/enclosure_daemon/
ssh pi@octopi.local "sudo systemctl restart enclosure-daemon"
```

### Updating the Plugin
```bash
scp octoprint-plugin/enclosure_proxy/__init__.py \
    pi@octopi.local:/home/pi/.octoprint/plugins/enclosure_proxy/

scp octoprint-plugin/enclosure_proxy/static/js/enclosure_proxy.js \
    pi@octopi.local:/home/pi/.octoprint/plugins/enclosure_proxy/static/js/

# Restart OctoPrint after any plugin file changes
ssh pi@octopi.local "sudo systemctl restart octoprint"
```

### Using Git (recommended)
On the Pi, clone the repo once:
```bash
git clone https://github.com/YOUR_USERNAME/enclosure-control.git ~/enclosure-control
```

After pushing changes from your dev machine, pull and restart on the Pi:
```bash
cd ~/enclosure-control
git pull
sudo cp daemon/enclosure_daemon.py ~/enclosure_daemon/
sudo systemctl restart enclosure-daemon
```

---

## 8. Systemd Service Reference

The service file at `/etc/systemd/system/enclosure-daemon.service` defines how the daemon is managed by the OS.

### Key Settings Explained

| Setting | Value | Why |
|---------|-------|-----|
| `After=network.target` | — | Ensures the system is fully up before starting. Prevents race conditions on boot where `/dev/ttyUSB0` isn't ready yet. |
| `User=pi` | — | Runs as the `pi` user, not root. The `pi` user has access to `/dev/ttyUSB0` via the `dialout` group. |
| `Restart=always` | — | If the daemon crashes for any reason (Python exception, serial error), systemd restarts it automatically. |
| `RestartSec=3` | 3 seconds | Waits 3 seconds before restarting. Gives the USB port time to re-enumerate if the ESP32 was briefly disconnected. |
| `StandardOutput=journal` | — | Sends all `print()` output to the systemd journal, visible via `journalctl`. |

### Common Service Commands

```bash
# Check current status
sudo systemctl status enclosure-daemon --no-pager

# Start / stop / restart
sudo systemctl start enclosure-daemon
sudo systemctl stop enclosure-daemon
sudo systemctl restart enclosure-daemon

# Enable / disable auto-start on boot
sudo systemctl enable enclosure-daemon
sudo systemctl disable enclosure-daemon

# Live log feed
sudo journalctl -u enclosure-daemon -f

# Last 100 log lines
sudo journalctl -u enclosure-daemon -n 100 --no-pager

# After editing the service file, always reload:
sudo systemctl daemon-reload
```

---

## 9. Serial Command Reference

All commands are sent by the Pi daemon. Commands can optionally be prefixed with `@N` where N is a message ID — the ESP32 echoes the same prefix in its reply so the daemon can match responses.

| Command | Response | Notes |
|---------|----------|-------|
| `PING` | `OK PONG` | Health check |
| `GET STATUS` | `OK TEMP {t} RPM {r} HEATER {h} EXHAUST {e} SETPOINT {s} MODE {m} CONTROL {c} SAFETY {f}` | Full telemetry snapshot |
| `SET MODE OFF` | `OK MODE OFF` | Disable control, shut down outputs |
| `SET MODE AUTO` | `OK MODE AUTO` | Enable PID. Rejected with `ERR FAULT` if safety is tripped. |
| `SET MODE MANUAL` | `OK MODE MANUAL` | Disable PID, reset outputs to 0, enable manual commands |
| `SET SETPOINT {c}` | `OK SETPOINT {c}` | Set target temp in °C. Range: 10.0–50.0. Resets PID integral. |
| `SET HEATER {duty}` | `OK HEATER {duty}` | Set heater PWM 0–100. MANUAL mode only. |
| `SET EXHAUST {duty}` | `OK EXHAUST {duty}` | Set exhaust fan PWM 0–100. MANUAL mode only. |
| `RESET_FAULT` | `OK FAULT_CLEARED` | Clears safety fault so AUTO can be re-enabled |

**Error responses:**

| Response | Meaning |
|----------|---------|
| `ERR FAULT RESET_REQUIRED` | Safety is tripped. Send `RESET_FAULT` first. |
| `ERR BAD_VALUE SETPOINT_RANGE` | Setpoint outside 10–50°C |
| `ERR BAD_VALUE MODE` | Unknown mode string |
| `ERR UNKNOWN_CMD` | Command not recognised |

**Unsolicited events (ESP32 → Pi, no command sent):**

| Event | Meaning |
|-------|---------|
| `EVT FAULT OVER_TEMP {temp}` | Temperature exceeded 60°C. Safety has been engaged. |
| `OK READY` | ESP32 has finished booting and is ready for commands |

---

## 10. Daemon HTTP API Reference

All endpoints listen on `127.0.0.1:8070` (localhost only — not accessible from the network directly).

| Method | Endpoint | Body | Response |
|--------|----------|------|----------|
| `GET` | `/ping` | — | `{"ok": true, "reply": "@N OK PONG"}` |
| `GET` | `/status` | — | Cached status snapshot (see below) |
| `POST` | `/setpoint` | `{"c": 45.0}` | `{"ok": true, "reply": "@N OK SETPOINT 45.0"}` |
| `POST` | `/mode` | `{"mode": "AUTO"}` | `{"ok": true, "reply": "@N OK MODE AUTO"}` |
| `POST` | `/heater` | `{"value": 30.0}` | `{"ok": true, "reply": "@N OK HEATER 30.0"}` |
| `POST` | `/exhaust` | `{"value": 80.0}` | `{"ok": true, "reply": "@N OK EXHAUST 80.0"}` |

**Status response structure:**
```json
{
  "ok": true,
  "raw": "@77 OK TEMP 24.60 RPM 0 HEATER 0.0 EXHAUST 0.0 SETPOINT 25.0 MODE OFF CONTROL 0 SAFETY 0",
  "parsed": {
    "TEMP": "24.60",
    "RPM": "0",
    "HEATER": "0.0",
    "EXHAUST": "0.0",
    "SETPOINT": "25.0",
    "MODE": "OFF",
    "CONTROL": "0",
    "SAFETY": "0"
  },
  "ts": 1771245866.52,
  "error": ""
}
```

`ok: false` with a message in `error` means the daemon lost contact with the ESP32.

---

## 11. Troubleshooting

### Sidebar not visible in OctoPrint
```bash
tail -n 100 ~/.octoprint/logs/octoprint.log | grep -i enclosure
```
Look for Python import errors or syntax errors in `__init__.py`. After any plugin fix, restart OctoPrint: `sudo systemctl restart octoprint`.

### API returns 404
The plugin didn't load. Check the log above. Usually a syntax error in `__init__.py`.

### API returns 500
The plugin loaded but is crashing at runtime. Check the log for a Python traceback.

### API returns 502
The plugin is running but can't reach the daemon.
```bash
sudo systemctl status enclosure-daemon
curl -s http://127.0.0.1:8070/status ; echo
```

### Daemon won't start
```bash
sudo journalctl -u enclosure-daemon -n 50 --no-pager
```
Usually a wrong path in the service file, missing venv, or syntax error in the daemon script.

### Status shows `ok: false` / readings are stale
The daemon lost serial contact with the ESP32. Check:
```bash
ls /dev/ttyUSB0          # does the port exist?
sudo journalctl -u enclosure-daemon -f   # is it reconnecting?
```
Try unplugging and replugging the ESP32 USB cable. The daemon will reconnect automatically — no reboot needed.

### RPM shows 10000 at idle
Boot-noise artifact captured at startup. Fixed in firmware by only using the last-valid-RPM fallback when `exhaustDuty > 0`. Reflash the ESP32 with the corrected `main.cpp`.

### Can't enter AUTO mode
Safety fault is active (`SAFETY: 1` in status). The enclosure exceeded 60°C at some point. Fix the hardware issue, then send `RESET_FAULT` via the OctoPrint plugin or:
```bash
curl -s -X POST http://127.0.0.1:8070/mode \
  -H "Content-Type: application/json" \
  -d '{"mode":"AUTO"}' ; echo
# If fault is active this returns ERR FAULT RESET_REQUIRED
```

### OctoPrint is running but web UI is unreachable
```bash
sudo systemctl status octoprint --no-pager
sudo journalctl -u octoprint -n 50 --no-pager
```
