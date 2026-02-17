#include "tachometer.h"

// Static instance pointer for ISR
Tachometer* Tachometer::_instance = nullptr;

// Constructor
Tachometer::Tachometer(uint8_t pin) 
    : _pin(pin), _pulseCount(0), _lastReadTime(0), _mux(portMUX_INITIALIZER_UNLOCKED) {
    _instance = this;
}

// ISR: increments pulse count
void IRAM_ATTR Tachometer::handleInterrupt() {
    if (_instance) {
        portENTER_CRITICAL_ISR(&_instance->_mux);
        _instance->_pulseCount++;
        portEXIT_CRITICAL_ISR(&_instance->_mux);
    }
}

// Initialize tachometer
void Tachometer::begin() {
    // Use a pin that supports internal pull-up for open-collector fan outputs
    pinMode(_pin, INPUT_PULLUP);
    
    // Attach ISR
    attachInterrupt(digitalPinToInterrupt(_pin), handleInterrupt, FALLING);

    _lastReadTime = millis();
}

// Get RPM
float Tachometer::getRPM(int pulsesPerRev) {
    unsigned long currentTime = millis();
    unsigned long elapsed = currentTime - _lastReadTime;

        // Only update every 500 ms
    if (elapsed < 500) {
        return NAN;  // Signal "no new data"
    }


    // Get pulse count atomically
    portENTER_CRITICAL(&_mux);
    unsigned long count = _pulseCount;
    _pulseCount = 0; // reset counter
    portEXIT_CRITICAL(&_mux);

    _lastReadTime = currentTime;

    // Calculate RPM
    // RPM = (pulses / pulsesPerRev) * (60000ms / elapsed_ms)
    float rpm = ((float)count / pulsesPerRev) * (60000.0 / elapsed);

    // Optional: clamp negative or ridiculously high RPM
    if (rpm < 0) rpm = 0;
    if (rpm > 10000) rpm = 10000;

    return rpm;
}
