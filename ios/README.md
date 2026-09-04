# Smart Mailbox — iOS App

A native SwiftUI iOS app that mirrors every feature of the desktop GUI
(`tools/mailbox_gui.py`): BLE connect, WiFi network scan/connect/disconnect,
LED and relay control, live MJPEG camera stream view, video recording to
Photos, and a color-coded activity log. It talks to the same board over the
same BLE service/characteristic and the same HTTP `/cmd`, `/jpg`, `/stream`
endpoints described in the main [README](../README.md).

This project was hand-authored (including the `.xcodeproj` file itself) in
an environment without Xcode/macOS available, so it has **not been
build-verified**. It should open and build cleanly, but if Xcode reports a
missing setting on first open, check it against the notes below before
filing it as a real bug.

## Opening the project

1. Copy or clone this repo onto a Mac.
2. Open `ios/SmartMailboxApp.xcodeproj` in Xcode (15 or newer recommended;
   the project targets iOS 16.0+).
3. Select the `SmartMailboxApp` target → **Signing & Capabilities**:
   - Set your own **Team** (the project ships with no team assigned).
   - Change **Bundle Identifier** from the placeholder
     `com.example.SmartMailboxApp` to something unique to you, e.g.
     `com.yourname.SmartMailboxApp`.
4. Plug in an iPhone (or pick a simulator — see the Bluetooth caveat below)
   and hit Run.

## Permissions

The app requests three permissions on first use, all declared in
`SmartMailboxApp/Info.plist`:

- **Bluetooth** — to discover and pair with the mailbox over BLE.
- **Local Network** — to reach the board's HTTP server on your LAN.
- **Photos (Add)** — to save recorded video clips.

Accept all three when prompted, or the corresponding features (BLE connect,
HTTP control/stream, video save) will silently fail.

`Info.plist` also sets `NSAllowsLocalNetworking` under
`NSAppTransportSecurity`. This is required for the app to make plain HTTP
requests to the board's LAN IP address — without it, iOS's App Transport
Security blocks non-HTTPS connections by default.

## Simulator vs. real device

Bluetooth is not available in the iOS Simulator. Use a real iPhone to test
BLE connect/provisioning. Once the board has an IP on your WiFi, the HTTP
control/stream/video path also works from the Simulator (as long as the Mac
is on the same network as the board).

## How it maps to the Python GUI

| Python GUI (`tools/mailbox_gui.py`) | iOS app |
|---|---|
| `BLEWorker` (asyncio + bleak) | `BLEManager.swift` (CoreBluetooth) |
| `MJPEGReader` | `MJPEGStream.swift` |
| `_handle_notify` / `_readable_notify` / log tagging | `ProtocolTranslator.swift` |
| cv2-based AVI recording | `VideoRecorder.swift` (AVAssetWriter → Photos) |
| `MailboxApp` (Tk state + widgets) | `AppState.swift` + `ContentView.swift` |
| dark theme constants | `Theme.swift` |
| `mailbox_gui_config.json` (board IP + auth token) | `UserDefaults` in `AppState.swift` |

The BLE service/characteristic UUIDs and the HTTP command protocol
(`key=value` pairs, percent-encoding, auth token) are identical across both
clients and the firmware — see the main [README](../README.md) for the full
protocol reference.
