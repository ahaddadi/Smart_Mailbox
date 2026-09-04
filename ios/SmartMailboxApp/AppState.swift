//
//  AppState.swift
//  SmartMailboxApp
//
//  The app's single source of truth, published for SwiftUI to observe -
//  the Swift/Combine equivalent of tools/mailbox_gui.py's MailboxApp
//  class. Owns the BLEManager, decides BLE vs. HTTP transport for each
//  command (mirroring _http_command()), translates/logs notifications
//  (mirroring _handle_notify()), and drives the video stream + recorder.
//
//  BLE vs. WiFi transport: WiFi provisioning (scanning, credentials,
//  wifi=on/off) always goes over BLE, since the board isn't reachable
//  over WiFi until it has joined a network. Once connected, its
//  "ip=..."/"auth=..." notifications (sent after wifi=on/wifi=status and
//  auth=status respectively) tell this app where and how to reach it over
//  plain HTTP, and from then on LED/relay/stream control switches to
//  HTTP via httpCommand() - falling back to BLE automatically if either
//  piece is still unknown. Both are cached in UserDefaults so HTTP
//  control can work immediately on the next launch too.
//

import Combine
import Foundation
import UIKit

@MainActor
final class AppState: ObservableObject {
    struct LogEntry: Identifiable {
        let id = UUID()
        let text: String
        let tag: LogTag
    }

    @Published var statusText = "Disconnected"
    @Published var ledOn: Bool?
    @Published var relayOn: Bool?
    @Published var wifiState: String = "?"
    @Published var networks: [String] = []
    @Published var scanStatus: String = ""
    @Published var logs: [LogEntry] = []
    @Published var currentFrame: UIImage?
    @Published var isStreaming = false
    @Published var isRecording = false

    @Published var boardIP: String? {
        didSet { UserDefaults.standard.set(boardIP, forKey: "board_ip") }
    }
    @Published var authToken: String? {
        didSet { UserDefaults.standard.set(authToken, forKey: "auth_token") }
    }

    let ble = BLEManager()
    private var mjpegStream: MJPEGStream?
    private let recorder = VideoRecorder()
    private var knownNetworks: [String] = []
    private var cancellables = Set<AnyCancellable>()

    init() {
        boardIP = UserDefaults.standard.string(forKey: "board_ip")
        authToken = UserDefaults.standard.string(forKey: "auth_token")

        ble.onNotify = { [weak self] text in
            Task { @MainActor in self?.handleNotify(text) }
        }
        ble.onReady = { [weak self] in
            Task { @MainActor in self?.refreshStatus() }
        }
        // Mirror BLEManager's own status text into the log, matching the
        // desktop app's [STATUS] log lines.
        ble.$statusText
            .receive(on: DispatchQueue.main)
            .sink { [weak self] text in
                guard let self else { return }
                self.statusText = text
                let failed = text.lowercased().contains("failed") || text.lowercased().contains("unavailable")
                self.log("[STATUS] \(text)", tag: failed ? .error : .normal)
            }
            .store(in: &cancellables)

        if boardIP != nil, authToken != nil {
            log("[App] Using last known board IP: \(boardIP!) (unconfirmed)", tag: .normal)
        }
    }

    // MARK: - actions

    func connectBLE() { ble.connect() }
    func disconnectBLE() { ble.disconnect() }

    func refreshStatus() {
        ble.send("led=status;relay=status;wifi=status;stream=status;auth=status")
    }

    func wifiConnect(ssid: String, password: String) {
        guard !ssid.isEmpty else {
            log("[App] Enter an SSID first", tag: .normal)
            return
        }
        let ssidEnc = ProtocolTranslator.percentEncode(ssid)
        let passEnc = ProtocolTranslator.percentEncode(password)
        ble.send("router_ssid=\(ssidEnc);router_password=\(passEnc);wifi=on")
    }

    func wifiDisconnect() {
        ble.send("wifi=off")
    }

    func wifiScan() {
        networks = []
        knownNetworks = []
        scanStatus = "Scanning..."
        ble.send("wifi=scan")
    }

    /// Send `cmd` (e.g. "led=on") over HTTP once board_ip/auth_token are
    /// both known, falling back to BLE otherwise - mirrors
    /// tools/mailbox_gui.py's _http_command().
    func httpCommand(_ cmd: String) {
        guard let ip = boardIP, let token = authToken else {
            log("[App] Board IP or auth token not known yet - sending \"\(cmd)\" over BLE instead", tag: .normal)
            ble.send(cmd)
            return
        }
        Task {
            var components = URLComponents()
            components.scheme = "http"
            components.host = ip
            components.path = "/cmd"
            components.queryItems = [
                URLQueryItem(name: "c", value: cmd),
                URLQueryItem(name: "t", value: token),
            ]
            guard let url = components.url else { return }
            do {
                var request = URLRequest(url: url)
                request.timeoutInterval = 5
                let (data, _) = try await URLSession.shared.data(for: request)
                let text = String(data: data, encoding: .utf8) ?? ""
                for line in text.split(separator: "\n") {
                    let trimmed = line.trimmingCharacters(in: .whitespaces)
                    if !trimmed.isEmpty {
                        handleNotify(trimmed)
                    }
                }
            } catch {
                log("[STATUS] HTTP command '\(cmd)' failed: \(error.localizedDescription)", tag: .error)
            }
        }
    }

    func streamOff() {
        httpCommand("stream=off")
        stopStream()
    }

    func toggleRecording() {
        if isRecording {
            recorder.stop { [weak self] result in
                Task { @MainActor in
                    guard let self else { return }
                    switch result {
                    case .success:
                        self.log("Saved recording to Photos", tag: .ok)
                    case .failure(let error):
                        self.log("Recording save failed: \(error.localizedDescription)", tag: .error)
                    }
                    self.isRecording = false
                }
            }
        } else {
            guard isStreaming else {
                log("[App] Start the stream before recording", tag: .error)
                return
            }
            recorder.start()
            isRecording = true
            log("Recording started", tag: .ok)
        }
    }

    // MARK: - notify handling (mirrors _handle_notify in mailbox_gui.py)

    private func handleNotify(_ text: String) {
        log(ProtocolTranslator.readable(text), tag: ProtocolTranslator.tag(for: text))

        if text.hasPrefix("wifi_scan_done=") {
            scanStatus = "Found \(ProtocolTranslator.value(of: text)) network(s)"
        } else if text.hasPrefix("wifi_scan=") {
            let ssid = ProtocolTranslator.value(of: text)
            if !ssid.isEmpty, !knownNetworks.contains(ssid) {
                knownNetworks.append(ssid)
                networks.append(ssid)
            }
        } else if text == "err=wifi_scan_failed" {
            scanStatus = "Scan failed"
        } else if text.hasPrefix("led=") {
            ledOn = text.hasSuffix("1")
        } else if text.hasPrefix("relay=") {
            relayOn = text.hasSuffix("1")
        } else if text == "wifi=connecting" {
            // Must be checked before the generic "wifi=" prefix case below -
            // the real wifi=1/wifi=0 follows later once the firmware's
            // pollWifiConnect() resolves it.
            wifiState = "Connecting..."
        } else if text.hasPrefix("wifi=") {
            wifiState = text.hasSuffix("1") ? "Connected" : "Disconnected"
        } else if text.hasPrefix("ip=") {
            boardIP = ProtocolTranslator.value(of: text)
        } else if text.hasPrefix("auth=") {
            authToken = ProtocolTranslator.value(of: text)
        } else if text.hasPrefix("stream_url=") {
            startStream(urlString: ProtocolTranslator.value(of: text))
        } else if text == "stream=0" {
            stopStream()
        }
    }

    // MARK: - streaming

    private func startStream(urlString: String) {
        stopStream()
        guard let url = URL(string: urlString) else { return }
        isStreaming = true
        let stream = MJPEGStream(
            url: url,
            onFrame: { [weak self] image in
                Task { @MainActor in
                    guard let self else { return }
                    self.currentFrame = image
                    if self.isRecording {
                        self.recorder.append(image: image)
                    }
                }
            },
            onError: { [weak self] message in
                Task { @MainActor in self?.log("[STATUS] \(message)", tag: .error) }
            }
        )
        mjpegStream = stream
        stream.start()
    }

    private func stopStream() {
        mjpegStream?.stop()
        mjpegStream = nil
        isStreaming = false
        currentFrame = nil
        if isRecording {
            recorder.stop { _ in }
            isRecording = false
        }
    }

    func log(_ text: String, tag: LogTag) {
        logs.append(LogEntry(text: text, tag: tag))
        if logs.count > 500 {
            logs.removeFirst(logs.count - 500)
        }
    }
}
