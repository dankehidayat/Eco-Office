/*
 * EcoOffice v1 (Blynk) — REVISI
 * Skripsi: Implementasi Logika Fuzzy Mamdani Untuk Pemantauan Energi Listrik
 *          dan Suhu Ruangan Secara Real-Time Berbasis IoT
 * Penulis: Danke Hidayat (J0304211049) — Pembimbing: Ridwan Siskandar
 *
 * PERUBAHAN REVISI terhadap versi sebelumnya:
 * 1. Fungsi fuzzyTemperatureComfort() dimigrasikan ke DESAIN FINAL skripsi
 *    (desain notebook Colab): 5 himpunan suhu, 3 himpunan kelembaban,
 *    8 aturan Mamdani, implikasi MIN, agregasi MAX, dan defuzzifikasi
 *    CENTROID diskrit pada skor 0-10 (bukan max-membership seperti sebelumnya).
 *    Himpunan Panas diperluas sampai 35 C agar seluruh semesta suhu tercakup.
 *    Keluaran perangkat kini identik dengan software (error 0% pada Tabel 6).
 * 2. Perbaikan satuan energi: pzem.energy() mengembalikan kWh, sehingga
 *    tampilan LCD mode 2 memakai kWh (sebelumnya salah berlabel Wh).
 * 3. Modul energi fuzzyEnergyConsumption() TIDAK diubah (15 aturan,
 *    tie -> NORMAL sudah sesuai desain final).
 * Catatan keamanan: token Blynk dan kredensial WiFi ada di file ini —
 * segera rotasi setelah skripsi dipublikasikan.
 */

#include <WiFi.h> // Koneksi WiFi ESP32
#include <WiFiClient.h> // Klien TCP/IP untuk koneksi internet
#define BLYNK_TEMPLATE_ID "TMPL6eUbLFTuj" // ID template Blynk dari dashboard
#define BLYNK_TEMPLATE_NAME "Energy Monitor" // Nama template Blynk
#include <BlynkSimpleEsp32.h> // Kirim data ke dashboard HP via Blynk
#include <LiquidCrystal_I2C.h> // Tampil teks ke LCD 16x2 I2C
#include <WiFiManager.h> // Setting WiFi otomatis tanpa hardcode
#include <PZEM004Tv30.h> // Baca voltage, current, power, PF, energy
#include <DHT.h> // Baca suhu dan kelembaban dari DHT11
#include <Wire.h> // Komunikasi I2C untuk LCD
#include <math.h> // Fungsi sqrt() untuk hitung daya reaktif

LiquidCrystal_I2C lcd(0x27, 16, 2); // Inisialisasi LCD alamat I2C 0x27 ukuran 16x2
HardwareSerial hwSerial(1); // Gunakan UART1 pada ESP32 untuk komunikasi serial
PZEM004Tv30 pzem(hwSerial, 16, 17); // Inisialisasi PZEM dengan RX pin16 TX pin17

#define DHTPIN 27 // Pin D27 pada ESP32 untuk sensor DHT11
#define DHTTYPE DHT11 // Tipe sensor DHT11
DHT dht(DHTPIN, DHTTYPE); // Inisialisasi objek DHT11
#define TRIGGER_PIN 0 // Pin 0 untuk tombol boot/reset WiFi

char auth[] = "blank"; // Token Blynk untuk autentikasi

bool lcdBacklightState = true; // Status awal backlight LCD menyala
unsigned long previousBlinkMillis = 0; // Waktu terakhir kedip LCD
const unsigned long BLINK_DELAY = 4500; // Delay 4.5 detik sebelum mulai kedip
const unsigned long BLINK_INTERVAL = 250; // Interval kedip setiap 250ms

const float TEMP_SLOPE = 0.923; // Slope regresi linear untuk kalibrasi suhu
const float TEMP_INTERCEPT = -1.618; // Intercept regresi linear untuk kalibrasi suhu
const float HUM_SLOPE = 0.926; // Slope regresi linear untuk kalibrasi kelembaban
const float HUM_INTERCEPT = 18.052; // Intercept regresi linear untuk kalibrasi kelembaban
const float TEMP_BIAS = -3.84; // Koreksi bias suhu: DHT11 lebih tinggi 3.84°C
const float HUM_BIAS = +14.18; // Koreksi bias kelembaban: DHT11 lebih rendah 14.18%

float tempErrors[10]; // Array untuk menyimpan 10 error suhu terakhir
float humErrors[10]; // Array untuk menyimpan 10 error kelembaban terakhir
int errorIndex = 0; // Index circular buffer untuk penyimpanan error

float calibrateTemperature(float rawTemp) { return (TEMP_SLOPE * rawTemp) + TEMP_INTERCEPT; } // Kalibrasi suhu dengan regresi linear
float calibrateHumidity(float rawHum) { return (HUM_SLOPE * rawHum) + HUM_INTERCEPT; } // Kalibrasi kelembaban dengan regresi linear
float calibrateTemperatureSimple(float rawTemp) { return rawTemp + TEMP_BIAS; } // Kalibrasi suhu metode bias sederhana
float calibrateHumiditySimple(float rawHum) { return rawHum + HUM_BIAS; } // Kalibrasi kelembaban metode bias sederhana
void recordCalibrationError(float tempError, float humError) { tempErrors[errorIndex] = tempError; humErrors[errorIndex] = humError; errorIndex = (errorIndex + 1) % 10; } // Catat error kalibrasi ke circular buffer
void getCurrentMAE(float &tempMAE, float &humMAE) { float tempSum=0,humSum=0; int count=0; for(int i=0;i<10;i++){ if(tempErrors[i]!=0){ tempSum+=abs(tempErrors[i]); humSum+=abs(humErrors[i]); count++; } } tempMAE=count>0?tempSum/count:0; humMAE=count>0?humSum/count:0; } // Hitung Mean Absolute Error real-time
float calculateAccuracy(float mae, float range) { return max(0.0, 100.0 - (mae / range * 100.0)); } // Hitung akurasi dalam persentase dari MAE
float zeroIfNan(float value) { return isnan(value) ? 0.0 : value; } // Ubah nilai NaN menjadi 0 untuk menghindari error

float fwTrimf(float x, float a, float b, float c) { // Fungsi keanggotaan segitiga (semantik identik skfuzzy.trimf)
  if (x <= a || x >= c) return (x == b) ? 1.0 : 0.0; // Tepat di kaki bernilai 0, tepat di puncak bernilai 1
  if (x <= b) return (x - a) / (b - a); // Sisi naik segitiga
  return (c - x) / (c - b); // Sisi turun segitiga
}

float fwTrapmf(float x, float a, float b, float c, float d) { // Fungsi keanggotaan trapesium (semantik identik skfuzzy.trapmf)
  if (x <= a || x >= d) return (x >= b && x <= c) ? 1.0 : 0.0; // Di luar bahu: penuh hanya jika plateau menyentuh tepi semesta
  if (x < b) return (b > a) ? (x - a) / (b - a) : 1.0; // Sisi naik trapesium
  if (x <= c) return 1.0; // Dataran penuh (plateau)
  return (d > c) ? (d - x) / (d - c) : 1.0; // Sisi turun trapesium
}

String fuzzyTemperatureComfort(float temp, float humidity) { // Fungsi fuzzy kenyamanan termal (REVISI: desain final skripsi)
  // REVISI: desain lama (4 himpunan suhu, 5 aturan, max-membership) diganti
  // dengan desain final notebook Colab: 5 himpunan suhu, 3 himpunan kelembaban,
  // 8 aturan Mamdani, implikasi MIN, agregasi MAX, defuzzifikasi centroid.
  if (temp < 20.0) temp = 20.0; // Clamp suhu ke batas bawah semesta universal (20 C)
  if (temp > 35.0) temp = 35.0; // Clamp suhu ke batas atas semesta universal (35 C)
  if (humidity < 30.0) humidity = 30.0; // Clamp kelembaban ke batas bawah semesta (30%)
  if (humidity > 90.0) humidity = 90.0; // Clamp kelembaban ke batas atas semesta (90%)

  float cold = fwTrapmf(temp, 20.0, 20.0, 23.0, 24.5); // Derajat keanggotaan Dingin: trapesium [20; 20; 23; 24,5]
  float cool = fwTrimf(temp, 23.0, 24.5, 26.0); // Derajat keanggotaan Sejuk: segitiga [23; 24,5; 26]
  float comfortable = fwTrimf(temp, 24.0, 26.5, 29.0); // Derajat keanggotaan Nyaman: segitiga [24; 26,5; 29]
  float warm = fwTrimf(temp, 26.0, 28.0, 30.0); // Derajat keanggotaan Hangat: segitiga [26; 28; 30]
  float hot = fwTrapmf(temp, 28.0, 29.5, 35.0, 35.0); // Derajat keanggotaan Panas: trapesium [28; 29,5; 35; 35] (REVISI: diperluas s.d. 35 C)

  float dry = fwTrimf(humidity, 30.0, 40.0, 50.0); // Derajat keanggotaan Kering: segitiga [30; 40; 50]
  float comfortable_hum = fwTrimf(humidity, 45.0, 60.0, 75.0); // Derajat keanggotaan Kelembaban Nyaman: segitiga [45; 60; 75]
  float humid = fwTrimf(humidity, 70.0, 80.0, 90.0); // Derajat keanggotaan Lembab: segitiga [70; 80; 90]

  float r1 = cold; // R1: IF Suhu=Cold THEN Kenyamanan=Cold
  float r2 = cool; // R2: IF Suhu=Cool THEN Kenyamanan=Cool
  float r3 = min(comfortable, comfortable_hum); // R3: IF Suhu=Comfortable AND Kelembaban=Comfortable THEN Kenyamanan=Comfortable
  float r4 = min(comfortable, dry); // R4: IF Suhu=Comfortable AND Kelembaban=Dry THEN Kenyamanan=Cool
  float r5 = min(comfortable, humid); // R5: IF Suhu=Comfortable AND Kelembaban=Humid THEN Kenyamanan=Warm
  float r6 = warm; // R6: IF Suhu=Warm THEN Kenyamanan=Warm
  float r7 = hot; // R7: IF Suhu=Hot THEN Kenyamanan=Hot
  float r8 = min(cold, humid); // R8: IF Suhu=Cold AND Kelembaban=Humid THEN Kenyamanan=Cool

  float strengths[5]; // Array kekuatan agregat per kategori output
  strengths[0] = r1; // Agregasi kategori Cold (hanya R1)
  strengths[1] = max(max(r2, r4), r8); // Agregasi kategori Cool (R2, R4, R8 digabung MAX)
  strengths[2] = r3; // Agregasi kategori Comfortable (hanya R3)
  strengths[3] = max(r5, r6); // Agregasi kategori Warm (R5, R6 digabung MAX)
  strengths[4] = r7; // Agregasi kategori Hot (hanya R7)

  float outMF[5][3] = {{0.0, 2.0, 4.0}, {2.0, 3.5, 5.0}, {4.0, 5.5, 7.0}, {5.5, 7.0, 8.5}, {7.0, 8.5, 10.0}}; // Segitiga output: Cold, Cool, Comfortable, Warm, Hot pada skor 0-10
  float num = 0.0; // Pembilang centroid (akumulasi momen)
  float den = 0.0; // Penyebut centroid (akumulasi luas agregat)
  for (int i = 0; i <= 100; i++) { // Sapuan 101 titik diskrit dari 0,0 sampai 10,0
    float x = i * 0.1; // Titik output saat ini (langkah 0,1)
    float agg = 0.0; // Nilai agregat pada titik x
    for (int k = 0; k < 5; k++) { // Gabungkan kelima himpunan output
      if (strengths[k] > 0.0) { // Hanya himpunan yang terbakar yang dihitung
        float mu = fwTrimf(x, outMF[k][0], outMF[k][1], outMF[k][2]); // Derajat keanggotaan output di titik x
        float clipped = min(strengths[k], mu); // Implikasi MIN (pemotongan/clipping)
        if (clipped > agg) agg = clipped; // Agregasi MAX antar himpunan output
      }
    }
    num += agg * x; // Akumulasi momen (agg x x)
    den += agg; // Akumulasi luas agregat
  }
  float score = (den > 0.0) ? (num / den) : 0.0; // Defuzzifikasi centroid: skor kenyamanan 0-10

  if (score <= 2.5) return "COLD"; // Skor 0-2,5 -> Dingin (nilai 1)
  if (score <= 4.0) return "COOL"; // Skor 2,5-4 -> Sejuk (nilai 1)
  if (score <= 6.0) return "COMFORTABLE"; // Skor 4-6 -> Nyaman (nilai 2)
  if (score <= 7.5) return "WARM"; // Skor 6-7,5 -> Hangat (nilai 3)
  return "HOT"; // Skor >7,5 -> Panas (nilai 3)
}

String fuzzyEnergyConsumption(float voltage, float power, float powerFactor, float reactivePower) { // Fuzzy klasifikasi konsumsi energi
  float voltage_low = (voltage <= 200) ? 1.0 : (voltage <= 210) ? (210 - voltage) / 10.0 : 0.0; // Derajat tegangan rendah
  float voltage_normal = (voltage >= 205 && voltage <= 220) ? (voltage - 205) / 15.0 : (voltage > 220 && voltage <= 235) ? (235 - voltage) / 15.0 : 0.0; // Derajat tegangan normal
  float voltage_high = (voltage >= 235) ? 1.0 : (voltage >= 230) ? (voltage - 230) / 5.0 : 0.0; // Derajat tegangan tinggi
  float power_economical = (power <= 20) ? 1.0 : (power <= 30) ? (30 - power) / 10.0 : 0.0; // Derajat daya ekonomis
  float power_normal = (power >= 25 && power <= 47.5) ? (power - 25) / 22.5 : (power > 47.5 && power <= 70) ? (70 - power) / 22.5 : 0.0; // Derajat daya normal
  float power_wasteful = (power >= 80) ? 1.0 : (power >= 60) ? (power - 60) / 20.0 : 0.0; // Derajat daya boros
  float pf_poor = (powerFactor <= 0.5) ? 1.0 : (powerFactor <= 0.6) ? (0.6 - powerFactor) / 0.1 : 0.0; // Derajat faktor daya buruk
  float pf_fair = (powerFactor >= 0.55 && powerFactor <= 0.7) ? (powerFactor - 0.55) / 0.15 : (powerFactor > 0.7 && powerFactor <= 0.85) ? (0.85 - powerFactor) / 0.15 : 0.0; // Derajat faktor daya cukup
  float pf_good = (powerFactor >= 0.90) ? 1.0 : (powerFactor >= 0.80) ? (powerFactor - 0.80) / 0.10 : 0.0; // Derajat faktor daya baik
  float reactive_low = (reactivePower <= 15) ? 1.0 : (reactivePower <= 25) ? (25 - reactivePower) / 10.0 : 0.0; // Derajat daya reaktif rendah
  float reactive_medium = (reactivePower >= 20 && reactivePower <= 37.5) ? (reactivePower - 20) / 17.5 : (reactivePower > 37.5 && reactivePower <= 55) ? (55 - reactivePower) / 17.5 : 0.0; // Derajat daya reaktif sedang
  float reactive_high = (reactivePower >= 60) ? 1.0 : (reactivePower >= 45) ? (reactivePower - 45) / 15.0 : 0.0; // Derajat daya reaktif tinggi
  float economical_strength = 0.0; // Inisialisasi kekuatan kategori ekonomis
  float normal_strength = 0.0; // Inisialisasi kekuatan kategori normal
  float wasteful_strength = 0.0; // Inisialisasi kekuatan kategori boros

  economical_strength = max(economical_strength, min(power_economical, pf_good)); // Rule 1: Daya EKONOMIS & PF BAIK -> EKONOMIS
  economical_strength = max(economical_strength, min(power_economical, reactive_low)); // Rule 2: Daya EKONOMIS & Reaktif RENDAH -> EKONOMIS
  economical_strength = max(economical_strength, min(power_economical, voltage_normal)); // Rule 3: Daya EKONOMIS & Tegangan NORMAL -> EKONOMIS
  economical_strength = max(economical_strength, min(pf_good, reactive_low)); // Rule 4: PF BAIK & Reaktif RENDAH -> EKONOMIS

  normal_strength = max(normal_strength, min(power_normal, pf_fair)); // Rule 5: Daya NORMAL & PF CUKUP -> NORMAL
  normal_strength = max(normal_strength, min(power_normal, voltage_normal)); // Rule 6: Daya NORMAL & Tegangan NORMAL -> NORMAL
  normal_strength = max(normal_strength, min(power_normal, reactive_medium)); // Rule 7: Daya NORMAL & Reaktif SEDANG -> NORMAL
  normal_strength = max(normal_strength, min(pf_fair, voltage_normal)); // Rule 8: PF CUKUP & Tegangan NORMAL -> NORMAL
  normal_strength = max(normal_strength, min(power_economical, pf_poor)); // Rule 9: Daya EKONOMIS & PF BURUK -> NORMAL (kompensasi)

  wasteful_strength = max(wasteful_strength, power_wasteful); // Rule 10: Daya BOROS -> BOROS
  wasteful_strength = max(wasteful_strength, pf_poor); // Rule 11: PF BURUK -> BOROS
  wasteful_strength = max(wasteful_strength, reactive_high); // Rule 12: Reaktif TINGGI -> BOROS
  wasteful_strength = max(wasteful_strength, max(voltage_low, voltage_high)); // Rule 13: Tegangan RENDAH atau TINGGI -> BOROS
  wasteful_strength = max(wasteful_strength, min(power_normal, pf_poor)); // Rule 14: Daya NORMAL & PF BURUK -> BOROS
  wasteful_strength = max(wasteful_strength, min(power_wasteful, reactive_high)); // Rule 15: Daya BOROS & Reaktif TINGGI -> BOROS

  if (economical_strength > normal_strength && economical_strength > wasteful_strength) return "ECONOMICAL"; // Defuzzifikasi: pilih ekonomis jika tertinggi
  else if (wasteful_strength > normal_strength && wasteful_strength > economical_strength) return "WASTEFUL"; // Defuzzifikasi: pilih boros jika tertinggi
  else return "NORMAL"; // Defuzzifikasi: pilih normal jika tidak ada yang dominan
}

void updateBlynkFuzzyStatus(float temperature, float humidity, float voltage, float power, float powerFactor, float reactivePower) { // Kirim status fuzzy ke Blynk
  String tempComfort = fuzzyTemperatureComfort(temperature, humidity); // Dapatkan status kenyamanan termal
  String energyStatus = fuzzyEnergyConsumption(voltage, power, powerFactor, reactivePower); // Dapatkan status konsumsi energi
  int energyNumeric = (energyStatus == "ECONOMICAL") ? 1 : (energyStatus == "NORMAL") ? 2 : 3; // Konversi status ke angka 1,2,3
  Blynk.virtualWrite(V10, tempComfort); // Kirim status kenyamanan ke Blynk virtual pin V10
  Blynk.virtualWrite(V11, energyNumeric); // Kirim status energi ke Blynk virtual pin V11
  Serial.println("=== FUZZY STATUS ==="); // Cetak header ke serial monitor
  Serial.print("Thermal: "); Serial.print(tempComfort); Serial.print(" | "); Serial.print(temperature,1); Serial.print("°C / "); Serial.print(humidity,1); Serial.println("%"); // Cetak status termal
  Serial.print("Energy: "); Serial.print(energyStatus); Serial.print(" | "); Serial.print(voltage,1); Serial.print("V / "); Serial.print(power,1); Serial.print("W / PF:"); Serial.print(powerFactor,2); Serial.println(); // Cetak status energi
}

void handleLCDBlink(unsigned long currentMillis, unsigned long startTime) { // Fungsi kedip backlight LCD
  bool shouldBlink = (currentMillis - startTime >= BLINK_DELAY); // Cek apakah sudah melewati delay awal
  if (shouldBlink && currentMillis - previousBlinkMillis >= BLINK_INTERVAL) { // Jika waktu kedip tercapai
    previousBlinkMillis = currentMillis; // Update waktu terakhir kedip
    lcdBacklightState = !lcdBacklightState; // Balik status backlight
    lcdBacklightState ? lcd.backlight() : lcd.noBacklight(); // Nyalakan atau matikan backlight
  }
}

void stopLCDBlink() { lcdBacklightState = true; lcd.backlight(); } // Hentikan kedip dan nyalakan backlight permanen

void checkBoot() { // Cek tombol boot untuk reset WiFi
  pinMode(TRIGGER_PIN, INPUT_PULLUP); // Set pin tombol sebagai input pull-up
  if (digitalRead(TRIGGER_PIN) == LOW) { // Jika tombol ditekan
    delay(100); // Debounce 100ms
    if (digitalRead(TRIGGER_PIN) == LOW) { // Konfirmasi tekan
      Serial.println("Boot button pressed"); // Log ke serial
      delay(5000); // Tunggu 5 detik
      if (digitalRead(TRIGGER_PIN) == LOW) { // Jika masih ditekan
        Serial.println("Resetting WiFi config..."); // Log reset
        WiFiManager wfm; // Buat objek WiFiManager
        wfm.resetSettings(); // Hapus konfigurasi WiFi tersimpan
        ESP.restart(); // Restart ESP32
      }
    }
  }
}

void showIntroText() { // Tampilkan teks pembuka di LCD
  lcd.clear(); // Bersihkan LCD
  lcd.setCursor((16 - String("EcoOffice").length())/2, 0); lcd.print("EcoOffice"); // Tampilkan judul di tengah baris 0
  lcd.setCursor((16 - String("By Danke Hidayat").length())/2, 1); lcd.print("By Danke Hidayat"); // Tampilkan author di tengah baris 1
}

void setup() { // Fungsi setup dijalankan sekali saat boot
  Serial.begin(115200); // Inisialisasi serial monitor 115200 baud
  Wire.begin(); // Inisialisasi komunikasi I2C
  lcd.init(); // Inisialisasi LCD
  lcd.backlight(); // Nyalakan backlight LCD

  for (int i = 0; i < 10; i++) { tempErrors[i] = 0; humErrors[i] = 0; } // Reset array error

  checkBoot(); // Cek tombol boot untuk reset WiFi
  showIntroText(); // Tampilkan teks intro
  delay(3500); // Tunggu 3.5 detik

  #define AP_PASS "guard14n0ff1ce" // Password untuk portal WiFi manager
  #define AP_SSID "EcoOffice" // Nama SSID untuk portal WiFi manager
  const unsigned long WIFI_TIMEOUT = 60; // Timeout koneksi WiFi 60 detik
  WiFiManager wfm; // Buat objek WiFiManager

  wfm.setConfigPortalTimeout(WIFI_TIMEOUT); // Set timeout portal config
  wfm.setHostname(AP_SSID); // Set hostname ESP32 di jaringan
  wfm.setConnectTimeout(WIFI_TIMEOUT); // Set timeout proses koneksi

  lcd.clear(); lcd.setCursor(0,0); lcd.print("Waiting for WiFi"); lcd.setCursor(0,1); lcd.print("Connection..."); // Tampilkan status mencari WiFi
  unsigned long wifiStartTime = millis(); // Catat waktu mulai koneksi
  bool connected = false; // Flag status koneksi

  while (!connected && (millis() - wifiStartTime < WIFI_TIMEOUT * 1000)) { // Loop sampai timeout
    connected = wfm.autoConnect(AP_SSID, AP_PASS); // Coba konek ke WiFi atau buat portal
    handleLCDBlink(millis(), wifiStartTime); // Kedip LCD selama proses
    delay(100); // Delay 100ms
  }
  if (!connected) { // Jika gagal konek
    Serial.println("Failed to connect to WiFi!"); // Log kegagalan
    stopLCDBlink(); lcd.clear(); lcd.setCursor(0,0); lcd.print("Failed to Connect"); delay(1000); // Tampilkan pesan gagal
    lcd.clear(); lcd.setCursor(0,0); lcd.print("Restarting ESP32..."); delay(3500); ESP.restart(); // Restart ESP32
  } else { // Jika berhasil konek
    Serial.println("WiFi Connected!"); // Log sukses
    stopLCDBlink(); // Hentikan kedip LCD
    lcd.clear(); lcd.setCursor(0,0); lcd.print("WiFi Connected!"); lcd.setCursor(0,1); lcd.print("IP:" + WiFi.localIP().toString()); delay(3000); // Tampilkan IP address
  }

  Blynk.begin(auth, WiFi.SSID().c_str(), WiFi.psk().c_str(), "iot.serangkota.go.id", 8080); // Inisialisasi Blynk ke server lokal
  hwSerial.begin(9600, SERIAL_8N1, 16, 17); // Inisialisasi serial untuk PZEM-004T
  dht.begin(); // Inisialisasi sensor DHT11

  lcd.clear(); lcd.setCursor(0,0); lcd.print("System Ready!"); lcd.setCursor(0,1); lcd.print("Monitoring..."); delay(1500); // Tampilkan siap monitoring
  Serial.println("System Started - Fuzzy Energy & Temp Monitoring"); // Log startup ke serial
}

void loop() { // Fungsi loop dijalankan berulang
  Blynk.run(); // Jalankan Blynk (keep alive dan proses data)
  static unsigned long previousMillis = 0; // Waktu terakhir baca sensor
  const unsigned long SENSOR_INTERVAL = 3000; // Interval baca sensor 3 detik
  if (millis() - previousMillis >= SENSOR_INTERVAL) { // Jika sudah waktunya baca sensor
    previousMillis = millis(); // Update waktu terakhir
    float voltage = zeroIfNan(pzem.voltage()); // Baca tegangan dari PZEM
    float current = zeroIfNan(pzem.current()); // Baca arus dari PZEM
    float power = zeroIfNan(pzem.power()); // Baca daya dari PZEM
    float energyKwh = zeroIfNan(pzem.energy()); // Baca energi dari PZEM (satuan kWh — REVISI: sebelumnya salah dinamai Wh)
    float frequency = zeroIfNan(pzem.frequency()); // Baca frekuensi dari PZEM
    float pf = zeroIfNan(pzem.pf()); // Baca faktor daya dari PZEM
    float humidity = zeroIfNan(dht.readHumidity()); // Baca kelembaban dari DHT11
    float temperature = zeroIfNan(dht.readTemperature()); // Baca suhu dari DHT11
    float calibratedTemp = calibrateTemperature(temperature); // Kalibrasi suhu dengan regresi
    float calibratedHum = calibrateHumidity(humidity); // Kalibrasi kelembaban dengan regresi
    float simpleTemp = calibrateTemperatureSimple(temperature); // Kalibrasi suhu metode bias
    float simpleHum = calibrateHumiditySimple(humidity); // Kalibrasi kelembaban metode bias
    
    recordCalibrationError(abs(calibratedTemp - simpleTemp), abs(calibratedHum - simpleHum)); // Catat error kalibrasi

    float apparentPower = (pf == 0) ? 0 : power / pf; // Hitung daya semu
    float reactivePower = (pf == 0) ? 0 : sqrt(sq(apparentPower) - sq(power)); // Hitung daya reaktif
    updateBlynkFuzzyStatus(calibratedTemp, calibratedHum, voltage, power, pf, reactivePower); // Kirim status fuzzy ke Blynk
    static int displayMode = 0; // Mode tampilan LCD (0-4)
    lcd.clear(); // Bersihkan LCD
    
    switch(displayMode) { // Tampilkan data sesuai mode
      case 0: lcd.setCursor(0,0); lcd.print("Voltage: " + String(voltage,1) + "V"); lcd.setCursor(0,1); // Mode 0 baris atas: tegangan (dilanjutkan baris bawah)
      lcd.print("Current: " + String(current,3) + "A"); break; // Mode 0: Tegangan & Arus
      case 1: lcd.setCursor(0,0); lcd.print("Power: " + String(power,1) + "W"); lcd.setCursor(0,1); // Mode 1 baris atas: daya aktif (dilanjutkan baris bawah)
      lcd.print("Freq: " + String(frequency,1) + "Hz"); break; // Mode 1: Daya & Frekuensi
      case 2: lcd.setCursor(0,0); lcd.print("E:" + String(energyKwh,3) + "kWh"); lcd.setCursor(0,1); // Mode 2 baris atas: energi kWh (dilanjutkan baris bawah)
       lcd.print("PF: " + String(pf,2)); break; // Mode 2: Energi (kWh — REVISI) & Faktor Daya
      case 3: lcd.setCursor(0,0); lcd.print("Temp: " + String(calibratedTemp,1) + "C"); lcd.setCursor(0,1); // Mode 3 baris atas: suhu terkalibrasi (dilanjutkan baris bawah)
      lcd.print("Humidity: " + String(calibratedHum,1) + "%"); break; // Mode 3: Suhu & Kelembaban
      case 4: lcd.setCursor(0,0); lcd.print("Comfort:" + fuzzyTemperatureComfort(calibratedTemp, calibratedHum)); // Mode 4 baris atas: status kenyamanan termal (dilanjutkan baris bawah)
      lcd.setCursor(0,1); lcd.print("Energy:" + fuzzyEnergyConsumption(voltage, power, pf, reactivePower)); break; // Mode 4: Status Fuzzy
    }
    displayMode = (displayMode + 1) % 5; // Ganti mode tampilan berikutnya
    Blynk.virtualWrite(V0, voltage); // Kirim tegangan ke pin virtual V0
    Blynk.virtualWrite(V1, current); // Kirim arus ke pin virtual V1
    Blynk.virtualWrite(V2, power); // Kirim tegangan, arus, daya ke Blynk
    
    Blynk.virtualWrite(V3, pf); // Kirim faktor daya ke pin virtual V3
    Blynk.virtualWrite(V4, apparentPower); // Kirim daya semu ke pin virtual V4
    Blynk.virtualWrite(V5, energyKwh); // Kirim PF, daya semu, energi (kWh) ke Blynk

    Blynk.virtualWrite(V6, frequency); // Kirim frekuensi ke pin virtual V6
    Blynk.virtualWrite(V7, reactivePower); // Kirim daya reaktif ke pin virtual V7
    Blynk.virtualWrite(V8, calibratedTemp); // Kirim suhu terkalibrasi ke pin virtual V8
    Blynk.virtualWrite(V9, calibratedHum); // Kirim frekuensi, reaktif, suhu, kelembaban ke Blynk
  }
}
