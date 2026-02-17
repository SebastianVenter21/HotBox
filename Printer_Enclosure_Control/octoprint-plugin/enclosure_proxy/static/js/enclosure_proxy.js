$(function() {
    function EnclosureProxyViewModel(parameters) {
        var self = this;

        self.updateInterval = null;
        self.currentMode = null;  // null so first poll always triggers a full render

        // ── API helpers ───────────────────────────────────────────────────
        self.apiGet = function(callback) {
            $.ajax({
                url: API_BASEURL + "plugin/enclosure_proxy",
                type: "GET",
                dataType: "json",
                success: callback,
                error: function(xhr) {
                    console.warn("Enclosure GET failed:", xhr.status);
                }
            });
        };

        self.apiCommand = function(command, data, callback) {
            $.ajax({
                url: API_BASEURL + "plugin/enclosure_proxy",
                type: "POST",
                dataType: "json",
                contentType: "application/json",
                data: JSON.stringify(Object.assign({ command: command }, data)),
                success: callback || function() {},
                error: function(xhr) {
                    console.warn("Enclosure POST failed:", xhr.status, command);
                }
            });
        };

        // ── Status display ────────────────────────────────────────────────
        self.updateStatus = function() {
            self.apiGet(function(response) {
                if (!response || !response.ok || !response.parsed) {
                    $("#enclosure-temp").text("--");
                    $("#enclosure-rpm").text("--");
                    $("#enclosure-mode").text("NO SIGNAL");
                    return;
                }

                var p = response.parsed;
                $("#enclosure-temp").text(parseFloat(p.TEMP).toFixed(1) + "°C");
                $("#enclosure-rpm").text(parseInt(p.RPM).toLocaleString());
                $("#enclosure-mode").text(p.MODE);

                // Only update controls if mode changed — avoids slider jumping
                if (p.MODE !== self.currentMode) {
                    self.currentMode = p.MODE;

                    $(".mode-btn").removeClass("active btn-primary");
                    $(".mode-btn[data-mode='" + p.MODE + "']").addClass("active btn-primary");

                    if (p.MODE === "AUTO") {
                        $("#auto-controls").show();
                        $("#manual-controls").hide();
                    } else if (p.MODE === "MANUAL") {
                        $("#auto-controls").hide();
                        $("#manual-controls").show();
                    } else {
                        $("#auto-controls").hide();
                        $("#manual-controls").hide();
                    }
                }

                // Always update setpoint display in AUTO
                if (p.MODE === "AUTO") {
                    $("#setpoint-input").val(parseFloat(p.SETPOINT).toFixed(1));
                }
            });
        };

        // ── Polling — single interval, cleaned up properly ────────────────
        self.startPolling = function() {
            self.stopPolling();   // always clear before starting — prevents stacking
            self.updateStatus();  // immediate first update
            self.updateInterval = setInterval(self.updateStatus, 3000); // 3s is plenty
        };

        self.stopPolling = function() {
            if (self.updateInterval !== null) {
                clearInterval(self.updateInterval);
                self.updateInterval = null;
            }
        };

        // Slow down polling when tab is hidden, resume when visible
        document.addEventListener("visibilitychange", function() {
            if (document.hidden) {
                self.stopPolling();
            } else {
                self.startPolling();
            }
        });

        // ── Mode buttons ──────────────────────────────────────────────────
        $(document).on("click", ".mode-btn", function() {
            var mode = $(this).data("mode");
            self.apiCommand("mode", { mode: mode }, function() {
                self.updateStatus();
            });
        });

        // ── Setpoint ──────────────────────────────────────────────────────
        $(document).on("click", "#setpoint-btn", function() {
            var setpoint = parseFloat($("#setpoint-input").val());
            if (isNaN(setpoint) || setpoint < 10 || setpoint > 50) {
                alert("Setpoint must be between 10 and 50°C");
                return;
            }
            self.apiCommand("setpoint", { c: setpoint }, function() {
                self.updateStatus();
            });
        });

        // ── Heater slider — update label live, send only on release ──────
        $(document).on("input", "#heater-slider", function() {
            $("#heater-value").text($(this).val() + "%");
        });

        $(document).on("change", "#heater-slider", function() {
            self.apiCommand("heater", { value: parseFloat($(this).val()) }, function() {
                self.updateStatus();
            });
        });

        // ── Exhaust slider — update label live, send only on release ─────
        $(document).on("input", "#exhaust-slider", function() {
            $("#exhaust-value").text($(this).val() + "%");
        });

        $(document).on("change", "#exhaust-slider", function() {
            self.apiCommand("exhaust", { value: parseFloat($(this).val()) }, function() {
                self.updateStatus();
            });
        });

        // ── OctoPrint lifecycle hooks ─────────────────────────────────────
        self.onStartupComplete = function() {
            self.startPolling();
        };

        self.onBeforeBinding = function() {
            self.stopPolling();
        };
    }

    OCTOPRINT_VIEWMODELS.push({
        construct: EnclosureProxyViewModel,
        dependencies: [],
        elements: ["#sidebar_plugin_enclosure_proxy"]
    });
});
