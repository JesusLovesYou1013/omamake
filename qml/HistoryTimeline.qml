import QtQuick

// The history along the bottom (Fusion calls it the Timeline), oldest on the left: the
// same list Ctrl+Z steps back through, shown as two kinds of row (owner, 2026-10-29):
//   the design's row   one dot per action; a sketch is ONE dot, at the point it was made
//                      (starting and finishing it, and all that was done inside it)
//   a sketch's row     while that sketch is open, only what was done inside it (entries
//                      tagged in_sketch with its id), in the sketch green and named; no
//                      other sketch's steps and none of the design's
// For now every action is a dot (later, each action's own icon). Hovering a dot grows it
// and fades in a tip naming the action. Floating like the Browser: no panel, a faint track
// behind the dots. More than fit scroll sideways; a new action scrolls into view.
// The tab on the bottom edge, halfway across, hides it into the edge (the Browser's tab,
// turned); it never moves.
Item {
    id: timeline

    required property var theme
    property var history: []                 // doc.history: [{type, summary, at, in_sketch?, …}]
    property int sketchId: 0                 // the open sketch (0: none)
    property bool shown: true

    readonly property int dot: 12
    readonly property int pitch: 26           // dot to dot
    readonly property string sketchName: {
        const made = history.find(a => a.type === "sketch" && a.id === sketchId)
        return made ? made.name : ""
    }
    readonly property color dotColor: sketchId ? theme.success : theme.accent

    // The row on show: [{summary}], oldest first.
    readonly property var actions: {
        const out = []
        if (sketchId) {
            for (const a of history)
                if (a.in_sketch === sketchId)
                    out.push({ summary: a.summary })
            return out
        }
        for (const a of history) {
            if (a.in_sketch || a.type === "finish_sketch")
                continue
            out.push({ summary: a.type === "sketch" ? a.name + (a.on ? " on " + a.on : "") : a.summary })
        }
        return out
    }

    height: 44 + 26

    // The tab, halfway along the bottom edge, partly past it like the Browser's.
    Rectangle {
        id: tab
        anchors.horizontalCenter: parent.horizontalCenter
        y: parent.height - 18
        width: 40
        height: 26
        radius: 8
        color: tabArea.containsMouse ? timeline.theme.controlHover
             : Qt.rgba(timeline.theme.panelRaised.r, timeline.theme.panelRaised.g, timeline.theme.panelRaised.b, 0.9)
        border.color: timeline.theme.borderStrong
        Behavior on color { ColorAnimation { duration: 120 } }

        Text {
            width: parent.width
            height: parent.height - 8
            horizontalAlignment: Text.AlignHCenter
            verticalAlignment: Text.AlignVCenter
            text: "‹"
            rotation: timeline.shown ? -90 : 90       // points into the edge, or out of it
            color: tabArea.containsMouse ? timeline.theme.textPrimary : timeline.theme.textSecondary
            font.pixelSize: 20
        }
        MouseArea {
            id: tabArea
            anchors.fill: parent
            hoverEnabled: true
            cursorShape: Qt.PointingHandCursor
            onClicked: timeline.shown = !timeline.shown
        }
    }

    // The row, which slides down into the edge when hidden.
    Item {
        id: row
        width: parent.width
        height: 44
        y: timeline.shown ? 0 : timeline.height + 10
        opacity: timeline.shown ? 1 : 0
        visible: opacity > 0
        Behavior on y { NumberAnimation { duration: 180; easing.type: Easing.OutCubic } }
        Behavior on opacity { NumberAnimation { duration: 180 } }

        // In a sketch, its name before its row, so it's clear whose steps these are.
        Text {
            id: label
            visible: timeline.sketchId !== 0
            x: timeline.actions.length ? track.x - width - 8 : (row.width - width) / 2
            anchors.verticalCenter: parent.verticalCenter
            text: timeline.sketchName + (timeline.actions.length ? "" : "  ·  no steps yet")
            color: timeline.theme.success
            font.pixelSize: 12
            font.bold: true
        }

        Rectangle {
            id: track
            anchors.centerIn: list
            width: list.width + 20
            height: 30
            radius: height / 2
            visible: timeline.actions.length > 0
            color: Qt.rgba(timeline.theme.panelBg.r, timeline.theme.panelBg.g, timeline.theme.panelBg.b, 0.55)
            border.color: Qt.rgba(timeline.theme.border.r, timeline.theme.border.g, timeline.theme.border.b, 0.6)
        }

        ListView {
            id: list
            anchors.horizontalCenter: parent.horizontalCenter
            anchors.verticalCenter: parent.verticalCenter
            width: Math.min(contentWidth, timeline.width - 240)
            height: row.height
            orientation: ListView.Horizontal
            interactive: contentWidth > width
            boundsBehavior: Flickable.StopAtBounds
            clip: true
            model: timeline.actions
            onCountChanged: positionViewAtEnd()

            delegate: Item {
                id: slot
                required property var modelData
                required property int index
                width: timeline.pitch
                height: list.height

                Rectangle {
                    anchors.centerIn: parent
                    width: timeline.dot
                    height: timeline.dot
                    radius: width / 2
                    color: timeline.dotColor
                    border.color: Qt.rgba(timeline.theme.windowBg.r, timeline.theme.windowBg.g, timeline.theme.windowBg.b, 0.8)
                    border.width: 1.5
                    scale: dotArea.containsMouse ? 1.6 : 1
                    Behavior on scale { NumberAnimation { duration: 140; easing.type: Easing.OutCubic } }
                }
                MouseArea {
                    id: dotArea
                    anchors.fill: parent
                    hoverEnabled: true
                    onContainsMouseChanged: {
                        if (containsMouse)
                            tip.showFor(slot, slot.modelData.summary)
                        else
                            tip.hideFor(slot)
                    }
                }
            }
        }
    }

    // One tip for all the dots, above the one under the mouse. It keeps its text while
    // fading out, and moves straight to the next dot when the mouse slides along.
    Rectangle {
        id: tip
        property Item owner: null
        property string text: ""

        function showFor(item, text) {
            owner = item
            tip.text = text
            const at = item.mapToItem(timeline, item.width / 2, 0)
            x = Math.max(0, Math.min(at.x - width / 2, timeline.width - width))
            shown = true
        }
        function hideFor(item) {
            if (owner === item)
                shown = false
        }
        property bool shown: false

        y: row.y + list.y - height - 2
        width: tipText.implicitWidth + 20
        height: 28
        radius: 8
        color: Qt.rgba(timeline.theme.panelBg.r, timeline.theme.panelBg.g, timeline.theme.panelBg.b, 0.95)
        border.color: timeline.theme.border
        opacity: shown ? 1 : 0
        visible: opacity > 0
        Behavior on opacity { NumberAnimation { duration: 150 } }

        Text {
            id: tipText
            anchors.centerIn: parent
            text: tip.text
            color: timeline.theme.textPrimary
            font.pixelSize: 12
        }
    }
}
