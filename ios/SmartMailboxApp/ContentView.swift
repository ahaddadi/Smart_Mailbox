//
//  ContentView.swift
//  SmartMailboxApp
//
//  Mirrors tools/mailbox_gui.py's layout: a status/connect row, then
//  CONTROLS (LED/relay), WIFI (scan/picker/credentials/connect), CAMERA
//  (stream/record/video), and LOG cards, all on the same dark palette
//  (see Theme.swift).
//

import SwiftUI

struct ContentView: View {
    @StateObject private var state = AppState()
    @State private var ssid = ""
    @State private var password = ""

    var body: some View {
        ScrollView {
            VStack(spacing: 16) {
                statusSection
                controlsSection
                wifiSection
                cameraSection
                logSection
            }
            .padding()
        }
        .background(Theme.background.ignoresSafeArea())
        .preferredColorScheme(.dark)
    }

    private var statusSection: some View {
        VStack(spacing: 8) {
            Text(state.statusText)
                .foregroundColor(Theme.mutedText)
                .font(.footnote)
                .multilineTextAlignment(.center)
            HStack(spacing: 10) {
                Button("Connect") { state.connectBLE() }
                Button("Disconnect") { state.disconnectBLE() }
                Button("Refresh") { state.refreshStatus() }
            }
            .buttonStyle(MailboxButtonStyle())
        }
    }

    private var controlsSection: some View {
        CardView(title: "CONTROLS") {
            VStack(spacing: 10) {
                HStack {
                    Text("LED: \(stateText(state.ledOn))").foregroundColor(Theme.text)
                    Spacer()
                    Button("On") { state.httpCommand("led=on") }
                    Button("Off") { state.httpCommand("led=off") }
                }
                .buttonStyle(MailboxButtonStyle())

                HStack {
                    Text("Relay: \(stateText(state.relayOn))").foregroundColor(Theme.text)
                    Spacer()
                    Button("On") { state.httpCommand("relay=on") }
                    Button("Off") { state.httpCommand("relay=off") }
                }
                .buttonStyle(MailboxButtonStyle())
            }
        }
    }

    private var wifiSection: some View {
        CardView(title: "WIFI") {
            VStack(alignment: .leading, spacing: 10) {
                HStack {
                    Button("Scan Networks") { state.wifiScan() }
                        .buttonStyle(MailboxButtonStyle())
                    Text(state.scanStatus)
                        .foregroundColor(Theme.mutedText)
                        .font(.footnote)
                }

                if !state.networks.isEmpty {
                    ScrollView {
                        VStack(alignment: .leading, spacing: 0) {
                            ForEach(state.networks, id: \.self) { network in
                                Button {
                                    ssid = network
                                } label: {
                                    Text(network)
                                        .foregroundColor(Theme.text)
                                        .frame(maxWidth: .infinity, alignment: .leading)
                                        .padding(8)
                                }
                            }
                        }
                    }
                    .frame(maxHeight: 140)
                    .background(Theme.field)
                    .cornerRadius(8)
                }

                styledField(placeholder: "Network Name", text: $ssid, secure: false)
                styledField(placeholder: "Password", text: $password, secure: true)

                HStack {
                    Button("Connect") { state.wifiConnect(ssid: ssid, password: password) }
                    Button("Disconnect") { state.wifiDisconnect() }
                }
                .buttonStyle(MailboxButtonStyle())

                Text("WiFi: \(state.wifiState)")
                    .foregroundColor(Theme.text)
                    .font(.footnote)
            }
        }
    }

    private var cameraSection: some View {
        CardView(title: "CAMERA") {
            VStack(spacing: 10) {
                HStack {
                    Button("Stream On") { state.httpCommand("stream=on") }
                    Button("Stream Off") { state.streamOff() }
                }
                .buttonStyle(MailboxButtonStyle())

                HStack {
                    Button(state.isRecording ? "Stop Recording" : "Start Recording") {
                        state.toggleRecording()
                    }
                    .buttonStyle(MailboxButtonStyle())
                    if state.isRecording {
                        Text("● REC").foregroundColor(Theme.errorColor).font(.footnote)
                    }
                }

                ZStack {
                    Theme.field
                    if let image = state.currentFrame {
                        Image(uiImage: image)
                            .resizable()
                            .aspectRatio(contentMode: .fit)
                    } else {
                        Text("(no stream)").foregroundColor(Theme.mutedText)
                    }
                }
                .frame(height: 260)
                .cornerRadius(8)
                .clipped()
            }
        }
    }

    private var logSection: some View {
        CardView(title: "LOG") {
            ScrollViewReader { proxy in
                ScrollView {
                    VStack(alignment: .leading, spacing: 2) {
                        ForEach(state.logs) { entry in
                            Text(entry.text)
                                .font(.system(.footnote, design: .monospaced))
                                .foregroundColor(color(for: entry.tag))
                                .id(entry.id)
                        }
                    }
                    .frame(maxWidth: .infinity, alignment: .leading)
                }
                .frame(height: 180)
                .onChange(of: state.logs.count) { _ in
                    if let last = state.logs.last {
                        withAnimation {
                            proxy.scrollTo(last.id, anchor: .bottom)
                        }
                    }
                }
            }
        }
    }

    // MARK: - helpers

    @ViewBuilder
    private func styledField(placeholder: String, text: Binding<String>, secure: Bool) -> some View {
        Group {
            if secure {
                SecureField(placeholder, text: text)
            } else {
                TextField(placeholder, text: text)
                    .textInputAutocapitalization(.never)
                    .autocorrectionDisabled()
            }
        }
        .padding(8)
        .background(Theme.field)
        .foregroundColor(Theme.text)
        .cornerRadius(8)
    }

    private func stateText(_ value: Bool?) -> String {
        guard let value else { return "?" }
        return value ? "On" : "Off"
    }

    private func color(for tag: LogTag) -> Color {
        switch tag {
        case .ok: return Theme.okColor
        case .error: return Theme.errorColor
        case .normal: return Theme.text
        }
    }
}

#Preview {
    ContentView()
}
