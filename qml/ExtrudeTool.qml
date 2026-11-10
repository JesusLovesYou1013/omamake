import QtQuick
import "Units.js" as Units

// EXTRUDE (owner, 2026-11-02, GB-043): pick one or more sketch profiles or flat faces on one plane (clicks in the
// view, Shift/Ctrl for more), set a distance (a length, a formula or a parameter name), drag the arrow, press Done.
// Each side is Auto (New Body in open space, Join against a face, Cut into a body) or Join / Cut / New Body /
// New Component. "Two sides" and "Symmetric" like Fusion, but each side's operation is its own. "Distance per
// area" is off by default: each area then has its own arrow and its own box, which stays where its area starts.
// Cutting into several bodies: Main.qml shows `cutChoices` as checkboxes in the browser tree (Browser.cutChoices).
Item {
    id: tool
    required property var theme
    required property var selection              // Selection.qml
    required property var previewA                // MeshGeometry for side 1's pushed shape, B for side 2
    required property var previewB
    required property var wireA                   // the same shapes as outlines (MeshGeometry with lines)
    required property var wireB
    required property var cutA                    // the exact region a cut removes, drawn strongly after a pause
    required property var cutB
    property var project: p => Qt.point(0, 0)     // a 3D point on the screen (Main.toScreen)
    property int fadeMs: 250

    property bool open: false
    // Closing hands the keys back to the window: a hidden distance box kept them, and swallowed the
    // next E (owner, 2026-11-03).
    onOpenChanged: {
        selection.clickHook = open ? viewClicked : null
        if (open)
            return
        const box = tool.Window.activeFocusItem
        for (let f = box; f; f = f.parent)
            if (f === tool) {
                box.focus = false
                return
            }
    }
    property var areas: []
    property string mode: "one"                   // "one" | "symmetric" | "two"
    property string d1Text: "1.000 in"
    property string d2Text: "1.000 in"
    property string op1: "auto"
    property string op2: "auto"
    property bool perArea: false                  // off unless the owner turns it on
    // Extent per side, Fusion's three: "distance" (the box), "object" (up to a picked face, edge or point;
    // the box is then the offset past it) or "all" (out past every shown body, the box's sign picks the way).
    property string extent1: "distance"
    property string extent2: "distance"
    property var target1: null
    property var target2: null
    property string off1Text: "0.000 in"
    property string off2Text: "0.000 in"
    property string taper1Text: "0°"
    property string taper2Text: "0°"
    property int picking: -1                      // the side whose object the next click picks, or -1
    // A click in the view while Extrude is open (owner, 2026-11-03). Ctrl or Shift adds an area or
    // takes it away, as in Fusion; the very first area needs neither. Any other plain click is the
    // object to extrude up to: a point, edge, face, profile, sketch line or origin part, one per
    // side, and the side goes To Object. Clicking empty space changes nothing.
    function viewClicked(part, adding) {
        if (!part)
            return
        const isArea = p => p.kind === "profile" || (p.kind === "face" && doc.facePlane(p.index).flat)
        const picked = selection.selected.some(p => selection.same(p, part))
        if (adding || (!areas.length && picking < 0)) {
            if (!isArea(part)) {
                say(adding ? "Only profiles and flat faces can be extruded; without Ctrl or Shift, a click picks the object to extrude up to."
                           : "Click a profile or a flat face to extrude first.", 0)
                return
            }
            selection.selected = picked ? selection.selected.filter(p => !selection.same(p, part))
                                        : selection.selected.concat([part])
            return
        }
        if (picked) {
            say("That's being extruded. Ctrl or Shift and click takes it away.", 0)
            return
        }
        setTarget(picking >= 0 ? picking : 0, part)
    }
    function setTarget(side, part) {
        const was = side === 0 ? { e: extent1, t: target1 } : { e: extent2, t: target2 }
        picking = -1
        if (side === 0) { target1 = part; extent1 = "object" } else { target2 = part; extent2 = "object" }
        const f = doc.extrudeFrame(spec())
        if (!f.ok) {
            say(f.why, 0)
            if (side === 0) { target1 = was.t; extent1 = was.e } else { target2 = was.t; extent2 = was.e }
            return
        }
        say(targetName(part) + ": the distance box is now how far past it (negative stops short)", 0)
    }
    property var lastSelected: []                 // the areas as they were, for a click that's turned away
    property var keepSelected: []                 // the areas, put back after that click
    readonly property bool perOn: perArea && extent1 === "distance"
    readonly property real rowsBottom: open ? options.y + options.height : 0    // Main puts the guide under the rows
    property var perText: []
    property var skip1: []
    property var skip2: []
    property var plan: null
    property var frame: null
    property var cutChoices: null                 // {body: cut or not} while a cut reaches several bodies
    property int cutSide: 0
    property int tick: 0                          // bumped while open so the arrows follow the view
    readonly property bool ready: areas.length > 0

    signal say(string text, int ms)   // ms: how long it stays (0: the usual)
    // Long enough to read: 2 s, and 70 ms a character (about 14 a second), up to a minute.
    function readingMs(text) { return Math.min(60000, 2000 + 70 * text.length) }
    signal finished()

    visible: open || fadeOut.running
    opacity: open ? 1 : 0
    Behavior on opacity { NumberAnimation { id: fadeOut; duration: tool.fadeMs } }

    function opOf(side) {
        return plan && plan.sides && plan.sides[side] ? plan.sides[side].op : "new"
    }
    // Cuts are red, everything that adds (new body, join, new component) is blue.
    function fillOf(side) {
        return opOf(side) === "cut" || opOf(side) === "intersect" ? Qt.rgba(0.9, 0.12, 0.12, 0.10) : Qt.rgba(0.2, 0.5, 1.0, 0.14)
    }
    function lineOf(side) {
        return opOf(side) === "cut" || opOf(side) === "intersect" ? Qt.rgba(1.0, 0.2, 0.2, 1) : Qt.rgba(0.3, 0.62, 1.0, 1)
    }
    readonly property color cutColor: Qt.rgba(0.9, 0.1, 0.1, 0.35)
    function clearCut() {
        doc.fillExtrudeCutaway(cutA, {}, 0)
        doc.fillExtrudeCutaway(cutB, {}, 1)
    }
    function drawCut() {
        if (!ready) return
        for (let s = 0; s < 2; ++s) {
            const t = s === 0 ? cutA : cutB
            if (opOf(s) === "cut" && (s === 0 || mode === "two"))
                doc.fillExtrudeCutaway(t, spec(), s)
            else
                doc.fillExtrudeCutaway(t, {}, s)
        }
    }

    function parseLen(text) {
        let s = String(text).trim()
        if (!s)
            return { ok: false }
        let sign = 1
        if (s[0] === "-") { sign = -1; s = s.slice(1).trim() }
        const v = Units.parse(s)
        if (v)
            return { ok: true, mm: sign * v.mm, formula: "" }
        if (/[A-Za-z_\u00C0-\uFFFF]/.test(s) || /[-+*\/^()]/.test(s)) {
            const r = doc.evaluateExpression(sign < 0 ? "-(" + s + ")" : s, "length")
            if (r.ok)
                return { ok: true, mm: r.value, formula: r.names.length ? String(text).trim() : "" }
            return { ok: false, why: r.error }
        }
        return { ok: false }
    }
    // Lengths shown in inches to 0.001 (owner, 2026-11-04: inches by default); what's shown is
    // exactly what's used, so a dragged length is never rounder than it looks.
    function lenText(mm) { return Units.plain(mm, "in") }
    function taperOf(text) {
        const r = Units.parseAngle(text)
        return r ? r.mm * 180 / Math.PI : NaN
    }
    function spec() {
        const d1 = parseLen(d1Text), d2 = parseLen(d2Text)
        const o1 = parseLen(off1Text), o2 = parseLen(off2Text)
        const t1 = taperOf(taper1Text), t2 = taperOf(taper2Text)
        const base = d1.ok ? d1.mm : 0
        // To Object with nothing picked yet goes by the box, so the preview stays while picking.
        return { areas: areas, mode: mode, d1: base, d2: d2.ok ? d2.mm : 0, wholeLength: false,
                 perArea: perOn ? perText.map(t => { const r = parseLen(t); return r.ok ? r.mm : base }) : [],
                 extent1: extent1 === "object" && !target1 ? "distance" : extent1,
                 extent2: extent2 === "object" && !target2 ? "distance" : extent2,
                 target1: target1 || {}, target2: target2 || {},
                 offset1: o1.ok ? o1.mm : 0, offset2: o2.ok ? o2.mm : 0,
                 taper1: isNaN(t1) ? 0 : t1, taper2: isNaN(t2) ? 0 : t2,
                 op1: op1, op2: op2, skip1: skip1, skip2: skip2 }
    }
    // The extent chips. To Object waits for the next click in the view; the areas stay picked.
    function setExtent(side, e) {
        if (e === "all" && (side === 0 ? extent1 : extent2) === "all") {     // All again: the other way, if there's anything there
            const key = side === 0 ? "d1Text" : "d2Text"
            const was = tool[key]
            const r = parseLen(was)
            tool[key] = lenText(-(r.ok ? r.mm : 25.4))
            if (!doc.extrudeFrame(spec()).ok) {
                tool[key] = was
                say("No body lies the other way to go through.", 0)
            }
            return
        }
        picking = e === "object" ? side : -1
        if (e === "object") {
            keepSelected = selection.selected.slice()
            say("Click a face, edge or point to extrude up to (Esc goes back to the distance)", 0)
        }
        if (side === 0) extent1 = e; else extent2 = e
        // All, first time: the way there are bodies to go through (Fusion's arrow starts that way too).
        if (e === "all" && ready && !doc.extrudeFrame(spec()).ok) {
            const key = side === 0 ? "d1Text" : "d2Text"
            const r = parseLen(tool[key])
            tool[key] = lenText(-(r.ok ? r.mm : 25.4))
        }
    }
    function targetName(t) {
        if (!t) return ""
        switch (t.kind) {
        case "face": return "To Face"
        case "edge": return "To Edge"
        case "point": return "To Point"
        case "sketch_point": return "To Sketch Point"
        case "sketch_line": case "sketch_arc": return "To Sketch Line"
        case "profile": return "To Sketch"
        case "origin": return "To " + t.name
        }
        return "To Object"
    }
    // A click while picking: the new part is the object; the areas are put back.
    function takeTarget() {
        const before = keepSelected.map(p => JSON.stringify(p))
        const fresh = selection.selected.filter(p => before.indexOf(JSON.stringify(p)) < 0)
        const side = picking
        picking = -1
        if (fresh.length) {
            if (side === 0) target1 = fresh[fresh.length - 1]; else target2 = fresh[fresh.length - 1]
        } else {
            picking = side                       // a click on nothing: still waiting
        }
        selection.selected = keepSelected
    }
    // A new area off the first one's plane is turned away: it can't go in the same extrude.
    function areasPicked() {
        if (!open)
            return
        const next = gather()
        if (next.length > 1 && !doc.extrudeFrame(Object.assign(spec(), { areas: next })).ok) {
            say("That isn't on the same plane as the rest, so it wasn't added. To extrude up to it, choose To Object, then click it.", 0)
            selection.selected = lastSelected     // comes back here
            return
        }
        lastSelected = selection.selected.slice()
        refresh()
    }
    function clearAreas() {
        picking = -1
        selection.clear()
    }
    // To Object's ✕: the object goes, and the next click picks another.
    function clearTarget(side) {
        if (side === 0) target1 = null; else target2 = null
        setExtent(side, "object")
    }
    // "Profiles: 2 selected", as Fusion's row reads; faces say so.
    readonly property string areasLabel: {
        const faces = areas.filter(a => a.kind === "face").length, profiles = areas.length - faces
        if (!areas.length) return "Profiles: none"
        return (faces === 0 ? "Profiles: " : profiles === 0 ? "Faces: " : "Areas: ") + areas.length + " selected"
    }
    function gather() {
        const out = []
        for (const p of selection.selected) {
            if (p.kind === "profile") {
                const i = doc.profileAt(p.sketch, p.u, p.v)
                if (i >= 0 && !out.some(a => a.kind === "sketch" && a.sketch === p.sketch && a.profile === i))
                    out.push({ kind: "sketch", sketch: p.sketch, profile: i })
            } else if (p.kind === "face" && doc.facePlane(p.index).flat) {    // a curved face can't be pushed: passed over, like Fusion
                out.push({ kind: "face", face: p.index })
            }
        }
        return out
    }
    // Nothing picked yet and exactly one closed profile in the shown sketches: it is the one to extrude.
    function pickOnlyProfile() {
        let found = null, count = 0
        for (const id of selection.profileSketches) {
            // The cached profiles: sketchProfiles also hunts for gaps, which froze the app on a
            // projected outline.
            for (const at of doc.profileSpots(id)) {
                count++
                found = { kind: "profile", sketch: id, index: doc.profileAt(id, at.u, at.v), u: at.u, v: at.v }
            }
        }
        if (count === 1)
            selection.selected = [found]
    }
    function clearPreview() {
        doc.fillExtrudePreview(previewA, wireA, {}, 0)
        doc.fillExtrudePreview(previewB, wireB, {}, 1)
        clearCut()
    }
    // Every Extrude starts blank (owner, 2026-11-03): nothing typed or picked last time carries over.
    function blank() {
        mode = "one"
        d1Text = "1.000 in"; d2Text = "1.000 in"
        op1 = "auto"; op2 = "auto"
        perArea = false; perText = []
        extent1 = "distance"; extent2 = "distance"
        target1 = null; target2 = null
        off1Text = "0.000 in"; off2Text = "0.000 in"
        taper1Text = "0°"; taper2Text = "0°"
        picking = -1
        skip1 = []; skip2 = []
        cutChoices = null; cutSide = 0
    }
    function begin() {
        blank()
        lastSelected = selection.selected.slice()
        open = true
        refresh()
        if (!ready)
            pickOnlyProfile()
        if (!ready)
            say("Click a profile or a flat face to extrude; Ctrl or Shift adds more. Then a plain click picks what to extrude up to.", 0)
        Qt.callLater(() => { d1Field.forceActiveFocus(); d1Field.selectAll() })
    }
    function cancel() {
        if (!open)
            return
        if (picking >= 0) {                      // Esc while picking an object: back to the distance
            const side = picking
            picking = -1
            if (!(side === 0 ? target1 : target2)) {
                if (side === 0) extent1 = "distance"; else extent2 = "distance"
            }
            return
        }
        open = false
        cutChoices = null
        tool.clearPreview()
        finished()
    }
    // The solid work runs off the main thread: the options go, the preview stays until the result is in.
    function done() {
        if (!ready) { cancel(); return }
        const r = doc.extrudeInBackground(spec())
        if (!r.ok) {
            say(r.why, 0)
            return
        }
        open = false
        cutChoices = null
        finished()
    }
    Connections {
        target: doc
        function onExtrudeFinished(r) {
            tool.clearPreview()
            tool.selection.clear()
            if (!r.ok)
                tool.say(r.why, tool.readingMs(r.why))
            else {
                // A note (Fuzzy profile was needed, how far off it may be) stays long enough to read.
                const text = (r.note ? r.note + " " : "") + "Extruded in " + r.seconds.toFixed(1) + " s"
                tool.say(text, r.note ? tool.readingMs(text) : 0)
            }
        }
    }
    function refresh() {
        if (!open)
            return
        areas = gather()
        while (perText.length < areas.length)
            perText = perText.concat([d1Text])
        if (!ready) {
            frame = null; plan = null; cutChoices = null
            tool.clearPreview()
            return
        }
        frame = doc.extrudeFrame(spec())
        if (!frame.ok)
            say(frame.why, 0)
        replan.restart()
        drawPreview()
    }
    // A distance, mode or operation changed: only the pushed shape is redrawn now; the join/cut look-up (the
    // slow part with many bodies) waits until the drag or typing pauses.
    function redraw() {
        if (!open || !ready)
            return
        replan.restart()
        clearCut()
        if (extent1 !== "distance" || extent2 !== "distance" || frame === null || !frame.ok)
            frame = doc.extrudeFrame(spec())
        drawPreview()
    }
    function drawPreview() {
        if (!ready) return
        doc.fillExtrudePreview(previewA, wireA, spec(), 0)
        if (mode === "two")
            doc.fillExtrudePreview(previewB, wireB, spec(), 1)
        else
            doc.fillExtrudePreview(previewB, wireB, {}, 1)
    }
    function replanNow() {
        if (!ready) return
        plan = doc.extrudePlan(spec())
        let choices = null, side = 0
        if (plan.ok)
            for (let s = 0; s < plan.sides.length && !choices; ++s) {
                const sd = plan.sides[s]
                if ((sd.op === "cut" || sd.op === "intersect") && sd.hits.length > 1) {
                    const skip = s === 0 ? skip1 : skip2
                    choices = {}
                    for (const h of sd.hits)
                        choices[h.body] = skip.indexOf(h.body) < 0
                    side = s
                }
            }
        cutChoices = choices
        cutSide = side
        drawCut()
    }
    function toggleCut(id, on) {
        const key = cutSide === 0 ? "skip1" : "skip2"
        const list = (cutSide === 0 ? skip1 : skip2).filter(b => b !== id)
        if (!on)
            list.push(id)
        tool[key] = list
        replanNow()
    }
    Timer { id: replan; interval: 16; onTriggered: tool.replanNow() }    // a frame: the look-up takes milliseconds now
    Timer { running: tool.open; repeat: true; interval: 33; onTriggered: tool.tick++ }
    Connections {
        target: tool.selection
        function onSelectedChanged() {
            if (tool.open && tool.picking >= 0)
                tool.takeTarget()                // puts the areas back, which comes here again
            else
                tool.areasPicked()
        }
    }
    onD1TextChanged: redraw()
    onD2TextChanged: redraw()
    onModeChanged: redraw()
    onOp1Changed: redraw()
    onOp2Changed: redraw()
    onPerAreaChanged: redraw()
    onPerTextChanged: redraw()
    onExtent1Changed: redraw()
    onExtent2Changed: redraw()
    onTarget1Changed: redraw()
    onTarget2Changed: redraw()
    onOff1TextChanged: redraw()
    onOff2TextChanged: redraw()
    onTaper1TextChanged: redraw()
    onTaper2TextChanged: redraw()

    // What the side does now, for the Auto chip.
    function resolved(side) {
        return plan && plan.ok && plan.sides[side] ? plan.sides[side].op : "new"
    }
    function nameOf(op) {
        return op === "cut" ? "Cut" : op === "intersect" ? "Intersect" : op === "join" ? "Join" : op === "component" ? "New Component" : "New Body"
    }

    // --- arrows -------------------------------------------------------------------------------------
    // One per side (one per area and side when "distance per area" is on).
    readonly property var handles: {
        tick
        if (!open || !frame || !frame.ok)
            return []
        const out = []
        const n = frame.normal
        const sides = mode === "two" ? 2 : 1
        const anchors = frame.anchors
        const fixed = [extent1 !== "distance" && (extent1 !== "object" || target1), extent2 !== "distance" && (extent2 !== "object" || target2)]
        const mid = { x: 0, y: 0, z: 0 }
        for (const a of anchors) { mid.x += a.x / anchors.length; mid.y += a.y / anchors.length; mid.z += a.z / anchors.length }
        const lengthOf = (text) => { const r = parseLen(text); return r.ok ? r.mm : 0 }
        for (let s = 0; s < sides; ++s)
            for (let i = 0; i < (perOn && s === 0 ? anchors.length : 1); ++i) {
                const at = perOn && s === 0 ? anchors[i] : Qt.vector3d(mid.x, mid.y, mid.z)
                const d = fixed[s] ? (s === 0 ? frame.reach1 : frame.reach2)
                                   : lengthOf(perOn && s === 0 ? perText[i] : (s === 0 ? d1Text : d2Text))
                const sign = s === 0 ? 1 : -1
                const start = project(at)
                const unit = project(Qt.vector3d(at.x + n.x * 100, at.y + n.y * 100, at.z + n.z * 100))
                const tip = project(Qt.vector3d(at.x + n.x * d * sign, at.y + n.y * d * sign, at.z + n.z * d * sign))
                // A short push must still show an arrow: drawn at least 60 px out (dragging still follows the mouse).
                let dx = tip.x - start.x, dy = tip.y - start.y
                const dl = Math.hypot(dx, dy)
                if (dl < 60) {
                    if (dl < 1e-3) { dx = 0; dy = -1 } else { dx /= dl; dy /= dl }
                    tip.x = start.x + dx * 60
                    tip.y = start.y + dy * 60
                }
                out.push({ side: s, area: perOn && s === 0 ? i : -1, start: start, axis: Qt.point(unit.x - start.x, unit.y - start.y), tip: tip, sign: sign, mm: d })
            }
        return out
    }
    function setDistance(h, mm) {
        if (h.side === 0 && extent1 !== "distance") { extent1 = "distance"; picking = -1 }
        if (h.side === 1 && extent2 !== "distance") { extent2 = "distance"; picking = -1 }
        const text = lenText(mm)
        if (h.area >= 0) {
            const list = perText.slice()
            list[h.area] = text
            perText = list
        } else if (h.side === 0) {
            d1Text = text
        } else {
            d2Text = text
        }
    }

    // By count, not by the list: the list is new on every view move and every distance, and a new list
    // rebuilt the arrows, dropping the one being dragged (owner, 2026-11-03: the arrow wouldn't drag).
    readonly property var noHandle: ({ side: 0, area: -1, start: Qt.point(0, 0), axis: Qt.point(0, 0), tip: Qt.point(0, 0), sign: 1, mm: 0 })
    Repeater {
        model: tool.handles.length
        Item {
            id: arrow
            required property int index
            readonly property var modelData: tool.handles[index] || tool.noHandle
            readonly property real len: Math.hypot(modelData.axis.x, modelData.axis.y)
            readonly property real ang: Math.atan2(modelData.axis.y * modelData.sign, modelData.axis.x * modelData.sign)
            // the stem from where the area starts to the handle
            Rectangle {
                x: arrow.modelData.start.x
                y: arrow.modelData.start.y - 1.5
                width: Math.hypot(arrow.modelData.tip.x - arrow.modelData.start.x, arrow.modelData.tip.y - arrow.modelData.start.y)
                height: 3
                color: tool.theme.accent
                transformOrigin: Item.Left
                rotation: Math.atan2(arrow.modelData.tip.y - arrow.modelData.start.y, arrow.modelData.tip.x - arrow.modelData.start.x) * 180 / Math.PI
            }
            Rectangle {
                id: grip
                objectName: "extrudeArrow " + arrow.modelData.side + " " + arrow.modelData.area
                x: arrow.modelData.tip.x - 15
                y: arrow.modelData.tip.y - 15
                width: 30
                height: 30
                radius: 15
                color: gripArea.pressed || gripArea.containsMouse ? tool.theme.accent : tool.theme.panelRaised
                border.color: tool.theme.accent
                border.width: 2
                Text {
                    anchors.centerIn: parent
                    text: "➤"
                    rotation: Math.atan2(arrow.modelData.tip.y - arrow.modelData.start.y + 0.001, arrow.modelData.tip.x - arrow.modelData.start.x + 0.001) * 180 / Math.PI
                    color: gripArea.pressed || gripArea.containsMouse ? tool.theme.windowBg : tool.theme.accent
                    font.pixelSize: 13
                }
                MouseArea {
                    id: gripArea
                    anchors.fill: parent
                    anchors.margins: -4
                    hoverEnabled: true
                    cursorShape: Qt.SizeAllCursor
                    // Counted from where it was grabbed: a short distance draws the arrow longer than it is,
                    // and measuring from the profile made the first move jump.
                    property real grabAt: 0
                    property real grabMm: 0
                    function along(mouse) {
                        const p = mapToItem(tool, mouse.x, mouse.y)
                        const a = arrow.modelData
                        return ((p.x - a.start.x) * a.axis.x + (p.y - a.start.y) * a.axis.y) / (arrow.len * arrow.len) * 100
                    }
                    onPressed: mouse => {
                        grabMm = arrow.modelData.mm
                        grabAt = arrow.len < 1e-6 ? 0 : along(mouse)
                    }
                    onPositionChanged: mouse => {
                        if (!pressed || arrow.len < 1e-6)
                            return
                        const a = arrow.modelData
                        tool.setDistance(a, grabMm + (along(mouse) - grabAt) * a.sign)
                    }
                }
            }
        }
    }

    // The distance boxes sit ON the profile, where it starts, and never follow the arrow (owner, t110u/t118u).
    // One box per side; with "distance per area" on, side 1 has one box per area, each at its own area.
    component Distance: Rectangle {
        id: dist
        property alias text: input.text
        property alias input: input
        property string tag: ""
        property string fixedText: ""             // All: the length it came to, not typed
        signal edited(string text)
        height: 34
        width: 132
        radius: 8
        color: Qt.rgba(tool.theme.panelBg.r, tool.theme.panelBg.g, tool.theme.panelBg.b, 0.96)
        border.color: dist.fixedText ? tool.theme.border : !tool.parseLen(input.text).ok ? tool.theme.warning : input.activeFocus ? tool.theme.accent : tool.theme.border
        border.width: 1.5
        Text {
            visible: !!dist.fixedText
            anchors.left: parent.left
            anchors.leftMargin: dist.tag.length ? 26 : 10
            anchors.verticalCenter: parent.verticalCenter
            text: dist.fixedText
            color: tool.theme.textSecondary
            font.pixelSize: 14
        }
        Text {
            visible: dist.tag.length > 0
            anchors.left: parent.left
            anchors.leftMargin: 10
            anchors.verticalCenter: parent.verticalCenter
            text: dist.tag
            color: tool.theme.textSecondary
            font.pixelSize: 12
        }
        TextInput {
            id: input
            anchors.fill: parent
            anchors.leftMargin: dist.tag.length ? 26 : 10
            anchors.rightMargin: 28
            verticalAlignment: TextInput.AlignVCenter
            color: tool.theme.textPrimary
            selectionColor: tool.theme.selection
            selectedTextColor: tool.theme.windowBg
            font.pixelSize: 14
            clip: true
            visible: !dist.fixedText
            onTextEdited: dist.edited(text)
            onAccepted: tool.done()
        }
        Text {
            anchors.right: parent.right
            anchors.rightMargin: 9
            anchors.verticalCenter: parent.verticalCenter
            visible: !dist.fixedText
            text: "fx"
            font.pixelSize: 12
            font.italic: true
            color: tool.parseLen(input.text).formula ? tool.theme.accent : tool.theme.textMuted
            opacity: tool.parseLen(input.text).formula ? 1 : 0.5
        }
    }
    readonly property var startPoint: {
        tick
        if (!open || !frame || !frame.ok || !frame.anchors.length)
            return Qt.point(-1000, -1000)
        const m = { x: 0, y: 0, z: 0 }
        for (const q of frame.anchors) { m.x += q.x / frame.anchors.length; m.y += q.y / frame.anchors.length; m.z += q.z / frame.anchors.length }
        return project(Qt.vector3d(m.x, m.y, m.z))
    }
    Column {
        id: sideBoxes
        visible: tool.ready && (!tool.perOn || tool.mode === "two")
        x: tool.startPoint.x + 14
        y: tool.startPoint.y + 14
        spacing: 6
        Distance {
            id: d1Box
            // To Object: the offset past the object (Fusion's Offset); All: how far that came to.
            tag: (tool.mode === "two" ? "1" : "") + (tool.extent1 === "object" ? "±" : "")
            visible: !tool.perOn
            text: tool.extent1 === "object" ? tool.off1Text : tool.d1Text
            fixedText: tool.extent1 === "all" ? "All · " + (tool.frame && tool.frame.ok ? tool.lenText(Math.abs(tool.frame.reach1)) : "–") : ""
            onEdited: t => { if (tool.extent1 === "object") tool.off1Text = t; else tool.d1Text = t }
            Component.onCompleted: tool.d1Field = d1Box.input
        }
        Distance {
            tag: "2" + (tool.extent2 === "object" ? "±" : "")
            visible: tool.mode === "two"
            text: tool.extent2 === "object" ? tool.off2Text : tool.d2Text
            fixedText: tool.extent2 === "all" ? "All · " + (tool.frame && tool.frame.ok ? tool.lenText(Math.abs(tool.frame.reach2)) : "–") : ""
            onEdited: t => { if (tool.extent2 === "object") tool.off2Text = t; else tool.d2Text = t }
        }
    }
    Repeater {
        model: tool.perOn && tool.frame && tool.frame.ok ? tool.areas.length : 0
        Distance {
            id: pbox
            required property int index
            readonly property var at: { tool.tick; return tool.project(tool.frame.anchors[index]) }
            x: at.x + 14
            y: at.y + 14
            text: tool.perText[pbox.index] !== undefined ? tool.perText[pbox.index] : ""
            onEdited: t => {
                const list = tool.perText.slice()
                list[pbox.index] = t
                tool.perText = list
            }
        }
    }

    // --- the options: rows of chips at the top, like Project's, and Done in the same style ----------
    component Chip: Rectangle {
        id: chip
        property string label
        property bool on: false
        property bool clearSlot: false              // room kept for the ✕, so the label never moves
        property bool clearable: false
        signal clicked()
        signal cleared()
        height: 34
        width: Math.max(110, chipText.implicitWidth + 28 + (clearSlot ? 22 : 0))
        radius: 8
        color: Qt.rgba(tool.theme.panelBg.r, tool.theme.panelBg.g, tool.theme.panelBg.b, 0.92)
        border.color: on ? tool.theme.accent : chipArea.containsMouse ? tool.theme.textSecondary : tool.theme.border
        border.width: 1.5
        Text {
            id: chipText
            anchors.centerIn: parent
            anchors.horizontalCenterOffset: chip.clearSlot ? -11 : 0
            text: chip.label
            color: chip.on ? tool.theme.textPrimary : tool.theme.textSecondary
            font.pixelSize: 12
        }
        MouseArea {
            id: chipArea
            anchors.fill: parent
            hoverEnabled: true
            cursorShape: Qt.PointingHandCursor
            onClicked: chip.clicked()
        }
        Text {
            visible: chip.clearable
            anchors.right: parent.right
            anchors.rightMargin: 10
            anchors.verticalCenter: parent.verticalCenter
            text: "✕"
            color: clearArea.containsMouse ? tool.theme.textPrimary : tool.theme.textSecondary
            font.pixelSize: 13
            MouseArea {
                id: clearArea
                anchors.fill: parent
                anchors.margins: -6
                enabled: chip.clearable
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                onClicked: chip.cleared()
            }
        }
    }
    component Operations: Row {
        id: ops
        property int side: 0
        property string current
        property string tag: ""
        signal chosen(string op)
        spacing: 8
        Text {
            visible: ops.tag.length > 0
            height: 34
            verticalAlignment: Text.AlignVCenter
            text: ops.tag
            color: tool.theme.textSecondary
            font.pixelSize: 12
        }
        Chip { label: tool.ready ? "Auto: " + tool.nameOf(tool.resolved(ops.side)) : "Auto"; on: ops.current === "auto"; onClicked: ops.chosen("auto") }
        Chip { label: "Join"; on: ops.current === "join"; onClicked: ops.chosen("join") }
        Chip { label: "Cut"; on: ops.current === "cut"; onClicked: ops.chosen("cut") }
        Chip { label: "Intersect"; on: ops.current === "intersect"; onClicked: ops.chosen("intersect") }
        Chip { label: "New Body"; on: ops.current === "new"; onClicked: ops.chosen("new") }
        Chip { label: "New Component"; on: ops.current === "component"; onClicked: ops.chosen("component") }
    }
    // Extent and taper for one side: Distance / To Object / All, then the taper angle.
    component Extent: Row {
        id: ext
        property int side: 0
        property string tag: ""
        readonly property string current: side === 0 ? tool.extent1 : tool.extent2
        readonly property var target: side === 0 ? tool.target1 : tool.target2
        spacing: 8
        Text {
            visible: ext.tag.length > 0
            height: 34
            verticalAlignment: Text.AlignVCenter
            text: ext.tag
            color: tool.theme.textSecondary
            font.pixelSize: 12
        }
        Chip { objectName: "extentDistance " + ext.side; label: "Distance"; on: ext.current === "distance"; onClicked: tool.setExtent(ext.side, "distance") }
        Chip {
            objectName: "extentObject " + ext.side
            width: 172                                 // the longest label and the ✕, so nothing moves when it changes
            label: tool.picking === ext.side ? "Click the object…" : ext.current === "object" && ext.target ? tool.targetName(ext.target) : "To Object"
            on: ext.current === "object"
            clearSlot: true
            clearable: ext.current === "object" && !!ext.target && tool.picking !== ext.side
            onClicked: tool.setExtent(ext.side, "object")
            onCleared: tool.clearTarget(ext.side)
        }
        Chip {
            objectName: "extentAll " + ext.side
            width: 120
            readonly property bool flipped: { const r = tool.parseLen(ext.side === 0 ? tool.d1Text : tool.d2Text); return r.ok && r.mm < 0 }
            label: ext.current === "all" && flipped ? "All, flipped" : "All"
            on: ext.current === "all"
            onClicked: tool.setExtent(ext.side, "all")
        }
        Rectangle {
            id: taperBox
            readonly property string text: ext.side === 0 ? tool.taper1Text : tool.taper2Text
            height: 34
            width: 132
            radius: 8
            color: Qt.rgba(tool.theme.panelBg.r, tool.theme.panelBg.g, tool.theme.panelBg.b, 0.92)
            border.color: isNaN(tool.taperOf(taperInput.text)) ? tool.theme.warning : taperInput.activeFocus ? tool.theme.accent : tool.theme.border
            border.width: 1.5
            Text {
                id: taperTag
                anchors.left: parent.left
                anchors.leftMargin: 10
                anchors.verticalCenter: parent.verticalCenter
                text: "Taper"
                color: tool.theme.textSecondary
                font.pixelSize: 12
            }
            TextInput {
                id: taperInput
                objectName: "extrudeTaper " + ext.side
                anchors.fill: parent
                anchors.leftMargin: taperTag.implicitWidth + 18
                anchors.rightMargin: 10
                verticalAlignment: TextInput.AlignVCenter
                text: taperBox.text
                color: tool.theme.textPrimary
                selectionColor: tool.theme.selection
                selectedTextColor: tool.theme.windowBg
                font.pixelSize: 14
                clip: true
                onTextEdited: { if (ext.side === 0) tool.taper1Text = text; else tool.taper2Text = text }
                onAccepted: tool.done()
                }
        }
    }
    Column {
        id: options
        objectName: "extrudeOptions"
        visible: tool.open
        anchors.top: parent.top
        anchors.topMargin: 14
        anchors.horizontalCenter: parent.horizontalCenter
        spacing: 8
        Rectangle {
            visible: !tool.ready
            x: (options.width - width) / 2
            width: hintText.implicitWidth + 36
            height: 34
            radius: 8
            color: Qt.rgba(tool.theme.panelBg.r, tool.theme.panelBg.g, tool.theme.panelBg.b, 0.92)
            border.color: tool.theme.accent
            border.width: 1.5
            Text {
                id: hintText
                anchors.centerIn: parent
                text: "Click a profile or a flat face to extrude (Ctrl or Shift adds more); the arrow appears on it"
                color: tool.theme.textPrimary
                font.pixelSize: 12
            }
        }
        Row {
            spacing: 8
            Chip {
                objectName: "extrudeAreas"
                width: 172
                label: tool.areasLabel
                on: tool.picking < 0
                clearSlot: true
                clearable: tool.areas.length > 0
                onClicked: tool.picking = -1         // the profiles row is the one clicks go to again
                onCleared: tool.clearAreas()
            }
            Chip { label: "One Side"; on: tool.mode === "one"; onClicked: tool.mode = "one" }
            Chip { label: "Symmetric"; on: tool.mode === "symmetric"; onClicked: tool.mode = "symmetric" }
            Chip { label: "Two Sides"; on: tool.mode === "two"; onClicked: tool.mode = "two" }
            Chip { label: tool.perArea ? "Distance per area: on" : "Distance per area: off"; on: tool.perArea; onClicked: tool.perArea = !tool.perArea }
            Rectangle {
                objectName: "extrudeDone"
                width: 80
                height: 34
                radius: 8
                color: tool.theme.success
                opacity: tool.ready ? 1 : 0.5
                Text {
                    anchors.centerIn: parent
                    text: "Done"
                    color: tool.theme.windowBg
                    font.pixelSize: 12
                    font.bold: true
                }
                MouseArea {
                    anchors.fill: parent
                    cursorShape: Qt.PointingHandCursor
                    onClicked: tool.done()
                }
            }
        }
        Extent { x: (options.width - width) / 2; side: 0; tag: tool.mode === "two" ? "Side 1" : "" }
        Extent { x: (options.width - width) / 2; visible: tool.mode === "two"; side: 1; tag: "Side 2" }
        Operations { x: (options.width - width) / 2; side: 0; tag: tool.mode === "two" ? "Side 1" : ""; current: tool.op1; onChosen: op => tool.op1 = op }
        Operations { x: (options.width - width) / 2; visible: tool.mode === "two"; side: 1; tag: "Side 2"; current: tool.op2; onChosen: op => tool.op2 = op }
    }
    property var d1Field: null
}
