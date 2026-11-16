import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

ApplicationWindow {
    id: win
    width: 1160
    height: 780
    minimumWidth: 900
    minimumHeight: 640
    visible: true
    title: "Atomic Audio"
    color: theme.background

    Material.theme: theme.dark ? Material.Dark : Material.Light
    Material.accent: theme.accent
    Material.foreground: theme.foreground
    Material.background: theme.background
    Material.roundedScale: theme.radius

    property color fg: theme.foreground
    property color accent: theme.accent
    // Real colors, not strings: tint() reads their channels.
    readonly property color warning: "#f59e0b"
    readonly property color danger: "#ef4444"
    property bool helpVisible: false
    property int selectedEffect: 0
    readonly property var effectKeys: ["surround", "dialogue", "bass", "leveler", "loudness", "tuning"]
    readonly property bool editingHeadphones: backend.editingHeadphones

    function tint(c, a) { return Qt.rgba(c.r, c.g, c.b, a) }
    function adjustSelected(direction) {
        const key = effectKeys[selectedEffect]
        if (key === "tuning") backend.setTuning(direction > 0)
        else backend.nudge(key, 0.05 * direction)
    }

    readonly property var modeInfo: ({
        "dynamic": { icon: "dynamic", title: qsTr("Dynamic"), subtitle: qsTr("Adapts to anything") },
        "movie": { icon: "movie", title: qsTr("Movie"), subtitle: qsTr("Wide and cinematic") },
        "music": { icon: "music", title: qsTr("Music"), subtitle: qsTr("Rich and natural") },
        "game": { icon: "game", title: qsTr("Game"), subtitle: qsTr("Positional and punchy") },
        "voice": { icon: "voice", title: qsTr("Voice"), subtitle: qsTr("Podcasts and calls") },
        "custom": { icon: "custom", title: qsTr("Custom"), subtitle: qsTr("Your own mix") }
    })
    readonly property var eqInfo: ({
        "flat": qsTr("Flat"), "open": qsTr("Open"), "rich": qsTr("Rich"), "focused": qsTr("Focused"), "custom": qsTr("Custom"),
        "bass-boost": qsTr("Bass Boost"), "treble-boost": qsTr("Treble Boost"), "vocal-clarity": qsTr("Vocal Clarity"),
        "podcast": qsTr("Podcast"), "loudness-curve": qsTr("Loudness Curve"),
        "american-rap": qsTr("American Rap"), "ballad": qsTr("Ballad"), "classical": qsTr("Classical"),
        "club": qsTr("Club"), "dance": qsTr("Dance"), "deep-house": qsTr("Deep House"), "german-rap": qsTr("German Rap"),
        "hard-techno": qsTr("Hard Techno"), "minimal": qsTr("Minimal"), "party": qsTr("Party"), "pop": qsTr("Pop"),
        "reggae": qsTr("Reggae"), "rock": qsTr("Rock"), "ska": qsTr("Ska"), "soft": qsTr("Soft"), "techno": qsTr("Techno")
    })
    readonly property bool libraryEqPreset: backend.eqToneIds.indexOf(backend.eqPreset) >= 0
                                            || backend.eqGenreIds.indexOf(backend.eqPreset) >= 0

    // Watch the spectrum only while the window is on screen.
    Binding { target: outputAnalyzer; property: "target"; value: backend.outputSink }
    Binding { target: outputAnalyzer; property: "active"; value: win.visible && win.visibility !== Window.Minimized }
    Binding { target: inputAnalyzer; property: "target"; value: "atomicaudio" }
    Binding { target: inputAnalyzer; property: "active"; value: win.visible && win.visibility !== Window.Minimized && backend.filterRunning && !backend.bypass }

    Shortcut { sequence: "?"; context: Qt.ApplicationShortcut; onActivated: win.helpVisible = !win.helpVisible }
    Shortcut { sequence: "Escape"; context: Qt.ApplicationShortcut; enabled: win.helpVisible; onActivated: win.helpVisible = false }
    Shortcut { sequence: "Q"; context: Qt.ApplicationShortcut; onActivated: Qt.quit() }
    Shortcut { sequence: "Space"; context: Qt.ApplicationShortcut; onActivated: backend.toggleEngine() }
    Shortcut { sequence: "A"; context: Qt.ApplicationShortcut; onActivated: backend.toggleBypass() }
    Shortcut { sequence: "E"; context: Qt.ApplicationShortcut; onActivated: backend.cycleEqPreset() }
    Shortcut { sequence: "O"; context: Qt.ApplicationShortcut; onActivated: backend.setTuning(!backend.tuning) }
    Shortcut { sequence: "H"; context: Qt.ApplicationShortcut; onActivated: backend.editingHeadphones = !backend.editingHeadphones }
    Shortcut { sequence: "R"; context: Qt.ApplicationShortcut; onActivated: backend.resetProfile() }
    Shortcut { sequences: ["Up", "K"]; context: Qt.ApplicationShortcut; onActivated: win.selectedEffect = (win.selectedEffect + win.effectKeys.length - 1) % win.effectKeys.length }
    Shortcut { sequences: ["Down", "J"]; context: Qt.ApplicationShortcut; onActivated: win.selectedEffect = (win.selectedEffect + 1) % win.effectKeys.length }
    Shortcut { sequence: "Left"; context: Qt.ApplicationShortcut; autoRepeat: true; onActivated: win.adjustSelected(-1) }
    Shortcut { sequence: "Right"; context: Qt.ApplicationShortcut; autoRepeat: true; onActivated: win.adjustSelected(1) }
    Shortcut { sequences: [StandardKey.Undo]; context: Qt.ApplicationShortcut; onActivated: backend.undo() }
    Shortcut { sequences: [StandardKey.Redo, "Ctrl+Shift+Z"]; context: Qt.ApplicationShortcut; onActivated: backend.redo() }
    Repeater {
        model: 6
        delegate: Item {
            Shortcut {
                sequence: String(index + 1)
                context: Qt.ApplicationShortcut
                onActivated: backend.setMode(backend.modeIds[index])
            }
        }
    }

    // Right-to-left languages mirror the window; the Translator sets the
    // application's layout direction.
    Item {
        anchors.fill: parent
        LayoutMirroring.enabled: Qt.application.layoutDirection === Qt.RightToLeft
        LayoutMirroring.childrenInherit: true

        ColumnLayout {
            anchors.fill: parent
            anchors.margins: 22
            spacing: 18

            // Header: name, output, profile switch, power.
            RowLayout {
                Layout.fillWidth: true
                spacing: 14

                Image {
                    source: "qrc:/atomicaudio.svg"
                    sourceSize: Qt.size(96, 96)
                    Layout.preferredWidth: 38
                    Layout.preferredHeight: 38
                    smooth: true
                    mipmap: true
                }
                ColumnLayout {
                    spacing: 0
                    Label {
                        text: "Atomic Audio"
                        font.pixelSize: 22
                        font.weight: Font.Bold
                        font.letterSpacing: 0.4
                        color: win.fg
                    }
                    Label {
                        text: backend.outputSink === ""
                              ? qsTr("Looking for your speakers…")
                              : backend.playingElsewhere
                              ? qsTr("%1 · not the output in use").arg(backend.outputDescription || backend.outputSink)
                              : qsTr("%1 · playing on %2").arg(backend.outputDescription || backend.outputSink)
                                    .arg(backend.headphonesActive ? qsTr("headphones") : qsTr("speakers"))
                        font.pixelSize: 12
                        color: win.tint(win.fg, 0.55)
                        elide: Text.ElideRight
                        Layout.maximumWidth: 420
                    }
                }

                Item { Layout.fillWidth: true }

                // Which output's sound you're editing.
                Rectangle {
                    implicitWidth: profileRow.implicitWidth + 8
                    implicitHeight: 40
                    radius: Math.min(20, theme.radius)
                    color: win.tint(win.fg, 0.05)
                    border.color: win.tint(win.fg, 0.08)
                    Row {
                        id: profileRow
                        anchors.centerIn: parent
                        spacing: 4
                        Repeater {
                            model: [
                                { headphones: false, icon: "speakers", label: qsTr("Speakers") },
                                { headphones: true, icon: "headphones", label: qsTr("Headphones") }
                            ]
                            delegate: AbstractButton {
                                id: seg
                                readonly property bool current: modelData.headphones === backend.editingHeadphones
                                readonly property bool live: modelData.headphones === backend.headphonesActive
                                width: segRow.implicitWidth + 28
                                height: 32
                                hoverEnabled: true
                                focusPolicy: Qt.NoFocus
                                onClicked: backend.editingHeadphones = modelData.headphones
                                background: Rectangle {
                                    radius: Math.min(16, theme.radius)
                                    color: seg.current ? win.tint(win.accent, 0.18) : seg.hovered ? win.tint(win.fg, 0.06) : "transparent"
                                }
                                contentItem: Item {
                                    Row {
                                        id: segRow
                                        anchors.centerIn: parent
                                        spacing: 8
                                        AppIcon {
                                            name: modelData.icon
                                            width: 18
                                            height: 18
                                            anchors.verticalCenter: parent.verticalCenter
                                            color: seg.current ? win.accent : win.tint(win.fg, 0.65)
                                        }
                                        Label {
                                            text: modelData.label
                                            anchors.verticalCenter: parent.verticalCenter
                                            font.pixelSize: 13
                                            font.weight: seg.current ? Font.DemiBold : Font.Normal
                                            color: seg.current ? win.accent : win.tint(win.fg, 0.75)
                                        }
                                        Rectangle {
                                            visible: seg.live
                                            width: 6
                                            height: 6
                                            radius: 3
                                            anchors.verticalCenter: parent.verticalCenter
                                            color: "#4ade80"
                                            ToolTip.visible: liveHover.hovered
                                            ToolTip.text: qsTr("Playing now")
                                            HoverHandler { id: liveHover }
                                        }
                                    }
                                }
                            }
                        }
                    }
                }

                // Compare with the original.
                AbstractButton {
                    id: compare
                    implicitWidth: compareRow.implicitWidth + 28
                    implicitHeight: 40
                    hoverEnabled: true
                    focusPolicy: Qt.NoFocus
                    onClicked: backend.toggleBypass()
                    ToolTip.visible: hovered
                    ToolTip.delay: 500
                    ToolTip.text: qsTr("Hear the original sound (A)")
                    background: Rectangle {
                        radius: Math.min(20, theme.radius)
                        color: backend.bypass ? win.tint(win.warning, 0.2) : compare.hovered ? win.tint(win.fg, 0.08) : win.tint(win.fg, 0.05)
                        border.color: backend.bypass ? win.warning : win.tint(win.fg, 0.08)
                    }
                    contentItem: Item {
                        Row {
                            id: compareRow
                            anchors.centerIn: parent
                            spacing: 8
                            AppIcon {
                                name: "compare"
                                width: 18
                                height: 18
                                anchors.verticalCenter: parent.verticalCenter
                                color: backend.bypass ? win.warning : win.tint(win.fg, 0.7)
                            }
                            Label {
                                text: backend.bypass ? qsTr("Original") : qsTr("Compare")
                                anchors.verticalCenter: parent.verticalCenter
                                font.pixelSize: 13
                                color: backend.bypass ? win.warning : win.tint(win.fg, 0.8)
                            }
                        }
                    }
                }

                ColumnLayout {
                    spacing: 0
                    Layout.leftMargin: 8
                    Label {
                        text: backend.engineBusy ? qsTr("Working…")
                              : backend.engineOn ? (backend.filterRunning ? qsTr("On") : qsTr("Starting…"))
                              : qsTr("Off")
                        font.pixelSize: 15
                        font.weight: Font.DemiBold
                        color: backend.engineOn ? win.accent : win.tint(win.fg, 0.6)
                        Layout.alignment: Qt.AlignRight
                    }
                    Label {
                        text: qsTr("Space")
                        font.pixelSize: 11
                        color: win.tint(win.fg, 0.35)
                        Layout.alignment: Qt.AlignRight
                    }
                }
                PowerButton {
                    on: backend.engineOn
                    busy: backend.engineBusy
                    onClicked: backend.toggleEngine()
                }
            }

            // Sound going to Bluetooth, HDMI or USB isn't processed; say so
            // rather than leave a dead switch.
            Rectangle {
                visible: backend.playingElsewhere && backend.error === ""
                Layout.fillWidth: true
                implicitHeight: elsewhereLabel.implicitHeight + 20
                radius: Math.min(6, theme.radius)
                color: win.tint(win.warning, 0.12)
                border.color: win.tint(win.warning, 0.45)
                Label {
                    id: elsewhereLabel
                    anchors.fill: parent
                    anchors.margins: 10
                    text: qsTr("Sound is going to %1. Atomic Audio enhances the built-in speakers and headphone jack, so switch the output to them to hear it.")
                              .arg(backend.elsewhereDescription)
                    wrapMode: Text.Wrap
                    color: win.fg
                    font.pixelSize: 13
                }
            }

            // Problems are said, not hidden.
            Rectangle {
                visible: backend.error !== ""
                Layout.fillWidth: true
                implicitHeight: errorLabel.implicitHeight + 20
                radius: Math.min(6, theme.radius)
                color: win.tint(win.danger, 0.14)
                border.color: win.tint(win.danger, 0.5)
                Label {
                    id: errorLabel
                    anchors.fill: parent
                    anchors.margins: 10
                    text: backend.error
                    wrapMode: Text.Wrap
                    color: win.fg
                    font.pixelSize: 13
                }
            }

            // Modes.
            RowLayout {
                Layout.fillWidth: true
                spacing: 12
                Repeater {
                    model: backend.modeIds
                    delegate: ModeTile {
                        Layout.fillWidth: true
                        Layout.preferredWidth: 1
                        glyph: win.modeInfo[modelData].icon
                        title: win.modeInfo[modelData].title
                        subtitle: win.modeInfo[modelData].subtitle
                        shortcutKey: String(index + 1)
                        selected: backend.mode === modelData
                        onClicked: backend.setMode(modelData)
                    }
                }
            }

            RowLayout {
                Layout.fillWidth: true
                Layout.fillHeight: true
                spacing: 18

                // Equalizer and spectrum.
                Rectangle {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    Layout.minimumWidth: 420
                    radius: theme.radius
                    color: win.tint(win.fg, 0.03)
                    border.color: win.tint(win.fg, 0.07)

                    ColumnLayout {
                        anchors.fill: parent
                        anchors.margins: 18
                        spacing: 12

                        RowLayout {
                            Layout.fillWidth: true
                            spacing: 8
                            ColumnLayout {
                                spacing: 1
                                Layout.fillWidth: true
                                Layout.minimumWidth: 80
                                Label {
                                    text: qsTr("Equalizer")
                                    font.pixelSize: 16
                                    font.weight: Font.DemiBold
                                    color: win.fg
                                    elide: Text.ElideRight
                                    Layout.fillWidth: true
                                }
                                Label {
                                    text: win.libraryEqPreset
                                          ? qsTr("%1 from the preset library · drag the points to adjust").arg(win.eqInfo[backend.eqPreset])
                                          : qsTr("Drag the points · double-click to reset one · E for presets")
                                    font.pixelSize: 11
                                    color: win.tint(win.fg, 0.5)
                                    elide: Text.ElideRight
                                    Layout.fillWidth: true
                                }
                            }
                            Repeater {
                                model: backend.eqPresetIds
                                delegate: Chip {
                                    text: win.eqInfo[modelData]
                                    current: backend.eqPreset === modelData
                                    compact: win.width < 1000
                                    onClicked: backend.setEqPreset(modelData)
                                }
                            }
                            // The library: tone fixes and genre curves from open-source players.
                            Chip {
                                id: libraryChip
                                text: qsTr("More") + " ▾"
                                current: win.libraryEqPreset
                                compact: win.width < 1000
                                onClicked: libraryPopup.visible ? libraryPopup.close() : libraryPopup.open()

                                Popup {
                                    id: libraryPopup
                                    x: libraryChip.width - width
                                    y: libraryChip.height + 6
                                    width: 372
                                    margins: 12
                                    padding: 16
                                    // Stays open while you try presets; a click outside closes it.
                                    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutsideParent
                                    background: Rectangle {
                                        radius: theme.radius
                                        color: theme.background
                                        border.color: win.tint(win.fg, 0.16)
                                    }
                                    contentItem: ColumnLayout {
                                        spacing: 8
                                        Label {
                                            text: qsTr("Tone")
                                            font.pixelSize: 11
                                            font.weight: Font.DemiBold
                                            color: win.tint(win.fg, 0.55)
                                        }
                                        Flow {
                                            Layout.fillWidth: true
                                            spacing: 6
                                            Repeater {
                                                model: backend.eqToneIds
                                                delegate: Chip {
                                                    text: win.eqInfo[modelData]
                                                    current: backend.eqPreset === modelData
                                                    onClicked: backend.setEqPreset(modelData)
                                                }
                                            }
                                        }
                                        Label {
                                            text: qsTr("Genres")
                                            topPadding: 6
                                            font.pixelSize: 11
                                            font.weight: Font.DemiBold
                                            color: win.tint(win.fg, 0.55)
                                        }
                                        Flow {
                                            Layout.fillWidth: true
                                            spacing: 6
                                            Repeater {
                                                model: backend.eqGenreIds
                                                delegate: Chip {
                                                    text: win.eqInfo[modelData]
                                                    current: backend.eqPreset === modelData
                                                    onClicked: backend.setEqPreset(modelData)
                                                }
                                            }
                                        }
                                        Label {
                                            text: qsTr("Curves from iQualize and GStreamer, as they publish them: bolder than ours.")
                                            topPadding: 6
                                            font.pixelSize: 11
                                            color: win.tint(win.fg, 0.45)
                                            wrapMode: Text.WordWrap
                                            Layout.fillWidth: true
                                        }
                                    }
                                }
                            }
                        }

                        Item {
                            Layout.fillWidth: true
                            Layout.fillHeight: true

                            EqGraph {
                                id: graph
                                anchors.fill: parent
                                outputBands: outputAnalyzer.bands
                                inputBands: inputAnalyzer.bands
                                showInput: inputAnalyzer.receiving
                                gains: backend.eq
                                freqs: backend.eqFrequencies
                                curve: { backend.eq; return backend.eqResponse(180) }
                                tuningCurve: { backend.tuning; backend.editingHeadphones; return backend.tuning && !backend.editingHeadphones ? backend.tuningResponse(180) : [] }
                                opacity: backend.bypass ? 0.5 : 1
                                onBandChanged: (band, db) => backend.setEqBand(band, db)
                            }

                            // Off: say so, and how to turn it on.
                            Rectangle {
                                visible: !backend.engineOn && !backend.engineBusy
                                anchors.horizontalCenter: parent.horizontalCenter
                                anchors.top: parent.top
                                anchors.topMargin: 4
                                width: offLabel.implicitWidth + 32
                                height: offLabel.implicitHeight + 20
                                radius: Math.min(8, theme.radius)
                                color: win.tint(theme.background, 0.9)
                                border.color: win.tint(win.fg, 0.12)
                                Label {
                                    id: offLabel
                                    anchors.centerIn: parent
                                    text: qsTr("Atomic Audio is off · press Space to turn it on")
                                    color: win.fg
                                    font.pixelSize: 13
                                }
                            }
                        }

                        // Legend.
                        Row {
                            spacing: 18
                            Layout.fillWidth: true
                            Repeater {
                                model: [
                                    { color: win.accent, label: qsTr("Equalizer"), dash: false, show: true },
                                    { color: win.tint(win.accent, 0.4), label: qsTr("What you hear"), dash: false, show: true },
                                    { color: win.tint(win.fg, 0.5), label: qsTr("Before processing"), dash: false, show: graph.showInput },
                                    { color: win.tint(win.fg, 0.4), label: qsTr("Speaker optimizer"), dash: true, show: graph.tuningCurve.length > 0 }
                                ]
                                delegate: Row {
                                    visible: modelData.show
                                    spacing: 6
                                    Rectangle {
                                        width: 14
                                        height: modelData.dash ? 1.5 : 3
                                        radius: 1
                                        color: modelData.color
                                        anchors.verticalCenter: parent.verticalCenter
                                    }
                                    Label {
                                        text: modelData.label
                                        font.pixelSize: 11
                                        color: win.tint(win.fg, 0.55)
                                    }
                                }
                            }
                        }
                    }
                }

                // Effects.
                Rectangle {
                    Layout.preferredWidth: win.width < 1080 ? 330 : 380
                    Layout.fillHeight: true
                    radius: theme.radius
                    color: win.tint(win.fg, 0.03)
                    border.color: win.tint(win.fg, 0.07)

                    ColumnLayout {
                        anchors.fill: parent
                        anchors.margins: 12
                        spacing: 2

                        RowLayout {
                            Layout.fillWidth: true
                            Layout.margins: 6
                            Layout.bottomMargin: 4
                            Label {
                                text: win.editingHeadphones ? qsTr("Headphone effects") : qsTr("Speaker effects")
                                font.pixelSize: 16
                                font.weight: Font.DemiBold
                                color: win.fg
                                Layout.fillWidth: true
                            }
                            Label {
                                text: qsTr("↑↓ ←→")
                                font.pixelSize: 11
                                color: win.tint(win.fg, 0.35)
                            }
                        }

                        // Scrolls when the window is short.
                        ScrollView {
                            id: effectsScroll
                            Layout.fillWidth: true
                            Layout.fillHeight: true
                            // Take only the room that's left, not the list's full height.
                            Layout.preferredHeight: 0
                            Layout.minimumHeight: 0
                            clip: true
                            contentWidth: availableWidth

                            ColumnLayout {
                                width: effectsScroll.availableWidth
                                spacing: 2
                                EffectRow {
                                    Layout.fillWidth: true
                                    icon: "surround"
                                    title: qsTr("Surround virtualizer")
                                    subtitle: win.editingHeadphones ? qsTr("Sound in front of you, not in your head")
                                                                    : qsTr("Wider than the laptop itself")
                                    value: backend.surround
                                    selected: win.selectedEffect === 0
                                    onMoved: (v) => backend.setAmount("surround", v)
                                    onPicked: win.selectedEffect = 0
                                }
                                EffectRow {
                                    Layout.fillWidth: true
                                    icon: "dialogue"
                                    title: qsTr("Dialogue enhancer")
                                    subtitle: qsTr("Voices stand out from the mix")
                                    value: backend.dialogue
                                    selected: win.selectedEffect === 1
                                    onMoved: (v) => backend.setAmount("dialogue", v)
                                    onPicked: win.selectedEffect = 1
                                }
                                EffectRow {
                                    Layout.fillWidth: true
                                    icon: "bass"
                                    title: qsTr("Bass enhancer")
                                    subtitle: win.editingHeadphones ? qsTr("Fuller low end, even at low volume")
                                                                    : qsTr("Rebuilds bass, fuller at low volume")
                                    value: backend.bass
                                    selected: win.selectedEffect === 2
                                    onMoved: (v) => backend.setAmount("bass", v)
                                    onPicked: win.selectedEffect = 2
                                }
                                EffectRow {
                                    Layout.fillWidth: true
                                    icon: "leveler"
                                    title: qsTr("Volume leveler")
                                    subtitle: qsTr("Quiet scenes up, loud ones down")
                                    value: backend.leveler
                                    selected: win.selectedEffect === 3
                                    onMoved: (v) => backend.setAmount("leveler", v)
                                    onPicked: win.selectedEffect = 3
                                }
                                EffectRow {
                                    Layout.fillWidth: true
                                    icon: "loudness"
                                    title: qsTr("Loudness")
                                    subtitle: qsTr("Louder without distortion")
                                    value: backend.loudness
                                    selected: win.selectedEffect === 4
                                    onMoved: (v) => backend.setAmount("loudness", v)
                                    onPicked: win.selectedEffect = 4
                                }
                                EffectRow {
                                    Layout.fillWidth: true
                                    icon: "optimizer"
                                    title: qsTr("Speaker optimizer")
                                    subtitle: win.editingHeadphones ? qsTr("Only for the built-in speakers")
                                                                    : backend.speakerProfileMeasured ? qsTr("Measured for the %1").arg(backend.speakerProfileName)
                                                                    : qsTr("General tuning for laptop speakers")
                                    toggle: true
                                    available: !win.editingHeadphones
                                    checked: backend.tuning
                                    selected: win.selectedEffect === 5
                                    onToggled: (on) => backend.setTuning(on)
                                    onPicked: win.selectedEffect = 5
                                }
                            }
                        }
                    }
                }
            }

            // Footer: the shortcuts that matter most.
            RowLayout {
                Layout.fillWidth: true
                spacing: 18
                Repeater {
                    model: [
                        ["Space", qsTr("on/off")], ["A", qsTr("compare")], ["1–6", qsTr("modes")],
                        ["E", qsTr("EQ preset")], ["H", qsTr("speakers/headphones")], ["?", qsTr("all shortcuts")]
                    ]
                    delegate: Row {
                        spacing: 6
                        Rectangle {
                            width: keyLabel.implicitWidth + 12
                            height: 20
                            radius: Math.min(4, theme.radius)
                            color: win.tint(win.fg, 0.07)
                            Label {
                                id: keyLabel
                                anchors.centerIn: parent
                                text: modelData[0]
                                font.pixelSize: 11
                                font.weight: Font.DemiBold
                                color: win.tint(win.fg, 0.8)
                            }
                        }
                        Label {
                            text: modelData[1]
                            font.pixelSize: 12
                            color: win.tint(win.fg, 0.5)
                            anchors.verticalCenter: parent.verticalCenter
                        }
                    }
                }
                Item { Layout.fillWidth: true }
                Label {
                    visible: backend.bypass
                    text: qsTr("Comparing: you're hearing the original sound")
                    color: win.warning
                    font.pixelSize: 12
                    font.weight: Font.DemiBold
                }
            }
        }

        HelpOverlay {
            anchors.fill: parent
            visible: win.helpVisible
            onClosed: win.helpVisible = false
        }
    }
}
