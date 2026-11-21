// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QString>
#include <QFrame>
#include <QVBoxLayout>

namespace simvis {

// ============================================================================
// Shared visual style for info-panel content (trip cards, stop lists, etc.)
// Keep every panel-facing module on these constants so the app looks uniform.
// ============================================================================
namespace panelstyle {

// Accent (selection / active item)
inline constexpr const char* kAccent        = "#1E88E5";
inline constexpr const char* kAccentBgTint  = "rgba(30,136,229,0.12)";

// Neutral card
inline constexpr const char* kCardBg        = "rgba(128,128,128,0.10)";
inline constexpr const char* kCardBorder    = "rgba(128,128,128,0.25)";

// Button (yellow, matches the person-route overlay color)
inline constexpr const char* kButtonStyle =
    "QPushButton { padding: 7px 12px; border: none; border-radius: 6px; "
    "color: #333; font-weight: bold; font-size: 10.5pt; "
    "background-color: #FFE082; }"
    "QPushButton:hover { background-color: #FDD835; }"
    "QPushButton:pressed { background-color: #FBC02D; }";
inline constexpr const char* kButtonStyleOn =
    "QPushButton { padding: 7px 12px; border: none; border-radius: 6px; "
    "color: #333; font-weight: bold; font-size: 10.5pt; "
    "background-color: #FDD835; }"
    "QPushButton:hover { background-color: #FBC02D; }"
    "QPushButton:pressed { background-color: #F9A825; }";

// Card container stylesheet (objectName must be "card")
inline QString cardStyle(bool selected) {
    return selected
        ? QString("#card { background-color: %1; border: 1px solid %2; "
                  "border-radius: 6px; }").arg(kAccentBgTint, kAccent)
        : QString("#card { background-color: %1; border: 1px solid %2; "
                  "border-radius: 6px; }").arg(kCardBg, kCardBorder);
}

// Primary text inside a card (bold, accent when selected)
inline QString cardTitleStyle(bool selected) {
    return QString("font-size: 11pt; font-weight: bold; color: %1; "
                   "background: transparent; border: none;")
        .arg(selected ? kAccent : "palette(text)");
}

// Secondary/detail text inside a card
inline QString cardDetailStyle(bool selected) {
    return QString("font-size: 9.5pt; color: %1; background: transparent; "
                   "border: none;")
        .arg(selected ? kAccent : "gray");
}

// Mode/tag pill badge (accent-filled when selected, gray otherwise)
inline QString pillStyle(bool selected) {
    return selected
        ? QString("font-size: 10.5pt; color: white; background-color: %1; "
                  "border: none; border-radius: 12px; padding: 4px 14px;")
              .arg(kAccent)
        : QString("font-size: 10.5pt; color: palette(text); "
                  "background-color: rgba(128,128,128,0.25); "
                  "border: none; border-radius: 12px; padding: 4px 14px;");
}

// Create a card frame + its vertical layout, inserted before the trailing
// stretch of `parentLayout`. Caller owns nothing; Qt parent tree does.
inline QFrame* makeCard(QWidget* parentWidget, QVBoxLayout* parentLayout,
                        bool selected) {
    auto* card = new QFrame(parentWidget);
    card->setObjectName("card");
    card->setStyleSheet(cardStyle(selected));
    parentLayout->insertWidget(parentLayout->count() - 1, card);
    return card;
}

// Emoji icon for a MATSim activity type (matched loosely, case-insensitive).
// Shared by the info panel trip cards and the on-map activity markers.
inline QString actIcon(const QString& act) {
    const QString a = act.toLower();
    if (a.contains("home")) return QString::fromUtf8("\U0001F3E0");      // house
    if (a.contains("work")) return QString::fromUtf8("\U0001F4BC");      // briefcase
    if (a.contains("shop")) return QString::fromUtf8("\U0001F6D2");      // cart
    if (a.contains("edu") || a.contains("school") || a.contains("uni"))
        return QString::fromUtf8("\U0001F393");                          // grad cap
    if (a.contains("leisure") || a.contains("restaurant") || a.contains("eat"))
        return QString::fromUtf8("\U0001F37D");                          // plate
    if (a.contains("pt interaction")) return QString::fromUtf8("\U0001F68F"); // bus stop
    if (a.isEmpty()) return QString();
    return QString::fromUtf8("\U0001F4CD");                              // pin
}

// Emoji icon for a MATSim leg mode
inline QString modeIcon(const QString& mode) {
    const QString m = mode.toLower();
    if (m == "car" || m.contains("car")) return QString::fromUtf8("\U0001F697");
    if (m.contains("walk")) return QString::fromUtf8("\U0001F6B6");
    if (m.contains("bike") || m.contains("bicycle")) return QString::fromUtf8("\U0001F6B2");
    if (m.contains("tram")) return QString::fromUtf8("\U0001F68B");
    if (m.contains("rail") || m.contains("train")) return QString::fromUtf8("\U0001F686");
    if (m == "pt" || m.contains("bus") || m.contains("transit"))
        return QString::fromUtf8("\U0001F68C");
    return QString::fromUtf8("\U000027A1");                              // arrow
}

} // namespace panelstyle
} // namespace simvis
