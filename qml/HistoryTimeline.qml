import QtQuick

// The history along the bottom (Fusion calls it the Timeline), oldest on the left: the
// same list Ctrl+Z steps back through, shown as two kinds of row (owner, 2026-10-29):
//   the design's row   one dot per action; a sketch is ONE dot, at the point it was made
//                      (starting, finishing and editing it, and all that was done inside
//                      it); double-click it to go back into the sketch (Fusion's Edit Sketch)
//   a sketch's row     while that sketch is open, only what was done inside it (entries
//                      tagged in_sketch with its id), in the sketch green and named; no
//                      other sketch's steps and none of the design's
// Most actions are a dot; an action with an icon of its own shows that instead (so far only a
// move: a cross with an arrow at each end, owner 2026-11-01, more to come, and the same for
// the design's row later). Moves done one after another would swamp the row, so a run of two or
// more folds into ONE move icon with its count; hovering it opens the run out into its moves,
// and it folds back a few seconds after the mouse has left. Hovering a dot or icon grows it
// and fades in a tip naming the action. Floating like the Browser: no panel, a faint track
// behind the dots. More than fit scroll sideways; a new action scrolls into view.
// The tab on the bottom edge, halfway across, hides it into the edge (the Browser's tab,
// turned); it never moves.
Item {
    id: timeline

    required property var theme
    property var history: []                 // doc.history: [{type, summary, at, in_sketch?, …}]
    property var future: []                  // doc.future: the steps behind the marker, greyed
    property int totalSteps: 0               // every step, before and behind the marker
    // The marker (owner, 2026-11-07): a line with a ball on top between the design row's items;
    // `marker` is where it is (0: before the first item, items.length: after the last), in between
    // while it's dragged. Main moves the design to follow (Main.qml `rewind`).
    property real marker: 0
    property bool markerHeld: false          // being dragged
    property bool markerEnabled: true        // not while a tool is open on the model
    // While the marker is dragged or settling the row stays as it was (same items, only the greying
    // follows the marker), so nothing is rebuilt as the design moves under it.
    property bool frozen: false
    property var heldActions: null
    onFrozenChanged: heldActions = frozen ? actions : null
    signal markerDragged(real position)
    signal markerDropped()
    readonly property int presentItems: items.filter(a => !a.future).length
    // The history length the marker stands for at boundary k (after k items). In a sketch, after its
    // last item: the history now and the sketch's own steps behind the marker (nothing past them).
    property int presentCount: 0
    // What the marker can reach (owner, 2026-11-07): the items whose history from `readyCount` on is worked out;
    // `readyDone`: all there is. Shown as a translucent fill along the track that grows from the newest end.
    property int readyCount: 0
    property bool readyDone: true
    // The design's row of an opened file: its steps built again in the background, oldest first (Document
    // modelBuiltTo): the history length up to which they're built (-1: nothing being built).
    property int builtCount: -1
    // An opened file being built again (GB-062): the build fill is the progress bar.
    readonly property bool building: builtCount >= 0 && !readyDone
    readonly property int builtItem: {
        if (builtCount < 0 || readyDone)
            return 0
        let k = 0
        while (k < items.length && countAt(k + 1) <= builtCount)
            ++k
        return k
    }
    readonly property int readyItem: {
        let k = Math.min(items.length, firstMovable)
        while (k > 0 && items[k - 1].step >= readyCount)
            --k
        return k
    }
    function countAt(k) {
        return k < items.length ? items[k].step : sketchId ? presentCount + sketchAhead : totalSteps
    }
    // In a sketch, where this time in it starts (the last time it was started or opened again): the
    // document can't go back past that without leaving the sketch, so before it the sketch's earlier
    // steps are only shown (Main.qml `rewind`). 0 on the design's row.
    readonly property int firstMovable: {
        if (!sketchId)
            return 0
        let opened = -1
        for (const a of history)
            if ((a.type === "sketch" && a.id === sketchId) || (a.type === "edit_sketch" && a.sketch === sketchId))
                opened = a.step
        let k = 0
        while (k < items.length && items[k].step <= opened)
            ++k
        return k
    }
    // The left edge of slot k in the row (slots aren't all one width: a folded run of moves is wider).
    function slotX(k) {
        let x = 0
        for (let i = 0; i < k && i < items.length; ++i)
            x += items[i].folded ? pitch + 14 : pitch
        return x
    }
    // Back from a place in the row to a marker position (whole at the gaps, in between across a slot).
    function markerAt(x) {
        let left = 0
        for (let i = 0; i < items.length; ++i) {
            const w = items[i].folded ? pitch + 14 : pitch
            if (x < left + w)
                return i + Math.max(0, x - left) / w
            left += w
        }
        return items.length
    }
    property int sketchId: 0                 // the open sketch (0: none)
    property bool shown: true

    signal sketchEditRequested(int id)
    // Break Link was clicked on a projection's step in sketch `sketch`; Main asks "are you sure".
    signal breakLinkRequested(int sketch, int link)

    readonly property int dot: 12
    readonly property int pitch: 26           // dot to dot
    // The size every step symbol is drawn at (GB-060 standard, owner pt162u: Fillet / Chamfer's size
    // "would be the standard size for sizing going forward"): SketchIcon (full), EyeIcon, ChainIcon, the
    // move cross. Hover grows them 1.18 (to the pitch, 26, no overlap).
    readonly property int icon: 22
    readonly property string sketchName: {
        const made = history.find(a => a.type === "sketch" && a.id === sketchId)
        return made ? made.name : ""
    }
    readonly property color dotColor: sketchId ? theme.success : theme.accent

    // The symbol a step shows instead of a dot (SketchIcon's kinds; "" for any other action):
    // a constraint or dimension by its type, Fix / Unfix by what was done, a drawn shape or an
    // edit (Fillet, Trim, Mirror...) by its tool.
    function symbolOf(a) {
        if (a.type === "constraint")
            return String(a.constraint || "")
        if (a.type === "line")
            return "line"
        if (a.type === "rectangle")
            return "rect2"
        if (a.type === "shape" && !a.link && !a.break_link)
            return String(a.tool || "")
        if (a.type === "text_edit")
            return "text"
        if (a.type === "construction")
            return "construction"
        if (a.type === "parameters")
            return "fx"
        if (a.type === "extrude")
            return "extrude"
        if (a.type === "chamfer")
            return "chamfermodel"
        if (a.type === "fillet")
            return "filletmodel"
        if (a.type === "fix")
            return String(a.summary || "").startsWith("Unfixed") ? "unfix" : "fix"
        return ""
    }

    // The row on show: [{summary, sketch?}], oldest first.
    readonly property var actions: frozen && heldActions ? heldActions : fresh()
    // How many of the steps behind the marker are the open sketch's, next in a row.
    readonly property int sketchAhead: {
        let n = 0
        while (sketchId && n < future.length && future[n].in_sketch === sketchId)
            ++n
        return n
    }
    function fresh() {
        const out = []
        if (sketchId) {
            // Its own steps, and those of its steps behind the marker that come next (greyed).
            for (const a of history.concat(future.slice(0, sketchAhead)))
                if (a.in_sketch === sketchId)
                    out.push({ summary: a.summary, sym: timeline.symbolOf(a), move: a.type === "drag", eye: a.type === "visibility" ? (a.shown ? "open" : "shut") : "",
                               link: a.link || 0, linkBroken: !!a.link_broken,
                               breaks: a.break_link || 0, issue: a.issue || "", issueText: a.issue_text || "",
                               step: a.step, future: !!a.future })
            return out
        }
        for (const a of history.concat(future)) {
            if (a.in_sketch || a.type === "finish_sketch" || a.type === "edit_sketch")
                continue
            if (a.type === "sketch")
                out.push({ summary: a.name + (a.on ? " on " + a.on : "") + (a.future ? "" : "  ·  double-click to edit"), sketch: a.id,
                           issue: a.issue || "", issueText: a.issue_text || "", step: a.step, future: !!a.future })
            else
                out.push({ summary: a.summary, sym: timeline.symbolOf(a), eye: a.type === "visibility" ? (a.shown ? "open" : "shut") : "",
                           issue: a.issue || "", issueText: a.issue_text || "", step: a.step, future: !!a.future })
        }
        return out
    }

    // Which run of moves is open (the index in `actions` of its first move; -1: none). Opened by
    // hovering the folded icon, folded again foldDelay ms after the mouse has left the run.
    property int openRun: -1
    readonly property int foldDelay: 3000
    // How long the mouse rests on a folded run before it opens out (owner, 2026-11-01), and on a
    // projection's step before its Break Link button pops up.
    readonly property int openDelay: 1500
    readonly property int breakDelay: 1500
    // What an issue's colour means (Fusion shows a yellow mark when a step still works but needs
    // a look, a red one when it failed): yellow, what was projected moved and the constraints on
    // it can't all hold; red, something it was projected from is gone.
    function issueColor(level) { return level === "red" ? theme.danger : theme.warning }

    // `actions` with every run of 2+ moves folded to one {move, count, from} item, unless it is
    // the open run: then its moves are separate items, each tagged with the run's `run` index.
    readonly property var items: {
        const out = []
        const list = actions
        let i = 0
        while (i < list.length) {
            if (!list[i].move) {
                out.push(list[i])
                ++i
                continue
            }
            let j = i
            while (j < list.length && list[j].move && list[j].future === list[i].future)   // a run never spans the marker
                ++j
            if (j - i < 2)
                out.push({ summary: list[i].summary, move: true, run: -1, step: list[i].step, future: list[i].future })
            else if (openRun === i)
                for (let k = i; k < j; ++k)
                    out.push({ summary: list[k].summary, move: true, run: i, step: list[k].step, future: list[k].future })
            else
                out.push({ summary: (j - i) + " moves in a row  ·  hover to open them out", move: true, folded: true, count: j - i, run: i,
                           step: list[i].step, future: list[i].future })
            i = j
        }
        return out
    }

    onOpenRunChanged: {
        tip.shown = false                // the icon the tip belonged to is gone
        if (openRun >= 0)
            foldTimer.restart()          // folds on its own unless the mouse is over the run
    }

    Timer {
        id: foldTimer
        interval: timeline.foldDelay
        onTriggered: timeline.openRun = -1
    }
    // The wait before a folded run opens out.
    Timer {
        id: openTimer
        property int run: -1
        interval: timeline.openDelay
        onTriggered: timeline.openRun = run
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
            // GB-061 (owner pt163u: "once it's fully filled, basically defaults back to a much darker background
            // color, so these glyphs stand out more"): while steps are still being worked out the fills below
            // show progress over the usual see-through track; once everything is (`settled`) the fills fade out
            // and the track fades to the theme's window background, nearly opaque: the darkest the theme has
            // (light in a light theme), so the symbols in the accent colour stand out most.
            readonly property bool settled: timeline.readyDone && timeline.readyItem === 0
            color: settled ? Qt.rgba(timeline.theme.windowBg.r, timeline.theme.windowBg.g, timeline.theme.windowBg.b, 0.94)
                           : Qt.rgba(timeline.theme.panelBg.r, timeline.theme.panelBg.g, timeline.theme.panelBg.b, 0.55)
            Behavior on color { ColorAnimation { duration: 350; easing.type: Easing.OutCubic } }
            border.color: Qt.rgba(timeline.theme.border.r, timeline.theme.border.g, timeline.theme.border.b, 0.6)

            // The ready fill: from the first item that can be reached to the end, faint while the rest is still
            // being worked out; once it all can be, it fades away (GB-061) and the track goes dark instead.
            Rectangle {
                id: readyFill
                objectName: "timelineReady"
                readonly property real fromX: timeline.readyItem === 0 ? 0
                                             : list.x - track.x + timeline.slotX(timeline.readyItem) - list.contentX
                x: Math.max(0, fromX)
                width: track.width - x
                height: track.height
                radius: track.radius
                visible: timeline.items.length > 0
                color: timeline.dotColor
                // Gone once it's all ready (GB-061; was 0.26 then), and while an opened file is being built again (GB-062):
                // then the build fill alone is the progress bar, over the plain dark track.
                opacity: track.settled || timeline.building ? 0 : 0.13
                Behavior on x { NumberAnimation { duration: 220; easing.type: Easing.OutCubic } }
                Behavior on opacity { NumberAnimation { duration: 300 } }
            }
            // The opened file's steps being built again: a fill growing from the oldest end as the rebuild gets on
            // (Document modelBuildingTo, GB-062), over the plain track; when it's all done the track settles dark (GB-061).
            Rectangle {
                id: builtFill
                objectName: "timelineBuilt"
                // Under the steps built when the whole row is on show; when it's longer than the track (it scrolls), the
                // share of all the steps built (GB-062: else it filled the track at the last step in view, 49 of 116).
                readonly property bool overflow: list.contentWidth > list.width + 1
                readonly property real toX: timeline.builtItem === 0 ? 0
                                            : overflow ? track.width * timeline.builtItem / Math.max(1, timeline.items.length)
                                            : list.x - track.x + timeline.slotX(timeline.builtItem) - list.contentX
                x: 0
                width: Math.max(0, Math.min(track.width, toX))
                height: track.height
                radius: track.radius
                visible: timeline.items.length > 0 && timeline.building && width > 0
                color: timeline.dotColor
                opacity: 0.16                          // GB-062: the progress bar on its own (was 0.07 over the ready fill)
                Behavior on width { NumberAnimation { duration: 220; easing.type: Easing.OutCubic } }
            }
        }

        // The marker (owner, 2026-11-07): a line with a ball on top, between the row's items. Drag it
        // left and right: the design follows (Main.qml `rewind`), smoothly in between, and it settles on
        // the nearest gap when let go. In a sketch it is on the sketch's own row, in the sketch's green.
        Item {
            id: markerItem
            objectName: "timelineMarker"
            z: 5
            visible: timeline.items.length > 0
            enabled: timeline.markerEnabled
            opacity: enabled ? 1 : 0.45
            width: 22
            height: row.height + 8
            y: -12
            x: list.x + timeline.slotX(Math.floor(timeline.marker))
               + (timeline.marker - Math.floor(timeline.marker)) * (timeline.slotX(Math.floor(timeline.marker) + 1) - timeline.slotX(Math.floor(timeline.marker)))
               - list.contentX - width / 2
            readonly property color ink: timeline.dotColor

            // A soft glow behind the ball, brighter while held.
            Rectangle {
                anchors.horizontalCenter: parent.horizontalCenter
                y: 0
                width: 22
                height: 22
                radius: 11
                color: Qt.rgba(markerItem.ink.r, markerItem.ink.g, markerItem.ink.b, timeline.markerHeld || markerArea.containsMouse ? 0.35 : 0.18)
                scale: timeline.markerHeld ? 1.25 : 1
                Behavior on scale { NumberAnimation { duration: 120; easing.type: Easing.OutCubic } }
                Behavior on color { ColorAnimation { duration: 120 } }
            }
            Rectangle {
                anchors.horizontalCenter: parent.horizontalCenter
                y: 11
                width: 2
                height: parent.height - 11 - 4
                radius: 1
                color: markerItem.ink
            }
            Rectangle {
                anchors.horizontalCenter: parent.horizontalCenter
                y: 4
                width: 14
                height: 14
                radius: 7
                color: markerItem.ink
                border.color: Qt.rgba(timeline.theme.windowBg.r, timeline.theme.windowBg.g, timeline.theme.windowBg.b, 0.85)
                border.width: 1.5
                scale: timeline.markerHeld ? 1.2 : markerArea.containsMouse ? 1.1 : 1
                Behavior on scale { NumberAnimation { duration: 120; easing.type: Easing.OutCubic } }
            }
            MouseArea {
                id: markerArea
                anchors.fill: parent
                anchors.margins: -4
                hoverEnabled: true
                cursorShape: pressed ? Qt.ClosedHandCursor : Qt.OpenHandCursor
                preventStealing: true
                property real grab: 0          // where on the marker it was taken, so it doesn't jump
                onPressed: mouse => {
                    grab = mouse.x - width / 2
                    timeline.markerHeld = true
                }
                onPositionChanged: mouse => {
                    if (!pressed)
                        return
                    const inRow = mapToItem(list, mouse.x - grab, 0).x + list.contentX
                    timeline.markerDragged(Math.max(0, Math.min(timeline.items.length, timeline.markerAt(inRow))))
                }
                onReleased: { timeline.markerHeld = false; timeline.markerDropped() }
                onCanceled: { timeline.markerHeld = false; timeline.markerDropped() }
            }
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
            model: timeline.items
            // A new action scrolls into view (not when a run only opens or folds).
            Connections {
                target: timeline
                function onHistoryChanged() { if (!timeline.frozen) Qt.callLater(list.positionViewAtEnd); breakBtn.hideNow() }
            }

            delegate: Item {
                id: slot
                required property var modelData
                required property int index
                width: modelData.folded ? timeline.pitch + 14 : timeline.pitch
                height: list.height
                readonly property bool hot: dotArea.containsMouse
                // Behind the marker: greyed, as Fusion does (still there, put back by moving the marker).
                opacity: 0.32 + 0.68 * Math.max(0, Math.min(1, timeline.marker - slot.index))

                // A step with an issue is ringed in its colour, with a small mark.
                Rectangle {
                    visible: !!slot.modelData.issue
                    anchors.centerIn: parent
                    anchors.horizontalCenterOffset: slot.modelData.folded ? -6 : 0
                    width: 24
                    height: 24
                    radius: 12
                    color: Qt.rgba(1, 1, 1, 0)
                    border.width: 2
                    border.color: timeline.issueColor(slot.modelData.issue)
                    Rectangle {
                        x: parent.width - 8
                        y: -4
                        width: 11
                        height: 11
                        radius: 6
                        color: timeline.issueColor(slot.modelData.issue)
                        Text {
                            anchors.centerIn: parent
                            text: "!"
                            color: timeline.theme.windowBg
                            font.pixelSize: 9
                            font.bold: true
                        }
                    }
                }
                Rectangle {
                    visible: !slot.modelData.move && !slot.modelData.link && !slot.modelData.breaks && !slot.modelData.eye && !slot.modelData.sym
                    anchors.centerIn: parent
                    width: timeline.dot
                    height: timeline.dot
                    radius: width / 2
                    color: timeline.dotColor
                    border.color: Qt.rgba(timeline.theme.windowBg.r, timeline.theme.windowBg.g, timeline.theme.windowBg.b, 0.8)
                    border.width: 1.5
                    scale: slot.hot ? 1.6 : 1
                    Behavior on scale { NumberAnimation { duration: 140; easing.type: Easing.OutCubic } }
                }

                // A projection's step: a chain (dimmed once its link is broken); Break Link's step:
                // the same chain pulled apart.
                ChainIcon {
                    visible: !!slot.modelData.link || !!slot.modelData.breaks
                    anchors.centerIn: parent
                    width: timeline.icon
                    height: timeline.icon
                    broken: !!slot.modelData.breaks
                    ink: timeline.dotColor
                    opacity: slot.modelData.linkBroken ? 0.5 : 1
                    scale: slot.hot ? 1.18 : 1
                    Behavior on scale { NumberAnimation { duration: 140; easing.type: Easing.OutCubic } }
                }

                // A constraint or dimension: its own symbol (the same ones as the function search).
                SketchIcon {
                    visible: !!slot.modelData.sym
                    anchors.centerIn: parent
                    width: timeline.icon
                    height: timeline.icon
                    full: true
                    kind: slot.modelData.sym || ""
                    ink: timeline.dotColor
                    scale: slot.hot ? 1.18 : 1
                    Behavior on scale { NumberAnimation { duration: 140; easing.type: Easing.OutCubic } }
                }

                // Hidden or shown: the Browser's eye, shut for hidden, open for shown.
                EyeIcon {
                    visible: !!slot.modelData.eye
                    anchors.centerIn: parent
                    width: timeline.icon
                    open: slot.modelData.eye === "open"
                    ink: timeline.dotColor
                    scale: slot.hot ? 1.18 : 1
                    Behavior on scale { NumberAnimation { duration: 140; easing.type: Easing.OutCubic } }
                }

                // A move: a cross with an arrow head at each of its four ends.
                Canvas {
                    id: moveIcon
                    visible: !!slot.modelData.move
                    anchors.centerIn: parent
                    anchors.horizontalCenterOffset: slot.modelData.folded ? -6 : 0
                    width: timeline.icon
                    height: timeline.icon
                    scale: slot.hot ? 1.18 : 1
                    Behavior on scale { NumberAnimation { duration: 140; easing.type: Easing.OutCubic } }
                    property color ink: timeline.dotColor
                    onInkChanged: requestPaint()
                    onPaint: {
                        const c = getContext("2d")
                        c.reset()
                        c.scale(width / 18, height / 18)          // drawn in 18, scaled to the standard size (GB-060)
                        c.strokeStyle = ink
                        c.fillStyle = ink
                        c.lineWidth = 1.6
                        c.lineCap = "round"
                        const m = 9, e = 1.5, h = 3.6     // centre, tip inset, arrow head size
                        c.beginPath()
                        c.moveTo(m, e + h * 0.6); c.lineTo(m, 18 - e - h * 0.6)
                        c.moveTo(e + h * 0.6, m); c.lineTo(18 - e - h * 0.6, m)
                        c.stroke()
                        const head = (tx, ty, dx, dy) => {        // tip at (tx,ty), pointing along (dx,dy)
                            c.beginPath()
                            c.moveTo(tx, ty)
                            c.lineTo(tx - dx * h - dy * h * 0.85, ty - dy * h + dx * h * 0.85)
                            c.lineTo(tx - dx * h + dy * h * 0.85, ty - dy * h - dx * h * 0.85)
                            c.closePath()
                            c.fill()
                        }
                        head(m, e, 0, -1); head(m, 18 - e, 0, 1)
                        head(e, m, -1, 0); head(18 - e, m, 1, 0)
                    }
                }
                // How many moves a folded icon holds.
                Rectangle {
                    visible: !!slot.modelData.folded
                    x: moveIcon.x + moveIcon.width - 2
                    y: moveIcon.y - 5
                    width: Math.max(14, countText.implicitWidth + 7)
                    height: 14
                    radius: 7
                    color: timeline.dotColor
                    Text {
                        id: countText
                        anchors.centerIn: parent
                        text: slot.modelData.count || ""
                        color: timeline.theme.windowBg
                        font.pixelSize: 10
                        font.bold: true
                    }
                }
                MouseArea {
                    id: dotArea
                    objectName: "dotArea"
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: slot.modelData.sketch ? Qt.PointingHandCursor : Qt.ArrowCursor
                    onDoubleClicked: if (slot.modelData.sketch && !slot.modelData.future) timeline.sketchEditRequested(slot.modelData.sketch)
                    function hover(containsMouse) {
                        const run = slot.modelData.run !== undefined ? slot.modelData.run : -1
                        if (containsMouse) {
                            tip.showFor(slot, slot.modelData.summary + (slot.modelData.issueText ? "  ·  " + slot.modelData.issueText : ""))
                            if (run >= 0) {
                                if (timeline.openRun === run)
                                    foldTimer.stop()                  // already open: keep it open
                                else {
                                    openTimer.run = run               // rest here for a moment, then it opens
                                    openTimer.restart()
                                }
                            }
                            if (timeline.sketchId && slot.modelData.link && !slot.modelData.linkBroken)
                                breakBtn.arm(slot, slot.modelData.link)
                        } else {
                            tip.hideFor(slot)
                            if (openTimer.run === run)
                                openTimer.stop()
                            if (timeline.openRun >= 0)
                                foldTimer.restart()                   // folds a few seconds after leaving
                            breakBtn.leave(slot)
                        }
                    }
                    onContainsMouseChanged: hover(containsMouse)
                }
            }
        }
    }

    // Break Link (owner, 2026-11-01): not on show anywhere. Resting the mouse on a projection's
    // step for a moment makes it pop up above the step; it can't be clicked until it has
    // finished appearing, and a click only asks (Main's card), nothing breaks yet.
    Rectangle {
        id: breakBtn
        objectName: "breakBtn"
        property Item owner: null
        property int link: 0
        property bool shown: false
        property bool armed: false
        function arm(item, id) {
            owner = item
            link = id
            hideTimer.stop()
            if (!shown)
                showTimer.restart()
        }
        function leave(item) {
            if (owner !== item)
                return
            showTimer.stop()
            if (shown)
                hideTimer.restart()
        }
        function hideNow() {
            showTimer.stop()
            hideTimer.stop()
            armTimer.stop()
            shown = false
            armed = false
        }
        Timer {
            id: showTimer
            interval: timeline.breakDelay
            onTriggered: { breakBtn.shown = true; armTimer.restart() }
        }
        Timer {
            id: armTimer
            interval: 450
            onTriggered: breakBtn.armed = true
        }
        Timer {
            id: hideTimer
            interval: 500
            onTriggered: breakBtn.hideNow()
        }

        readonly property real baseY: row.y + list.y - height - 2
        x: owner ? Math.max(0, Math.min(owner.mapToItem(timeline, owner.width / 2, 0).x - width / 2, timeline.width - width)) : 0
        y: shown ? baseY : baseY + 16               // rises into place
        width: breakText.implicitWidth + 44
        height: 30
        radius: 8
        opacity: shown ? 1 : 0
        visible: opacity > 0
        color: breakArea.containsMouse && armed ? timeline.theme.controlHover
             : Qt.rgba(timeline.theme.panelBg.r, timeline.theme.panelBg.g, timeline.theme.panelBg.b, 0.97)
        border.color: armed ? timeline.theme.borderStrong : timeline.theme.border
        Behavior on y { NumberAnimation { duration: 200; easing.type: Easing.OutCubic } }
        Behavior on opacity { NumberAnimation { duration: 180 } }
        Behavior on color { ColorAnimation { duration: 120 } }

        ChainIcon {
            x: 9
            anchors.verticalCenter: parent.verticalCenter
            broken: true
            ink: breakBtn.armed ? timeline.theme.textPrimary : timeline.theme.textSecondary
        }
        Text {
            id: breakText
            x: 32
            anchors.verticalCenter: parent.verticalCenter
            text: "Break Link"
            color: breakBtn.armed ? timeline.theme.textPrimary : timeline.theme.textSecondary
            font.pixelSize: 12
        }
        MouseArea {
            id: breakArea
            objectName: "breakArea"
            anchors.fill: parent
            hoverEnabled: true
            enabled: breakBtn.shown
            cursorShape: breakBtn.armed ? Qt.PointingHandCursor : Qt.ArrowCursor
            onContainsMouseChanged: if (containsMouse) hideTimer.stop(); else if (breakBtn.shown) hideTimer.restart()
            onClicked: {
                if (!breakBtn.armed)
                    return
                const id = breakBtn.link
                breakBtn.hideNow()
                timeline.breakLinkRequested(timeline.sketchId, id)
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

        y: row.y + list.y - height - 2 - (breakBtn.shown && breakBtn.owner === owner ? 34 : 0)
        Behavior on y { NumberAnimation { duration: 180; easing.type: Easing.OutCubic } }
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
