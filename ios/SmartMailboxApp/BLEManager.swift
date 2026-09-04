//
//  BLEManager.swift
//  SmartMailboxApp
//
//  CoreBluetooth central for the board's custom command/notify
//  characteristic (see Smart_Mailbox.ino's setupBLE()). Mirrors
//  tools/mailbox_gui.py's BLEWorker: connect() scans and connects,
//  send() writes a command string, and onNotify/onReady callbacks report
//  results back to the app's state (AppState) - the SwiftUI/Combine
//  equivalent of the Python version's event-queue handoff to the Tk
//  thread, except CoreBluetooth already delivers its delegate callbacks
//  on the queue we hand it (set to .main below), so no extra queue
//  hand-off is needed here.
//
//  Pairing: the command characteristic requires an encrypted link
//  (PROPERTY_READ_ENC/PROPERTY_WRITE_ENC in the firmware, when
//  ENABLE_BLE_PAIRING is on). iOS's CoreBluetooth stack handles the
//  "Bluetooth Pairing Request" system prompt automatically the first
//  time a read/write needs it - no app code required here.
//

import Foundation
import CoreBluetooth

final class BLEManager: NSObject, ObservableObject {
    // Must match SERVICE_UUID/MESSAGE_UUID in Smart_Mailbox.ino exactly.
    static let serviceUUID = CBUUID(string: "ab0828b1-198e-4351-b779-901fa0e0371e")
    static let messageUUID = CBUUID(string: "4ac8a682-9736-4e5d-932b-e9b31405049c")
    static let deviceName = "Smart_Mailbox"

    @Published var statusText: String = "Disconnected"
    @Published var isConnected: Bool = false

    /// Called with each raw notify string (e.g. "led=1") as it arrives.
    var onNotify: ((String) -> Void)?
    /// Called once the connection is fully usable (subscribed to notifications).
    var onReady: (() -> Void)?

    private var centralManager: CBCentralManager!
    private var peripheral: CBPeripheral?
    private var commandCharacteristic: CBCharacteristic?
    // Set when connect() is called before the radio is ready (e.g. right at
    // launch, while CBCentralManager is still resolving the Bluetooth
    // permission prompt and its state is still .unknown/.resetting rather
    // than .poweredOn). centralManagerDidUpdateState retries automatically
    // once the state actually becomes .poweredOn, so a Connect tap made too
    // early isn't silently dropped.
    private var connectPending = false

    override init() {
        super.init()
        centralManager = CBCentralManager(delegate: self, queue: .main)
    }

    func connect() {
        guard centralManager.state == .poweredOn else {
            connectPending = true
            statusText = "Waiting for Bluetooth..."
            return
        }
        startScan()
    }

    private func startScan() {
        connectPending = false
        if let existing = peripheral {
            centralManager.cancelPeripheralConnection(existing)
        }
        statusText = "Scanning for \(Self.deviceName)..."
        centralManager.scanForPeripherals(withServices: [Self.serviceUUID], options: nil)
    }

    func disconnect() {
        centralManager.stopScan()
        if let p = peripheral {
            centralManager.cancelPeripheralConnection(p)
        } else {
            statusText = "Disconnected"
        }
    }

    /// Write a command string (e.g. "led=on") to the board. No-ops with a
    /// "Not connected" status if there's no live, ready connection.
    func send(_ cmd: String) {
        guard isConnected, let characteristic = commandCharacteristic, let p = peripheral else {
            statusText = "Not connected"
            return
        }
        guard let data = cmd.data(using: .utf8) else { return }
        p.writeValue(data, for: characteristic, type: .withResponse)
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
            statusText = "Bluetooth unavailable"
            isConnected = false
        }
    }

    func centralManager(_ central: CBCentralManager, didDiscover peripheral: CBPeripheral, advertisementData: [String: Any], rssi RSSI: NSNumber) {
        // First matching advertiser wins - same as the Python tools'
        // find_device_by_name(), which also just takes the first hit.
        central.stopScan()
        self.peripheral = peripheral
        peripheral.delegate = self
        statusText = "Connecting..."
        central.connect(peripheral, options: nil)
    }

    func centralManager(_ central: CBCentralManager, didConnect peripheral: CBPeripheral) {
        statusText = "Discovering services..."
        peripheral.discoverServices([Self.serviceUUID])
    }

    func centralManager(_ central: CBCentralManager, didFailToConnect peripheral: CBPeripheral, error: Error?) {
        statusText = "Connect failed: \(error?.localizedDescription ?? "unknown error")"
    }

    func centralManager(_ central: CBCentralManager, didDisconnectPeripheral peripheral: CBPeripheral, error: Error?) {
        isConnected = false
        commandCharacteristic = nil
        self.peripheral = nil
        statusText = error != nil ? "Disconnected: \(error!.localizedDescription)" : "Disconnected"
    }
}

// MARK: - CBPeripheralDelegate

extension BLEManager: CBPeripheralDelegate {
    func peripheral(_ peripheral: CBPeripheral, didDiscoverServices error: Error?) {
        if let error = error {
            statusText = "Service discovery failed: \(error.localizedDescription)"
            return
        }
        guard let services = peripheral.services else { return }
        for service in services where service.uuid == Self.serviceUUID {
            peripheral.discoverCharacteristics([Self.messageUUID], for: service)
        }
    }

    func peripheral(_ peripheral: CBPeripheral, didDiscoverCharacteristicsFor service: CBService, error: Error?) {
        if let error = error {
            statusText = "Characteristic discovery failed: \(error.localizedDescription)"
            return
        }
        guard let characteristics = service.characteristics else { return }
        for characteristic in characteristics where characteristic.uuid == Self.messageUUID {
            commandCharacteristic = characteristic
            // Enabling notifications is itself a write to the descriptor;
            // since the characteristic requires encryption, this is what
            // typically triggers iOS's pairing prompt on first connect.
            peripheral.setNotifyValue(true, for: characteristic)
        }
    }

    func peripheral(_ peripheral: CBPeripheral, didUpdateNotificationStateFor characteristic: CBCharacteristic, error: Error?) {
        guard characteristic.uuid == Self.messageUUID else { return }
        if let error = error {
            statusText = "Subscribe failed: \(error.localizedDescription)"
            return
        }
        isConnected = true
        statusText = "Connected to \(peripheral.name ?? peripheral.identifier.uuidString)"
        onReady?()
    }

    func peripheral(_ peripheral: CBPeripheral, didUpdateValueFor characteristic: CBCharacteristic, error: Error?) {
        guard characteristic.uuid == Self.messageUUID, error == nil,
              let data = characteristic.value,
              let text = String(data: data, encoding: .utf8) else { return }
        onNotify?(text)
    }

    func peripheral(_ peripheral: CBPeripheral, didWriteValueFor characteristic: CBCharacteristic, error: Error?) {
        if let error = error {
            statusText = "Write failed: \(error.localizedDescription)"
        }
    }
}
