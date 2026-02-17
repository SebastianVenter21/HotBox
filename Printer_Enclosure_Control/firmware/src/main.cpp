#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_Sensor.h>
#include <Adafruit_BME280.h>
//#include <BluetoothSerial.h>

#include "pinControl.h" 
#include "tachometer.h"

// Pin definitions
#define LED_PIN         2    // Onboard LED
#define PIN_HEATER_PWM  33   // Heater PWM via optocoupler
#define PIN_HEATER_FAN  25   // Heater cooling fan
#define PIN_EXHAUST_PWM 26   // Exhaust fan PWM control
#define PIN_EXHAUST_MOSFET 27 // Exhaust fan power MOSFET
#define PIN_TACH        35   // Tachometer sensor (input)

// Safety limits
#define MAX_TEMP_C 60.0           // Hard safety limit
#define MIN_EXHAUST_RPM 100.0     // Minimum RPM when cooling is active
#define CONTROL_INTERVAL_MS 500   // PID loop rate

// PID Parameters
#define KP 25.0                   // Proportional gain
#define KI 0.15                    // Integral gain
#define KD 40.0                    // Derivative gain
#define OUTPUT_LIMIT 100.0        // Max output (+/- 100%)
#define INTEGRAL_LIMIT 20.0       // Anti-windup limit

// Output mapping parameters
#define MIN_HEATER_OUTPUT 5.0    // Minimum heater duty when heating
#define MIN_COOLING_OUTPUT 25.0   // Minimum exhaust fan duty when cooling

Adafruit_BME280 bme;
Stream& ctrl = Serial;

// Hardware controllers (using separate PWM channels)
HeaterControl heaterCtrl(PIN_HEATER_PWM, PIN_HEATER_FAN, 0);      // Channel 0
ExhaustFanControl exhaustCtrl(PIN_EXHAUST_MOSFET, PIN_EXHAUST_PWM, 1); // Channel 1
Tachometer exhaustTach(PIN_TACH);

// ============================================================
// CONTROLLER STATE VARIABLES
// ============================================================
float setpoint = 25.0;            // Target temperature (°C)
bool controlEnabled = false;      // Master control enable
unsigned long lastControlTime = 0;

// PID state variables
float integral = 0.0;
float previousError = 0.0;
float pidOutput = 0.0;

// Current states
float currentTemp = 0.0;
float currentRPM = 0.0;
float heaterDuty = 0.0;
float exhaustDuty = 0.0;
bool safetyTripped = false;
String modeStr = "OFF";

// Telemetry tracking
float lastValidRPM = 0.0;  // For handling NaN from tachometer

// ============================================================
// FORWARD DECLARATIONS (needed because we call these in loop())
// ============================================================
void replyOK(const String& prefix, const String& payload = "");
void replyERR(const String& prefix, const String& code, const String& message = "");
void enterError(const String& errorMsg);
void handleCommandLine(String line);

// ============================================================
// HELPERS
// ============================================================
void replyOK(const String& prefix, const String& payload) {
  if (payload.length()) {
    ctrl.println(prefix + "OK " + payload);
  } else {
    ctrl.println(prefix + "OK");
  }
}

void replyERR(const String& prefix, const String& code, const String& message) {
  if (message.length()) {
    ctrl.println(prefix + "ERR " + code + " " + message);
  } else {
    ctrl.println(prefix + "ERR " + code);
  }
}

// Error / Fault handling
void enterError(const String& errorMsg) {
  safetyTripped = true;
  controlEnabled = false;

  // Shut controls down, turn exhaust fan on for failsafe cooling.
  heaterCtrl.shutdown();
  heaterDuty = 0.0;
  exhaustDuty = 0.0;
  exhaustCtrl.setFanDuty(100.0);
  modeStr = "OFF";

  // Inform PI of fault event, Daemon watches for EVT FAULT to notify user.
  ctrl.println("EVT FAULT " + errorMsg);
}

// HANDLE ALL COMMANDS
void handleCommandLine(String line) {

  // Trim and ignore empty command lines
  line.trim();
  if (!line.length()) {
    return;
  }

  // Optional message id: "@12 <cmd...>"
  String prefix = "";
  if (line.startsWith("@")) {
    int sp = line.indexOf(' ');
    if (sp > 1) {
      prefix = line.substring(0, sp) + " ";
      line = line.substring(sp + 1);
      line.trim();
    }
  }

  // ** Backward compatability for old ESP32 Python Interface commands. ** //
  if (line.startsWith("SETPOINT:")) {
    float newSetpoint = line.substring(9).toFloat();

    // Only accept setpoints between 10°C and 50°C 
    if (newSetpoint >= 10.0 && newSetpoint <= 50.0) {
      setpoint = newSetpoint;
      integral = 0.0; // Reset PID state on new target
      previousError = 0.0;
      replyOK(prefix, "SETPOINT " + String(setpoint, 1));
      return;

    // Error if setpoint outside allowable range
    } else {
      replyERR(prefix, "BAD_VALUE", "SETPOINT_RANGE");
      return;
    }
  }

  if (line.equalsIgnoreCase("CONTROL_ON")) {

    // Check safetyTripped before initialising control.
    if (safetyTripped) {
      replyERR(prefix, "FAULT", "RESET_REQUIRED");
      return;
    }

    // Start control; reset PID
    modeStr = "AUTO";
    controlEnabled = true;
    integral = 0.0;
    previousError = 0.0;
    replyOK(prefix, "MODE AUTO");
    return;
  }

  if (line.equalsIgnoreCase("CONTROL_OFF")) {
    modeStr = "OFF";
    controlEnabled = false;

    heaterCtrl.shutdown();
    exhaustCtrl.shutdown();
    heaterDuty = 0.0;
    exhaustDuty = 0.0;

    replyOK(prefix, "MODE OFF");
    return;
  }

  // OctoPrint-friendly commands
  if (line.equalsIgnoreCase("PING")) {
    replyOK(prefix, "PONG");
    return;
  }

  // Report Telemetry Status
  if (line.equalsIgnoreCase("GET STATUS")) {
    String payload = "";
    payload += "TEMP " + String(currentTemp, 2);
    payload += " RPM " + String(currentRPM, 0);
    payload += " HEATER " + String(heaterDuty, 1);
    payload += " EXHAUST " + String(exhaustDuty, 1);
    payload += " SETPOINT " + String(setpoint, 1);
    payload += " MODE " + modeStr;
    payload += " CONTROL " + String(controlEnabled ? 1 : 0);
    payload += " SAFETY " + String(safetyTripped ? 1 : 0);

    replyOK(prefix, payload);
    return;
  }

  if (line.startsWith("SET MODE ")) {
    String mode = line.substring(9);
    mode.trim();
    mode.toUpperCase();

    if (mode == "OFF") {
      controlEnabled = false;
      modeStr = "OFF";
      heaterCtrl.shutdown();
      exhaustCtrl.shutdown();
      heaterDuty = 0.0;
      exhaustDuty = 0.0;
      replyOK(prefix, "MODE OFF");
      return;
    }

    if (mode == "AUTO") {
      // Check Fault Status Before Enabling Control
      if (safetyTripped) {
        replyERR(prefix, "FAULT", "RESET_REQUIRED");
        return;
      }

      // Start control; reset PID
      modeStr = "AUTO";
      controlEnabled = true;
      integral = 0.0;
      previousError = 0.0;
      replyOK(prefix, "MODE AUTO");
      return;
    }

    if (mode == "MANUAL") {
      controlEnabled = false;
      modeStr = "MANUAL";

      // One-time: stop PID outputs and start manual from a known state
      heaterCtrl.shutdown();
      exhaustCtrl.shutdown();
      heaterDuty = 0.0;
      exhaustDuty = 0.0;

      replyOK(prefix, "MODE MANUAL");
      return;
    }

    replyERR(prefix, "BAD_VALUE", "MODE");
    return;
  }

  if (line.startsWith("SET SETPOINT ")) {
    float setValue = line.substring(13).toFloat();

    // Allowable Setpoint between 10°C and 50°C 
    if (setValue >= 10.0 && setValue <= 50.0) {
      setpoint = setValue;
      integral = 0.0;
      previousError = 0.0;
      replyOK(prefix, "SETPOINT " + String(setpoint, 1));
      return;

    // Give error if setpoint outside of allowable range
    } else {
      replyERR(prefix, "BAD_VALUE", "SETPOINT_RANGE");
      return;
    }
  }

  if (line.equalsIgnoreCase("RESET_FAULT")) {
    safetyTripped = false;
    replyOK(prefix, "FAULT_CLEARED");
    return;
  }

  // MANUAL OUTPUTS || ONLY WHEN IN MANUAL MODE (or when control is off)
  if (modeStr == "MANUAL" || !controlEnabled) {

    // Manual modes for heater
    if (line.startsWith("HEATER:") || line.startsWith("SET HEATER ")) {
      float setHeaterDuty = line.startsWith("HEATER:") ? line.substring(7).toFloat(): line.substring(10).toFloat();
      setHeaterDuty = constrain(setHeaterDuty, 0, 100);
      heaterDuty = setHeaterDuty;
      heaterCtrl.setHeaterDuty(heaterDuty);
      replyOK(prefix, "HEATER " + String(heaterDuty, 1));
      return;
    }

    // Manual modes for exhaust
    if (line.startsWith("EXHAUST:") || line.startsWith("SET EXHAUST ")) {
      float setExhaustDuty = line.startsWith("EXHAUST:") ? line.substring(8).toFloat() : line.substring(11).toFloat();
      setExhaustDuty = constrain(setExhaustDuty, 0, 100);
      exhaustDuty = setExhaustDuty;
      exhaustCtrl.setFanDuty(exhaustDuty);
      replyOK(prefix, "EXHAUST " + String(exhaustDuty, 1));
      return;
    }
  }

  // Unknown command
  replyERR(prefix, "UNKNOWN_CMD");
}

void setup() {
  Serial.begin(115200);
  //SerialBT.begin("ESP32_PID_Controller");

  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);

  // Initialize hardware
  heaterCtrl.begin();
  exhaustCtrl.begin();
  exhaustTach.begin();

  // Initialize BME280
  if (!bme.begin(0x76)) {
    Serial.println("ERR BME280_NOT_FOUND");
  

    // Added fail safe
    heaterCtrl.shutdown();
    exhaustCtrl.shutdown();
    while (1) delay(1000);
  }

  digitalWrite(LED_PIN, HIGH);
  Serial.println("OK READY");
}

void loop() {
  unsigned long currentTime = millis();

  // ============================================================
  // 1. READ SENSORS (Every loop for accuracy)
  // ============================================================

  // Read temp and check if below Max Temp
  currentTemp = bme.readTemperature();
  if (!safetyTripped && currentTemp > MAX_TEMP_C) {
  enterError("OVER_TEMP " + String(currentTemp, 1));
  }

  float rawRPM = exhaustTach.getRPM();
  if (!isnan(rawRPM)) {
      currentRPM = rawRPM;
      lastValidRPM = rawRPM;
  } else {
      // Only use lastValidRPM as fallback if fan is actually commanded on.
      // If exhaust duty is 0 the fan isn't spinning — report 0, not stale noise.
      currentRPM = (exhaustDuty > 0.0) ? lastValidRPM : 0.0;
  }

  // ==============================================================
  // SERIAL COMMAND RECEIVER
  // ==============================================================

  static String rxLine = "";
  while (ctrl.available() > 0) {
    char c = (char)ctrl.read();
    if (c == '\r') continue;
    if (c == '\n') {
      handleCommandLine(rxLine);
      rxLine = "";
    } else {
      rxLine += c;
      if (rxLine.length() > 200) rxLine = ""; // drop garbage
    }
  }

  // PID CONTROLLER
  if (!safetyTripped && controlEnabled) {
    float error = setpoint - currentTemp;

    // 1. Proportional
    float P = KP * error;

    // 2. Integral (with anti-windup)
    // Only accumulate integral if we are close to the target to prevent massive overshoot
    if (abs(error) < 3.0) {
      integral += error * (CONTROL_INTERVAL_MS / 1000.0);
    } else {
      integral = 0; // Clear integral if error is large to prevent windup
    }
    integral = constrain(integral, -INTEGRAL_LIMIT, INTEGRAL_LIMIT);
    float I = KI * integral;

    // 3. Derivative (The "Brake")
    // We use (previousError - error) to focus on the change in process variable
    float dError = (error - previousError) / (CONTROL_INTERVAL_MS / 1000.0);
    float D = KD * dError;

    pidOutput = P + I + D;
    pidOutput = constrain(pidOutput, -OUTPUT_LIMIT, OUTPUT_LIMIT);
    previousError = error;

    // Hardware Mapping - Reduced Deadband
    const float DEADBAND = 0.2; // Smaller deadband for higher accuracy

    if (pidOutput > DEADBAND) { // Heating
      // Use a smoother map
      heaterDuty = map(pidOutput, 0, OUTPUT_LIMIT, MIN_HEATER_OUTPUT, 100.0);
      heaterDuty = constrain(heaterDuty, 0, 100);
      exhaustDuty = 0.0;
      heaterCtrl.setHeaterDuty(heaterDuty);
      exhaustCtrl.shutdown();
    }
    else if (pidOutput < -DEADBAND) { // Cooling
      exhaustDuty = map(-pidOutput, 0, OUTPUT_LIMIT, MIN_COOLING_OUTPUT, 100.0);
      exhaustDuty = constrain(exhaustDuty, 0, 100);
      heaterDuty = 0.0;
      heaterCtrl.shutdown();
      exhaustCtrl.setFanDuty(exhaustDuty);
    }
    else { // Near Zero
      heaterDuty = 0.0;
      exhaustDuty = 0.0;
      heaterCtrl.shutdown();
      exhaustCtrl.shutdown();
    }
  }

// ============================================================
// TELEMETRY SENDING
// ============================================================

// ********NOTE: Disabled: telemetry is now only sent on-demand via "GET STATUS".
// This prevents the serial stream from being flooded and makes OctoPrint/daemon parsing reliable.

  // static unsigned long lastTeleTime = 0;
  // if (currentTime - lastTeleTime >= 1000) {
  //   lastTeleTime = currentTime;

  //   // Create a single buffer to hold the message
  //   // This ensures the whole line is sent as one packet
  //   String dataMsg = "";
  //   dataMsg += "TEMP: " + String(currentTemp, 2);
  //   dataMsg += ", RPM: " + String(currentRPM, 0);
  //   dataMsg += ", HEATER: " + String(heaterDuty, 1);
  //   dataMsg += ", EXHAUST: " + String(exhaustDuty, 1);
  //   dataMsg += ", SETPOINT: " + String(setpoint, 1);
  //   dataMsg += ", CONTROL: " + String(controlEnabled ? 1 : 0);
  //   dataMsg += ", PID: " + String(pidOutput, 2);
  //   dataMsg += ", SAFETY: " + String(safetyTripped ? 1 : 0);

  //   // Keep output exactly as a single serial stream
  //   Serial.println(dataMsg);
  // }

  delay(10); // Short delay to prevent CPU hogging
}

// Helper function for mapping with float values
float map(float x, float in_min, float in_max, float out_min, float out_max) {
  return (x - in_min) * (out_max - out_min) / (in_max - in_min) + out_min;
}
