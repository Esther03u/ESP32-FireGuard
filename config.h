#ifndef CONFIG_H
#define CONFIG_H

// ============================================================
//  config.h — ตั้งค่าทั้งหมดอยู่ที่นี่ แก้ไขก่อนอัปโหลด!
// ============================================================

// ── WiFi ──────────────────────────────────────────────────
#define WIFI_SSID       "IoT_PKRU"
#define WIFI_PASSWORD   "Pkru123456"

// ── MQTT Broker (HiveMQ Public — ฟรี ไม่ต้อง login) ───────
#define MQTT_BROKER     "202.29.50.41"
#define MQTT_PORT       1883
#define MQTT_USER       "s9999999999"
#define MQTT_PASS       "lknt9553"
#define MQTT_PUB_TOPIC  "iot/promax/sensors"   // topic ส่งข้อมูล
#define MQTT_SUB_TOPIC  "iot/promax/cmd"       // topic รับคำสั่ง

// ── Firebase Realtime Database ────────────────────────────
#define FIREBASE_URL    "https://cs-iot-503b5-default-rtdb.asia-southeast1.firebasedatabase.app"
#define FIREBASE_AUTH   "DwL1oXvlvfNLlKzfJGH8UYyMnffMtv307jbDWVKb"

// ── NTP Server (เวลาไทย UTC+7) ───────────────────────────
#define NTP_SERVER      "pool.ntp.org"
#define NTP_OFFSET_SEC  25200   // UTC+7 = 7 * 3600 = 25200

// ╔══════════════════════════════════════════════════════════════╗
// ║         SIGNATURE DEMO CONDITIONS — แสดงอาจารย์            ║
// ╠══════════════════════════════════════════════════════════════╣
// ║                                                              ║
// ║  🟢 SAFE    : ปล่อยทิ้งไว้ ไม่แตะ sensor                   ║
// ║               temp < 32°C, humidity ~55-60%, flame < 50%    ║
// ║               → LED เขียว, Buzzer เงียบ                     ║
// ║                                                              ║
// ║  🟡 WARNING : ครอบมือบน DHT11 ค้างไว้ ~20 วินาที           ║
// ║               temp ≥ 32°C + humidity 55% (ต่ำกว่า 63%=DRY) ║
// ║               → LED เหลือง, Buzzer 2 beep, LCD: WARNING     ║
// ║               trigger_reason: TempHigh+DryAir               ║
// ║                                                              ║
// ║  🔴 FIRE!   : ครอบมือ + ส่องไฟฉายที่ LDR พร้อมกัน         ║
// ║               temp ≥ 32°C + DRY + flame ≥ 72%  (3 sensor)  ║
// ║               → LED แดง, Buzzer alarm, Firebase log         ║
// ║               trigger_reason: AllThree (Cascading Logic)    ║
// ║                                                              ║
// ║  ลำดับสาธิต (3 นาที):                                       ║
// ║  1. เปิดระบบ         → Safe                                 ║
// ║  2. ครอบมือ ~20 วิ   → Warning                              ║
// ║  3. เพิ่มไฟฉาย       → FIRE! (ครบ 3 เงื่อนไข)              ║
// ║  4. ปล่อยทุกอย่าง    → กลับ Safe ใน 4 วิ                   ║
// ╚══════════════════════════════════════════════════════════════╝

// ── Alert Thresholds — DEMO MODE ──────────────────────────────
#define TEMP_CAUTION      32.0  // °C → Warning  | ครอบมือ ~15-20 วิ
#define TEMP_ALERT        36.0  // °C → FIRE!    | ครอบมือแน่นๆ ~45 วิ
#define TEMP_COMBINED     33.0  // °C → ใช้คู่ flame ≥ 72% = ยืนยัน FIRE!
// ความชื้น: ห้องนี้ปกติ 55-60% ต่ำกว่า 63% = DRY เสมอ (EnvRisk: Yes)
#define HUM_CAUTION       63.0  // % → DRY | ห้องปกติ 55-60% trigger ได้เลย
#define HUM_ALERT         20.0  // % (สำรอง)
// แสง: ส่องไฟฉายที่ LDR
#define LIGHT_WARNING     50    // % → Warning | ส่องไฟฉายห่างๆ
#define LIGHT_FIRE        72    // % → FIRE!   | ส่องไฟฉายชิด

// ── Fire Confirmation ─────────────────────────────────────────
#define ESCALATE_CONFIRM    2   // ×2s = 4s ก่อนยกระดับ
#define DEESCALATE_CONFIRM  2   // ×2s = 4s ก่อนลดระดับ (เร็วสำหรับ demo)

// ── Temperature Rate of Change ────────────────────────────────
#define TEMP_RISE_RATE_WARN 0.3 // °C/2s → Warning | กด EN reset trigger ได้เลย
#define TEMP_RISE_RATE_FIRE 1.2 // °C/2s → FIRE!   | boot ~1.66°C/2s trigger ✓

#endif // CONFIG_H
