#ifndef TACHOMETER_H
#define TACHOMETER_H

#include <Arduino.h>

class Tachometer {
private:
    uint8_t _pin;
    volatile unsigned long _pulseCount;
    unsigned long _lastReadTime;
    
    // Mutex for thread-safe interrupt handling 
    portMUX_TYPE _mux;

    // Static instance pointer for the ISR
    static Tachometer* _instance;

    // Interrupt Service Routine
    static void IRAM_ATTR handleInterrupt();

public:
    Tachometer(uint8_t pin);
    void begin();
    
    // Returns RPM. 'pulsesPerRev' is usually 2 for PC fans.
    float getRPM(int pulsesPerRev = 2);
};

#endif
