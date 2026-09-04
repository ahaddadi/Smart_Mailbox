//
//  BLEManager.swift
//  SmartMailboxApp
//
//  CoreBluetooth central for the board's custom command/notify
//  characteristic (see Smart_Mailbox.ino's setupBLE()). Mirrors
//  tools/mailbox_gui.py's BLEWorker: connect() scans and connects,
//  send() writes a command string, and onNotify/onReady callbacks report
//  results back to the app's state (AppState) — the SwiftUI/Combine
//  equivalent of the Python version's event-queue handoff to the Tk
//  thread, except CoreBluetooth already delivers its delegate callbacks
//  on the queue we hand it (set to .main below), so no extra queue
//  hand-off is needed here.
//
//  Pairing: the command characteristic requires an encrypted link
//  (PROPERTY_READ_ENC/PROPERTY_WRITE_ENC in the firmware, when
//  ENABLE_BLE_PAIRING is on). iOS's CoreBluetooth stack handles the
//  "Bluetooth Pairing Request" system prompt automatically the first
//  time a read/write needs it — no app code required here.
//

import Foundation
import CoreBluetooth

final class BLEManager: NSObject, ObservableObject {

    // MARK: - Constants (must match Smart_Mailbox.ino exactly)

    static let serviceUUID = CBUUID(string: "ab0828b1-198e-4351-b779-901fa0e0371e")
    static let messageUUID = CBUUID(string: "4ac8a682-9736-4e5d-932b-e9b31405049c")
    static let deviceName  = "Smart_Mailbox"

    /// How long to scan before giving up.
    static let scanTimeout: TimeInterval = 15

    /// Delay before an automatic reconnect attempt after unexpected disconnect.
    static let reconnectDelay: TimeInterval = 2

    // MARK: - Published state

    @Published var statusText: String = "Disconnected"
    @Published var isConnected: Bool  = false

    // MARK: - Callbacks

    /// Called with each raw notify string (e.g. "led=1") as it arrives.
    var onNotify: ((String) -> Void)?
    /// Called once the connection is fully usable (subscribed to notifications).
    var onReady: (() -> Void)?

    // MARK: - Private state

    private var centralManager: CBCentralManager!
    private var peripheral: CBPeripheral?
    private var commandCharacteristic: CBCharacteristic?

    /// True when the caller asked to connect but Bluetooth wasn't powered on yet.
    private var connectPending = false

    /// True when the caller explicitly wants a connection (cleared on disconnect()).
    private var shouldBeConnected = false

    /// Fires to abort a scan that hasn't found anything.
    private var scanTimer: Timer?

    // MARK: - Init

    override init() {
        super.init()
        centralManager = CBCentralManager(delegate: self, queue: .main)
    }

    // MARK: - Public API

    func connect() {
        shouldBeConnected = true

        guard centralManager.state == .poweredOn else {
            connectPending = true
            statusText = "Waiting for Bluetooth…"
            return
        }
        startScan()
    }

    func disconnect() {
        shouldBeConnected = false
        connectPending = false
        cancelScanTimer()
        centralManager.stopScan()

        if let p = peripheral {
            centralManager.cancelPeripheralConnection(p)
        }
        cleanUpPeripheral()
        statusText = "Disconnected"
    }

    /// Write a command string (e.g. "led=on") to the board.
    func send(_ cmd: String) {
        guard isConnected,
              let characteristic = commandCharacteristic,
              let p = peripheral else {
            statusText = "Not connected"
            return
        }
        guard let data = cmd.data(using: .utf8) else { return }
        p.writeValue(data, for: characteristic, type: .withResponse)
    }

    // MARK: - Private helpers

    private func startScan() {
        connectPending = false

        if let existing = peripheral {
            centralManager.cancelPeripheralConnection(existing)
        }
        cleanUpPeripheral()

        statusText = "Scanning for \(Self.deviceName)…"
        // Scan without a service filter — many ESP32 boards don't advertise
        // the service UUID in the advertisement packet. We match by name instead.
        centralManager.scanForPeripherals(withServices: nil, options: nil)

        // Timeout so we don't scan forever.
        cancelScanTimer()
        scanTimer = Timer.scheduledTimer(withTimeInterval: Self.scanTimeout, repeats: false) { [weak self] _ in
            guard let self = self else { return }
            self.centralManager.stopScan()
            self.statusText = "Scan timed out — \(Self.deviceName) not found"
        }
    }

    private func cancelScanTimer() {
        scanTimer?.invalidate()
        scanTimer = nil
    }

    private func cleanUpPeripheral() {
        isConnected = false
        commandCharacteristic = nil
        peripheral = nil
    }

    /// Schedule an automatic reconnect if the caller still wants a connection.
    private func scheduleReconnect() {
        guard shouldBeConnected else { return }
        statusText = "Reconnecting in \(Int(Self.reconnectDelay))s…"
        Timer.scheduledTimer(withTimeInterval: Self.reconnectDelay, repeats: false) { [weak self] _ in
            guard let self = self, self.shouldBeConnected else { return }
            self.connect()
        }
    }
}

// MARK: - CBCentralManagerDelegate

extension BLEManager: CBCentralManagerDelegate {

    func centralManagerDidUpdateState(_ central: CBCentralManager) {
        if central.state == .poweredOn {
            if connectPending {
                startScan()
            }
        } else {
            cleanUpPeripheral()
            statusText = "Bluetooth unavailable"
        }
    }

    func centralManager(_ central: CBCentralManager,
                        didDiscover peripheral: CBPeripheral,
                        advertisementData: [String: Any],
                        rssi RSSI: NSNumber) {
        // Match by advertised name. The advertisement's local name is the
        // most reliable source; fall back to peripheral.name which may be
        // cached from a previous session.
        let advName = advertisementData[CBAdvertisementDataLocalNameKey] as? String
        guard advName == Self.deviceName || peripheral.name == Self.deviceName else {
            return // Not our device — keep scanning.
        }

        central.stopScan()
        cancelScanTimer()

        self.peripheral = peripheral
        peripheral.delegate = self
        statusText = "Connecting…"
        central.connect(peripheral, options: nil)
    }

    func centralManager(_ central: CBCentralManager,
                        didConnect peripheral: CBPeripheral) {
        statusText = "Discovering services…"
        peripheral.discoverServices([Self.serviceUUID])
    }

    func centralManager(_ central: CBCentralManager,
                        didFailToConnect peripheral: CBPeripheral,
                        error: Error?) {
        statusText = "Connect failed: \(error?.localizedDescription ?? "unknown error")"
        cleanUpPeripheral()
        scheduleReconnect()
    }

    func centralManager(_ central: CBCentralManager,
                        didDisconnectPeripheral peripheral: CBPeripheral,
                        error: Error?) {
        handleDisconnect(error: error)
    }

    private func handleDisconnect(error: Error?) {
        cleanUpPeripheral()

        if let error = error {
            statusText = "Disconnected: \(error.localizedDescription)"
            scheduleReconnect()
        } else {
            statusText = "Disconnected"
            if shouldBeConnected {
                scheduleReconnect()
            }
        }
    }
}

// MARK: - CBPeripheralDelegate

extension BLEManager: CBPeripheralDelegate {

    func peripheral(_ peripheral: CBPeripheral,
                    didDiscoverServices error: Error?) {
        if let error = error {
            statusText = "Service discovery failed: \(error.localizedDescription)"
            return
        }
        guard let services = peripheral.services else { return }
        for service in services where service.uuid == Self.serviceUUID {
            peripheral.discoverCharacteristics([Self.messageUUID], for: service)
        }
    }

    func peripheral(_ peripheral: CBPeripheral,
                    didDiscoverCharacteristicsFor service: CBService,
                    error: Error?) {
        if let error = error {
            statusText = "Characteristic discovery failed: \(error.localizedDescription)"
            return
        }
        guard let characteristics = service.characteristics else { return }
        for characteristic in characteristics where characteristic.uuid == Self.messageUUID {
            commandCharacteristic = characteristic
            // Enabling notifications writes a descriptor; since the
            // characteristic requires encryption this typically triggers
            // iOS's pairing prompt on first connect.
            peripheral.setNotifyValue(true, for: characteristic)
        }
    }

    func peripheral(_ peripheral: CBPeripheral,
                    didUpdateNotificationStateFor characteristic: CBCharacteristic,
                    error: Error?) {
        guard characteristic.uuid == Self.messageUUID else { return }
        if let error = error {
            statusText = "Subscribe failed: \(error.localizedDescription)"
            return
        }
        isConnected = true
        statusText = "Connected to \(peripheral.name ?? peripheral.identifier.uuidString)"
        onReady?()
    }

    func peripheral(_ peripheral: CBPeripheral,
                    didUpdateValueFor characteristic: CBCharacteristic,
                    error: Error?) {
        guard characteristic.uuid == Self.messageUUID,
              error == nil,
              let data = characteristic.value,
              let text = String(data: data, encoding: .utf8) else { return }
        onNotify?(text)
    }

    func peripheral(_ peripheral: CBPeripheral,
                    didWriteValueFor characteristic: CBCharacteristic,
                    error: Error?) {
        if let error = error {
            statusText = "Write failed: \(error.localizedDescription)"
        }
    }
}
