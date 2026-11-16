import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// An effect with its amount: icon, name, what it does, and a slider (or a
// switch for the on/off ones).
Rectangle {
    id: row
    property string key
    property string icon
    property string title
    property string subtitle
    property real value: 0
    property bool toggle: false
    property bool checked: false
    property bool selected: false
    property bool available: true
    property color fg: theme.foreground
    property color accent: theme.accent
    signal moved(real value)
    signal toggled(bool on)
    signal picked()

    function tint(c, a) { return Qt.rgba(c.r, c.g, c.b, a) }

    Material.roundedScale: theme.radius
    implicitHeight: toggle ? 60 : 76
    radius: Math.min(6, theme.radius)
    color: selected ? tint(fg, 0.07) : "transparent"
    border.width: selected ? 1 : 0
    border.color: tint(accent, 0.45)
    opacity: available ? 1 : 0.45

    MouseArea {
        anchors.fill: parent
        onPressed: (mouse) => { row.picked(); mouse.accepted = false }
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.leftMargin: 12
        anchors.rightMargin: 12
        anchors.topMargin: 10
        anchors.bottomMargin: row.toggle ? 10 : 2
        spacing: 0

        RowLayout {
            Layout.fillWidth: true
            spacing: 12
            AppIcon {
                name: row.icon
                width: 20
                height: 20
                color: row.selected ? row.accent : tint(row.fg, 0.7)
                Layout.alignment: Qt.AlignTop
                Layout.topMargin: 2
            }
            ColumnLayout {
                Layout.fillWidth: true
                spacing: 1
                Label {
                    text: row.title
                    font.pixelSize: 14
                    font.weight: Font.Medium
                    color: row.fg
                    elide: Text.ElideRight
                    Layout.fillWidth: true
                }
                Label {
                    text: row.subtitle
                    font.pixelSize: 11
                    color: tint(row.fg, 0.55)
                    elide: Text.ElideRight
                    Layout.fillWidth: true
                }
            }
            Label {
                visible: !row.toggle
                text: Math.round(row.value * 100).toLocaleString(Qt.locale(), 'f', 0) + "%"
                font.pixelSize: 14
                font.weight: Font.DemiBold
                font.features: { "tnum": 1 }
                color: row.value > 0 ? row.accent : tint(row.fg, 0.4)
                Layout.alignment: Qt.AlignTop
            }
            Switch {
                visible: row.toggle
                enabled: row.available
                checked: row.checked
                focusPolicy: Qt.NoFocus
                onToggled: row.toggled(checked)
                Layout.alignment: Qt.AlignVCenter
            }
        }
        Slider {
            visible: !row.toggle
            enabled: row.available
            from: 0
            to: 1
            value: row.value
            stepSize: 0.01
            focusPolicy: Qt.NoFocus
            onMoved: row.moved(value)
            onPressedChanged: if (pressed) row.picked()
            Layout.fillWidth: true
            Layout.leftMargin: 26
        }
    }
}
