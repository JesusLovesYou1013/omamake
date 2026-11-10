import QtQuick
import "Units.js" as Units

// MODEL CHAMFER AND MODEL FILLET, one tool (owner, 2026-11-09, GB-051: Fillet "with the same concepts"): `feature`
// says which. Everything below is shared (sets, picking edges / faces / features with tangent chains, Ctrl/Shift
// pick mode, the preview worked out one at a time and kept under Ctrl/Shift, hover and picks by the preview,
// the arrows and boxes placed in front, last good preview + note, Done / Esc); only the size rows differ:
// Chamfer's Equal Distance / Two Distances / Distance and Angle, Flip and Corner Type; Fillet's Constant Radius /
// Chord Length. The engine side is one pipeline too (Document::chamfer* with Fillet's set types, chamfer.cpp).
//
// MODEL CHAMFER (owner, 2026-11-08: "match all the features that Fusion 360 [has], and ... more reliable than
// Fusion's version where it will do its best to apply the chamfer and blend it into the surrounding geometry").
// Fusion's dialog, as rows of chips at the top like Extrude's:
//  - selection sets (+ adds one, ✕ takes one away), each with its own type, distances, angle, Flip and Tangent Chain;
//  - Equal Distance, Two Distances, Distance and Angle;
//  - edges, faces (all their edges) or features (all the edges a Timeline step's faces made: click a face it made);
//  - Corner Type: Chamfer, Miter, Blend.
// A click on an edge or face adds it to the set being picked, a click on one already in it takes it away (no
// Ctrl or Shift needed, as in Fusion). The model is drawn as the chamfer would leave it (worked out in the
// background); the distance box sits on the set's first edge, and its arrow drags the distance.
// Where Fusion stops with an error, this runs the chamfer on through (the engine's chamfer.cpp), and an edge even
// that can't build is left sharp and named in a yellow flag on the step.
Item {
    id: tool
    required property var theme
    required property var selection              // Selection.qml
    required property var previewFaces           // MeshGeometry: the model as the chamfer would leave it
    required property var previewEdges
    property var project: p => Qt.point(0, 0)     // a 3D point on the screen (Main.toScreen)
    property string feature: "chamfer"            // "chamfer" | "fillet"
    readonly property bool rounding: feature === "fillet"
    readonly property string verb: rounding ? "fillet" : "chamfer"
    readonly property string defaultType: rounding ? "radius" : "equal"
    property int fadeMs: 250

    property bool open: false
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
    // Each set: {type, d1Text, d2Text, angleText, flip, chain, parts: [{kind, index}], features: [{step, summary, edges}]}
    property var sets: []
    property int current: 0
    property string corner: "chamfer"             // "chamfer" | "miter" | "blend"
    property string pick: "parts"                 // "parts" (edges and faces) | "features"
    property bool previewOn: false                // the worked-out model is being drawn in place of the real one
    property string previewNote: ""
    property var picked: []                       // per set, its edges as they'll be chamfered (tangent chains included)
    property var frames: []                       // per set, where its bevel sits (Document::chamferFrame)
    property int tick: 0
    readonly property var set: sets[current] || null
    readonly property bool ready: sets.some(s => s.parts.length || s.features.length)
    readonly property real rowsBottom: open ? options.y + options.height : 0

    signal say(string text, int ms)
    function readingMs(text) { return Math.min(60000, 2000 + 70 * text.length) }
    signal finished()

    visible: open || fadeOut.running
    opacity: open ? 1 : 0
    Behavior on opacity { NumberAnimation { id: fadeOut; duration: tool.fadeMs } }

    function parseLen(text) {
        const s = String(text).trim()
        if (!s)
            return { ok: false }
        const v = Units.parse(s)
        if (v)
            return { ok: v.mm > 0, mm: v.mm, formula: "" }
        if (/[A-Za-z_À-￿]/.test(s) || /[-+*\/^()]/.test(s)) {
            const r = doc.evaluateExpression(s, "length")
            if (r.ok)
                return { ok: r.value > 0, mm: r.value, formula: r.names.length ? s : "" }
            return { ok: false, why: r.error }
        }
        return { ok: false }
    }
    function lenText(mm) { return Units.plain(mm, "in") }
    function angleOf(text) {
        const r = Units.parseAngle(text)
        return r ? r.mm * 180 / Math.PI : NaN
    }
    function newSet() {
        const last = sets.length ? sets[sets.length - 1] : null
        // A new set starts with the last one's sizes (Fusion's does too), and nothing picked.
        return { type: last ? last.type : defaultType, d1Text: last ? last.d1Text : "0.100 in", d2Text: last ? last.d2Text : "0.100 in",
                 angleText: last ? last.angleText : "45°", flip: false, chain: true, parts: [], features: [] }
    }
    function edit(key, value) {
        const list = sets.slice()
        const s = Object.assign({}, list[current])
        s[key] = value
        list[current] = s
        sets = list
    }
    function setSpec(s) {
        const d1 = parseLen(s.d1Text), d2 = parseLen(s.d2Text), a = angleOf(s.angleText)
        const edges = [], faces = [], points = []
        for (const p of s.parts) {
            if (p.kind === "edge") {
                const r = doc.edgeRef(p.index)
                if (r.body) edges.push(r)
            } else if (p.kind === "face") {
                const r = doc.faceRef(p.index)
                if (r.body) faces.push(r)
            } else if (p.kind === "point") {             // a corner or a tip (GB-053)
                const r = doc.pointRef(p.index)
                if (r.body) points.push(r)
            }
        }
        return { type: s.type, d1: d1.ok ? d1.mm : 0, d2: d2.ok ? d2.mm : 0, angle: isNaN(a) ? 45 : a,
                 flip: s.flip, chain: s.chain, edges: edges, faces: faces, points: points, features: s.features }
    }
    function spec() {
        return { feature: feature, corner: corner, sets: sets.filter(s => s.parts.length || s.features.length).map(setSpec) }
    }
    function sizesOk(s) {
        return parseLen(s.d1Text).ok && (s.type !== "two" || parseLen(s.d2Text).ok)
            && (s.type !== "angle" || (angleOf(s.angleText) > 0 && angleOf(s.angleText) < 90))
    }

    // A click in the view: an edge or a face goes into (or out of) the set being picked; with Features on,
    // a face picks the Timeline step that made it.
    function viewClicked(part, adding) {
        if (!part || !set)
            return
        if (pick === "features") {
            if (part.kind !== "face" && part.kind !== "edge") {
                say("Click a face (or edge) of the feature to " + verb + " all its edges.", 0)
                return
            }
            const f = doc.featureAt(part.kind, part.index)
            if (!f.ok) {
                say(f.why, 0)
                return
            }
            const i = set.features.findIndex(x => x.step === f.step)
            edit("features", i >= 0 ? set.features.filter((x, k) => k !== i) : set.features.concat([{ step: f.step, summary: f.summary, edges: f.edges }]))
            if (i < 0)
                say(f.summary + ": " + f.edges.length + " edges", 0)
            return
        }
        if (part.kind !== "edge" && part.kind !== "face" && part.kind !== "point") {
            say("Click an edge, a point or a face of a body to " + verb + " (a face " + verb + "s all its edges).", 0)
            return
        }
        const i = set.parts.findIndex(p => p.kind === part.kind && p.index === part.index)
        if (i >= 0) {
            edit("parts", set.parts.filter((p, k) => k !== i))
            return
        }
        // Clicked an edge another set has: it moves to this set (an edge is in one set only).
        const list = sets.map((s, k) => k === current ? s : Object.assign({}, s, { parts: s.parts.filter(p => !(p.kind === part.kind && p.index === part.index)) }))
        list[current] = Object.assign({}, list[current], { parts: list[current].parts.concat([{ kind: part.kind, index: part.index }]) })
        sets = list
    }
    function addSet() {
        sets = sets.concat([newSet()])
        current = sets.length - 1
    }
    function removeSet(k) {
        if (sets.length <= 1) {
            sets = [Object.assign(newSet(), { parts: [], features: [] })]
            current = 0
            return
        }
        sets = sets.filter((s, i) => i !== k)
        current = Math.min(current, sets.length - 1)
    }
    function setLabel(s, k) {
        const edges = picked[k] ? picked[k].length : 0, points = s.parts.filter(p => p.kind === "point").length
        const what = !(s.parts.length || s.features.length) ? "none"
                   : (edges || !points ? edges + " edge" + (edges === 1 ? "" : "s") : "")
                     + (edges && points ? ", " : "") + (points ? points + " point" + (points === 1 ? "" : "s") : "")
        return "Set " + (k + 1) + ": " + what
    }

    // Every Chamfer (Fillet) starts blank, with edges and faces already selected taken as the first set's.
    function begin() {
        const first = newSet()
        first.d1Text = "0.100 in"; first.d2Text = "0.100 in"; first.angleText = "45°"; first.type = defaultType
        first.parts = selection.selected.filter(p => p.kind === "edge" || p.kind === "face" || p.kind === "point").map(p => ({ kind: p.kind, index: p.index }))
        sets = [first]
        current = 0
        corner = "chamfer"
        pick = "parts"
        stopPreview()
        open = true
        refresh()
        if (!ready)
            say("Click the edges, points or faces to " + verb + " (a face " + verb + "s all its edges); click one again to take it away.", 0)
        Qt.callLater(() => { d1Field.forceActiveFocus(); d1Field.selectAll() })
    }
    function cancel() {
        if (!open)
            return
        open = false
        stopPreview()
        selection.clear()
        finished()
    }
    function done() {
        if (!ready) { cancel(); return }
        const bad = sets.findIndex(s => (s.parts.length || s.features.length) && !sizesOk(s))
        if (bad >= 0) {
            current = bad
            say("Set " + (bad + 1) + "'s size isn't a length more than 0 (or its angle isn't between 0 and 90°).", 0)
            return
        }
        // The last preview couldn't be built: Done would only fail the same way, so it says so and stays open.
        if (stale && !computing && !redo) {
            say((previewNote || "This size can't be built.") + " Change the size or the picks, then Done.", readingMs(previewNote))
            return
        }
        const r = doc.chamferInBackground(spec())
        if (!r.ok) {
            say(r.why, 0)
            return
        }
        submitted = true
        open = false            // the preview stays until the result is in
        finished()
    }
    property bool stale: false                    // the newest preview failed: the one drawn is an older one (GB-054)
    property bool submitted: false                // this tool's Done is being built (the other tool hears it too)
    Connections {
        target: doc
        function onChamferFinished(r) {
            if (!tool.submitted)
                return
            tool.submitted = false
            tool.stopPreview()
            tool.selection.clear()
            if (!r.ok)
                tool.say(r.why, tool.readingMs(r.why))
            else {
                const text = (r.note ? r.note + " " : "") + (tool.rounding ? "Filleted in " : "Chamfered in ") + r.seconds.toFixed(1) + " s"
                tool.say(text, r.note ? tool.readingMs(text) : 0)
            }
        }
        function onChamferPreviewReady(ok, note) {
            tool.computing = false
            if (!tool.open)
                return
            // One that couldn't be built leaves the last up, its note says why, and that what's drawn isn't what
            // Done would make (owner, pt156u, GB-054: "the shape updates ... Clicking done ... does nothing").
            tool.stale = !ok && tool.previewOn
            tool.previewOn = ok || tool.previewOn
            if (ok)
                tool.shownKey = tool.pendingKey
            tool.previewNote = ok || !tool.previewOn ? note
                : (note || (tool.rounding ? "This size can't be filleted" : "This size can't be chamfered")) + " (the preview shows the last size that could be)"
            tool.selection.redraw(true)          // the picks, drawn on the preview now (Document::fillParts)
            if (tool.redo)
                tool.rework()
        }
    }
    // Ctrl or Shift held (owner, 2026-11-09, as Fusion): the preview steps aside and what's picked is lit on
    // the model as it is, to see what's in the sets and click more in or out; let go and the preview comes
    // back, worked out for the new picks. With neither held nothing picked is lit: it hid the bevel.
    readonly property bool showPicks: open && modifierKeys.ctrlOrShift
    onShowPicksChanged: refresh()
    // The preview is what's drawn (it steps aside, kept, while Ctrl/Shift shows the picks): Main draws it in the
    // model's place, and hover and picking go by it (Document::chamferPreviewShown, GB-050).
    readonly property bool previewShown: previewOn && !showPicks

    // Where each set's boxes and arrows sit along its first edge (owner, 2026-11-09, GB-050: "often putting the
    // arrow away from the viewer and actually behind the model"): chosen once, when the set's picks change, among
    // spots along the edge: one in front (its bevel faces the camera, nothing of the model before it, on screen),
    // nearest 5 or 8 o'clock round the edge as seen. Kept while sizes are typed or dragged, so it doesn't jump.
    property var placed: []                       // per set: { key, at }
    function placeKey(s) { return JSON.stringify([s.parts, s.features]) }
    function placeOn(spots) {
        const ok = spots.filter(c => c && c.ok)
        if (!ok.length)
            return null
        let cx = 0, cy = 0
        for (const c of ok) { const s = project(v3(c.edge)); cx += s.x / ok.length; cy += s.y / ok.length }
        let best = null, bestScore = Infinity
        for (const c of ok) {
            const p = v3(c.edge), mid = v3(c.end1).plus(v3(c.end2)).times(0.5)
            const out = p.minus(mid), size = out.length()
            if (size < 1e-9)
                continue
            const n = out.times(1 / size)
            const s = project(p)
            const ray = selection.rayAt(s.x, s.y), d = ray.direction.normalized()
            const facing = -n.dotProduct(d)
            const q = p.plus(n.times(0.02 * size + 0.01)), sq = project(q), rq = selection.rayAt(sq.x, sq.y)
            const hit = doc.pick(rq.origin, rq.direction, 0, 0, false)
            const far = q.minus(rq.origin).dotProduct(rq.direction.normalized())
            const seen = hit.depth < 0 || hit.depth >= far - 0.02 * size - 0.05
            const onScreen = s.x > 40 && s.y > 40 && s.x < width - 40 && s.y < height - 40
            const clock = (Math.atan2(s.x - cx, -(s.y - cy)) * 180 / Math.PI + 360) % 360
            const off = a => Math.min(Math.abs(clock - a), 360 - Math.abs(clock - a))
            const round = Math.hypot(s.x - cx, s.y - cy) > 4 ? Math.min(off(150), off(240)) : 0
            // Facing the eye more is better too (a picked face's near rim over its far one).
            const score = round + 60 * (1 - facing) + (facing > 0.05 ? 0 : 1000) + (seen ? 0 : 500) + (onScreen ? 0 : 2000) + 20 * Math.abs(c.at - 0.5)
            if (score < bestScore) { bestScore = score; best = c }
        }
        return best || ok[0]
    }
    function frameSpecs() {
        const was = placed
        return sets.map((s, k) => {
            const p = was[k] && was[k].key === placeKey(s) ? { at: was[k].at, atEdge: was[k].atEdge } : { candidates: 16 }
            return Object.assign(setSpec(s), p)
        })
    }
    function placeFrames(got) {
        const list = [], keep = []
        for (let k = 0; k < sets.length; ++k) {
            let f = got[k] || { ok: false }
            if (f.ok && f.candidates) {
                const c = placeOn(f.candidates)
                if (c) f = c
            }
            list.push(f)
            keep.push(f.ok ? { key: placeKey(sets[k]), at: f.at, atEdge: f.atEdge } : null)
        }
        placed = keep
        return list
    }

    // What's picked changed (or Ctrl/Shift): the highlight, the boxes, the preview.
    function refresh() {
        if (!open)
            return
        const all = doc.chamferPicked({ corner: corner, sets: sets.map(setSpec) })
        picked = all
        frames = placeFrames(doc.chamferFrame({ corner: corner, sets: frameSpecs() }))
        // Lit only while showing the picks: each set's faces, and its edges with their tangent chains.
        const shown = []
        for (let k = 0; showPicks && k < sets.length; ++k) {
            for (const p of sets[k].parts)
                if (p.kind === "face" || p.kind === "point")
                    shown.push({ kind: p.kind, index: p.index })
            for (const e of all[k] || [])
                if (!shown.some(q => q.kind === "edge" && q.index === e))
                    shown.push({ kind: "edge", index: e })
        }
        selection.selected = shown
        selection.redraw()
        if (!ready) {
            stopPreview()
            return
        }
        // Ctrl/Shift held: the preview only steps aside (previewShown), kept for the moment they're let go.
        if (showPicks) {
            redo = false
            return
        }
        // A size part typed ("0.", "1/"): the last preview stays up rather than the model flashing back.
        if (!sets.every(x => !(x.parts.length || x.features.length) || sizesOk(x))) {
            redo = false
            return
        }
        rework()
    }
    // The preview follows a drag or each key typed: one is worked out at a time (on its own thread), and
    // what changed meanwhile starts the next the moment it's in, so it keeps up as fast as the engine can
    // (owner, 2026-11-09: "follow the mouse as I'm dragging it"; a timer restarted on every move only
    // showed it once the mouse stopped).
    property bool computing: false
    property bool redo: false
    property string pendingKey: ""                // the spec being worked out, and the one the preview shows
    property string shownKey: ""
    function rework() {
        if (computing) {
            redo = true
            return
        }
        const s = spec(), key = JSON.stringify(s)
        // Ctrl/Shift let go with nothing changed: the kept preview is simply shown again, at once.
        if (previewOn && key === shownKey) {
            redo = false
            return
        }
        computing = true
        redo = false
        pendingKey = key
        doc.chamferPreview(s, previewFaces, previewEdges)
    }
    function stopPreview() {
        computing = false
        redo = false
        shownKey = ""
        previewOn = false
        stale = false
        previewNote = ""
        doc.dropChamferPreview()
        selection.redraw(true)
    }
    Timer { running: tool.open; repeat: true; interval: 33; onTriggered: tool.tick++ }
    onSetsChanged: Qt.callLater(refresh)
    onCornerChanged: refresh()

    // --- the distance box and arrows on the set's first edge ----------------------------------------------
    readonly property var frame: frames[current] && frames[current].ok ? frames[current] : null
    function v3(a) { return Qt.vector3d(a[0], a[1], a[2]) }
    // The arrows point the way the bevel grows (owner, 2026-11-09: "I should be dragging closer to it"), so into
    // the body on an outside edge. Equal Distance and Distance and Angle: one, on the bevel's middle, across the
    // bevel away from the edge it replaces; Two Distances: one on each of its ends, each along its own face away
    // from the edge, the way that end moves as its distance grows (so the two point apart, not on top of each
    // other). Each carries how many pixels along it one mm of its distance moves its spot.
    readonly property var handles: {
        tick
        if (!open || !frame || !set)
            return []
        const p = v3(frame.edge), a = v3(frame.end1), b = v3(frame.end2)
        const mid = a.plus(b).times(0.5)
        const away = mid.minus(p)
        // A point (GB-053): its one spot is on the cut's axis, so the arrow goes along it, into the body.
        const point = !!frame.point || a.minus(b).length() < 1e-9
        const w = point ? Qt.vector3d(0, 0, 0) : a.minus(b).normalized()
        const grow = point ? away.normalized() : away.minus(w.times(away.dotProduct(w))).normalized()
        // A fillet's grip goes by its radius (or chord), the spot on the round moving in step with it.
        const d1 = rounding ? parseLen(set.d1Text).mm || 0 : a.minus(p).length(), d2 = b.minus(p).length()
        const grips = set.type === "two" && !point ? [{ which: "d1Text", at: a, mm: d1, way: a.minus(p).normalized() },
                                            { which: "d2Text", at: b, mm: d2, way: b.minus(p).normalized() }]
                                         : [{ which: "d1Text", at: mid, mm: d1, way: grow }]
        const out = []
        for (const g of grips) {
            if (g.mm < 1e-9 || g.way.length() < 0.5)
                continue
            const start = project(g.at)
            const ahead = project(g.at.plus(g.way.times(g.mm)))
            let dx = ahead.x - start.x, dy = ahead.y - start.y
            const dl = Math.hypot(dx, dy)
            if (dl > 1e-3) { dx /= dl; dy /= dl } else { dx = 0; dy = -1 }
            // The spot moves away from the edge as the distance grows: g.at - p per mm of it.
            const moved = project(g.at.plus(g.at.minus(p).times(1 / g.mm)))
            const pxPerMm = (moved.x - start.x) * dx + (moved.y - start.y) * dy
            out.push({ which: g.which, start: start, dir: Qt.point(dx, dy), pxPerMm: pxPerMm,
                       tip: Qt.point(start.x + dx * 50, start.y + dy * 50), mm: g.mm })
        }
        return out
    }
    readonly property var noHandle: ({ which: "d1Text", start: Qt.point(0, 0), dir: Qt.point(1, 0), pxPerMm: 1, tip: Qt.point(0, 0), mm: 0 })
    Repeater {
        model: tool.handles.length
        Item {
            id: arrow
            required property int index
            readonly property var modelData: tool.handles[index] || tool.noHandle
            Rectangle {
                x: arrow.modelData.start.x
                y: arrow.modelData.start.y - 1.5
                width: Math.hypot(arrow.modelData.tip.x - arrow.modelData.start.x, arrow.modelData.tip.y - arrow.modelData.start.y)
                height: 3
                color: tool.theme.accent
                transformOrigin: Item.Left
                rotation: Math.atan2(arrow.modelData.dir.y, arrow.modelData.dir.x) * 180 / Math.PI
            }
            Rectangle {
                objectName: tool.feature + "Arrow " + arrow.index
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
                    rotation: Math.atan2(arrow.modelData.dir.y, arrow.modelData.dir.x) * 180 / Math.PI
                    color: gripArea.pressed || gripArea.containsMouse ? tool.theme.windowBg : tool.theme.accent
                    font.pixelSize: 13
                }
                MouseArea {
                    id: gripArea
                    anchors.fill: parent
                    anchors.margins: -4
                    hoverEnabled: true
                    cursorShape: Qt.SizeAllCursor
                    // The arrow rides on the bevel as it grows, so the drag is measured along it as it was pressed.
                    property real grabMm: 0
                    property real scale: 1
                    property point from
                    property point dir
                    property string which
                    function along(mouse) {
                        const p = mapToItem(tool, mouse.x, mouse.y)
                        return (p.x - from.x) * dir.x + (p.y - from.y) * dir.y
                    }
                    onPressed: mouse => {
                        const a = arrow.modelData
                        grabMm = a.mm
                        from = mapToItem(tool, mouse.x, mouse.y)
                        dir = a.dir
                        which = a.which
                        scale = a.pxPerMm > 1e-6 ? a.pxPerMm : 1
                    }
                    onPositionChanged: mouse => {
                        if (!pressed)
                            return
                        const mm = Math.max(0.001, grabMm + along(mouse) / scale)
                        tool.edit(which, tool.lenText(mm))
                    }
                }
            }
        }
    }
    component Box: Rectangle {
        id: box
        property alias text: input.text
        property alias input: input
        property string tag: ""
        property bool angle: false
        signal edited(string text)
        readonly property bool good: angle ? (tool.angleOf(input.text) > 0 && tool.angleOf(input.text) < 90) : tool.parseLen(input.text).ok
        height: 34
        width: 132
        radius: 8
        color: Qt.rgba(tool.theme.panelBg.r, tool.theme.panelBg.g, tool.theme.panelBg.b, 0.96)
        border.color: !box.good ? tool.theme.warning : input.activeFocus ? tool.theme.accent : tool.theme.border
        border.width: 1.5
        Text {
            visible: box.tag.length > 0
            anchors.left: parent.left
            anchors.leftMargin: 10
            anchors.verticalCenter: parent.verticalCenter
            text: box.tag
            color: tool.theme.textSecondary
            font.pixelSize: 12
        }
        TextInput {
            id: input
            anchors.fill: parent
            anchors.leftMargin: box.tag.length ? 26 : 10
            anchors.rightMargin: 28
            verticalAlignment: TextInput.AlignVCenter
            color: tool.theme.textPrimary
            selectionColor: tool.theme.selection
            selectedTextColor: tool.theme.windowBg
            font.pixelSize: 14
            clip: true
            onTextEdited: box.edited(text)
            onAccepted: tool.done()
        }
        Text {
            visible: !box.angle
            anchors.right: parent.right
            anchors.rightMargin: 9
            anchors.verticalCenter: parent.verticalCenter
            text: "fx"
            font.pixelSize: 12
            font.italic: true
            color: tool.parseLen(input.text).formula ? tool.theme.accent : tool.theme.textMuted
            opacity: tool.parseLen(input.text).formula ? 1 : 0.5
        }
    }
    readonly property var boxAt: {
        tick
        return frame ? project(v3(frame.edge)) : Qt.point(-1000, -1000)
    }
    // Which way the boxes sit off the edge: away from the arrows (which now point into the body, often down
    // and across, where the boxes used to sit and covered the grips); with no arrow, down and right as before.
    readonly property point boxAway: {
        let dx = 0, dy = 0
        for (const h of handles) { dx += h.dir.x; dy += h.dir.y }
        const l = Math.hypot(dx, dy)
        return l > 1e-3 ? Qt.point(-dx / l, -dy / l) : Qt.point(Math.SQRT1_2, Math.SQRT1_2)
    }
    // The boxes: on the edge when there is one, else (nothing picked yet) under the rows, so the size can be typed first.
    Column {
        id: sizeBoxes
        visible: tool.open && !!tool.set
        x: tool.frame ? tool.boxAt.x + tool.boxAway.x * 20 - (tool.boxAway.x < 0 ? width : 0) : (tool.width - width) / 2
        y: tool.frame ? tool.boxAt.y + tool.boxAway.y * 20 - (tool.boxAway.y < 0 ? height : 0) : tool.rowsBottom + 12
        spacing: 6
        Box {
            id: d1Box
            objectName: tool.feature + "D1"
            tag: tool.set && tool.set.type === "two" ? "1" : tool.set && tool.set.type === "chord" ? "c" : tool.set && tool.set.type === "radius" ? "r" : ""
            text: tool.set ? tool.set.d1Text : ""
            onEdited: t => tool.edit("d1Text", t)
            Component.onCompleted: tool.d1Field = d1Box.input
        }
        Box {
            objectName: tool.feature + "D2"
            visible: !!tool.set && tool.set.type === "two"
            tag: "2"
            text: tool.set ? tool.set.d2Text : ""
            onEdited: t => tool.edit("d2Text", t)
        }
        Box {
            objectName: tool.feature + "Angle"
            visible: !!tool.set && tool.set.type === "angle"
            angle: true
            tag: "∠"
            text: tool.set ? tool.set.angleText : ""
            onEdited: t => tool.edit("angleText", t)
        }
    }
    property var d1Field: null

    // --- the options --------------------------------------------------------------------------------------
    component Chip: Rectangle {
        id: chip
        property string label
        property bool on: false
        property bool dim: false
        property bool clearSlot: false
        property bool clearable: false
        signal clicked()
        signal cleared()
        height: 34
        width: Math.max(110, chipText.implicitWidth + 28 + (clearSlot ? 22 : 0))
        radius: 8
        color: Qt.rgba(tool.theme.panelBg.r, tool.theme.panelBg.g, tool.theme.panelBg.b, 0.92)
        border.color: on ? tool.theme.accent : chipArea.containsMouse && !dim ? tool.theme.textSecondary : tool.theme.border
        border.width: 1.5
        opacity: dim ? 0.45 : 1
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
            enabled: !chip.dim
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
    Column {
        id: options
        objectName: tool.feature + "Options"
        visible: tool.open
        anchors.top: parent.top
        anchors.topMargin: 14
        anchors.horizontalCenter: parent.horizontalCenter
        spacing: 8
        // The sets, + for another, ✕ on each.
        Row {
            x: (options.width - width) / 2
            spacing: 8
            Repeater {
                model: tool.sets.length
                Chip {
                    required property int index
                    objectName: tool.feature + "Set " + index
                    width: 150
                    label: tool.sets[index] ? tool.setLabel(tool.sets[index], index) : ""
                    on: tool.current === index
                    clearSlot: true
                    clearable: tool.sets.length > 1 || !!(tool.sets[index] && (tool.sets[index].parts.length || tool.sets[index].features.length))
                    onClicked: tool.current = index
                    onCleared: tool.removeSet(index)
                }
            }
            Chip { objectName: tool.feature + "AddSet"; label: "+ Set"; width: 80; onClicked: tool.addSet() }
        }
        Row {
            x: (options.width - width) / 2
            spacing: 8
            Chip { visible: tool.rounding; objectName: tool.feature + "Radius"; label: "Constant Radius"; on: !!tool.set && tool.set.type === "radius"; onClicked: tool.edit("type", "radius") }
            Chip { visible: tool.rounding; objectName: tool.feature + "Chord"; label: "Chord Length"; on: !!tool.set && tool.set.type === "chord"; onClicked: tool.edit("type", "chord") }
            Chip { visible: !tool.rounding; objectName: tool.feature + "Equal"; label: "Equal Distance"; on: !!tool.set && tool.set.type === "equal"; onClicked: tool.edit("type", "equal") }
            Chip { visible: !tool.rounding; objectName: tool.feature + "Two"; label: "Two Distances"; on: !!tool.set && tool.set.type === "two"; onClicked: tool.edit("type", "two") }
            Chip { visible: !tool.rounding; objectName: tool.feature + "AngleType"; label: "Distance and Angle"; on: !!tool.set && tool.set.type === "angle"; onClicked: tool.edit("type", "angle") }
            Chip {
                objectName: tool.feature + "Flip"
                visible: !tool.rounding
                label: "Flip"
                width: 80
                on: !!tool.set && tool.set.flip
                dim: !tool.set || tool.set.type === "equal"      // the same both ways
                onClicked: tool.edit("flip", !tool.set.flip)
            }
            Chip {
                objectName: tool.feature + "Chain"
                label: !!tool.set && tool.set.chain ? "Tangent Chain: on" : "Tangent Chain: off"
                width: 150
                on: !!tool.set && tool.set.chain
                onClicked: tool.edit("chain", !tool.set.chain)
            }
        }
        Row {
            x: (options.width - width) / 2
            spacing: 8
            Chip { objectName: tool.feature + "PickParts"; label: "Edges and Faces"; on: tool.pick === "parts"; onClicked: tool.pick = "parts" }
            Chip {
                objectName: tool.feature + "PickFeatures"
                width: 150
                label: tool.set && tool.set.features.length ? "Features: " + tool.set.features.length : "Features"
                on: tool.pick === "features"
                onClicked: tool.pick = "features"
            }
            Text { visible: !tool.rounding; height: 34; verticalAlignment: Text.AlignVCenter; text: "Corners"; color: tool.theme.textSecondary; font.pixelSize: 12 }
            Chip { visible: !tool.rounding; objectName: tool.feature + "CornerChamfer"; label: "Chamfer"; width: 90; on: tool.corner === "chamfer"; onClicked: tool.corner = "chamfer" }
            Chip { visible: !tool.rounding; objectName: tool.feature + "CornerMiter"; label: "Miter"; width: 90; on: tool.corner === "miter"; onClicked: tool.corner = "miter" }
            Chip { visible: !tool.rounding; objectName: tool.feature + "CornerBlend"; label: "Blend"; width: 90; on: tool.corner === "blend"; onClicked: tool.corner = "blend" }
            Rectangle {
                objectName: tool.feature + "Done"
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
        // What the preview found: edges that run on through, edges that would stay sharp. Always there, so
        // nothing moves when it fills.
        Item {
            width: options.width
            height: 20
            Text {
                anchors.centerIn: parent
                text: tool.previewNote
                color: tool.theme.warning
                font.pixelSize: 12
            }
        }
    }
}
