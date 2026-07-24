# Energy_Monitor — Selene MQTT + OTA (ESP32 DevKit V1)

This folder is the **edge firmware** for the [Selene](https://github.com/dankehidayat/Selene) dashboard (MQTT telemetry + remote OTA).

> **Branch:** `feat/selene-mqtt-ota`  
> **`main`** stays reserved for the final-report / Eco Office documentation and the original sketch.

## Hardware

| Part | Role |
|------|------|
| ESP32 DevKit V1 | MCU |
| PZEM-004T | AC voltage/current/power |
| DHT11 | Temperature / humidity |
| LCD 1602 I2C (`0x27`) | Local display |

## Configure before flash

Edit `Energy_Monitor.ino` and set:

```cpp
#define MQTT_BROKER   ""   // e.g. your VPS public IP or hostname
#define MQTT_PORT     1883
#define MQTT_USER     ""   // EMQX username
#define MQTT_PASSWORD ""   // EMQX password
#define NODE_ID       "office-main"  // unique per device

#define SELENE_API_BASE "https://YOUR_DOMAIN/api"  // for OTA result reporting

char auth[] = "";          // Blynk auth token (optional)
#define BLYNK_TEMPLATE_ID ""
// Blynk.config(auth, "YOUR_BLYNK_HOST", 8080);

#define AP_SSID "EcoOffice"
#define AP_PASS ""         // WiFiManager portal password
```

**Do not commit real tokens or passwords.**

## Arduino IDE (ESP32 DevKit V1)

| Setting | Value |
|---------|--------|
| Board | ESP32 Dev Module |
| Flash Size | 4MB |
| **Partition Scheme** | Default 4MB with spiffs (**must include OTA**) |
| Upload Speed | 921600 (or 115200) |

Libraries: Blynk, LiquidCrystal I2C, WiFiManager, PZEM004Tv30, DHT sensor library, PubSubClient, ArduinoJson v6.

## MQTT topics

| Topic | Direction | Purpose |
|-------|-----------|---------|
| `selene/<NODE_ID>/telemetry` | device → broker | Sensor JSON |
| `selene/<NODE_ID>/command` | broker → device | `reboot`, `status`, `ota` |
| `selene/<NODE_ID>/status` | device → broker | online / LWT offline |

### OTA command (from Selene Admin)

```json
{
  "command": "ota",
  "url": "https://YOUR_DOMAIN/api/firmware/download/office-main",
  "size": 1086187
}
```

Device downloads via HTTPS (`HTTPUpdate`) and reboots. Optional result:

`POST /api/firmware/result` `{ "nodeId", "success", "error?" }`

## Workflow

1. Fill secrets locally (never push them).
2. **USB upload once** with this sketch (OTA-capable firmware).
3. Confirm Serial @ 115200: WiFi + MQTT connected + subscribed to `/command`.
4. Later updates: export `.bin` → Selene **Admin Tools → Firmware** → target `NODE_ID`.

## Relation to `main`

| Branch | Content |
|--------|---------|
| `main` | Final report write-up + original Eco Office sketch |
| `feat/selene-mqtt-ota` | Selene MQTT/OTA firmware (`Energy_Monitor/`) |

See also: [Selene](https://github.com/dankehidayat/Selene) backend/frontend (modular monorepo).
