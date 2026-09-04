# Smart Mailbox

A BLE- and WiFi-controlled "smart mailbox" built on an ESP32-S3 with a camera
module. It can:

- Toggle an **LED** and a **relay** (driving the mailbox's lock/latch)
- Be **provisioned with WiFi credentials** over Bluetooth and reconnect to
  that network automatically after every reboot or power loss
- **Scan for nearby WiFi networks** and report them back to a client
- Start a **live MJPEG camera stream** (and single JPEG snapshots) over HTTP
  once WiFi is connected
- Be controlled over **BLE, USB Serial, or plain HTTP** — whichever is most
  convenient at the time

A desktop GUI app (`tools/mailbox_gui.py`) and a native iOS app
(`ios/SmartMailbox/`) both act as the "phone app" for this project: connect,
provision WiFi, flip the LED/relay, watch and record the video stream.

## Hardware

- An ESP32-S3 board with a camera module. The firmware defaults to
  `CAMERA_MODEL_ESP32S3_EYE` — if you're using a different board (e.g. XIAO
  ESP32S3 Sense), change the `#define CAMERA_MODEL_...` near the top of
  [`Smart_Mailbox.ino`](Smart_Mailbox.ino) to match your board's pinout (see
  [`camera_pins.h`](camera_pins.h) for the supported list).
- An LED on **GPIO 2**.
- A relay module on **GPIO 41**, driving the mailbox lock/latch.

## Repository layout

```
Smart_Mailbox.ino       Main firmware (BLE + Serial + WiFi + camera + HTTP control)
camera_pins.h            Pin maps for various ESP32-CAM board variants
BLETest/BLETest.ino      Minimal standalone BLE-only test sketch
SerialTest/SerialTest.ino  Minimal sketch that prints an incrementing counter,
                            for verifying the USB/COM connection in isolation
tools/
  mailbox_gui.py          Desktop GUI: BLE + HTTP control, live video, recording
  ble_console.py          Minimal interactive BLE command-line client
  requirements.txt        Python dependencies for both tools
ios/SmartMailbox/
  SmartMailboxApp.xcodeproj  Xcode project
  SmartMailboxApp/           Swift sources for the native iOS app
  README.md                  iOS-specific setup instructions
```

## Firmware setup

### Arduino IDE settings

Board: **ESP32S3 Dev Module** (or your specific ESP32-S3 board entry).

Two settings matter a lot on ESP32-S3 boards that use the native USB
peripheral (no separate USB-UART bridge chip):

| Setting | Value |
|---|---|
| Tools → USB CDC On Boot | **Enabled** |
| Tools → USB Mode | **Hardware CDC and JTAG** |

Without these, `Serial` output won't reach the same USB port used for
flashing, and the Serial Monitor will show the boot ROM banner but nothing
from the sketch itself.

Also worth checking: **Tools → Partition Scheme**. The sketch is fairly
large (BLE + WiFi + camera + HTTP server); if it's close to filling the
default partition's app space, switch to a scheme with a bigger app
partition.

### Flashing

Open `Smart_Mailbox.ino` in the Arduino IDE, select the board and port, and
upload as usual. On first boot, open the Serial Monitor at **115200 baud**
to watch the `[DBG]`/`[STATE]` log output described below.

## Control protocol

The board is controlled with short text commands of the form `key=value`.
Multiple commands can be chained with `;` or a space:

```
led=on
relay=off
led=on;relay=off
led=on relay=off
router_ssid=MyWifi;router_password=secret;wifi=on
```

Commands can arrive over three transports, all funneled into the same
dispatcher (`processCommand()` in the firmware):

1. **BLE** — write the command string to the custom characteristic
   (UUID `4ac8a682-9736-4e5d-932b-e9b31405049c`, under service
   `ab0828b1-198e-4351-b779-901fa0e0371e`, advertised as `Smart_Mailbox`).
   Replies come back as notifications on the same characteristic. This is
   the only transport that works **before** the board has joined a WiFi
   network, so initial provisioning always happens here.
2. **USB Serial** — type a command into the Serial Monitor (115200 baud)
   and press Enter. Useful for testing without any BLE client.
3. **HTTP**, once WiFi is connected — `GET http://<board-ip>/cmd?c=<url-encoded command>&t=<auth token>`
   runs the same command and replies with the same text a BLE notification
   would have carried. A client learns the board's IP from the `ip=...`
   notification sent after a successful `wifi=on` or `wifi=status`, and
   the auth token from `auth=status` — see "Security" below; every HTTP
   request without a valid `t=` is rejected with `401 Unauthorized`.

**Values containing `=`, `;`, or a space** (SSIDs and passwords, mainly)
must be percent-encoded by the sender — the parser treats a raw one of
those characters as structural. E.g. a network named `My House WiFi` is
sent as `router_ssid=My%20House%20WiFi`. The GUI does this automatically
(`urllib.parse.quote`); the firmware reverses it with `urlDecode()`.

### Command reference

| Command | Effect | Reply |
|---|---|---|
| `led=on` / `led=off` | Turns the LED on/off | `led=1` / `led=0` |
| `led=status` | Reports current LED state | `led=1` or `led=0` |
| `relay=on` / `relay=off` | Turns the relay on/off | `relay=1` / `relay=0` |
| `relay=status` | Reports current relay state | `relay=1` or `relay=0` |
| `router_ssid=<name>` | Sets and saves the WiFi SSID to flash (percent-encoded, see above) | `ok=router_ssid` |
| `router_password=<pass>` | Sets and saves the WiFi password to flash (percent-encoded) | `ok=router_password` |
| `wifi=on` | Starts connecting to the saved SSID/password — **non-blocking**, returns immediately | `wifi=connecting` right away, then `wifi=1` + `ip=<addr>` on success or `wifi=0` on failure/timeout (up to ~10s later) |
| `wifi=off` | Disconnects, cancels any in-progress connect attempt, and stops auto-reconnecting on future boots | `wifi=0` |
| `wifi=status` | Reports current WiFi connection state | `wifi=1`/`wifi=0`, plus `ip=<addr>` if connected |
| `wifi=scan` | Scans for nearby networks — **non-blocking** | One `wifi_scan=<ssid>` per network found, then `wifi_scan_done=<count>`, once the scan (which itself takes a few seconds) completes. `err=wifi_scan_busy` if one is already running. |
| `auth=status` | Reveals the current HTTP auth token — **BLE/Serial only** | `auth=<32-char hex token>` |
| `auth=new` | Rotates the HTTP auth token (invalidates the old one) — **BLE/Serial only** | `auth=<new token>` |
| `stream=on` | Starts the camera and HTTP video server | `stream_url=http://<ip>:81/stream?t=<token>` and `jpg_url=http://<ip>/jpg?t=<token>` |
| `stream=off` | Stops streaming (server keeps running, just stops serving frames) | `stream=0` |
| `stream=status` | Reports current streaming state | `stream=1`/`stream=0`; if already streaming, also resends the URLs above |

Errors are reported as `err=<reason>` (`err=no_ssid`, `err=wifi_off`,
`err=camera_init`, `err=wifi_scan_failed`, `err=wifi_scan_busy`,
`err=no_command`, `err=unauthorized`, `err=forbidden_over_http`).

### WiFi persistence

`router_ssid`/`router_password` are saved to flash (NVS) immediately when
received. A separate persisted flag tracks *intent*: `wifi=on` sets it,
`wifi=off` clears it. On every boot, the board only auto-reconnects if both
a saved SSID **and** that intent flag are present — so an explicit
disconnect sticks across power cycles, instead of the board silently
reconnecting on its own every time it boots.

### Non-blocking WiFi connect/scan

`wifi=on` and `wifi=scan` return immediately rather than blocking the
caller for the duration of the operation (up to ~10s for a connect, a few
seconds for a scan). `pollWifiConnect()`/`pollWifiScan()`, called every
`loop()` iteration, track progress in the background and send the real
result as a follow-up notification once it resolves. This keeps BLE
writes, Serial input, and other HTTP requests responsive the whole time —
previously, a `stream=off` sent while a `wifi=on` was still connecting
(or a scan was running) would have to wait for it to finish first.

### HTTP endpoints and why streaming has its own port

Once WiFi is connected, two separate HTTP server instances run:

- **Port 80** — `/jpg` (single snapshot) and `/cmd` (command dispatch, see
  above).
- **Port 81** — `/stream` only (MJPEG multipart video).

They're deliberately split because `/stream`'s handler blocks in a loop for
as long as a client stays connected, and ESP-IDF's HTTP server only
processes one request at a time per server instance — sharing a server
would make every `/cmd` request queue up (and time out) behind an open
video stream. The two server tasks are also pinned to different CPU cores
with different priorities so control commands stay responsive under
streaming load. (This mirrors the structure of Espressif's own
`CameraWebServer` example.)

Every request to any of these three endpoints (`/cmd`, `/jpg`, `/stream`)
must include `?t=<auth token>` (or `&t=...` alongside other query
parameters) or it gets `401 Unauthorized` — see "Security" below.

### Debug logging

Every 5 seconds (configurable via `STATE_LOG_INTERVAL_MS`, and disable
entirely by setting `ENABLE_STATE_LOG` to `0`), the board prints a one-line
state snapshot to Serial:

```
[STATE] uptime=72s heap=77916 ble=1 led=1 relay=0 wifi=1 ssid="MyWifi" ip=10.0.0.60 stream=1 streamServer=1 camera=1
```

## Python tools

Install dependencies once:

```bash
pip install -r tools/requirements.txt
```

### `tools/mailbox_gui.py` — desktop GUI

```bash
python tools/mailbox_gui.py
```

A dark-themed Tkinter app that:

- Connects to the board over BLE (scan by name, auto-subscribes to
  notifications).
- Toggles LED and relay — sent over HTTP once the board's WiFi IP *and*
  HTTP auth token are known (both learned from BLE, cached locally in
  `tools/mailbox_gui_config.json` so they're available immediately on the
  next launch too), falling back to BLE automatically if either isn't
  known yet.
- Scans for WiFi networks and lets you pick one, enter a password, and
  connect — this part always goes over BLE, since the board isn't
  reachable over WiFi until it has joined a network.
- Starts/stops the camera stream and displays it live, decoding the MJPEG
  multipart response frame by frame.
- Records the live stream to an `.avi` file (via OpenCV) at the camera's
  full resolution.
- Shows a scrolling, color-coded log of every event, translating raw
  protocol messages (`led=1`, `wifi=0`, ...) into plain English.

### `tools/ble_console.py` — minimal BLE CLI

```bash
python tools/ble_console.py
```

Scans for the board, connects, subscribes to notifications, and drops you
into a prompt where you can type raw commands (`led=on`, `wifi=status`,
`stream=on`, ...) and see the replies — useful for quick testing without
the full GUI.

## iOS app

A native SwiftUI app under [`ios/SmartMailbox/`](ios/SmartMailbox/) mirrors
every feature of the desktop GUI, using the same BLE service/characteristic
and HTTP `/cmd`, `/jpg`, `/stream` protocol described above:

- Connects over BLE (scans by advertised name, same as the Python tools),
  auto-subscribes to notifications, and reconnects automatically if the
  link drops.
- Toggles LED and relay over HTTP once the board's IP and auth token are
  known (cached in `UserDefaults`), falling back to BLE otherwise.
- Scans for WiFi networks and connects/disconnects, over BLE.
- Displays the live MJPEG camera stream and records it to Photos.
- Shows a color-coded activity log, same translation logic as the desktop
  GUI's log view.

Open `ios/SmartMailbox/SmartMailboxApp.xcodeproj` in Xcode (16.0+ deployment
target) — see [`ios/SmartMailbox/README.md`](ios/SmartMailbox/README.md)
for signing setup, required permissions, and the Simulator's Bluetooth
limitation.

## Security notes

This is a hobby project, not a hardened product:

- **HTTP endpoints require a per-device auth token.** A 32-character hex
  token is generated on first boot (hardware RNG, `esp_random()`) and
  persisted to flash. Every request to `/cmd`, `/jpg`, or `/stream` must
  include it (`?t=...`); a missing or wrong token gets
  `401 Unauthorized` with no further processing. The token itself is
  obtainable **only** via `auth=status`/`auth=new` over BLE or Serial —
  an HTTP request for it (`captureForHttp` is set) is refused with
  `err=forbidden_over_http`. So merely being on the same WiFi network is
  no longer enough to drive the relay or view the camera; an attacker
  still needs BLE (or physical USB Serial) access first to obtain the
  token.
- **BLE requires pairing.** The command/notify characteristic needs an
  encrypted, paired link (`PROPERTY_READ_ENC`/`PROPERTY_WRITE_ENC`) —
  a central has to complete BLE pairing before it can read, write, or be
  notified on it at all, including reading the HTTP auth token via
  `auth=status`. Pairing uses "Just Works" (`ESP_IO_CAP_NONE`, no MITM
  protection) with bonding, so there's no PIN to type and it only
  happens once per central — but Just Works is still vulnerable to an
  active man-in-the-middle *during that first pairing*, so it raises the
  bar (knowing the UUIDs is no longer enough) without being
  bulletproof. Controlled by `ENABLE_BLE_PAIRING` in the firmware (set
  to `0` and reflash to fall back to fully open BLE if pairing ever
  causes connection trouble with a particular central).
- **WiFi credentials and the auth token are stored in plaintext at rest** —
  on the board's flash, in the desktop GUI's local
  `mailbox_gui_config.json` cache, and in the iOS app's `UserDefaults`
  cache. In transit over BLE they're protected by the link-layer
  encryption pairing provides (see above) when `ENABLE_BLE_PAIRING` is on
  (the default); with it disabled, BLE transit is plaintext too.
- Neither HTTP port is exposed to the internet unless you explicitly
  configure port forwarding on your router.

If BLE pairing ever needs to be reset (e.g. after re-flashing the board,
if the two sides' bond keys get out of sync), forget/remove the
`Smart_Mailbox` device from your OS's Bluetooth settings and reconnect —
a fresh pairing will be negotiated automatically.
