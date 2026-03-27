/*
 * ╔══════════════════════════════════════════════════════════════╗
 * ║   ระบบเฝ้าระวังไฟไหม้ห้อง Server — FireGuard v2.0           ║
 * ║              ESP32  ProMax  Fire Monitor                     ║
 * ╠══════════════════════════════════════════════════════════════╣
 * ║  Hardware:  ESP32 DevKit + DHT11 + LDR + Buzzer              ║
 * ║             Traffic Light Module + LCD 16x2 I2C              ║
 * ╠══════════════════════════════════════════════════════════════╣
 * ║  Features:                                                   ║
 * ║  [1] Sensor   — DHT11 (Temp+Hum) + LDR (Flame Detection)    ║
 * ║  [2] WiFi     — ESP32 built-in, auto-reconnect               ║
 * ║  [3] MQTT     — JSON payload -> MQTT Broker                  ║
 * ║  [4] Database — Firebase Realtime Database                   ║
 * ║  [5] Dashboard— IoT MQTT Panel JSON widgets                  ║
 * ║  [+] Fire Alert Logic, Moving Average, Non-blocking millis() ║
 * ╠══════════════════════════════════════════════════════════════╣
 * ║  Libraries ที่ต้องติดตั้งใน Arduino IDE Library Manager:      ║
 * ║  - DHT sensor library        (Adafruit)                      ║
 * ║  - Adafruit Unified Sensor   (Adafruit)                      ║
 * ║  - PubSubClient              (Nick O'Leary)                  ║
 * ║  - Firebase ESP Client       (Mobizt)                        ║
 * ║  - ArduinoJson               (Benoit Blanchon)               ║
 * ║  - LiquidCrystal I2C         (Frank de Brabander)            ║
 * ║  - NTPClient                 (Fabrice Weinberg)              ║
 * ╚══════════════════════════════════════════════════════════════╝
 */

// ── Includes ────────────────────────────────────────────────────
#include <WiFi.h>
#include "soc/soc.h"
#include "soc/rtc_cntl_reg.h"
#include <PubSubClient.h>
#include <DHT.h>
#include <LiquidCrystal_I2C.h>
#include <ArduinoJson.h>
#include <NTPClient.h>
#include <WiFiUdp.h>
#include <Firebase_ESP_Client.h>
#include "addons/TokenHelper.h"
#include "addons/RTDBHelper.h"
#include "config.h"

// ── Pin Definitions ─────────────────────────────────────────────
#define DHT_PIN     15    // DHT11 data pin
#define DHT_TYPE    DHT11
#define LDR_PIN     34    // LDR analog (ADC1 -- ใช้ได้กับ WiFi)
#define BUZZER_PIN  27    // Buzzer PWM
#define LED_RED     25    // Traffic Light แดง
#define LED_YELLOW  26    // Traffic Light เหลือง
#define LED_GREEN   33    // Traffic Light เขียว

// ── Moving Average Buffer (5 samples) ───────────────────────────
#define MA_SIZE     5
float tempBuf[MA_SIZE] = {25.0, 25.0, 25.0, 25.0, 25.0};
float humBuf[MA_SIZE]  = {60.0, 60.0, 60.0, 60.0, 60.0};
int   maCursor = 0;

// ── Fire Confirmation State ──────────────────────────────────────
int   confirmedLevel   = 0;
int   escalateCount    = 0;
int   deescalateCount  = 0;
float prevTemperature  = 25.0;
float tempRiseRate     = 0.0;
int   bootWarmup       = MA_SIZE + 2;  // ข้าม rate-of-rise N รอบแรก (buffer ยังไม่ stable)

// ── Buzzer Pattern State Machine ─────────────────────────────────
// รูปแบบเสียง: สลับ ON/OFF ตาม array (index คู่=ON, คี่=OFF) หน่วย ms
//
// Warning  : beep สั้น 2 ครั้ง แล้วหยุด  → "เตือน ระวัง"
//   ON120 OFF120 ON120 OFF640   (cycle ~1s)
//
// FIRE!    : beep เร็ว 3 ครั้ง แล้วหยุดสั้น → "อันตราย! อพยพ!"
//   ON80 OFF80 ON80 OFF80 ON80 OFF80 ON80 OFF80 ON80 OFF280  (cycle ~1s x3)
//   แล้วหยุด 400ms  → รวม pattern ฟัง ruid มากขึ้น
//
const int WARN_PAT[]    = {120, 120, 120, 640};         // 2-beep
const int FIRE_PAT[]    = {80, 70, 80, 70, 80, 70, 80, 70, 80, 400};  // 5-rapid-beep
const int WARN_PAT_LEN  = 4;
const int FIRE_PAT_LEN  = 10;

int           buzzerState      = 0;   // ตำแหน่งใน pattern ปัจจุบัน
unsigned long lastBuzzerChange = 0;   // เวลาที่เปลี่ยน state ล่าสุด
String        lastBuzzerStatus = "";  // ตรวจสอบว่า status เปลี่ยนหรือไม่

// ── Object Declarations ─────────────────────────────────────────
DHT dht(DHT_PIN, DHT_TYPE);
LiquidCrystal_I2C lcd(0x27, 16, 2);  // ถ้า LCD ไม่ติด ลอง 0x3F
WiFiClient   espClient;
PubSubClient mqttClient(espClient);
WiFiUDP      ntpUDP;
NTPClient    timeClient(ntpUDP, NTP_SERVER, NTP_OFFSET_SEC);
FirebaseData fbdo;
FirebaseAuth fbAuth;
FirebaseConfig fbConfig;

// ── Global Variables ────────────────────────────────────────────
float  temperature    = 0.0;
float  humidity       = 0.0;
float  heatIndex      = 0.0;
int    lightPct       = 0;
String currentStatus  = "Safe";
String previousStatus = "Safe";
String triggerReason  = "Normal";  // บอกว่าอะไรทำให้ trigger
bool   buzzerOverride = false;     // ปิด buzzer จากคำสั่ง MQTT

// ── Timers (non-blocking millis) ────────────────────────────────
unsigned long lastSensorRead    = 0;
unsigned long lastFirebaseSave  = 0;
unsigned long lastLCDSwitch     = 0;
unsigned long lastMqttReconnect = 0;   // timer สำหรับ MQTT reconnect

// ── Network Health Watchdog ──────────────────────────────────────
unsigned long lastMqttOK        = 0;   // เวลาที่ MQTT publish สำเร็จล่าสุด
unsigned long lastFirebaseOK    = 0;   // เวลาที่ Firebase บันทึกสำเร็จล่าสุด
unsigned long lastFbReinit      = 0;   // เวลาที่ reinit Firebase ล่าสุด
unsigned long lastWifiReconnect = 0;   // เวลาที่ WiFi reconnect ล่าสุด
int lcdPage = 0;  // 0 = หน้า Temp/Hum, 1 = หน้า Light/Status

// ── Dynamic Thresholds (ปรับได้จาก MQTT cmd) ───────────────────
float threshTempCaution  = TEMP_CAUTION;
float threshTempAlert    = TEMP_ALERT;
float threshHumWarning   = HUM_CAUTION;   // ต่ำกว่า = Warning
float threshHumAlert     = HUM_ALERT;     // ต่ำกว่า = FIRE (dry air)
int   threshLightWarning = LIGHT_WARNING;
int   threshLightFire    = LIGHT_FIRE;


// ════════════════════════════════════════════════════════════════
//  getDateString -- แปลง epoch time เป็น "YYYY-MM-DD"
//  (NTPClient บางเวอร์ชันไม่มี getFormattedDate() จึงทำเอง)
// ════════════════════════════════════════════════════════════════
String getDateString() {
  time_t rawtime = (time_t)timeClient.getEpochTime();
  struct tm* ti  = localtime(&rawtime);
  char buf[11];
  sprintf(buf, "%04d-%02d-%02d", ti->tm_year + 1900, ti->tm_mon + 1, ti->tm_mday);
  return String(buf);
}


// ════════════════════════════════════════════════════════════════
//  connectWiFi -- เชื่อม WiFi แสดง progress บน LCD + Serial
// ════════════════════════════════════════════════════════════════
void connectWiFi() {
  Serial.println("\n[WiFi] กำลังเชื่อมต่อ: " + String(WIFI_SSID));
  lcd.clear();
  lcd.setCursor(0, 0); lcd.print("WiFi Connecting.");
  lcd.setCursor(0, 1); lcd.print(WIFI_SSID);

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  int dots = 0;
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
    dots++;
    if (dots > 40) {  // timeout 20 วินาที
      Serial.println("\n[WiFi] ERROR: เชื่อมไม่ได้ รีสตาร์ทใน 3 วิ...");
      delay(3000);
      ESP.restart();
    }
  }

  Serial.printf("\n[WiFi] OK | IP: %s | RSSI: %d dBm\n",
    WiFi.localIP().toString().c_str(), WiFi.RSSI());

  lcd.clear();
  lcd.setCursor(0, 0); lcd.print("WiFi Connected!");
  lcd.setCursor(0, 1); lcd.print(WiFi.localIP().toString());
  delay(1500);
}


// ════════════════════════════════════════════════════════════════
//  mqttCallback -- รับและประมวลผลคำสั่งจาก MQTT subscribe
// ════════════════════════════════════════════════════════════════
void mqttCallback(char* topic, byte* payload, unsigned int length) {
  String msg = "";
  for (unsigned int i = 0; i < length; i++) {
    msg += (char)payload[i];
  }
  Serial.printf("[MQTT] รับ [%s]: %s\n", topic, msg.c_str());

  if (msg == "BUZZER_OFF") {
    // ปิด buzzer จากระยะไกล
    buzzerOverride = true;
    digitalWrite(BUZZER_PIN, LOW);
    Serial.println("[CMD] Buzzer -> OFF (remote override)");

  } else if (msg == "BUZZER_ON") {
    // เปิด buzzer ตามปกติ
    buzzerOverride = false;
    Serial.println("[CMD] Buzzer -> ON (resume normal)");

  } else if (msg.startsWith("THRESH:")) {
    // ปรับ threshold ใหม่ รูปแบบ: "THRESH:35,45,30,20,60,80"
    // TempCaution,TempAlert,HumWarning,HumAlert,LightWarning,LightFire
    String vals = msg.substring(7);
    int c1 = vals.indexOf(',');
    int c2 = vals.indexOf(',', c1 + 1);
    int c3 = vals.indexOf(',', c2 + 1);
    int c4 = vals.indexOf(',', c3 + 1);
    int c5 = vals.indexOf(',', c4 + 1);
    if (c1 > 0 && c2 > 0 && c3 > 0 && c4 > 0 && c5 > 0) {
      threshTempCaution  = vals.substring(0, c1).toFloat();
      threshTempAlert    = vals.substring(c1+1, c2).toFloat();
      threshHumWarning   = vals.substring(c2+1, c3).toFloat();
      threshHumAlert     = vals.substring(c3+1, c4).toFloat();
      threshLightWarning = vals.substring(c4+1, c5).toInt();
      threshLightFire    = vals.substring(c5+1).toInt();
      Serial.printf("[CMD] Threshold -> TempW:%.0f TempF:%.0f HumW:%.0f HumF:%.0f LightW:%d LightF:%d\n",
        threshTempCaution, threshTempAlert, threshHumWarning, threshHumAlert,
        threshLightWarning, threshLightFire);
    }
  }
}


// ════════════════════════════════════════════════════════════════
//  connectMQTT -- เชื่อม MQTT Broker พร้อม unique client ID
// ════════════════════════════════════════════════════════════════
void connectMQTT() {
  // สร้าง Client ID จาก MAC address -> ไม่ซ้ำกันแน่นอน
  String clientId = "ESP32-ProMax-" +
    String((uint32_t)(ESP.getEfuseMac() >> 32), HEX) +
    String((uint32_t)ESP.getEfuseMac(), HEX);

  Serial.printf("[MQTT] เชื่อม %s:%d (ID: %s)\n",
    MQTT_BROKER, MQTT_PORT, clientId.c_str());

  lcd.clear();
  lcd.setCursor(0, 0); lcd.print("MQTT Connecting.");

  int attempt = 0;
  while (!mqttClient.connected()) {
    if (mqttClient.connect(clientId.c_str(), MQTT_USER, MQTT_PASS)) {
      // subscribe รับคำสั่งควบคุม
      mqttClient.subscribe(MQTT_SUB_TOPIC);
      Serial.printf("[MQTT] Connected! Subscribe: %s\n", MQTT_SUB_TOPIC);
      lcd.clear();
      lcd.setCursor(0, 0); lcd.print("MQTT Connected!");
      lcd.setCursor(0, 1); lcd.print(MQTT_BROKER);
      delay(1200);
    } else {
      attempt++;
      int waitSec = (attempt <= 5) ? attempt * 2 : 10;  // backoff สูงสุด 10 วิ
      Serial.printf("[MQTT] ล้มเหลว rc=%d รอ %d วิ... (ครั้งที่ %d)\n",
        mqttClient.state(), waitSec, attempt);
      delay(waitSec * 1000);
      // ไม่ restart -- รอจนกว่า broker จะกลับมา
    }
  }
}


// ════════════════════════════════════════════════════════════════
//  readSensors -- อ่าน DHT11 + LDR + Moving Average 5 samples
// ════════════════════════════════════════════════════════════════
void readSensors() {
  float rawTemp = dht.readTemperature();
  float rawHum  = dht.readHumidity();

  // ถ้า DHT11 อ่านไม่ได้ ใช้ค่าเดิมใน buffer
  if (isnan(rawTemp) || isnan(rawHum)) {
    Serial.println("[Sensor] DHT11 error -- ใช้ค่าก่อนหน้า");
    rawTemp = tempBuf[(maCursor - 1 + MA_SIZE) % MA_SIZE];
    rawHum  = humBuf[(maCursor - 1 + MA_SIZE) % MA_SIZE];
  }

  // เพิ่มค่าลง Moving Average buffer
  tempBuf[maCursor] = rawTemp;
  humBuf[maCursor]  = rawHum;
  maCursor = (maCursor + 1) % MA_SIZE;

  // คำนวณค่าเฉลี่ย 5 samples (ลด noise)
  float sumT = 0.0, sumH = 0.0;
  for (int i = 0; i < MA_SIZE; i++) {
    sumT += tempBuf[i];
    sumH += humBuf[i];
  }
  temperature = sumT / MA_SIZE;
  humidity    = sumH / MA_SIZE;

  // LDR: อ่าน ADC 5 ครั้งแล้วเฉลี่ย -> map เป็น % แสง
  long sumLDR = 0;
  for (int i = 0; i < 5; i++) {
    sumLDR += analogRead(LDR_PIN);
  }
  int rawLDR = sumLDR / 5;
  lightPct = map(rawLDR, 0, 4095, 100, 0);  // invert: สว่าง=ADC ต่ำ → % สูง
  lightPct = constrain(lightPct, 0, 100);

  // Heat Index (ความร้อนที่รู้สึก) -- Adafruit DHT built-in
  heatIndex = dht.computeHeatIndex(temperature, humidity, false);

  // อัตราการเปลี่ยนแปลงอุณหภูมิ (rate of change)
  // ช่วง warmup หลัง boot: ยังนับไม่ได้ เพราะ MA buffer ยังไม่ stable
  if (bootWarmup > 0) {
    bootWarmup--;
    tempRiseRate    = 0.0;        // ไม่นับ rate ระหว่าง warmup
    prevTemperature = temperature; // sync ให้ตรงกับค่าจริงก่อน
  } else {
    tempRiseRate    = temperature - prevTemperature;
    prevTemperature = temperature;
  }

  // ══════════════════════════════════════════════════════════════
  //  Cascading Fire Detection Logic
  //  ขั้นตอน: ตรวจ temp+humidity ก่อน → ค่อยเช็คแสง
  //  แสงอย่างเดียวไม่ trigger FIRE! — ต้องมี env risk รองรับ
  // ══════════════════════════════════════════════════════════════

  // ── Step 1: ประเมินสภาพแวดล้อม (temp + humidity) ─────────────
  // envRisk = TRUE เมื่อสภาพห้องเอื้อต่อการเกิดไฟ
  bool envRisk = (temperature >= threshTempCaution)   // ร้อนผิดปกติ
              && (humidity < threshHumWarning);         // อากาศแห้ง

  // ── Step 2: ตรวจแสง/เปลวไฟ (เฉพาะเมื่อ envRisk หรือ temp สูงมาก) ─
  // lightPct ถูกตรวจเมื่อ:
  //   - envRisk = true (สภาพแวดล้อมเสี่ยงแล้ว)
  //   - หรือ temp สูงวิกฤตมากพอที่จะยืนยันได้โดยตัวเอง

  // ── เงื่อนไข FIRE! — ต้องการหลักฐานหลายตัว ────────────────────
  // A) temp วิกฤต ≥45°C (ไม่ต้องรอ sensor อื่น — อันตรายแน่นอน)
  // B) envRisk (temp≥35 AND hum<30) + light≥80% ← 3 sensor ยืนยัน
  // C) envRisk + temp พุ่งเร็ว ≥1.5°C/2s + light≥60% ← ไฟกำลังลาม
  bool condFire = (temperature >= threshTempAlert)
               || (envRisk && lightPct >= threshLightFire)
               || (envRisk && tempRiseRate >= TEMP_RISE_RATE_FIRE && lightPct >= threshLightWarning);

  // ── เงื่อนไข Warning — early detection ──────────────────────────
  // A) temp สูง ≥35°C (ยังไม่ถึง alert)
  // B) humidity ต่ำ <30% (อากาศแห้ง = เสี่ยง)
  // C) envRisk (ทั้ง temp+hum) = เฝ้าระวังพิเศษ
  // D) envRisk + light≥60% = สิ่งแวดล้อมเสี่ยง + แสงผิดปกติ
  // E) temp พุ่งเร็ว ≥0.5°C/2s (สัญญาณเบื้องต้น)
  bool condWarning = (temperature >= threshTempCaution)
                  || (humidity < threshHumWarning)
                  || (envRisk)
                  || (envRisk && lightPct >= threshLightWarning)
                  || (tempRiseRate >= TEMP_RISE_RATE_WARN);

  // ── บันทึกสาเหตุที่ trigger (เพื่อ debug + Firebase log) ────────
  if (condFire) {
    if (temperature >= threshTempAlert)
      triggerReason = "TempCritical";
    else if (envRisk && lightPct >= threshLightFire)
      triggerReason = "EnvRisk+Flame";
    else
      triggerReason = "EnvRisk+RapidRise+Light";
  } else if (condWarning) {
    if (envRisk && lightPct >= threshLightWarning)
      triggerReason = "EnvRisk+LightAnomaly";
    else if (envRisk)
      triggerReason = "EnvRisk(Temp+Dry)";
    else if (temperature >= threshTempCaution)
      triggerReason = "TempHigh";
    else if (humidity < threshHumWarning)
      triggerReason = "DryAir";
    else
      triggerReason = "RapidTempRise";
  } else {
    triggerReason = "Normal";
  }

  // ── คำนวณระดับ raw จาก sensor ────────────────────────────────
  int rawLevel = 0;
  if (condFire)         rawLevel = 2;
  else if (condWarning) rawLevel = 1;

  // ── Confirmation: ยกระดับเร็ว, ลดระดับช้า ────────────────────
  // หลักการ: อย่าเร่งบอกว่าปลอดภัย — ต้องมั่นใจก่อน
  if (rawLevel > confirmedLevel) {
    // สภาพแย่ลง: นับครั้งสะสม ถึง ESCALATE_CONFIRM ค่อยยกระดับ
    escalateCount++;
    deescalateCount = 0;
    if (escalateCount >= ESCALATE_CONFIRM) {
      confirmedLevel = rawLevel;
      escalateCount  = 0;
    }
  } else if (rawLevel < confirmedLevel) {
    // สภาพดีขึ้น: นับครั้งสะสม ถึง DEESCALATE_CONFIRM ค่อยลดระดับ
    deescalateCount++;
    escalateCount = 0;
    if (deescalateCount >= DEESCALATE_CONFIRM) {
      confirmedLevel  = rawLevel;
      deescalateCount = 0;
    }
  } else {
    // คงที่: reset ทั้งคู่
    escalateCount   = 0;
    deescalateCount = 0;
  }

  // ── แปลงระดับเป็น status string ──────────────────────────────
  previousStatus = currentStatus;
  if      (confirmedLevel == 2) currentStatus = "FIRE!";
  else if (confirmedLevel == 1) currentStatus = "Warning";
  else                          currentStatus = "Safe";
}


// ════════════════════════════════════════════════════════════════
//  updateLCD -- แสดงผล LCD 16x2 สลับ 2 หน้า
// ════════════════════════════════════════════════════════════════
void updateLCD() {
  lcd.clear();

  if (lcdPage == 0) {
    // หน้า 1: อุณหภูมิ + ความชื้น + สถานะความชื้น
    // บรรทัด 0: "T:32.5C  H:65%"
    lcd.setCursor(0, 0);
    char line0[17];
    snprintf(line0, sizeof(line0), "T:%.1fC  H:%.0f%%", temperature, humidity);
    lcd.print(line0);

    // บรรทัด 1: "HI:36.1C H:OK  "
    // แสดงสถานะความชื้น: DRY! = อากาศแห้ง = เสี่ยงไฟ
    const char* humTag = (humidity < threshHumWarning) ? "DRY!" : "OK  ";
    lcd.setCursor(0, 1);
    char line1[17];
    snprintf(line1, sizeof(line1), "HI:%.1fC H:%-4s", heatIndex, humTag);
    lcd.print(line1);

  } else {
    // หน้า 2: Flame % + สถานะ + uptime
    // บรรทัด 0: "Flame:  78%"
    lcd.setCursor(0, 0);
    char line0[17];
    snprintf(line0, sizeof(line0), "Flame: %3d%%", lightPct);
    lcd.print(line0);

    // บรรทัด 1: "Up: 15m Safe   "
    unsigned long uptimeMin = millis() / 60000;
    lcd.setCursor(0, 1);
    const char* shortStatus = (currentStatus == "Safe")    ? "Safe   " :
                              (currentStatus == "Warning") ? "WARNING" : "FIRE!!!";
    char line1[17];
    snprintf(line1, sizeof(line1), "Up:%3lum %s", uptimeMin, shortStatus);
    lcd.print(line1);
  }
}


// ════════════════════════════════════════════════════════════════
//  controlOutputs -- ควบคุม Traffic Light + Buzzer ตามสถานะ
// ════════════════════════════════════════════════════════════════
void controlOutputs() {
  // ── Traffic Light ─────────────────────────────────────────────
  digitalWrite(LED_RED,    currentStatus == "FIRE!"   ? HIGH : LOW);
  digitalWrite(LED_YELLOW, currentStatus == "Warning" ? HIGH : LOW);
  digitalWrite(LED_GREEN,  currentStatus == "Safe"    ? HIGH : LOW);

  // reset buzzerOverride เมื่อกลับมา Safe
  if (currentStatus == "Safe") buzzerOverride = false;
}


// ════════════════════════════════════════════════════════════════
//  updateBuzzer -- Pattern state machine (non-blocking)
//  Warning : 2 beep สั้น แล้วพัก  → "ระวัง"
//  FIRE!   : 5 beep เร็ว แล้วพัก  → "อพยพ!"
// ════════════════════════════════════════════════════════════════
void updateBuzzer() {
  // buzzer ถูก override หรือ Safe → ปิดทันที
  if (buzzerOverride || currentStatus == "Safe") {
    digitalWrite(BUZZER_PIN, LOW);
    buzzerState      = 0;
    lastBuzzerChange = millis();
    lastBuzzerStatus = currentStatus;
    return;
  }

  // status เพิ่งเปลี่ยน → reset pattern เริ่มใหม่ทันที
  if (currentStatus != lastBuzzerStatus) {
    buzzerState      = 0;
    lastBuzzerChange = millis();
    lastBuzzerStatus = currentStatus;
    digitalWrite(BUZZER_PIN, HIGH);  // เริ่มด้วย ON (state 0 = คู่ = ON)
    return;
  }

  // เลือก pattern ตาม status
  const int* pat    = (currentStatus == "FIRE!") ? FIRE_PAT    : WARN_PAT;
  int        patLen = (currentStatus == "FIRE!") ? FIRE_PAT_LEN : WARN_PAT_LEN;

  // ถึงเวลาเปลี่ยน state หรือยัง?
  if (millis() - lastBuzzerChange >= (unsigned long)pat[buzzerState]) {
    lastBuzzerChange = millis();
    buzzerState      = (buzzerState + 1) % patLen;
    // index คู่ = ON, คี่ = OFF
    digitalWrite(BUZZER_PIN, (buzzerState % 2 == 0) ? HIGH : LOW);
  }
}


// ════════════════════════════════════════════════════════════════
//  buildJSON -- สร้าง JSON payload ด้วย ArduinoJson
// ════════════════════════════════════════════════════════════════
String buildJSON() {
  StaticJsonDocument<300> doc;
  doc["device"]        = "ESP32-FireGuard";
  doc["temperature"]   = round(temperature * 10) / 10.0;
  doc["humidity"]      = round(humidity    * 10) / 10.0;
  doc["heatIndex"]     = round(heatIndex   * 10) / 10.0;
  doc["flame_pct"]     = lightPct;
  doc["fire_detected"]   = (confirmedLevel == 2);
  doc["status"]          = currentStatus;
  doc["trigger_reason"]  = triggerReason;
  doc["uptime"]        = (unsigned long)(millis() / 1000);
  doc["timestamp"]     = getDateString() + "T" +
                         timeClient.getFormattedTime();

  String output;
  serializeJson(doc, output);
  return output;
}


// ════════════════════════════════════════════════════════════════
//  publishMQTT -- ส่งข้อมูลไปยัง MQTT Broker [ข้อ 3]
// ════════════════════════════════════════════════════════════════
void publishMQTT() {
  if (!mqttClient.connected()) {
    Serial.println("[MQTT] ยังไม่ได้เชื่อม ข้าม publish รอบนี้");
    return;  // ไม่บล็อก -- loop() จะ reconnect เองใน 5 วิ
  }

  String payload = buildJSON();

  // retained=true -> IoT MQTT Panel เห็นค่าล่าสุดทันทีที่เปิด app
  bool ok = mqttClient.publish(MQTT_PUB_TOPIC, payload.c_str(), true);
  if (ok) lastMqttOK = millis();  // บันทึกเวลา publish สำเร็จ

  Serial.printf("[MQTT] Publish %s -> %s\n",
    ok ? "OK  " : "FAIL", payload.c_str());
}


// ════════════════════════════════════════════════════════════════
//  saveFirebase -- บันทึกลง Firebase RTDB [ข้อ 4]
// ════════════════════════════════════════════════════════════════
void saveFirebase() {
  if (!Firebase.ready()) {
    Serial.println("[Firebase] ยังไม่พร้อม ข้ามรอบนี้");
    return;
  }

  String ts = getDateString() + " " +
              timeClient.getFormattedTime();

  // /server_room/latest -- overwrite ทุกรอบ (ค่าปัจจุบัน)
  FirebaseJson latest;
  latest.set("temperature",   (double)temperature);
  latest.set("humidity",      (double)humidity);
  latest.set("heatIndex",     (double)heatIndex);
  latest.set("flame_pct",     lightPct);
  latest.set("fire_detected",   (confirmedLevel == 2));
  latest.set("status",          currentStatus.c_str());
  latest.set("trigger_reason",  triggerReason.c_str());
  latest.set("updatedAt",       ts.c_str());

  if (Firebase.RTDB.setJSON(&fbdo, "/server_room/latest", &latest)) {
    Serial.println("[Firebase] /server_room/latest OK");
    lastFirebaseOK = millis();  // บันทึกเวลา Firebase สำเร็จ
  } else {
    Serial.printf("[Firebase] latest ERROR: %s\n", fbdo.errorReason().c_str());
  }

  // /server_room/history -- push เก็บประวัติทุก 10 วิ
  FirebaseJson hist;
  hist.set("temperature",   (double)temperature);
  hist.set("humidity",      (double)humidity);
  hist.set("heatIndex",     (double)heatIndex);
  hist.set("flame_pct",     lightPct);
  hist.set("fire_detected", (confirmedLevel == 2));
  hist.set("status",        currentStatus.c_str());
  hist.set("ts",            ts.c_str());

  if (Firebase.RTDB.pushJSON(&fbdo, "/server_room/history", &hist)) {
    Serial.println("[Firebase] /server_room/history push OK");
  } else {
    Serial.printf("[Firebase] history ERROR: %s\n", fbdo.errorReason().c_str());
  }
}


// ════════════════════════════════════════════════════════════════
//  logAlert -- บันทึก event เมื่อ status เปลี่ยน
// ════════════════════════════════════════════════════════════════
void logAlert() {
  if (currentStatus == previousStatus) return;  // ไม่มีการเปลี่ยนแปลง

  Serial.printf("[Alert] สถานะเปลี่ยน: %s -> %s\n",
    previousStatus.c_str(), currentStatus.c_str());

  if (!Firebase.ready()) return;

  String ts = getDateString() + " " +
              timeClient.getFormattedTime();

  FirebaseJson alertEntry;
  alertEntry.set("from",           previousStatus.c_str());
  alertEntry.set("to",             currentStatus.c_str());
  alertEntry.set("trigger_reason", triggerReason.c_str());
  alertEntry.set("temperature",    (double)temperature);
  alertEntry.set("humidity",       (double)humidity);
  alertEntry.set("flame_pct",      lightPct);
  alertEntry.set("ts",             ts.c_str());

  // /server_room/fire_log -- push เมื่อ status เปลี่ยน
  if (Firebase.RTDB.pushJSON(&fbdo, "/server_room/fire_log", &alertEntry)) {
    Serial.println("[Firebase] /server_room/fire_log push OK");
  }
}


// ════════════════════════════════════════════════════════════════
//  printSerial -- แสดงตาราง ASCII สวยงามใน Serial Monitor
// ════════════════════════════════════════════════════════════════
void printSerial() {
  Serial.println();
  bool envRiskNow = (temperature >= threshTempCaution) && (humidity < threshHumWarning);
  Serial.println("+------ FireGuard v2.0: Server Room Monitor ------+");
  Serial.printf( "| Temp      : %6.1f C  (Rise: %+.2f C/2s)       |\n", temperature, tempRiseRate);
  Serial.printf( "| Humidity  : %6.1f %%  EnvRisk: %-3s              |\n", humidity, envRiskNow ? "YES" : "No");
  Serial.printf( "| Heat Index: %6.1f C                            |\n", heatIndex);
  Serial.printf( "| Flame %%   : %6d %%                            |\n", lightPct);
  Serial.printf( "| Trigger   : %-30s  |\n", triggerReason.c_str());
  Serial.printf( "| Confirm   : Esc=%d/%d  Desc=%d/%d                  |\n",
    escalateCount, ESCALATE_CONFIRM, deescalateCount, DEESCALATE_CONFIRM);
  Serial.printf( "| Status    : %-10s                          |\n", currentStatus.c_str());
  Serial.printf( "| Uptime    : %6lu min                           |\n", millis() / 60000);
  Serial.printf( "| Time      : %-20s                  |\n",
    timeClient.getFormattedTime().c_str());
  Serial.println("+-------------------------------------------------+");
}


// ════════════════════════════════════════════════════════════════
//  setup -- เริ่มต้นระบบทั้งหมด
// ════════════════════════════════════════════════════════════════
void setup() {
  WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0); // ปิด brownout detector
  Serial.begin(115200);
  delay(500);

  Serial.println();
  Serial.println("+======================================+");
  Serial.println("|  FireGuard v2.0 -- Server Room Fire  |");
  Serial.println("|         Monitor -- BOOT              |");
  Serial.println("+======================================+");

  // Init Output Pins
  pinMode(LED_RED,    OUTPUT);
  pinMode(LED_YELLOW, OUTPUT);
  pinMode(LED_GREEN,  OUTPUT);
  pinMode(BUZZER_PIN, OUTPUT);

  // ทดสอบ LED ทุกดวงและ buzzer ตอน boot
  digitalWrite(LED_RED,    HIGH); delay(200); digitalWrite(LED_RED,    LOW);
  digitalWrite(LED_YELLOW, HIGH); delay(200); digitalWrite(LED_YELLOW, LOW);
  digitalWrite(LED_GREEN,  HIGH); delay(200); digitalWrite(LED_GREEN,  LOW);
  digitalWrite(BUZZER_PIN, HIGH); delay(200); digitalWrite(BUZZER_PIN, LOW);  // beep boot
  Serial.println("[PIN] Init OK -- LED & Buzzer test done");

  // Init LCD
  lcd.init();
  lcd.backlight();
  lcd.setCursor(1, 0); lcd.print("FireGuard v2.0");
  lcd.setCursor(0, 1); lcd.print("Server Room Mon");
  Serial.println("[LCD] Init OK");
  delay(2000);

  // Init DHT11 (ใช้ internal pull-up ของ ESP32 แทน resistor ภายนอก)
  dht.begin();
  pinMode(DHT_PIN, INPUT_PULLUP);
  delay(2000);  // DHT11 ต้องการ warm-up 1-2 วิ หลัง power-on
  Serial.println("[DHT] Init OK");

  // Connect WiFi
  connectWiFi();

  // Init NTP -- ดึงเวลาจริงจาก internet
  timeClient.begin();
  for (int i = 0; i < 5; i++) {
    if (timeClient.update()) break;
    delay(500);
  }
  Serial.printf("[NTP] เวลาปัจจุบัน: %s %s\n",
    getDateString().c_str(),
    timeClient.getFormattedTime().c_str());

  // Init MQTT
  mqttClient.setServer(MQTT_BROKER, MQTT_PORT);
  mqttClient.setCallback(mqttCallback);
  mqttClient.setBufferSize(512);  // เพิ่ม buffer สำหรับ JSON payload
  mqttClient.setKeepAlive(60);    // keepalive 60 วิ (Firebase SSL ใช้เวลานาน)
  connectMQTT();

  // Init Firebase
  fbConfig.database_url = FIREBASE_URL;
  fbConfig.signer.tokens.legacy_token = FIREBASE_AUTH;
  Firebase.begin(&fbConfig, &fbAuth);
  Firebase.reconnectWiFi(true);
  Serial.println("[Firebase] Init OK");

  // อ่าน sensor ครั้งแรก
  readSensors();
  controlOutputs();
  updateLCD();

  lcd.clear();
  lcd.setCursor(0, 0); lcd.print("FireGuard Ready!");
  lcd.setCursor(0, 1); lcd.print("Monitoring...   ");
  Serial.println("[SYS] FireGuard Ready! Server Room Monitoring Active");
  delay(2000);
}


// ════════════════════════════════════════════════════════════════
//  loop -- วนรอบหลัก (non-blocking ทั้งหมด ไม่ใช้ delay())
// ════════════════════════════════════════════════════════════════
void loop() {
  unsigned long now = millis();

  // ── Network Health Watchdog ─────────────────────────────────────
  // ตรวจว่า MQTT หรือ Firebase ล้มเหลวนานเกินไปหรือไม่
  bool mqttDead     = !mqttClient.connected() && (now - lastMqttOK     > 60000UL);
  bool firebaseDead = !Firebase.ready()       && (now - lastFirebaseOK > 60000UL);

  // 1) Firebase SSL พัง → reinit Firebase ทุก 30 วิ
  if (firebaseDead && (now - lastFbReinit > 30000UL) && WiFi.status() == WL_CONNECTED) {
    lastFbReinit = now;
    Serial.println("[Firebase] SSL fail นาน → Reinit Firebase...");
    Firebase.begin(&fbConfig, &fbAuth);
  }

  // 2) ทั้ง MQTT + Firebase ตายพร้อมกัน → TCP stack พัง → WiFi reconnect
  if (mqttDead && firebaseDead && (now - lastWifiReconnect > 60000UL)) {
    lastWifiReconnect = now;
    Serial.println("[NET] MQTT+Firebase fail > 60s → WiFi reconnect...");
    mqttClient.disconnect();
    WiFi.disconnect();
    delay(1000);
    WiFi.reconnect();
    // รอ WiFi กลับมา (max 10 วิ ไม่บล็อกนาน)
    for (int i = 0; i < 20 && WiFi.status() != WL_CONNECTED; i++) {
      delay(500);
    }
    if (WiFi.status() == WL_CONNECTED) {
      Serial.println("[NET] WiFi reconnect OK");
      Firebase.begin(&fbConfig, &fbAuth);
      lastFbReinit = now;
    }
  }

  // 3) Last resort: ถ้ายังตายอยู่นาน > 3 นาที → restart
  if (mqttDead && firebaseDead && (now - lastWifiReconnect > 180000UL)
      && (now - lastMqttOK > 180000UL)) {
    Serial.println("[SYS] Network dead > 3 min → ESP.restart()");
    delay(500);
    ESP.restart();
  }

  // MQTT loop -- non-blocking reconnect (ไม่บล็อก loop)
  if (!mqttClient.connected()) {
    if (now - lastMqttReconnect >= 5000) {  // ลองใหม่ทุก 5 วิ
      lastMqttReconnect = now;
      String clientId = "ESP32-FireGuard-" +
        String((uint32_t)(ESP.getEfuseMac() >> 32), HEX) +
        String((uint32_t)ESP.getEfuseMac(), HEX);
      if (mqttClient.connect(clientId.c_str(), MQTT_USER, MQTT_PASS)) {
        mqttClient.subscribe(MQTT_SUB_TOPIC);
        Serial.println("[MQTT] Reconnected OK");
      } else {
        Serial.printf("[MQTT] Reconnect fail rc=%d (จะลองใหม่ใน 5 วิ)\n",
          mqttClient.state());
      }
    }
  }
  mqttClient.loop();

  // NTP update (auto ทุก 60 วิ)
  timeClient.update();

  // [Sensor][WiFi][MQTT] อ่านค่า + ส่ง MQTT ทุก 2 วินาที
  if (now - lastSensorRead >= 2000) {
    lastSensorRead = now;

    readSensors();    // [1] DHT11 + LDR + Moving Avg
    controlOutputs(); // Traffic Light + Buzzer
    publishMQTT();    // [3] ส่ง JSON -> HiveMQ
    logAlert();       // บันทึก alert_log เฉพาะเมื่อ status เปลี่ยน
    printSerial();    // แสดงตาราง ASCII ใน Serial Monitor
  }

  // [Firebase] บันทึก Database ทุก 10 วินาที
  if (now - lastFirebaseSave >= 10000) {
    lastFirebaseSave = now;
    saveFirebase();  // [4] /sensors/latest + /sensors/history
  }

  // สลับหน้า LCD ทุก 3 วินาที
  if (now - lastLCDSwitch >= 3000) {
    lastLCDSwitch = now;
    lcdPage = 1 - lcdPage;  // toggle 0 <-> 1
    updateLCD();
  }

  // Buzzer pattern — non-blocking state machine
  updateBuzzer();

}
