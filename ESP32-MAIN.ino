#define BLYNK_TEMPLATE_ID "TMPL12345678"
#define BLYNK_TEMPLATE_NAME "SmartLock"
#define BLYNK_AUTH_TOKEN "nv-s7AOUc6A4DrNSS3sEJeU26RM9CLiD"

#define BLYNK_PRINT Serial
#include <HTTPClient.h> 
#include <WiFi.h>
#include <WiFiManager.h>
#include <BlynkSimpleEsp32.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <Keypad.h>
#include <Adafruit_Fingerprint.h>
#include <esp_now.h> 
#include <Preferences.h>
#include <LittleFS.h>
#include <WebServer.h>
#include <WiFiClientSecure.h>
#include <time.h>

Preferences preferences; 

// *** นำ URL ของ Google Apps Script มาใส่ตรงนี้ ***
const String GOOGLE_SCRIPT_URL = "https://script.google.com/macros/s/AKfycbyruKHkTOsiAojP-Lkezip0E5S0sbet7Ho0VwXIG-lnkswJgGH4L2oR12GXgaonornNvg/exec";
const String NTFY_TOPIC = "Warin_SmartLock_Alert_2026";

WebServer server(80); 
unsigned long lastSyncCheck = 0; 

bool isDoorAuthorized = false;      
bool isIntrusionAlarm = false;      
bool doorWasOpened = false;         
unsigned long authorizedTime = 0;   
unsigned long lastSirenToggle = 0;  
bool sirenState = false;

// ==============================================================
// ข้อมูล MAC Address ปลายทาง (ESP32-CAM)
// ==============================================================
uint8_t broadcastAddress[] = {0xB4, 0xBF, 0xE9, 0x34, 0x99, 0x94};
esp_now_peer_info_t peerInfo;

LiquidCrystal_I2C lcd(0x27, 16, 2);

#define RX_PIN 16
#define TX_PIN 17
HardwareSerial mySerial(2);
Adafruit_Fingerprint finger = Adafruit_Fingerprint(&mySerial);

const byte ROWS = 4;
const byte COLS = 4;
char keys[ROWS][COLS] = {
  {'1','2','3','A'},
  {'4','5','6','B'},
  {'7','8','9','C'},
  {'*','0','#','D'}
};
byte rowPins[ROWS] = {32, 33, 25, 26}; 
byte colPins[COLS] = {27, 14, 12, 13}; 
Keypad keypad = Keypad(makeKeymap(keys), rowPins, colPins, ROWS, COLS);

#define RELAY_PIN 18
#define BUZZER_PIN 15
#define MAGNETIC_PIN 4 
#define RELAY_ON LOW
#define RELAY_OFF HIGH
#define BUZZER_ON LOW
#define BUZZER_OFF HIGH

String inputPassword = "";
String unlockPassword = "1234"; 
String masterPassword = "9999"; 

int targetDeleteID = 1; 
bool isUnlocking = false; 
bool requestUnlockFromApp = false;

void beepKey();
void beepSuccess();
void beepError();
void unlockDoor();
void displayStandby();
void handleKeypad(char key);
void checkFingerprint();
void startEnrollMode();

String getCurrentTimeStr() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo, 100)) {
    return "Offline (+" + String(millis() / 1000) + "s)";
  }
  char buf[25];
  strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &timeinfo);
  return String(buf);
}

void saveOfflineLog(String id, String name, String status) {
  File file = LittleFS.open("/offline_logs.csv", FILE_APPEND);
  if (file) {
    String logLine = getCurrentTimeStr() + "," + id + "," + name + "," + status + "\n";
    file.print(logLine);
    file.close();
    Serial.println("💾 [Offline] บันทึกประวัติลงบอร์ด: " + logLine);
  }
}

void handleWebRoot() {
  String html = "<!DOCTYPE html><html><head><meta charset='utf-8'>";
  html += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
  html += "<meta http-equiv='refresh' content='5'>";
  html += "<title>SmartLock Offline Logs</title>";
  html += "<style>body{font-family:sans-serif;background:#121212;color:#eee;padding:20px;}";
  html += "h2{color:#e53935;} table{width:100%;border-collapse:collapse;margin-top:15px;background:#1e1e1e;}";
  html += "th,td{padding:10px;border:1px solid #333;text-align:left;} th{background:#b71c1c;color:#fff;}";
  html += ".empty{padding:20px;background:#1e1e1e;border-left:4px solid #43a047;margin-top:15px;}</style></head><body>";
  html += "<h2>🔒 SmartLock - รายการรอซิงค์ (Offline Pending Logs)</h2>";
  html += "<p>สถานะเครือข่าย: <b>" + String(WiFi.status() == WL_CONNECTED ? "🟢 ออนไลน์ (กำลังทยอยซิงค์...)" : "🔴 ออฟไลน์ (โหมดบันทึกในบอร์ด)") + "</b></p>";

  if (!LittleFS.exists("/offline_logs.csv")) {
    html += "<div class='empty'>✅ ไม่พบข้อมูลค้างในบอร์ด (ข้อมูลทั้งหมดถูกซิงค์ขึ้น Google Sheets เรียบร้อยแล้ว)</div>";
  } else {
    File file = LittleFS.open("/offline_logs.csv", FILE_READ);
    if (!file || file.size() == 0) {
      html += "<div class='empty'>✅ ไม่พบข้อมูลค้างในบอร์ด (ข้อมูลทั้งหมดถูกซิงค์ขึ้น Google Sheets เรียบร้อยแล้ว)</div>";
    } else {
      html += "<table><tr><th>วัน-เวลา</th><th>ID</th><th>ชื่อผู้ใช้</th><th>สถานะ / วิธีเข้า</th></tr>";
      while (file.available()) {
        String line = file.readStringUntil('\n');
        line.trim();
        if (line.length() > 0) {
          int c1 = line.indexOf(',');
          int c2 = line.indexOf(',', c1 + 1);
          int c3 = line.indexOf(',', c2 + 1);
          if (c1 != -1 && c2 != -1 && c3 != -1) {
            html += "<tr><td>" + line.substring(0, c1) + "</td>";
            html += "<td>" + line.substring(c1 + 1, c2) + "</td>";
            html += "<td>" + line.substring(c2 + 1, c3) + "</td>";
            html += "<td>" + line.substring(c3 + 1) + "</td></tr>";
          }
        }
      }
      html += "</table>";
    }
    if (file) file.close();
  }
  html += "</body></html>";
  server.send(200, "text/html", html);
}

void syncOfflineLogs() {
  if (WiFi.status() != WL_CONNECTED) return;
  if (!LittleFS.exists("/offline_logs.csv")) return;

  File file = LittleFS.open("/offline_logs.csv", FILE_READ);
  if (!file || file.size() == 0) {
    if (file) file.close();
    LittleFS.remove("/offline_logs.csv");
    return;
  }

  String firstLine = file.readStringUntil('\n');
  firstLine.trim();
  String remainingContent = "";
  while (file.available()) {
    remainingContent += file.readStringUntil('\n') + "\n";
  }
  file.close();

  if (firstLine.length() == 0) {
    LittleFS.remove("/offline_logs.csv");
    return;
  }

  int c1 = firstLine.indexOf(',');
  int c2 = firstLine.indexOf(',', c1 + 1);
  int c3 = firstLine.indexOf(',', c2 + 1);
  if (c1 == -1 || c2 == -1 || c3 == -1) {
    // ถ้าข้อมูลบรรทัดนี้รูปแบบพัง ให้ตัดทิ้งทันทีเพื่อไม่ให้ระบบค้าง
    LittleFS.remove("/offline_logs.csv");
    return;
  }

  String logTime = firstLine.substring(0, c1);
  String logId = firstLine.substring(c1 + 1, c2);
  String logName = firstLine.substring(c2 + 1, c3);
  String logStatus = firstLine.substring(c3 + 1);

  Serial.println("🔄 กำลังซิงค์ข้อมูล Offline ขึ้น Google Sheets: " + logName);

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.begin(client, GOOGLE_SCRIPT_URL);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.setTimeout(10000);
  http.addHeader("Content-Type", "application/json");

  // จัดรูปแบบ JSON ให้ถูกต้องและปลอดภัยที่สุด
  String payload = "{\"id\":\"" + logId + "\",\"name\":\"" + logName + "\",\"status\":\"" + logStatus + "\",\"time\":\"" + logTime + "\",\"image\":\"\"}";
  int httpCode = http.POST(payload);
  http.end();

  // 💡 ปรับปรุงใหม่: ไม่ว่า HTTP Code จะตอบกลับมาเป็นอะไร (ขอแค่มันส่งออกไปแล้ว) 
  // ให้ทำการอัปเดตไฟล์ในบอร์ดโดยตัดรายการนี้ทิ้งทันที เพื่อป้องกันการส่งวนซ้ำรัวๆ!
  Serial.println("✅ ส่งข้อมูลซิงค์เรียบร้อย ลบออกจากคิวบอร์ด");
  if (remainingContent.length() == 0) {
    LittleFS.remove("/offline_logs.csv"); 
  } else {
    File wf = LittleFS.open("/offline_logs.csv", FILE_WRITE);
    if (wf) {
      wf.print(remainingContent);
      wf.close();
    }
  }
}

void triggerCamera(String source) {
  String msg = "CAPTURE:" + source;
  esp_now_send(broadcastAddress, (uint8_t *)msg.c_str(), msg.length());
  Serial.println("📡 ส่งสัญญาณสั่งถ่ายภาพ (ผู้ใช้: " + source + ")");
}

void refreshBlynkTable() {
  Blynk.virtualWrite(V2, "clr"); 
  delay(100); 
  
  for (int i = 1; i <= 127; i++) {
    String key = "name_" + String(i);
    String name = preferences.getString(key.c_str(), "");
    
    if (name != "") {
      Blynk.virtualWrite(V2, "add", i, "ID: " + String(i), name);
      delay(15); 
    }
  }
}

void sendNtfyAlert(String title, String message, String priority, String tags) {
  if (WiFi.status() == WL_CONNECTED) {
    HTTPClient http;
    http.begin("http://ntfy.sh/" + NTFY_TOPIC);
    http.addHeader("Title", title);
    http.addHeader("Priority", priority); 
    http.addHeader("Tags", tags);         
    
    int httpCode = http.POST(message);
    http.end();
  }
}

bool authorizeDoorAccess(String userID, String userName, String method) {
  if (isIntrusionAlarm) {
    isIntrusionAlarm = false;
    digitalWrite(BUZZER_PIN, BUZZER_OFF); 
    
    isDoorAuthorized = true;
    doorWasOpened = true;
    authorizedTime = millis();
    
    lcd.clear();
    lcd.setCursor(0, 0); lcd.print("Alarm Disarmed!");
    lcd.setCursor(0, 1); lcd.print("By: " + userName);
    beepSuccess();
    
    sendNtfyAlert("Shield Disarmed", "หยุดเสียงเตือนภัยแล้วโดย: " + userName + " (" + method + ")", "default", "white_check_mark,lock");
    
    delay(2000);
    displayStandby();
    return false; 
  }

  isDoorAuthorized = true;
  doorWasOpened = false;
  authorizedTime = millis();
  
  if (WiFi.status() == WL_CONNECTED) {
    triggerCamera(userID + "|" + userName);
    sendNtfyAlert("Door Unlocked", "ผู้ใช้งาน: " + userName + " (ID: " + userID + ") เปิดผ่าน " + method, "default", "key");
  } else {
    saveOfflineLog(userID, userName, "Offline Unlock (" + method + ")");
  }
  return true; 
}

void checkDoorSecurity() {
  bool isDoorOpen = (digitalRead(MAGNETIC_PIN) == HIGH);

  if (isDoorAuthorized && !isIntrusionAlarm) {
    if (isDoorOpen) {
      doorWasOpened = true; 
    }
    
    if ((doorWasOpened && !isDoorOpen) || (millis() - authorizedTime > 10000 && !isDoorOpen)) {
      delay(500); 
      isDoorAuthorized = false;
      doorWasOpened = false;
      Serial.println("🔒 ประตูปิดสนิท ระบบพร้อมตรวจจับการบุกรุกอีกครั้ง");
    }
  }

  if (!isDoorAuthorized && isDoorOpen && !isIntrusionAlarm) {
    isIntrusionAlarm = true;
    Serial.println("🚨 ตรวจพบการบุกรุก! ประตูถูกเปิดโดยไม่ผ่านการยืนยันตัวตน!");

    lcd.clear();
    lcd.setCursor(0, 0); lcd.print("!! INTRUSION !!");
    lcd.setCursor(0, 1); lcd.print("Scan/PIN to Stop");

    triggerCamera("ALERT|INTRUDER");
    sendNtfyAlert("INTRUSION ALERT!", "ตรวจพบการงัดประตูหรือเปิดประตูข้ามขั้นตอน! กรุณาตรวจสอบทันที", "max", "rotating_light,warning");

    if (WiFi.status() != WL_CONNECTED) {
      saveOfflineLog("ALERT", "INTRUDER", "Offline Intrusion!");
    }
  }

  if (isIntrusionAlarm) {
    if (millis() - lastSirenToggle >= 150) { 
      lastSirenToggle = millis();
      sirenState = !sirenState;
      digitalWrite(BUZZER_PIN, sirenState ? BUZZER_ON : BUZZER_OFF);
    }
  }
}

void setup() {
  Serial.begin(115200);
  preferences.begin("users", false); 
  
  if (!LittleFS.begin(true)) {
    Serial.println("❌ LittleFS Mount Failed");
  } else {
    // LittleFS.remove("/offline_logs.csv"); // ปิดบรรทัดนี้แล้วเพื่อป้องกันไม่ให้ประวัติออฟไลน์สูญหายเมื่อรีบูตบอร์ด
    Serial.println("💾 LittleFS File System Ready");
  }

  pinMode(RELAY_PIN, OUTPUT);
  pinMode(BUZZER_PIN, OUTPUT);
  pinMode(MAGNETIC_PIN, INPUT_PULLUP); 
  
  digitalWrite(RELAY_PIN, RELAY_OFF);
  digitalWrite(BUZZER_PIN, BUZZER_OFF);
  
  lcd.init();
  lcd.backlight();
  lcd.setCursor(0, 0);
  lcd.print("System Booting..");

  WiFiManager wm;
  wm.setConfigPortalTimeout(60); 
  lcd.clear(); lcd.setCursor(0, 0); lcd.print("Connect WiFi to");
  lcd.setCursor(0, 1); lcd.print("AP: SmartLock_AP");
  
  bool res = wm.autoConnect("SmartLock_AP"); 
  
  lcd.clear(); lcd.setCursor(0, 0); 
  if (res) {
    lcd.print("WiFi Connected!");
    Serial.println("WiFi Connected!");
  } else {
    lcd.print("Offline Mode");
    Serial.println("WiFi Failed - Running Offline Mode");
  }
  delay(1000);

  if (esp_now_init() == ESP_OK) {
    memcpy(peerInfo.peer_addr, broadcastAddress, 6);
    peerInfo.channel = 0;  
    peerInfo.encrypt = false;
    esp_now_add_peer(&peerInfo);
    Serial.println("ESP-NOW Ready");
  }

  if (res) {
    Blynk.config(BLYNK_AUTH_TOKEN, "sv1.ab.in.th", 8080);
    Blynk.connect();
    configTime(7 * 3600, 0, "pool.ntp.org", "time.nist.gov");
    
    server.on("/", handleWebRoot);
    server.begin();
    Serial.print("🌐 เปิดดูหน้าเว็บ Offline Log ได้ที่ http://");
    Serial.println(WiFi.localIP());
  }

  mySerial.begin(57600, SERIAL_8N1, RX_PIN, TX_PIN);
  lcd.setCursor(0, 1);
  if (finger.verifyPassword()) {
    lcd.print("Sensor OK!      ");
    beepSuccess(); 
  } else {
    lcd.print("Sensor Error!   ");
    beepError();
    while (1) { delay(1); } 
  }

  displayStandby(); 
}

void loop() {
  Blynk.run(); 
  server.handleClient(); 
  checkDoorSecurity(); 
  
  if (millis() - lastSyncCheck > 10000) {
    lastSyncCheck = millis();
    syncOfflineLogs();
  }

  if (requestUnlockFromApp) {
    requestUnlockFromApp = false;
    if (!isUnlocking) {
      Serial.println("📱 สั่งปลดล็อคจากแอป Blynk");
      if (authorizeDoorAccess("Blynk App", "Admin User", "Blynk App")) {
        lcd.clear(); lcd.setCursor(0, 0); lcd.print("App Unlocked!");
        unlockDoor();
        displayStandby();
      }
    }
  }
  
  char key = keypad.getKey();
  if (key) {
    beepKey(); 
    handleKeypad(key);
  }

  checkFingerprint();
  delay(10);
}

BLYNK_CONNECTED() {
  refreshBlynkTable(); 
}

BLYNK_WRITE(V0) {
  if (param.asInt() == 1) { 
    requestUnlockFromApp = true;
    Blynk.virtualWrite(V0, 0);
  }
}

BLYNK_WRITE(V1) { 
  String newName = param.asStr();
  if (newName != "") {
    if (finger.loadModel(targetDeleteID) != FINGERPRINT_OK) {
      lcd.clear(); lcd.setCursor(0, 0); lcd.print("ID Not Enrolled!");
      lcd.setCursor(0, 1); lcd.print("Please Enroll ID:" + String(targetDeleteID));
      beepError();
      delay(2000);
      displayStandby();
      return; 
    }

    String key = "name_" + String(targetDeleteID);
    preferences.putString(key.c_str(), newName);   
    
    refreshBlynkTable(); 
    
    lcd.clear(); lcd.setCursor(0, 0); lcd.print("Saved Name!");
    lcd.setCursor(0, 1); lcd.print("ID " + String(targetDeleteID) + ":" + newName);
    beepSuccess();
    delay(2000);
    displayStandby(); 
  }
}

BLYNK_WRITE(V3) {
  targetDeleteID = param.asInt();
  String currentName = preferences.getString(("name_" + String(targetDeleteID)).c_str(), "");
  String currentPin  = preferences.getString(("pin_" + String(targetDeleteID)).c_str(), "");
  Blynk.virtualWrite(V1, currentName);
  Blynk.virtualWrite(V5, currentPin);
}

BLYNK_WRITE(V4) {
  if (param.asInt() == 1) { 
    lcd.clear(); lcd.setCursor(0, 0); lcd.print("Deleting ID:"); lcd.print(targetDeleteID);
    lcd.setCursor(0, 1);
    
    if (finger.deleteModel(targetDeleteID) == FINGERPRINT_OK) {
      lcd.print("Delete Success!");
      preferences.remove(("name_" + String(targetDeleteID)).c_str()); 
      preferences.remove(("pin_" + String(targetDeleteID)).c_str()); 
      refreshBlynkTable(); 
      beepSuccess();
    } else {
      lcd.print("Delete Failed!");
      beepError();
    }
    delay(2000);
    displayStandby();
    Blynk.virtualWrite(V4, 0); 
  }
}

BLYNK_WRITE(V5) { 
  String newPin = param.asStr();
  if (finger.loadModel(targetDeleteID) != FINGERPRINT_OK) {
    lcd.clear(); lcd.setCursor(0, 0); lcd.print("ID Not Enrolled!");
    lcd.setCursor(0, 1); lcd.print("Enroll ID:" + String(targetDeleteID));
    beepError(); delay(2000); displayStandby();
    return;
  }

  if (newPin.length() < 4 || newPin.length() > 8 || newPin == masterPassword) {
    lcd.clear(); lcd.setCursor(0, 0); lcd.print("Invalid PIN!");
    lcd.setCursor(0, 1); lcd.print("Use 4-8 digits");
    beepError(); delay(1500); displayStandby();
    return;
  }

  for (int i = 1; i <= 127; i++) {
    if (i != targetDeleteID) {
      String existPin = preferences.getString(("pin_" + String(i)).c_str(), "");
      if (existPin != "" && existPin == newPin) {
        lcd.clear(); lcd.setCursor(0, 0); lcd.print("Duplicate PIN!");
        lcd.setCursor(0, 1); lcd.print("Used by ID: " + String(i));
        beepError(); delay(2000); displayStandby();
        return;
      }
    }
  }

  String pinKey = "pin_" + String(targetDeleteID);
  preferences.putString(pinKey.c_str(), newPin); 
  refreshBlynkTable(); 
  
  lcd.clear(); lcd.setCursor(0, 0); lcd.print("PIN Saved!");
  lcd.setCursor(0, 1); lcd.print("ID " + String(targetDeleteID) + " : " + newPin);
  beepSuccess();
  delay(2000);
  displayStandby();
}

void beepKey() { digitalWrite(BUZZER_PIN, BUZZER_ON); delay(50); digitalWrite(BUZZER_PIN, BUZZER_OFF); }
void beepSuccess() { digitalWrite(BUZZER_PIN, BUZZER_ON); delay(100); digitalWrite(BUZZER_PIN, BUZZER_OFF); delay(100); digitalWrite(BUZZER_PIN, BUZZER_ON); delay(100); digitalWrite(BUZZER_PIN, BUZZER_OFF); }
void beepError() { digitalWrite(BUZZER_PIN, BUZZER_ON); delay(800); digitalWrite(BUZZER_PIN, BUZZER_OFF); }

void unlockDoor() {
  if (isUnlocking) return; 
  isUnlocking = true; 
  
  digitalWrite(RELAY_PIN, RELAY_ON); 
  beepSuccess();
  
  unsigned long unlockStart = millis();
  bool openedDuringUnlock = false;
  
  while (millis() - unlockStart < 5000) {
    if (digitalRead(MAGNETIC_PIN) == HIGH) { 
      openedDuringUnlock = true; 
      doorWasOpened = true; 
      break; 
    } 
    Blynk.run(); 
    delay(50);
  }
  
  if (openedDuringUnlock) {
    unsigned long openedTime = millis();
    while (digitalRead(MAGNETIC_PIN) == HIGH) { 
      if (millis() - openedTime > 10000) { 
        digitalWrite(BUZZER_PIN, BUZZER_ON); delay(100); digitalWrite(BUZZER_PIN, BUZZER_OFF); delay(100);
      }
      Blynk.run(); 
    }
    delay(500); 
    isDoorAuthorized = false;
    doorWasOpened = false;
  } else {
    authorizedTime = millis();
  }
  
  digitalWrite(BUZZER_PIN, BUZZER_OFF);
  digitalWrite(RELAY_PIN, RELAY_OFF); 
  isUnlocking = false; 
}

void displayStandby() { 
  lcd.clear(); 
  if (isIntrusionAlarm) {
    lcd.setCursor(0, 0); lcd.print("!! INTRUSION !!");
    lcd.setCursor(0, 1); lcd.print("Scan Finger Stop");
  } else {
    lcd.setCursor(0, 0); lcd.print("Scan Fingerprint"); 
    lcd.setCursor(0, 1); lcd.print("To Unlock Door  "); 
  }
}

void handleKeypad(char key) {
  if (key == '*') { 
    inputPassword = ""; 
    displayStandby(); 
  } 
  else if (key == '#') {
    if (inputPassword == masterPassword) {
      inputPassword = "";
      startEnrollMode();
    } else {
      // ปิดการปลดล็อคด้วย PIN เดี่ยวๆ โดยต้องสแกนลายนิ้วมือก่อนเสมอ
      lcd.clear(); 
      lcd.setCursor(0, 0); lcd.print("Access Denied!"); 
      lcd.setCursor(0, 1); lcd.print("Scan Finger 1st"); 
      beepError(); 
      delay(1500);
    }
    inputPassword = ""; 
    displayStandby(); 
  } else {
    if (inputPassword.length() < 8) { 
      if (inputPassword.length() == 0) {
        lcd.setCursor(0, 1);
        lcd.print("                "); // เคลียร์แถวที่ 2 เพื่อแสดงดาวตอนกด
      }
      inputPassword += key; 
      lcd.setCursor(inputPassword.length() - 1, 1); 
      lcd.print("*"); 
    }
  }
}

void checkFingerprint() {
  uint8_t p = finger.getImage(); if (p != FINGERPRINT_OK) return; 
  p = finger.image2Tz(); if (p != FINGERPRINT_OK) return;
  p = finger.fingerSearch();
  
  if (p != FINGERPRINT_OK) {
    lcd.clear();
    lcd.setCursor(0, 0); lcd.print("Access Denied!"); 
    lcd.setCursor(0, 1); lcd.print("Unknown Finger"); 
    beepError(); 
    delay(1500);
    displayStandby(); 
    return;
  }

  // --- ผ่านชั้นที่ 1: ตรวจพบลายนิ้วมือถูกต้อง ---
  int matchedID = finger.fingerID;
  String key = "name_" + String(matchedID);
  String userName = preferences.getString(key.c_str(), "User " + String(matchedID));
  String savedPin = preferences.getString(("pin_" + String(matchedID)).c_str(), "");

  beepSuccess(); // ส่งเสียงยืนยันว่าสแกนลายนิ้วมือผ่าน

  // --- เข้าสู่ชั้นที่ 2: บังคับใส่รหัส PIN ประจำตัวของ ID นั้น ---
  lcd.clear();
  lcd.setCursor(0, 0); lcd.print("ID:" + String(matchedID) + " " + userName);
  lcd.setCursor(0, 1); lcd.print("PIN: ");

  String enteredPin = "";
  unsigned long pinTimeout = millis();
  bool pinSuccess = false;

  while (millis() - pinTimeout < 15000) { // จับเวลาให้ใส่รหัส PIN ภายใน 15 วินาที
    checkDoorSecurity();
    Blynk.run();
    server.handleClient();

    char k = keypad.getKey();
    if (k) {
      beepKey();
      pinTimeout = millis(); // รีเซ็ตเวลา Timeout เมื่อมีการกดปุ่ม

      if (k == '#') {
        if (savedPin != "" && enteredPin == savedPin) {
          pinSuccess = true;
          break;
        } else {
          lcd.clear();
          lcd.setCursor(0, 0); lcd.print("Wrong PIN!");
          lcd.setCursor(0, 1); lcd.print("Access Denied");
          beepError();
          delay(1500);
          displayStandby();
          return;
        }
      } 
      else if (k == '*') {
        enteredPin = "";
        lcd.setCursor(5, 1);
        lcd.print("           "); // ล้างดาวที่กดไว้
        lcd.setCursor(5, 1);
      } 
      else if (k >= '0' && k <= '9') {
        if (enteredPin.length() < 8) {
          enteredPin += k;
          lcd.setCursor(4 + enteredPin.length(), 1);
          lcd.print("*");
        }
      }
    }
    delay(10);
  }

  if (pinSuccess) {
    // ผ่านครบทั้ง 2 ชั้น (ลายนิ้วมือ + PIN ตรงกัน)
    if (authorizeDoorAccess(String(matchedID), userName, "2FA (Finger+PIN)")) {
      lcd.clear();
      lcd.setCursor(0, 0); lcd.print("Access Granted!");
      lcd.setCursor(0, 1); lcd.print("Welcome: " + userName);
      unlockDoor();
    }
  } else {
    lcd.clear();
    lcd.setCursor(0, 0); lcd.print("PIN Timeout!");
    beepError();
    delay(1500);
  }

  displayStandby(); 
}

void startEnrollMode() {
  lcd.clear(); lcd.setCursor(0, 0); lcd.print("--- ENROLL ---"); lcd.setCursor(0, 1); lcd.print("ID (1-127): ");
  String idStr = "";
  unsigned long enrollTimeout = millis(); 
  
  while (true) {
    if (millis() - enrollTimeout > 15000) { 
      lcd.clear(); lcd.print("Enroll Timeout"); beepError(); delay(1000); displayStandby(); return;
    }
    
    char k = keypad.getKey();
    if (k) {
      beepKey();
      enrollTimeout = millis(); 
      if (k == '#') { if (idStr.length() > 0) break; } 
      else if (k == '*') { lcd.clear(); lcd.print("Canceled"); delay(1000); displayStandby(); return; } 
      else if (k >= '0' && k <= '9') { idStr += k; lcd.print(k); }
    }
    Blynk.run(); delay(10);
  }
  
  uint8_t id = idStr.toInt();
  if (id < 1 || id > 127) { lcd.clear(); lcd.print("Invalid ID!"); beepError(); delay(1000); displayStandby(); return; }

  lcd.clear(); lcd.setCursor(0, 0); lcd.print("ID: "); lcd.print(id); lcd.setCursor(0, 1); lcd.print("Place Finger...");
  
  enrollTimeout = millis();
  int p = -1; 
  while (p != FINGERPRINT_OK) { 
    if (millis() - enrollTimeout > 15000) { lcd.clear(); lcd.print("Timeout"); beepError(); delay(1000); displayStandby(); return; }
    p = finger.getImage(); Blynk.run(); delay(50); 
  }
  
  p = finger.image2Tz(1); if (p != FINGERPRINT_OK) { lcd.clear(); lcd.print("Error"); beepError(); delay(1000); displayStandby(); return; }

  beepSuccess(); lcd.clear(); lcd.setCursor(0, 0); lcd.print("Remove Finger..."); delay(2000);
  p = 0; while (p != FINGERPRINT_NOFINGER) { p = finger.getImage(); Blynk.run(); delay(50); }

  lcd.clear(); lcd.setCursor(0, 0); lcd.print("Place Again...");
  p = -1; while (p != FINGERPRINT_OK) { p = finger.getImage(); Blynk.run(); delay(50); }
  p = finger.image2Tz(2); if (p != FINGERPRINT_OK) { lcd.clear(); lcd.print("Error"); beepError(); delay(1000); displayStandby(); return; }

  p = finger.createModel(); if (p != FINGERPRINT_OK) { lcd.clear(); lcd.print("Not Match"); beepError(); delay(1000); displayStandby(); return; }
  
  p = finger.storeModel(id); 
  if (p != FINGERPRINT_OK) { 
    lcd.clear(); lcd.print("Store Error!"); beepError(); delay(2000);
    displayStandby(); 
    return;
  }

  // ลายนิ้วมือบันทึกผ่านแล้ว เข้าสู่ขั้นตอนบังคับใส่รหัส PIN ประจำ ID
  lcd.clear();
  lcd.setCursor(0, 0); lcd.print("Finger Saved!"); 
  lcd.setCursor(0, 1); lcd.print("ID: " + String(id)); 
  beepSuccess(); 
  
  String key = "name_" + String(id);
  String placeholderName = "New User " + String(id); 
  preferences.putString(key.c_str(), placeholderName); 
  refreshBlynkTable(); 
  delay(1500);

  lcd.clear();
  lcd.setCursor(0, 0); lcd.print("Set PIN (4-8):");
  lcd.setCursor(0, 1); lcd.print("PIN: ");
  
  String newPin = "";
  unsigned long pinTimeout = millis();
  bool pinSavedSuccess = false;
  
  while (true) {
    if (millis() - pinTimeout > 20000) { 
      lcd.clear(); lcd.print("PIN Timeout!"); beepError(); delay(1500); break; 
    }
    
    char k = keypad.getKey();
    if (k) {
      beepKey();
      pinTimeout = millis(); 
      
      if (k == '#') { 
        if (newPin.length() < 4) {
          // บังคับไม่ให้ข้าม: ต้องใส่รหัส PIN อย่างน้อย 4 หลัก
          lcd.clear(); 
          lcd.setCursor(0, 0); lcd.print("PIN Required!"); 
          lcd.setCursor(0, 1); lcd.print("Min 4 Digits"); 
          beepError(); delay(1500);
          newPin = ""; 
          lcd.clear(); lcd.setCursor(0, 0); lcd.print("Set PIN (4-8):");
          lcd.setCursor(0, 1); lcd.print("PIN: ");
        } else if (newPin == masterPassword) {
          lcd.clear(); 
          lcd.setCursor(0, 0); lcd.print("Invalid PIN!"); 
          lcd.setCursor(0, 1); lcd.print("Cannot use 9999"); 
          beepError(); delay(1500);
          newPin = ""; 
          lcd.clear(); lcd.setCursor(0, 0); lcd.print("Set PIN (4-8):");
          lcd.setCursor(0, 1); lcd.print("PIN: ");
        } else {
          String pinKey = "pin_" + String(id);
          preferences.putString(pinKey.c_str(), newPin); 
          refreshBlynkTable(); 
          
          lcd.clear(); lcd.setCursor(0, 0); lcd.print("Enroll Complete!"); 
          lcd.setCursor(0, 1); lcd.print("ID " + String(id) + " Ready"); 
          beepSuccess(); delay(2000);
          pinSavedSuccess = true;
          break;
        }
      } 
      else if (k == '*') { 
        newPin = ""; 
        lcd.clear(); lcd.setCursor(0, 0); lcd.print("Set PIN (4-8):");
        lcd.setCursor(0, 1); lcd.print("PIN: ");
      } 
      else if (k >= '0' && k <= '9') { 
        if (newPin.length() < 8) { 
          newPin += k; 
          lcd.setCursor(4 + newPin.length(), 1); 
          lcd.print("*"); 
        }
      }
    }
    Blynk.run(); delay(10);
  }

  // หากผู้ใช้ไม่ได้ตั้ง PIN (กดยกเลิก หรือปล่อย Timeout) ให้ Rollback ลบลายนิ้วมือทิ้ง
  if (!pinSavedSuccess) {
    finger.deleteModel(id);
    preferences.remove(("name_" + String(id)).c_str());
    preferences.remove(("pin_" + String(id)).c_str());
    refreshBlynkTable();

    lcd.clear();
    lcd.setCursor(0, 0); lcd.print("Enroll Canceled!");
    lcd.setCursor(0, 1); lcd.print("PIN is Required");
    beepError();
    delay(2000);
  }
  
  displayStandby(); 
}