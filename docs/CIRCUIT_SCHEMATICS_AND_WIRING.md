# Circuit Schematics & Hardware Wiring Guide

This document details the exact electrical connections, protection circuits, and pinout tables for both ESP32 nodes.

---

## 1. Node 1: Tap Head Pressure Sensor Node (Direct ADS1115 — Zero Resistors!)

> [!IMPORTANT]
> **Zero Resistors Needed!**
> By using the **ADS1115 16-Bit I2C ADC module** powered at **$5\text{V}$ (VIN)**, the entire resistive voltage divider circuit is **completely eliminated**.
> - The 0.2 MPa sensor outputs $0.5\text{V} - 4.5\text{V}$.
> - The ADS1115 operates up to $5.5\text{V}$ supply and reads $0.5\text{V} - 4.5\text{V}$ natively on pin **A0** at full 16-bit resolution ($0.1875\text{ mV}$ per step).
> - You do **NOT** need any $10\text{k}\Omega$, $20\text{k}\Omega$, or $33\text{k}\Omega$ resistors.

### 1.1 Complete Wiring Schematic (Direct ADC Mode)

```text
  +-----------------------------------------------------------+
  |              0.2 MPa Pressure Transducer                  |
  |                                                           |
  |   [ RED Wire   : 5V VCC ]                                 |
  |   [ BLACK Wire : GND    ]                                 |
  |   [ YELLOW Wire: Signal (0.5V - 4.5V) ]                   |
  +--------+-------------+----------------+-------------------+
           |             |                |
           |             |                |
           |             |                +---------------------------> [ A0 ] (ADS1115)
           |             |                                                 |
           |             +--------------------------------+                |
           |                                              |                |
           +-----------------------------+                |                |
                                         |                |                |
     ====================================v================v================v=====
     ADS1115 16-Bit ADC Module:
       [ VDD  ] --------------------------------------------> ESP32 VIN (5V)
       [ GND  ] --------------------------------------------> ESP32 GND (Shared Common)
       [ SCL  ] --------------------------------------------> ESP32 GPIO 22 (I2C Clock)
       [ SDA  ] --------------------------------------------> ESP32 GPIO 21 (I2C Data)
       [ ADDR ] --------------------------------------------> ESP32 GND (Sets I2C Addr to 0x48)
       [ ALRT ] --------------------------------------------> Leave Unconnected
       [ A0   ] --------------------------------------------> SENSOR YELLOW (Signal Wire)
       [ A1, A2, A3 ] --------------------------------------> Leave Unconnected
     ============================================================================
```

### 1.2 Pinout Connection Table (Direct ADS1115 Mode)

| Device / Wire | Connects To | Wire Color / Function |
| :--- | :--- | :--- |
| **Sensor RED** | ESP32 **VIN** ($5\text{V}$) & ADS1115 **VDD** | $5.0\text{V}$ Power Rail |
| **Sensor BLACK** | ESP32 **GND** & ADS1115 **GND** | Shared Common Ground |
| **Sensor YELLOW** | ADS1115 **A0** Pin | Direct Analog Signal ($0.5\text{V} - 4.5\text{V}$) — **NO Resistors!** |
| **ADS1115 VDD** | ESP32 **VIN** ($5\text{V}$) | ADC Supply Voltage |
| **ADS1115 GND** | ESP32 **GND** | Shared Common Ground |
| **ADS1115 SCL** | ESP32 **GPIO 22** | Hardware I2C Clock Line |
| **ADS1115 SDA** | ESP32 **GPIO 21** | Hardware I2C Data Line |
| **ADS1115 ADDR** | ESP32 **GND** (or GND wire) | Locks I2C Address to `0x48` |
| **ADS1115 ALRT** | *Unconnected* | Not used |
| **ADS1115 A1..A3** | *Unconnected* | Auxiliary analog channels |

---

### 1.3 Electrical Specifications & Safety
* **Supply Voltage ($V_{CC}$):** $5.0 \text{ V}$ DC (from ESP32 `VIN` pin when USB-powered).
* **Sensor Signal Output:** $0.5 \text{ V}$ at $0 \text{ kPa}$ up to $4.5 \text{ V}$ at $200 \text{ kPa}$.
* **ADS1115 Operating Mode:** PGA configured to $\pm 6.144\text{ V}$ (`GAIN_TWOTHIRDS`).
* **Measurement Resolution:**
  $$1\text{ LSB} = 0.1875\text{ mV} \approx 0.00938\text{ kPa} \approx \mathbf{0.95\text{ mm}}\text{ of water column!}$$
* **Firmware Configuration in `config.h`:**
  ```c
  #define USE_EXTERNAL_ADS1115    true
  #define PIN_ADS_SDA             21
  #define PIN_ADS_SCL             22
  #define ADS1115_I2C_ADDR        0x48
  #define ADS1115_PRESSURE_CH     0
  #define ADS1115_DIRECT_5V       true   // Direct 5V reading (zero resistors)
  #define DEFAULT_TANK_DEPTH_CM   122.0f // Overhead tank usable water depth
  ```

---

### 1.4 Optional Appendix: Legacy Internal ADC with Voltage Divider
*(Only applicable if you do NOT have an external ADC and must feed raw analog signals directly into the ESP32 3.3V GPIO 34).*
* $R_1 = 33\text{ k}\Omega$ (between sensor Yellow and GPIO 34).
* $R_2 = 10\text{ k}\Omega$ (between GPIO 34 and GND).
* Ratio: $\approx 0.23256$ (steps down $4.5\text{V}$ to $1.05\text{V}$ to protect the $3.3\text{V}$ ESP32 pin).
* *Note: When using ADS1115 (Section 1.1), this entire circuit is omitted!*

---

## 2. Node 2: Overhead Tank Ultrasonic Sensor Node (Ground Reality)

### 2.1 Sensor Selection & Geometry
* **Sensor:** Waterproof **JSN-SR04T / AJ-SR04M** or **HC-SR04**.
* **Operating Voltage:** $5\text{V}$ DC (from ESP32 `VIN`).
* **Tank Usable Height:** **`122.0 cm`**
* **Tank Full Reference Offset:** **`6.0 cm`** (air gap from sensor face to water surface when tank is 100% full).
* **Physical Level Calculation:**
  $$\text{Water Height (cm)} = 122.0 - (\text{Distance}_{\text{measured}} - 6.0)$$
  $$\text{Level (\%)} = \left(1 - \frac{\text{Distance}_{\text{measured}} - 6.0}{122.0}\right) \times 100\%$$

### 2.2 Echo Pin Protection
When powered with $5\text{V}$, ultrasonic sensors output an `ECHO` signal of $5\text{V}$. A simple 2-resistor divider ($1\text{ k}\Omega / 2\text{ k}\Omega$) steps down the ECHO pulse to $\approx 3.3\text{V}$ for ESP32 GPIO 18.

```text
  +-------------------------------------------------------+
  |        JSN-SR04T / HC-SR04 Ultrasonic Sensor          |
  +--------+-------------+----------------+---------------+
           |             |                |               |
         [VCC]         [GND]            [TRIG]          [ECHO]
           |             |                |               |
           |             |                |         1k Ohm (R3)
           |             |                |         +---[===]---+
           |             |                |         |           |
           |             |                |         |           v
           |             |                |         |      +---------+
           |             |                |         +----> | GPIO 18 | (ESP32 Echo In)
           |             |                |         |      +---------+
           |             |                |    2k Ohm (R4)
           |             |                |    +---[===]---+
           |             |                |    |           |
           |             |                |    v           |
           |             +----------------+----+---------> [ GND ]
           |                              |
           |                              +--------------> [ GPIO 5 ]  (ESP32 Trig Out)
           +---------------------------------------------> [ VIN (5V) ]
                                                            ESP32 DEV MODULE
```

### 2.3 Pinout Summary (Node 2)
| Ultrasonic Sensor Pin | Resistor Network | ESP32 Dev Board Pin |
| :--- | :--- | :--- |
| **VCC** | None | **VIN (5V)** |
| **GND** | Common ground | **GND** |
| **TRIG** | Direct connection | **GPIO 5** |
| **ECHO** | Through $1\text{ k}\Omega / 2\text{ k}\Omega$ divider | **GPIO 18** |
