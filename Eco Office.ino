#include <WiFi.h> // Koneksi WiFi ESP32 (station dan access point)
#include <WiFiClient.h> // Klien TCP/IP polos untuk koneksi MQTT/Blynk
#include <WiFiClientSecure.h> // Klien TLS/HTTPS untuk unduh OTA dan lapor hasil
#include <HTTPClient.h> // Klien HTTP untuk request GET/POST ke server backend
#include <HTTPUpdate.h> // Pembaruan firmware over-the-air (OTA) dari URL HTTPS

#define BLYNK_TEMPLATE_ID "TMPL6eUbLFTuj" // ID template perangkat pada dasbor Blynk
#define BLYNK_TEMPLATE_NAME "Energy Monitor" // Nama template Blynk (monitor energi)
#include <BlynkSimpleEsp32.h> // Pustaka Blynk khusus ESP32 untuk kirim data ke aplikasi HP
#include <LiquidCrystal_I2C.h> // Pustaka tampilan teks untuk LCD 16x2 via I2C
#include <WiFiManager.h> // Portal konfigurasi WiFi otomatis tanpa hardcode SSID
#include <PZEM004Tv30.h> // Pustaka sensor PZEM-004T v3: tegangan, arus, daya, PF, frekuensi, energi
#include <DHT.h> // Pustaka sensor DHT11: suhu dan kelembaban
#include <Wire.h> // Komunikasi I2C (dipakai LCD)
#include <math.h> // Fungsi matematika: sqrt() untuk hitung daya reaktif

#include <PubSubClient.h> // Klien MQTT: publish telemetri dan subscribe perintah
#include <ArduinoJson.h> // Parse dan susun dokumen JSON (telemetri, perintah, hasil OTA)

// ── Konfigurasi MQTT / server backend ─────────────────────────────────────
#define MQTT_BROKER "blank" // Alamat IP broker MQTT
#define MQTT_PORT 1883 // Port standar MQTT (tanpa TLS)
#define MQTT_USER "selene" // Nama pengguna autentikasi broker MQTT
#define MQTT_PASSWORD "blank" // Kata sandi broker MQTT (rotasi setelah publikasi!)
#define NODE_ID "office-main" // Identitas unik node/perangkat ini di sistem
#define MQTT_PUBLISH_INTERVAL 30000 // Interval kirim telemetri MQTT: 30 detik (ms)

// Lapor hasil OTA ke server backend (rute ini saat ini tanpa autentikasi)
#define SELENE_API_BASE "https://selene.dankehidayat.my.id/api" // URL dasar API backend firmware

// ── Perangkat keras ───────────────────────────────────────────────────────
LiquidCrystal_I2C lcd(0x27, 16, 2); // Inisialisasi LCD alamat I2C 0x27, ukuran 16 kolom x 2 baris
HardwareSerial hwSerial(1); // Gunakan UART1 ESP32 untuk komunikasi serial PZEM
PZEM004Tv30 pzem(hwSerial, 16, 17); // Inisialisasi PZEM-004T: RX pin 16, TX pin 17

#define DHTPIN 27 // Pin GPIO 27 untuk jalur data sensor DHT11
#define DHTTYPE DHT11 // Tipe sensor yang dipakai adalah DHT11
DHT dht(DHTPIN, DHTTYPE); // Inisialisasi objek sensor DHT11
#define TRIGGER_PIN 0 // Pin GPIO 0 = tombol BOOT untuk reset konfigurasi WiFi

char auth[] = "blank"; // Token autentikasi Blynk (rotasi setelah publikasi!)

WiFiClient mqttWiFiClient; // Klien TCP/IP yang dipakai klien MQTT
PubSubClient mqtt(mqttWiFiClient); // Objek klien MQTT di atas klien TCP/IP

unsigned long lastMqttPublish = 0; // Waktu (ms) terakhir telemetri MQTT dikirim

bool lcdBacklightState = true; // Status awal backlight LCD: menyala
unsigned long previousBlinkMillis = 0; // Waktu terakhir backlight LCD berkedip
const unsigned long BLINK_DELAY = 4500; // Jeda 4,5 detik sebelum LCD mulai berkedip saat menunggu WiFi
const unsigned long BLINK_INTERVAL = 250; // Interval kedip backlight setiap 250 ms

const float TEMP_SLOPE = 0.923; // Slope regresi linier kalibrasi suhu (dari 34 titik data pre-kalibrasi)
const float TEMP_INTERCEPT = -1.618; // Intercept regresi linier kalibrasi suhu
const float HUM_SLOPE = 0.926; // Slope regresi linier kalibrasi kelembaban
const float HUM_INTERCEPT = 18.052; // Intercept regresi linier kalibrasi kelembaban
const float TEMP_BIAS = -3.84; // Koreksi bias sederhana suhu: DHT11 membaca 3,84 C lebih tinggi dari HTC-1
const float HUM_BIAS = +14.18; // Koreksi bias sederhana kelembaban: DHT11 membaca 14,18% lebih rendah dari HTC-1

float tempErrors[10]; // Circular buffer 10 selisih error suhu terakhir (diagnostik kalibrasi)
float humErrors[10]; // Circular buffer 10 selisih error kelembaban terakhir
int errorIndex = 0; // Indeks tulis circular buffer error

// Flag: OTA dijalankan di luar callback MQTT (lebih aman karena bersifat blocking)
bool otaPending = false; // Penanda ada pembaruan firmware yang menunggu dieksekusi
String otaUrl = ""; // URL firmware yang akan diunduh
unsigned long lastOtaPullCheck = 0; // Waktu terakhir pengecekan firmware ke server
const unsigned long OTA_PULL_INTERVAL_MS = 60000; // Jeda cek firmware via HTTP tiap 60 detik (cadangan bila push MQTT terlewat)

float calibrateTemperature(float rawTemp) { return (TEMP_SLOPE * rawTemp) + TEMP_INTERCEPT; } // Kalibrasi suhu dengan persamaan regresi linier
float calibrateHumidity(float rawHum) { return (HUM_SLOPE * rawHum) + HUM_INTERCEPT; } // Kalibrasi kelembaban dengan persamaan regresi linier
float calibrateTemperatureSimple(float rawTemp) { return rawTemp + TEMP_BIAS; } // Kalibrasi suhu metode bias sederhana (untuk diagnostik)
float calibrateHumiditySimple(float rawHum) { return rawHum + HUM_BIAS; } // Kalibrasi kelembaban metode bias sederhana (untuk diagnostik)
void recordCalibrationError(float tempError, float humError) { // Catat selisih metode regresi vs metode bias ke circular buffer
  tempErrors[errorIndex] = tempError; // Simpan selisih error suhu pada indeks berjalan
  humErrors[errorIndex] = humError; // Simpan selisih error kelembaban pada indeks berjalan
  errorIndex = (errorIndex + 1) % 10; // Geser indeks, kembali ke 0 setelah penuh
}
void getCurrentMAE(float &tempMAE, float &humMAE) { // Hitung Mean Absolute Error real-time dari buffer error
  float tempSum = 0, humSum = 0; // Akumulator jumlah absolut error suhu dan kelembaban
  int count = 0; // Banyaknya sampel error yang valid (bukan nol)
  for (int i = 0; i < 10; i++) { // Telusuri seluruh isi circular buffer
    if (tempErrors[i] != 0) { // Hanya sampel yang pernah diisi yang dihitung
      tempSum += abs(tempErrors[i]); // Tambahkan absolut error suhu
      humSum += abs(humErrors[i]); // Tambahkan absolut error kelembaban
      count++; // Tambah penghitung sampel valid
    }
  }
  tempMAE = count > 0 ? tempSum / count : 0; // MAE suhu = rata-rata absolut error (0 bila belum ada data)
  humMAE = count > 0 ? humSum / count : 0; // MAE kelembaban = rata-rata absolut error
}
float calculateAccuracy(float mae, float range) { // Konversi MAE menjadi persentase akurasi terhadap rentang ukur
  return max(0.0, 100.0 - (mae / range * 100.0)); // Akurasi = 100% - (MAE / rentang x 100%), minimum 0%
}
float zeroIfNan(float value) { return isnan(value) ? 0.0 : value; } // Ubah nilai NaN (gagal baca sensor) menjadi 0 agar aman dihitung

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

String fuzzyEnergyConsumption(float voltage, float power, float powerFactor, float reactivePower) { // Fuzzy klasifikasi konsumsi energi (TIDAK diubah pada revisi)
  float voltage_low = (voltage <= 200) ? 1.0 : (voltage <= 210) ? (210 - voltage) / 10.0 : 0.0; // Derajat keanggotaan Tegangan Rendah: penuh <=200 V, turun sampai 210 V
  float voltage_normal = (voltage >= 205 && voltage <= 220) ? (voltage - 205) / 15.0 // Derajat Tegangan Normal: naik 205-220 V,
                         : (voltage > 220 && voltage <= 235) ? (235 - voltage) / 15.0 : 0.0; // turun 220-235 V (puncak 220 V)
  float voltage_high = (voltage >= 235) ? 1.0 : (voltage >= 230) ? (voltage - 230) / 5.0 : 0.0; // Derajat Tegangan Tinggi: naik dari 230 V, penuh >=235 V
  float power_economical = (power <= 20) ? 1.0 : (power <= 30) ? (30 - power) / 10.0 : 0.0; // Derajat Daya Ekonomis: penuh <=20 W, turun sampai 30 W
  float power_normal = (power >= 25 && power <= 47.5) ? (power - 25) / 22.5 // Derajat Daya Normal: naik 25-47,5 W,
                       : (power > 47.5 && power <= 70) ? (70 - power) / 22.5 : 0.0; // turun 47,5-70 W (puncak 47,5 W)
  float power_wasteful = (power >= 80) ? 1.0 : (power >= 60) ? (power - 60) / 20.0 : 0.0; // Derajat Daya Boros: naik dari 60 W, penuh >=80 W
  float pf_poor = (powerFactor <= 0.5) ? 1.0 : (powerFactor <= 0.6) ? (0.6 - powerFactor) / 0.1 : 0.0; // Derajat PF Buruk: penuh <=0,5, turun sampai 0,6
  float pf_fair = (powerFactor >= 0.55 && powerFactor <= 0.7) ? (powerFactor - 0.55) / 0.15 // Derajat PF Cukup: naik 0,55-0,7,
                  : (powerFactor > 0.7 && powerFactor <= 0.85) ? (0.85 - powerFactor) / 0.15 : 0.0; // turun 0,7-0,85 (puncak 0,7)
  float pf_good = (powerFactor >= 0.90) ? 1.0 : (powerFactor >= 0.80) ? (powerFactor - 0.80) / 0.10 : 0.0; // Derajat PF Baik: naik dari 0,8, penuh >=0,9
  float reactive_low = (reactivePower <= 15) ? 1.0 : (reactivePower <= 25) ? (25 - reactivePower) / 10.0 : 0.0; // Derajat Reaktif Rendah: penuh <=15 VAR, turun sampai 25 VAR
  float reactive_medium = (reactivePower >= 20 && reactivePower <= 37.5) ? (reactivePower - 20) / 17.5 // Derajat Reaktif Sedang: naik 20-37,5 VAR,
                          : (reactivePower > 37.5 && reactivePower <= 55) ? (55 - reactivePower) / 17.5 : 0.0; // turun 37,5-55 VAR (puncak 37,5)
  float reactive_high = (reactivePower >= 60) ? 1.0 : (reactivePower >= 45) ? (reactivePower - 45) / 15.0 : 0.0; // Derajat Reaktif Tinggi: naik dari 45 VAR, penuh >=60 VAR
  float economical_strength = 0.0; // Inisialisasi kekuatan agregat kategori Ekonomis
  float normal_strength = 0.0; // Inisialisasi kekuatan agregat kategori Normal
  float wasteful_strength = 0.0; // Inisialisasi kekuatan agregat kategori Boros

  economical_strength = max(economical_strength, min(power_economical, pf_good)); // R1: Daya Ekonomis AND PF Baik -> Ekonomis
  economical_strength = max(economical_strength, min(power_economical, reactive_low)); // R2: Daya Ekonomis AND Reaktif Rendah -> Ekonomis
  economical_strength = max(economical_strength, min(power_economical, voltage_normal)); // R3: Daya Ekonomis AND Tegangan Normal -> Ekonomis
  economical_strength = max(economical_strength, min(pf_good, reactive_low)); // R4: PF Baik AND Reaktif Rendah -> Ekonomis
  normal_strength = max(normal_strength, min(power_normal, pf_fair)); // R5: Daya Normal AND PF Cukup -> Normal
  normal_strength = max(normal_strength, min(power_normal, voltage_normal)); // R6: Daya Normal AND Tegangan Normal -> Normal
  normal_strength = max(normal_strength, min(power_normal, reactive_medium)); // R7: Daya Normal AND Reaktif Sedang -> Normal
  normal_strength = max(normal_strength, min(pf_fair, voltage_normal)); // R8: PF Cukup AND Tegangan Normal -> Normal
  normal_strength = max(normal_strength, min(power_economical, pf_poor)); // R9: Daya Ekonomis AND PF Buruk -> Normal (aturan kompensasi)
  wasteful_strength = max(wasteful_strength, power_wasteful); // R10: Daya Boros -> Boros
  wasteful_strength = max(wasteful_strength, pf_poor); // R11: PF Buruk -> Boros
  wasteful_strength = max(wasteful_strength, reactive_high); // R12: Reaktif Tinggi -> Boros
  wasteful_strength = max(wasteful_strength, max(voltage_low, voltage_high)); // R13: Tegangan Rendah OR Tegangan Tinggi -> Boros
  wasteful_strength = max(wasteful_strength, min(power_normal, pf_poor)); // R14: Daya Normal AND PF Buruk -> Boros
  wasteful_strength = max(wasteful_strength, min(power_wasteful, reactive_high)); // R15: Daya Boros AND Reaktif Tinggi -> Boros

  if (economical_strength > normal_strength && economical_strength > wasteful_strength) return "ECONOMICAL"; // Defuzzifikasi max-membership: Ekonomis hanya menang bila strictly terbesar
  if (wasteful_strength > normal_strength && wasteful_strength > economical_strength) return "WASTEFUL"; // Boros hanya menang bila strictly terbesar
  return "NORMAL"; // Bila terjadi seri (tie) antar kategori, hasilnya Normal
}

void updateBlynkFuzzyStatus(float temperature, float humidity, float voltage, float power, // Deklarasi fungsi kirim status fuzzy ke Blynk (baris parameter pertama)
                            float powerFactor, float reactivePower) { // Kirim status kedua modul fuzzy ke aplikasi Blynk
  String tempComfort = fuzzyTemperatureComfort(temperature, humidity); // Hitung status kenyamanan termal (modul 1)
  String energyStatus = fuzzyEnergyConsumption(voltage, power, powerFactor, reactivePower); // Hitung status konsumsi energi (modul 2)
  int energyNumeric = (energyStatus == "ECONOMICAL") ? 1 : (energyStatus == "NORMAL") ? 2 : 3; // Konversi status energi ke angka 1/2/3 untuk widget
  Blynk.virtualWrite(V10, tempComfort); // Kirim label kenyamanan termal ke pin virtual V10
  Blynk.virtualWrite(V11, energyNumeric); // Kirim angka status energi ke pin virtual V11
}

void handleLCDBlink(unsigned long currentMillis, unsigned long startTime) { // Kedipkan backlight LCD sebagai indikator menunggu WiFi
  bool shouldBlink = (currentMillis - startTime >= BLINK_DELAY); // Kedip hanya dimulai setelah jeda awal terpenuhi
  if (shouldBlink && currentMillis - previousBlinkMillis >= BLINK_INTERVAL) { // Jika sudah waktunya berkedip
    previousBlinkMillis = currentMillis; // Perbarui waktu kedip terakhir
    lcdBacklightState = !lcdBacklightState; // Balik status backlight
    lcdBacklightState ? lcd.backlight() : lcd.noBacklight(); // Nyalakan atau matikan backlight sesuai status
  }
}

void stopLCDBlink() { // Hentikan kedip dan nyalakan backlight secara permanen
  lcdBacklightState = true; // Kembalikan status backlight ke menyala
  lcd.backlight(); // Nyalakan backlight LCD
}

void checkBoot() { // Cek tombol BOOT (GPIO 0) saat menyala untuk mereset konfigurasi WiFi
  pinMode(TRIGGER_PIN, INPUT_PULLUP); // Set pin tombol sebagai input dengan pull-up internal
  if (digitalRead(TRIGGER_PIN) == LOW) { // Jika tombol sedang ditekan saat boot
    delay(100); // Debounce 100 ms agar tekanan stabil
    if (digitalRead(TRIGGER_PIN) == LOW) { // Konfirmasi tombol masih ditekan
      Serial.println("Boot button pressed"); // Log ke serial monitor
      delay(5000); // Tunggu 5 detik memberi waktu pengguna memutuskan
      if (digitalRead(TRIGGER_PIN) == LOW) { // Jika tombol ditahan penuh 5 detik
        Serial.println("Resetting WiFi config..."); // Log proses reset konfigurasi
        WiFiManager wfm; // Buat objek WiFiManager sementara
        wfm.resetSettings(); // Hapus seluruh konfigurasi WiFi tersimpan
        ESP.restart(); // Restart ESP32 agar masuk portal konfigurasi baru
      }
    }
  }
}

void showIntroText() { // Tampilkan teks pembuka identitas alat di LCD
  lcd.clear(); // Bersihkan layar LCD
  lcd.setCursor((16 - String("EcoOffice").length()) / 2, 0); // Posisikan kursor di tengah baris atas
  lcd.print("EcoOffice"); // Cetak nama alat
  lcd.setCursor((16 - String("By Danke Hidayat").length()) / 2, 1); // Posisikan kursor di tengah baris bawah
  lcd.print("By Danke Hidayat"); // Cetak nama pembuat
}

// ── Jadwalkan OTA dari push MQTT atau pull HTTP ───────────────────────────
void scheduleOta(const String &url) { // Menandai OTA untuk dieksekusi di loop() (bukan di callback)
  if (url.length() < 8) { // Tolak URL kosong atau terlalu pendek (tidak valid)
    Serial.println("OTA: reject empty/short url"); // Log penolakan URL
    return; // Keluar tanpa menjadwalkan apa pun
  }
  if (otaPending && otaUrl == url) { // Jika URL yang sama sudah masuk antrean
    Serial.println("OTA: already queued for same url"); // Log duplikasi
    return; // Tidak perlu menjadwalkan ulang
  }
  otaUrl = url; // Simpan URL firmware yang akan diunduh
  otaPending = true; // Aktifkan penanda OTA menunggu eksekusi
  Serial.println("OTA: scheduled"); // Log OTA berhasil dijadwalkan
  Serial.println(otaUrl); // Cetak URL firmware ke serial
  lcd.clear(); // Bersihkan layar LCD
  lcd.setCursor(0, 0); // Pindah kursor ke baris atas
  lcd.print("OTA queued..."); // Tampilkan pesan OTA masuk antrean
}

// ── Lapor hasil OTA ke API server backend ─────────────────────────────────
void reportOtaResult(bool success, const String &errMsg) { // Kirim hasil OTA (sukses/gagal) via HTTP POST
  WiFiClientSecure client; // Buat klien TLS untuk koneksi HTTPS
  client.setInsecure(); // Abaikan verifikasi sertifikat (produksi: pasang CA/pinning)
  client.setTimeout(20); // Timeout koneksi klien 20 detik
  HTTPClient http; // Buat objek klien HTTP
  http.setTimeout(20000); // Timeout request HTTP 20.000 ms
  String url = String(SELENE_API_BASE) + "/firmware/result"; // Susun URL endpoint laporan hasil OTA
  if (!http.begin(client, url)) { // Jika koneksi ke endpoint gagal dibuka
    Serial.println("OTA: cannot begin result POST"); // Log kegagalan koneksi
    return; // Keluar tanpa mengirim laporan
  }
  http.addHeader("Content-Type", "application/json"); // Set header tipe konten JSON
  StaticJsonDocument<256> doc; // Buat dokumen JSON kapasitas 256 byte
  doc["nodeId"] = NODE_ID; // Isi identitas node pelapor
  doc["success"] = success; // Isi status keberhasilan OTA
  if (!success && errMsg.length()) doc["error"] = errMsg; // Sertakan pesan error hanya bila gagal dan pesannya ada
  String body; // Penampung hasil serialisasi JSON
  serializeJson(doc, body); // Ubah dokumen JSON menjadi string
  int code = http.POST(body); // Kirim request POST dan simpan kode status HTTP
  Serial.printf("OTA: result POST HTTP %d\n", code); // Log kode status HTTP
  http.end(); // Tutup koneksi HTTP dan bebaskan sumber daya
}

/**
 * Cadangan pull: bila backend sudah menyiapkan firmware tetapi push MQTT
 * terlewat (perangkat sibuk atau sempat terputus), GET /api/firmware/check/<node>
 * tetap menjadwalkan unduhan.
 */
void checkPendingOtaPull() { // Cek berkala ke server apakah ada firmware baru menunggu
  if (otaPending) return; // Lewati bila sudah ada OTA yang menunggu dieksekusi
  if (WiFi.status() != WL_CONNECTED) return; // Lewati bila WiFi belum terhubung

  String base = String(SELENE_API_BASE); // Ambil URL dasar API backend
  if (base.indexOf("YOUR_DOMAIN") >= 0 || base.length() < 12) { // Jika URL masih placeholder/belum dikonfigurasi
    // Kredensial belum diisi pada build ini
    return; // Keluar tanpa mengecek
  }

  WiFiClientSecure client; // Buat klien TLS untuk HTTPS
  client.setInsecure(); // Abaikan verifikasi sertifikat (produksi: pasang CA/pinning)
  client.setTimeout(15); // Timeout koneksi klien 15 detik
  HTTPClient http; // Buat objek klien HTTP
  http.setTimeout(15000); // Timeout request HTTP 15.000 ms
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS); // Ikuti redirect hanya ke host yang sama (lebih aman)

  String url = base + "/firmware/check/" + String(NODE_ID); // Susun URL pengecekan firmware untuk node ini
  Serial.print("OTA: pull check "); // Log awal pengecekan
  Serial.println(url); // Cetak URL yang dicek

  if (!http.begin(client, url)) { // Jika koneksi gagal dibuka
    Serial.println("OTA: pull check begin failed"); // Log kegagalan koneksi
    return; // Keluar dari fungsi
  }

  int code = http.GET(); // Jalankan request GET dan ambil kode status HTTP
  if (code != 200) { // Jika server tidak merespons sukses
    Serial.printf("OTA: pull check HTTP %d\n", code); // Log kode status error
    http.end(); // Tutup koneksi HTTP
    return; // Keluar dari fungsi
  }

  String body = http.getString(); // Ambil isi respons JSON dari server
  http.end(); // Tutup koneksi HTTP

  StaticJsonDocument<384> doc; // Buat dokumen JSON kapasitas 384 byte untuk parse
  if (deserializeJson(doc, body)) { // Jika parse JSON gagal
    Serial.println("OTA: pull check JSON parse failed"); // Log kegagalan parse
    return; // Keluar dari fungsi
  }

  if (!doc["pending"].as<bool>()) { // Jika server menyatakan tidak ada firmware menunggu
    Serial.println("OTA: no pending firmware on server"); // Log tidak ada pembaruan
    return; // Keluar dari fungsi
  }

  const char *dl = doc["url"]; // Ambil URL unduhan firmware dari respons
  if (!dl || strlen(dl) < 8) { // Jika URL tidak ada atau terlalu pendek
    Serial.println("OTA: pending but missing url"); // Log firmware ada tetapi URL hilang
    return; // Keluar dari fungsi
  }

  Serial.printf("OTA: server has pending firmware (%d bytes)\n", // Log tersedia firmware baru di server...
                doc["size"].as<int>()); // ...beserta ukuran firmware dalam byte
  scheduleOta(String(dl)); // Jadwalkan unduhan firmware
}

// ── Jalankan OTA (blocking; dipanggil dari loop, bukan dari callback) ─────
void performOtaUpdate(const String &url) { // Eksekusi unduhan dan pemasangan firmware baru
  Serial.println("========================================"); // Garis pembatas log
  Serial.println("OTA: starting HTTPS firmware update"); // Log mulai pembaruan firmware
  Serial.println(url); // Cetak URL firmware
  Serial.printf("OTA: free heap before = %u\n", ESP.getFreeHeap()); // Log sisa memori heap sebelum unduh
  Serial.println("========================================"); // Garis pembatas log

  lcd.clear(); // Bersihkan layar LCD
  lcd.setCursor(0, 0); // Pindah kursor ke baris atas
  lcd.print("OTA Update..."); // Tampilkan pesan pembaruan berjalan
  lcd.setCursor(0, 1); // Pindah kursor ke baris bawah
  lcd.print("Preparing..."); // Tampilkan pesan persiapan

  // Bebaskan soket / RAM sebelum unduh TLS (MQTT dan Blynk menahan koneksi)
  mqtt.disconnect(); // Putuskan koneksi MQTT agar memori tersedia
  delay(200); // Jeda singkat agar pelepasan koneksi selesai

  lcd.setCursor(0, 1); // Pindah kursor ke baris bawah
  lcd.print("Downloading..."); // Tampilkan pesan sedang mengunduh

  WiFiClientSecure client; // Buat klien TLS untuk unduhan HTTPS
  client.setInsecure();  // Abaikan verifikasi sertifikat (produksi: pasang CA/pinning)
  client.setTimeout(120);  // Timeout klien dalam detik (WiFiClientSecure ESP32)

  httpUpdate.rebootOnUpdate(false);  // Jangan restart otomatis; restart dilakukan manual setelah lapor hasil
  httpUpdate.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS); // Ikuti redirect hanya ke host yang sama
  // Catatan: closeConnectionsOnUpdate() tidak tersedia di semua core Arduino ESP32
  // (mis. 3.3.x). MQTT sudah diputuskan di atas sebelum HTTPS dimulai.

  httpUpdate.onProgress([](int cur, int total) { // Callback progres unduhan firmware
    static int lastPct = -1; // Simpan persentase terakhir yang ditampilkan
    int pct = (total > 0) ? (cur * 100) / total : 0; // Hitung persentase unduhan saat ini
    if (pct != lastPct && (pct % 10 == 0 || pct == 100)) { // Tampilkan hanya tiap kelipatan 10% dan saat selesai
      lastPct = pct; // Perbarui persentase terakhir
      Serial.printf("OTA: progress %d%% (%d/%d)\n", pct, cur, total); // Log progres ke serial
      lcd.setCursor(0, 1); // Pindah kursor ke baris bawah LCD
      lcd.print("DL "); // Awali baris dengan label unduh
      lcd.print(pct); // Cetak angka persentase
      lcd.print("%          "); // Cetak simbol persen dan spasi pembersih sisa karakter
    }
  }); // Akhiri pendaftaran lambda callback progres unduhan

  t_httpUpdate_return ret = httpUpdate.update(client, url); // Jalankan unduhan + penulisan firmware ke partisi OTA

  if (ret == HTTP_UPDATE_OK) { // Jika pembaruan sukses
    Serial.println("OTA: SUCCESS, rebooting"); // Log sukses dan akan restart
    lcd.clear(); // Bersihkan layar LCD
    lcd.setCursor(0, 0); // Pindah kursor ke baris atas
    lcd.print("OTA Success"); // Tampilkan pesan sukses
    lcd.setCursor(0, 1); // Pindah kursor ke baris bawah
    lcd.print("Rebooting..."); // Tampilkan pesan restart
    reportOtaResult(true, ""); // Lapor sukses ke server backend
    delay(1500); // Jeda agar pesan LCD terbaca
    ESP.restart(); // Restart ESP32 untuk menjalankan firmware baru
  }

  int errCode = httpUpdate.getLastError(); // Ambil kode error terakhir pembaruan
  String err = httpUpdate.getLastErrorString(); // Ambil pesan error terakhir pembaruan
  Serial.printf("OTA: FAILED (%d) %s\n", errCode, err.c_str()); // Log kegagalan beserta detailnya
  Serial.printf("OTA: free heap after = %u\n", ESP.getFreeHeap()); // Log sisa memori heap setelah gagal
  lcd.clear(); // Bersihkan layar LCD
  lcd.setCursor(0, 0); // Pindah kursor ke baris atas
  lcd.print("OTA Failed"); // Tampilkan pesan gagal
  lcd.setCursor(0, 1); // Pindah kursor ke baris bawah
  lcd.print(err.substring(0, 16)); // Tampilkan 16 karakter pertama pesan error
  reportOtaResult(false, String(errCode) + " " + err); // Lapor kegagalan ke server backend
  delay(3000); // Jeda agar pesan LCD terbaca
  // MQTT akan tersambung ulang pada loop berikutnya lewat connectMQTT()
}

// ── Sambungkan MQTT ───────────────────────────────────────────────────────
bool connectMQTT() { // Hubungkan (atau pastikan terhubung) klien MQTT ke broker
  if (mqtt.connected()) return true; // Jika sudah terhubung, tidak perlu apa-apa

  String clientId = "selene-" + String(NODE_ID) + "-" + String(random(0xffff), HEX); // Susun ID klien unik per sesi
  Serial.print("MQTT: Menghubungkan ke broker..."); // Log proses koneksi

  String lwTopic = "selene/" + String(NODE_ID) + "/status"; // Topik status untuk Last Will dan laporan online
  String lwMessage = "{\"status\":\"offline\"}"; // Pesan Last Will: dikirim broker bila perangkat putus mendadak

  if (mqtt.connect(clientId.c_str(), MQTT_USER, MQTT_PASSWORD, lwTopic.c_str(), 1, true, // Hubungkan MQTT dengan ID klien, kredensial, dan topik Last Will...
                   lwMessage.c_str())) { // Koneksi dengan autentikasi + Last Will (QoS 1, retained)
    Serial.println(" berhasil!"); // Log koneksi sukses
    mqtt.publish(lwTopic.c_str(), "{\"status\":\"online\"}", true); // Umumkan status online (retained)
    String cmdTopic = "selene/" + String(NODE_ID) + "/command"; // Susun topik perintah untuk node ini
    mqtt.subscribe(cmdTopic.c_str(), 1); // Subscribe topik perintah dengan QoS 1
    Serial.println("MQTT: Subscribe ke " + cmdTopic); // Log subscribe berhasil
    // Tangkap OTA yang sempat diunggah saat perangkat offline
    lastOtaPullCheck = 0; // Paksa pengecekan firmware via pull segera setelah tersambung
    return true; // Koneksi sukses
  }

  Serial.print(" gagal, rc="); // Log koneksi gagal
  Serial.println(mqtt.state()); // Cetak kode status kegagalan klien MQTT
  return false; // Koneksi gagal
}

// energyKwh: nilai kumulatif dari pzem.energy() — sudah dalam kWh (pustaka membagi Wh dengan 1000)
void publishTelemetryMQTT(float voltage, float current, float power, float pf, float energyKwh, // Deklarasi fungsi kirim telemetri MQTT (baris parameter pertama)...
                          float frequency, float calibratedTemp, float calibratedHum, // ...parameter baris kedua: frekuensi dan nilai lingkungan terkalibrasi...
                          float apparentPower, float reactivePower) { // Susun dan kirim telemetri lengkap ke topik MQTT
  StaticJsonDocument<384> doc; // Buat dokumen JSON kapasitas 384 byte
  doc["voltage"] = voltage; // Isi tegangan (Volt)
  doc["current"] = current; // Isi arus (Ampere)
  doc["power"] = power; // Isi daya aktif (Watt)
  doc["pf"] = pf; // Isi faktor daya (cos phi)
  doc["energy"] = energyKwh;  // Isi energi kumulatif dalam kWh (satuan yang disimpan backend)
  doc["frequency"] = frequency; // Isi frekuensi jaringan (Hz)
  doc["apparentPower"] = apparentPower; // Isi daya semu (VA)
  doc["reactivePower"] = reactivePower; // Isi daya reaktif (VAR)
  doc["temperature"] = calibratedTemp; // Isi suhu terkalibrasi (Celsius)
  doc["humidity"] = calibratedHum; // Isi kelembaban terkalibrasi (%RH)

  String json; // Penampung hasil serialisasi JSON
  serializeJson(doc, json); // Ubah dokumen JSON menjadi string

  String topic = "selene/" + String(NODE_ID) + "/telemetry"; // Susun topik telemetri untuk node ini
  if (mqtt.publish(topic.c_str(), json.c_str())) { // Kirim pesan telemetri ke broker
    Serial.println("MQTT: Data terkirim ke " + topic); // Log pengiriman sukses
  } else { // Jika pengiriman gagal
    Serial.println("MQTT: GAGAL mengirim data"); // Log pengiriman gagal
  }
}

// ── Callback MQTT: jadwalkan OTA, tangani reboot/status ───────────────────
void mqttCallback(char *topic, byte *payload, unsigned int length) { // Dipanggil otomatis saat pesan dari broker tiba
  String message; // Penampung isi pesan masuk
  message.reserve(length + 1); // Siapkan kapasitas string sesuai panjang payload
  for (unsigned int i = 0; i < length; i++) message += (char)payload[i]; // Salin byte payload menjadi string

  Serial.print("MQTT: Perintah diterima ["); // Log awal pesan masuk
  Serial.print(topic); // Cetak topik pesan
  Serial.print("]: "); // Pemisah log
  Serial.println(message); // Cetak isi pesan

  // Kapasitas ekstra untuk URL OTA HTTPS yang panjang
  StaticJsonDocument<768> doc; // Buat dokumen JSON 768 byte untuk parse perintah
  DeserializationError error = deserializeJson(doc, message); // Parse JSON perintah
  if (error) { // Jika parse gagal (bukan JSON valid)
    Serial.print("MQTT: Gagal parse JSON command: "); // Log kegagalan parse
    Serial.println(error.c_str()); // Cetak detail error parse
    return; // Abaikan pesan
  }

  // Salin ke buffer lokal agar tidak bergantung pada masa pakai pointer JsonDocument
  const char *commandPtr = doc["command"] | ""; // Ambil field command (string kosong bila tidak ada)
  char command[24]; // Buffer lokal untuk nama perintah
  strncpy(command, commandPtr, sizeof(command) - 1); // Salin nama perintah secara aman
  command[sizeof(command) - 1] = '\0'; // Pastikan string selalu berakhiran null

  if (command[0] == '\0') { // Jika tidak ada field command pada pesan
    Serial.println("MQTT: no command field"); // Log pesan tanpa perintah
    return; // Abaikan pesan
  }

  Serial.print("MQTT: parsed command="); // Log perintah hasil parse
  Serial.println(command); // Cetak nama perintah

  if (strcmp(command, "reboot") == 0) { // Jika perintahnya reboot jarak jauh
    Serial.println("MQTT: Menjalankan perintah REBOOT..."); // Log eksekusi reboot
    lcd.clear(); // Bersihkan layar LCD
    lcd.setCursor(0, 0); // Pindah kursor ke baris atas
    lcd.print("Remote Reboot..."); // Tampilkan pesan reboot jarak jauh
    delay(1000); // Jeda agar pesan terbaca
    ESP.restart(); // Restart ESP32 sekarang juga
    return; // (Tidak pernah tercapai setelah restart)
  }

  if (strcmp(command, "status") == 0) { // Jika perintahnya minta laporan status perangkat
    String statusTopic = "selene/" + String(NODE_ID) + "/status"; // Susun topik status
    String statusPayload = "{\"status\":\"online\",\"rssi\":" + String(WiFi.RSSI()) + // Susun JSON status berisi kekuatan sinyal WiFi,
                           ",\"uptime\":" + String(millis() / 1000) + // lama perangkat menyala (detik),
                           ",\"free_heap\":" + String(ESP.getFreeHeap()) + "}"; // dan sisa memori heap
    mqtt.publish(statusTopic.c_str(), statusPayload.c_str()); // Kirim status ke broker
    Serial.println("MQTT: Status terkirim"); // Log status terkirim
    return; // Selesai menangani perintah status
  }

  if (strcmp(command, "ota") == 0) { // Jika perintahnya pembaruan firmware
    const char *urlPtr = doc["url"] | ""; // Ambil URL firmware dari pesan
    if (strlen(urlPtr) < 8) { // Jika URL tidak ada atau terlalu pendek
      Serial.println("MQTT: OTA missing/invalid url"); // Log URL tidak valid
      return; // Abaikan perintah
    }
    Serial.println("MQTT: OTA branch OK, queueing download"); // Log perintah OTA valid
    // Tunda HTTPUpdate yang blocking ke loop() — jangan pernah blocking di dalam callback
    scheduleOta(String(urlPtr)); // Jadwalkan unduhan firmware
    return; // Selesai menangani perintah OTA
  }

  Serial.print("MQTT: unknown command (ignored): "); // Log perintah tak dikenal
  Serial.println(command); // Cetak nama perintah yang diabaikan
}

void setup() { // Fungsi setup dijalankan sekali saat perangkat menyala
  Serial.begin(115200); // Inisialisasi serial monitor dengan baudrate 115200
  delay(200); // Jeda singkat agar serial siap
  Wire.begin(); // Inisialisasi komunikasi I2C (untuk LCD)
  lcd.init(); // Inisialisasi modul LCD
  lcd.backlight(); // Nyalakan backlight LCD

  for (int i = 0; i < 10; i++) { // Kosongkan seluruh circular buffer error kalibrasi
    tempErrors[i] = 0; // Nolkan sel error suhu
    humErrors[i] = 0; // Nolkan sel error kelembaban
  }

  checkBoot(); // Cek tombol BOOT untuk kemungkinan reset konfigurasi WiFi
  showIntroText(); // Tampilkan identitas alat di LCD
  delay(3500); // Tampilkan intro selama 3,5 detik

#define AP_PASS "" // Kata sandi access point portal WiFiManager (kosong = terbuka)
#define AP_SSID "EcoOffice" // Nama SSID access point portal konfigurasi
  const unsigned long WIFI_TIMEOUT = 60; // Batas waktu koneksi/portal WiFi: 60 detik
  WiFiManager wfm; // Buat objek WiFiManager

  wfm.setConfigPortalTimeout(WIFI_TIMEOUT); // Portal konfigurasi otomatis tertutup setelah 60 detik
  wfm.setHostname(AP_SSID); // Set nama host perangkat di jaringan
  wfm.setConnectTimeout(WIFI_TIMEOUT); // Batas waktu proses mencoba koneksi WiFi

  lcd.clear(); // Bersihkan layar LCD
  lcd.setCursor(0, 0); // Pindah kursor ke baris atas
  lcd.print("Waiting for WiFi"); // Tampilkan pesan menunggu WiFi
  lcd.setCursor(0, 1); // Pindah kursor ke baris bawah
  lcd.print("Connection..."); // Lanjutan pesan menunggu koneksi
  unsigned long wifiStartTime = millis(); // Catat waktu mulai menunggu WiFi
  bool connected = false; // Penanda status koneksi WiFi

  while (!connected && (millis() - wifiStartTime < WIFI_TIMEOUT * 1000)) { // Ulangi sampai terhubung atau melewati batas waktu
    connected = wfm.autoConnect(AP_SSID, AP_PASS); // Coba terhubung ke WiFi tersimpan, atau buka portal bila gagal
    handleLCDBlink(millis(), wifiStartTime); // Kedipkan backlight LCD sebagai indikator proses
    delay(100); // Jeda singkat antar percobaan
  }
  if (!connected) { // Jika tetap gagal terhubung sampai batas waktu
    Serial.println("Failed to connect to WiFi!"); // Log kegagalan koneksi
    stopLCDBlink(); // Hentikan kedip LCD
    lcd.clear(); // Bersihkan layar LCD
    lcd.setCursor(0, 0); // Pindah kursor ke baris atas
    lcd.print("Failed to Connect"); // Tampilkan pesan gagal koneksi
    delay(1000); // Jeda 1 detik agar pesan terbaca
    lcd.clear(); // Bersihkan layar LCD lagi
    lcd.setCursor(0, 0); // Pindah kursor ke baris atas
    lcd.print("Restarting ESP32..."); // Tampilkan pesan akan restart
    delay(3500); // Jeda 3,5 detik agar pesan terbaca
    ESP.restart(); // Restart ESP32 untuk mencoba dari awal
  }

  Serial.println("WiFi Connected!"); // Log WiFi berhasil terhubung
  stopLCDBlink(); // Hentikan kedip backlight
  lcd.clear(); // Bersihkan layar LCD
  lcd.setCursor(0, 0); // Pindah kursor ke baris atas
  lcd.print("WiFi Connected!"); // Tampilkan pesan terhubung
  lcd.setCursor(0, 1); // Pindah kursor ke baris bawah
  lcd.print("IP:" + WiFi.localIP().toString()); // Tampilkan alamat IP perangkat
  delay(3000); // Jeda 3 detik agar pesan terbaca

  Blynk.config(auth, "iot.serangkota.go.id", 8080); // Konfigurasi Blynk: token, server, dan port
  if (Blynk.connect(3000)) { // Coba sambungkan ke server Blynk maksimal 3 detik
    Serial.println("Blynk connected!"); // Log Blynk terhubung
  } else { // Jika Blynk tidak terhubung dalam 3 detik
    Serial.println("Blynk connection timeout, continuing..."); // Log timeout tetapi program lanjut
  }

  mqtt.setServer(MQTT_BROKER, MQTT_PORT); // Set alamat dan port broker MQTT
  mqtt.setCallback(mqttCallback); // Daftarkan fungsi callback pesan masuk
  mqtt.setBufferSize(1024);  // Perbesar buffer MQTT agar muat JSON perintah OTA + URL panjang
  mqtt.setKeepAlive(60); // Kirim keep-alive MQTT tiap 60 detik

  hwSerial.begin(9600, SERIAL_8N1, 16, 17); // Inisialisasi UART1 untuk PZEM-004T: 9600 baud, RX 16, TX 17
  dht.begin(); // Inisialisasi sensor DHT11

  lcd.clear(); // Bersihkan layar LCD
  lcd.setCursor(0, 0); // Pindah kursor ke baris atas
  lcd.print("System Ready!"); // Tampilkan pesan sistem siap
  lcd.setCursor(0, 1); // Pindah kursor ke baris bawah
  lcd.print("Monitoring..."); // Tampilkan pesan mulai memantau
  delay(1500); // Jeda 1,5 detik agar pesan terbaca
  lcd.clear(); // Bersihkan layar sebelum masuk mode tampilan bergilir

  Serial.println("System Started - Energy + Environment + MQTT OTA v2"); // Log sistem versi lengkap dimulai
  Serial.print("MQTT Node ID: "); // Log identitas node
  Serial.println(NODE_ID); // Cetak identitas node MQTT
  Serial.printf("Free heap: %u\n", ESP.getFreeHeap()); // Log sisa memori heap saat start
  Serial.println("OTA: USB-flash marker — if you see ota MQTT without 'OTA branch OK', reflash this sketch"); // Penanda versi firmware untuk debugging OTA
}

void loop() { // Fungsi loop dijalankan berulang selama perangkat menyala
  // Jalankan OTA di luar callback MQTT (unduhan bersifat blocking)
  if (otaPending) { // Jika ada pembaruan firmware yang menunggu
    otaPending = false; // Nonaktifkan penanda sebelum eksekusi
    String url = otaUrl; // Ambil URL firmware yang dijadwalkan
    otaUrl = ""; // Kosongkan URL terjadwal
    performOtaUpdate(url); // Jalankan unduhan dan pemasangan firmware
  }

  Blynk.run(); // Jalankan mesin Blynk (keep-alive dan pemrosesan data)

  static unsigned long lastMqttAttempt = 0; // Waktu percobaan koneksi MQTT terakhir
  const unsigned long MQTT_RETRY_INTERVAL = 5000; // Interval coba ulang koneksi MQTT: 5 detik

  if (!mqtt.connected() && millis() - lastMqttAttempt >= MQTT_RETRY_INTERVAL) { // Jika MQTT putus dan sudah waktunya coba ulang
    lastMqttAttempt = millis(); // Perbarui waktu percobaan terakhir
    connectMQTT(); // Coba sambungkan ulang ke broker
  }
  mqtt.loop(); // Proses lalu lintas MQTT (keep-alive, pesan masuk/keluar)

  // Cadangan pull bila push OTA via MQTT terlewat
  if (!otaPending && millis() - lastOtaPullCheck >= OTA_PULL_INTERVAL_MS) { // Jika tidak ada OTA menunggu dan sudah waktunya cek server
    lastOtaPullCheck = millis(); // Perbarui waktu pengecekan terakhir
    checkPendingOtaPull(); // Tanyakan ke server apakah ada firmware baru
  }

  static unsigned long previousMillis = 0; // Waktu pembacaan sensor terakhir
  const unsigned long SENSOR_INTERVAL = 3000; // Interval pembacaan sensor: 3 detik
  if (millis() - previousMillis >= SENSOR_INTERVAL) { // Jika sudah waktunya membaca sensor
    previousMillis = millis(); // Perbarui waktu pembacaan terakhir
    float voltage = zeroIfNan(pzem.voltage()); // Baca tegangan dari PZEM-004T (Volt)
    float current = zeroIfNan(pzem.current()); // Baca arus dari PZEM-004T (Ampere)
    float power = zeroIfNan(pzem.power()); // Baca daya aktif dari PZEM-004T (Watt)
    // PZEM004Tv30::energy() mengembalikan kWh (register Wh internal dibagi 1000)
    float energyKwh = zeroIfNan(pzem.energy()); // Baca energi kumulatif dari PZEM (kWh)
    float frequency = zeroIfNan(pzem.frequency()); // Baca frekuensi jaringan dari PZEM (Hz)
    float pf = zeroIfNan(pzem.pf()); // Baca faktor daya dari PZEM (cos phi)
    float humidity = zeroIfNan(dht.readHumidity()); // Baca kelembaban mentah dari DHT11 (%RH)
    float temperature = zeroIfNan(dht.readTemperature()); // Baca suhu mentah dari DHT11 (Celsius)
    float calibratedTemp = calibrateTemperature(temperature); // Terapkan kalibrasi regresi pada suhu
    float calibratedHum = calibrateHumidity(humidity); // Terapkan kalibrasi regresi pada kelembaban
    float simpleTemp = calibrateTemperatureSimple(temperature); // Hitung suhu metode bias (untuk diagnostik)
    float simpleHum = calibrateHumiditySimple(humidity); // Hitung kelembaban metode bias (untuk diagnostik)

    recordCalibrationError(abs(calibratedTemp - simpleTemp), abs(calibratedHum - simpleHum)); // Catat selisih kedua metode kalibrasi ke buffer diagnostik

    float apparentPower = (pf == 0) ? 0 : power / pf; // Hitung daya semu S = P / cos phi (0 bila PF nol)
    float reactivePower = (pf == 0) ? 0 : sqrt(sq(apparentPower) - sq(power)); // Hitung daya reaktif Q = akar(S^2 - P^2)
    updateBlynkFuzzyStatus(calibratedTemp, calibratedHum, voltage, power, pf, reactivePower); // Hitung dan kirim status kedua modul fuzzy ke Blynk

    static int displayMode = 0; // Mode tampilan LCD bergilir (0-4)
    lcd.clear(); // Bersihkan layar sebelum menampilkan mode baru

    switch (displayMode) { // Tampilkan data sesuai mode aktif
      case 0: // Mode 0: Tegangan dan Arus
        lcd.setCursor(0, 0); // Pindah kursor ke baris atas
        lcd.print("Voltage: " + String(voltage, 1) + "V"); // Cetak tegangan 1 desimal
        lcd.setCursor(0, 1); // Pindah kursor ke baris bawah
        lcd.print("Current: " + String(current, 3) + "A"); // Cetak arus 3 desimal
        break; // Akhiri mode 0
      case 1: // Mode 1: Daya dan Frekuensi
        lcd.setCursor(0, 0); // Pindah kursor ke baris atas
        lcd.print("Power: " + String(power, 1) + "W"); // Cetak daya aktif 1 desimal
        lcd.setCursor(0, 1); // Pindah kursor ke baris bawah
        lcd.print("Freq: " + String(frequency, 1) + "Hz"); // Cetak frekuensi 1 desimal
        break; // Akhiri mode 1
      case 2: // Mode 2: Energi dan Faktor Daya
        // 3 desimal: 0,001 kWh = 1 Wh (resolusi penyimpanan PZEM)
        lcd.setCursor(0, 0); // Pindah kursor ke baris atas
        lcd.print("E:" + String(energyKwh, 3) + "kWh"); // Cetak energi kumulatif dalam kWh
        lcd.setCursor(0, 1); // Pindah kursor ke baris bawah
        lcd.print("PF: " + String(pf, 2)); // Cetak faktor daya 2 desimal
        break; // Akhiri mode 2
      case 3: // Mode 3: Suhu dan Kelembaban terkalibrasi
        lcd.setCursor(0, 0); // Pindah kursor ke baris atas
        lcd.print("Temp: " + String(calibratedTemp, 1) + "C"); // Cetak suhu terkalibrasi 1 desimal
        lcd.setCursor(0, 1); // Pindah kursor ke baris bawah
        lcd.print("Humidity: " + String(calibratedHum, 1) + "%"); // Cetak kelembaban terkalibrasi 1 desimal
        break; // Akhiri mode 3
      case 4: // Mode 4: Status kedua modul fuzzy
        lcd.setCursor(0, 0); // Pindah kursor ke baris atas
        lcd.print("Comfort:" + fuzzyTemperatureComfort(calibratedTemp, calibratedHum)); // Cetak kategori kenyamanan termal
        lcd.setCursor(0, 1); // Pindah kursor ke baris bawah
        lcd.print("Energy:" + fuzzyEnergyConsumption(voltage, power, pf, reactivePower)); // Cetak kategori konsumsi energi
        break; // Akhiri mode 4
    }
    displayMode = (displayMode + 1) % 5; // Ganti ke mode tampilan berikutnya (kembali ke 0 setelah 4)

    Blynk.virtualWrite(V0, voltage); // Kirim tegangan ke pin virtual V0
    Blynk.virtualWrite(V1, current); // Kirim arus ke pin virtual V1
    Blynk.virtualWrite(V2, power); // Kirim daya aktif ke pin virtual V2
    Blynk.virtualWrite(V3, pf); // Kirim faktor daya ke pin virtual V3
    Blynk.virtualWrite(V4, apparentPower); // Kirim daya semu ke pin virtual V4
    Blynk.virtualWrite(V5, energyKwh);  // Kirim energi ke V5 dalam kWh — set satuan widget Blynk ke kWh
    Blynk.virtualWrite(V6, frequency); // Kirim frekuensi ke pin virtual V6
    Blynk.virtualWrite(V7, reactivePower); // Kirim daya reaktif ke pin virtual V7
    Blynk.virtualWrite(V8, calibratedTemp); // Kirim suhu terkalibrasi ke pin virtual V8
    Blynk.virtualWrite(V9, calibratedHum); // Kirim kelembaban terkalibrasi ke pin virtual V9
  }

  if (millis() - lastMqttPublish >= MQTT_PUBLISH_INTERVAL) { // Jika sudah waktunya kirim telemetri MQTT (tiap 30 detik)
    lastMqttPublish = millis(); // Perbarui waktu kirim telemetri terakhir

    float voltageMQTT = zeroIfNan(pzem.voltage()); // Baca ulang tegangan terbaru untuk telemetri
    float currentMQTT = zeroIfNan(pzem.current()); // Baca ulang arus terbaru untuk telemetri
    float powerMQTT = zeroIfNan(pzem.power()); // Baca ulang daya terbaru untuk telemetri
    float energyKwhMQTT = zeroIfNan(pzem.energy());  // Baca ulang energi terbaru (kWh) untuk telemetri
    float frequencyMQTT = zeroIfNan(pzem.frequency()); // Baca ulang frekuensi terbaru untuk telemetri
    float pfMQTT = zeroIfNan(pzem.pf()); // Baca ulang faktor daya terbaru untuk telemetri
    float humidityMQTT = zeroIfNan(dht.readHumidity()); // Baca ulang kelembaban mentah terbaru
    float temperatureMQTT = zeroIfNan(dht.readTemperature()); // Baca ulang suhu mentah terbaru
    float calibratedTempMQTT = calibrateTemperature(temperatureMQTT); // Kalibrasi suhu untuk telemetri
    float calibratedHumMQTT = calibrateHumidity(humidityMQTT); // Kalibrasi kelembaban untuk telemetri
    float apparentPowerMQTT = (pfMQTT == 0) ? 0 : powerMQTT / pfMQTT; // Hitung daya semu untuk telemetri
    float reactivePowerMQTT = // Hitung daya reaktif telemetri (dilanjutkan di baris bawah)...
        (pfMQTT == 0) ? 0 : sqrt(sq(apparentPowerMQTT) - sq(powerMQTT)); // ...Q = akar(S^2 - P^2), nol bila PF nol

    publishTelemetryMQTT(voltageMQTT, currentMQTT, powerMQTT, pfMQTT, energyKwhMQTT, // Panggil kirim telemetri (argumen baris pertama: kelistrikan dasar)...
                         frequencyMQTT, calibratedTempMQTT, calibratedHumMQTT, // ...argumen baris kedua: frekuensi dan lingkungan terkalibrasi...
                         apparentPowerMQTT, reactivePowerMQTT); // Kirim seluruh telemetri ke broker MQTT
  }
}
