import QtQuick
import QtQuick.Controls

// A pill button, as used for the equalizer presets.
AbstractButton {
    id: chip
    property bool current: false
    // Tighter padding for narrow windows.
    property bool compact: false

    readonly property color fg: theme.foreground
    function tint(c, a) { return Qt.rgba(c.r, c.g, c.b, a) }

    leftPadding: compact ? 8 : 12
    rightPadding: leftPadding
    implicitWidth: chipLabel.implicitWidth + leftPadding + rightPadding
    implicitHeight: 30
    hoverEnabled: true
    focusPolicy: Qt.NoFocus

    background: Rectangle {
        radius: Math.min(15, theme.radius)
        color: chip.current ? theme.accent : chip.hovered ? chip.tint(chip.fg, 0.08) : "transparent"
        border.color: chip.current ? theme.accent : chip.tint(chip.fg, 0.14)
    }
    contentItem: Label {
        id: chipLabel
        text: chip.text
        horizontalAlignment: Text.AlignHCenter
        verticalAlignment: Text.AlignVCenter
        font.pixelSize: 12
        font.weight: chip.current ? Font.DemiBold : Font.Normal
        color: chip.current ? theme.accentForeground : chip.tint(chip.fg, 0.8)
    }
}
