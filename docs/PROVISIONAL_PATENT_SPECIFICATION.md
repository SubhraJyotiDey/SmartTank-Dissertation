# PROVISIONAL PATENT SPECIFICATION & APPLICATION BLUEPRINT

---

## TITLE OF THE INVENTION
**SYSTEM AND METHOD FOR NON-INVASIVE INLINE HYDROSTATIC OVERHEAD TANK WATER LEVEL ESTIMATION WITH DYNAMIC FLOW COMPENSATION AND TRANSIENT WATER HAMMER REJECTION**

---

## INVENTORS
* **Name:** [Primary Student / Researcher Name]  
* **Affiliation / Address:** [University / Department / Residential Address]  
* **Co-Inventors (if applicable):** [Supervisor / Co-Researchers]

---

## 1. FIELD OF THE INVENTION
The present invention relates generally to IoT-based fluid monitoring and smart plumbing systems. More specifically, the invention relates to non-invasive water level measurement in overhead water storage tanks using inline pressure measurements taken at accessible residential tap fixtures, coupled with real-time derivative-based hydrodynamic compensation and water hammer transient suppression algorithms.

---

## 2. BACKGROUND OF THE INVENTION & PRIOR ART LIMITATIONS
Conventional residential and commercial building overhead water storage tank monitoring systems rely heavily on sensors installed directly inside or directly above the rooftop storage vessel:
1. **Ultrasonic / ToF Ranging Sensors:** Mounted atop the tank rim. These suffer from surface condensation, biological encrustation, acoustic reflection artifacts from internal tank ribbing, and require running high-voltage or complex outdoor wiring across roofs exposed to rain, sunlight, and extreme temperatures.
2. **Submersible Hydrostatic Pressure Transducers:** Suspended to the bottom of the tank. These require continuous contact with water, making them susceptible to algae buildup, mineral sediment fouling, cable degradation, and contamination of potable water.
3. **Mechanical Float Switches:** Prone to mechanical jamming, corrosion, and binary (on/off) resolution without continuous fractional tracking.

### The Unmet Need & Prior Art Failure
Attempts to measure head pressure at low-elevation taps have historically failed in practical residential settings because:
* **Friction & Kinetic Pressure Drops (Bernoulli Effect):** When a tap, shower, or appliance is operated, the measured pressure drops precipitously ($\Delta P_{\text{dynamic}}$) due to fluid velocity and pipe friction. Prior systems register this as an immediate tank emptying, triggering erroneous alerts.
* **Hydraulic Water Hammer Oscillations:** Sudden valve or tap closures induce high-pressure acoustic shockwaves and oscillatory transients that trigger false high/overflow alarms.
* **Unknown Building Pipe Geometry:** Prior mathematical systems require knowing the exact physical vertical height ($h_{\text{elevation}}$), pipe length ($L$), and friction factor ($f$), which are virtually impossible to obtain in finished concrete buildings.

---

## 3. SUMMARY & NOVELTY OF THE INVENTION
The present invention provides an autonomous, retrofittable inline IoT device and algorithmic method that can be attached to any standard plumbing fixture (e.g., washbasin tap, garden bib, washing machine line). 

Key novel aspects include:
1. **Derivative-Driven State Machine ($\frac{dP}{dt}$):** Continuously monitors the first-order rate of pressure change to differentiate between genuine storage volume loss and abrupt hydrodynamic tap discharge.
2. **Baseline Freezing & Dynamic Decoupling:** Instantaneously freezes reported water level to the last confirmed static hydrostatic baseline upon detecting flow initiation ($\frac{dP}{dt} < -\text{Threshold}$), suppressing false "empty" states.
3. **Water Hammer Rejection Buffer:** Detects closure-induced rebound spikes and institutes a non-blocking settling window to damp oscillatory waves before accepting new static equilibria.
4. **Dimensionless Two-Point Hydrostatic Calibration:** Fully eliminates the need for pipe length, riser elevation, or plumbing layout measurements by mathematically canceling elevation through calibration at empty ($P_{\text{empty}}$) and full ($P_{\text{full}}$) states:
$$\text{Level \%} = \frac{P_{\text{static}}(t) - P_{\text{empty}}}{P_{\text{full}} - P_{\text{empty}}} \times 100$$
5. **Fail-Safe Offline-to-Cloud Dual Queuing:** High-resolution dynamic event buffering (10 Hz) and long-term interval logging (1-minute) inside non-volatile Flash SPIFFS with automatic bulk recovery when Wi-Fi connectivity returns.

---

## 4. DETAILED DESCRIPTION OF THE PREFERRED EMBODIMENT

### 4.1 System Architecture & Hardware Setup
The system comprises:
* **A Piezoresistive Pressure Transducer:** Threaded inline onto a residential water distribution pipe or tap outlet via a standard T-junction adapter (range: $0 - 0.5 \text{ MPa}$ or $0 - 50 \text{ psi}$).
* **Precision ADC Frontend:** A 16-bit Delta-Sigma Analog-to-Digital Converter (ADS1115) connected via $I^2C$ providing micro-volt level resolution.
* **Microcontroller Unit (MCU):** An ESP32 dual-core processor operating at 240 MHz.
* **Secondary Verification Node (Optional Ground Truth):** An outdoor ultrasonic transceiver (JSN-SR04T) mounted on the overhead tank communicating wirelessly over local REST API / MQTT for real-time validation.

```
+-------------------------------------------------------------+
|                     OVERHEAD WATER TANK                     |
+-------------------------------------------------------------+
                              |
                     Riser Pipe Column (h_elevation)
                              |
+-------------------------------------------------------------+
|                      RESIDENTIAL TAP                        |
|                                                             |
|   [Inline Pressure Transducer] -> [ADS1115 16-bit ADC]      |
|                                         |                   |
|                                 [ESP32 MCU Node]            |
|                                         |                   |
|                        +----------------+---------------+   |
|                        |                                |   |
|               [Flash SPIFFS Queue]              [Wi-Fi / MQTT]
+-------------------------------------------------------------+
```

---

### 4.2 Algorithmic State Machine Execution

The core firmware operates under a finite state machine:

#### A. Static Hydrostatic State (`STATE_STATIC`)
* Continuous oversampling (e.g., 40 samples per cycle) passed through an Exponential Moving Average (EMA, filter coefficient $\alpha = 0.15$).
* Calculates instantaneous gauge head:
$$P_{\text{static}} = \rho g \left( h_{\text{elevation}} + H_{\text{water}}(t) \right)$$
* Evaluates numerical derivative $\frac{dP}{dt}$ across rolling 100 ms windows.

#### B. Dynamic Discharge State (`STATE_TAP_OPEN`)
* If $\frac{dP}{dt} < -1.2 \text{ kPa/s}$, a dynamic discharge event is recognized.
* The system locks the output level reading to $P_{\text{last\_static}}$.
* All downstream alerts and cloud telemetry flag the active state as `TAP_OPEN`, preventing false low-level warnings.

#### C. Transient Settling State (`STATE_SETTLING`)
* When the tap valve abruptly shuts, $\frac{dP}{dt} > +1.0 \text{ kPa/s}$.
* The system engages a 3.0-second non-blocking settling window.
* During this window, transient oscillatory pressure waves (water hammer) exceeding static head by up to $150\%$ are isolated, logged for pipe health diagnostics, and filtered out of the tank volume estimate.

---

## 5. PATENT CLAIMS (PROVISIONAL SKELETON)

### What is claimed is:

1. **A system for monitoring fluid level in an overhead liquid storage vessel from an inline plumbing fixture, comprising:**
   * a pressure sensor operatively coupled to an inline fluid pipe downstream of said overhead vessel;
   * a processing device configured to receive pressure signals from said sensor;
   * a memory storing algorithmic instructions executable by said processor to:
     * continuously sample fluid pressure at a predetermined sampling frequency;
     * compute a real-time rate of pressure change ($\frac{dP}{dt}$);
     * classify the plumbing system into a static equilibrium state, a dynamic discharge state, or an oscillatory settling state based on said rate of pressure change; and
     * compute an estimated liquid level of said overhead vessel based on a static baseline while dynamically suppressing level estimation errors during said dynamic discharge and oscillatory settling states.

2. **The system of claim 1,** wherein said processor is configured to freeze the reported liquid level to a pre-flow static baseline immediately upon detecting that $\frac{dP}{dt}$ falls below a negative flow threshold, maintaining said frozen reading until pressure recovery occurs.

3. **The system of claim 1,** wherein said processor is configured to detect valve closure by a positive rate of change in pressure and initiate a predetermined time-settling window that suppresses water hammer pressure oscillations from altering the calculated liquid level.

4. **The system of claim 1,** wherein the liquid level is calculated using a two-point calibration formula that establishes an empty reference pressure ($P_{\text{empty}}$) and a full reference pressure ($P_{\text{full}}$), thereby eliminating dependency on the physical elevation or distance of the overhead vessel.

5. **A method for estimating water level in a gravity-fed tank using an inline tap sensor, the method comprising:**
   * acquiring raw pressure values from an inline sensor;
   * filtering said values using exponential moving averages;
   * determining whether fluid flow is active using first-order pressure derivative comparisons;
   * isolating transient water hammer spikes following flow cessation; and
   * transmitting settled hydrostatic data to a remote monitoring platform.

---

## 6. ABSTRACT
An IoT-based system and method for non-invasively determining the liquid level of an overhead storage tank using an inline pressure sensor positioned at an accessible household tap or plumbing branch. The system utilizes real-time rate-of-change pressure derivatives ($\frac{dP}{dt}$) within a finite state machine to detect fluid discharge, freeze liquid level telemetry during flow, and reject high-pressure water hammer shockwaves during sudden valve closures. A two-point calibration protocol eliminates the need to measure vertical pipe elevation, enabling plug-and-play installation without rooftop sensor mounting or tank penetration.
