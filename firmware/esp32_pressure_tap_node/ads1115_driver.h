#ifndef ADS1115_DRIVER_H
#define ADS1115_DRIVER_H

#include <Arduino.h>
#include <Wire.h>

/**
 * =========================================================================
 * ADS1115 16-Bit Precision I2C ADC Driver (Zero-Dependency Native Wire)
 * =========================================================================
 * Designed for ESP32 High-Resolution Water Pressure & Elevation Head Sensing
 * 
 * - Full 16-bit resolution (65,536 levels / 32,768 single-ended steps)
 * - Programmable Gain Amplifier (PGA) up to +/- 6.144V
 * - Hardware I2C on ESP32 default pins: SDA (GPIO 21), SCL (GPIO 22)
 * - Zero third-party library dependencies (uses built-in Arduino Wire.h)
 */

class ADS1115_Lite {
public:
  enum Gain {
    GAIN_TWOTHIRDS = 0, // +/- 6.144V (1 LSB = 0.1875 mV) - Ideal for 5V sensor (0.5V to 4.5V)
    GAIN_ONE       = 1, // +/- 4.096V (1 LSB = 0.1250 mV) - Ideal with voltage divider
    GAIN_TWO       = 2, // +/- 2.048V (1 LSB = 0.0625 mV)
    GAIN_FOUR      = 3, // +/- 1.024V (1 LSB = 0.03125 mV)
    GAIN_EIGHT     = 4, // +/- 0.512V (1 LSB = 0.015625 mV)
    GAIN_SIXTEEN   = 5  // +/- 0.256V (1 LSB = 0.0078125 mV)
  };

  ADS1115_Lite(uint8_t i2cAddress = 0x48) 
    : _address(i2cAddress), _gain(GAIN_TWOTHIRDS), _isFound(false) {}

  /**
   * Initializes I2C bus and tests communication with ADS1115
   */
  bool begin(int sdaPin = 21, int sclPin = 22, uint32_t frequency = 400000) {
    Wire.begin(sdaPin, sclPin);
    Wire.setClock(frequency);

    // Ping device on I2C bus
    Wire.beginTransmission(_address);
    if (Wire.endTransmission() == 0) {
      _isFound = true;
      return true;
    }
    _isFound = false;
    return false;
  }

  bool isConnected() const {
    return _isFound;
  }

  void setGain(Gain gain) {
    _gain = gain;
  }

  Gain getGain() const {
    return _gain;
  }

  /**
   * Get LSB voltage multiplier based on active PGA gain
   */
  float getLsbVoltage() const {
    switch (_gain) {
      case GAIN_TWOTHIRDS: return 0.000187500f; // 6.144V / 32768
      case GAIN_ONE:       return 0.000125000f; // 4.096V / 32768
      case GAIN_TWO:       return 0.000062500f; // 2.048V / 32768
      case GAIN_FOUR:      return 0.000031250f; // 1.024V / 32768
      case GAIN_EIGHT:     return 0.000015625f; // 0.512V / 32768
      case GAIN_SIXTEEN:   return 0.0000078125f;// 0.256V / 32768
      default:             return 0.000187500f;
    }
  }

  /**
   * Reads single-ended raw 16-bit counts on channel A0..A3 (0 to 3)
   * Returns -32768 on I2C communication error.
   */
  int16_t readRawCounts(uint8_t channel = 0) {
    if (!_isFound) return -32768;
    if (channel > 3) channel = 3;

    // Config word composition:
    // Bit 15: OS = 1 (start single conversion)
    // Bits 14-12: MUX = 100 + channel (AIN[x] vs GND)
    // Bits 11-9: PGA = gain
    // Bit 8: MODE = 1 (single-shot power down)
    // Bits 7-5: DR = 100 (128 SPS)
    // Bit 4: COMP_MODE = 0
    // Bit 3: COMP_POL = 0
    // Bit 2: COMP_LAT = 0
    // Bits 1-0: COMP_QUE = 11 (disable comparator)
    uint16_t mux = (0x04 | (channel & 0x03)) << 12;
    uint16_t pga = ((uint16_t)_gain & 0x07) << 9;
    uint16_t config = 0x8000 | mux | pga | 0x0100 | 0x0080 | 0x0003;

    // Send Config Register pointer (0x01) and 16-bit word
    Wire.beginTransmission(_address);
    Wire.write(0x01);
    Wire.write((uint8_t)(config >> 8));
    Wire.write((uint8_t)(config & 0xFF));
    if (Wire.endTransmission() != 0) {
      _isFound = false;
      return -32768;
    }

    // Poll OS bit (bit 15) until conversion completes (~8 ms at 128 SPS)
    unsigned long startTime = millis();
    bool completed = false;
    while ((millis() - startTime) < 25) {
      Wire.beginTransmission(_address);
      Wire.write(0x01);
      if (Wire.endTransmission() != 0) break;

      Wire.requestFrom(_address, (uint8_t)2);
      if (Wire.available() >= 2) {
        uint16_t status = ((uint16_t)Wire.read() << 8) | Wire.read();
        if (status & 0x8000) {
          completed = true;
          break;
        }
      }
      delayMicroseconds(400);
    }

    if (!completed) {
      // Conversion timed out
      return -32768;
    }

    // Set Pointer Register to Conversion Register (0x00)
    Wire.beginTransmission(_address);
    Wire.write(0x00);
    if (Wire.endTransmission() != 0) return -32768;

    // Read 2-byte 16-bit conversion result
    Wire.requestFrom(_address, (uint8_t)2);
    if (Wire.available() < 2) return -32768;

    int16_t rawResult = (int16_t)(((uint16_t)Wire.read() << 8) | Wire.read());
    return rawResult;
  }

  /**
   * Reads calibrated voltage on channel A0..A3 (in Volts)
   * Returns -1.0f on error.
   */
  float readVoltage(uint8_t channel = 0) {
    int16_t raw = readRawCounts(channel);
    if (raw == -32768) return -1.0f;
    if (raw < 0) raw = 0; // Clamped single-ended floor

    return (float)raw * getLsbVoltage();
  }
  
  /**
   * Performs trimmed-mean oversampling over N readings for zero noise
   */
  float readVoltageAveraged(uint8_t channel = 0, int numSamples = 5) {
    if (numSamples < 1) numSamples = 1;
    float sum = 0.0f;
    int validCount = 0;
    
    for (int i = 0; i < numSamples; i++) {
      float v = readVoltage(channel);
      if (v >= 0.0f) {
        sum += v;
        validCount++;
      }
      if (i < numSamples - 1) delay(2);
    }
    
    if (validCount == 0) return -1.0f;
    return sum / (float)validCount;
  }

private:
  uint8_t _address;
  Gain    _gain;
  bool    _isFound;
};

#endif // ADS1115_DRIVER_H
