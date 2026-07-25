# Eco Office — Energy & Environment Monitor (Selene)

**Branch:** `feat/selene-mqtt-ota`  

ESP32 firmware for the **[Selene](https://github.com/dankehidayat/Selene)** smart energy & climate dashboard.  
Monitors **electrical energy** (PZEM-004T) and **environment** (DHT11 temperature & humidity), publishes MQTT telemetry, and supports **HTTPS OTA** from the Selene Admin UI.

> **`main` is separate.** That branch is reserved for the final academic report and the original sketch.  
> This branch **replaces** root `Eco Office.ino` with the Selene-integrated firmware.  
> **Do not commit secrets** (MQTT passwords, Blynk tokens, WiFi portal passwords).

---

## Table of contents

1. [Overview](#overview)
2. [Hardware](#hardware)
3. [Repository layout](#repository-layout)
4. [Features](#features)
5. [Configuration](#configuration)
6. [MQTT protocol](#mqtt-protocol)
7. [Arduino IDE setup](#arduino-ide-setup)
8. [First flash (USB)](#first-flash-usb)
9. [OTA update via Selene](#ota-update-via-selene)
10. [Blynk (optional)](#blynk-optional)
11. [LCD display modes](#lcd-display-modes)
12. [Fuzzy classification](#fuzzy-classification)
13. [Troubleshooting](#troubleshooting)
14. [Related projects](#related-projects)
15. [License](#license)

---

## Overview

| Layer | Role |
|-------|------|
| **ESP32 DevKit V1** | Edge device |
| **PZEM-004T** | AC energy: voltage, current, power, PF, frequency, energy |
| **DHT11** | Environment: temperature, humidity (with calibration) |
| **EMQX (VPS)** | MQTT broker |
| **Selene backend** | Ingest, TimescaleDB, Admin OTA API |
| **Selene frontend** | Dashboard + Admin Tools → Firmware |

Data path:

```text
Sensors → ESP32 → MQTT selene/<NODE_ID>/telemetry → Selene backend → TimescaleDB → Dashboard
Selene Admin → POST /api/firmware/upload → MQTT selene/<NODE_ID>/command {ota} → ESP32 HTTPS download → flash → reboot
```

---

## Hardware

| Component | Connection / notes |
|-----------|-------------------|
| ESP32 DevKit V1 | 4MB flash recommended |
| PZEM-004T | UART1: RX=GPIO16, TX=GPIO17, 9600 8N1 |
| DHT11 | Data on **GPIO27** |
| LCD 1602 I2C | Address **0x27**, SDA/SCL default Wire |
| BOOT button (GPIO0) | Hold 5s at boot to reset WiFiManager settings |

---

## Repository layout

```text
Eco-Office/                    # this branch
├── Eco Office.ino             # full firmware (energy + environment + MQTT + OTA)
├── README.md                  # this file
└── LICENSE
```

On **`main`**: original report materials and the pre-Selene sketch (unchanged by this branch’s intent).

---

## Features

- **Energy monitoring** — voltage, current, active/apparent/reactive power, PF, frequency, cumulative energy (**kWh** from `PZEM004Tv30::energy()`)  
- **Environment monitoring** — temperature & humidity with linear-regression calibration  
- **Fuzzy Mamdani** — thermal comfort (COLD…HOT) and energy class (ECONOMICAL / NORMAL / WASTEFUL)  
- **LCD** — 5 rotating screens (power + climate + fuzzy labels)  
- **WiFiManager** — captive portal if no saved WiFi  
- **MQTT** — telemetry publish + command subscribe (`reboot`, `status`, `ota`)  
- **HTTPS OTA** — `HTTPUpdate` from Selene Admin `.bin` deploy  
- **Optional Blynk** — virtual pins for live widgets (credentials blank in git)

---

## Configuration

Open **`Eco Office.ino`** and fill placeholders **locally** (never push real values):

```cpp
// MQTT / Selene broker
#define MQTT_BROKER   ""      // e.g. "198.x.x.x" or hostname
#define MQTT_PORT     1883
#define MQTT_USER     ""      // EMQX username
#define MQTT_PASSWORD ""
#define NODE_ID       "office-main"   // unique per device

// OTA result callback base (no trailing slash after /api)
#define SELENE_API_BASE "https://YOUR_DOMAIN/api"

// Blynk (optional)
#define BLYNK_TEMPLATE_ID ""
#define BLYNK_TEMPLATE_NAME "Eco Office"
char auth[] = "";
// Blynk.config(auth, "YOUR_BLYNK_HOST", 8080);

// WiFiManager portal
#define AP_SSID "EcoOffice"
#define AP_PASS ""            // portal password when configuring WiFi
```

| Placeholder | Purpose |
|-------------|---------|
| `MQTT_*` | Broker used by Selene / EMQX |
| `NODE_ID` | Must match Admin “Target node” (e.g. `office-main`) |
| `SELENE_API_BASE` | Host for `POST .../firmware/result` |
| `auth` / Blynk host | Optional cloud widgets |
| `AP_PASS` | WiFi setup portal |

---

## MQTT protocol

### Telemetry (device → broker)

- **Topic:** `selene/<NODE_ID>/telemetry`  
- **Interval:** 30 s (default)  
- **Payload (JSON):**

```json
{
  "voltage": 220.5,
  "current": 0.5,
  "power": 100.0,
  "pf": 0.9,
  "energy": 12.3,
  "frequency": 50.0,
  "apparentPower": 111.1,
  "reactivePower": 48.0,
  "temperature": 27.5,
  "humidity": 60.0
}
```

> **`energy` is kWh** (not Wh). The PZEM-004T raw register is Wh; the PZEM004Tv30 library divides by 1000 before returning. Selene stores this as `total_energy` in kWh.

### Status

- **Topic:** `selene/<NODE_ID>/status`  
- Online retain + Last Will offline  

### Commands (broker → device)

- **Topic:** `selene/<NODE_ID>/command`  
- Subscribe at QoS 1  

| `command` | Action |
|-----------|--------|
| `reboot` | `ESP.restart()` |
| `status` | Publish RSSI, uptime, free heap |
| `ota` | HTTPS download `url`, flash, report result, reboot |

Example OTA command (from Selene backend):

```json
{
  "command": "ota",
  "url": "https://YOUR_DOMAIN/api/firmware/download/office-main",
  "size": 1086187
}
```

---

## Arduino IDE setup

| Setting | Value |
|---------|--------|
| Board | **ESP32 Dev Module** (or DOIT ESP32 DEVKIT V1) |
| Flash Size | **4MB** |
| **Partition Scheme** | **Default 4MB with spiffs** (or any scheme **with OTA**) |
| Upload Speed | 921600 (or 115200 if unstable) |
| Port | USB serial of the DevKit |

### Libraries (Library Manager)

- Blynk  
- LiquidCrystal I2C  
- WiFiManager (tzapu)  
- PZEM004Tv30  
- DHT sensor library + Adafruit Unified Sensor  
- PubSubClient  
- ArduinoJson **v6**  

Built into ESP32 core: `WiFi`, `WiFiClientSecure`, `HTTPClient`, `HTTPUpdate`.

---

## First flash (USB)

OTA cannot install itself. Flash this sketch **once over USB**:

1. Clone / checkout this branch:
   ```bash
   git clone https://github.com/dankehidayat/Eco-Office.git
   cd Eco-Office
   git checkout feat/selene-mqtt-ota
   ```
2. Open **`Eco Office.ino`** (repo root) in Arduino IDE.  
3. Fill configuration placeholders.  
4. Select board + **OTA partition scheme**.  
5. **Sketch → Upload**.  
6. Serial Monitor **115200** — expect WiFi OK, MQTT connect, subscribe to `selene/<NODE_ID>/command`.

---

## OTA update via Selene

1. In Arduino: **Sketch → Export compiled Binary** (optional for later OTAs).  
2. Selene → log in as **ADMIN** → **Admin Tools → Firmware**.  
3. Target node = `NODE_ID` (e.g. `office-main`).  
4. Upload `Eco Office.ino.bin` (main app binary, magic `0xE9`).  
5. Serial / LCD should show:
   ```text
   MQTT: Perintah diterima [...]: {"command":"ota",...}
   OTA: scheduled
   OTA: starting HTTPS firmware update
   OTA: progress 10% ...
   OTA: SUCCESS, rebooting
   ```
   LCD: `OTA queued...` → `Downloading...` → `OTA Success`.
6. Device reboots into the new firmware. Selene may mark history **success** when the full binary is delivered.

**How OTA is delivered**

1. **MQTT push** (immediate): backend publishes `{ command:"ota", url, size }` to `selene/<NODE_ID>/command` and re-publishes every ~12s until download starts.  
2. **HTTP pull** (fallback): device GETs `/api/firmware/check/<NODE_ID>` about once a minute. If a binary is pending, it starts the same HTTPS download.

**Notes:**

- Admin upload is **not** a USB flash. The sketch already running must include the OTA handler (this branch, flashed once over USB).  
- Confirm EMQX shows the device online **and** that Serial responds to a status command before blaming the .bin.  
- Do not power-cycle mid-flash.  
- TLS uses `setInsecure()` for bring-up; pin a CA cert for production.  
- Sketch size is large (~1 MB); keep an OTA-capable partition table.  
- Export the **application** `.bin` (magic byte `0xE9`), not bootloader-only images.

---

## Blynk (optional)

Virtual pins used when Blynk is connected:

| Pin | Data |
|-----|------|
| V0–V7 | Voltage, current, power, PF, apparent, energy, frequency, reactive |
| V8–V9 | Temperature, humidity |
| V10–V11 | Fuzzy comfort label, energy class (1/2/3) |

Leave `auth` empty and Blynk host blank if unused; firmware continues with MQTT + LCD.

---

## LCD display modes

Rotates every sensor sample (~3 s):

0. Voltage / current  
1. Power / frequency  
2. Energy / power factor  
3. Temperature / humidity  
4. Fuzzy comfort / energy class  

---

## Fuzzy classification

- **Thermal comfort:** COLD, COOL, COMFORTABLE, WARM, HOT (temp + humidity membership)  
- **Energy:** ECONOMICAL, NORMAL, WASTEFUL (voltage, power, PF, reactive rules)  

Aligned with Selene backend analytics for consistent labels on device and dashboard.

---

## Troubleshooting

| Symptom | Check |
|---------|--------|
| No MQTT in EMQX | Broker IP, user/pass, `NODE_ID`, firewall :1883 |
| `bad_username_or_password` | Create EMQX user matching `MQTT_USER` / `MQTT_PASSWORD` |
| OTA command ignored | USB-flashed this branch? Serial shows “Perintah diterima”? Wrong `NODE_ID`? |
| EMQX connected but no OTA | MQTT push may be fine while HTTPS download fails. Watch Serial for `OTA: FAILED`. LCD shows error. |
| OTA fails HTTP | `SELENE_API_BASE` must be `https://selene.dankehidayat.my.id/api`. Device Wi-Fi must reach that host. |
| History stuck pending | Device never hit download URL. Check Serial for schedule; wrong node in Admin; firmware expired (~15 min). |
| OTA partition error | Tools → Partition Scheme → one **with OTA** |
| Admin target empty | Device must publish telemetry first so backend discovers the node |

---

## Related projects

| Project | Role |
|---------|------|
| [Selene](https://github.com/dankehidayat/Selene) | Cloud dashboard, API, Timescale, Admin OTA (`feat/modular-microservices`) |
| This repo `main` | Final report + original Eco Office documentation sketch |
| This repo `feat/selene-mqtt-ota` | Production-oriented edge firmware for Selene |

---

## License

See [LICENSE](./LICENSE).
