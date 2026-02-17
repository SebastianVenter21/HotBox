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
}


def open_serial():
    global _ser
    if _ser and _ser.is_open:
        return True

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

        return True

    except Exception:
        _ser = None
        return False


def connect_serial():
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
        time.sleep(2)


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
            time.sleep(0.01)

    return None


def send_ser_command(cmd, timeout=CMD_TIMEOUT_S):
    """
    Send one command and wait for a reply with matching @ID.
    IMPORTANT: Do NOT return None after a single short wait; keep waiting until overall timeout expires.
    """
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
        _ser.write(full_cmd.encode("utf-8"))

        # Wait up to the FULL timeout for the matching reply
        end = time.time() + float(timeout)
        while time.time() < end:
            # Read in small slices so we can keep looping
            line = read_ser_feedback(timeout_s=0.25)
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
                _last_status["ok"] = True
                _last_status["raw"] = reply
                _last_status["ts"] = time.time()
                _last_status["parsed"] = parse_status_reply(reply)
            else:
                _last_status["ok"] = False

        except Exception as e:
            print("Poll error:", e)
            _last_status["ok"] = False

        time.sleep(POLL_INTERVAL_S)


# ================================
# HTTP API ENDPOINTS
# ================================

@app.route("/ping", methods=["GET"])
def ping():
    r = send_ser_command("PING", timeout=CMD_TIMEOUT_S)
    return jsonify({"reply": r})


@app.route("/status", methods=["GET"])
def status():
    return jsonify(_last_status)


@app.route("/setpoint", methods=["POST"])
def setpoint():
    data = request.json
    c = float(data["c"])
    reply = send_ser_command(f"SET SETPOINT {c}")
    return jsonify({"reply": reply})


@app.route("/mode", methods=["POST"])
def mode():
    data = request.json
    m = data["mode"]
    reply = send_ser_command(f"SET MODE {m}")
    return jsonify({"reply": reply})


@app.route("/exhaust", methods=["POST"])
def exhaust():
    data = request.json
    v = float(data["value"])
    reply = send_ser_command(f"SET EXHAUST {v}")
    return jsonify({"reply": reply})


@app.route("/heater", methods=["POST"])
def heater():
    data = request.json
    v = float(data["value"])
    reply = send_ser_command(f"SET HEATER {v}")
    return jsonify({"reply": reply})


if __name__ == "__main__":
    connect_serial()

    t = threading.Thread(target=poller, daemon=True)
    t.start()

    print("Enclosure daemon running on port 8070...")
    app.run(host="127.0.0.1", port=8070)
