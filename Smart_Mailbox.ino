// ============================================================================
// Smart_Mailbox.ino
// ----------------------------------------------------------------------------
// Firmware for an ESP32-S3 based "smart mailbox": a BLE-controlled box with
// an LED, a relay (driving the mailbox lock/latch), WiFi provisioning, and a
// camera that can stream live video over HTTP once WiFi is up.
//
// Control protocol
// -----------------
// The board is controlled by sending short text commands of the form
// "key=value" (multiple can be chained with ';' or a space, e.g.
// "led=on;relay=off" or "led=on relay=off"). Commands can arrive three
// ways, all funneled into the same processCommand() dispatcher:
//   1. BLE: write the command string to the MESSAGE_UUID characteristic.
//      Replies/events come back as BLE notifications on that same
//      characteristic (see bleNotifyAndPrint()). This is also the only
//      path that works before the board has joined a WiFi network, so
//      initial provisioning (router_ssid/router_password/wifi=on) has to
//      happen here.
//   2. Serial (USB): type a command into the Serial Monitor and press
//      Enter. Useful for testing without a phone/BLE client.
//   3. HTTP, once WiFi is up: GET /cmd?c=<url-encoded command>&t=<auth
//      token> runs the same command and replies with the same text a BLE
//      notify would have carried (see cmd_handler()). A client learns the
//      board's IP from the "ip=..." BLE notification sent on a successful
//      wifi=on or wifi=status, and can then drive led=/relay=/stream=/etc.
//      entirely over WiFi without staying connected over BLE. Every HTTP
//      endpoint (/cmd, /jpg, /stream) requires that token - see "Security"
//      below.
//
// Values containing '=', ';', or a space (most importantly WiFi SSIDs and
// passwords) must be percent-encoded by the sender - see urlDecode() and
// its use on router_ssid/router_password in processCommand(). The command
// parser itself treats a raw '=', ';', or ' ' as structural, so an
// unencoded one inside a value would be misread as a delimiter.
//
// Supported keys: led, relay, router_ssid, router_password, wifi, stream,
// auth. See processCommand() for the full set of accepted values per key
// and the notifications each one produces.
//
// WiFi credentials (router_ssid/router_password) and the "should we be
// connected" intent are persisted to flash (NVS, via the Preferences
// library) so the board can reconnect automatically after a reboot or power
// loss - see the wifiWanted flag and its use in setup()/processCommand().
//
// Security
// --------
// BLE has no pairing/bonding/encryption configured - anyone in range who
// knows the service/characteristic UUIDs (published in this repo) can
// connect and issue commands. That's a real, currently-unaddressed gap;
// what IS addressed is the HTTP surface: every HTTP request must include
// a per-device secret token (?t=...) that is generated on first boot and
// obtainable ONLY over BLE/Serial (see "auth=status"/"auth=new" in
// processCommand() and checkHttpAuth()), never over HTTP itself. So
// merely being on the same WiFi network is no longer enough to drive the
// relay or view the camera - an attacker still needs BLE proximity first.
//
// Robustness
// ----------
// wifi=on and wifi=scan are both non-blocking: they kick off the
// operation and return immediately (reporting "wifi=connecting" for
// wifi=on), with the real result following later via a BLE/Serial
// notification once pollWifiConnect()/pollWifiScan() (called every
// loop() iteration) see it complete. This keeps the BLE callback, Serial
// input, and other HTTP requests responsive instead of being blocked for
// the up-to-10-second WiFi connect timeout or the several seconds a scan
// can take.
// ============================================================================

#include "BLEDevice.h"
#include "BLEServer.h"
#include "BLEUtils.h"
#include "BLE2902.h"

#include <WiFi.h>
#include <Preferences.h>
#include "esp_http_server.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_random.h"

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
#define LED_BUILTIN 2     // status/command LED
#define RELAY_BUILTIN 41  // drives the mailbox lock/latch relay

// ============================
// Debug state logging
// ============================
// Set to 0 to silence the periodic state dump below.
#define ENABLE_STATE_LOG 1
#define STATE_LOG_INTERVAL_MS 5000

// ============================
// BLE UUIDs
// ============================
// SERVICE_UUID / MESSAGE_UUID identify the custom "command + notify"
// characteristic that carries the whole control protocol described above.
#define SERVICE_UUID "ab0828b1-198e-4351-b779-901fa0e0371e"
#define MESSAGE_UUID "4ac8a682-9736-4e5d-932b-e9b31405049c"

// Standard BLE Device Information service/characteristic UUIDs, exposed
// read-only so a BLE client can identify the board (manufacturer, name,
// and a serial derived from the chip's own MAC/efuse ID).
#define DEVINFO_UUID (uint16_t)0x180a
#define DEVINFO_MANUFACTURER_UUID (uint16_t)0x2a29
#define DEVINFO_NAME_UUID (uint16_t)0x2a24
#define DEVINFO_SERIAL_UUID (uint16_t)0x2a25
#define DEVICE_MANUFACTURER "Abbas"

// ============================
// Globals
// ============================
BLECharacteristic *pCharacteristic = nullptr;  // the command/notify characteristic, set up in setupBLE()
bool deviceConnected = false;                  // true while a BLE central (phone/app) is connected

// rxload is the raw command string most recently written by a BLE client.
// It's written from the BLE stack's own task (inside MyCallbacks::onWrite)
// and read/cleared from loop() (the main Arduino task) - two different
// FreeRTOS tasks touching the same Arduino String, so every access to
// rxload must be wrapped in rxMutex to avoid racing on its heap buffer.
SemaphoreHandle_t rxMutex = nullptr;
String rxload = "";

// WiFi credentials, persisted to flash (see wifiPrefs) so the board can
// reconnect automatically after a reboot without needing to be
// reprovisioned over BLE every time.
String Router_SSID = "";
String Router_Password = "";
bool wifiConnected = false;      // true once a connect attempt has actually succeeded
bool wifiWanted = false;         // persisted: should we auto-connect on boot?

Preferences wifiPrefs;  // NVS namespace "wifi": stores ssid, pass, wanted flag, and the auth token

// Per-device secret required for every HTTP request (see checkHttpAuth()).
// Generated once on first boot and persisted; only ever revealed over
// BLE/Serial ("auth=status"/"auth=new" in processCommand()), never HTTP.
String authToken = "";

// Non-blocking WiFi connect: startConnectWiFi() kicks off WiFi.begin() and
// returns immediately; pollWifiConnect() (called every loop() iteration)
// checks progress and sends the eventual wifi=1/wifi=0 (+ip=) reply once
// it resolves, without blocking whichever command triggered it.
bool wifiConnectPending = false;
uint32_t wifiConnectStartMs = 0;
uint32_t wifiConnectLastBlinkMs = 0;
static const uint32_t WIFI_CONNECT_TIMEOUT_MS = 10000;

// Non-blocking WiFi scan: same idea, using WiFi.scanNetworks(true) (async
// mode) and polling WiFi.scanComplete() from pollWifiScan().
bool wifiScanPending = false;

bool ledState = false;  // used only to alternate the LED while pollWifiConnect() is retrying
bool ledOn = false;      // last commanded LED state (led=on/off), used for led=status and the debug log
bool relayOn = false;    // last commanded relay state (relay=on/off), used for relay=status and the debug log

bool streamEnabled = false;       // true once stream=on has succeeded; gates the HTTP handlers
bool streamServerStarted = false; // true once the HTTP server (httpd) has been started (only done once)
bool cameraReady = false;         // true once the camera driver has been initialized (only done once)

// Two separate HTTP server instances - see startCameraServer() for why:
// /stream's handler blocks in an infinite loop for as long as a client
// stays connected, which would otherwise starve every other request on
// the same server (ESP-IDF's httpd services one request at a time).
httpd_handle_t httpd = NULL;        // control server on port 80: /jpg, /cmd
httpd_handle_t streamHttpd = NULL;  // dedicated streaming server on port 81: /stream only

// When true, every reply that would normally only go to BLE/Serial is also
// appended to httpReplyBuffer instead of being sent to those. Set around a
// processCommand() call triggered by an HTTP /cmd request (see
// cmd_handler()) so that request's HTTP response can echo back exactly
// what a BLE client would have received - see bleNotifyAndPrint().
bool captureForHttp = false;
String httpReplyBuffer = "";

// Stream constants: the exact byte sequences the MJPEG multipart HTTP
// response is built from. Each frame is sent as:
//   STREAM_BOUNDARY + (STREAM_PART with the real length) + <JPEG bytes>
static const char *STREAM_CONTENT_TYPE = "multipart/x-mixed-replace;boundary=frame";
static const char *STREAM_BOUNDARY = "\r\n--frame\r\n";
static const char *STREAM_PART = "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n";

// ============================
// Helpers: notify + serial debug
// ============================
// Sends `msg` back to whichever BLE client is subscribed (as a
// characteristic notify) and always echoes it to the Serial console too,
// so the same function works as the single "reply" mechanism regardless of
// whether the triggering command came in over BLE or Serial. When a
// command is being dispatched on behalf of an HTTP /cmd request instead
// (captureForHttp == true), the message is also collected into
// httpReplyBuffer so cmd_handler() can send it back as that request's HTTP
// response body - this is what lets led=/relay=/stream= (and everything
// else processCommand() understands) be driven over plain HTTP, reusing
// the exact same command parsing and reply text as BLE/Serial.
void bleNotifyAndPrint(const String &msg) {
  Serial.print("[BLE_NOTIFY] ");
  Serial.println(msg);
  if (pCharacteristic) {
    pCharacteristic->setValue(msg.c_str());
    pCharacteristic->notify();
  }
  if (captureForHttp) {
    httpReplyBuffer += msg;
    httpReplyBuffer += "\n";
  }
}

// ============================
// Parse helper (accepts "key=val", "key =val", "key= val", "key = val";
// space acts as an extra command separator alongside ';', so
// "led=on relay=on" and "led=on;relay=on" both work. Values therefore
// can't contain spaces themselves.)
// ============================

// Strips leading/trailing spaces from a String (Arduino's String class has
// no built-in trim() that returns a copy, so this is a small local helper).
String trimSpaces(const String &s) {
  int start = 0, end = s.length();
  while (start < end && s[start] == ' ') start++;
  while (end > start && s[end - 1] == ' ') end--;
  return s.substring(start, end);
}

// Extracts the value for `key` out of a "key1=val1;key2=val2 key3=val3"
// style command string, or "" if the key isn't present. Handles optional
// whitespace around '=' and treats both ';' and ' ' as separators between
// key=value pairs. Also verifies the key match is a real token boundary
// (start of string, or right after a ';'/' ') so a key can't accidentally
// match as a substring inside a longer key or value (e.g. searching for
// "ssid" wouldn't wrongly match inside "router_ssid").
String getValue(const String &data, const String &key) {
  int searchFrom = 0;
  while (true) {
    int keyIdx = data.indexOf(key, searchFrom);
    if (keyIdx == -1) return "";  // key not present anywhere in the string
    searchFrom = keyIdx + 1;      // if this occurrence turns out invalid, keep scanning past it

    // key must start at the beginning of a "key=val" token: start of
    // string, or right after a ';' or space delimiter
    int before = keyIdx - 1;
    if (before >= 0 && data[before] != ';' && data[before] != ' ') continue;

    // Skip optional spaces between the key name and its '=' (handles
    // "key = val" as well as "key=val"). If there's no '=' where expected,
    // this wasn't a real match (e.g. key was a substring of a longer
    // identifier) - keep scanning.
    int eq = keyIdx + key.length();
    while (eq < (int)data.length() && data[eq] == ' ') eq++;
    if (eq >= (int)data.length() || data[eq] != '=') continue;

    // Skip optional spaces right after '=', then the value runs until the
    // next ';' or ' ' (or end of string), whichever comes first.
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

// Decodes a application/x-www-form-urlencoded string (the format used for
// HTTP query-string values): '+' becomes a space, and "%XX" becomes the
// byte 0xXX. Used to recover the original command text from the "c" query
// parameter of an HTTP GET /cmd?c=... request (see cmd_handler()), since
// that command text itself uses '=', ';', and spaces which must be
// percent-encoded by the client to survive as a single query value.
String urlDecode(const String &s) {
  String out;
  out.reserve(s.length());
  for (int i = 0; i < (int)s.length(); i++) {
    char c = s[i];
    if (c == '+') {
      out += ' ';
    } else if (c == '%' && i + 2 < (int)s.length()) {
      char hex[3] = { s[i + 1], s[i + 2], 0 };
      out += (char)strtol(hex, nullptr, 16);
      i += 2;
    } else {
      out += c;
    }
  }
  return out;
}

// Generates a fresh 32-character hex auth token using the hardware RNG
// (esp_random(), true entropy on ESP32/ESP32-S3, not a seeded PRNG) - see
// authToken above. Called once on first boot, and again on "auth=new" to
// rotate it (invalidating any previously-shared token).
String generateAuthToken() {
  static const char *hexChars = "0123456789abcdef";
  String token;
  token.reserve(32);
  for (int i = 0; i < 32; i++) {
    token += hexChars[esp_random() % 16];
  }
  return token;
}

// ============================
// BLE callbacks
// ============================
// Runs on the BLE/NimBLE stack's own FreeRTOS task, not the main loop()
// task - keep these handlers quick and be careful about shared state
// (see the rxMutex comment above rxload).
class MyServerCallbacks : public BLEServerCallbacks {
  // Called when a central (phone/app) completes a BLE connection.
  void onConnect(BLEServer *pServer) override {
    deviceConnected = true;
    // Clear out any stale command left over from a previous session so a
    // fresh connection doesn't accidentally replay an old write.
    xSemaphoreTake(rxMutex, portMAX_DELAY);
    rxload = "";
    xSemaphoreGive(rxMutex);
    Serial.println("[DBG] BLE connected");
  }

  // Called when the central disconnects. Immediately restarts advertising
  // so a new client (or the same one reconnecting) can find the board
  // again without needing a physical reset.
  void onDisconnect(BLEServer *pServer) override {
    deviceConnected = false;
    Serial.println("[DBG] BLE disconnected");
    delay(100);
    pServer->getAdvertising()->start();
    Serial.println("[DBG] Advertising restarted");
  }
};

class MyCallbacks : public BLECharacteristicCallbacks {
  // Called whenever a BLE client writes to the command characteristic.
  // Just stashes the raw command string into rxload (mutex-protected);
  // the actual parsing/dispatch happens later on the main loop() task via
  // processCommand(), keeping this callback itself fast.
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

// Brings up the whole BLE GATT server: the custom command/notify
// characteristic, the standard Device Info service, and advertising.
// Called once from setup().
void setupBLE(const String &BLEName) {
  const char *ble_name = BLEName.c_str();

  BLEDevice::init(ble_name);
  BLEDevice::setMTU(185);  // allow room for stream_url/jpg_url notifications; still capped by what the central negotiates
  BLEServer *server = BLEDevice::createServer();
  server->setCallbacks(new MyServerCallbacks());

  // Custom command service: one characteristic used for both writing
  // commands (from the client) and receiving notifications (replies/events
  // sent back by the board).
  BLEService *service = server->createService(SERVICE_UUID);
  pCharacteristic = service->createCharacteristic(
    MESSAGE_UUID,
    BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY | BLECharacteristic::PROPERTY_WRITE);

  pCharacteristic->setCallbacks(new MyCallbacks());
  pCharacteristic->addDescriptor(new BLE2902());  // required for the client to be able to enable notifications
  service->start();

  // Device Info service: standard, read-only identification fields.
  service = server->createService(DEVINFO_UUID);

  BLECharacteristic *c = service->createCharacteristic(DEVINFO_MANUFACTURER_UUID, BLECharacteristic::PROPERTY_READ);
  c->setValue(DEVICE_MANUFACTURER);

  c = service->createCharacteristic(DEVINFO_NAME_UUID, BLECharacteristic::PROPERTY_READ);
  c->setValue(ble_name);

  c = service->createCharacteristic(DEVINFO_SERIAL_UUID, BLECharacteristic::PROPERTY_READ);
  String chipId = String((uint32_t)(ESP.getEfuseMac() >> 24), HEX);
  c->setValue(chipId.c_str());

  service->start();

  // Advertising: broadcast the board's name and the custom service UUID so
  // a scanning client (phone app, nRF Connect, our own ble_console.py/
  // mailbox_gui.py) can find and identify it.
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
// Configures and starts the onboard camera driver. Only actually does the
// (relatively slow) init work once - cameraReady short-circuits repeat
// calls. Called lazily, the first time it's actually needed (either by
// stream=on or by an incoming HTTP request), rather than unconditionally
// at boot.
bool initCameraOnce() {
  if (cameraReady) return true;

  Serial.printf("[CAM] psramFound=%d psramSize=%u heap=%u\n",
                psramFound(), ESP.getPsramSize(), ESP.getFreeHeap());

  camera_config_t config;
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer = LEDC_TIMER_0;
  // Pin assignments come from camera_pins.h, selected by the
  // CAMERA_MODEL_* #define above.
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
  // With PSRAM available we can afford bigger frames and double-buffering;
  // without it, fall back to a smaller frame size and a single buffer so
  // it still fits in regular DRAM.
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

  // OV3660-specific tweaks some of these camera modules need to look right
  // (mirrored image / exposure) out of the box.
  sensor_t *s = esp_camera_sensor_get();
  if (s && s->id.PID == OV3660_PID) {
    s->set_vflip(s, 1);
    s->set_brightness(s, 1);
    s->set_saturation(s, -2);
  }
  // Actual streaming frame size (overrides config.frame_size above, which
  // only affected the initial buffer allocation).
  if (s) s->set_framesize(s, FRAMESIZE_QVGA);

  cameraReady = true;
  Serial.println("[CAM] Ready");
  return true;
}

// ============================
// HTTP Handlers
// ============================
// Checks the "t" query parameter of an HTTP request against authToken.
// Applied to every HTTP endpoint (cmd_handler, jpg_handler, stream_handler)
// as the first thing they do, before touching WiFi/camera/relay state at
// all - without this, anyone on the same WiFi network (no BLE proximity
// needed) could drive the relay or watch the camera with zero credentials.
bool checkHttpAuth(httpd_req_t *req) {
  if (authToken.length() == 0) return false;  // fail closed if somehow not yet generated
  size_t qlen = httpd_req_get_url_query_len(req);
  if (qlen == 0 || qlen >= 300) return false;
  char query[300];
  if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) return false;
  char value[40];
  if (httpd_query_key_value(query, "t", value, sizeof(value)) != ESP_OK) return false;
  return authToken == value;
}

void sendHttpUnauthorized(httpd_req_t *req) {
  httpd_resp_set_status(req, "401 Unauthorized");
  httpd_resp_set_type(req, "text/plain");
  httpd_resp_send(req, "err=unauthorized", HTTPD_RESP_USE_STRLEN);
}

// GET /cmd?c=<url-encoded command> - runs any command processCommand()
// understands (led=on, relay=off, stream=on, wifi=status, etc.) over
// plain WiFi instead of BLE/Serial, and replies with exactly the same
// text a BLE client would have received as notifications, one per line.
// This lets a WiFi-connected client (like the Python GUI, once it knows
// the board's IP from an earlier "ip=" BLE notification) control the
// board without needing to stay connected over BLE.
static esp_err_t cmd_handler(httpd_req_t *req) {
  if (!checkHttpAuth(req)) {
    sendHttpUnauthorized(req);
    return ESP_OK;
  }

  String cmd = "";
  size_t qlen = httpd_req_get_url_query_len(req);
  if (qlen > 0 && qlen < 256) {
    char query[256];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
      char value[220];
      if (httpd_query_key_value(query, "c", value, sizeof(value)) == ESP_OK) {
        cmd = urlDecode(String(value));
      }
    }
  }

  httpd_resp_set_type(req, "text/plain");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

  if (cmd.length() == 0) {
    httpd_resp_set_status(req, "400 Bad Request");
    httpd_resp_send(req, "err=no_command", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
  }

  // Route this command's replies into httpReplyBuffer instead of (only)
  // BLE/Serial, then send whatever it collected back as the HTTP body.
  captureForHttp = true;
  httpReplyBuffer = "";
  processCommand(cmd);
  captureForHttp = false;

  if (httpReplyBuffer.length() == 0) {
    httpd_resp_send(req, "ok", HTTPD_RESP_USE_STRLEN);
  } else {
    httpd_resp_send(req, httpReplyBuffer.c_str(), HTTPD_RESP_USE_STRLEN);
  }
  return ESP_OK;
}

// GET /jpg - a single JPEG snapshot from the camera. Requires WiFi to be
// connected and stream=on to have been requested at least once
// (streamEnabled); the camera itself is (re)initialized on demand here if
// it isn't ready yet.
static esp_err_t jpg_handler(httpd_req_t *req) {
  if (!checkHttpAuth(req)) {
    sendHttpUnauthorized(req);
    return ESP_OK;
  }

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
  esp_camera_fb_return(fb);  // must always be returned to the driver, success or failure
  return res;
}

// GET /stream - an MJPEG multipart stream: repeatedly grabs a frame from
// the camera and writes it as one multipart chunk, for as long as the
// client stays connected and streamEnabled/wifiConnected remain true. This
// is what a browser (or the Python GUI's MJPEGReader) opens directly to
// display live video.
static esp_err_t stream_handler(httpd_req_t *req) {
  if (!checkHttpAuth(req)) {
    sendHttpUnauthorized(req);
    return ESP_OK;
  }

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
    // Bail out promptly if streaming got turned off or WiFi dropped while
    // we were mid-loop.
    if (!wifiConnected || !streamEnabled) break;

    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) {
      Serial.println("[HTTP] Camera capture failed");
      break;
    }

    // Each frame: boundary marker, then a small text header carrying the
    // real Content-Length, then the raw JPEG bytes themselves. Any failed
    // chunk write means the client went away - stop streaming to it.
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
    vTaskDelay(1);  // yield briefly so other tasks (BLE, WiFi) get CPU time between frames
  }

  httpd_resp_send_chunk(req, NULL, 0);  // terminate the chunked response
  Serial.println("[HTTP] client left /stream");
  return ESP_OK;
}

// Starts two ESP-IDF HTTP server instances and registers the handlers
// above. Only runs once (streamServerStarted guards repeat calls) -
// called as soon as WiFi connects (see pollWifiConnect()), not only when
// streaming starts, since /cmd needs to be reachable over WiFi even if
// the camera stream itself is never turned on. Both servers stay up for
// the rest of the board's uptime once started, even if streaming is
// later turned off (the /jpg and /stream handlers themselves check
// streamEnabled and just reply 503 while it's off).
//
// /stream is deliberately on its own server/port (81) rather than
// sharing the control server (80) with /jpg and /cmd: ESP-IDF's httpd
// services one request at a time per server instance, and stream_handler
// blocks in an infinite loop for as long as a client stays connected. If
// it shared a server with /cmd, an open video stream would make every
// LED/relay/status command time out until the stream stopped - exactly
// the read-timeout behavior this split avoids. (This mirrors how
// Espressif's own CameraWebServer example is structured.)
void startCameraServer() {
  if (streamServerStarted) return;

  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.server_port = 80;
  config.ctrl_port = 32768;
  config.max_uri_handlers = 4;
  // Being on a separate server instance from /stream isn't enough by
  // itself - both server tasks can still be scheduled onto the same CPU
  // core and contend for it while a stream is actively pushing frames
  // over WiFi. Pin the control server to core 1 (the same core the
  // Arduino loop()/BLE stack run on) and give it a slightly higher
  // priority than the stream server's default, so /cmd stays responsive
  // even under streaming load.
  config.core_id = 1;
  config.task_priority = tskIDLE_PRIORITY + 6;

  if (httpd_start(&httpd, &config) == ESP_OK) {
    httpd_uri_t uri_jpg = { .uri = "/jpg", .method = HTTP_GET, .handler = jpg_handler, .user_ctx = NULL };
    httpd_uri_t uri_cmd = { .uri = "/cmd", .method = HTTP_GET, .handler = cmd_handler, .user_ctx = NULL };
    httpd_register_uri_handler(httpd, &uri_jpg);
    httpd_register_uri_handler(httpd, &uri_cmd);
  } else {
    Serial.println("[HTTP] control server start failed");
    httpd = NULL;
  }

  httpd_config_t stream_config = HTTPD_DEFAULT_CONFIG();
  stream_config.server_port = 81;
  stream_config.ctrl_port = 32769;  // must differ from the control server's ctrl_port above
  stream_config.max_uri_handlers = 2;
  stream_config.core_id = 0;  // keep the heavy, continuous streaming loop off the control server's core
  // task_priority left at its default (tskIDLE_PRIORITY+5) - intentionally
  // lower than the control server's, above, so streaming yields to control
  // commands when the two would otherwise compete for the same core.

  if (httpd_start(&streamHttpd, &stream_config) == ESP_OK) {
    httpd_uri_t uri_stream = { .uri = "/stream", .method = HTTP_GET, .handler = stream_handler, .user_ctx = NULL };
    httpd_register_uri_handler(streamHttpd, &uri_stream);
  } else {
    Serial.println("[HTTP] stream server start failed");
    streamHttpd = NULL;
  }

  streamServerStarted = true;
  Serial.println("[HTTP] servers started (:80 /jpg /cmd, :81 /stream)");
}

// ============================
// WiFi connect helper (non-blocking)
// ============================
// Kicks off joining `ssid`/`pass` in station mode and returns immediately -
// does NOT wait for the result. pollWifiConnect(), called every loop()
// iteration, tracks progress (blinking the LED) and reports the outcome
// once WiFi.status() resolves or WIFI_CONNECT_TIMEOUT_MS elapses. Used
// both for the wifi=on command and for the auto-connect attempt at boot,
// neither of which should block the caller for up to 10 seconds.
void startConnectWiFi(const String &ssid, const String &pass) {
  wifiConnected = false;

  WiFi.setAutoReconnect(false);  // we manage (re)connect attempts ourselves via the wifi= command
  WiFi.persistent(false);        // don't let the WiFi driver itself write creds to flash; we do that via Preferences
  WiFi.setSleep(false);          // keep the radio fully awake for lower/steadier latency

  if (WiFi.getMode() != WIFI_STA) WiFi.mode(WIFI_STA);

  WiFi.disconnect(true, true);  // clear any previous connection/config before starting a fresh attempt

  Serial.print("[WIFI] begin: ");
  Serial.println(ssid);

  WiFi.begin(ssid.c_str(), pass.c_str());

  wifiConnectPending = true;
  wifiConnectStartMs = millis();
  wifiConnectLastBlinkMs = 0;
}

// Call once per loop() iteration. No-op unless a startConnectWiFi() is
// currently in progress.
void pollWifiConnect() {
  if (!wifiConnectPending) return;

  if (WiFi.status() == WL_CONNECTED) {
    wifiConnectPending = false;
    wifiConnected = true;
    Serial.print("[WIFI] connected, IP=");
    Serial.println(WiFi.localIP());
    digitalWrite(LED_BUILTIN, HIGH);  // solid LED = connected
    // Bring up the HTTP server (/jpg, /stream, /cmd) as soon as we have
    // an IP, not only when streaming is first requested - /cmd needs to
    // be reachable over WiFi for led=/relay=/stream= control even if
    // the camera stream itself is never turned on.
    if (!streamServerStarted) startCameraServer();
    bleNotifyAndPrint("wifi=1");
    // Report our IP so a client can start talking to /cmd, /jpg, and
    // /stream directly over WiFi instead of needing to stay on BLE.
    bleNotifyAndPrint("ip=" + WiFi.localIP().toString());
    return;
  }

  uint32_t now = millis();
  if (now - wifiConnectStartMs >= WIFI_CONNECT_TIMEOUT_MS) {
    wifiConnectPending = false;
    Serial.println("[WIFI] connect timeout");
    WiFi.disconnect(true, false);
    digitalWrite(LED_BUILTIN, LOW);
    wifiConnected = false;
    bleNotifyAndPrint("wifi=0");
    return;
  }

  if (now - wifiConnectLastBlinkMs >= 500) {  // blink roughly twice a second while connecting
    wifiConnectLastBlinkMs = now;
    ledState = !ledState;
    digitalWrite(LED_BUILTIN, ledState ? HIGH : LOW);
    Serial.println("[WIFI] connecting...");
  }
}

// Call once per loop() iteration. No-op unless a wifi=scan (see
// processCommand()) is currently in progress. Once WiFi.scanComplete()
// stops returning WIFI_SCAN_RUNNING, reports the results (or failure) and
// frees them via WiFi.scanDelete().
void pollWifiScan() {
  if (!wifiScanPending) return;

  int n = WiFi.scanComplete();
  if (n == WIFI_SCAN_RUNNING) return;  // still in progress

  wifiScanPending = false;
  if (n < 0) {
    bleNotifyAndPrint("err=wifi_scan_failed");
  } else {
    for (int i = 0; i < n; i++) {
      bleNotifyAndPrint("wifi_scan=" + WiFi.SSID(i));
      delay(20);  // give the BLE notify queue time to drain
    }
    bleNotifyAndPrint("wifi_scan_done=" + String(n));
  }
  WiFi.scanDelete();  // free the scan result memory
}

// ============================
// Streaming control
// ============================
// Handles "stream=on": makes sure WiFi is up and the camera is
// initialized, starts the HTTP server if it hasn't been started yet, and
// notifies the client with the actual stream_url/jpg_url to open (built
// from the board's current IP).
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

  // Only mark streaming enabled once we know both WiFi and the camera are
  // actually ready - otherwise a status query could report stream=1 when
  // nothing is really working.
  streamEnabled = true;

  if (!streamServerStarted) startCameraServer();

  String ip = WiFi.localIP().toString();
  // ?t=<authToken> - required by checkHttpAuth() on every HTTP endpoint;
  // embedding it here means the client never has to add it itself for
  // these two URLs (only for its own /cmd requests - see cmd_handler()).
  String url = "stream_url=http://" + ip + ":81/stream?t=" + authToken;  // dedicated stream server - see startCameraServer()
  String jpg = "jpg_url=http://" + ip + "/jpg?t=" + authToken;

  Serial.print("[DBG] ");
  Serial.println(url);
  Serial.print("[DBG] ");
  Serial.println(jpg);

  bleNotifyAndPrint(url);
  bleNotifyAndPrint(jpg);
}

// Handles "stream=off": just flips the flag off (the HTTP handlers check
// it on every request/loop iteration and will stop serving frames), and
// tells the client it's stopped. The camera driver and HTTP server itself
// are left running/initialized so a later stream=on is instant.
void stopStreamingAndReport() {
  streamEnabled = false;
  bleNotifyAndPrint("stream=0");
}

// ============================
// Debug state log
// ============================
// Periodic one-line snapshot of the board's whole state, printed to
// Serial only (not sent over BLE) - purely a debugging aid. Controlled by
// ENABLE_STATE_LOG / STATE_LOG_INTERVAL_MS near the top of the file.
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
  digitalWrite(RELAY_BUILTIN, LOW);  // relay/lock starts de-energized
  digitalWrite(LED_BUILTIN, LOW);

  Serial.begin(115200);
  delay(300);
  Serial.println();
  Serial.println("[DBG] Boot");

  rxMutex = xSemaphoreCreateMutex();

  // Load persisted WiFi credentials + connect intent from flash (NVS).
  // wifiWanted reflects whatever the last wifi=on/wifi=off command set it
  // to, so a reboot only reconnects automatically if the board was meant
  // to be online when it last went down.
  wifiPrefs.begin("wifi", false);
  Router_SSID = wifiPrefs.getString("ssid", "");
  Router_Password = wifiPrefs.getString("pass", "");
  wifiWanted = wifiPrefs.getBool("wanted", false);

  // Load the HTTP auth token, generating and persisting one on first boot
  // ever (or after a "auth=new" rotation cleared it - it never does, but
  // this is also the recovery path if NVS were ever erased).
  authToken = wifiPrefs.getString("authtok", "");
  if (authToken.length() == 0) {
    authToken = generateAuthToken();
    wifiPrefs.putString("authtok", authToken);
    Serial.println("[DBG] Generated new HTTP auth token");
  }

  setupBLE("Smart_Mailbox");
  Serial.println("[DBG] Bluetooth is ready!");

  if (Router_SSID.length() > 0 && wifiWanted) {
    Serial.print("[DBG] Auto-connecting with saved WiFi: ");
    Serial.println(Router_SSID);
    startConnectWiFi(Router_SSID, Router_Password);  // non-blocking; loop()'s pollWifiConnect() finishes it
  } else if (Router_SSID.length() > 0) {
    Serial.println("[DBG] Saved WiFi found but last state was disconnected - not auto-connecting");
  }
}

// ============================
// Command dispatch (shared by BLE writes and Serial input)
// ============================
// Parses a single command string (already assembled from either the BLE
// characteristic or a line typed into Serial) and acts on every key it
// recognizes. Each action replies via bleNotifyAndPrint(), which both
// notifies any connected BLE client and echoes to Serial - so the same
// function serves both input paths uniformly.
void processCommand(const String &command) {
    Serial.print("[CMD] ");
    Serial.println(command);

    // LED: led=on / led=off / led=status
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

    // Relay (mailbox lock/latch): relay=on / relay=off / relay=status
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

    // Auth token: auth=status (reveal it) / auth=new (rotate it).
    // Deliberately refused when this command arrived via HTTP
    // (captureForHttp) - the whole point of the token is that it can only
    // ever be learned by someone with BLE/Serial access, never merely by
    // being on the same WiFi network (see checkHttpAuth()).
    String Auth_State = getValue(command, "auth");
    if (Auth_State == "status" || Auth_State == "new") {
      if (captureForHttp) {
        bleNotifyAndPrint("err=forbidden_over_http");
      } else {
        if (Auth_State == "new") {
          authToken = generateAuthToken();
          wifiPrefs.putString("authtok", authToken);
        }
        bleNotifyAndPrint("auth=" + authToken);
      }
    }

    // WiFi credentials: router_ssid=<name>, router_password=<pass>.
    // Values are urlDecode()'d after extraction, so a sender can
    // percent-encode a space/';'/'=' inside the SSID or password (which
    // getValue()'s delimiter-based parser would otherwise misread as a
    // structural character) - e.g. a network named "My House WiFi" must
    // be sent as "router_ssid=My%20House%20WiFi". Each is saved to flash
    // immediately on receipt, independent of whether a "wifi=on" follows
    // in the same command - so partial provisioning (e.g. sending just a
    // new password) still persists.
    String s = urlDecode(getValue(command, "router_ssid"));
    if (s.length()) {
      Router_SSID = s;
      wifiPrefs.putString("ssid", Router_SSID);
      Serial.print("[DBG] router_ssid set (saved): ");
      Serial.println(Router_SSID);
      bleNotifyAndPrint("ok=router_ssid");
    }

    String p = urlDecode(getValue(command, "router_password"));
    if (p.length()) {
      Router_Password = p;
      wifiPrefs.putString("pass", Router_Password);
      Serial.println("[DBG] router_password set (saved)");
      bleNotifyAndPrint("ok=router_password");
    }

    // WiFi control: wifi=on / wifi=off / wifi=status / wifi=scan
    String Wifi_State = getValue(command, "wifi");
    if (Wifi_State == "on") {
      if (Router_SSID.length() == 0) {
        bleNotifyAndPrint("err=no_ssid");
      } else {
        // Record "we want to be connected" before attempting, so even a
        // failed attempt still causes a reconnect retry on the next boot
        // (the user's intent was to be online, not necessarily that it
        // has to have already worked once).
        wifiWanted = true;
        wifiPrefs.putBool("wanted", true);
        // Non-blocking: kicks off the connect attempt and returns right
        // away. The real "wifi=1"/"wifi=0" (+ "ip=...") reply follows
        // later, once pollWifiConnect() (called from loop()) sees it
        // resolve - see startConnectWiFi() for why this can't just wait
        // here the way it used to.
        startConnectWiFi(Router_SSID, Router_Password);
        bleNotifyAndPrint("wifi=connecting");
      }
    } else if (Wifi_State == "off") {
      // Explicit disconnect: clear the "wanted" intent too, so the board
      // stays offline across a reboot until told otherwise (wifi=on or
      // new credentials), instead of silently reconnecting on its own.
      // Also cancels a still-in-progress connect attempt, if any.
      wifiConnectPending = false;
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
      // Same reasoning as the wifi=on branch above: a client that just
      // (re)connected over BLE needs the IP to be able to switch to
      // controlling the board over WiFi instead.
      if (wifiConnected) bleNotifyAndPrint("ip=" + WiFi.localIP().toString());
    } else if (Wifi_State == "scan") {
      // Non-blocking: WiFi.scanNetworks(true) starts an async scan and
      // returns immediately; pollWifiScan() (called from loop()) reports
      // each found network - individually, rather than one giant combined
      // message that could exceed the BLE notify payload limit - once
      // WiFi.scanComplete() indicates it's done.
      if (wifiScanPending) {
        bleNotifyAndPrint("err=wifi_scan_busy");
      } else {
        Serial.println("[WIFI] scanning...");
        if (WiFi.getMode() != WIFI_STA) WiFi.mode(WIFI_STA);
        WiFi.scanNetworks(true);
        wifiScanPending = true;
      }
    }

    // Stream control: stream=on / stream=off / stream=status
    String Stream_State = getValue(command, "stream");
    if (Stream_State == "on") {
      startStreamingAndReport();
    } else if (Stream_State == "off") {
      stopStreamingAndReport();
    } else if (Stream_State == "status") {
      bleNotifyAndPrint(streamEnabled ? "stream=1" : "stream=0");
      if (streamEnabled && wifiConnected) {
        // re-send the URLs too, so a client that just (re)connected while
        // streaming was already running can resume showing video
        String ip = WiFi.localIP().toString();
        bleNotifyAndPrint("stream_url=http://" + ip + ":81/stream?t=" + authToken);
        bleNotifyAndPrint("jpg_url=http://" + ip + "/jpg?t=" + authToken);
      }
    }
}

void loop() {
  static uint32_t lastTick = 0;
  uint32_t now = millis();

#if ENABLE_STATE_LOG
  // Runs on its own timer, independent of the 50ms command-poll gate below,
  // so it never gets skipped/delayed by that early return.
  static uint32_t lastStateLog = 0;
  if (now - lastStateLog >= STATE_LOG_INTERVAL_MS) {
    lastStateLog = now;
    logState();
  }
#endif

  // Non-blocking WiFi connect/scan progress checks - both are no-ops
  // unless one is actually in flight. Run every iteration, ahead of the
  // 50ms tick gate below, so they resolve promptly.
  pollWifiConnect();
  pollWifiScan();

  // Serial commands: accumulate a line, dispatch on newline. Handles any
  // Serial Monitor line-ending setting (CR, LF, or CRLF), and also runs
  // ahead of the 50ms tick gate below so typed commands are never delayed.
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

  // Throttle the rest of the loop body (BLE command polling) to run at
  // most once every 50ms, instead of as fast as possible.
  if (now - lastTick < 50) return;
  lastTick = now;

  // BLE commands: snapshot and clear rxload under the mutex, then dispatch
  // outside the lock so the BLE callback task (which only ever touches
  // rxload itself) isn't held up by processCommand() running here.
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
