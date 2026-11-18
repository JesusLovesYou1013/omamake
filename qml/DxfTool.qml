import QtQuick

// EXPORT DXF (owner, 2026-11-13), one sketch laid flat, step by step with a guide saying what's next; the sketch
// itself is never edited:
//   1 Sketch          click it in the Browser. It's shown facing you as it was drawn, filling the view.
//   2 Side            the front faces you. To export it seen from the back, turn the view (ViewCube) and click
//                     any point, line or area of the sketch: the side facing you then is the one exported.
//   3 Bottom line     two clicks: a line for this export only (never in the sketch or the file, dimensions or
//                     not). It's the direction of the page's bottom edge, nothing more: the whole sketch is on the
//                     page wherever the line is. It snaps level or upright on screen, as a sketch Line does.
//   4 Measure from    a line of the sketch (drawn, construction or projected) or an origin axis lying in the
//                     sketch's plane (the others are hidden while the tool is open).
//   5 Parallel, Perpendicular or an Angle (degrees) between the two. The line stays where it was drawn; the
//     SKETCH turns on screen (a turn of the view about the line of sight) so it sits against the line as set,
//     which is how it lies on the page (owner, 2026-11-13).
// Steps 2 to 5 can be skipped (Next), or all at once (owner, 2026-11-13): the sketch picked, Export (with or
// without "Include dimensions") keeps the defaults, the front facing you and the sketch as it was drawn. "Include dimensions" (off:
// most DXFs go to a laser or waterjet shop) puts every dimension in as lines and text. Exported, it's listed
// under its sketch in the Browser; a double-click there brings it all back (reopen()), so exporting again gives
// the same sheet. Document::exportSketchDxf writes it. Cancel, or Esc twice quickly (GB-081), leaves.
Item {
    id: tool
    required property var theme
    property int fadeMs: 250
    property bool open: false
    // From Main: where things are on screen, the view, and what's under the pointer.
    property var toScreen: null              // engine point -> screen
    property var rayAt: null                 // screen -> {origin, direction} (engine)
    property var viewForward: null           // () -> the camera's forward (scene)
    property var lookAlong: null             // (scene dir, {center, radius} or null) -> turns the view
    property var pickLine: null              // (sketch, x, y) -> id of the sketch line under the pointer, or 0
    property var pickPart: null              // (sketch, x, y) -> a point/line/arc/curve/area of it, or null
    property var hitOrigin: null             // (x, y) -> the origin part under the pointer ({part, name}) or null
    property var showHover: null             // (part or null) -> lit as the app lights what's under the mouse
    property var rollTo: null                // (degrees) -> turns the view about the line of sight
    property real levelDegrees: 2            // the bottom line snaps level/upright this near (the sketch Line's)
    property string hold: ""                 // "Horizontal" / "Vertical" while the line being drawn is snapped
    property int viewTick: 0
    property var picked: []                  // [{id, name}]: one sketch
    readonly property var pickedMap: { const m = {}; for (const s of picked) m[s.id] = true; return m }
    property int step: 1                     // the step the guide is on
    property bool flip: false                // seen from the sketch's back
    property var lineA: null                 // the bottom line on the sketch, {u, v}
    property var lineB: null
    property var hover: null                 // the line's free end while it's drawn
    property var ref: null                   // what it's measured from: {kind: "line", id} or {kind: "axis", axis}
    property string relation: ""             // "", "parallel", "perpendicular", "angle"
    property real angle: 0                   // degrees, counter-clockwise as seen
    property bool dims: false                // "Include dimensions": off by default (owner)
    property int row: -1                     // the Browser's export row this was reopened from
    readonly property bool hasLine: !!lineA && !!lineB
    readonly property bool wantsClicks: open && picked.length > 0
    signal say(string text, int ms)
    signal exportRequested(int id, var way)

    visible: open || fadeOut.running
    opacity: open ? 1 : 0
    Behavior on opacity { NumberAnimation { id: fadeOut; duration: tool.fadeMs } }

    readonly property var plane: picked.length ? doc.sketchPlane(picked[0].id) : null
    // The origin's axes lying in the sketch's plane (the only ones it can be measured from); the rest of the
    // origin is hidden while a sketch is picked. null: the origin as the Browser has it.
    readonly property var coplanarAxes: {
        const p = plane
        if (!p)
            return []
        const n = p.normal, off = Math.abs(p.origin.dotProduct(n)), out = []
        for (const [name, d] of [["X", Qt.vector3d(1, 0, 0)], ["Y", Qt.vector3d(0, 1, 0)], ["Z", Qt.vector3d(0, 0, 1)]])
            if (Math.abs(d.dotProduct(n)) < 1e-6 && off < 1e-4)
                out.push(name)
        return out
    }
    readonly property var axesShown: open && picked.length ? { X: coplanarAxes.includes("X"), Y: coplanarAxes.includes("Y"), Z: coplanarAxes.includes("Z") } : null
    function way() {
        return { flip: flip, bottom: hasLine ? [lineA.u, lineA.v, lineB.u, lineB.v] : [], relation: ref ? relation : "",
                 ref: ref || {}, angle: angle, dims: dims, row: row }
    }
    // The bottom line as the export will use it (measured as set), [u0, v0, u1, v1] or [].
    readonly property var shownLine: picked.length && hasLine ? doc.dxfBottomLine(picked[0].id, way()) : []
    // The view's turn that shows the sketch against the line as set: the line keeps the direction it was drawn
    // in on screen, the sketch turns under it by the angle between the drawn line and the measured one (as seen).
    readonly property real sketchTurn: {
        const L = shownLine
        if (L.length !== 4 || !hasLine)
            return 0
        const s = flip ? -1 : 1
        const drawn = Math.atan2(lineB.v - lineA.v, s * (lineB.u - lineA.u)), set = Math.atan2(L[3] - L[1], s * (L[2] - L[0]))
        return (drawn - set) * 180 / Math.PI
    }
    property real viewRoll: 0                // the view's turn now (Navigation.roll)
    // Turns the view to sketchTurn about the bottom line's middle, so the line stays where it is on screen.
    function turnView() {
        if (!rollTo)
            return
        if (!hasLine)
            return rollTo(0)
        const m = onSketch({ u: (lineA.u + lineB.u) / 2, v: (lineA.v + lineB.v) / 2 })
        rollTo(sketchTurn, Qt.vector3d(m.x, m.z, -m.y))    // engine -> scene
    }
    onSketchTurnChanged: if (open) turnView()

    // What the guide says on each step.
    readonly property var guides: [
        "",
        "Click the sketch to export in the Browser on the left.",
        "Export now keeps the defaults: the front, as drawn. To export the back, turn the view (ViewCube) and click the sketch; or Next to set the bottom line.",
        "Click twice to draw the bottom line: the direction of the page's bottom edge (it snaps level or upright). It's never in the file. Next skips it (the page is as drawn).",
        "Now click the sketch line (or origin axis) to measure from: it lights up under the pointer, and turns green when picked.",
        "Parallel, Perpendicular or Angle: the sketch turns on screen so the green line sits that way to the bottom line. Then Export."
    ]
    readonly property string guide: step === 4 && relation ? relationName(relation) + ": click the sketch line (or origin axis) to measure from. It lights up under the pointer."
                                  : step === 5 && relation ? (relation === "angle" ? "Type the angle between the green line and the bottom line; the sketch turns to match. Then Export."
                                                                                   : "The green line is " + relation + " to the bottom line. Export saves the DXF as it's shown.")
                                  : guides[step]
    function relationName(r) { return r === "angle" ? "Angle" : r === "parallel" ? "Parallel" : "Perpendicular" }

    function hovered(part) { if (showHover) showHover(part) }
    function clearAll() {
        picked = []
        step = 1
        flip = false
        lineA = lineB = hover = null
        ref = null
        relation = ""
        angle = 0
        angleIn.text = ""
        row = -1
        hold = ""
        hovered(null)
    }
    function begin() {
        clearAll()
        dims = false
        open = true
    }
    function cancel() { open = false; clearAll(); if (rollTo) rollTo(0) }
    // The sketch facing you (from its back when flipped), filling the view, as when it's opened to edit.
    function face() {
        const p = plane
        if (!p || !lookAlong)
            return
        const g = doc.sketchGeometry(picked[0].id)
        let lo = null, hi = null
        for (const q of g.points || []) {
            lo = lo ? { u: Math.min(lo.u, q.u), v: Math.min(lo.v, q.v) } : { u: q.u, v: q.v }
            hi = hi ? { u: Math.max(hi.u, q.u), v: Math.max(hi.v, q.v) } : { u: q.u, v: q.v }
        }
        let around = null
        if (lo) {
            const c = onSketch({ u: (lo.u + hi.u) / 2, v: (lo.v + hi.v) / 2 })
            around = { center: Qt.vector3d(c.x, c.z, -c.y), radius: Math.max(1, Math.hypot(hi.u - lo.u, hi.v - lo.v) / 2) }
        }
        const n = p.normal
        lookAlong(flip ? Qt.vector3d(-n.x, -n.z, n.y) : Qt.vector3d(n.x, n.z, -n.y), around)     // engine -> scene
        if (rollTo && sketchTurn)
            turnLater.start()                     // after the view has flown round (it straightens the turn)
    }
    Timer { id: turnLater; interval: 400; onTriggered: if (tool.open) tool.turnView() }
    // The Browser's click: that sketch, shown, facing you as it was drawn.
    function toggle(id, name) {
        if (picked.length && picked[0].id === id)
            return
        const keepDims = dims
        clearAll()
        dims = keepDims
        picked = [{ id: id, name: name }]
        doc.setShown("s" + id, true)
        face()
        step = 2
    }
    // A double-click on an export row in the Browser: everything as it was exported.
    function reopen(r) {
        const e = doc.dxfExport(r)
        if (!e.sketch)
            return say("That export's sketch is gone.", 0)
        clearAll()
        open = true
        const w = e.way
        picked = [{ id: e.sketch, name: e.name }]
        row = r
        flip = !!w.flip
        const b = w.bottom || []
        if (b.length === 4) {
            lineA = { u: b[0], v: b[1] }
            lineB = { u: b[2], v: b[3] }
        }
        ref = w.ref && w.ref.kind ? w.ref : null
        relation = ref ? (w.relation || "") : ""
        angle = w.angle || 0
        angleIn.text = relation === "angle" ? String(angle) : ""
        dims = !!w.dims
        doc.setShown("s" + e.sketch, true)
        face()
        step = 5
        say("Export " + e.path.split("/").pop() + " again: change any step, then Export.", 0)
    }
    function goTo(n) {
        if (!picked.length)
            return
        step = n
        hover = null
        hovered(null)
    }
    function next() { if (picked.length && step < 5) goTo(!hasLine && step >= 3 ? 5 : step + 1) }
    function onPlane(x, y) {
        const p = plane
        if (!p || !rayAt)
            return null
        const r = rayAt(x, y)
        const along = r.direction.dotProduct(p.normal)
        if (Math.abs(along) < 1e-9)
            return null
        const t = p.origin.minus(r.origin).dotProduct(p.normal) / along
        const at = r.origin.plus(r.direction.times(t)).minus(p.origin)
        return { u: at.dotProduct(p.xAxis), v: at.dotProduct(p.yAxis) }
    }
    function onSketch(q) { const p = plane; return p.origin.plus(p.xAxis.times(q.u)).plus(p.yAxis.times(q.v)) }
    // The bottom line's free end under the pointer, snapped level or upright ON SCREEN when it's within
    // levelDegrees of it (as a sketch Line snaps), so Parallel/Perpendicular plainly look it. Sets `hold`.
    function lineEnd(x, y) {
        hold = ""
        if (lineA && toScreen) {
            const a = toScreen(onSketch(lineA)), dx = x - a.x, dy = y - a.y
            if (Math.hypot(dx, dy) > 3) {
                const deg = Math.atan2(Math.abs(dy), Math.abs(dx)) * 180 / Math.PI
                if (deg <= levelDegrees) { y = a.y; hold = "Horizontal" }
                else if (deg >= 90 - levelDegrees) { x = a.x; hold = "Vertical" }
            }
        }
        return onPlane(x, y)
    }
    // Step 4's target under the pointer: a line of the sketch, else an axis in its plane.
    function refAt(x, y) {
        const id = pickLine ? pickLine(picked[0].id, x, y) : 0
        if (id)
            return { ref: { kind: "line", id: id }, part: { kind: "sketch_line", sketch: picked[0].id, index: id } }
        const o = hitOrigin ? hitOrigin(x, y) : null
        if (o && o.part === "axis" && coplanarAxes.includes(o.name))
            return { ref: { kind: "axis", axis: o.name }, part: { kind: "origin", name: o.name, part: "axis", index: -1 } }
        return null
    }
    function move(x, y) {
        if (step === 2)
            hovered(pickPart ? pickPart(picked[0].id, x, y) : null)
        else if (step === 3 && lineA && !lineB)
            hover = lineEnd(x, y)
        else if (step === 4) {
            const r = refAt(x, y)
            hovered(r ? r.part : null)
        }
    }
    function click(x, y) {
        if (step === 2) {
            if (!(pickPart && pickPart(picked[0].id, x, y)))
                return say("Click a point, line or area of the sketch, from the side that should face you.", 0)
            const n = plane.normal, f = viewForward()
            flip = f.x * n.x + f.y * n.z - f.z * n.y > 0                 // looking along the normal: its back faces you
            say((flip ? "The back" : "The front") + " of the sketch will face you.", 0)
            return goTo(3)
        }
        if (step === 3) {
            if (!lineA || lineB) {
                const q = onPlane(x, y)
                if (!q)
                    return
                lineA = q
                lineB = null
                hover = q
                return
            }
            const q = lineEnd(x, y)
            if (!q)
                return
            // Start to end is the page's left to right, so it reads left to right on screen as drawn (an upright
            // line: as drawn); the sheet is never turned upside down by the order of the clicks.
            const a = toScreen(onSketch(lineA)), b = toScreen(onSketch(q))
            if (b.x < a.x - 0.5) { lineB = lineA; lineA = q } else lineB = q
            hover = null
            hold = ""
            return goTo(4)
        }
        if (step === 4) {
            const r = refAt(x, y)
            if (!r)
                return say("Click a line of the sketch, or an origin axis shown in its plane.", 0)
            ref = r.ref
            goTo(5)
            if (relation === "angle")
                focusAngle()
            return
        }
    }
    // Parallel / Perpendicular / Angle. Picked before the line to measure from, it waits for that (step 4).
    function setRelation(r) {
        if (!hasLine)
            return goTo(3)
        relation = relation === r ? "" : r
        if (!ref)
            return goTo(4)
        if (relation === "angle")
            focusAngle()
    }
    function focusAngle() {
        if (!angleIn.text)
            angleIn.text = String(angle)
        angleIn.forceActiveFocus()
        angleIn.selectAll()
    }
    // One Esc (GB-081): a bottom line with only its first end clicked is dropped (the step stays, ready to
    // click again); what's under the pointer stops being lit. True if a line was being drawn.
    function stopLine() {
        hovered(null)
        if (!(step === 3 && lineA && !lineB))
            return false
        lineA = hover = null
        hold = ""
        return true
    }
    function clearSide() { flip = false; face(); goTo(2) }
    function clearLine() { lineA = lineB = hover = null; ref = null; relation = ""; angleIn.text = ""; goTo(3) }
    function clearRef() { ref = null; goTo(hasLine ? 4 : 3) }
    function done() {
        if (!picked.length)
            return say("Click a sketch in the Browser first.", 0)
        hovered(null)
        exportRequested(picked[0].id, way())
    }

    // The bottom line (dashed) and what it's measured from (solid), drawn over the view, never in the sketch.
    Canvas {
        id: overlay
        anchors.fill: parent
        visible: tool.picked.length > 0
        property int tick: tool.viewTick
        onTickChanged: requestPaint()
        Connections {
            target: tool
            function onShownLineChanged() { overlay.requestPaint() }
            function onLineAChanged() { overlay.requestPaint() }
            function onHoverChanged() { overlay.requestPaint() }
            function onHoldChanged() { overlay.requestPaint() }
            function onViewRollChanged() { overlay.requestPaint() }
            function onRefChanged() { overlay.requestPaint() }
        }
        onPaint: {
            const ctx = getContext("2d")
            ctx.reset()
            if (!tool.toScreen || !tool.plane || !tool.lineA)
                return
            if (tool.ref && tool.ref.kind === "line") {
                const g = doc.sketchGeometry(tool.picked[0].id)
                const l = (g.lines || []).find(k => k.id === tool.ref.id)
                const pts = {}
                for (const p of g.points || [])
                    pts[p.id] = p
                if (l && pts[l.a] && pts[l.b]) {
                    const p = tool.toScreen(tool.onSketch(pts[l.a])), q = tool.toScreen(tool.onSketch(pts[l.b]))
                    ctx.strokeStyle = tool.theme.success
                    ctx.lineWidth = 4
                    ctx.globalAlpha = 0.7
                    ctx.beginPath(); ctx.moveTo(p.x, p.y); ctx.lineTo(q.x, q.y); ctx.stroke()
                    ctx.globalAlpha = 1
                }
            }
            const L = tool.shownLine
            const b = L.length === 4 ? { u: L[2], v: L[3] } : tool.hover
            if (!b)
                return
            let p = tool.toScreen(tool.onSketch(tool.lineA)), q = tool.toScreen(tool.onSketch(b))
            if (L.length === 4) {
                // About the drawn line's middle, along the measured direction turned by what the view has still to
                // turn: so the line never moves while the sketch turns under it.
                const m = tool.toScreen(tool.onSketch({ u: (tool.lineA.u + tool.lineB.u) / 2, v: (tool.lineA.v + tool.lineB.v) / 2 }))
                const left = (((tool.sketchTurn - tool.viewRoll) % 360) + 540) % 360 - 180
                const a = Math.atan2(q.y - p.y, q.x - p.x) - left * Math.PI / 180, h = Math.hypot(q.x - p.x, q.y - p.y) / 2
                p = { x: m.x - h * Math.cos(a), y: m.y - h * Math.sin(a) }
                q = { x: m.x + h * Math.cos(a), y: m.y + h * Math.sin(a) }
            }
            ctx.strokeStyle = tool.theme.accent
            ctx.lineWidth = 3
            ctx.setLineDash([8, 5])
            ctx.beginPath(); ctx.moveTo(p.x, p.y); ctx.lineTo(q.x, q.y); ctx.stroke()
            ctx.fillStyle = tool.theme.accent
            for (const s of [p, q]) { ctx.beginPath(); ctx.arc(s.x, s.y, 4, 0, 2 * Math.PI); ctx.fill() }
            if (tool.hold && !L.length) {
                ctx.font = "12px sans-serif"
                ctx.fillStyle = tool.theme.textPrimary
                ctx.fillText(tool.hold, q.x + 12, q.y - 10)
            }
        }
    }

    // A chip: fixed width (nothing moves when its words change); `now` is the step the guide is on (lit),
    // `usable` false dims it; a ✕ on the right clears what it holds.
    component Chip: Rectangle {
        id: chip
        property string label
        property bool on: false
        property bool now: false
        property bool clearable: false
        property bool usable: true
        property color fill: Qt.rgba(tool.theme.panelBg.r, tool.theme.panelBg.g, tool.theme.panelBg.b, 0.92)
        property color ink: on || now ? tool.theme.textPrimary : tool.theme.textSecondary
        signal clicked()
        signal cleared()
        height: 34
        width: 120
        radius: 8
        color: fill
        opacity: usable ? 1 : 0.45
        border.color: now ? tool.theme.accent : on ? tool.theme.success : chipArea.containsMouse && usable ? tool.theme.textSecondary : tool.theme.border
        border.width: now ? 2.5 : 1.5
        Text {
            anchors.left: parent.left
            anchors.right: clearBox.visible ? clearBox.left : parent.right
            anchors.leftMargin: 10
            anchors.rightMargin: clearBox.visible ? 2 : 10
            anchors.verticalCenter: parent.verticalCenter
            horizontalAlignment: clearBox.visible ? Text.AlignLeft : Text.AlignHCenter
            elide: Text.ElideRight
            text: chip.label
            color: chip.ink
            font.pixelSize: 12
            font.bold: chip.now
        }
        MouseArea { id: chipArea; anchors.fill: parent; hoverEnabled: true; enabled: chip.usable; cursorShape: Qt.PointingHandCursor; onClicked: chip.clicked() }
        Rectangle {
            id: clearBox
            visible: chip.clearable
            anchors.right: parent.right
            anchors.rightMargin: 5
            anchors.verticalCenter: parent.verticalCenter
            width: 24; height: 24; radius: 6
            color: clearArea.containsMouse ? tool.theme.controlHover : "transparent"
            Text { anchors.centerIn: parent; text: "✕"; color: tool.theme.textSecondary; font.pixelSize: 12 }
            MouseArea { id: clearArea; anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.PointingHandCursor; onClicked: chip.cleared() }
        }
    }
    Column {
        objectName: "dxfOptions"
        visible: tool.open
        anchors.top: parent.top
        anchors.topMargin: 14
        anchors.horizontalCenter: parent.horizontalCenter
        spacing: 8
        // The steps, in order: lit is where the guide is, ✓ is done; a click goes back to one.
        Row {
            anchors.horizontalCenter: parent.horizontalCenter
            spacing: 8
            Chip {
                objectName: "dxfSketch"; width: 200
                label: tool.picked.length ? "✓  1  Sketch:  " + tool.picked[0].name : "1  Sketch"
                now: tool.step === 1; on: tool.picked.length > 0; clearable: tool.picked.length > 0
                onCleared: tool.clearAll()
            }
            Chip {
                objectName: "dxfSide"; width: 210; usable: tool.picked.length > 0
                label: (tool.step > 2 ? "✓  " : "") + "2  Side:  " + (tool.flip ? "back faces you" : "front faces you")
                now: tool.step === 2; on: tool.step > 2; clearable: tool.flip
                onClicked: tool.goTo(2)
                onCleared: tool.clearSide()
            }
            Chip {
                objectName: "dxfLine"; width: 170; usable: tool.picked.length > 0
                label: (tool.hasLine ? "✓  " : "") + "3  Bottom line"
                now: tool.step === 3; on: tool.hasLine; clearable: !!tool.lineA
                onClicked: tool.goTo(3)
                onCleared: tool.clearLine()
            }
            Chip {
                objectName: "dxfRef"; width: 230; usable: tool.hasLine
                label: tool.ref ? "✓  4  Measure from:  " + (tool.ref.kind === "axis" ? tool.ref.axis + " axis" : "sketch line") : "4  Measure from"
                now: tool.step === 4; on: !!tool.ref; clearable: !!tool.ref
                onClicked: tool.goTo(4)
                onCleared: tool.clearRef()
            }
        }
        Row {
            anchors.horizontalCenter: parent.horizontalCenter
            spacing: 8
            Chip { objectName: "dxfParallel"; width: 100; usable: tool.hasLine; label: "5  Parallel"; now: tool.step === 5 && !tool.relation; on: tool.relation === "parallel"; onClicked: tool.setRelation("parallel") }
            Chip { objectName: "dxfPerpendicular"; width: 120; usable: tool.hasLine; label: "Perpendicular"; now: tool.step === 5 && !tool.relation; on: tool.relation === "perpendicular"; onClicked: tool.setRelation("perpendicular") }
            Chip { objectName: "dxfAngleChip"; width: 80; usable: tool.hasLine; label: "Angle"; now: tool.step === 5 && !tool.relation; on: tool.relation === "angle"; onClicked: tool.setRelation("angle") }
            Rectangle {
                height: 34; width: 90; radius: 8
                opacity: tool.relation === "angle" ? 1 : 0.45
                color: Qt.rgba(tool.theme.panelBg.r, tool.theme.panelBg.g, tool.theme.panelBg.b, 0.92)
                border.color: angleIn.activeFocus ? tool.theme.accent : tool.theme.border
                border.width: 1.5
                TextInput {
                    id: angleIn
                    objectName: "dxfAngle"
                    anchors.fill: parent; anchors.margins: 8
                    verticalAlignment: TextInput.AlignVCenter
                    enabled: tool.relation === "angle"
                    color: tool.theme.textPrimary; font.pixelSize: 12
                    selectByMouse: true
                    onTextEdited: { const d = parseFloat(text); if (!isNaN(d)) tool.angle = d }
                    Text { visible: !angleIn.text && !angleIn.activeFocus; anchors.verticalCenter: parent.verticalCenter; text: "degrees"; color: tool.theme.textSecondary; font.pixelSize: 12 }
                }
            }
            Item { width: 12; height: 1 }
            Chip {
                objectName: "dxfDims"; width: 190; usable: tool.picked.length > 0
                label: (tool.dims ? "☑" : "☐") + "  Include dimensions"; on: tool.dims
                onClicked: tool.dims = !tool.dims
            }
            Chip { objectName: "dxfCancel"; width: 80; label: "Cancel"; onClicked: tool.cancel() }
            Chip {
                objectName: "dxfExport"; width: 90; usable: tool.picked.length > 0
                label: "Export"
                fill: tool.theme.success
                ink: tool.theme.windowBg
                onClicked: tool.done()
            }
        }
        // The guide: what to do now, and Next to skip a step that isn't needed.
        Row {
            anchors.horizontalCenter: parent.horizontalCenter
            spacing: 8
            Rectangle {
                objectName: "dxfGuide"
                width: 620; height: 52; radius: 10
                color: Qt.rgba(tool.theme.panelBg.r, tool.theme.panelBg.g, tool.theme.panelBg.b, 0.95)
                border.color: tool.theme.accent
                border.width: 1.5
                Text {
                    anchors.fill: parent; anchors.margins: 10
                    verticalAlignment: Text.AlignVCenter
                    wrapMode: Text.WordWrap
                    maximumLineCount: 2
                    elide: Text.ElideRight
                    text: (tool.step > 0 ? "Step " + tool.step + ":  " : "") + tool.guide
                    color: tool.theme.textPrimary
                    font.pixelSize: 13
                }
            }
            Chip {
                objectName: "dxfNext"; width: 90; height: 52
                usable: tool.picked.length > 0 && tool.step >= 2 && tool.step <= 4
                label: "Next  ›"
                onClicked: tool.next()
            }
        }
    }
}
