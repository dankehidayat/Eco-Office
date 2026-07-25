/*
 * Eco Office — Smart Energy & Environment Monitor
 * Board: ESP32 DevKit V1
 * Sensors:
 *   - PZEM-004T  → electrical (V, I, P, PF, f, energy in kWh)
 *     Note: PZEM004Tv30::energy() returns kWh (raw register is Wh / 1000).
 *     MQTT field "energy", Blynk V5, and LCD all use kWh — not Wh.
 *   - DHT11      → environment (temperature, humidity)
 * Features: Blynk, LCD, MQTT telemetry to Selene, MQTT commands (reboot/status/OTA)
 *
 * OTA: Selene Admin publishes to selene/<NODE_ID>/command
 *   { "command":"ota", "url":"https://YOUR_DOMAIN/api/firmware/download/<node>", "size":N }
 * Device downloads via HTTPS (HTTPUpdate) and reboots.
 *
 * Secrets: leave blank in git; fill locally before flash.
 */

#include <WiFi.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <HTTPUpdate.h>

#define BLYNK_TEMPLATE_ID ""
#define BLYNK_TEMPLATE_NAME "Eco Office"
#include <BlynkSimpleEsp32.h>
#include <LiquidCrystal_I2C.h>
#include <WiFiManager.h>
#include <PZEM004Tv30.h>
#include <DHT.h>
#include <Wire.h>
#include <math.h>

#include <PubSubClient.h>
#include <ArduinoJson.h>

// ── MQTT / Selene ─────────────────────────────────────────
#define MQTT_BROKER ""
#define MQTT_PORT 1883
#define MQTT_USER ""
#define MQTT_PASSWORD ""
#define NODE_ID "office-main"  // change per device
#define MQTT_PUBLISH_INTERVAL 30000

// Report OTA result to backend (no auth required on this route today)
#define SELENE_API_BASE "https://YOUR_DOMAIN/api"

// ── Hardware ──────────────────────────────────────────────
LiquidCrystal_I2C lcd(0x27, 16, 2);
HardwareSerial hwSerial(1);
PZEM004Tv30 pzem(hwSerial, 16, 17);

#define DHTPIN 27
#define DHTTYPE DHT11
DHT dht(DHTPIN, DHTTYPE);
#define TRIGGER_PIN 0

char auth[] = "";

WiFiClient mqttWiFiClient;
PubSubClient mqtt(mqttWiFiClient);

unsigned long lastMqttPublish = 0;

bool lcdBacklightState = true;
unsigned long previousBlinkMillis = 0;
const unsigned long BLINK_DELAY = 4500;
const unsigned long BLINK_INTERVAL = 250;

const float TEMP_SLOPE = 0.923;
const float TEMP_INTERCEPT = -1.618;
const float HUM_SLOPE = 0.926;
const float HUM_INTERCEPT = 18.052;
const float TEMP_BIAS = -3.84;
const float HUM_BIAS = +14.18;

float tempErrors[10];
float humErrors[10];
int errorIndex = 0;

// Flag: OTA should run outside mqtt callback (safer)
bool otaPending = false;
String otaUrl = "";
unsigned long lastOtaPullCheck = 0;
const unsigned long OTA_PULL_INTERVAL_MS = 60000;  // also poll server if MQTT push missed

float calibrateTemperature(float rawTemp) { return (TEMP_SLOPE * rawTemp) + TEMP_INTERCEPT; }
float calibrateHumidity(float rawHum) { return (HUM_SLOPE * rawHum) + HUM_INTERCEPT; }
float calibrateTemperatureSimple(float rawTemp) { return rawTemp + TEMP_BIAS; }
float calibrateHumiditySimple(float rawHum) { return rawHum + HUM_BIAS; }
void recordCalibrationError(float tempError, float humError) {
  tempErrors[errorIndex] = tempError;
  humErrors[errorIndex] = humError;
  errorIndex = (errorIndex + 1) % 10;
}
void getCurrentMAE(float &tempMAE, float &humMAE) {
  float tempSum = 0, humSum = 0;
  int count = 0;
  for (int i = 0; i < 10; i++) {
    if (tempErrors[i] != 0) {
      tempSum += abs(tempErrors[i]);
      humSum += abs(humErrors[i]);
      count++;
    }
  }
  tempMAE = count > 0 ? tempSum / count : 0;
  humMAE = count > 0 ? humSum / count : 0;
}
float calculateAccuracy(float mae, float range) {
  return max(0.0, 100.0 - (mae / range * 100.0));
}
float zeroIfNan(float value) { return isnan(value) ? 0.0 : value; }

String fuzzyTemperatureComfort(float temp, float humidity) {
  float cold = (temp <= 18) ? 1.0 : (temp <= 22) ? (22 - temp) / 4.0 : 0.0;
  float comfortable = (temp >= 20 && temp <= 23) ? (temp - 20) / 3.0
                      : (temp > 23 && temp <= 26) ? (26 - temp) / 3.0 : 0.0;
  float warm = (temp >= 24 && temp <= 27) ? (temp - 24) / 3.0
               : (temp > 27 && temp <= 30) ? (30 - temp) / 3.0 : 0.0;
  float hot = (temp >= 28) ? 1.0 : (temp >= 26) ? (temp - 26) / 2.0 : 0.0;
  float dry = (humidity <= 30) ? 1.0 : (humidity <= 40) ? (40 - humidity) / 10.0 : 0.0;
  float comfortable_hum = (humidity >= 30 && humidity <= 50) ? (humidity - 30) / 20.0
                          : (humidity > 50 && humidity <= 70) ? (70 - humidity) / 20.0 : 0.0;
  float humid = (humidity >= 60) ? 1.0 : (humidity >= 50) ? (humidity - 50) / 10.0 : 0.0;
  float cold_strength = cold;
  float cool_strength = max(min(comfortable, dry), min(cold, humid));
  float comfortable_strength = min(comfortable, comfortable_hum);
  float warm_strength = max(warm, min(comfortable, humid));
  float hot_strength = max(hot, min(hot, humid));
  float strengths[] = {cold_strength, cool_strength, comfortable_strength, warm_strength, hot_strength};
  String categories[] = {"COLD", "COOL", "COMFORTABLE", "WARM", "HOT"};
  int max_index = 0;
  for (int i = 1; i < 5; i++) {
    if (strengths[i] > strengths[max_index]) max_index = i;
  }
  return categories[max_index];
}

String fuzzyEnergyConsumption(float voltage, float power, float powerFactor, float reactivePower) {
  float voltage_low = (voltage <= 200) ? 1.0 : (voltage <= 210) ? (210 - voltage) / 10.0 : 0.0;
  float voltage_normal = (voltage >= 205 && voltage <= 220) ? (voltage - 205) / 15.0
                         : (voltage > 220 && voltage <= 235) ? (235 - voltage) / 15.0 : 0.0;
  float voltage_high = (voltage >= 235) ? 1.0 : (voltage >= 230) ? (voltage - 230) / 5.0 : 0.0;
  float power_economical = (power <= 20) ? 1.0 : (power <= 30) ? (30 - power) / 10.0 : 0.0;
  float power_normal = (power >= 25 && power <= 47.5) ? (power - 25) / 22.5
                       : (power > 47.5 && power <= 70) ? (70 - power) / 22.5 : 0.0;
  float power_wasteful = (power >= 80) ? 1.0 : (power >= 60) ? (power - 60) / 20.0 : 0.0;
  float pf_poor = (powerFactor <= 0.5) ? 1.0 : (powerFactor <= 0.6) ? (0.6 - powerFactor) / 0.1 : 0.0;
  float pf_fair = (powerFactor >= 0.55 && powerFactor <= 0.7) ? (powerFactor - 0.55) / 0.15
                  : (powerFactor > 0.7 && powerFactor <= 0.85) ? (0.85 - powerFactor) / 0.15 : 0.0;
  float pf_good = (powerFactor >= 0.90) ? 1.0 : (powerFactor >= 0.80) ? (powerFactor - 0.80) / 0.10 : 0.0;
  float reactive_low = (reactivePower <= 15) ? 1.0 : (reactivePower <= 25) ? (25 - reactivePower) / 10.0 : 0.0;
  float reactive_medium = (reactivePower >= 20 && reactivePower <= 37.5) ? (reactivePower - 20) / 17.5
                          : (reactivePower > 37.5 && reactivePower <= 55) ? (55 - reactivePower) / 17.5 : 0.0;
  float reactive_high = (reactivePower >= 60) ? 1.0 : (reactivePower >= 45) ? (reactivePower - 45) / 15.0 : 0.0;
  float economical_strength = 0.0;
  float normal_strength = 0.0;
  float wasteful_strength = 0.0;

  economical_strength = max(economical_strength, min(power_economical, pf_good));
  economical_strength = max(economical_strength, min(power_economical, reactive_low));
  economical_strength = max(economical_strength, min(power_economical, voltage_normal));
  economical_strength = max(economical_strength, min(pf_good, reactive_low));
  normal_strength = max(normal_strength, min(power_normal, pf_fair));
  normal_strength = max(normal_strength, min(power_normal, voltage_normal));
  normal_strength = max(normal_strength, min(power_normal, reactive_medium));
  normal_strength = max(normal_strength, min(pf_fair, voltage_normal));
  normal_strength = max(normal_strength, min(power_economical, pf_poor));
  wasteful_strength = max(wasteful_strength, power_wasteful);
  wasteful_strength = max(wasteful_strength, pf_poor);
  wasteful_strength = max(wasteful_strength, reactive_high);
  wasteful_strength = max(wasteful_strength, max(voltage_low, voltage_high));
  wasteful_strength = max(wasteful_strength, min(power_normal, pf_poor));
  wasteful_strength = max(wasteful_strength, min(power_wasteful, reactive_high));

  if (economical_strength > normal_strength && economical_strength > wasteful_strength) return "ECONOMICAL";
  if (wasteful_strength > normal_strength && wasteful_strength > economical_strength) return "WASTEFUL";
  return "NORMAL";
}

void updateBlynkFuzzyStatus(float temperature, float humidity, float voltage, float power,
                            float powerFactor, float reactivePower) {
  String tempComfort = fuzzyTemperatureComfort(temperature, humidity);
  String energyStatus = fuzzyEnergyConsumption(voltage, power, powerFactor, reactivePower);
  int energyNumeric = (energyStatus == "ECONOMICAL") ? 1 : (energyStatus == "NORMAL") ? 2 : 3;
  Blynk.virtualWrite(V10, tempComfort);
  Blynk.virtualWrite(V11, energyNumeric);
}

void handleLCDBlink(unsigned long currentMillis, unsigned long startTime) {
  bool shouldBlink = (currentMillis - startTime >= BLINK_DELAY);
  if (shouldBlink && currentMillis - previousBlinkMillis >= BLINK_INTERVAL) {
    previousBlinkMillis = currentMillis;
    lcdBacklightState = !lcdBacklightState;
    lcdBacklightState ? lcd.backlight() : lcd.noBacklight();
  }
}

void stopLCDBlink() {
  lcdBacklightState = true;
  lcd.backlight();
}

void checkBoot() {
  pinMode(TRIGGER_PIN, INPUT_PULLUP);
  if (digitalRead(TRIGGER_PIN) == LOW) {
    delay(100);
    if (digitalRead(TRIGGER_PIN) == LOW) {
      Serial.println("Boot button pressed");
      delay(5000);
      if (digitalRead(TRIGGER_PIN) == LOW) {
        Serial.println("Resetting WiFi config...");
        WiFiManager wfm;
        wfm.resetSettings();
        ESP.restart();
      }
    }
  }
}

void showIntroText() {
  lcd.clear();
  lcd.setCursor((16 - String("EcoOffice").length()) / 2, 0);
  lcd.print("EcoOffice");
  lcd.setCursor((16 - String("By Danke Hidayat").length()) / 2, 1);
  lcd.print("By Danke Hidayat");
}

// ── Schedule OTA from MQTT push or HTTP pull ──────────────
void scheduleOta(const String &url) {
  if (url.length() < 8) {
    Serial.println("OTA: reject empty/short url");
    return;
  }
  if (otaPending && otaUrl == url) {
    Serial.println("OTA: already queued for same url");
    return;
  }
  otaUrl = url;
  otaPending = true;
  Serial.println("OTA: scheduled");
  Serial.println(otaUrl);
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("OTA queued...");
}

// ── Report OTA result to Selene API ───────────────────────
void reportOtaResult(bool success, const String &errMsg) {
  WiFiClientSecure client;
  client.setInsecure();
  client.setTimeout(20);
  HTTPClient http;
  http.setTimeout(20000);
  String url = String(SELENE_API_BASE) + "/firmware/result";
  if (!http.begin(client, url)) {
    Serial.println("OTA: cannot begin result POST");
    return;
  }
  http.addHeader("Content-Type", "application/json");
  StaticJsonDocument<256> doc;
  doc["nodeId"] = NODE_ID;
  doc["success"] = success;
  if (!success && errMsg.length()) doc["error"] = errMsg;
  String body;
  serializeJson(doc, body);
  int code = http.POST(body);
  Serial.printf("OTA: result POST HTTP %d\n", code);
  http.end();
}

/**
 * Pull fallback: if Admin uploaded a .bin but the MQTT ota push was missed
 * (device busy, brief disconnect), GET /api/firmware/check/<node> still works.
 */
void checkPendingOtaPull() {
  if (otaPending) return;
  if (WiFi.status() != WL_CONNECTED) return;

  String base = String(SELENE_API_BASE);
  if (base.indexOf("YOUR_DOMAIN") >= 0 || base.length() < 12) {
    // Secrets not configured in this build
    return;
  }

  WiFiClientSecure client;
  client.setInsecure();
  client.setTimeout(15);
  HTTPClient http;
  http.setTimeout(15000);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);

  String url = base + "/firmware/check/" + String(NODE_ID);
  Serial.print("OTA: pull check ");
  Serial.println(url);

  if (!http.begin(client, url)) {
    Serial.println("OTA: pull check begin failed");
    return;
  }

  int code = http.GET();
  if (code != 200) {
    Serial.printf("OTA: pull check HTTP %d\n", code);
    http.end();
    return;
  }

  String body = http.getString();
  http.end();

  StaticJsonDocument<384> doc;
  if (deserializeJson(doc, body)) {
    Serial.println("OTA: pull check JSON parse failed");
    return;
  }

  if (!doc["pending"].as<bool>()) {
    Serial.println("OTA: no pending firmware on server");
    return;
  }

  const char *dl = doc["url"];
  if (!dl || strlen(dl) < 8) {
    Serial.println("OTA: pending but missing url");
    return;
  }

  Serial.printf("OTA: server has pending firmware (%d bytes)\n",
                doc["size"].as<int>());
  scheduleOta(String(dl));
}

// ── Perform OTA (blocking; called from loop, not callback) ─
void performOtaUpdate(const String &url) {
  Serial.println("========================================");
  Serial.println("OTA: starting HTTPS firmware update");
  Serial.println(url);
  Serial.printf("OTA: free heap before = %u\n", ESP.getFreeHeap());
  Serial.println("========================================");

  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("OTA Update...");
  lcd.setCursor(0, 1);
  lcd.print("Preparing...");

  // Free sockets / RAM before TLS download (MQTT + Blynk hold connections)
  mqtt.disconnect();
  delay(200);

  lcd.setCursor(0, 1);
  lcd.print("Downloading...");

  WiFiClientSecure client;
  client.setInsecure();  // production: pin CA or use setCACert
  client.setTimeout(120);  // seconds on ESP32 WiFiClientSecure

  httpUpdate.rebootOnUpdate(false);  // we reboot after reporting
  httpUpdate.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  // Note: closeConnectionsOnUpdate() is not available on all ESP32 Arduino cores
  // (e.g. 3.3.x). MQTT is already disconnected above before HTTPS starts.

  httpUpdate.onProgress([](int cur, int total) {
    static int lastPct = -1;
    int pct = (total > 0) ? (cur * 100) / total : 0;
    if (pct != lastPct && (pct % 10 == 0 || pct == 100)) {
      lastPct = pct;
      Serial.printf("OTA: progress %d%% (%d/%d)\n", pct, cur, total);
      lcd.setCursor(0, 1);
      lcd.print("DL ");
      lcd.print(pct);
      lcd.print("%          ");
    }
  });

  t_httpUpdate_return ret = httpUpdate.update(client, url);

  if (ret == HTTP_UPDATE_OK) {
    Serial.println("OTA: SUCCESS, rebooting");
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print("OTA Success");
    lcd.setCursor(0, 1);
    lcd.print("Rebooting...");
    reportOtaResult(true, "");
    delay(1500);
    ESP.restart();
  }

  int errCode = httpUpdate.getLastError();
  String err = httpUpdate.getLastErrorString();
  Serial.printf("OTA: FAILED (%d) %s\n", errCode, err.c_str());
  Serial.printf("OTA: free heap after = %u\n", ESP.getFreeHeap());
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("OTA Failed");
  lcd.setCursor(0, 1);
  lcd.print(err.substring(0, 16));
  reportOtaResult(false, String(errCode) + " " + err);
  delay(3000);
  // MQTT will reconnect on next loop via connectMQTT()
}

// ── MQTT connect ──────────────────────────────────────────
bool connectMQTT() {
  if (mqtt.connected()) return true;

  String clientId = "selene-" + String(NODE_ID) + "-" + String(random(0xffff), HEX);
  Serial.print("MQTT: Menghubungkan ke broker...");

  String lwTopic = "selene/" + String(NODE_ID) + "/status";
  String lwMessage = "{\"status\":\"offline\"}";

  if (mqtt.connect(clientId.c_str(), MQTT_USER, MQTT_PASSWORD, lwTopic.c_str(), 1, true,
                   lwMessage.c_str())) {
    Serial.println(" berhasil!");
    mqtt.publish(lwTopic.c_str(), "{\"status\":\"online\"}", true);
    String cmdTopic = "selene/" + String(NODE_ID) + "/command";
    mqtt.subscribe(cmdTopic.c_str(), 1);
    Serial.println("MQTT: Subscribe ke " + cmdTopic);
    // Catch OTA that was uploaded while we were offline
    lastOtaPullCheck = 0;
    return true;
  }

  Serial.print(" gagal, rc=");
  Serial.println(mqtt.state());
  return false;
}

// energyKwh: cumulative from pzem.energy() — already kWh (library divides Wh by 1000)
void publishTelemetryMQTT(float voltage, float current, float power, float pf, float energyKwh,
                          float frequency, float calibratedTemp, float calibratedHum,
                          float apparentPower, float reactivePower) {
  StaticJsonDocument<384> doc;
  doc["voltage"] = voltage;
  doc["current"] = current;
  doc["power"] = power;
  doc["pf"] = pf;
  doc["energy"] = energyKwh;  // kWh (same unit Selene stores as total_energy)
  doc["frequency"] = frequency;
  doc["apparentPower"] = apparentPower;
  doc["reactivePower"] = reactivePower;
  doc["temperature"] = calibratedTemp;
  doc["humidity"] = calibratedHum;

  String json;
  serializeJson(doc, json);

  String topic = "selene/" + String(NODE_ID) + "/telemetry";
  if (mqtt.publish(topic.c_str(), json.c_str())) {
    Serial.println("MQTT: Data terkirim ke " + topic);
  } else {
    Serial.println("MQTT: GAGAL mengirim data");
  }
}

// ── MQTT callback: schedule OTA, handle reboot/status ─────
void mqttCallback(char *topic, byte *payload, unsigned int length) {
  String message;
  message.reserve(length + 1);
  for (unsigned int i = 0; i < length; i++) message += (char)payload[i];

  Serial.print("MQTT: Perintah diterima [");
  Serial.print(topic);
  Serial.print("]: ");
  Serial.println(message);

  // Capacity for long HTTPS OTA URLs
  StaticJsonDocument<768> doc;
  DeserializationError error = deserializeJson(doc, message);
  if (error) {
    Serial.print("MQTT: Gagal parse JSON command: ");
    Serial.println(error.c_str());
    return;
  }

  // Copy into stack buffers so we never depend on JsonDocument pointer lifetime
  const char *commandPtr = doc["command"] | "";
  char command[24];
  strncpy(command, commandPtr, sizeof(command) - 1);
  command[sizeof(command) - 1] = '\0';

  if (command[0] == '\0') {
    Serial.println("MQTT: no command field");
    return;
  }

  Serial.print("MQTT: parsed command=");
  Serial.println(command);

  if (strcmp(command, "reboot") == 0) {
    Serial.println("MQTT: Menjalankan perintah REBOOT...");
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print("Remote Reboot...");
    delay(1000);
    ESP.restart();
    return;
  }

  if (strcmp(command, "status") == 0) {
    String statusTopic = "selene/" + String(NODE_ID) + "/status";
    String statusPayload = "{\"status\":\"online\",\"rssi\":" + String(WiFi.RSSI()) +
                           ",\"uptime\":" + String(millis() / 1000) +
                           ",\"free_heap\":" + String(ESP.getFreeHeap()) + "}";
    mqtt.publish(statusTopic.c_str(), statusPayload.c_str());
    Serial.println("MQTT: Status terkirim");
    return;
  }

  if (strcmp(command, "ota") == 0) {
    const char *urlPtr = doc["url"] | "";
    if (strlen(urlPtr) < 8) {
      Serial.println("MQTT: OTA missing/invalid url");
      return;
    }
    Serial.println("MQTT: OTA branch OK, queueing download");
    // Defer blocking HTTPUpdate to loop() — never block inside callback
    scheduleOta(String(urlPtr));
    return;
  }

  Serial.print("MQTT: unknown command (ignored): ");
  Serial.println(command);
}

void setup() {
  Serial.begin(115200);
  delay(200);
  Wire.begin();
  lcd.init();
  lcd.backlight();

  for (int i = 0; i < 10; i++) {
    tempErrors[i] = 0;
    humErrors[i] = 0;
  }

  checkBoot();
  showIntroText();
  delay(3500);

#define AP_PASS ""
#define AP_SSID "EcoOffice"
  const unsigned long WIFI_TIMEOUT = 60;
  WiFiManager wfm;

  wfm.setConfigPortalTimeout(WIFI_TIMEOUT);
  wfm.setHostname(AP_SSID);
  wfm.setConnectTimeout(WIFI_TIMEOUT);

  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("Waiting for WiFi");
  lcd.setCursor(0, 1);
  lcd.print("Connection...");
  unsigned long wifiStartTime = millis();
  bool connected = false;

  while (!connected && (millis() - wifiStartTime < WIFI_TIMEOUT * 1000)) {
    connected = wfm.autoConnect(AP_SSID, AP_PASS);
    handleLCDBlink(millis(), wifiStartTime);
    delay(100);
  }
  if (!connected) {
    Serial.println("Failed to connect to WiFi!");
    stopLCDBlink();
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print("Failed to Connect");
    delay(1000);
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print("Restarting ESP32...");
    delay(3500);
    ESP.restart();
  }

  Serial.println("WiFi Connected!");
  stopLCDBlink();
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("WiFi Connected!");
  lcd.setCursor(0, 1);
  lcd.print("IP:" + WiFi.localIP().toString());
  delay(3000);

  Blynk.config(auth, "", 8080);
  if (Blynk.connect(3000)) {
    Serial.println("Blynk connected!");
  } else {
    Serial.println("Blynk connection timeout, continuing...");
  }

  mqtt.setServer(MQTT_BROKER, MQTT_PORT);
  mqtt.setCallback(mqttCallback);
  mqtt.setBufferSize(1024);  // room for OTA command JSON + long URL
  mqtt.setKeepAlive(60);

  hwSerial.begin(9600, SERIAL_8N1, 16, 17);
  dht.begin();

  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("System Ready!");
  lcd.setCursor(0, 1);
  lcd.print("Monitoring...");
  delay(1500);
  lcd.clear();

  Serial.println("System Started - Energy + Environment + MQTT OTA v2");
  Serial.print("MQTT Node ID: ");
  Serial.println(NODE_ID);
  Serial.printf("Free heap: %u\n", ESP.getFreeHeap());
  Serial.println("OTA: USB-flash marker — if you see ota MQTT without 'OTA branch OK', reflash this sketch");
}

void loop() {
  // Run OTA outside MQTT callback (blocking download)
  if (otaPending) {
    otaPending = false;
    String url = otaUrl;
    otaUrl = "";
    performOtaUpdate(url);
  }

  Blynk.run();

  static unsigned long lastMqttAttempt = 0;
  const unsigned long MQTT_RETRY_INTERVAL = 5000;

  if (!mqtt.connected() && millis() - lastMqttAttempt >= MQTT_RETRY_INTERVAL) {
    lastMqttAttempt = millis();
    connectMQTT();
  }
  mqtt.loop();

  // Pull fallback if MQTT ota push was missed
  if (!otaPending && millis() - lastOtaPullCheck >= OTA_PULL_INTERVAL_MS) {
    lastOtaPullCheck = millis();
    checkPendingOtaPull();
  }

  static unsigned long previousMillis = 0;
  const unsigned long SENSOR_INTERVAL = 3000;
  if (millis() - previousMillis >= SENSOR_INTERVAL) {
    previousMillis = millis();
    float voltage = zeroIfNan(pzem.voltage());
    float current = zeroIfNan(pzem.current());
    float power = zeroIfNan(pzem.power());
    // PZEM004Tv30::energy() returns kWh (internal Wh register / 1000)
    float energyKwh = zeroIfNan(pzem.energy());
    float frequency = zeroIfNan(pzem.frequency());
    float pf = zeroIfNan(pzem.pf());
    float humidity = zeroIfNan(dht.readHumidity());
    float temperature = zeroIfNan(dht.readTemperature());
    float calibratedTemp = calibrateTemperature(temperature);
    float calibratedHum = calibrateHumidity(humidity);
    float simpleTemp = calibrateTemperatureSimple(temperature);
    float simpleHum = calibrateHumiditySimple(humidity);

    recordCalibrationError(abs(calibratedTemp - simpleTemp), abs(calibratedHum - simpleHum));

    float apparentPower = (pf == 0) ? 0 : power / pf;
    float reactivePower = (pf == 0) ? 0 : sqrt(sq(apparentPower) - sq(power));
    updateBlynkFuzzyStatus(calibratedTemp, calibratedHum, voltage, power, pf, reactivePower);

    static int displayMode = 0;
    lcd.clear();

    switch (displayMode) {
      case 0:
        lcd.setCursor(0, 0);
        lcd.print("Voltage: " + String(voltage, 1) + "V");
        lcd.setCursor(0, 1);
        lcd.print("Current: " + String(current, 3) + "A");
        break;
      case 1:
        lcd.setCursor(0, 0);
        lcd.print("Power: " + String(power, 1) + "W");
        lcd.setCursor(0, 1);
        lcd.print("Freq: " + String(frequency, 1) + "Hz");
        break;
      case 2:
        // 3 decimals: 0.001 kWh = 1 Wh (PZEM resolution)
        lcd.setCursor(0, 0);
        lcd.print("E:" + String(energyKwh, 3) + "kWh");
        lcd.setCursor(0, 1);
        lcd.print("PF: " + String(pf, 2));
        break;
      case 3:
        lcd.setCursor(0, 0);
        lcd.print("Temp: " + String(calibratedTemp, 1) + "C");
        lcd.setCursor(0, 1);
        lcd.print("Humidity: " + String(calibratedHum, 1) + "%");
        break;
      case 4:
        lcd.setCursor(0, 0);
        lcd.print("Comfort:" + fuzzyTemperatureComfort(calibratedTemp, calibratedHum));
        lcd.setCursor(0, 1);
        lcd.print("Energy:" + fuzzyEnergyConsumption(voltage, power, pf, reactivePower));
        break;
    }
    displayMode = (displayMode + 1) % 5;

    Blynk.virtualWrite(V0, voltage);
    Blynk.virtualWrite(V1, current);
    Blynk.virtualWrite(V2, power);
    Blynk.virtualWrite(V3, pf);
    Blynk.virtualWrite(V4, apparentPower);
    Blynk.virtualWrite(V5, energyKwh);  // kWh — set Blynk V5 widget unit to kWh
    Blynk.virtualWrite(V6, frequency);
    Blynk.virtualWrite(V7, reactivePower);
    Blynk.virtualWrite(V8, calibratedTemp);
    Blynk.virtualWrite(V9, calibratedHum);
  }

  if (millis() - lastMqttPublish >= MQTT_PUBLISH_INTERVAL) {
    lastMqttPublish = millis();

    float voltageMQTT = zeroIfNan(pzem.voltage());
    float currentMQTT = zeroIfNan(pzem.current());
    float powerMQTT = zeroIfNan(pzem.power());
    float energyKwhMQTT = zeroIfNan(pzem.energy());  // kWh
    float frequencyMQTT = zeroIfNan(pzem.frequency());
    float pfMQTT = zeroIfNan(pzem.pf());
    float humidityMQTT = zeroIfNan(dht.readHumidity());
    float temperatureMQTT = zeroIfNan(dht.readTemperature());
    float calibratedTempMQTT = calibrateTemperature(temperatureMQTT);
    float calibratedHumMQTT = calibrateHumidity(humidityMQTT);
    float apparentPowerMQTT = (pfMQTT == 0) ? 0 : powerMQTT / pfMQTT;
    float reactivePowerMQTT =
        (pfMQTT == 0) ? 0 : sqrt(sq(apparentPowerMQTT) - sq(powerMQTT));

    publishTelemetryMQTT(voltageMQTT, currentMQTT, powerMQTT, pfMQTT, energyKwhMQTT,
                         frequencyMQTT, calibratedTempMQTT, calibratedHumMQTT,
                         apparentPowerMQTT, reactivePowerMQTT);
  }
}
