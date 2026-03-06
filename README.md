<div align="center">

# Smart Office Guardian - Energy & Environment Monitoring IoT System

</div>

## Table of Contents

1. [Overview](#overview)
2. [Features](#features)
3. [Hardware Components](#hardware-components)
4. [Sensor Calibration Methodology](#sensor-calibration-methodology)
5. [Fuzzy Mamdani Logic System](#fuzzy-mamdani-logic-system)
   - [Thermal Comfort Classification](#1-thermal-comfort-classification-v10)
   - [Energy Consumption Classification](#2-energy-consumption-classification-v11)
6. [LCD Display System](#lcd-display-system)
7. [Blynk Virtual Pin Mapping](#blynk-virtual-pin-mapping)
8. [Installation and Setup](#installation-and-setup)
9. [System Operation](#system-operation)
10. [Technical Specifications](#technical-specifications)
11. [Google Sheets Integration](#google-sheets-integration)
12. [Applications](#applications)
13. [Future Enhancements](#future-enhancements)
14. [License](#license)
15. [Author](#author)

---

## Overview

An IoT monitoring system for tracking electrical energy consumption and environmental conditions using an ESP32 microcontroller. The system integrates a PZEM-004T power monitoring module and a DHT11 temperature/humidity sensor, enhanced with linear regression calibration and a dual Fuzzy Mamdani inference engine for smart office classification.

<div align="center">

| Blynk Dashboard | LCD Display |
|:------------:|:--------------:|
| <img width="240" height="533" src="https://github.com/user-attachments/assets/912dc2a3-61f9-486d-ab43-2b00a42f24fa"> | <img width="240" height="533" src="https://github.com/user-attachments/assets/38b05b3e-0596-4843-8f7a-c68e2ff991aa"> |
| **Figure 1**: Blynk Dashboard | **Figure 2**: LCD Display Interface |

</div>

---

## Features

- **Real-time Power Monitoring**: Voltage, current, active power, energy, frequency, power factor, apparent power, and reactive power
- **Environmental Sensing**: Temperature and humidity monitoring with HTC-1 reference calibration achieving 95.8%–97.7% accuracy
- **Fuzzy Mamdani Intelligence**: Dual fuzzy inference systems for thermal comfort and energy consumption classification
- **IoT Connectivity**: Blynk platform integration with Google Sheets data logging
- **LCD Display**: 5-mode rotating display showing power metrics, environmental data, and fuzzy classification results
- **WiFi Manager**: On-device network configuration via "EcoOffice" access point
- **Data Logging**: Serial monitoring with real-time calibration accuracy analysis

---

## Hardware Components

<div align="center">

| |
|:-------------------------:|
| <img width="480" height="720" alt="Wiring Schematic" src="https://github.com/user-attachments/assets/2fa6e24f-227e-4ad9-a243-f8f93257cdf7"> |
| **Figure 3**: Hardware system monitoring connection schematic diagram |

</div>

- ESP32 Development Board
- PZEM-004T Power Monitoring Module
- DHT11 Temperature/Humidity Sensor
- HTC-1 Reference Thermometer/Hygrometer (used for calibration)
- 16x2 I2C LCD Display (Address: 0x27)
- Breadboard and Jumper Wires

---

## Sensor Calibration Methodology

### Linear Regression Analysis

Calibration uses linear regression based on 34 paired measurement points between the DHT11 and an HTC-1 reference device.

#### Temperature Calibration (R² = 0.958, Accuracy: 95.8%)

```
HTC-1 = 0.923 × DHT11 - 1.618
```

- **Slope (0.923)**: Rate-of-change correction between DHT11 and reference
- **Intercept (-1.618)**: Systematic offset correction at zero
- **R² = 0.958**: Model explains 95.8% of variance in the reference data

#### Humidity Calibration (R² = 0.977, Accuracy: 97.7%)

```
HTC-1 = 0.926 × DHT11 + 18.052
```

- **Slope (0.926)**: Fine-tuned humidity scaling factor
- **Intercept (+18.052)**: Significant baseline upward correction (DHT11 reads consistently lower)
- **R² = 0.977**: Model explains 97.7% of variance in the reference data

### Alternative Bias-Based Calibration

A simpler constant-offset method is also implemented for comparison:

- Temperature: `corrected = raw - 3.84°C`
- Humidity: `corrected = raw + 14.18%`

The regression model is the primary method used for all Blynk and fuzzy outputs.

### Real-Time Error Monitoring

A circular buffer stores the 10 most recent absolute differences between the two calibration methods. Mean Absolute Error (MAE) and accuracy percentage are computed on each reading cycle for continuous self-assessment.

```
Accuracy (%) = max(0, 100 - (MAE / Range × 100))
```

Where range is 50°C for temperature and 100% for humidity.

#### Calibration Performance Summary

| Parameter | Raw DHT11 Error | Post-Calibration Error | Accuracy |
|-----------|-----------------|------------------------|----------|
| Temperature | ~4.1°C | ~0.42°C | 95.8% |
| Humidity | ~13.8% | ~2.87% | 97.7% |

---

## Fuzzy Mamdani Logic System

The system implements a **Fuzzy Mamdani** inference method. Each fuzzy system follows the standard four-stage pipeline: fuzzification, rule evaluation (using MIN for AND, MAX for OR), aggregation, and defuzzification via maximum membership (max-crisp).

### 1. Thermal Comfort Classification (V10)

Based on ASHRAE 55 and ISO 7730 thermal comfort standards.

#### Input Membership Functions

**Temperature (°C) — 4 sets:**

| Linguistic Variable | Type | Range |
|---|---|---|
| COLD | Trapezoidal | <= 18 (full), fades out at 22 |
| COMFORTABLE | Triangular | 20 – 23 – 26 |
| WARM | Triangular | 24 – 27 – 30 |
| HOT | Trapezoidal | rises from 26, full at >= 28 |

**Humidity (%) — 3 sets:**

| Linguistic Variable | Type | Range |
|---|---|---|
| DRY | Trapezoidal | <= 30 (full), fades out at 40 |
| COMFORTABLE | Triangular | 30 – 50 – 70 |
| HUMID | Trapezoidal | rises from 50, full at >= 60 |

#### Output Categories

`COLD` | `COOL` | `COMFORTABLE` | `WARM` | `HOT`

#### Rule Base (8 Rules)

| Rule | Condition | Output |
|------|-----------|--------|
| R1 | IF Temperature is COLD | COLD |
| R2 | IF Temperature is COMFORTABLE AND Humidity is COMFORTABLE | COMFORTABLE |
| R3 | IF Temperature is COMFORTABLE AND Humidity is DRY | COOL |
| R4 | IF Temperature is COMFORTABLE AND Humidity is HUMID | WARM |
| R5 | IF Temperature is WARM | WARM |
| R6 | IF Temperature is HOT | HOT |
| R7 | IF Temperature is COLD AND Humidity is HUMID | COOL |
| R8 | IF Temperature is HOT AND Humidity is HUMID | HOT |

---

### 2. Energy Consumption Classification (V11)

Designed and tuned for **small office electrical loads in the range of 0–150W**, consistent with equipment such as laptops, monitors, LED lighting, and phone chargers. This system replaces a prior version that was scaled for larger loads (200–1500W) and is incompatible with this environment.

The output is sent to Blynk V11 as a **numeric integer** (1, 2, or 3) for compatibility with Google Sheets processing.

| Numeric | Category |
|---------|----------|
| 1 | ECONOMICAL |
| 2 | NORMAL |
| 3 | WASTEFUL |

#### Input Membership Functions

**Voltage (V) — 3 sets:**

| Linguistic Variable | Type | Range |
|---|---|---|
| LOW | Trapezoidal | <= 200 (full), fades out at 210 |
| NORMAL | Triangular | 205 – 220 – 235 |
| HIGH | Trapezoidal | rises from 230, full at >= 235 |

**Active Power (W) — 3 sets (tuned for 0–150W office load):**

| Linguistic Variable | Type | Range |
|---|---|---|
| ECONOMICAL | Trapezoidal | <= 20 (full), fades out at 30 |
| NORMAL | Triangular | 25 – 47.5 – 70 |
| WASTEFUL | Trapezoidal | rises from 60, full at >= 80 |

**Power Factor — 3 sets:**

| Linguistic Variable | Type | Range |
|---|---|---|
| POOR | Trapezoidal | <= 0.5 (full), fades out at 0.6 |
| FAIR | Triangular | 0.55 – 0.70 – 0.85 |
| GOOD | Trapezoidal | rises from 0.80, full at >= 0.90 |

**Reactive Power (VAR) — 3 sets:**

| Linguistic Variable | Type | Range |
|---|---|---|
| LOW | Trapezoidal | <= 15 (full), fades out at 25 |
| MEDIUM | Triangular | 20 – 37.5 – 55 |
| HIGH | Trapezoidal | rises from 45, full at >= 60 |

#### Output Categories

`ECONOMICAL` | `NORMAL` | `WASTEFUL`

#### Rule Base (15 Rules)

**Group 1 — ECONOMICAL (4 rules):**

| Rule | Condition | Output |
|------|-----------|--------|
| R1 | IF Power is ECONOMICAL AND Power Factor is GOOD | ECONOMICAL |
| R2 | IF Power is ECONOMICAL AND Reactive Power is LOW | ECONOMICAL |
| R3 | IF Power is ECONOMICAL AND Voltage is NORMAL | ECONOMICAL |
| R4 | IF Power Factor is GOOD AND Reactive Power is LOW | ECONOMICAL |

**Group 2 — NORMAL (5 rules):**

| Rule | Condition | Output |
|------|-----------|--------|
| R5 | IF Power is NORMAL AND Power Factor is FAIR | NORMAL |
| R6 | IF Power is NORMAL AND Voltage is NORMAL | NORMAL |
| R7 | IF Power is NORMAL AND Reactive Power is MEDIUM | NORMAL |
| R8 | IF Power Factor is FAIR AND Voltage is NORMAL | NORMAL |
| R9 | IF Power is ECONOMICAL AND Power Factor is POOR | NORMAL (compensated) |

**Group 3 — WASTEFUL (6 rules):**

| Rule | Condition | Output |
|------|-----------|--------|
| R10 | IF Power is WASTEFUL | WASTEFUL |
| R11 | IF Power Factor is POOR | WASTEFUL |
| R12 | IF Reactive Power is HIGH | WASTEFUL |
| R13 | IF Voltage is LOW OR Voltage is HIGH | WASTEFUL |
| R14 | IF Power is NORMAL AND Power Factor is POOR | WASTEFUL |
| R15 | IF Power is WASTEFUL AND Reactive Power is HIGH | WASTEFUL |

#### Defuzzification

Maximum membership (max-crisp) method: the output category with the highest aggregated firing strength is selected as the final classification.

---

## LCD Display System

The LCD cycles through 5 display modes, updating every 3 seconds.

```
Mode 0: Voltage (V)     | Current (A)
Mode 1: Power (W)       | Frequency (Hz)
Mode 2: Energy (Wh)     | Power Factor
Mode 3: Temperature (C) | Humidity (%)
Mode 4: Comfort status  | Energy status (numeric)
```

---

## Blynk Virtual Pin Mapping

| Virtual Pin | Data Type | Description |
|-------------|-----------|-------------|
| V0 | Float | Voltage (V) |
| V1 | Float | Current (A) |
| V2 | Float | Active Power (W) |
| V3 | Float | Power Factor |
| V4 | Float | Apparent Power (VA) |
| V5 | Float | Energy (Wh) |
| V6 | Float | Frequency (Hz) |
| V7 | Float | Reactive Power (VAR) |
| V8 | Float | Calibrated Temperature (°C) |
| V9 | Float | Calibrated Humidity (%) |
| V10 | String | Thermal Comfort Status (e.g. "COMFORTABLE") |
| V11 | Integer | Energy Consumption Status (1=ECONOMICAL, 2=NORMAL, 3=WASTEFUL) |

---

## Installation and Setup

### Prerequisites

- Arduino IDE with ESP32 board support installed
- Required libraries:
  - `Blynk` — IoT platform integration
  - `LiquidCrystal_I2C` — LCD display control
  - `WiFiManager` — On-device network configuration
  - `PZEM004Tv30` — Power monitoring
  - `DHT sensor library` — Environmental sensing

### Hardware Pin Configuration

```
PZEM-004T : RX = GPIO16, TX = GPIO17 (HardwareSerial UART1)
DHT11     : GPIO27
LCD       : I2C Address 0x27
Boot Btn  : GPIO0 (hold 5s to reset WiFi credentials)
```

### WiFi Configuration

```
AP SSID   : EcoOffice
AP Pass   : guard14n0ff1ce
Timeout   : 60 seconds
```

### Blynk Setup

1. Template ID: `TMPL6eUbLFTuj` — "Energy Monitor"
2. Local server: `iot.serangkota.go.id`, port `8080`
3. Configure virtual pins V0–V11 in the Blynk dashboard

---

## System Operation

### Startup Sequence

1. LCD initialization with EcoOffice branding
2. Boot button check (hold GPIO0 for 5 seconds to wipe WiFi credentials)
3. WiFi connection via WiFiManager with LCD blink feedback
4. IP address display on successful connection
5. Blynk connection to local server
6. PZEM-004T and DHT11 sensor initialization
7. Continuous monitoring begins at 3-second intervals

### Runtime Behavior

- Sensor readings every 3 seconds
- LCD rotates through 5 display modes each cycle
- Blynk receives updated values every reading
- Fuzzy Mamdani classification runs on every reading cycle (V10, V11)
- Serial monitor logs full sensor and classification data every 18 seconds (every 6 readings)

---

## Technical Specifications

### System Performance

| Parameter | Value |
|-----------|-------|
| Sensor Reading Interval | 3 seconds |
| LCD Mode Rotation | 5 modes x 3 seconds |
| Blynk Update Rate | Every reading (3s) |
| Serial Log Interval | Every 18 seconds |
| WiFi Portal Timeout | 60 seconds |
| Calibration Buffer Size | 10 readings (circular) |

### Fuzzy System Summary

| System | Input Variables | Rule Count | Output Categories |
|--------|----------------|------------|-------------------|
| Thermal Comfort | Temperature, Humidity | 8 | COLD, COOL, COMFORTABLE, WARM, HOT |
| Energy Consumption | Voltage, Power, Power Factor, Reactive Power | 15 | ECONOMICAL (1), NORMAL (2), WASTEFUL (3) |

---

## Google Sheets Integration

### App Script Features

- Automatic data logging from Blynk virtual pins V0–V11
- Fuzzy classification capture: V10 (string) and V11 (numeric integer)
- Derived calculations:
  - Current per kW analysis
  - Power Quality Score (0–100)
  - Energy cost estimation (Rp/kWh)
  - Voltage stability percentage

### Data Access

<div align="center">

**Scan for Mobile Access:**

<img width="128" height="128" alt="QR Code" src="https://github.com/user-attachments/assets/7a710f50-6f1c-44da-a7c9-cdb8f2d4445a" />

[Live Monitoring Dashboard](https://ipb.link/energy-temperature-monitoring-data)

</div>

---

## Applications

- Smart office optimization — thermal comfort and energy efficiency
- HVAC system management — environmental condition tracking
- Energy consumption analytics — Fuzzy Mamdani-based classification
- IoT research platform — sensor calibration and fuzzy inference studies
- Building management systems — real-time monitoring

---

## Future Enhancements

- Machine learning integration for predictive maintenance
- Multi-zone monitoring with expanded sensor networks
- ~~Mobile application development for enhanced UI~~
- Cloud analytics for advanced data processing
- Automated scheduled reporting

---

## License

This project is licensed under the [UNLICENSE](https://github.com/dankehidayat/Eco-Office/blob/master/UNLICENSE).

---

## Author

**Danke Hidayat** — IoT & Embedded Systems Developer
Specializing in smart office solutions and sensor fusion technologies

---

**Last updated**: March 2026

Also see my own app called [flowpoint](https://github.com/dankehidayat/flowpoint)

*For real-time sensor data and performance metrics, [access the live dashboard](https://ipb.link/energy-temperature-monitoring-data).*
