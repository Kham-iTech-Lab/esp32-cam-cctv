/*
 * ESP32-CAM CCTV — กล้องวงจรปิดราคาหลักร้อย ดูสดผ่านมือถือ
 * K iTech Review (TikTok) • Kham iTech Lab (YouTube / Facebook)
 *
 * ทำอะไรได้
 *   - ดูภาพสด (MJPEG) ผ่านเบราว์เซอร์ในมือถือ/คอม ใน Wi-Fi บ้าน
 *   - ถ่ายภาพนิ่ง (กดแล้วได้ไฟล์ .jpg)
 *   - เปิด/ปิดไฟแฟลช LED บนบอร์ด
 *   - ปรับความละเอียดภาพ และกลับหัวภาพได้จากหน้าเว็บ
 *
 * บอร์ด: AI-Thinker ESP32-CAM (OV2640)
 * Arduino IDE > Tools > Board > "AI Thinker ESP32-CAM"
 * ไม่ต้องติดตั้งไลบรารีเพิ่ม (esp_camera มากับ ESP32 board package แล้ว)
 *
 * การอัปโหลด (ใช้ USB-TTL หรือบอร์ด ESP32-CAM-MB)
 *   USB-TTL 5V -> 5V, GND -> GND, TX -> U0R, RX -> U0T
 *   ต่อ IO0 ลง GND แล้วกด RESET ก่อนอัปโหลด -> อัปโหลดเสร็จถอด IO0 แล้วกด RESET อีกครั้ง
 *   (ถ้าใช้บอร์ด ESP32-CAM-MB เสียบ USB แล้วอัปโหลดได้เลย)
 */

#include <Arduino.h>
#include <WiFi.h>
#include "esp_camera.h"
#include "esp_http_server.h"

// ------------------------------------------------------------------ ตั้งค่า
const char* WIFI_SSID = "ชื่อ Wi-Fi บ้าน";
const char* WIFI_PASS = "รหัส Wi-Fi";

// ขาของบอร์ด AI-Thinker ESP32-CAM
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
#define FLASH_LED_PIN      4

httpd_handle_t webServer = NULL;     // หน้าเว็บ + คำสั่ง (พอร์ต 80)
httpd_handle_t streamServer = NULL;  // ภาพสด (พอร์ต 81)
bool flashOn = false;

// ------------------------------------------------------------------ หน้าเว็บ
static const char INDEX_HTML[] PROGMEM = R"HTML(<!doctype html>
<html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>ESP32-CAM CCTV</title><style>
body{font-family:sans-serif;background:#0b1530;color:#fff;margin:0;padding:12px;text-align:center}
h1{font-size:20px;margin:6px 0 10px}
img{width:100%;max-width:800px;border-radius:12px;background:#000}
.r{display:flex;gap:8px;justify-content:center;flex-wrap:wrap;margin:12px 0}
button,select{background:#1c2b55;border:2px solid #3cc8ff;color:#fff;border-radius:12px;padding:12px 16px;font-size:16px}
.s{color:#9ab;font-size:13px}
</style></head><body>
<h1>ESP32-CAM CCTV</h1>
<img id="v" alt="กำลังโหลดภาพ...">
<div class="r">
  <button onclick="location.href='/capture'">ถ่ายภาพ</button>
  <button onclick="fetch('/flash').then(r=>r.text()).then(t=>this.innerText='แฟลช: '+t)">แฟลช: ปิด</button>
  <button onclick="fetch('/flip')">กลับหัวภาพ</button>
  <select onchange="fetch('/res?v='+this.value)">
    <option value="5">QVGA 320x240</option>
    <option value="8" selected>VGA 640x480</option>
    <option value="9">SVGA 800x600</option>
    <option value="10">XGA 1024x768</option>
  </select>
</div>
<div class="s">ภาพสดจากพอร์ต 81 • ยิ่งความละเอียดสูง ภาพยิ่งกระตุก</div>
<script>document.getElementById('v').src=location.protocol+'//'+location.hostname+':81/stream';</script>
</body></html>)HTML";

static esp_err_t indexHandler(httpd_req_t* req) {
  httpd_resp_set_type(req, "text/html; charset=utf-8");
  return httpd_resp_send(req, INDEX_HTML, strlen(INDEX_HTML));
}

// ------------------------------------------------------------------ ถ่ายภาพนิ่ง
static esp_err_t captureHandler(httpd_req_t* req) {
  camera_fb_t* fb = esp_camera_fb_get();
  if (!fb) { httpd_resp_send_500(req); return ESP_FAIL; }
  httpd_resp_set_type(req, "image/jpeg");
  httpd_resp_set_hdr(req, "Content-Disposition", "inline; filename=capture.jpg");
  esp_err_t res = httpd_resp_send(req, (const char*)fb->buf, fb->len);
  esp_camera_fb_return(fb);
  return res;
}

// ------------------------------------------------------------------ คำสั่งจากหน้าเว็บ
static esp_err_t flashHandler(httpd_req_t* req) {
  flashOn = !flashOn;
  digitalWrite(FLASH_LED_PIN, flashOn ? HIGH : LOW);
  const char* t = flashOn ? "เปิด" : "ปิด";
  httpd_resp_set_type(req, "text/plain; charset=utf-8");
  return httpd_resp_send(req, t, strlen(t));
}

static esp_err_t flipHandler(httpd_req_t* req) {
  sensor_t* s = esp_camera_sensor_get();
  static bool flipped = false;
  flipped = !flipped;
  s->set_vflip(s, flipped);
  s->set_hmirror(s, flipped);
  return httpd_resp_send(req, "OK", 2);
}

static esp_err_t resHandler(httpd_req_t* req) {
  char q[32], v[8];
  if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK &&
      httpd_query_key_value(q, "v", v, sizeof(v)) == ESP_OK) {
    int f = atoi(v);
    if (f >= FRAMESIZE_QVGA && f <= FRAMESIZE_XGA) {
      esp_camera_sensor_get()->set_framesize(esp_camera_sensor_get(), (framesize_t)f);
    }
  }
  return httpd_resp_send(req, "OK", 2);
}

// ------------------------------------------------------------------ ภาพสด MJPEG
#define BOUNDARY "frame"
static const char* STREAM_TYPE = "multipart/x-mixed-replace;boundary=" BOUNDARY;
static const char* STREAM_SEP  = "\r\n--" BOUNDARY "\r\n";
static const char* STREAM_HDR  = "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n";

static esp_err_t streamHandler(httpd_req_t* req) {
  esp_err_t res = httpd_resp_set_type(req, STREAM_TYPE);
  if (res != ESP_OK) return res;
  char hdr[64];
  while (true) {
    camera_fb_t* fb = esp_camera_fb_get();
    if (!fb) { res = ESP_FAIL; break; }
    size_t n = snprintf(hdr, sizeof(hdr), STREAM_HDR, fb->len);
    res = httpd_resp_send_chunk(req, STREAM_SEP, strlen(STREAM_SEP));
    if (res == ESP_OK) res = httpd_resp_send_chunk(req, hdr, n);
    if (res == ESP_OK) res = httpd_resp_send_chunk(req, (const char*)fb->buf, fb->len);
    esp_camera_fb_return(fb);
    if (res != ESP_OK) break;   // ผู้ชมปิดหน้าเว็บแล้ว
  }
  return res;
}

void startServers() {
  httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
  cfg.server_port = 80;
  httpd_uri_t uris[] = {
    {"/",        HTTP_GET, indexHandler,   NULL},
    {"/capture", HTTP_GET, captureHandler, NULL},
    {"/flash",   HTTP_GET, flashHandler,   NULL},
    {"/flip",    HTTP_GET, flipHandler,    NULL},
    {"/res",     HTTP_GET, resHandler,     NULL},
  };
  if (httpd_start(&webServer, &cfg) == ESP_OK) {
    for (auto& u : uris) httpd_register_uri_handler(webServer, &u);
  }

  cfg.server_port = 81;
  cfg.ctrl_port += 1;            // แต่ละเซิร์ฟเวอร์ต้องใช้ ctrl_port ไม่ซ้ำกัน
  httpd_uri_t stream = {"/stream", HTTP_GET, streamHandler, NULL};
  if (httpd_start(&streamServer, &cfg) == ESP_OK) {
    httpd_register_uri_handler(streamServer, &stream);
  }
}

// ------------------------------------------------------------------ setup / loop
void setup() {
  Serial.begin(115200);
  pinMode(FLASH_LED_PIN, OUTPUT);
  digitalWrite(FLASH_LED_PIN, LOW);

  camera_config_t c = {};
  c.ledc_channel = LEDC_CHANNEL_0;
  c.ledc_timer   = LEDC_TIMER_0;
  c.pin_d0 = Y2_GPIO_NUM;  c.pin_d1 = Y3_GPIO_NUM;  c.pin_d2 = Y4_GPIO_NUM;  c.pin_d3 = Y5_GPIO_NUM;
  c.pin_d4 = Y6_GPIO_NUM;  c.pin_d5 = Y7_GPIO_NUM;  c.pin_d6 = Y8_GPIO_NUM;  c.pin_d7 = Y9_GPIO_NUM;
  c.pin_xclk = XCLK_GPIO_NUM;   c.pin_pclk = PCLK_GPIO_NUM;
  c.pin_vsync = VSYNC_GPIO_NUM; c.pin_href = HREF_GPIO_NUM;
  c.pin_sccb_sda = SIOD_GPIO_NUM; c.pin_sccb_scl = SIOC_GPIO_NUM;
  c.pin_pwdn = PWDN_GPIO_NUM;   c.pin_reset = RESET_GPIO_NUM;
  c.xclk_freq_hz = 20000000;
  c.pixel_format = PIXFORMAT_JPEG;
  c.grab_mode = CAMERA_GRAB_LATEST;   // เอาภาพล่าสุดเสมอ ภาพสดไม่ดีเลย์
  if (psramFound()) {
    c.frame_size = FRAMESIZE_VGA;  c.jpeg_quality = 12; c.fb_count = 2; c.fb_location = CAMERA_FB_IN_PSRAM;
  } else {
    c.frame_size = FRAMESIZE_QVGA; c.jpeg_quality = 15; c.fb_count = 1; c.fb_location = CAMERA_FB_IN_DRAM;
  }

  if (esp_camera_init(&c) != ESP_OK) {
    Serial.println("เปิดกล้องไม่สำเร็จ — เช็กสายแพกล้องและไฟเลี้ยง 5V แล้วกด RESET");
    while (true) delay(1000);
  }

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);              // ปิดโหมดประหยัดไฟ Wi-Fi ภาพสดลื่นขึ้น
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.print("กำลังต่อ Wi-Fi");
  while (WiFi.status() != WL_CONNECTED) { delay(400); Serial.print("."); }

  startServers();
  Serial.printf("\nเปิดในมือถือ: http://%s\n", WiFi.localIP().toString().c_str());
}

void loop() {
  delay(10000);   // งานทั้งหมดทำในเซิร์ฟเวอร์ ไม่ต้องทำอะไรใน loop
}
