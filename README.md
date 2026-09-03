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

A desktop GUI app (`tools/mailbox_gui.py`) acts as the "phone app" for this
project: connect, provision WiFi, flip the LED/relay, watch and record the
video stream.

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
3. **HTTP**, once WiFi is connected — `GET http://<board-ip>/cmd?c=<url-encoded command>`
   runs the same command and replies with the same text a BLE notification
   would have carried. A client learns the board's IP from the `ip=...`
   notification sent after a successful `wifi=on` or `wifi=status`.

### Command reference

| Command | Effect | Reply |
|---|---|---|
| `led=on` / `led=off` | Turns the LED on/off | `led=1` / `led=0` |
| `led=status` | Reports current LED state | `led=1` or `led=0` |
| `relay=on` / `relay=off` | Turns the relay on/off | `relay=1` / `relay=0` |
| `relay=status` | Reports current relay state | `relay=1` or `relay=0` |
| `router_ssid=<name>` | Sets and saves the WiFi SSID to flash | `ok=router_ssid` |
| `router_password=<pass>` | Sets and saves the WiFi password to flash | `ok=router_password` |
| `wifi=on` | Connects to the saved SSID/password (blocks up to ~10s) | `wifi=1` on success + `ip=<addr>`, or `wifi=0` on failure |
| `wifi=off` | Disconnects and stops auto-reconnecting on future boots | `wifi=0` |
| `wifi=status` | Reports current WiFi connection state | `wifi=1`/`wifi=0`, plus `ip=<addr>` if connected |
| `wifi=scan` | Scans for nearby networks | One `wifi_scan=<ssid>` per network found, then `wifi_scan_done=<count>` |
| `stream=on` | Starts the camera and HTTP video server | `stream_url=http://<ip>:81/stream` and `jpg_url=http://<ip>/jpg` |
| `stream=off` | Stops streaming (server keeps running, just stops serving frames) | `stream=0` |
| `stream=status` | Reports current streaming state | `stream=1`/`stream=0`; if already streaming, also resends the URLs above |

Errors are reported as `err=<reason>` (`err=no_ssid`, `err=wifi_off`,
`err=camera_init`, `err=wifi_scan_failed`, `err=no_command`).

### WiFi persistence

`router_ssid`/`router_password` are saved to flash (NVS) immediately when
received. A separate persisted flag tracks *intent*: `wifi=on` sets it,
`wifi=off` clears it. On every boot, the board only auto-reconnects if both
a saved SSID **and** that intent flag are present — so an explicit
disconnect sticks across power cycles, instead of the board silently
reconnecting on its own every time it boots.

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
- Toggles LED and relay — sent over HTTP once the board's WiFi IP is known
  (learned from BLE, cached locally in `tools/mailbox_gui_config.json` so
  it's available immediately on the next launch too), falling back to BLE
  automatically if the IP isn't known yet.
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

## Security notes

This is a hobby project, not a hardened product:

- **No BLE pairing/bonding/encryption** — anyone in range who knows the
  service/characteristic UUIDs (visible in this repo) can connect and issue
  commands.
- **No authentication on the HTTP endpoints** — anyone on the same WiFi
  network can hit `/cmd`, `/jpg`, or `/stream` directly.
- **WiFi credentials are stored in plaintext** in both BLE transit and
  on-flash storage.
- Neither port is exposed to the internet unless you explicitly configure
  port forwarding on your router.

Don't rely on this for anything where unauthorized access to the relay
(mailbox lock) or camera feed would be a real problem, without adding
proper authentication first.
