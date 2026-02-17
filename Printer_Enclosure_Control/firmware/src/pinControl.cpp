
#include "pinControl.h"

// ==========================================
// HEATER CONTROL IMPLEMENTATION
// ==========================================
HeaterControl::HeaterControl(uint8_t heaterPWMPin, uint8_t heaterFanPin, uint8_t pwmChannel)
    : _heaterPWMPin(heaterPWMPin), _heaterFanPin(heaterFanPin), _pwmChannel(pwmChannel) {}

void HeaterControl::begin() {
    // Initialize heater fan pin (digital output)
    pinMode(_heaterFanPin, OUTPUT);
    digitalWrite(_heaterFanPin, LOW); // Start OFF
    
    // Initialize heater PWM pin
    pinMode(_heaterPWMPin, OUTPUT);
    digitalWrite(_heaterPWMPin, LOW); // Start OFF
    
    // Setup PWM channel for heater (10 Hz for optocoupler)
    ledcSetup(_pwmChannel, _pwmFreq, _pwmResolution);
    ledcAttachPin(_heaterPWMPin, _pwmChannel);
    
    _currentDuty = 0;
    ledcWrite(_pwmChannel, 0); // Ensure starts at 0
}

void HeaterControl::setHeaterDuty(float percent) {
    // Clamp percent to 0-100
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    
    // Convert to duty cycle (0-255)
    _currentDuty = (int)((percent / 100.0) * 255);
    
    // Apply PWM to heater
    ledcWrite(_pwmChannel, _currentDuty);
    
    // Manage heater fan: ON if any heating, OFF if no heating
    if (_currentDuty > 0) {
        digitalWrite(_heaterFanPin, HIGH); // Turn on heater cooling fan
    } else {
        digitalWrite(_heaterFanPin, LOW);  // Turn off heater cooling fan
    }
}

void HeaterControl::shutdown() {
    setHeaterDuty(0); // Turn everything off safely
}

// ==========================================
// EXHAUST FAN CONTROL IMPLEMENTATION
// ==========================================
ExhaustFanControl::ExhaustFanControl(uint8_t mosfetPin, uint8_t pwmPin, uint8_t pwmChannel)
    : _mosfetPin(mosfetPin), _pwmPin(pwmPin), _pwmChannel(pwmChannel) {}

void ExhaustFanControl::begin() {
    // Initialize PWM pin - force LOW to prevent boot glitches
    pinMode(_pwmPin, OUTPUT);
    digitalWrite(_pwmPin, LOW);
    
    // Initialize MOSFET pin
    pinMode(_mosfetPin, OUTPUT);
    digitalWrite(_mosfetPin, LOW); // Start OFF
    
    // Setup PWM channel (25 kHz for smooth fan operation)
    ledcSetup(_pwmChannel, _pwmFreq, _pwmResolution);
    ledcAttachPin(_pwmPin, _pwmChannel);
    
    _currentDuty = 0;
    ledcWrite(_pwmChannel, 0); // Ensure starts at 0
}

void ExhaustFanControl::setFanDuty(float percent) {
    // Clamp percent to 0-100
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    
    // Convert to duty cycle (0-255)
    _currentDuty = (int)((percent / 100.0) * 255);
    
    // Apply PWM
    ledcWrite(_pwmChannel, _currentDuty);
    
    // Manage MOSFET power: ON if any fan speed needed, OFF otherwise
    if (_currentDuty > 0) {
        digitalWrite(_mosfetPin, HIGH); // Power ON
    } else {
        digitalWrite(_mosfetPin, LOW);  // Power OFF
    }
}

void ExhaustFanControl::shutdown() {
    // Safe shutdown sequence
    ledcWrite(_pwmChannel, 0);
    digitalWrite(_mosfetPin, LOW);
    
    // Detach and pull PWM pin LOW to prevent floating
    ledcDetachPin(_pwmPin);
    pinMode(_pwmPin, OUTPUT);
    digitalWrite(_pwmPin, LOW);
    
    _currentDuty = 0;
}