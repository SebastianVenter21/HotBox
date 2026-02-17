
#ifndef PIN_CONTROL_H
#define PIN_CONTROL_H

#include <Arduino.h>

// ==========================================
// HEATER SYSTEM (Pin 33: PWM Heater via Optocoupler, Pin 25: Heater Fan)
// ==========================================
class HeaterControl {
private:
    uint8_t _heaterPWMPin;      // Pin 33 - PWM to optocoupler
    uint8_t _heaterFanPin;      // Pin 25 - Heater cooling fan (digital on/off)
    uint8_t _pwmChannel;        // LEDC channel for heater PWM
    
    // PWM Settings for heater (low frequency for optocoupler)
    const uint32_t _pwmFreq = 10;     // 10 Hz for optocoupler/SSR
    const uint8_t _pwmResolution = 8; // 8-bit (0-255)
    
    int _currentDuty = 0;       // Current duty cycle (0-255)

public:
    HeaterControl(uint8_t heaterPWMPin, uint8_t heaterFanPin, uint8_t pwmChannel);
    
    void begin();
    
    // Set heater duty cycle (0-100%)
    // Automatically manages heater fan: ON if duty > 0, OFF if duty = 0
    void setHeaterDuty(float percent);
    
    // Get current duty cycle percentage
    float getCurrentDuty() const { return (_currentDuty / 255.0) * 100.0; }
    
    // Emergency shutdown
    void shutdown();
};

// ==========================================
// EXHAUST FAN SYSTEM (Pin 27: MOSFET Power, Pin 26: PWM Speed Control)
// ==========================================
class ExhaustFanControl {
private:
    uint8_t _mosfetPin;         // Pin 27 - Power control
    uint8_t _pwmPin;            // Pin 26 - PWM speed control
    uint8_t _pwmChannel;        // LEDC channel
    
    int _currentDuty = 0;       // Current duty cycle (0-255)
    
    // PWM Settings for fan (standard high frequency)
    const uint32_t _pwmFreq = 25000;  // 25 kHz for smooth fan control
    const uint8_t _pwmResolution = 8; // 8-bit (0-255)

public:
    ExhaustFanControl(uint8_t mosfetPin, uint8_t pwmPin, uint8_t pwmChannel);
    
    void begin();
    
    // Set exhaust fan duty cycle (0-100%)
    // Automatically manages MOSFET: ON if duty > 0, OFF if duty = 0
    void setFanDuty(float percent);
    
    // Get current duty cycle percentage
    float getCurrentDuty() const { return (_currentDuty / 255.0) * 100.0; }
    
    // Emergency shutdown
    void shutdown();
};

#endif