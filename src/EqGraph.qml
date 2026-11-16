import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material

// The equalizer drawn over the live spectrum: what apps send in (outline),
// what the speakers play (filled), the speaker optimizer (dashed) and the
// ten-band EQ you drag (solid, with handles).
Item {
    id: graph
    property var outputBands: []
    property var inputBands: []
    property var curve: []
    property var tuningCurve: []
    property var gains: []
    property var freqs: []
    property bool showInput: false
    property color fg: theme.foreground
    property color accent: theme.accent
    property color bg: theme.background
    signal bandChanged(int band, real db)

    readonly property real minDb: -15
    readonly property real maxDb: 15
    readonly property int padLeft: 40
    readonly property int padBottom: 24
    readonly property int padTop: 10
    readonly property int padRight: 10
    readonly property real plotW: width - padLeft - padRight
    readonly property real plotH: height - padTop - padBottom
    property int hoverBand: -1
    property int dragBand: -1

    function tint(c, a) { return Qt.rgba(c.r, c.g, c.b, a) }
    function xOf(f) { return padLeft + plotW * Math.log(f / 20) / Math.log(1000) }
    function fOfX(x) { return 20 * Math.pow(1000, (x - padLeft) / plotW) }
    function yOf(db) { return padTop + plotH * (maxDb - db) / (maxDb - minDb) }
    function dbOfY(y) { return maxDb - (y - padTop) / plotH * (maxDb - minDb) }
    function bandHz(b, count) { return 30 * Math.pow(20000 / 30, (b + 0.5) / count) }
    function hzLabel(f) {
        return f >= 1000 ? qsTr("%1 kHz").arg((f / 1000).toLocaleString(Qt.locale(), 'f', f % 1000 === 0 ? 0 : 1))
                         : qsTr("%1 Hz").arg(Math.round(f).toLocaleString(Qt.locale(), 'f', 0))
    }
    function nearestBand(x) {
        let best = -1, bestD = 1e9
        for (let i = 0; i < freqs.length; ++i) {
            const d = Math.abs(xOf(freqs[i]) - x)
            if (d < bestD) { bestD = d; best = i }
        }
        return bestD < Math.max(28, plotW / 20) ? best : -1
    }

    onOutputBandsChanged: canvas.requestPaint()
    onCurveChanged: canvas.requestPaint()
    onTuningCurveChanged: canvas.requestPaint()
    onHoverBandChanged: canvas.requestPaint()
    onDragBandChanged: canvas.requestPaint()
    onWidthChanged: canvas.requestPaint()
    onHeightChanged: canvas.requestPaint()
    onFgChanged: canvas.requestPaint()
    onAccentChanged: canvas.requestPaint()

    Canvas {
        id: canvas
        anchors.fill: parent
        renderStrategy: Canvas.Cooperative

        onPaint: {
            const ctx = getContext("2d")
            ctx.reset()
            const W = width, H = height
            const fg = graph.fg, ac = graph.accent
            const x0 = graph.padLeft, x1 = graph.padLeft + graph.plotW
            const y0 = graph.padTop, y1 = graph.padTop + graph.plotH

            // Grid.
            ctx.lineWidth = 1
            ctx.font = "11px sans-serif"
            ctx.fillStyle = graph.tint(fg, 0.4)
            ctx.textAlign = "center"
            const marks = [50, 100, 200, 500, 1000, 2000, 5000, 10000]
            for (const f of marks) {
                const x = Math.round(graph.xOf(f)) + 0.5
                ctx.strokeStyle = graph.tint(fg, 0.06)
                ctx.beginPath(); ctx.moveTo(x, y0); ctx.lineTo(x, y1); ctx.stroke()
                ctx.fillText(f >= 1000 ? (f / 1000) + "k" : f, x, H - 7)
            }
            ctx.textAlign = "right"
            for (let db = -12; db <= 12; db += 6) {
                const y = Math.round(graph.yOf(db)) + 0.5
                ctx.strokeStyle = graph.tint(fg, db === 0 ? 0.16 : 0.06)
                ctx.beginPath(); ctx.moveTo(x0, y); ctx.lineTo(x1, y); ctx.stroke()
                ctx.fillText((db > 0 ? "+" : "") + db, x0 - 8, y + 4)
            }

            // Spectrum of what the speakers play, as soft bars.
            const ob = graph.outputBands
            if (ob && ob.length) {
                const n = ob.length
                const grad = ctx.createLinearGradient(0, y0, 0, y1)
                grad.addColorStop(0, graph.tint(ac, 0.55))
                grad.addColorStop(1, graph.tint(ac, 0.06))
                ctx.fillStyle = grad
                for (let b = 0; b < n; ++b) {
                    const xa = graph.xOf(30 * Math.pow(20000 / 30, b / n))
                    const xb = graph.xOf(30 * Math.pow(20000 / 30, (b + 1) / n))
                    const h = ob[b] * graph.plotH * 0.96
                    if (h < 1) continue
                    ctx.fillRect(xa + 1, y1 - h, Math.max(1, xb - xa - 2), h)
                }
            }

            // What apps send in, before processing.
            const ib = graph.inputBands
            if (graph.showInput && ib && ib.length) {
                ctx.strokeStyle = graph.tint(fg, 0.45)
                ctx.lineWidth = 1.25
                ctx.beginPath()
                for (let b = 0; b < ib.length; ++b) {
                    const x = graph.xOf(graph.bandHz(b, ib.length))
                    const y = y1 - ib[b] * graph.plotH * 0.96
                    if (b === 0) ctx.moveTo(x, y); else ctx.lineTo(x, y)
                }
                ctx.stroke()
            }

            const points = graph.curve.length
            const xAt = (i, count) => x0 + graph.plotW * i / (count - 1)

            // Speaker optimizer.
            const tc = graph.tuningCurve
            if (tc && tc.length) {
                ctx.save()
                ctx.setLineDash([4, 4])
                ctx.strokeStyle = graph.tint(fg, 0.35)
                ctx.lineWidth = 1.25
                ctx.beginPath()
                for (let i = 0; i < tc.length; ++i) {
                    const y = graph.yOf(Math.max(graph.minDb, tc[i]))
                    if (i === 0) ctx.moveTo(xAt(i, tc.length), y); else ctx.lineTo(xAt(i, tc.length), y)
                }
                ctx.stroke()
                ctx.restore()
            }

            // The EQ curve, shaded toward 0 dB.
            if (points > 1) {
                const zero = graph.yOf(0)
                ctx.beginPath()
                ctx.moveTo(x0, zero)
                for (let i = 0; i < points; ++i)
                    ctx.lineTo(xAt(i, points), graph.yOf(graph.curve[i]))
                ctx.lineTo(x1, zero)
                ctx.closePath()
                ctx.fillStyle = graph.tint(ac, 0.12)
                ctx.fill()

                ctx.beginPath()
                for (let i = 0; i < points; ++i) {
                    const y = graph.yOf(graph.curve[i])
                    if (i === 0) ctx.moveTo(x0, y); else ctx.lineTo(xAt(i, points), y)
                }
                ctx.strokeStyle = ac
                ctx.lineWidth = 2.5
                ctx.stroke()
            }

            // Handles.
            for (let i = 0; i < graph.freqs.length; ++i) {
                const x = graph.xOf(graph.freqs[i]), y = graph.yOf(graph.gains[i] || 0)
                const active = i === graph.dragBand || i === graph.hoverBand
                ctx.beginPath()
                ctx.arc(x, y, active ? 8 : 5.5, 0, Math.PI * 2)
                ctx.fillStyle = active ? ac : graph.bg
                ctx.fill()
                ctx.lineWidth = 2
                ctx.strokeStyle = ac
                ctx.stroke()
            }
        }
    }

    // The value of the band being touched.
    Rectangle {
        readonly property int band: graph.dragBand >= 0 ? graph.dragBand : graph.hoverBand
        visible: band >= 0
        x: Math.min(graph.width - width - 4, Math.max(graph.padLeft, graph.xOf(graph.freqs[band] || 20) - width / 2))
        y: Math.max(0, graph.yOf(graph.gains[band] || 0) - height - 14)
        width: tip.implicitWidth + 16
        height: tip.implicitHeight + 8
        radius: Math.min(4, theme.radius)
        color: graph.tint(graph.fg, 0.92)
        Label {
            id: tip
            anchors.centerIn: parent
            readonly property real db: graph.gains[parent.band] || 0
            text: qsTr("%1 · %2 dB").arg(graph.hzLabel(graph.freqs[parent.band] || 0))
                                     .arg((parent.band >= 0 && tip.db > 0 ? "+" : "") + tip.db.toLocaleString(Qt.locale(), 'f', 1))
            color: graph.bg
            font.pixelSize: 12
            font.weight: Font.DemiBold
        }
    }

    MouseArea {
        anchors.fill: parent
        hoverEnabled: true
        preventStealing: true
        cursorShape: graph.hoverBand >= 0 || graph.dragBand >= 0 ? Qt.SizeVerCursor : Qt.ArrowCursor
        onPositionChanged: (mouse) => {
            if (graph.dragBand >= 0)
                graph.bandChanged(graph.dragBand, Math.max(-12, Math.min(12, graph.dbOfY(mouse.y))))
            else
                graph.hoverBand = graph.nearestBand(mouse.x)
        }
        onPressed: (mouse) => {
            graph.dragBand = graph.nearestBand(mouse.x)
            if (graph.dragBand >= 0)
                graph.bandChanged(graph.dragBand, Math.max(-12, Math.min(12, graph.dbOfY(mouse.y))))
        }
        onReleased: graph.dragBand = -1
        onExited: graph.hoverBand = -1
        onDoubleClicked: (mouse) => {
            const b = graph.nearestBand(mouse.x)
            if (b >= 0) graph.bandChanged(b, 0)
        }
        onWheel: (wheel) => {
            const b = graph.nearestBand(wheel.x)
            if (b >= 0) graph.bandChanged(b, (graph.gains[b] || 0) + (wheel.angleDelta.y > 0 ? 0.5 : -0.5))
        }
    }
}
