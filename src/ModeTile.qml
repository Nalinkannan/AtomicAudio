import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// One of the sound modes along the top, like Dolby Access's.
AbstractButton {
    id: tile
    property string glyph
    property string title
    property string subtitle
    property string shortcutKey
    property bool selected: false
    property color fg: theme.foreground
    property color accent: theme.accent

    function tint(c, a) { return Qt.rgba(c.r, c.g, c.b, a) }

    implicitHeight: 92
    hoverEnabled: true
    focusPolicy: Qt.NoFocus

    background: Rectangle {
        radius: theme.radius
        color: tile.selected ? tint(tile.accent, 0.16) : tile.hovered ? tint(tile.fg, 0.07) : tint(tile.fg, 0.035)
        border.width: tile.selected ? 1.5 : 1
        border.color: tile.selected ? tile.accent : tint(tile.fg, 0.08)
        Behavior on color { ColorAnimation { duration: 140 } }
    }

    contentItem: Item {
        AppIcon {
            id: glyphIcon
            x: 14
            y: 14
            width: 24
            height: 24
            name: tile.glyph
            color: tile.selected ? tile.accent : tint(tile.fg, 0.75)
        }
        Label {
            anchors.right: parent.right
            anchors.rightMargin: 12
            y: 12
            text: tile.shortcutKey
            font.pixelSize: 11
            color: tint(tile.fg, 0.35)
        }
        ColumnLayout {
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            anchors.margins: 14
            spacing: 1
            Label {
                text: tile.title
                font.pixelSize: 15
                font.weight: Font.DemiBold
                color: tile.selected ? tile.accent : tile.fg
                elide: Text.ElideRight
                Layout.fillWidth: true
            }
            Label {
                text: tile.subtitle
                font.pixelSize: 11
                color: tint(tile.fg, 0.55)
                elide: Text.ElideRight
                Layout.fillWidth: true
            }
        }
    }
}
