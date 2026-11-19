import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// Every shortcut, shown with ?.
Rectangle {
    id: overlay
    property color fg: theme.foreground
    signal closed()
    function tint(c, a) { return Qt.rgba(c.r, c.g, c.b, a) }

    color: Qt.rgba(0, 0, 0, 0.55)

    readonly property var rows: [
        ["Space", qsTr("Turn Atomic Audio on or off")],
        ["A", qsTr("Compare with the original sound")],
        ["1 – 6", qsTr("Pick a mode")],
        ["E", qsTr("Next equalizer preset")],
        ["↑ ↓", qsTr("Pick an effect")],
        ["← →", qsTr("Turn the effect down or up")],
        ["O", qsTr("Speaker optimizer on or off")],
        ["H", qsTr("Edit speakers, headphones or external speakers")],
        ["R", qsTr("Reset this output to the defaults")],
        ["Ctrl+Z", qsTr("Undo")],
        ["Ctrl+Shift+Z", qsTr("Redo")],
        ["?", qsTr("Show or hide shortcuts")],
        ["Q", qsTr("Quit (the sound stays on)")]
    ]

    MouseArea { anchors.fill: parent; onClicked: overlay.closed() }

    Rectangle {
        anchors.centerIn: parent
        width: Math.min(parent.width - 48, 520)
        height: content.implicitHeight + 48
        radius: theme.radius
        color: theme.background
        border.color: tint(overlay.fg, 0.12)

        ColumnLayout {
            id: content
            anchors.fill: parent
            anchors.margins: 24
            spacing: 10
            Label {
                text: qsTr("Keyboard shortcuts")
                font.pixelSize: 18
                font.weight: Font.DemiBold
                color: overlay.fg
                Layout.bottomMargin: 6
            }
            GridLayout {
                columns: 2
                columnSpacing: 24
                rowSpacing: 8
                Repeater {
                    model: overlay.rows.length * 2
                    delegate: Label {
                        readonly property var row: overlay.rows[Math.floor(index / 2)]
                        text: index % 2 === 0 ? row[0] : row[1]
                        color: index % 2 === 0 ? theme.accent : overlay.fg
                        font.pixelSize: 13
                        font.weight: index % 2 === 0 ? Font.DemiBold : Font.Normal
                        Layout.fillWidth: index % 2 === 1
                    }
                }
            }
        }
    }
}
