#include <WiFi.h>
#include <WiFiManager.h> // เพิ่มไลบรารี WiFiManager
#include <HTTPClient.h>
#include <WiFiClientSecure.h> // เพิ่มบรรทัดนี้เข้าไป
#include "esp_camera.h"
#include <esp_now.h>
#include <base64.h>

// ==========================================
// 1. ตั้งค่า URL ของ Google Apps Script
// ==========================================
// *** นำ Web App URL ที่ได้จากการ Deploy รอบล่าสุดมาใส่ที่นี่ ***
String GOOGLE_SCRIPT_URL = "https://script.google.com/macros/s/AKfycbyruKHkTOsiAojP-Lkezip0E5S0sbet7Ho0VwXIG-lnkswJgGH4L2oR12GXgaonornNvg/exec";
const String NTFY_TOPIC = "Warin_SmartLock_Alert_2026";

// ==========================================
// ขาของกล้อง (บอร์ด AI Thinker)
// ==========================================
#define PWDN_GPIO_NUM     32
#define RESET_GPIO_NUM    -1
#define XCLK_GPIO_NUM      0
#define SIOD_GPIO_NUM     26
#define SIOC_GPIO_NUM     27
#define Y9_GPIO_NUM       35
#define Y8_GPIO_NUM       34
#define Y7_GPIO_NUM       39
#define Y6_GPIO_NUM       36
#define Y5_GPIO_NUM       21
#define Y4_GPIO_NUM       19
#define Y3_GPIO_NUM       18
#define Y2_GPIO_NUM        5
#define VSYNC_GPIO_NUM    25
#define HREF_GPIO_NUM     23
#define PCLK_GPIO_NUM     22

bool takePicture = false;
String currentUserID = "Unknown";
String currentUserName = "Unknown"; // เพิ่มตัวแปรนี้เข้ามา

// ฟังก์ชันรับสัญญาณจากบอร์ดหลัก (ESP-NOW) - อัปเดตแก้ Error สำหรับบอร์ดเวอร์ชัน 3.x
void OnDataRecv(const esp_now_recv_info *info, const uint8_t *incomingData, int len) {
  String msg = "";
  for (int i = 0; i < len; i++) {
    msg += (char)incomingData[i];
  }
  Serial.println("📥 ได้รับคำสั่งจากบอร์ดหลัก: " + msg);
  
  // เช็คว่าคำสั่งขึ้นต้นด้วย "CAPTURE:" หรือไม่
  if (msg.startsWith("CAPTURE:")) {
    String data = msg.substring(8); // ตัดคำว่า CAPTURE: ออก จะเหลือเช่น "1|Warin"
    
    int separatorIndex = data.indexOf('|'); // หาตำแหน่งเครื่องหมาย |
    if (separatorIndex != -1) {
      currentUserID = data.substring(0, separatorIndex);      // ดึงตัวเลข ID (ด้านหน้า |)
      currentUserName = data.substring(separatorIndex + 1);   // ดึงชื่อ (ด้านหลัง |)
    } else {
      currentUserID = data;
      currentUserName = "Unknown";
    }
    
    takePicture = true; 
  }
}

void setup() {
  Serial.begin(115200);

  // ==========================================
  // ตั้งค่ากล้อง
  // ==========================================
  camera_config_t config;
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer = LEDC_TIMER_0;
  config.pin_d0 = Y2_GPIO_NUM;
  config.pin_d1 = Y3_GPIO_NUM;
  config.pin_d2 = Y4_GPIO_NUM;
  config.pin_d3 = Y5_GPIO_NUM;
  config.pin_d4 = Y6_GPIO_NUM;
  config.pin_d5 = Y7_GPIO_NUM;
  config.pin_d6 = Y8_GPIO_NUM;
  config.pin_d7 = Y9_GPIO_NUM;
  config.pin_xclk = XCLK_GPIO_NUM;
  config.pin_pclk = PCLK_GPIO_NUM;
  config.pin_vsync = VSYNC_GPIO_NUM;
  config.pin_href = HREF_GPIO_NUM;
  config.pin_sscb_sda = SIOD_GPIO_NUM;
  config.pin_sscb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn = PWDN_GPIO_NUM;
  config.pin_reset = RESET_GPIO_NUM;
  config.xclk_freq_hz = 10000000; // หรือลอง 16000000
  config.pixel_format = PIXFORMAT_JPEG;
  
  // ตั้งค่าขนาดภาพเป็น VGA (640x480) 
  config.frame_size = FRAMESIZE_VGA;
  config.jpeg_quality = 12;
  config.fb_count = 1;

  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("❌ Camera init failed: 0x%x", err);
    return;
  }
  Serial.println("✅ Camera Ready!");
  
  // ดึงค่าเซ็นเซอร์กล้องมาตั้งค่า
  sensor_t * s = esp_camera_sensor_get();
  s->set_vflip(s, 1);   // 1 = พลิกภาพแนวตั้ง (แก้ภาพกลับหัว)
  s->set_hmirror(s, 1); // 1 = พลิกภาพแนวนอน (แก้ภาพกลับซ้ายขวาเหมือนส่องกระจก)
  // ==========================================
  // ส่วนของ WiFi Manager
  // ==========================================
  WiFi.mode(WIFI_STA); // ตั้งค่าโหมดให้อยู่ใน Station เพื่อรองรับ ESP-NOW ด้วย
  WiFiManager wm;
  wm.setConfigPortalTimeout(60); // ถ้าไม่มีเน็ตเกิน 60 วินาที ให้ข้ามหน้าตั้งค่าไปทำงานต่อ
  
  // wm.resetSettings(); // (เอาคอมเมนต์ออกถ้าอยากล้างค่า Wi-Fi เดิมที่เคยจำไว้)

  Serial.println("กำลังเชื่อมต่อ WiFi...");
  
  // ปล่อย Hotspot ชื่อ ESP32_CAM_AP ออกมาให้เราตั้งค่า
  bool res = wm.autoConnect("ESP32_CAM_AP"); 
  
  if(!res) {
    Serial.println("Failed to connect Wi-Fi");
  } 
  Serial.println("\n✅ WiFi Connected!");

  // ==========================================
  // เริ่มต้น ESP-NOW (หลังจากเชื่อมต่อ Wi-Fi สำเร็จ)
  // ==========================================
  if (esp_now_init() != ESP_OK) {
    Serial.println("❌ Error initializing ESP-NOW");
    return;
  }
  esp_now_register_recv_cb(OnDataRecv);
  Serial.println("✅ ESP-NOW Ready (รอรับคำสั่ง...)");
}

void sendPhotoDirectlyToNtfy(camera_fb_t *fb) {
  if (WiFi.status() == WL_CONNECTED && fb != NULL) {
    Serial.println("🚨 กำลังส่งภาพคนร้ายตรงเข้า ntfy.sh...");
    HTTPClient http;
    // ใช้ http ปกติเพื่อความรวดเร็วและประหยัด RAM ของ ESP32-CAM
    http.begin("http://ntfy.sh/" + NTFY_TOPIC);
    http.addHeader("Title", "INTRUSION CAMERA!");
    http.addHeader("Message", "Captured intruder photo from ESP32-CAM");
    http.addHeader("Priority", "max");
    http.addHeader("Tags", "camera,rotating_light");
    http.addHeader("Filename", "intruder.jpg");
    http.addHeader("Content-Type", "image/jpeg");

    // ส่งข้อมูลภาพดิบ (Binary) ไปที่ ntfy.sh โดยตรง
    int code = http.PUT(fb->buf, fb->len);
    Serial.printf("📸 ส่งภาพเข้า ntfy สำเร็จ! HTTP Code: %d", code);
    http.end();
  }
}

void sendImageToGoogleScript() {
  Serial.println("📸 กำลังเคลียร์ภาพเก่า...");
  
  // 1. ถ่ายภาพทิ้ง 1 ครั้ง เพื่อล้าง Buffer ที่ค้างอยู่
  camera_fb_t *dummy_fb = esp_camera_fb_get();
  if (dummy_fb) {
    esp_camera_fb_return(dummy_fb); // โยนรูปเก่าทิ้ง
  }
  
  // หน่วงเวลาให้เซ็นเซอร์กล้องปรับแสงสว่างอัตโนมัติ (Auto Exposure) ให้ภาพชัดขึ้น
  delay(300); 

  Serial.println("📸 กำลังถ่ายภาพปัจจุบัน...");
  
  // 2. ถ่ายภาพของจริง
  camera_fb_t *fb = esp_camera_fb_get();
  String imageBase64 = "";

  if (!fb) {
    Serial.println("⚠️ ถ่ายภาพไม่สำเร็จ! จะส่งเฉพาะข้อความแจ้งเตือน...");
    // ไม่ return ออก เพื่อให้กระบวนการส่งแจ้งเตือนทำงานต่อได้
  } else {
    // ---> ย้ายมาวางตรงนี้ (ใน else และก่อนคืนหน่วยความจำ fb) <---
    if (currentUserID == "ALERT") {
      sendPhotoDirectlyToNtfy(fb);
    }

    Serial.println("🔄 แปลงภาพเป็น Base64...");
    imageBase64 = base64::encode(fb->buf, fb->len);
    esp_camera_fb_return(fb); // คืนหน่วยความจำของภาพจริง
  }

  Serial.println("🚀 กำลังส่งข้อมูลไปที่ Google Apps Script...");

  WiFiClientSecure client;
  client.setInsecure(); 

  HTTPClient http;
  http.begin(client, GOOGLE_SCRIPT_URL); 
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS); 
  http.setTimeout(20000); 

  http.addHeader("Content-Type", "application/json");

  // สร้าง Payload โดย imageBase64 อาจจะมีค่า (ถ้าถ่ายภาพสำเร็จ) หรือว่างเปล่า (ถ้าถ่ายไม่สำเร็จ)
  String payload = "{\"id\":\"" + currentUserID + "\",\"name\":\"" + currentUserName + "\",\"status\":\"Access Granted\",\"image\":\"" + imageBase64 + "\"}";
  
  int httpResponseCode = http.POST(payload);

  if (httpResponseCode > 0) {
    Serial.print("✅ ส่งสำเร็จ! HTTP Code: ");
    Serial.println(httpResponseCode); 
  } else {
    Serial.print("❌ ส่งพร้อมภาพไม่สำเร็จ Error: ");
    Serial.println(httpResponseCode);
    
    // ---- ระบบป้องกันความผิดพลาด (Fallback): ลองส่งเฉพาะข้อความอีกครั้ง ----
    if (imageBase64 != "") {
       Serial.println("⚠️ กำลังพยายามส่งข้อความแจ้งเตือนแบบไม่มีภาพซ้ำอีกครั้ง...");
       http.end(); // ปิดการเชื่อมต่อเดิมก่อน
       
       http.begin(client, GOOGLE_SCRIPT_URL); 
       http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
       http.setTimeout(10000); // ตั้งเวลาใหม่ให้สั้นลง
       http.addHeader("Content-Type", "application/json");
       
       // สร้าง Payload ใหม่แบบไม่มีรูปภาพ
       String fallbackPayload = "{\"id\":\"" + currentUserID + "\",\"name\":\"" + currentUserName + "\",\"status\":\"Access Granted\",\"image\":\"\"}";
       
       int fallbackResponseCode = http.POST(fallbackPayload);
       
       if (fallbackResponseCode > 0) {
          Serial.print("✅ ส่งแจ้งเตือนแบบไม่มีภาพสำเร็จ! HTTP Code: ");
          Serial.println(fallbackResponseCode);
       } else {
          Serial.print("❌ ล้มเหลวทั้งหมด ไม่สามารถติดต่อ Server ได้ Error: ");
          Serial.println(fallbackResponseCode);
       }
    }
  }
  
  http.end();
}

void loop() {
  if (takePicture) {
    sendImageToGoogleScript();
    takePicture = false; 
  }
  delay(10);
}