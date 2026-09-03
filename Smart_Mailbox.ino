#include "BLEDevice.h"
#include "BLEServer.h"
#include "BLEUtils.h"
#include "BLE2902.h"

#include <WiFi.h>
#include <Preferences.h>
#include "esp_http_server.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

// ===================
// Select camera model
// ===================
// IMPORTANT: set this to YOUR board model that matches your camera_pins.h
// Example for XIAO ESP32S3 Sense: #define CAMERA_MODEL_XIAO_ESP32S3
#define CAMERA_MODEL_ESP32S3_EYE
#include "esp_camera.h"
#include "camera_pins.h"

// ============================
// GPIO
// ============================
#define LED_BUILTIN 2
#define RELAY_BUILTIN 41

// ============================
// Debug state logging
// ============================
// Set to 0 to silence the periodic state dump below.
#define ENABLE_STATE_LOG 1
#define STATE_LOG_INTERVAL_MS 5000

// ============================
// BLE UUIDs
// ============================
#define SERVICE_UUID "ab0828b1-198e-4351-b779-901fa0e0371e"
#define MESSAGE_UUID "4ac8a682-9736-4e5d-932b-e9b31405049c"

#define DEVINFO_UUID (uint16_t)0x180a
#define DEVINFO_MANUFACTURER_UUID (uint16_t)0x2a29
#define DEVINFO_NAME_UUID (uint16_t)0x2a24
#define DEVINFO_SERIAL_UUID (uint16_t)0x2a25
#define DEVICE_MANUFACTURER "Abbas"

// ============================
// Globals
// ============================
BLECharacteristic *pCharacteristic = nullptr;
bool deviceConnected = false;

SemaphoreHandle_t rxMutex = nullptr;
String rxload = "";
String Router_SSID = "";
String Router_Password = "";
bool wifiConnected = false;
bool wifiWanted = false;  // persisted: should we auto-connect on boot?

Preferences wifiPrefs;
bool ledState = false;
bool ledOn = false;
bool relayOn = false;

bool streamEnabled = false;
bool streamServerStarted = false;
bool cameraReady = false;

httpd_handle_t httpd = NULL;

// Stream constants
static const char *STREAM_CONTENT_TYPE = "multipart/x-mixed-replace;boundary=frame";
static const char *STREAM_BOUNDARY = "\r\n--frame\r\n";
static const char *STREAM_PART = "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n";

// ============================
// Helpers: notify + serial debug
// ============================
void bleNotifyAndPrint(const String &msg) {
  Serial.print("[BLE_NOTIFY] ");
  Serial.println(msg);
  if (pCharacteristic) {
    pCharacteristic->setValue(msg.c_str());
    pCharacteristic->notify();
  }
}

// ============================
// Parse helper (accepts "key=val", "key =val", "key= val", "key = val";
// space acts as an extra command separator alongside ';', so
// "led=on relay=on" and "led=on;relay=on" both work. Values therefore
// can't contain spaces themselves.)
// ============================
String trimSpaces(const String &s) {
  int start = 0, end = s.length();
  while (start < end && s[start] == ' ') start++;
  while (end > start && s[end - 1] == ' ') end--;
  return s.substring(start, end);
}

String getValue(const String &data, const String &key) {
  int searchFrom = 0;
  while (true) {
    int keyIdx = data.indexOf(key, searchFrom);
    if (keyIdx == -1) return "";
    searchFrom = keyIdx + 1;

    // key must start at the beginning of a "key=val" token: start of
    // string, or right after a ';' or space delimiter
    int before = keyIdx - 1;
    if (before >= 0 && data[before] != ';' && data[before] != ' ') continue;

    int eq = keyIdx + key.length();
    while (eq < (int)data.length() && data[eq] == ' ') eq++;
    if (eq >= (int)data.length() || data[eq] != '=') continue;

    int valStart = eq + 1;
    while (valStart < (int)data.length() && data[valStart] == ' ') valStart++;

    int valEnd = data.length();
    int semi = data.indexOf(';', valStart);
    if (semi != -1 && semi < valEnd) valEnd = semi;
    int spc = data.indexOf(' ', valStart);
    if (spc != -1 && spc < valEnd) valEnd = spc;

    return trimSpaces(data.substring(valStart, valEnd));
  }
}

// ============================
// BLE callbacks
// ============================
class MyServerCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer *pServer) override {
    deviceConnected = true;
    xSemaphoreTake(rxMutex, portMAX_DELAY);
    rxload = "";
    xSemaphoreGive(rxMutex);
    Serial.println("[DBG] BLE connected");
  }

  void onDisconnect(BLEServer *pServer) override {
    deviceConnected = false;
    Serial.println("[DBG] BLE disconnected");
    delay(100);
    pServer->getAdvertising()->start();
    Serial.println("[DBG] Advertising restarted");
  }
};

class MyCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic *pChar) override {
    String rxValue = pChar->getValue();
    if (rxValue.length() > 0) {
      xSemaphoreTake(rxMutex, portMAX_DELAY);
      rxload = rxValue;
      xSemaphoreGive(rxMutex);
      Serial.print("[BLE_RX] ");
      Serial.println(rxValue);
    }
  }
};

void setupBLE(const String &BLEName) {
  const char *ble_name = BLEName.c_str();

  BLEDevice::init(ble_name);
  BLEDevice::setMTU(185);  // allow room for stream_url/jpg_url notifications; still capped by what the central negotiates
  BLEServer *server = BLEDevice::createServer();
  server->setCallbacks(new MyServerCallbacks());

  BLEService *service = server->createService(SERVICE_UUID);
  pCharacteristic = service->createCharacteristic(
    MESSAGE_UUID,
    BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY | BLECharacteristic::PROPERTY_WRITE);

  pCharacteristic->setCallbacks(new MyCallbacks());
  pCharacteristic->addDescriptor(new BLE2902());
  service->start();

  // Device Info
  service = server->createService(DEVINFO_UUID);

  BLECharacteristic *c = service->createCharacteristic(DEVINFO_MANUFACTURER_UUID, BLECharacteristic::PROPERTY_READ);
  c->setValue(DEVICE_MANUFACTURER);

  c = service->createCharacteristic(DEVINFO_NAME_UUID, BLECharacteristic::PROPERTY_READ);
  c->setValue(ble_name);

  c = service->createCharacteristic(DEVINFO_SERIAL_UUID, BLECharacteristic::PROPERTY_READ);
  String chipId = String((uint32_t)(ESP.getEfuseMac() >> 24), HEX);
  c->setValue(chipId.c_str());

  service->start();

  // Advertising
  BLEAdvertising *adv = server->getAdvertising();
  BLEAdvertisementData advData;
  advData.setName(ble_name);
  advData.setCompleteServices(BLEUUID(SERVICE_UUID));
  adv->setAdvertisementData(advData);
  adv->start();

  Serial.println("[DBG] BLE Ready");
}

// ============================
// Camera init
// ============================
bool initCameraOnce() {
  if (cameraReady) return true;

  Serial.printf("[CAM] psramFound=%d psramSize=%u heap=%u\n",
                psramFound(), ESP.getPsramSize(), ESP.getFreeHeap());

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
  config.pin_sccb_sda = SIOD_GPIO_NUM;
  config.pin_sccb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn = PWDN_GPIO_NUM;
  config.pin_reset = RESET_GPIO_NUM;
  config.xclk_freq_hz = 20000000;

  config.frame_size = FRAMESIZE_SVGA;
  config.pixel_format = PIXFORMAT_JPEG;
  config.grab_mode = CAMERA_GRAB_WHEN_EMPTY;
  config.fb_location = psramFound() ? CAMERA_FB_IN_PSRAM : CAMERA_FB_IN_DRAM;
  config.jpeg_quality = 12;
  config.fb_count = psramFound() ? 2 : 1;

  if (!psramFound()) {
    config.frame_size = FRAMESIZE_QVGA;
    config.jpeg_quality = 15;
    config.fb_count = 1;
  }

  Serial.println("[CAM] esp_camera_init...");
  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("[CAM] init failed err=0x%x\n", (unsigned)err);
    cameraReady = false;
    return false;
  }

  sensor_t *s = esp_camera_sensor_get();
  if (s && s->id.PID == OV3660_PID) {
    s->set_vflip(s, 1);
    s->set_brightness(s, 1);
    s->set_saturation(s, -2);
  }
  if (s) s->set_framesize(s, FRAMESIZE_QVGA);

  cameraReady = true;
  Serial.println("[CAM] Ready");
  return true;
}

// ============================
// HTTP Handlers
// ============================
static esp_err_t jpg_handler(httpd_req_t *req) {
  if (!wifiConnected || !streamEnabled) {
    httpd_resp_set_status(req, "503 Service Unavailable");
    httpd_resp_send(req, "stream_off", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
  }

  if (!cameraReady && !initCameraOnce()) {
    httpd_resp_set_status(req, "500 Internal Server Error");
    httpd_resp_send(req, "camera_init_fail", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
  }

  camera_fb_t *fb = esp_camera_fb_get();
  if (!fb) {
    httpd_resp_set_status(req, "500 Internal Server Error");
    httpd_resp_send(req, "capture_fail", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
  }

  httpd_resp_set_type(req, "image/jpeg");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  esp_err_t res = httpd_resp_send(req, (const char *)fb->buf, fb->len);
  esp_camera_fb_return(fb);
  return res;
}

static esp_err_t stream_handler(httpd_req_t *req) {
  if (!wifiConnected || !streamEnabled) {
    httpd_resp_set_status(req, "503 Service Unavailable");
    httpd_resp_send(req, "stream_off", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
  }

  if (!cameraReady && !initCameraOnce()) {
    httpd_resp_set_status(req, "500 Internal Server Error");
    httpd_resp_send(req, "camera_init_fail", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
  }

  httpd_resp_set_type(req, STREAM_CONTENT_TYPE);
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

  Serial.println("[HTTP] client connected to /stream");

  while (true) {
    if (!wifiConnected || !streamEnabled) break;

    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) {
      Serial.println("[HTTP] Camera capture failed");
      break;
    }

    if (httpd_resp_send_chunk(req, STREAM_BOUNDARY, strlen(STREAM_BOUNDARY)) != ESP_OK) {
      esp_camera_fb_return(fb);
      break;
    }

    char part[96];
    int hlen = snprintf(part, sizeof(part), STREAM_PART, (unsigned)fb->len);
    if (httpd_resp_send_chunk(req, part, hlen) != ESP_OK) {
      esp_camera_fb_return(fb);
      break;
    }

    if (httpd_resp_send_chunk(req, (const char *)fb->buf, fb->len) != ESP_OK) {
      esp_camera_fb_return(fb);
      break;
    }

    esp_camera_fb_return(fb);
    vTaskDelay(1);
  }

  httpd_resp_send_chunk(req, NULL, 0);
  Serial.println("[HTTP] client left /stream");
  return ESP_OK;
}

void startCameraServer() {
  if (streamServerStarted) return;

  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.server_port = 80;
  config.ctrl_port = 32768;
  config.max_uri_handlers = 8;

  if (httpd_start(&httpd, &config) != ESP_OK) {
    Serial.println("[HTTP] server start failed");
    httpd = NULL;
    return;
  }

  httpd_uri_t uri_jpg = { .uri = "/jpg", .method = HTTP_GET, .handler = jpg_handler, .user_ctx = NULL };
  httpd_uri_t uri_stream = { .uri = "/stream", .method = HTTP_GET, .handler = stream_handler, .user_ctx = NULL };

  httpd_register_uri_handler(httpd, &uri_jpg);
  httpd_register_uri_handler(httpd, &uri_stream);

  streamServerStarted = true;
  Serial.println("[HTTP] server started (/jpg, /stream)");
}

// ============================
// WiFi connect helper
// ============================
bool connectWiFi(const String &ssid, const String &pass) {
  wifiConnected = false;

  WiFi.setAutoReconnect(false);
  WiFi.persistent(false);
  WiFi.setSleep(false);

  if (WiFi.getMode() != WIFI_STA) WiFi.mode(WIFI_STA);

  WiFi.disconnect(true, true);
  delay(150);

  Serial.print("[WIFI] begin: ");
  Serial.println(ssid);

  WiFi.begin(ssid.c_str(), pass.c_str());

  for (int i = 0; i < 20; i++) {
    if (WiFi.status() == WL_CONNECTED) {
      wifiConnected = true;
      Serial.print("[WIFI] connected, IP=");
      Serial.println(WiFi.localIP());
      digitalWrite(LED_BUILTIN, HIGH);
      return true;
    } else {
      ledState = !ledState;
      digitalWrite(LED_BUILTIN, ledState ? HIGH : LOW);
      delay(500);
      Serial.println("[WIFI] connecting...");
    }
  }

  Serial.println("[WIFI] connect timeout");
  WiFi.disconnect(true, false);
  digitalWrite(LED_BUILTIN, LOW);
  wifiConnected = false;
  return false;
}

// ============================
// Streaming control
// ============================
void startStreamingAndReport() {
  Serial.printf("[DBG] startStreamingAndReport: wifiConnected=%d streamEnabled(before)=%d\n",
                wifiConnected, streamEnabled);

  if (!wifiConnected) {
    bleNotifyAndPrint("err=wifi_off");
    return;
  }

  if (!cameraReady && !initCameraOnce()) {
    bleNotifyAndPrint("err=camera_init");
    return;
  }

  streamEnabled = true;

  if (!streamServerStarted) startCameraServer();

  String ip = WiFi.localIP().toString();
  String url = "stream_url=http://" + ip + "/stream";
  String jpg = "jpg_url=http://" + ip + "/jpg";

  Serial.print("[DBG] ");
  Serial.println(url);
  Serial.print("[DBG] ");
  Serial.println(jpg);

  bleNotifyAndPrint(url);
  bleNotifyAndPrint(jpg);
}

void stopStreamingAndReport() {
  streamEnabled = false;
  bleNotifyAndPrint("stream=0");
}

// ============================
// Debug state log
// ============================
#if ENABLE_STATE_LOG
void logState() {
  Serial.printf(
    "[STATE] uptime=%lus heap=%u ble=%d led=%d relay=%d wifi=%d ssid=\"%s\" ip=%s stream=%d streamServer=%d camera=%d\n",
    (unsigned long)(millis() / 1000), ESP.getFreeHeap(),
    deviceConnected, ledOn, relayOn,
    wifiConnected, Router_SSID.c_str(),
    wifiConnected ? WiFi.localIP().toString().c_str() : "-",
    streamEnabled, streamServerStarted, cameraReady);
}
#endif

// ============================
// Arduino setup/loop
// ============================
void setup() {
  pinMode(LED_BUILTIN, OUTPUT);
  pinMode(RELAY_BUILTIN, OUTPUT);
  digitalWrite(RELAY_BUILTIN, LOW);
  digitalWrite(LED_BUILTIN, LOW);

  Serial.begin(115200);
  delay(300);
  Serial.println();
  Serial.println("[DBG] Boot");

  rxMutex = xSemaphoreCreateMutex();

  wifiPrefs.begin("wifi", false);
  Router_SSID = wifiPrefs.getString("ssid", "");
  Router_Password = wifiPrefs.getString("pass", "");
  wifiWanted = wifiPrefs.getBool("wanted", false);

  setupBLE("Smart_Mailbox");
  Serial.println("[DBG] Bluetooth is ready!");

  if (Router_SSID.length() > 0 && wifiWanted) {
    Serial.print("[DBG] Auto-connecting with saved WiFi: ");
    Serial.println(Router_SSID);
    connectWiFi(Router_SSID, Router_Password);
  } else if (Router_SSID.length() > 0) {
    Serial.println("[DBG] Saved WiFi found but last state was disconnected - not auto-connecting");
  }
}

// ============================
// Command dispatch (shared by BLE writes and Serial input)
// ============================
void processCommand(const String &command) {
    Serial.print("[CMD] ");
    Serial.println(command);

    // LED
    String LED_State = getValue(command, "led");
    if (LED_State == "on") {
      ledOn = true;
      digitalWrite(LED_BUILTIN, HIGH);
      bleNotifyAndPrint("led=1");
    } else if (LED_State == "off") {
      ledOn = false;
      digitalWrite(LED_BUILTIN, LOW);
      bleNotifyAndPrint("led=0");
    } else if (LED_State == "status") {
      bleNotifyAndPrint(ledOn ? "led=1" : "led=0");
    }

    // Relay
    String Relay_State = getValue(command, "relay");
    if (Relay_State == "on") {
      relayOn = true;
      digitalWrite(RELAY_BUILTIN, HIGH);
      bleNotifyAndPrint("relay=1");
    } else if (Relay_State == "off") {
      relayOn = false;
      digitalWrite(RELAY_BUILTIN, LOW);
      bleNotifyAndPrint("relay=0");
    } else if (Relay_State == "status") {
      bleNotifyAndPrint(relayOn ? "relay=1" : "relay=0");
    }

    // WiFi creds
    String s = getValue(command, "router_ssid");
    if (s.length()) {
      Router_SSID = s;
      wifiPrefs.putString("ssid", Router_SSID);
      Serial.print("[DBG] router_ssid set (saved): ");
      Serial.println(Router_SSID);
      bleNotifyAndPrint("ok=router_ssid");
    }

    String p = getValue(command, "router_password");
    if (p.length()) {
      Router_Password = p;
      wifiPrefs.putString("pass", Router_Password);
      Serial.println("[DBG] router_password set (saved)");
      bleNotifyAndPrint("ok=router_password");
    }

    // WiFi control
    String Wifi_State = getValue(command, "wifi");
    if (Wifi_State == "on") {
      if (Router_SSID.length() == 0) {
        bleNotifyAndPrint("err=no_ssid");
      } else {
        wifiWanted = true;
        wifiPrefs.putBool("wanted", true);
        bool ok = connectWiFi(Router_SSID, Router_Password);
        bleNotifyAndPrint(ok ? "wifi=1" : "wifi=0");
      }
    } else if (Wifi_State == "off") {
      wifiWanted = false;
      wifiPrefs.putBool("wanted", false);
      WiFi.disconnect(true);
      wifiConnected = false;
      streamEnabled = false;
      digitalWrite(LED_BUILTIN, LOW);
      Serial.println("[WIFI] disconnected");
      bleNotifyAndPrint("wifi=0");
    } else if (Wifi_State == "status") {
      bleNotifyAndPrint(wifiConnected ? "wifi=1" : "wifi=0");
    } else if (Wifi_State == "scan") {
      Serial.println("[WIFI] scanning...");
      if (WiFi.getMode() != WIFI_STA) WiFi.mode(WIFI_STA);
      int n = WiFi.scanNetworks();
      if (n < 0) {
        bleNotifyAndPrint("err=wifi_scan_failed");
      } else {
        for (int i = 0; i < n; i++) {
          bleNotifyAndPrint("wifi_scan=" + WiFi.SSID(i));
          delay(20);  // give the BLE notify queue time to drain
        }
        bleNotifyAndPrint("wifi_scan_done=" + String(n));
      }
      WiFi.scanDelete();
    }

    // Stream control
    String Stream_State = getValue(command, "stream");
    if (Stream_State == "on") {
      startStreamingAndReport();
    } else if (Stream_State == "off") {
      stopStreamingAndReport();
    } else if (Stream_State == "status") {
      bleNotifyAndPrint(streamEnabled ? "stream=1" : "stream=0");
    }
}

void loop() {
  static uint32_t lastTick = 0;
  uint32_t now = millis();

#if ENABLE_STATE_LOG
  static uint32_t lastStateLog = 0;
  if (now - lastStateLog >= STATE_LOG_INTERVAL_MS) {
    lastStateLog = now;
    logState();
  }
#endif

  // Serial commands: accumulate a line, dispatch on newline. Handles any
  // Serial Monitor line-ending setting (CR, LF, or CRLF).
  static String serialLine;
  while (Serial.available() > 0) {
    char c = (char)Serial.read();
    if (c == '\n' || c == '\r') {
      if (serialLine.length() > 0) {
        processCommand(serialLine);
        serialLine = "";
      }
    } else {
      serialLine += c;
      if (serialLine.length() > 200) serialLine = "";  // guard against runaway input
    }
  }

  if (now - lastTick < 50) return;
  lastTick = now;

  // BLE commands
  String bleCommand;
  if (deviceConnected) {
    xSemaphoreTake(rxMutex, portMAX_DELAY);
    bleCommand = rxload;
    rxload = "";
    xSemaphoreGive(rxMutex);
  }

  if (bleCommand.length() > 0) {
    processCommand(bleCommand);
  }
}
