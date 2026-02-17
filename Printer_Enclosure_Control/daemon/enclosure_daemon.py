#!/usr/bin/env python3

import threading
import time
import serial
from flask import Flask, request, jsonify

# Port which connects the enclosure's MCU to the PI.
SERIAL_PORT = "/dev/ttyUSB0"
BAUD = 115200
POLL_INTERVAL_S = 2.0
CMD_TIMEOUT_S = 4.0

RECONNECT_DELAY = 3.0   # seconds between reconnect attempts to ensure program oepration after potential communication losses

app = Flask(__name__)

# Thread lock to prevent daemon from writing serial command and reading simultaneously.
_lock = threading.Lock()
_ser = None

# MSG counter with ID's to keep track of commands + responses.
_msg_id = 1

# Status snapshot
_last_status = {
    "ok": False,
    "raw": "",
    "parsed": {},
    "ts": 0.0,
    "error": "",
}


def _close_serial():
    """Forcefully close and discard the serial handle."""
    global _ser
    if _ser:
        try:
            _ser.close()
        except Exception:
            pass
        _ser = None


def open_serial():
    global _ser

    # Already open — do a quick health check
    if _ser and _ser.is_open:
        return True

    # Port handle exists but is not open — discard it
    if _ser and not _ser.is_open:
        _close_serial()

    try:
        _ser = serial.Serial(SERIAL_PORT, BAUD, timeout=0.1, write_timeout=0.5)

        # slight delay after opening serial port (ESP may reset)
        time.sleep(0.2)

        # Flush any boot spam
        try:
            _ser.reset_input_buffer()
            _ser.reset_output_buffer()
        except Exception:
            pass

        print(f"[serial] Opened {SERIAL_PORT}")
        return True

    except Exception as e:
        print(f"[serial] Cannot open {SERIAL_PORT}: {e}")
        _ser = None
        return False


def connect_serial():
    #Block until the ESP32 responds to PING (startup only)
    print("[daemon] Waiting for ESP32...")
    while True:
        if open_serial():
            print(f"[daemon] Connected to {SERIAL_PORT} at {BAUD}")

            # Warm-up: try a couple pings with long timeout
            for _ in range(3):
                r = send_ser_command("PING", timeout=3.0)
                if r:
                    print("[daemon] ESP responded:", r)
                    return
                time.sleep(0.5)

            print("[daemon] ESP not responding yet, retrying...")
            _close_serial()   # close so open_serial() retries properly
        time.sleep(RECONNECT_DELAY)


def read_ser_feedback(timeout_s):
    # Read one line ending in '\n', return str or None on timeout
    end = time.time() + timeout_s
    buffer = bytearray()

    while time.time() < end:
        if _ser and _ser.in_waiting:
            b = _ser.read(1)

            if not b:
                continue
            if b == b"\r":
                continue
            if b == b"\n":
                return buffer.decode("utf-8", errors="replace").strip()

            buffer += b
        else:
            time.sleep(0.005)

    return None


def send_ser_command(cmd, timeout=CMD_TIMEOUT_S):
    ###########################
    # Send one command and wait for a reply with matching @ID.
    # IMPORTANT: Do NOT return None after a single short wait; keep waiting until overall timeout expires.
    # Handles serial errors by closing the port so the next call triggers a reconnect.
    ###########################
    global _msg_id

    with _lock:
        if not open_serial():
            return None

        # Optional: clear old noise so we don't accidentally read a stale line
        try:
            _ser.reset_input_buffer()
        except Exception:
            pass

        mid = _msg_id
        _msg_id += 1

        full_cmd = f"@{mid} {cmd}\n"

        try:
            _ser.write(full_cmd.encode("utf-8"))
        except Exception as e:
            # Write failed — port is dead. Close so next call reopens it.
            print(f"[serial] Write error on '{cmd}': {e} — closing port")
            _close_serial()
            return None

        # Wait up to the FULL timeout for the matching reply
        end = time.time() + float(timeout)
        while time.time() < end:
            # Read in small slices so we can keep looping
            try:
                line = read_ser_feedback(timeout_s=0.25)
            except Exception as e:
                print(f"[serial] Read error: {e} — closing port")
                _close_serial()
                return None

            if line is None:
                continue

            # Only accept replies matching our ID
            if line.startswith(f"@{mid}"):
                return line

            # Otherwise ignore (EVT lines, other noise) and keep waiting

        return None


def parse_status_reply(reply_line):
    # Expected example:
    # @12 OK TEMP 25.14 RPM 1234 HEATER 0.0 EXHAUST 0.0 SETPOINT 45.0 MODE OFF CONTROL 0 SAFETY 0
    parts = reply_line.split()
    parsed = {}
    keys = {"TEMP", "RPM", "HEATER", "EXHAUST", "SETPOINT", "MODE", "CONTROL", "SAFETY"}

    i = 0
    while i < len(parts):
        if parts[i] in keys and i + 1 < len(parts):
            parsed[parts[i]] = parts[i + 1]
            i += 2
        else:
            i += 1

    return parsed


def poller():
    global _last_status

    while True:
        try:
            reply = send_ser_command("GET STATUS", timeout=CMD_TIMEOUT_S)
            print("[poll] reply:", reply)

            if reply:
                _last_status.update({
                    "ok":     True,
                    "raw":    reply,
                    "ts":     time.time(),
                    "parsed": parse_status_reply(reply),
                    "error":  "",
                })
            else:
                # No reply -> port dead as hell or ESP32 not responding
                age = time.time() - _last_status["ts"]
                _last_status.update({
                    "ok":    False,
                    "error": f"No reply from ESP32 (last good reply {age:.0f}s ago)",
                })
                print(f"[poll] No reply — waiting {RECONNECT_DELAY}s before retry")
                time.sleep(RECONNECT_DELAY)   # extra wait before moering again
                continue

        except Exception as e:
            print("Poll error:", e)
            _last_status.update({"ok": False, "error": str(e)})
            time.sleep(RECONNECT_DELAY)
            continue

        time.sleep(POLL_INTERVAL_S)


# ================================
# HTTP API ENDPOINTS
# ================================

@app.route("/ping", methods=["GET"])
def ping():
    r = send_ser_command("PING", timeout=CMD_TIMEOUT_S)
    return jsonify({"ok": r is not None, "reply": r})


@app.route("/status", methods=["GET"])
def status():
    return jsonify(_last_status)


@app.route("/setpoint", methods=["POST"])
def setpoint():
    data = request.json or {}
    c = float(data["c"])
    reply = send_ser_command(f"SET SETPOINT {c}")
    return jsonify({"ok": reply is not None, "reply": reply})


@app.route("/mode", methods=["POST"])
def mode():
    data = request.json or {}
    m = data["mode"]
    reply = send_ser_command(f"SET MODE {m}")
    return jsonify({"ok": reply is not None, "reply": reply})


@app.route("/exhaust", methods=["POST"])
def exhaust():
    data = request.json or {}
    v = float(data["value"])
    reply = send_ser_command(f"SET EXHAUST {v}")
    return jsonify({"ok": reply is not None, "reply": reply})


@app.route("/heater", methods=["POST"])
def heater():
    data = request.json or {}
    v = float(data["value"])
    reply = send_ser_command(f"SET HEATER {v}")
    return jsonify({"ok": reply is not None, "reply": reply})


if __name__ == "__main__":
    connect_serial()

    t = threading.Thread(target=poller, daemon=True)
    t.start()

    print("Enclosure daemon running on port 8070...")
    app.run(host="127.0.0.1", port=8070, threaded=True)