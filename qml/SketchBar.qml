import QtQuick

// Top centre, only while sketching or picking a sketch plane:
//   sketching   "SKETCH", the Look at Sketch, Toggle Construction and Find Profile Gaps
//               buttons (room left for more), and the Finish Sketch button (Fusion's green tick)
//   picking     what to click, and Cancel
// Under it, in fixed places (owner, 2026-10-30): the tool in use, its whole name in a small
// pill that slides down and fades in when a tool starts and goes again when it stops; below
// that the guide, the note on what to do next. Both keep their words while fading out.
// Fixed size, so nothing ever moves or resizes anything.
Item {
    id: bar

    required property var theme
    required property var sketch
    property string toolName: ""          // the drawing tool in use (its own pill)
    property int toolCount: -1            // a running count shown to the right of the name (-1: none)
    property int toolSkipped: 0           // ...and a second bubble: picks left out (already projected)
    property string toolHint: ""          // how to use it: the guide while nothing else is said
    property real guideBelow: 0           // a tool's own rows reach down to here (the bar's y): the guide goes under them
    property bool construction: false     // the Construction switch is on (its button shows it)
    property bool gaps: false             // Find Profile Gaps' rings are showing (its button shows it)
    property int gapCount: 0              // ...how many gaps they ring
    property int gapAt: -1                // ...the one stepped to (0-based), -1 before stepping
    property string gapNote: ""           // ...said after the count ("3 closed profiles")
    signal clearCount()                   // the x beside the count: take everything it counts off
    signal stepGap(int by)                // the stepper's arrows: -1 back, +1 on
    // While gaps are ringed the stepper takes the guide's place (the guide waits under it).
    readonly property bool stepperOn: gaps && gapCount > 0
    signal toggleConstruction()
    signal findGaps()

    // Which of the two it shows, kept while fading out so it never flips on the way.
    property bool asSketch: false
    Connections {
        target: bar.sketch
        function onActiveChanged() { if (bar.sketch.active) bar.asSketch = true }
        function onPickingChanged() { if (bar.sketch.picking) bar.asSketch = false }
    }

    // The tool's name and the guide's words, kept while their pills fade out.
    property string shownTool: ""
    property int shownCount: -1
    property int shownSkipped: 0
    onToolSkippedChanged: if (toolSkipped > 0) shownSkipped = toolSkipped
    onToolCountChanged: if (toolName) shownCount = toolCount
    property string shownNote: ""
    onToolNameChanged: if (toolName) { shownTool = toolName; shownCount = toolCount }
    readonly property bool toolOn: toolName !== "" && sketch.active
    // The guide: what was just said (for a moment), else the tool's own hint while it's on.
    readonly property string guide: sketch.message || (toolOn ? toolHint : "")
    onGuideChanged: if (guide) shownNote = guide
    // A tool stopping (Esc, another tool) takes its last step's words with it, unless they
    // were said as it stopped (what it just made).
    onToolOnChanged: if (!toolOn && sketch.message && Date.now() - sketch.messageAt > 100) sketch.message = ""

    width: 400
    height: 40 + 8 + 30 + 8 + 30

    Rectangle {
        id: pill
        width: parent.width
        height: 40
        radius: height / 2
        color: Qt.rgba(bar.theme.panelBg.r, bar.theme.panelBg.g, bar.theme.panelBg.b, 0.92)
        border.color: bar.asSketch ? bar.theme.success : bar.theme.borderStrong
        opacity: bar.sketch.active || bar.sketch.picking ? 1 : 0
        visible: opacity > 0
        Behavior on opacity { NumberAnimation { duration: 150 } }

        Text {
            id: title
            anchors.left: parent.left
            anchors.leftMargin: 18
            anchors.verticalCenter: parent.verticalCenter
            visible: bar.asSketch
            text: "SKETCH"
            color: bar.theme.textPrimary
            font.pixelSize: 13
            font.bold: true
        }

        // Look at the sketch again (Sketch.lookAtSketch): a sheet with a dotted grid on it.
        Rectangle {
            id: lookButton
            objectName: "lookAtSketch"
            anchors.left: title.right
            anchors.leftMargin: 10
            anchors.verticalCenter: parent.verticalCenter
            visible: bar.asSketch
            width: 26
            height: 26
            radius: 13
            color: lookArea.containsMouse ? bar.theme.controlHover : bar.theme.controlBg
            border.color: bar.theme.border
            Behavior on color { ColorAnimation { duration: 120 } }

            Canvas {
                id: sheet
                anchors.centerIn: parent
                width: 14
                height: 14
                readonly property color ink: lookArea.containsMouse ? bar.theme.textPrimary : bar.theme.textSecondary
                onInkChanged: requestPaint()
                onPaint: {
                    const g = getContext("2d")
                    g.reset()
                    g.strokeStyle = ink
                    g.lineWidth = 1.2
                    g.strokeRect(0.6, 0.6, 12.8, 12.8)
                    g.setLineDash([1.2, 1.4])
                    g.beginPath()
                    g.moveTo(7, 1.5); g.lineTo(7, 12.5)
                    g.moveTo(1.5, 7); g.lineTo(12.5, 7)
                    g.stroke()
                }
            }
            MouseArea {
                id: lookArea
                anchors.fill: parent
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                onClicked: bar.sketch.lookAtSketch()
            }
            HoverTip {
                theme: bar.theme
                area: lookArea
                title: "Look at Sketch"
                text: "Look at and center sketch in the viewport"
            }
        }

        // Toggle Construction (Main.toggleConstruction, as X): a dash-dot diagonal in the
        // construction lines' own colour; ringed in it while the Construction switch is on.
        Rectangle {
            id: constructionButton
            objectName: "toggleConstruction"
            anchors.left: lookButton.right
            anchors.leftMargin: 6
            anchors.verticalCenter: parent.verticalCenter
            visible: bar.asSketch
            width: 26
            height: 26
            radius: 13
            color: constructionArea.containsMouse ? bar.theme.controlHover : bar.theme.controlBg
            border.color: bar.construction ? bar.theme.sketchConstruction : bar.theme.border
            border.width: bar.construction ? 1.5 : 1
            Behavior on color { ColorAnimation { duration: 120 } }

            Canvas {
                anchors.centerIn: parent
                width: 14
                height: 14
                readonly property color ink: bar.theme.sketchConstruction
                onInkChanged: requestPaint()
                onPaint: {
                    const g = getContext("2d")
                    g.reset()
                    g.strokeStyle = ink
                    g.lineWidth = 1.6
                    g.setLineDash([2.6, 1.6, 0.8, 1.6])
                    g.beginPath()
                    g.moveTo(1.5, 12.5); g.lineTo(12.5, 1.5)
                    g.stroke()
                }
            }
            MouseArea {
                id: constructionArea
                anchors.fill: parent
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                onClicked: bar.toggleConstruction()
            }
            HoverTip {
                theme: bar.theme
                area: constructionArea
                title: "Toggle Construction"
                text: "Turns the selected lines construction, or all back to normal (X). With nothing selected, or while drawing, what you draw next"
            }
        }

        // Find Profile Gaps (Main.showGaps, Alt+G): two line ends with a little red ring round
        // the gap between them; ringed red while the rings show (10 s, or till Esc).
        Rectangle {
            id: gapsButton
            objectName: "findGaps"
            anchors.left: constructionButton.right
            anchors.leftMargin: 6
            anchors.verticalCenter: parent.verticalCenter
            visible: bar.asSketch
            width: 26
            height: 26
            radius: 13
            color: gapsArea.containsMouse ? bar.theme.controlHover : bar.theme.controlBg
            border.color: bar.gaps ? bar.theme.danger : bar.theme.border
            border.width: bar.gaps ? 1.5 : 1
            Behavior on color { ColorAnimation { duration: 120 } }

            Canvas {
                anchors.centerIn: parent
                width: 16
                height: 14
                readonly property color ink: bar.theme.textSecondary
                readonly property color ring: bar.theme.danger
                onInkChanged: requestPaint()
                onRingChanged: requestPaint()
                onPaint: {
                    const g = getContext("2d")
                    g.reset()
                    g.lineCap = "round"
                    g.strokeStyle = ink
                    g.lineWidth = 1.6
                    g.beginPath()
                    g.moveTo(1, 7); g.lineTo(6, 7)
                    g.moveTo(10, 7); g.lineTo(15, 7)
                    g.stroke()
                    g.strokeStyle = ring
                    g.lineWidth = 1.3
                    g.beginPath()
                    g.arc(8, 7, 4.6, 0, Math.PI * 2)
                    g.stroke()
                }
            }
            MouseArea {
                id: gapsArea
                anchors.fill: parent
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                onClicked: bar.findGaps()
            }
            HoverTip {
                theme: bar.theme
                area: gapsArea
                title: "Find Profile Gaps"
                text: "Rings in red, for 10 s, where lines nearly meet (closer than 0.039 in) but aren't joined: what keeps a profile from closing. Esc clears them sooner (Alt+G)"
            }
        }

        Text {
            anchors.left: parent.left
            anchors.leftMargin: 18
            anchors.right: button.left
            anchors.rightMargin: 12
            anchors.verticalCenter: parent.verticalCenter
            visible: !bar.asSketch
            elide: Text.ElideRight
            text: "Click a plane or a flat face"
            color: bar.theme.textSecondary
            font.pixelSize: 13
        }

        Rectangle {
            id: button
            anchors.right: parent.right
            anchors.rightMargin: 5
            anchors.verticalCenter: parent.verticalCenter
            width: 140
            height: 30
            radius: height / 2
            color: bar.asSketch ? (area.containsMouse ? Qt.lighter(bar.theme.success, 1.15) : bar.theme.success)
                                     : (area.containsMouse ? bar.theme.controlHover : bar.theme.controlBg)
            Behavior on color { ColorAnimation { duration: 120 } }

            Text {
                anchors.centerIn: parent
                text: bar.asSketch ? "✓  Finish Sketch" : "Cancel   Esc"
                color: bar.asSketch ? bar.theme.windowBg : bar.theme.textPrimary
                font.pixelSize: 13
                font.bold: bar.sketch.active
            }
            MouseArea {
                id: area
                anchors.fill: parent
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                onClicked: bar.asSketch ? bar.sketch.finish() : bar.sketch.picking = false
            }
        }
    }

    // The tool in use, its whole name.
    Rectangle {
        id: toolPill
        objectName: "toolPill"
        anchors.top: pill.bottom
        anchors.topMargin: 8
        anchors.horizontalCenter: parent.horizontalCenter
        width: toolText.implicitWidth + 28 + (bar.shownCount >= 0 ? countBadge.width + 8 : 0) + (bar.shownCount > 0 || bar.toolSkipped > 0 ? 22 : 0) + (bar.toolSkipped > 0 ? skipBadge.width + 6 : 0)
        height: 30
        radius: height / 2
        color: Qt.rgba(bar.theme.panelBg.r, bar.theme.panelBg.g, bar.theme.panelBg.b, 0.92)
        border.color: bar.theme.success
        opacity: bar.toolOn ? 1 : 0
        visible: opacity > 0
        Behavior on opacity { NumberAnimation { duration: 180 } }
        transform: Translate {
            y: bar.toolOn ? 0 : -10
            Behavior on y { NumberAnimation { duration: 180; easing.type: Easing.OutCubic } }
        }
        Text {
            id: toolText
            anchors.left: parent.left
            anchors.leftMargin: 14
            anchors.verticalCenter: parent.verticalCenter
            text: bar.shownTool
            color: bar.theme.textPrimary
            font.pixelSize: 13
            font.bold: true
        }
        // How many things the tool has taken so far.
        Rectangle {
            id: countBadge
            objectName: "toolCount"
            visible: bar.shownCount >= 0
            anchors.left: toolText.right
            anchors.leftMargin: 8
            anchors.verticalCenter: parent.verticalCenter
            width: Math.max(20, countText.implicitWidth + 12)
            height: 20
            radius: 10
            color: bar.shownCount > 0 ? bar.theme.success : bar.theme.controlBg
            Text {
                id: countText
                anchors.centerIn: parent
                text: bar.shownCount
                color: bar.shownCount > 0 ? "#000000" : bar.theme.textSecondary
                font.pixelSize: 12
                font.bold: true
            }
        }
        // Clears the count: everything it counts comes off.
        Rectangle {
            id: clearButton
            objectName: "toolCountClear"
            visible: bar.shownCount > 0 || bar.toolSkipped > 0     // clears the left-out picks too
            anchors.left: bar.toolSkipped > 0 ? skipBadge.right : countBadge.right
            anchors.leftMargin: 4
            anchors.verticalCenter: parent.verticalCenter
            width: 18
            height: 18
            radius: 9
            color: clearArea.containsMouse ? bar.theme.controlHover : "transparent"
            Text {
                anchors.centerIn: parent
                text: "✕"
                color: bar.theme.textPrimary
                font.pixelSize: 11
                font.bold: true
            }
            MouseArea {
                id: clearArea
                anchors.fill: parent
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                onClicked: bar.clearCount()
            }
            HoverTip {
                theme: bar.theme
                area: clearArea
                title: "Clear"
                text: "Take everything off the sketch again, and forget the picks left out"
            }
        }
        // How many picks were left out because they are already projected into this sketch.
        Rectangle {
            id: skipBadge
            objectName: "toolSkipped"
            opacity: bar.toolSkipped > 0 && bar.toolOn ? 1 : 0
            visible: opacity > 0
            Behavior on opacity { NumberAnimation { duration: 250 } }
            anchors.left: countBadge.right
            anchors.leftMargin: 6
            anchors.verticalCenter: parent.verticalCenter
            width: Math.max(20, skipText.implicitWidth + 12)
            height: 20
            radius: 10
            color: bar.theme.warning
            Text {
                id: skipText
                anchors.centerIn: parent
                text: bar.shownSkipped
                color: "#000000"
                font.pixelSize: 12
                font.bold: true
            }
            MouseArea {
                id: skipArea
                anchors.fill: parent
                hoverEnabled: true
            }
            HoverTip {
                theme: bar.theme
                area: skipArea
                title: "Left out"
                text: bar.shownSkipped === 1 ? "1 pick is already projected into this sketch, so it is left out"
                                              : bar.shownSkipped + " picks are already projected into this sketch, so they are left out"
            }
        }
    }

    // The guide: what to do next.
    Rectangle {
        objectName: "guidePill"
        anchors.top: pill.bottom
        anchors.topMargin: Math.max(8 + 30 + 8, bar.guideBelow + 8 - pill.height)
        anchors.horizontalCenter: parent.horizontalCenter
        // A long note (an extrude's failure) wraps instead of running off the window.
        readonly property real most: Math.max(300, (bar.Window.width || 1200) - 120)
        width: Math.min(note.implicitWidth, most - 24) + 24
        height: Math.max(30, note.height + 12)
        radius: 15
        color: Qt.rgba(bar.theme.panelBg.r, bar.theme.panelBg.g, bar.theme.panelBg.b, 0.92)
        border.color: bar.theme.warning
        opacity: bar.guide !== "" && !bar.stepperOn ? 1 : 0
        visible: opacity > 0
        Behavior on opacity { NumberAnimation { duration: 180 } }
        Text {
            id: note
            anchors.centerIn: parent
            width: Math.min(implicitWidth, parent.most - 24)
            wrapMode: Text.Wrap
            horizontalAlignment: Text.AlignHCenter
            text: bar.shownNote
            color: bar.theme.textPrimary
            font.pixelSize: 13
        }
    }

    // Find Profile Gaps' stepper, in the guide's place: ‹ [ 2 of 5 ] › — the arrows fly the view
    // to each gap in turn (Main.stepGap), round and round; the box only shows, it isn't typed in.
    component Arrow: Rectangle {
        id: arrow
        property int by: 1
        property string tip: ""
        width: 22
        height: 22
        radius: 11
        color: arrowArea.containsMouse ? bar.theme.controlHover : bar.theme.controlBg
        border.color: bar.theme.border
        Behavior on color { ColorAnimation { duration: 120 } }
        Canvas {
            anchors.centerIn: parent
            width: 8
            height: 10
            readonly property color ink: bar.theme.textPrimary
            onInkChanged: requestPaint()
            onPaint: {
                const g = getContext("2d")
                g.reset()
                g.strokeStyle = ink
                g.lineWidth = 1.8
                g.lineCap = "round"
                g.lineJoin = "round"
                g.beginPath()
                if (arrow.by < 0) { g.moveTo(6, 1); g.lineTo(2, 5); g.lineTo(6, 9) }
                else { g.moveTo(2, 1); g.lineTo(6, 5); g.lineTo(2, 9) }
                g.stroke()
            }
        }
        MouseArea {
            id: arrowArea
            anchors.fill: parent
            hoverEnabled: true
            cursorShape: Qt.PointingHandCursor
            onClicked: bar.stepGap(arrow.by)
        }
        HoverTip {
            theme: bar.theme
            area: arrowArea
            title: arrow.tip
            text: "Zoom to the " + (arrow.by < 0 ? "previous" : "next") + " gap (the rings stay until Esc)"
        }
    }
    Rectangle {
        objectName: "gapStepper"
        anchors.top: pill.bottom
        anchors.topMargin: 8 + 30 + 8
        anchors.horizontalCenter: parent.horizontalCenter
        width: stepRow.implicitWidth + 12
        height: 30
        radius: height / 2
        color: Qt.rgba(bar.theme.panelBg.r, bar.theme.panelBg.g, bar.theme.panelBg.b, 0.92)
        border.color: bar.theme.danger
        opacity: bar.stepperOn ? 1 : 0
        visible: opacity > 0
        Behavior on opacity { NumberAnimation { duration: 180 } }
        Row {
            id: stepRow
            anchors.centerIn: parent
            spacing: 6
            Arrow { objectName: "gapBack"; by: -1; tip: "Previous Gap"; anchors.verticalCenter: parent.verticalCenter }
            Rectangle {
                anchors.verticalCenter: parent.verticalCenter
                width: 64                       // fits "88 of 88": stepping never resizes it
                height: 22
                radius: 5
                color: bar.theme.controlBg
                border.color: bar.theme.border
                Text {
                    objectName: "gapCountText"
                    anchors.centerIn: parent
                    text: bar.gapAt < 0 ? String(bar.gapCount) : (bar.gapAt + 1) + " of " + bar.gapCount
                    color: bar.theme.textPrimary
                    font.pixelSize: 13
                    font.bold: true
                }
            }
            Arrow { objectName: "gapNext"; by: 1; tip: "Next Gap"; anchors.verticalCenter: parent.verticalCenter }
            TextMetrics {
                id: widest                      // the longest it says: the pill never resizes
                font.pixelSize: 13
                text: "gaps under 0.039 in, not joined · 888 closed profiles"
            }
            Text {
                anchors.verticalCenter: parent.verticalCenter
                width: widest.advanceWidth + 10
                leftPadding: 4
                rightPadding: 6
                text: (bar.gapCount === 1 ? "gap" : "gaps") + " under 0.039 in, not joined · " + bar.gapNote
                color: bar.theme.textPrimary
                font.pixelSize: 13
            }
        }
    }
}
