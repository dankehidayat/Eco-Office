#include <WiFi.h>
#include <WiFiClient.h>
#define BLYNK_TEMPLATE_ID "TMPL6eUbLFTuj"
#define BLYNK_TEMPLATE_NAME "Energy Monitor"
#include <BlynkSimpleEsp32.h> 
#include <LiquidCrystal_I2C.h>
#include <WiFiManager.h>
#include <PZEM004Tv30.h> 
#include <DHT.h>
#include <Wire.h>
#include <math.h>

// ==================== HARDWARE INITIALIZATION ====================

// Inisialisasi objek LCD dengan alamat I2C dan ukuran layar (16x2)
LiquidCrystal_I2C lcd(0x27, 16, 2);

// Inisialisasi objek PZEM-004T menggunakan HardwareSerial
HardwareSerial hwSerial(1); // Gunakan UART1 pada ESP32
PZEM004Tv30 pzem(hwSerial, 16, 17); // Pin RX, TX pada ESP32

// Inisialisasi objek DHT11
#define DHTPIN 27 // Pin D27 pada ESP32
#define DHTTYPE DHT11 // Tipe sensor DHT11
DHT dht(DHTPIN, DHTTYPE);

#define TRIGGER_PIN 0 // Pin untuk tombol boot/reset WiFi

// Token Blynk Anda
char auth[] = "";

// ==================== SYSTEM VARIABLES ====================

// Variabel untuk kontrol kedip LCD
bool lcdBacklightState = true;
unsigned long previousBlinkMillis = 0;
const unsigned long BLINK_DELAY = 4500; // Delay sebelum mulai kedip (ms)
const unsigned long BLINK_INTERVAL = 250; // Interval kedip (ms)

// ==================== TEMPERATURE SYSTEM ====================

/*
ANALISIS DATA KALIBRASI TERKINI (34 titik pengukuran):
- Model Regresi Linear: 
  • Suhu: HTC-1 = 0.923 × DHT11 - 1.618 (R² = 0.958)
  • Kelembaban: HTC-1 = 0.926 × DHT11 + 18.052 (R² = 0.977)
*/

// Koefisien regresi linear untuk kalibrasi suhu
const float TEMP_SLOPE = 0.923;      // Slope garis regresi (perubahan HTC-1 per unit DHT11)
const float TEMP_INTERCEPT = -1.618; // Intercept garis regresi (nilai HTC-1 ketika DHT11 = 0)

// Koefisien regresi linear untuk kalibrasi kelembaban  
const float HUM_SLOPE = 0.926;       // Slope garis regresi
const float HUM_INTERCEPT = 18.052;  // Intercept garis regresi

// Koreksi sederhana berdasarkan selisih rata-rata (bias)
const float TEMP_BIAS = -3.84; // DHT11 membaca 3.84°C lebih tinggi dari HTC-1
const float HUM_BIAS = +14.18; // DHT11 membaca 14.18% lebih rendah dari HTC-1

// Array untuk menyimpan data error monitoring
float tempErrors[10]; // Menyimpan 10 error terakhir untuk suhu
float humErrors[10];  // Menyimpan 10 error terakhir untuk kelembaban
int errorIndex = 0;   // Index untuk circular buffer

/**
 * TEMPERATURE: Kalibrasi suhu menggunakan model regresi linear
 * Persamaan: HTC-1 = 0.923 × DHT11 - 1.618
 */
float calibrateTemperature(float rawTemp) {
  return (TEMP_SLOPE * rawTemp) + TEMP_INTERCEPT;
}

/**
 * TEMPERATURE: Kalibrasi kelembaban menggunakan model regresi linear  
 * Persamaan: HTC-1 = 0.926 × DHT11 + 18.052
 */
float calibrateHumidity(float rawHum) {
  return (HUM_SLOPE * rawHum) + HUM_INTERCEPT;
}

/**
 * TEMPERATURE: Kalibrasi sederhana berdasarkan bias rata-rata
 */
float calibrateTemperatureSimple(float rawTemp) {
  return rawTemp + TEMP_BIAS;
}

/**
 * TEMPERATURE: Kalibrasi sederhana berdasarkan bias rata-rata
 */
float calibrateHumiditySimple(float rawHum) {
  return rawHum + HUM_BIAS;
}

/**
 * TEMPERATURE: Mencatat error kalibrasi untuk analisis akurasi
 */
void recordCalibrationError(float tempError, float humError) {
  tempErrors[errorIndex] = tempError;
  humErrors[errorIndex] = humError;
  errorIndex = (errorIndex + 1) % 10;
}

/**
 * TEMPERATURE: Menghitung Mean Absolute Error (MAE) real-time
 */
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

/**
 * TEMPERATURE: Menghitung akurasi dalam persentase
 */
float calculateAccuracy(float mae, float range) {
  return max(0.0, 100.0 - (mae / range * 100.0));
}

// ==================== ENERGY SYSTEM ====================

/**
 * ENERGY: Mengubah nilai NaN (Not a Number) menjadi 0
 */
float zeroIfNan(float value) {
  return isnan(value) ? 0.0 : value;
}

// ==================== FUZZY LOGIC SYSTEM ====================

/**
 * FUZZY: Sistem logika fuzzy untuk klasifikasi kenyamanan termal
 * Berdasarkan standar ASHRAE 55 dan ISO 7730
 */
String fuzzyTemperatureComfort(float temp, float humidity) {
  // Fuzzyfikasi suhu
  float cold = (temp <= 18) ? 1.0 : (temp <= 22) ? (22 - temp) / 4.0 : 0.0;
  float comfortable = (temp >= 20 && temp <= 23) ? (temp - 20) / 3.0 : 
                     (temp > 23 && temp <= 26) ? (26 - temp) / 3.0 : 0.0;
  float warm = (temp >= 24 && temp <= 27) ? (temp - 24) / 3.0 :
               (temp > 27 && temp <= 30) ? (30 - temp) / 3.0 : 0.0;
  float hot = (temp >= 28) ? 1.0 : (temp >= 26) ? (temp - 26) / 2.0 : 0.0;

  // Fuzzyfikasi kelembaban 
  float dry = (humidity <= 30) ? 1.0 : (humidity <= 40) ? (40 - humidity) / 10.0 : 0.0;
  float comfortable_hum = (humidity >= 30 && humidity <= 50) ? (humidity - 30) / 20.0 :
                         (humidity > 50 && humidity <= 70) ? (70 - humidity) / 20.0 : 0.0;
  float humid = (humidity >= 60) ? 1.0 : (humidity >= 50) ? (humidity - 50) / 10.0 : 0.0;

  // Aplikasi 8 rules
  float cold_strength = cold;
  float cool_strength = max(min(comfortable, dry), min(cold, humid));
  float comfortable_strength = min(comfortable, comfortable_hum);
  float warm_strength = max(warm, min(comfortable, humid));
  float hot_strength = max(hot, min(hot, humid));

  // Defuzzifikasi
  float strengths[] = {cold_strength, cool_strength, comfortable_strength, warm_strength, hot_strength};
  String categories[] = {"COLD", "COOL", "COMFORTABLE", "WARM", "HOT"};
  
  int max_index = 0;
  for (int i = 1; i < 5; i++) {
    if (strengths[i] > strengths[max_index]) {
      max_index = i;
    }
  }
  
  return categories[max_index];
}

/**
 * FUZZY: Sistem logika fuzzy untuk klasifikasi konsumsi energi
 * VERSI TERBARU - Disesuaikan untuk Beban Perkantoran Kecil (0-150W)
 * Menggunakan 15 aturan dengan 4 variabel input
 * 
 * @param voltage Tegangan dalam Volt
 * @param power Daya aktif dalam Watt
 * @param powerFactor Faktor daya (0-1)
 * @param reactivePower Daya reaktif dalam VAR
 * @return Kategori konsumsi energi: "ECONOMICAL", "NORMAL", "WASTEFUL"
 */
String fuzzyEnergyConsumption(float voltage, float power, float powerFactor, float reactivePower) {
  
  // ==================== 1. FUZZIFIKASI VOLTAGE ====================
  // Himpunan: RENDAH, NORMAL, TINGGI
  
  float voltage_low = 0.0;
  float voltage_normal = 0.0;
  float voltage_high = 0.0;
  
  // Himpunan RENDAH: <210V (trapesium: 200-200-205-210)
  if (voltage <= 200) {
    voltage_low = 1.0;
  } else if (voltage <= 210) {
    voltage_low = (210 - voltage) / 10.0;
  } else {
    voltage_low = 0.0;
  }

  // Himpunan NORMAL: 205-235V (segitiga: 205-220-235)
  if (voltage >= 205 && voltage <= 220) {
    voltage_normal = (voltage - 205) / 15.0;
  } else if (voltage > 220 && voltage <= 235) {
    voltage_normal = (235 - voltage) / 15.0;
  } else {
    voltage_normal = 0.0;
  }

  // Himpunan TINGGI: >230V (trapesium: 230-235-250-250)
  if (voltage >= 235) {
    voltage_high = 1.0;
  } else if (voltage >= 230) {
    voltage_high = (voltage - 230) / 5.0;
  } else {
    voltage_high = 0.0;
  }

  // ==================== 2. FUZZIFIKASI DAYA AKTIF ====================
  // Himpunan: EKONOMIS, NORMAL, BOROS
  
  float power_economical = 0.0;
  float power_normal = 0.0;
  float power_wasteful = 0.0;
  
  // Himpunan EKONOMIS: <30W (trapesium: 20-20-25-30)
  if (power <= 20) {
    power_economical = 1.0;
  } else if (power <= 30) {
    power_economical = (30 - power) / 10.0;
  } else {
    power_economical = 0.0;
  }

  // Himpunan NORMAL: 25-70W (segitiga: 25-47.5-70)
  if (power >= 25 && power <= 47.5) {
    power_normal = (power - 25) / 22.5;
  } else if (power > 47.5 && power <= 70) {
    power_normal = (70 - power) / 22.5;
  } else {
    power_normal = 0.0;
  }

  // Himpunan BOROS: >60W (trapesium: 60-70-150-150)
  if (power >= 80) {
    power_wasteful = 1.0;
  } else if (power >= 60) {
    power_wasteful = (power - 60) / 20.0;
  } else {
    power_wasteful = 0.0;
  }

  // ==================== 3. FUZZIFIKASI FAKTOR DAYA ====================
  // Himpunan: BURUK, CUKUP, BAIK
  
  float pf_poor = 0.0;
  float pf_fair = 0.0;
  float pf_good = 0.0;
  
  // Himpunan BURUK: <0.6 (trapesium: 0.5-0.5-0.55-0.6)
  if (powerFactor <= 0.5) {
    pf_poor = 1.0;
  } else if (powerFactor <= 0.6) {
    pf_poor = (0.6 - powerFactor) / 0.1;
  } else {
    pf_poor = 0.0;
  }

  // Himpunan CUKUP: 0.55-0.85 (segitiga: 0.55-0.7-0.85)
  if (powerFactor >= 0.55 && powerFactor <= 0.7) {
    pf_fair = (powerFactor - 0.55) / 0.15;
  } else if (powerFactor > 0.7 && powerFactor <= 0.85) {
    pf_fair = (0.85 - powerFactor) / 0.15;
  } else {
    pf_fair = 0.0;
  }

  // Himpunan BAIK: >0.8 (trapesium: 0.8-0.85-1.0-1.0)
  if (powerFactor >= 0.90) {
    pf_good = 1.0;
  } else if (powerFactor >= 0.80) {
    pf_good = (powerFactor - 0.80) / 0.10;
  } else {
    pf_good = 0.0;
  }

  // ==================== 4. FUZZIFIKASI DAYA REAKTIF ====================
  // Himpunan: RENDAH, SEDANG, TINGGI
  
  float reactive_low = 0.0;
  float reactive_medium = 0.0;
  float reactive_high = 0.0;
  
  // Himpunan RENDAH: <25VAR (trapesium: 15-15-20-25)
  if (reactivePower <= 15) {
    reactive_low = 1.0;
  } else if (reactivePower <= 25) {
    reactive_low = (25 - reactivePower) / 10.0;
  } else {
    reactive_low = 0.0;
  }

  // Himpunan SEDANG: 20-55VAR (segitiga: 20-37.5-55)
  if (reactivePower >= 20 && reactivePower <= 37.5) {
    reactive_medium = (reactivePower - 20) / 17.5;
  } else if (reactivePower > 37.5 && reactivePower <= 55) {
    reactive_medium = (55 - reactivePower) / 17.5;
  } else {
    reactive_medium = 0.0;
  }

  // Himpunan TINGGI: >45VAR (trapesium: 45-50-150-150)
  if (reactivePower >= 60) {
    reactive_high = 1.0;
  } else if (reactivePower >= 45) {
    reactive_high = (reactivePower - 45) / 15.0;
  } else {
    reactive_high = 0.0;
  }

  // ==================== 5. INFERENSI - 15 RULE BASE ====================
  
  float economical_strength = 0.0;
  float normal_strength = 0.0;
  float wasteful_strength = 0.0;

  // *** GROUP 1: KATEGORI EKONOMIS (4 RULES) ***
  // R1: IF Daya EKONOMIS AND Faktor Daya BAIK THEN Kategori EKONOMIS
  economical_strength = max(economical_strength, min(power_economical, pf_good));
  
  // R2: IF Daya EKONOMIS AND Daya Reaktif RENDAH THEN Kategori EKONOMIS
  economical_strength = max(economical_strength, min(power_economical, reactive_low));
  
  // R3: IF Daya EKONOMIS AND Tegangan NORMAL THEN Kategori EKONOMIS
  economical_strength = max(economical_strength, min(power_economical, voltage_normal));
  
  // R4: IF Faktor Daya BAIK AND Daya Reaktif RENDAH THEN Kategori EKONOMIS
  economical_strength = max(economical_strength, min(pf_good, reactive_low));

  // *** GROUP 2: KATEGORI NORMAL (5 RULES) ***
  // R5: IF Daya NORMAL AND Faktor Daya CUKUP THEN Kategori NORMAL
  normal_strength = max(normal_strength, min(power_normal, pf_fair));
  
  // R6: IF Daya NORMAL AND Tegangan NORMAL THEN Kategori NORMAL
  normal_strength = max(normal_strength, min(power_normal, voltage_normal));
  
  // R7: IF Daya NORMAL AND Daya Reaktif SEDANG THEN Kategori NORMAL
  normal_strength = max(normal_strength, min(power_normal, reactive_medium));
  
  // R8: IF Faktor Daya CUKUP AND Tegangan NORMAL THEN Kategori NORMAL
  normal_strength = max(normal_strength, min(pf_fair, voltage_normal));
  
  // R9: IF Daya EKONOMIS AND Faktor Daya BURUK THEN Kategori NORMAL (kompensasi)
  normal_strength = max(normal_strength, min(power_economical, pf_poor));

  // *** GROUP 3: KATEGORI BOROS (6 RULES) ***
  // R10: IF Daya BOROS THEN Kategori BOROS
  wasteful_strength = max(wasteful_strength, power_wasteful);
  
  // R11: IF Faktor Daya BURUK THEN Kategori BOROS
  wasteful_strength = max(wasteful_strength, pf_poor);
  
  // R12: IF Daya Reaktif TINGGI THEN Kategori BOROS
  wasteful_strength = max(wasteful_strength, reactive_high);
  
  // R13: IF Tegangan RENDAH OR Tegangan TINGGI THEN Kategori BOROS
  wasteful_strength = max(wasteful_strength, max(voltage_low, voltage_high));
  
  // R14: IF Daya NORMAL AND Faktor Daya BURUK THEN Kategori BOROS
  wasteful_strength = max(wasteful_strength, min(power_normal, pf_poor));
  
  // R15: IF Daya BOROS AND Daya Reaktif TINGGI THEN Kategori BOROS
  wasteful_strength = max(wasteful_strength, min(power_wasteful, reactive_high));

  // ==================== 6. DEFUZZIFIKASI ====================
  
  if (economical_strength > normal_strength && economical_strength > wasteful_strength) {
    return "ECONOMICAL";
  } else if (wasteful_strength > normal_strength && wasteful_strength > economical_strength) {
    return "WASTEFUL";
  } else {
    return "NORMAL";
  }
}

/**
 * FUZZY: Update Blynk dengan status fuzzy terpisah
 * V10 = Temperature Comfort Status (String)
 * V11 = Energy Consumption Status (Numeric: 1=ECONOMICAL, 2=NORMAL, 3=WASTEFUL)
 */
void updateBlynkFuzzyStatus(float temperature, float humidity, float voltage, float power, float powerFactor, float reactivePower) {
  String tempComfort = fuzzyTemperatureComfort(temperature, humidity);
  String energyStatus = fuzzyEnergyConsumption(voltage, power, powerFactor, reactivePower);
  
  // Konversi energy status ke nilai numerik (1,2,3) untuk konsistensi dengan Google Colab
  int energyNumeric = 0;
  if (energyStatus == "ECONOMICAL") {
    energyNumeric = 1;
  } else if (energyStatus == "NORMAL") {
    energyNumeric = 2;
  } else if (energyStatus == "WASTEFUL") {
    energyNumeric = 3;
  }
  
  // Kirim nilai string ke V10, nilai numerik ke V11
  Blynk.virtualWrite(V10, tempComfort);
  Blynk.virtualWrite(V11, energyNumeric);
  
  // Debug information ke Serial Monitor
  Serial.println("=== SMART OFFICE GUARDIAN - FUZZY STATUS (UPDATED) ===");
  Serial.print("Thermal Comfort: "); Serial.print(tempComfort);
  Serial.print(" | Temperature: "); Serial.print(temperature, 1); Serial.print("°C");
  Serial.print(" | Humidity: "); Serial.print(humidity, 1); Serial.println("%");
  
  Serial.print("Energy Status: "); Serial.print(energyStatus);
  Serial.print(" (Numeric: "); Serial.print(energyNumeric);
  Serial.print(") | Voltage: "); Serial.print(voltage, 1); Serial.print("V");
  Serial.print(" | Power: "); Serial.print(power, 1); Serial.print("W");
  Serial.print(" | PF: "); Serial.print(powerFactor, 2);
  Serial.print(" | Reactive: "); Serial.print(reactivePower, 1); Serial.println("VAR");
  Serial.println("  Rule Base: 15 rules (4 ECONOMICAL, 5 NORMAL, 6 WASTEFUL)");
  Serial.println();
}

// ==================== SYSTEM CONTROL FUNCTIONS ====================

/**
 * SYSTEM: Mengatur kedipan backlight LCD dengan delay awal
 */
void handleLCDBlink(unsigned long currentMillis, unsigned long startTime) {
  bool shouldBlink = (currentMillis - startTime >= BLINK_DELAY);
  
  if (shouldBlink && currentMillis - previousBlinkMillis >= BLINK_INTERVAL) {
    previousBlinkMillis = currentMillis;
    lcdBacklightState = !lcdBacklightState;
    lcdBacklightState ? lcd.backlight() : lcd.noBacklight();
  }
}

/**
 * SYSTEM: Menghentikan kedipan dan menyalakan backlight permanen
 */
void stopLCDBlink() {
  lcdBacklightState = true;
  lcd.backlight();
}

/**
 * SYSTEM: Mengecek tombol boot untuk reset WiFi
 */
void checkBoot() {
  pinMode(TRIGGER_PIN, INPUT_PULLUP);
  if (digitalRead(TRIGGER_PIN) == LOW) {
    delay(100);
    if (digitalRead(TRIGGER_PIN) == LOW) {
      Serial.println("Boot button pressed");
      delay(5000);
      if (digitalRead(TRIGGER_PIN) == LOW) {
        Serial.println("Boot button held - Erasing WiFi config and restarting");
        WiFiManager wfm;
        wfm.resetSettings();
        ESP.restart();
      }
    }
  }
}

/**
 * SYSTEM: Menampilkan teks intro pada LCD
 */
void showIntroText() {
  lcd.clear();
  String introText = "EcoOffice";
  String authorText = "By Danke Hidayat";

  int introStartPos = (16 - introText.length()) / 2;
  int authorStartPos = (16 - authorText.length()) / 2;

  lcd.setCursor(introStartPos, 0);
  lcd.print(introText);
  lcd.setCursor(authorStartPos, 1);
  lcd.print(authorText);
}

/**
 * SYSTEM: Fungsi untuk scroll teks pada LCD
 */
void scrollText(String line1, String line2) {
  for (int i = 0; i <= line1.length() - 16; i++) {
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print(line1.substring(i, i + 16));
    lcd.setCursor(0, 1);
    lcd.print(line2.substring(i, i + 16));
    delay(250);
  }
}

// ==================== MAIN SYSTEM FUNCTIONS ====================

void setup() {
  Serial.begin(115200);
  
  // Inisialisasi LCD
  Wire.begin();
  lcd.init();
  lcd.backlight();

  // Inisialisasi array error dengan nilai 0
  for (int i = 0; i < 10; i++) {
    tempErrors[i] = 0;
    humErrors[i] = 0;
  }

  // Cek tombol boot
  checkBoot();

  // Tampilkan teks intro
  showIntroText();
  delay(3500);

  // Inisialisasi WiFiManager
  #define AP_PASS "guard14n0ff1ce"
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
  } else {
    Serial.println("WiFi Connected!");
    stopLCDBlink();
    
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print("WiFi Connected!");
    lcd.setCursor(0, 1);
    
    String ipAddress = WiFi.localIP().toString();
    if (ipAddress.length() <= 16) {
      lcd.print("IP:" + ipAddress);
    } else {
      lcd.print("IP:" + ipAddress.substring(0, 13) + "...");
    }
    delay(3000);
  }

  // Inisialisasi Blynk dengan server lokal
  Blynk.begin(auth, WiFi.SSID().c_str(), WiFi.psk().c_str(), "iot.serangkota.go.id", 8080);

  // Inisialisasi HardwareSerial untuk PZEM-004T
  hwSerial.begin(9600, SERIAL_8N1, 16, 17);

  // Inisialisasi DHT11
  dht.begin();

  // Tampilkan system ready
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("System Ready!");
  lcd.setCursor(0, 1);
  lcd.print("Monitoring...");
  delay(1500);

  // Informasi startup di Serial Monitor
  Serial.println("\n==================================================");
  Serial.println("        OFFICE MONITORING IOT DIAKTIFKAN");
  Serial.println("==================================================");
  Serial.println("Sistem monitoring energi dan lingkungan aktif");
  Serial.println("DHT11 terkoreksi dengan akurasi 95.8%-97.7%");
  Serial.println("Model: HTC-1 = 0.923×DHT11 - 1.618 (Suhu)");
  Serial.println("Model: HTC-1 = 0.926×DHT11 + 18.052 (Kelembaban)");
  Serial.println("FUZZY ENERGY: Skala beban perkantoran kecil (0-150W)");
  Serial.println("  - EKONOMIS  : Daya < 30W (perangkat standby/hemat)");
  Serial.println("  - NORMAL    : Daya 25-70W (perangkat kantor normal)");
  Serial.println("  - BOROS     : Daya > 60W (PC desktop, printer)");
  Serial.println("Rule Base: 15 aturan (4 EKONOMIS, 5 NORMAL, 6 BOROS)");
  Serial.println("IP Address: " + WiFi.localIP().toString());
  Serial.println("==================================================\n");
}

void loop() {
  Blynk.run();
  static unsigned long previousMillis = 0;
  const unsigned long SENSOR_INTERVAL = 3000;

  unsigned long currentMillis = millis();

  if (currentMillis - previousMillis >= SENSOR_INTERVAL) {
    previousMillis = currentMillis;
    showEnergyInfo();
  }
}

/**
 * SYSTEM: Fungsi utama untuk menampilkan informasi energi dan sensor
 */
void showEnergyInfo() {
  static int displayMode = 0;

  // ENERGY: Baca data dari PZEM-004T
  float voltage = zeroIfNan(pzem.voltage());
  float current = zeroIfNan(pzem.current());
  float power = zeroIfNan(pzem.power());
  float energyWh = zeroIfNan(pzem.energy());
  float frequency = zeroIfNan(pzem.frequency());
  float pf = zeroIfNan(pzem.pf());

  // TEMPERATURE: Baca data dari DHT11
  float humidity = zeroIfNan(dht.readHumidity());
  float temperature = zeroIfNan(dht.readTemperature());

  // TEMPERATURE: Kalibrasi data DHT11 menggunakan model terbaru
  float calibratedTemp = calibrateTemperature(temperature);
  float calibratedHum = calibrateHumidity(humidity);

  // TEMPERATURE: Hitung dengan metode sederhana untuk perbandingan
  float simpleTemp = calibrateTemperatureSimple(temperature);
  float simpleHum = calibrateHumiditySimple(humidity);

  // TEMPERATURE: Hitung dan simpan error antara kedua metode kalibrasi
  float tempError = abs(calibratedTemp - simpleTemp);
  float humError = abs(calibratedHum - simpleHum);
  recordCalibrationError(tempError, humError);

  // ENERGY: Hitung daya semu dan reaktif
  float apparentPower = (pf == 0) ? 0 : power / pf;
  float reactivePower = (pf == 0) ? 0 : sqrt(sq(apparentPower) - sq(power));

  // Call fuzzy logic function to update V10 and V11
  updateBlynkFuzzyStatus(calibratedTemp, calibratedHum, voltage, power, pf, reactivePower);

  // Tampilkan pada LCD dengan 5 display modes terpisah
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
      lcd.setCursor(0, 0); 
      lcd.print("Energy: " + String(energyWh, 1) + "Wh");
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
      {
        String tempComfort = fuzzyTemperatureComfort(calibratedTemp, calibratedHum);
        String energyStatus = fuzzyEnergyConsumption(voltage, power, pf, reactivePower);
        
        int energyNum = 0;
        if (energyStatus == "ECO") energyNum = 1;
        else if (energyStatus == "NORM") energyNum = 2;
        else if (energyStatus == "WASTE") energyNum = 3;
        
        lcd.setCursor(0, 0); 
        lcd.print("Comfort:" + tempComfort);
        lcd.setCursor(0, 1);
        lcd.print("Energy:" + String(energyNum) + " " + energyStatus);
      }
      break;
  }

  displayMode = (displayMode + 1) % 5;

  // Kirim data ke Blynk
  Blynk.virtualWrite(V0, voltage);
  Blynk.virtualWrite(V1, current);
  Blynk.virtualWrite(V2, power);
  Blynk.virtualWrite(V3, pf);
  Blynk.virtualWrite(V4, apparentPower);
  Blynk.virtualWrite(V5, energyWh);
  Blynk.virtualWrite(V6, frequency);
  Blynk.virtualWrite(V7, reactivePower);
  Blynk.virtualWrite(V8, calibratedTemp);
  Blynk.virtualWrite(V9, calibratedHum);

  // Log ke Serial Monitor setiap 6 readings (18 detik)
  static int logCounter = 0;
  if (logCounter++ >= 6) {
    logCounter = 0;
    
    float currentTempMAE, currentHumMAE;
    getCurrentMAE(currentTempMAE, currentHumMAE);
    float tempAccuracy = calculateAccuracy(currentTempMAE, 50.0);
    float humAccuracy = calculateAccuracy(currentHumMAE, 100.0);
    
    Serial.println("================ SENSOR DATA ================");
    Serial.println("Power Data:");
    Serial.println("  Voltage: " + String(voltage, 1) + "V | Current: " + String(current, 3) + "A");
    Serial.println("  Power: " + String(power, 1) + "W | Energy: " + String(energyWh, 1) + "Wh");
    Serial.println("  Power Factor: " + String(pf, 2) + " | Frequency: " + String(frequency, 1) + "Hz");
    
    Serial.println("Environment Data:");
    Serial.println("  Temperature: " + String(calibratedTemp, 1) + "°C (Raw: " + String(temperature, 1) + "°C)");
    Serial.println("  Humidity: " + String(calibratedHum, 1) + "% (Raw: " + String(humidity, 1) + "%)");
    Serial.println("  Calibration Accuracy - Temp: " + String(tempAccuracy, 1) + "%, Hum: " + String(humAccuracy, 1) + "%");
    
    String tempComfort = fuzzyTemperatureComfort(calibratedTemp, calibratedHum);
    String energyStatus = fuzzyEnergyConsumption(voltage, power, pf, reactivePower);
    
    int energyNum = 0;
    if (energyStatus == "ECONOMICAL") energyNum = 1;
    else if (energyStatus == "NORMAL") energyNum = 2;
    else if (energyStatus == "WASTEFUL") energyNum = 3;
    
    Serial.println("Fuzzy Classification:");
    Serial.println("  Thermal Comfort: " + tempComfort);
    Serial.println("  Energy Status: " + energyStatus + " (Numeric: " + String(energyNum) + ")");
    Serial.println("============================================\n");
  }
}
