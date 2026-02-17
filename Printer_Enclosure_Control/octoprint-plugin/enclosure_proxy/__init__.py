# /home/pi/.octoprint/plugins/enclosure_proxy/__init__.py

import octoprint.plugin
import requests
from flask import jsonify


DAEMON_BASE = "http://127.0.0.1:8070"
TIMEOUT_S = 3.0


class EnclosureProxyPlugin(octoprint.plugin.SimpleApiPlugin,
                           octoprint.plugin.StartupPlugin,
                           octoprint.plugin.TemplatePlugin,
                           octoprint.plugin.AssetPlugin):

    def on_after_startup(self):
        self._logger.info("EnclosureProxyPlugin started")

    # ---- Template Plugin ----
    def get_template_configs(self):
        return [
            dict(type="sidebar", name="Enclosure Control", icon="fire")
        ]

    # ---- Asset Plugin ----
    def get_assets(self):
        return dict(
            js=["js/enclosure_proxy.js"],
            css=["css/enclosure_proxy.css"]
        )

    # ---- Simple API Plugin ----
    def on_api_get(self, request):
        """GET /api/plugin/enclosure_proxy returns current status"""
        try:
            r = requests.get(f"{DAEMON_BASE}/status", timeout=TIMEOUT_S)
            r.raise_for_status()
            return jsonify(r.json())
        except Exception as e:
            self._logger.exception("Daemon /status failed")
            return jsonify({"ok": False, "error": str(e)}), 502

    def get_api_commands(self):
        """Define POST command handlers"""
        return {
            "setpoint": ["c"],
            "mode": ["mode"],
            "heater": ["value"],
            "exhaust": ["value"],
        }

    def on_api_command(self, command, data):
        """Handle POST /api/plugin/enclosure_proxy with command"""
        try:
            if command == "setpoint":
                c = float(data.get("c"))
                r = requests.post(f"{DAEMON_BASE}/setpoint", json={"c": c}, timeout=TIMEOUT_S)
                r.raise_for_status()
                return jsonify(r.json())

            elif command == "mode":
                m = str(data.get("mode", "")).upper()
                r = requests.post(f"{DAEMON_BASE}/mode", json={"mode": m}, timeout=TIMEOUT_S)
                r.raise_for_status()
                return jsonify(r.json())

            elif command == "heater":
                v = float(data.get("value"))
                r = requests.post(f"{DAEMON_BASE}/heater", json={"value": v}, timeout=TIMEOUT_S)
                r.raise_for_status()
                return jsonify(r.json())

            elif command == "exhaust":
                v = float(data.get("value"))
                r = requests.post(f"{DAEMON_BASE}/exhaust", json={"value": v}, timeout=TIMEOUT_S)
                r.raise_for_status()
                return jsonify(r.json())

            return jsonify({"error": "unknown command"}), 400

        except Exception as e:
            self._logger.exception(f"Command '{command}' failed")
            return jsonify({"error": str(e)}), 500


__plugin_name__ = "Enclosure Control"
__plugin_pythoncompat__ = ">=3.7,<4"


def __plugin_load__():
    global __plugin_implementation__
    __plugin_implementation__ = EnclosureProxyPlugin()
