import QtQuick
import QtQuick.Shapes
import QtQuick.Window

// Line icons on a 24-unit grid, stroked in any color so they follow the theme.
Item {
    id: icon
    property string name
    property color color: "white"
    property real stroke: 2
    implicitWidth: 24
    implicitHeight: 24

    readonly property var paths: ({
        "dynamic": "M2 12h3l2.5-6 4 13 3.5-10 2 3h5",
        "movie": "M3 5h18v14H3z M7 5v14 M17 5v14 M3 9h4 M3 15h4 M17 9h4 M17 15h4",
        "music": "M9 17V5l11-2v12 M9 17a3 3 0 1 1-6 0a3 3 0 1 1 6 0 M20 15a3 3 0 1 1-6 0a3 3 0 1 1 6 0",
        "game": "M7 7h10a5 5 0 0 1 0 10h-1.5l-2-2h-3l-2 2H7a5 5 0 0 1 0-10z M7.5 10v4 M5.5 12h4 M15.5 11h.01 M17.5 13h.01",
        "voice": "M12 3a3 3 0 0 0-3 3v6a3 3 0 0 0 6 0V6a3 3 0 0 0-3-3z M19 11a7 7 0 0 1-14 0 M12 18v3",
        "custom": "M4 7h9 M17 7h3 M4 17h3 M11 17h9 M15 7a2 2 0 1 0 4 0a2 2 0 1 0-4 0 M7 17a2 2 0 1 0 4 0a2 2 0 1 0-4 0",
        "power": "M12 3v8 M6.3 6.3a8 8 0 1 0 11.4 0",
        "speakers": "M11 5L6 9H3v6h3l5 4z M15.5 8.5a5 5 0 0 1 0 7 M18.5 5.5a9 9 0 0 1 0 13",
        "headphones": "M3 18v-6a9 9 0 0 1 18 0v6 M3 15h3v6H5a2 2 0 0 1-2-2z M21 15h-3v6h1a2 2 0 0 0 2-2z",
        "surround": "M12 12h.01 M8.5 8.5a5 5 0 0 0 0 7 M15.5 8.5a5 5 0 0 1 0 7 M5.6 5.6a9 9 0 0 0 0 12.8 M18.4 5.6a9 9 0 0 1 0 12.8",
        "dialogue": "M4 5h16v11H9l-5 4z M8 9h8 M8 12h5",
        "bass": "M2 12c2.5-7 5-7 7.5 0s5 7 7.5 0 3.5-4 5-2",
        "leveler": "M5 20V10 M10 20V6 M15 20V10 M20 20V6 M3 14h19",
        "loudness": "M11 5L6 9H3v6h3l5 4z M15 9.5a3.5 3.5 0 0 1 0 5 M17.5 7a7 7 0 0 1 0 10 M20 4.5a10.5 10.5 0 0 1 0 15",
        "optimizer": "M12 3l1.9 4.6L18.5 9l-4.6 1.9L12 15.5l-1.9-4.6L5.5 9l4.6-1.4z M18 15l.8 2.2L21 18l-2.2.8L18 21l-.8-2.2L15 18l2.2-.8z",
        "compare": "M8 4v16 M4 8l4-4 4 4 M16 20V4 M12 16l4 4 4-4"
    })

    Shape {
        anchors.centerIn: parent
        width: 24
        height: 24
        scale: Math.min(icon.width, icon.height) / 24
        // A supersampled layer keeps the strokes smooth and, unlike the curve
        // renderer, respects the clipping of scrolled lists.
        layer.enabled: true
        layer.smooth: true
        layer.samples: 4
        layer.textureSize: Qt.size(Math.ceil(icon.width * 2 * Screen.devicePixelRatio), Math.ceil(icon.height * 2 * Screen.devicePixelRatio))
        ShapePath {
            strokeColor: icon.color
            strokeWidth: icon.stroke
            fillColor: "transparent"
            capStyle: ShapePath.RoundCap
            joinStyle: ShapePath.RoundJoin
            PathSvg { path: icon.paths[icon.name] || "" }
        }
    }
}
