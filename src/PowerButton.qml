import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material

// The big on/off button; glows in the accent while the engine runs.
AbstractButton {
    id: power
    property bool on: false
    property bool busy: false
    property color fg: theme.foreground
    property color accent: theme.accent
    function tint(c, a) { return Qt.rgba(c.r, c.g, c.b, a) }

    implicitWidth: 48
    implicitHeight: 48
    hoverEnabled: true
    focusPolicy: Qt.NoFocus

    background: Item {
        Rectangle {
            anchors.centerIn: parent
            width: parent.width + 10
            height: width
            radius: width / 2
            color: "transparent"
            border.width: 2
            border.color: tint(power.accent, power.on ? 0.35 : 0)
            Behavior on border.color { ColorAnimation { duration: 200 } }
        }
        Rectangle {
            anchors.fill: parent
            radius: width / 2
            color: power.on ? power.accent : power.hovered ? tint(power.fg, 0.12) : tint(power.fg, 0.07)
            Behavior on color { ColorAnimation { duration: 200 } }
        }
    }
    contentItem: Item {
        AppIcon {
            anchors.centerIn: parent
            width: 22
            height: 22
            name: "power"
            stroke: 2.4
            color: power.on ? theme.accentForeground : tint(power.fg, 0.75)
            opacity: power.busy ? 0.4 : 1
        }
        BusyIndicator {
            anchors.centerIn: parent
            width: 54
            height: 54
            running: power.busy
            visible: power.busy
        }
    }
}
