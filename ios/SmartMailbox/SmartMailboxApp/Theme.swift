//
//  Theme.swift
//  SmartMailboxApp
//
//  Same three-tier elevation palette as tools/mailbox_gui.py's dark theme:
//  window is darkest, card panels a shade lighter, interactive fields
//  lighter still. Kept as one shared source of truth so every view pulls
//  from the same colors instead of hardcoding hex values inline.
//

import SwiftUI

enum Theme {
    static let background = Color(red: 0x1c / 255.0, green: 0x1c / 255.0, blue: 0x1e / 255.0)
    static let card = Color(red: 0x2c / 255.0, green: 0x2c / 255.0, blue: 0x2e / 255.0)
    static let field = Color(red: 0x3a / 255.0, green: 0x3a / 255.0, blue: 0x3c / 255.0)
    static let text = Color(red: 0xf2 / 255.0, green: 0xf2 / 255.0, blue: 0xf7 / 255.0)
    static let mutedText = Color(red: 0x98 / 255.0, green: 0x98 / 255.0, blue: 0x9d / 255.0)
    static let border = Color(red: 0x48 / 255.0, green: 0x48 / 255.0, blue: 0x4a / 255.0)
    static let accent = Color(red: 0x0a / 255.0, green: 0x84 / 255.0, blue: 0xff / 255.0)
    static let errorColor = Color(red: 0xff / 255.0, green: 0x45 / 255.0, blue: 0x3a / 255.0)
    static let okColor = Color(red: 0x30 / 255.0, green: 0xd1 / 255.0, blue: 0x58 / 255.0)
}

/// Matches the desktop app's button styling: field-colored, accent on press.
struct MailboxButtonStyle: ButtonStyle {
    func makeBody(configuration: Configuration) -> some View {
        configuration.label
            .font(.subheadline)
            .padding(.horizontal, 12)
            .padding(.vertical, 8)
            .background(configuration.isPressed ? Theme.accent : Theme.field)
            .foregroundColor(Theme.text)
            .cornerRadius(8)
    }
}

/// A card-style section container, matching the desktop app's LabelFrame
/// panels (title + card-colored background + rounded corners).
struct CardView<Content: View>: View {
    let title: String
    @ViewBuilder var content: Content

    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            Text(title)
                .font(.caption.bold())
                .foregroundColor(Theme.mutedText)
            content
        }
        .padding()
        .frame(maxWidth: .infinity, alignment: .leading)
        .background(Theme.card)
        .cornerRadius(12)
    }
}
