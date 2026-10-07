/*
 * sd_storage.h — บันทึกภาพลง microSD + ถ่ายอัตโนมัติ (timelapse) + หน้าดูรูปย้อนหลัง
 * K iTech Review (TikTok) • Kham iTech Lab (YouTube / Facebook)
 *
 * ใช้คู่กับไฟล์ .ino หลัก — ก่อน #include ไฟล์นี้ให้กำหนดอย่างใดอย่างหนึ่ง
 *   SD แบบ SD_MMC (ESP32-CAM, Freenove S3):  ไม่ต้องกำหนดอะไร หรือกำหนด SD_MMC_CLK / SD_MMC_CMD / SD_MMC_D0 (เฉพาะ S3)
 *   SD แบบ SPI (XIAO ESP32S3 Sense):          #define SD_USE_SPI  และ  #define SD_CS_PIN 21
 *
 * การ์ดที่ใช้: microSD ไม่เกิน 32GB ฟอร์แมตเป็น FAT32
 */
#pragma once
#include <Arduino.h>
#include <FS.h>
#include <time.h>
#include <vector>
#include <algorithm>
#include <Preferences.h>
#include "esp_camera.h"
#include "esp_http_server.h"

#if defined(SD_USE_SPI)
  #include <SPI.h>
  #include <SD.h>
  #define SDX SD
#else
  #include <SD_MMC.h>
  #define SDX SD_MMC
#endif

namespace sdcam {

const char* DIR = "/photos";
const uint64_t MIN_FREE = 200ULL * 1024 * 1024;   // เหลือน้อยกว่า 200MB -> ลบรูปเก่าสุดทิ้ง

bool ready = false;
uint32_t intervalSec = 0;      // 0 = ปิดถ่ายอัตโนมัติ
uint32_t lastShot = 0;
uint32_t savedCount = 0;

// ---------------------------------------------------------------- เริ่มต้นการ์ด
bool begin() {
#if defined(SD_USE_SPI)
  ready = SD.begin(SD_CS_PIN);
#else
  #if defined(SD_MMC_CLK)
  SD_MMC.setPins(SD_MMC_CLK, SD_MMC_CMD, SD_MMC_D0);
  #endif
  ready = SD_MMC.begin("/sdcard", true);   // true = โหมด 1-bit ไม่ชนขาไฟแฟลช
#endif
  if (!ready) return false;
  if (!SDX.exists(DIR)) SDX.mkdir(DIR);
  Preferences p;
  p.begin("sdcam", true);
  intervalSec = p.getUInt("iv", 0);        // จำค่าถ่ายอัตโนมัติไว้ ไฟดับแล้วกลับมาถ่ายต่อเอง
  p.end();
  lastShot = millis();
  return true;
}

uint64_t freeBytes() { return SDX.totalBytes() - SDX.usedBytes(); }

void setInterval(uint32_t s) {
  intervalSec = s;
  lastShot = millis();
  Preferences p;
  p.begin("sdcam", false);
  p.putUInt("iv", s);
  p.end();
}

String baseName(String n) {
  int s = n.lastIndexOf('/');
  return s >= 0 ? n.substring(s + 1) : n;
}

std::vector<String> listFiles() {
  std::vector<String> v;
  File d = SDX.open(DIR);
  if (!d) return v;
  File f;
  while ((f = d.openNextFile())) {
    if (!f.isDirectory()) v.push_back(baseName(f.name()));
    f.close();
  }
  d.close();
  std::sort(v.begin(), v.end());           // ชื่อไฟล์เป็นวันเวลา เรียงแล้วเก่า -> ใหม่
  return v;
}

// การ์ดใกล้เต็ม -> ลบรูปเก่าสุดทีละไฟล์ (บันทึกวนได้แบบกล้องวงจรปิด)
void makeRoom() {
  if (freeBytes() >= MIN_FREE) return;
  std::vector<String> v = listFiles();
  for (size_t i = 0; i < v.size() && freeBytes() < MIN_FREE; i++) {
    SDX.remove(String(DIR) + "/" + v[i]);
  }
}

String newName() {
  struct tm t;
  char b[48];
  if (getLocalTime(&t, 10) && t.tm_year > 120) {
    char ts[24];
    strftime(ts, sizeof(ts), "%Y%m%d_%H%M%S", &t);
    snprintf(b, sizeof(b), "%s/%s_%03lu.jpg", DIR, ts, (unsigned long)(millis() % 1000));
  } else {
    // ยังไม่ได้เวลาจากอินเทอร์เน็ต ใช้เวลาตั้งแต่เปิดเครื่องแทน
    snprintf(b, sizeof(b), "%s/boot_%010lu.jpg", DIR, (unsigned long)millis());
  }
  return b;
}

// ถ่าย 1 ภาพแล้วบันทึก คืนชื่อไฟล์ (ว่าง = ไม่สำเร็จ)
String snap() {
  if (!ready) return "";
  makeRoom();
  camera_fb_t* fb = esp_camera_fb_get();
  if (!fb) return "";
  String n = newName();
  File f = SDX.open(n, FILE_WRITE);
  bool ok = f && f.write(fb->buf, fb->len) == fb->len;
  if (f) f.close();
  esp_camera_fb_return(fb);
  if (!ok) return "";
  savedCount++;
  return baseName(n);
}

// เรียกใน loop() — ถ่ายอัตโนมัติตามรอบเวลา
void loop() {
  if (ready && intervalSec && millis() - lastShot >= intervalSec * 1000UL) {
    lastShot = millis();
    String n = snap();
    if (n.length()) Serial.println("บันทึกอัตโนมัติ: " + n);
  }
}

// ---------------------------------------------------------------- หน้าเว็บ
bool safeName(const char* n) {
  return n[0] && !strchr(n, '/') && !strstr(n, "..");
}

bool getArg(httpd_req_t* req, const char* key, char* out, size_t len) {
  char q[96];
  return httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK &&
         httpd_query_key_value(q, key, out, len) == ESP_OK;
}

esp_err_t sendText(httpd_req_t* req, const String& s) {
  httpd_resp_set_type(req, "text/plain; charset=utf-8");
  return httpd_resp_send(req, s.c_str(), s.length());
}

esp_err_t snapHandler(httpd_req_t* req) {
  if (!ready) return sendText(req, "ไม่พบการ์ด SD");
  String n = snap();
  return sendText(req, n.length() ? "บันทึกแล้ว: " + n : String("บันทึกไม่สำเร็จ"));
}

esp_err_t intervalHandler(httpd_req_t* req) {
  char v[12];
  if (getArg(req, "s", v, sizeof(v))) setInterval(strtoul(v, NULL, 10));
  return sendText(req, "OK");
}

esp_err_t statusHandler(httpd_req_t* req) {
  if (!ready) return sendText(req, "การ์ด SD: ไม่พบการ์ด (ใส่การ์ด FAT32 แล้วกด RESET)");
  char b[160];
  snprintf(b, sizeof(b), "การ์ด SD: ว่าง %.1f / %.1f GB • บันทึกรอบนี้ %lu รูป • ถ่ายอัตโนมัติ: %s",
           freeBytes() / 1073741824.0, SDX.totalBytes() / 1073741824.0, (unsigned long)savedCount,
           intervalSec ? (String("ทุก ") + intervalSec + " วิ").c_str() : "ปิด");
  return sendText(req, b);
}

esp_err_t fileHandler(httpd_req_t* req) {
  char n[48];
  if (!ready || !getArg(req, "n", n, sizeof(n)) || !safeName(n)) { httpd_resp_send_404(req); return ESP_FAIL; }
  File f = SDX.open(String(DIR) + "/" + n);
  if (!f) { httpd_resp_send_404(req); return ESP_FAIL; }
  httpd_resp_set_type(req, "image/jpeg");
  static uint8_t buf[4096];
  size_t r;
  esp_err_t res = ESP_OK;
  while ((r = f.read(buf, sizeof(buf))) > 0) {
    if ((res = httpd_resp_send_chunk(req, (const char*)buf, r)) != ESP_OK) break;
  }
  f.close();
  httpd_resp_send_chunk(req, NULL, 0);
  return res;
}

esp_err_t delHandler(httpd_req_t* req) {
  char n[48];
  if (ready && getArg(req, "n", n, sizeof(n)) && safeName(n)) SDX.remove(String(DIR) + "/" + n);
  httpd_resp_set_status(req, "303 See Other");
  httpd_resp_set_hdr(req, "Location", "/sd/list");
  return httpd_resp_send(req, NULL, 0);
}

// หน้าแกลเลอรี: รูปล่าสุด 60 รูป ใหม่สุดอยู่บน
esp_err_t listHandler(httpd_req_t* req) {
  httpd_resp_set_type(req, "text/html; charset=utf-8");
  String h = F("<!doctype html><html><head><meta charset='utf-8'>"
               "<meta name='viewport' content='width=device-width,initial-scale=1'><title>รูปในการ์ด SD</title><style>"
               "body{font-family:sans-serif;background:#0b1530;color:#fff;margin:0;padding:12px}"
               "a{color:#3cc8ff}.g{display:grid;grid-template-columns:repeat(auto-fill,minmax(150px,1fr));gap:10px}"
               ".c{background:#1c2b55;border-radius:10px;padding:6px;font-size:12px;word-break:break-all}"
               ".c img{width:100%;border-radius:6px}.d{color:#ff8080;float:right}</style></head><body>"
               "<p><a href='/'>&larr; กลับไปหน้ากล้อง</a></p><h2>รูปในการ์ด SD</h2>");
  httpd_resp_send_chunk(req, h.c_str(), h.length());
  if (!ready) {
    h = "<p>ไม่พบการ์ด SD</p>";
  } else {
    std::vector<String> v = listFiles();
    h = "<p>ทั้งหมด " + String(v.size()) + " รูป (แสดงล่าสุด 60 รูป)</p><div class='g'>";
    httpd_resp_send_chunk(req, h.c_str(), h.length());
    int shown = 0;
    for (int i = (int)v.size() - 1; i >= 0 && shown < 60; i--, shown++) {
      h = "<div class='c'><a href='/sd/file?n=" + v[i] + "'><img loading='lazy' src='/sd/file?n=" + v[i] + "'></a>" +
          v[i] + " <a class='d' href='/sd/del?n=" + v[i] + "' onclick=\"return confirm('ลบรูปนี้?')\">ลบ</a></div>";
      httpd_resp_send_chunk(req, h.c_str(), h.length());
    }
    h = "</div>";
  }
  h += "</body></html>";
  httpd_resp_send_chunk(req, h.c_str(), h.length());
  return httpd_resp_send_chunk(req, NULL, 0);
}

void registerHandlers(httpd_handle_t server) {
  httpd_uri_t uris[] = {
    {"/sd/snap",     HTTP_GET, snapHandler,     NULL},
    {"/sd/interval", HTTP_GET, intervalHandler, NULL},
    {"/sd/status",   HTTP_GET, statusHandler,   NULL},
    {"/sd/list",     HTTP_GET, listHandler,     NULL},
    {"/sd/file",     HTTP_GET, fileHandler,     NULL},
    {"/sd/del",      HTTP_GET, delHandler,      NULL},
  };
  for (auto& u : uris) httpd_register_uri_handler(server, &u);
}

// ปุ่มสำหรับใส่ในหน้าเว็บหลัก
const char* HTML_CONTROLS = R"HTML(
<div class="r">
  <button onclick="fetch('/sd/snap').then(r=>r.text()).then(t=>{document.getElementById('sd').innerText=t;setTimeout(sdst,1500)})">บันทึกลง SD</button>
  <select id="iv" onchange="fetch('/sd/interval?s='+this.value).then(sdst)">
    <option value="0">ถ่ายอัตโนมัติ: ปิด</option>
    <option value="10">ทุก 10 วินาที</option>
    <option value="60">ทุก 1 นาที</option>
    <option value="300">ทุก 5 นาที</option>
    <option value="1800">ทุก 30 นาที</option>
  </select>
  <button onclick="location.href='/sd/list'">ดูรูปใน SD</button>
</div>
<div class="s" id="sd"></div>
<script>
function sdst(){fetch('/sd/status').then(r=>r.text()).then(t=>{document.getElementById('sd').innerText=t;
  const m=t.match(/ทุก (\d+)/);document.getElementById('iv').value=m?m[1]:'0';});}
sdst();
</script>
)HTML";

}  // namespace sdcam
