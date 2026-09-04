//
//  ProtocolTranslator.swift
//  SmartMailboxApp
//
//  Ports tools/mailbox_gui.py's NOTIFY_TEXT / _readable_notify / _tag_for
//  functions: translates the board's raw "key=value" notifications (over
//  BLE or HTTP - see Smart_Mailbox.ino's processCommand()) into plain
//  English for the log panel, and classifies each as ok/error/neutral for
//  color-coding. Kept in sync by hand with the Python version and the
//  firmware's actual message vocabulary.
//

import Foundation

enum LogTag {
    case ok
    case error
    case normal
}

enum ProtocolTranslator {
    // Exact-match table first (checked in `readable(_:)`), matching
    // Smart_Mailbox.ino's bleNotifyAndPrint() call sites exactly.
    static let notifyText: [String: String] = [
        "led=1": "LED turned ON",
        "led=0": "LED turned OFF",
        "relay=1": "Relay turned ON",
        "relay=0": "Relay turned OFF",
        "wifi=connecting": "Connecting to WiFi...",
        "wifi=1": "WiFi connected",
        "wifi=0": "WiFi disconnected",
        "stream=1": "Streaming started",
        "stream=0": "Streaming stopped",
        "ok=router_ssid": "WiFi SSID saved",
        "ok=router_password": "WiFi password saved",
        "err=wifi_off": "Error: WiFi is not connected",
        "err=camera_init": "Error: camera failed to initialize",
        "err=no_ssid": "Error: no SSID set",
        "err=wifi_scan_failed": "Error: WiFi scan failed",
        "err=wifi_scan_busy": "Error: a WiFi scan is already in progress",
        "err=unauthorized": "Error: HTTP request rejected (bad/missing auth token)",
        "err=forbidden_over_http": "Error: the auth token can only be requested over BLE/Serial",
        "err=no_command": "Error: empty HTTP command",
    ]

    // Which raw (untranslated) notifications count as a clear success, for
    // log color-coding. Checked against the *raw* text (before translation)
    // so e.g. "wifi=0" ("WiFi disconnected") never gets miscolored just
    // because its English translation happens to contain "connected".
    static let okNotifies: Set<String> = [
        "led=1", "relay=1", "wifi=1", "stream=1", "ok=router_ssid", "ok=router_password",
    ]

    /// Everything after the first '=' in a "key=value" notification.
    static func value(of text: String) -> String {
        guard let eq = text.firstIndex(of: "=") else { return "" }
        return String(text[text.index(after: eq)...])
    }

    /// Raw board notification -> plain-English log text. Anything
    /// unrecognized is shown as-is rather than dropped, so nothing goes
    /// silently missing even if the firmware adds a new message type later.
    static func readable(_ text: String) -> String {
        if let mapped = notifyText[text] { return mapped }
        if text.hasPrefix("ip=") { return "Board WiFi IP: " + value(of: text) }
        if text.hasPrefix("auth=") { return "Auth token: " + value(of: text) }
        if text.hasPrefix("stream_url=") { return "Stream URL: " + value(of: text) }
        if text.hasPrefix("jpg_url=") { return "Snapshot URL: " + value(of: text) }
        if text.hasPrefix("wifi_scan_done=") { return "WiFi scan complete: \(value(of: text)) network(s) found" }
        if text.hasPrefix("wifi_scan=") { return "Found network: " + value(of: text) }
        return text
    }

    static func tag(for rawText: String) -> LogTag {
        if rawText.hasPrefix("err=") { return .error }
        if okNotifies.contains(rawText) || rawText.hasPrefix("wifi_scan=") || rawText.hasPrefix("auth=") {
            return .ok
        }
        return .normal
    }

    /// Percent-encodes a value for safe inclusion in a "key=value" command
    /// (e.g. router_ssid/router_password), matching the firmware's
    /// urlDecode() and the desktop app's urllib.parse.quote(safe="") - a
    /// raw '=', ';', or space inside a value would otherwise be misread as
    /// a command delimiter by the firmware's parser.
    static func percentEncode(_ s: String) -> String {
        var allowed = CharacterSet.alphanumerics
        allowed.insert(charactersIn: "-._~")
        return s.addingPercentEncoding(withAllowedCharacters: allowed) ?? s
    }
}
