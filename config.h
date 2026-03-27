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

// ── Alert Thresholds — Server Room Fire Monitor ────────────
// อุณหภูมิ: ห้อง Server ปกติ 18-27°C
#define TEMP_CAUTION      35.0  // °C → Warning (ร้อนผิดปกติ)
#define TEMP_ALERT        45.0  // °C → FIRE! (วิกฤต)
#define TEMP_COMBINED     40.0  // °C ใช้คู่กับ LIGHT_WARNING = ยืนยัน FIRE!
// ความชื้น: ต่ำ = อากาศแห้ง = ติดไฟง่าย
#define HUM_CAUTION       30.0  // % ต่ำกว่านี้ = Warning (แห้ง)
#define HUM_ALERT         20.0  // % ต่ำกว่านี้ = Warning ระดับสูง
// แสง/เปลวไฟ: ห้อง Server ปกติมืด
#define LIGHT_WARNING     60    // % → Warning (แสงผิดปกติ)
#define LIGHT_FIRE        80    // % → FIRE! (เปลวไฟ)

// ── Fire Confirmation — ป้องกัน False Alarm ──────────────
// ต้องอ่านค่าเกิน threshold ติดต่อกัน N ครั้งถึงจะเปลี่ยนสถานะ
#define ESCALATE_CONFIRM    2   // ×2s = 4s ก่อนยกระดับ (ตอบสนองเร็ว)
#define DEESCALATE_CONFIRM  5   // ×2s = 10s ก่อนลดระดับ (อย่าด่วนสรุปว่าปลอดภัย)

// ── Temperature Rate of Change — ตรวจจับไฟลามเร็ว ─────────
// อุณหภูมิเปลี่ยนเร็ว = สัญญาณไฟลาม แม้ยังไม่ถึง threshold สูงสุด
#define TEMP_RISE_RATE_WARN 0.5 // °C ต่อ 2 วิ (15°C/นาที) → Warning
#define TEMP_RISE_RATE_FIRE 1.5 // °C ต่อ 2 วิ (45°C/นาที) → FIRE!

#endif // CONFIG_H
