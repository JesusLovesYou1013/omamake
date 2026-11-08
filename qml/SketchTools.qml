import QtQuick
import "ShapeTools.js" as Shapes
import "SketchMath.js" as M

// Drawing in the open sketch (Fusion's names; search Alt+Space, Sketch mode):
//   Line                click the start, then click each next point: every click ends one line
//                       and starts the next from there. Clicking the chain's first point
//                       closes it. Esc stops.
//   2-Point Rectangle   click one corner, then the opposite one
//   Sketch Dimension    click a line (its length), or a point and then another point or a
//                       line (the distance); a box opens for the value (Main). While a line's
//                       length box is open, clicking a parallel line dimensions the gap
//                       between the two instead. Stays on for the next one until Esc.
//   Parallel, Perpendicular, Equal, Collinear
//                       click two lines (owner, 2026-10-29: pick after choosing the tool, as in
//                       Fusion; what's already selected counts as the first picks)
//   Horizontal/Vertical a line (level or upright, whichever it's nearer), or two points
//   Coincident          a point, then a point or a line (or a line, then a point)
//   Midpoint            a point and a line, either order
//   Symmetry            two points or two lines, then the line to mirror across
//   Fix/UnFix           click points or lines: each click fixes or lets go
// The first thing picked stays put where it can; the rest moves to meet it. Each stays on
// for the next one until Esc. ruleFor says what a list of picks makes.
// The model's origin point and the axes lying in the sketch can be picked too (owner,
// 2026-10-30: that's what locks a sketch in place): asSketchPart turns them into the sketch's
// own Origin (point -1) and axes (lines -1 along the sketch's X, -2 along its Y), which never move.
// Snapping while drawing (owner, 2026-10-30), within a few pixels, in this order: a point
// already drawn (lines join there: they share it), the Origin (held by Coincident), the middle
// of a line (held by Midpoint, without picking the Midpoint tool), anywhere along a line or an
// axis (held on it by Coincident, free to slide along it). Both ends of a line, and both
// clicked corners of a rectangle, snap alike. A Line within 2° of level or upright snaps to
// it and is held Horizontal / Vertical; it never snaps away from something it snapped onto.
// A snapped point or the Origin lights up; a middle shows a small triangle and a spot on a line
// a small ring (glyph, drawn by Main), and the snap is named beside the mouse (snapLabel).
// Hovering a point already held at a line's middle, or on a line, shows the same mark. While a tool waits
// for its next click, a preview follows the mouse. Each line or rectangle, with the
// constraints its snaps made, is one step in the sketch's own history.
//
// With no tool on, what's drawn can be dragged (owner, 2026-10-29): a point goes with the
// mouse (its lines turn and stretch), a line moves whole, and dragging anything selected
// moves everything selected. The rest follows only as much as the constraints make it,
// moving as little as it can; Fix-ed points stay put. Each drag is one step.
// Double-click a line: the whole shape it's joined to is selected.
//
// Every other Fusion sketch tool (owner, 2026-10-30: every variation its own tool) is a table
// entry in ShapeTools.js, run here: tool "shape", `shape` its key. Its steps are clicked in turn
// (clicks); a step that gathers several things (points of a spline, things to Mirror or Offset)
// collects them in `gather` until Enter. Its options (Fillet's radius, a polygon's sides…) sit in
// the box under the sketch bar (ToolOptions, Main); Enter there carries on too. When the last
// step is in, the table's build makes one spec for Document::addSketchShape: one step in the
// history, the constraints it makes included. The tool stays on for the next one until Esc.
// Offset's distance is also an arrow on the shape: drag it out or in (handleGrab).
QtObject {
    id: tools

    required property var sketch
    required property var selection
    // Ctrl held while drawing (Fusion): no snapping, so nothing gets attached or held level;
    // the point goes exactly where the mouse is.
    property bool free: false

    // "line" | "rectangle" | "circle" | "arc" | "dimension" | "fix" | a constraint in ruleFor | "shape" | ""
    property string tool: ""
    property string shape: ""             // tool "shape": its key in ShapeTools.tools
    property var clicks: []               // its finished steps
    property var gather: []               // the step gathering several things, so far
    property var hoverPart: null          // a pick step: what the mouse is over
    property var cursorEnd: null          // an end step: the end under the mouse
    property var optionValues: ({})       // key → {option: value}, kept while OmaMake runs
    property real mmPerPixel: 0.1         // on the sketch, under the mouse (the Offset arrow's size)
    readonly property var shapeDef: tool === "shape" ? Shapes.tools[shape] : null
    readonly property var step: shapeDef ? shapeDef.steps[Math.min(clicks.length, shapeDef.steps.length - 1)] : null
    readonly property bool pointStep: !!step && (step.kind === "point" || step.kind === "points")
    // Curves (owner, 2026-10-30: all of Fusion's drawing tools, curves first since most shapes
    // are built from them). Center Diameter Circle: click the centre, then anywhere on the rim.
    // Center Point Arc: the centre, where it starts (that sets the radius), then where it ends;
    // it goes round the way the mouse went (sweep, in radians, + counter-clockwise).
    property var arcStart: null          // Center Point Arc: its first end, once clicked
    property real sweep: 0
    property real sweepAngle: 0          // the mouse's last angle round the centre
    property var dimFirst: null           // Sketch Dimension: the point picked first, waiting for the second
    property int dimLine: 0               // Sketch Dimension: the line whose length box is open
    property var dimSpot: null            // Sketch Dimension: where on the sketch the first line was clicked
    property bool geometryShown: true     // false: the open sketch is hidden by its eye (no snapping onto it)
    property var angleSpots: null         // …and an angle's two clicks, first line's then second's (Main.askDimension)
    property var picks: []                // a constraint tool's picks so far
    property int applied: 0               // counts constraints the tool has handed over
    property var start: null             // where the next line or rectangle starts: {u, v, point?}
    property int chainFirst: 0           // the Line chain's first point (clicking it closes the chain)
    // the mouse on the sketch, snapped: {u, v, point? | on: "origin" / "midpoint", line?;
    // hold?: "horizontal" / "vertical"; label?: what it snapped to; shown?: the part to light}
    property var cursor: null
    property point mouse                 // where the mouse is in the window (for the snap's label)

    readonly property real snapPixels: 12   // a fifth more than at first (owner, 2026-10-30: points were hard to land on)
    readonly property real levelDegrees: 2  // a Line this near level or upright snaps to it
    readonly property string snapLabel: dragSnap ? dragSnap.label
                                      : tool && !picking && (tool !== "shape" || pointStep) && cursor && cursor.label ? cursor.label : ""
    // The mark for what's under the mouse: {u, v, kind: "midpoint" | "online"}, or null.
    readonly property var glyph: {
        if (!sketch.active)
            return null
        if (dragSnap && (dragSnap.on === "midpoint" || dragSnap.on === "line" || dragSnap.on === "arc"))
            return { u: dragSnap.u, v: dragSnap.v, kind: dragSnap.on === "midpoint" ? "midpoint" : "online" }
        if (tool && !picking && (tool !== "shape" || pointStep) && cursor) {
            if (cursor.on === "midpoint" || cursor.on === "line" || cursor.on === "arc")
                return { u: cursor.u, v: cursor.v, kind: cursor.on === "midpoint" ? "midpoint" : "online" }
            return cursor.point ? heldMark(cursor.point) : null
        }
        const h = selection.hovered
        return h && h.kind === "sketch_point" ? heldMark(h.index) : null
    }

    // A point held at a line's middle, or on a line: its mark; otherwise null.
    function heldMark(pointId) {
        const g = doc.sketchGeometry(sketch.sketchId)
        const c = g.constraints.find(c => (c.type === "midpoint" || c.type === "pointonline") && c.first === pointId)
        const p = g.points.find(q => q.id === pointId)
        return c && p ? { u: p.u, v: p.v, kind: c.type === "midpoint" ? "midpoint" : "online" } : null
    }

    // The constraints on what's selected, as badges (owner, 2026-10-30: so it's clear what holds
    // a thing when it won't move the way you expect). A selected point: its own constraints; a
    // selected line: its own and its ends'. Each constraint shows on every part it ties, the
    // partners included (a Midpoint and an on-a-line on the point only; Symmetry on the pair).
    // Dimensions aren't here (they show their values). Grouped by where they sit:
    // [{at: "point" | "line", id, u, v (a point) | a, b (a line's ends), badges: [{kind, label}]}].
    readonly property var constraintNames: ({
        parallel: "Parallel", perpendicular: "Perpendicular", equal: "Equal", collinear: "Collinear",
        horizontal: "Horizontal", vertical: "Vertical", horizontalpoints: "Horizontal", verticalpoints: "Vertical",
        fix: "Fix", coincident: "Coincident", pointonline: "Coincident (on a line)", midpoint: "Midpoint",
        symmetry: "Symmetry", symmetrylines: "Symmetry", tangent: "Tangent", tangentarcs: "Tangent",
        concentric: "Concentric", equalradius: "Equal", pointonarc: "Coincident (on a curve)",
        midpoints: "Midpoint", rotation: "Circular Pattern", translation: "Rectangular Pattern",
        curvature: "Curvature"
    })
    readonly property var constraintKinds: ({
        parallel: "LL", perpendicular: "LL", equal: "LL", collinear: "LL", horizontal: "L", vertical: "L",
        horizontalpoints: "PP", verticalpoints: "PP", fix: "P", coincident: "PP", pointonline: "PL",
        midpoint: "PL", symmetry: "PPL", symmetrylines: "LLL", tangent: "LA", tangentarcs: "AA",
        concentric: "AA", equalradius: "AA", pointonarc: "PA", midpoints: "PPP", rotation: "PPP", translation: "PP"
    })
    property var box: null                  // the box being dragged, in window pixels (selectBox)
    property var screenOf: null             // a point in space to window pixels
    property int drawnTick: 0               // bumped when the document changes, so badges follow
    readonly property Connections drawnWatch: Connections {
        target: doc
        function onSketchesChanged() {
            tools.drawnTick++
            if (tools.pickedConstraints.length) {
                const there = doc.sketchGeometry(sketch.sketchId).constraints.map(c => c.id)
                tools.pickedConstraints = tools.pickedConstraints.filter(id => there.includes(id))
            }
        }
    }
    // Constraints picked by clicking their badges, to Delete (owner, 2026-10-30: take a constraint
    // off without deleting lines). A click picks or lets go, no keys needed; picks stay while
    // other lines are selected (their badges stay up too), so badges from all over the sketch can
    // be gathered; clicking empty space (nothing selected) or Esc lets them all go.
    property var pickedConstraints: []
    readonly property Connections pickWatch: Connections {
        target: selection
        function onSelectedChanged() {
            if (!selection.selected.length)
                tools.pickedConstraints = []
        }
    }
    function pickConstraint(id, label) {
        const had = pickedConstraints.includes(id)
        pickedConstraints = had ? pickedConstraints.filter(x => x !== id) : pickedConstraints.concat([id])
        const n = pickedConstraints.length
        sketch.say(n === 0 ? "No constraints picked"
                   : had ? n + (n === 1 ? " constraint" : " constraints") + " picked: Delete takes them off"
                   : (n === 1 ? label.split(":")[0] : n + " constraints") + " picked: Delete takes "
                     + (n === 1 ? "it" : "them") + " off, the lines stay")
    }
    function deleteConstraints() {
        const g = doc.sketchGeometry(sketch.sketchId)
        const gone = g.constraints.filter(c => pickedConstraints.includes(c.id))
        if (!gone.length)
            return
        const what = gone.length === 1 ? (constraintNames[gone[0].type] || "Dimension") : gone.length + " constraints"
        doc.deleteSketchConstraints(sketch.sketchId, gone.map(c => c.id), "Deleted " + what)
        pickedConstraints = []
        sketch.say("Deleted " + what + " (Ctrl+Z brings " + (gone.length === 1 ? "it" : "them") + " back)")
    }
    readonly property var badges: {
        drawnTick
        const chosen = selection.selected.filter(p => p.kind === "sketch_point" || p.kind === "sketch_line" || p.kind === "sketch_arc" || p.kind === "sketch_curve")
        const picked = pickedConstraints
        if (!sketch.active || (!chosen.length && !picked.length))
            return []
        const g = doc.sketchGeometry(sketch.sketchId)
        const pointAt = id => g.points.find(q => q.id === id)
        const lineOf = id => g.lines.find(q => q.id === id)
        const arcOf = id => g.arcs.find(q => q.id === id)
        const points = new Set(), lines = new Set(), arcs = new Set(), splines = new Set()
        for (const p of chosen) {
            if (p.kind === "sketch_curve") {
                splines.add(p.index)
            } else if (p.kind === "sketch_point") {
                points.add(p.index)
            } else if (p.kind === "sketch_arc") {
                arcs.add(p.index)
                const k = arcOf(p.index)
                if (k) { points.add(k.c); points.add(k.a); points.add(k.b) }
            } else {
                lines.add(p.index)
                const l = lineOf(p.index)
                if (l) { points.add(l.a); points.add(l.b) }
            }
        }
        const name = (kind, id) => id < 0 ? (kind === "P" ? "the Origin" : axisName(id))
                                 : (kind === "P" ? "Point" : kind === "A" ? (arcOf(id) && arcOf(id).circle ? "Circle" : "Arc") : "Line") + id
        const groups = {}
        const put = (kind, id, badge) => {
            if (id < 0)
                return
            const key = kind + id
            if (!groups[key]) {
                if (kind === "P") {
                    const q = pointAt(id)
                    if (!q) return
                    groups[key] = { at: "point", id: id, u: q.u, v: q.v, badges: [] }
                } else if (kind === "A") {
                    // An arc's badges sit by its middle (a circle's: its top), like a point's.
                    const k = arcOf(id), c = k && pointAt(k.c), a = k && pointAt(k.a), b = k && pointAt(k.b)
                    if (!c || !a || !b) return
                    const from = Math.atan2(a.v - c.v, a.u - c.u)
                    const turn = k.circle ? 0 : M.wrap(Math.atan2(b.v - c.v, b.u - c.u) - from)
                    const angle = k.circle ? Math.PI / 2 : from + turn / 2
                    groups[key] = { at: "point", id: id, u: c.u + k.r * Math.cos(angle), v: c.v + k.r * Math.sin(angle), badges: [] }
                } else {
                    const l = lineOf(id), a = l && pointAt(l.a), b = l && pointAt(l.b)
                    if (!a || !b) return
                    groups[key] = { at: "line", id: id, a: { u: a.u, v: a.v }, b: { u: b.u, v: b.v }, badges: [] }
                }
            }
            if (!groups[key].badges.some(x => x.id === badge.id))
                groups[key].badges.push(badge)
        }
        for (const c of g.constraints) {
            if (c.type === "curvature") {
                // On the join the two share (owner, 2026-11-02): shown when either curve, or the join, is picked.
                const other = c.third === 1 ? lines.has(c.second) : c.third === 2 ? arcs.has(c.second) : splines.has(c.second)
                if (!(picked.includes(c.id) || other || splines.has(c.first) || points.has(c.joint)))
                    continue
                const second = (c.third === 1 ? "Line" : c.third === 2 ? "Arc" : "Spline") + c.second
                put("P", c.joint, { id: c.id, picked: picked.includes(c.id), kind: "curvature",
                                    label: "Curvature: Spline" + c.first + ", " + second })
                continue
            }
            const kinds = constraintKinds[c.type]
            if (!kinds)
                continue                      // a dimension
            const ids = [c.first, c.second, c.third].slice(0, kinds.length)
            const mine = picked.includes(c.id) || ids.some((id, i) => kinds[i] === "P" ? points.has(id)
                                                                     : kinds[i] === "A" ? arcs.has(id) : lines.has(id))
            if (!mine)
                continue
            const badge = {
                id: c.id,
                picked: picked.includes(c.id),
                kind: { horizontalpoints: "horizontal", verticalpoints: "vertical", pointonline: "online",
                        symmetrylines: "symmetry", tangentarcs: "tangent", equalradius: "equal", pointonarc: "online",
                        midpoints: "midpoint", rotation: "pattern", translation: "pattern" }[c.type] || c.type,
                label: constraintNames[c.type] + ": " + ids.map((id, i) => name(kinds[i], id)).join(", ")
            }
            const on = ["midpoint", "pointonline", "pointonarc", "midpoints"].includes(c.type) ? [0]
                     : c.type === "rotation" || c.type === "translation" ? [1]
                     : kinds.length === 3 ? [0, 1] : ids.map((id, i) => i)
            for (const i of on)
                put(kinds[i], ids[i], badge)
        }
        return Object.values(groups)
    }

    // "the X axis": an axis of the sketch named after the model's axis it lies along.
    function axisName(id) {
        const along = id === -1 ? sketch.plane.xAxis : sketch.plane.yAxis
        const letter = Math.abs(along.x) > 0.9999 ? "X" : Math.abs(along.y) > 0.9999 ? "Y" : Math.abs(along.z) > 0.9999 ? "Z" : ""
        return letter ? "the " + letter + " axis" : id === -1 ? "the horizontal axis" : "the vertical axis"
    }
    readonly property var toolNames: ({ line: "Line", rectangle: "2-Point Rectangle", circle: "Center Diameter Circle",
                                        arc: "Center Point Arc", dimension: "Sketch Dimension",
                                        parallel: "Parallel", perpendicular: "Perpendicular", equal: "Equal",
                                        collinear: "Collinear", hv: "Horizontal/Vertical", coincident: "Coincident",
                                        midpoint: "Midpoint", symmetry: "Symmetry", fix: "Fix/UnFix",
                                        tangent: "Tangent", concentric: "Concentric", curvature: "Curvature", project: "Project" })
    readonly property string toolName: shapeDef ? shapeDef.name : toolNames[tool] || ""
    readonly property var drawingTools: ["line", "rectangle", "circle", "arc", "shape"]
    // Fusion's Construction switch (its Sketch Palette; X, owner 2026-10-30): what's drawn from
    // now on is construction. Stays on through other tools until switched off or the sketch closes.
    property bool construction: false
    readonly property bool picking: tool !== "" && !drawingTools.includes(tool)

    // Sketch Dimension picked what it measures: Main opens the value box.
    signal dimensionWanted(string type, int first, int second, bool otherSide)
    // A constraint tool has all its picks: Main applies it (keep: 1 when `second` was picked first).
    signal constraintWanted(string type, int first, int second, int third, int keep)

    // Project (Fusion's name): Whole Model throws the model's outline onto the sketch, Face just
    // that face's, curved or flat. Stays on for more, until Esc.
    property string projectWhat: "model"
    // What this Project run has put on the sketch, one entry per click or box, each one step in the
    // document's history: {what: "model" | "face" | "edge", face, kind, index}, or {what: "box",
    // items: [edge entries]}. The badge beside the tool's name counts the things: a whole body or a
    // face is 1, so is each edge or point (owner, 2026-11-01). Clicking one again takes it off; the
    // x beside the count takes them all off.
    property var projectPicks: []
    // Picks left out because that thing is already projected into this sketch (it is never put on
    // twice); the second bubble beside the count shows how many (owner, 2026-11-02).
    property var projectSkipped: []
    // ONLY a pick that an earlier Project run of this sketch already put on it counts as left out
    // (owner, 2026-11-02, t45u). Something refused because its outline lands on lines the same run
    // made (the front and back rim of a bore, a corner on an edge's end) is dropped without a word.
    property var projectDone: ({})            // sketch id -> the picks its earlier runs made
    function flatPicks(list) {
        return list.reduce((all, q) => all.concat(q.what === "box" ? q.items : [q]), [])
    }
    function projectRemember(id, picks) {
        if (!id || !picks.length)
            return
        const m = Object.assign({}, projectDone)
        m[id] = (m[id] || []).concat(flatPicks(picks))
        projectDone = m
    }
    function projectSkip(p) {
        const before = projectDone[projectSketch || sketch.sketchId] || []
        if (before.some(q => samePick(q, p)) && !projectSkipped.some(q => samePick(q, p)))
            projectSkipped = projectSkipped.concat([p])
    }
    readonly property int projectCount: projectPicks.reduce((n, p) => n + (p.what === "box" ? p.items.length : 1), 0)
    function projectHover(x, y) {
        const face = projectWhat === "face" ? selection.faceAt(x, y) : -1
        const outside = projectWhat === "sketch" || projectWhat === "wholesketch"
        const part = outside ? selection.outsideSketchAt(x, y)
                   : projectWhat === "edge" ? selection.bodyEdgeOrPointAt(x, y)
                   : face >= 0 ? { kind: "face", index: face } : null
        if (!selection.same(part, selection.hovered)) {
            selection.hovered = part
            selection.redraw()
        }
    }
    function samePick(a, b) {
        if (a.what !== b.what)
            return false
        if (a.what === "model")
            return true
        if (a.what === "face")
            return a.face === b.face
        if (a.what === "wholesketch")
            return a.sketch === b.sketch
        return a.kind === b.kind && a.index === b.index && a.sketch === b.sketch
    }
    function projectApply(p) {
        if (p.what === "sketchpart")
            return doc.projectSketchIntoSketch(sketch.sketchId, p.sketch, [{ kind: p.kind, index: p.index }])
        if (p.what === "wholesketch")
            return doc.projectSketchIntoSketch(sketch.sketchId, p.sketch, [])
        return p.what === "edge" ? doc.projectPartIntoSketch(sketch.sketchId, p.kind, p.index)
                                 : doc.projectIntoSketch(sketch.sketchId, p.what === "face" ? p.face : -1)
    }
    // A pick whose outline lands on lines this run already made (the front and back rim of a bore
    // seen end-on, a corner on an edge's end) is still TAKEN: it is highlighted and counted, it just
    // adds no new line (owner, 2026-11-02, t45u: the edges in front were left out and the ones
    // behind were taken, depending on which went first). Only a pick an earlier run of the sketch
    // already put on it is left out, and counted in the skipped bubble.
    function projectWasBefore(p) {
        return (projectDone[projectSketch || sketch.sketchId] || []).some(q => samePick(q, p))
    }
    function projectTaken(p, why) {
        if (!why)
            return true
        if (!projectQuiet(why))
            return false
        if (projectWasBefore(p)) {
            projectSkip(p)
            return false
        }
        return true
    }
    // A box's edges and points, as one step; the ones taken.
    // Edges and points go in as one batch, and faces as another, each in one go (a face a time took ~20 ms each).
    function projectBatch(items) {
        const faceItems = items.filter(p => p.what === "face"), partItems = items.filter(p => p.what !== "face")
        const taken = new Set()
        if (partItems.length) {
            const done = doc.projectPartsIntoSketch(sketch.sketchId, partItems.map(p => ({ kind: p.kind, index: p.index })))
            partItems.forEach((p, i) => { if (projectTaken(p, done[i].why)) taken.add(p) })
        }
        if (faceItems.length) {
            const done = doc.projectFacesIntoSketch(sketch.sketchId, faceItems.map(p => p.face))
            faceItems.forEach((p, i) => { if (projectTaken(p, done[i].why)) taken.add(p) })
        }
        return items.filter(p => taken.has(p))
    }
    // Each entry is one step in the history, so taking something off is: undo them all, then put
    // the ones that stay back in the same order.
    // The run is ONE history item all the way through (owner, 2026-11-02): each pick is joined into
    // it as soon as it is made, and taking something off undoes that one item and puts the rest back.
    function projectFold() {
        if (projectSketch)
            doc.foldProjectRun(projectSketch, projectCount)
    }
    // "Already projected" is no news: the same thing is never put on a sketch twice, quietly.
    function projectQuiet(why) { return /already projected/.test(why) }
    function projectRetake(keep) {
        doc.undoProjectRun()
        const again = []
        for (const p of keep) {
            if (p.what === "box") {
                const items = projectBatch(p.items)
                if (items.length)
                    again.push({ what: "box", items: items })
            } else if (projectTaken(p, projectApply(p).why)) {
                again.push(p)
            }
        }
        projectPicks = again
        projectFold()
    }
    function projectClearAll() {
        const n = projectCount, left = projectSkipped.length
        projectSkipped = []
        if (!n) {
            if (left)
                sketch.say(left === 1 ? "Cleared 1 left-out pick" : "Cleared " + left + " left-out picks")
            return
        }
        projectRetake([])
        selection.clearHover()
        sketch.say(n === 1 ? "Took 1 projection off the sketch" : "Took " + n + " projections off the sketch")
    }
    function projectToggle(p) {
        const inBox = (q, k) => q.what === "box" && q.items.some(i => samePick(i, p))
        const i = projectPicks.findIndex((q, k) => samePick(q, p) || inBox(q, k))
        if (i >= 0) {
            const q = projectPicks[i]
            const rest = q.what === "box" ? [{ what: "box", items: q.items.filter(it => !samePick(it, p)) }] : []
            const keep = []
            projectPicks.forEach((e, k) => {
                if (k !== i)
                    keep.push(e)
                else if (rest.length && rest[0].items.length)
                    keep.push(rest[0])
            })
            projectRetake(keep)
            return sketch.say("Took it off the sketch")
        }
        const made = projectApply(p)
        if (!projectTaken(p, made.why))
            return sketch.say(projectQuiet(made.why) ? "" : made.why)
        projectPicks = projectPicks.concat([p])
        projectFold()
        sketch.say(made.summary || "")
    }
    function projectAt(x, y) {
        if (projectWhat === "sketch" || projectWhat === "wholesketch") {
            const part = selection.outsideSketchAt(x, y)
            if (!part)
                return sketch.say("Click a line, arc or point of an earlier sketch")
            return projectToggle(projectWhat === "sketch" ? { what: "sketchpart", kind: part.kind, sketch: part.sketch, index: part.index }
                                                         : { what: "wholesketch", sketch: part.sketch })
        }
        if (projectWhat === "edge") {
            const part = selection.bodyEdgeOrPointAt(x, y)
            if (!part)
                return sketch.say("Click an edge or a corner of the model")
            return projectToggle({ what: "edge", kind: part.kind, index: part.index })
        }
        const face = selection.faceAt(x, y, projectWhat === "model")
        if (face < 0)
            return sketch.say("Click the model")
        projectToggle(projectWhat === "face" ? { what: "face", face: face } : { what: "model" })
    }
    // A box dragged while Project runs takes what the chosen kind takes: edges and points, faces, the
    // whole model, parts of earlier sketches or whole earlier sketches (owner, 2026-11-02). Left to
    // right takes what lies wholly inside, right to left whatever the box touches.
    function projectBox() {
        const hits = boxHits()
        const screen = part => part.paths.map(path => path.map(p => screenOf(p)))
        const have = p => projectPicks.some(q => samePick(q, p) || (q.what === "box" && q.items.some(i => samePick(i, p))))
        const bodyOn = !selection.bodyFiltered || selection.allow.bodies
        const crossing = box.x2 < box.x
        const left = Math.min(box.x, box.x2), right = Math.max(box.x, box.x2)
        const top = Math.min(box.y, box.y2), bottom = Math.max(box.y, box.y2)
        const wants = []                 // what the box took, as project picks (some may be there already)
        let what = "edges or points"
        if (projectWhat === "edge") {
            const edges = selection.allow.edges || !selection.bodyFiltered
            const points = selection.allow.bodyPoints || !selection.bodyFiltered
            const near = doc.bodyOutlines(edges && bodyOn, points && bodyOn).filter(part => hits(screen(part)))
            for (const part of seenOutlines(near, crossing, left, right, top, bottom))
                wants.push({ what: "edge", kind: part.kind, index: part.index })
        } else if (projectWhat === "face") {
            what = "faces"
            if (bodyOn && (selection.allow.faces || !selection.bodyFiltered))
                for (const f of boxFaces(hits, crossing, left, right, top, bottom))
                    wants.push({ what: "face", face: f })
        } else if (projectWhat === "model") {
            what = "model"
            if (bodyOn && boxTouchesModel(hits, crossing, left, right, top, bottom))
                wants.push({ what: "model" })
        } else if (projectWhat === "sketch" || projectWhat === "wholesketch") {
            what = projectWhat === "sketch" ? "parts of earlier sketches" : "earlier sketches"
            if (!selection.bodyFiltered || selection.allow.outside) {
                const earlier = doc.earlierSketches(sketch.sketchId)
                for (const id of selection.outsideSketches.filter(id => earlier.indexOf(id) >= 0)) {
                    const parts = doc.sketchOutlines(id, 15)
                    if (projectWhat === "wholesketch") {
                        if (parts.length && hits(allPaths(parts)))
                            wants.push({ what: "wholesketch", sketch: id })
                        continue
                    }
                    const taken = parts.filter(part => hits(screen(part)))
                    const g = doc.sketchGeometry(id)
                    const owned = new Set()
                    const has = (kind, i) => taken.some(p => p.kind === kind && p.index === i)
                    for (const l of g.lines) if (has("sketch_line", l.id)) { owned.add(l.a); owned.add(l.b) }
                    for (const a of g.arcs) if (has("sketch_arc", a.id)) { owned.add(a.c); owned.add(a.a); owned.add(a.b) }
                    for (const c of g.curves) if (has("sketch_curve", c.id)) for (const q of c.points) owned.add(q)
                    for (const part of taken)
                        if (part.kind !== "sketch_point" || !owned.has(part.index))
                            wants.push({ what: "sketchpart", kind: part.kind, sketch: id, index: part.index })
                }
            }
        }
        const mode = box.mode
        box = null
        if (mode === "remove") {                         // Ctrl: take off what the box reaches, add nothing
            const was = projectCount
            const keep = []
            for (const q of projectPicks) {
                if (q.what === "box") {
                    const items = q.items.filter(i => !wants.some(w => samePick(i, w)))
                    if (items.length)
                        keep.push({ what: "box", items: items })
                } else if (!wants.some(w => samePick(q, w))) {
                    keep.push(q)
                }
            }
            const gone = was - keep.reduce((n, q) => n + (q.what === "box" ? q.items.length : 1), 0)
            if (gone > 0)
                projectRetake(keep)
            return sketch.say(gone > 0 ? "Took " + gone + (gone === 1 ? " projection" : " projections") + " off the sketch"
                                       : "The box reached nothing that is projected")
        }
        const fresh = wants.filter(p => !have(p))
        let added = 0
        if (projectWhat === "edge" || projectWhat === "face") {
            const taken = fresh.length ? projectBatch(fresh) : []
            if (taken.length)
                projectPicks = projectPicks.concat([{ what: "box", items: taken }])
            added = taken.length
        } else {
            for (const p of fresh) {                     // each one its own step, like a click
                const r = projectApply(p)
                if (!projectTaken(p, r.why))
                    continue
                projectPicks = projectPicks.concat([p])
                added++
            }
        }
        if (added)
            projectFold()
        sketch.say(!wants.length ? (what === "model" ? "The box took no part of the model" : "The box took no " + what)
                   : added ? "Projected " + added + (projectWhat === "edge" ? (added === 1 ? " edge or point" : " edges and points")
                                                    : projectWhat === "face" ? (added === 1 ? " face" : " faces")
                                                    : projectWhat === "model" ? " model"
                                                    : projectWhat === "sketch" ? (added === 1 ? " sketch part" : " sketch parts")
                                                    : (added === 1 ? " sketch" : " sketches"))
                   : "")
    }
    // The faces a box takes. A window takes faces whose whole rim is inside; a crossing also takes
    // any face the box touches, or lies wholly within (found by what is seen under the box's corners
    // and middle, since such a face has no rim line near the box).
    // A box takes only what the camera can see, never what lies behind the model's wall (the thread
    // inside a bore, the far side). Select Through, to take hidden things too, is a later addition
    // (owner, 2026-11-02); this switch is where it will plug in.
    property bool selectThrough: false
    function eyeForHidden() {
        const r = selection.rayAt(0, 0)          // the camera's place, or its direction when ortho
        return selection.ortho ? r.direction : r.origin
    }
    // Which of these model edges and corner points (as bodyOutlines gives them) are seen inside the
    // box: samples along each (nine on an edge), only those inside the box count when it touches, and
    // an edge window-selected needs at least half of its samples seen.
    function seenOutlines(parts, crossing, left, right, top, bottom) {
        if (selectThrough || !parts.length)
            return parts
        const all = [], owner = []
        const inBox = p => { const q = screenOf(p); return q.x >= left && q.x <= right && q.y >= top && q.y <= bottom }
        parts.forEach((part, k) => {
            const path = part.paths[0]
            const n = path.length
            const marks = []
            if (n === 1) {
                marks.push(path[0])
            } else {
                let total = 0
                const len = [0]
                for (let i = 1; i < n; ++i) {
                    total += path[i].minus(path[i - 1]).length()
                    len.push(total)
                }
                let at = 1
                for (let m = 0; m < 9; ++m) {
                    const want = total * m / 8
                    while (at < n - 1 && len[at] < want)
                        at++
                    const span = len[at] - len[at - 1]
                    const f = span > 0 ? Math.min(1, Math.max(0, (want - len[at - 1]) / span)) : 0
                    marks.push(path[at - 1].times(1 - f).plus(path[at].times(f)))
                }
            }
            let inside = marks.filter(inBox)
            if (!inside.length)
                inside = marks
            for (const m of inside) {
                all.push(m)
                owner.push(k)
            }
        })
        const hidden = doc.hiddenFrom(all, eyeForHidden(), selection.ortho)
        const seen = new Array(parts.length).fill(0), total = new Array(parts.length).fill(0)
        hidden.forEach((h, i) => { total[owner[i]]++; if (!h) seen[owner[i]]++ })
        return parts.filter((part, k) => crossing ? seen[k] > 0 : seen[k] * 2 >= total[k] && seen[k] > 0)
    }
    // Which of these faces the camera sees any of (the middles of a dozen of its triangles).
    function seenFaces(list) {
        if (selectThrough || !list.length)
            return list
        const all = [], owner = []
        list.forEach((f, k) => {
            for (const m of doc.faceSamples(f, 12)) {
                all.push(m)
                owner.push(k)
            }
        })
        const hidden = doc.hiddenFrom(all, eyeForHidden(), selection.ortho)
        const seen = new Array(list.length).fill(false)
        hidden.forEach((h, i) => { if (!h) seen[owner[i]] = true })
        return list.filter((f, k) => seen[k])
    }
    function boxFaces(hits, crossing, left, right, top, bottom) {
        const taken = new Set()
        const rimmed = []
        for (const part of doc.faceOutlines())
            if (hits(part.paths.map(path => path.map(p => screenOf(p)))))
                rimmed.push(part.index)
        for (const f of seenFaces(rimmed))
            taken.add(f)
        if (crossing)
            for (const [x, y] of [[left, top], [right, top], [left, bottom], [right, bottom], [(left + right) / 2, (top + bottom) / 2]]) {
                const f = selection.faceAt(x, y, true)
                if (f >= 0)
                    taken.add(f)
            }
        return Array.from(taken).sort((a, b) => a - b)
    }
    // Every path of several parts, on screen (the model's lists have no flatMap).
    function allPaths(parts) {
        const paths = []
        for (const part of parts)
            for (const path of part.paths)
                paths.push(path.map(p => screenOf(p)))
        return paths
    }
    function boxTouchesModel(hits, crossing, left, right, top, bottom) {
        const near = doc.bodyOutlines(true, false).filter(part => hits(part.paths.map(path => path.map(p => screenOf(p)))))
        if (seenOutlines(near, crossing, left, right, top, bottom).length)
            return true
        return crossing && [[left, top], [right, top], [left, bottom], [right, bottom], [(left + right) / 2, (top + bottom) / 2]]
                           .some(([x, y]) => selection.faceAt(x, y, true) >= 0)
    }
    // A box dragged in the model, outside a sketch: the faces, edges and corners it takes are selected.
    function selectModelBox() {
        const hits = boxHits()
        const crossing = box.x2 < box.x
        const left = Math.min(box.x, box.x2), right = Math.max(box.x, box.x2)
        const top = Math.min(box.y, box.y2), bottom = Math.max(box.y, box.y2)
        const screen = part => part.paths.map(path => path.map(p => screenOf(p)))
        const parts = []
        if (selection.pickable) {
            const near = doc.bodyOutlines(true, true).filter(part => hits(screen(part)))
            for (const part of seenOutlines(near, crossing, left, right, top, bottom))
                parts.push({ kind: part.kind, index: part.index })
            for (const f of boxFaces(hits, crossing, left, right, top, bottom))
                parts.push({ kind: "face", index: f })
        }
        const taken = parts.filter(p => selection.allowed(p))
        const same = (p, q) => p.kind === q.kind && p.index === q.index
        const mode = box.mode
        const changed = applyBox(mode, taken, taken)
        box = null
        selection.redraw()
        sketch.say(mode === "remove" ? (changed ? "Took " + changed + (changed === 1 ? " part" : " parts") + " off the selection" : "The box took nothing that was selected")
                   : taken.length ? (mode === "add" ? "Added " + changed : "Selected " + taken.length) + (changed === 1 && mode === "add" || taken.length === 1 && mode !== "add" ? " part" : " parts")
                   : "The box took nothing")
    }

    // One Project run (start to Done, or to the next click on Project) is one step in the history: the projections made meanwhile
    // are joined when the run ends, by Done, Esc, another tool or leaving the sketch (owner, 2026-11-02).
    property int projectSketch: 0
    function endProject() {
        if (!projectSketch)
            return
        const id = projectSketch
        projectSketch = 0
        projectRemember(id, projectPicks)
        doc.endProjectRun(id, projectCount)
    }
    function use(name) {
        endProject()     // choosing Project again ends the run before and starts a new one (its own history item)
        if (Shapes.tools[name])
            return useShape(name)
        if (name === "project") {
            projectPicks = []
            projectSkipped = []
            projectSketch = sketch.sketchId
            doc.beginProjectRun(projectSketch)
        }
        tool = name
        shape = ""
        dimFirst = null
        dimLine = 0
        picks = []
        start = null
        arcStart = null
        chainFirst = 0
        cursor = null
        selection.clear()
    }
    function cancel() {
        endProject()
        box = null
        if (tool && (!picking || tool === "project"))
            selection.clearHover()
        if (tool === "shape" && gather.length)
            selection.clear()
        tool = ""
        shape = ""
        clicks = []
        gather = []
        hoverPart = null
        cursorEnd = null
        handleGrab = null
        dimFirst = null
        dimLine = 0
        picks = []
        start = null
        arcStart = null
        chainFirst = 0
        cursor = null
    }
    // After Ctrl+Z: the tool stays, but starts afresh.
    function reset() {
        if (tool === "project" && (projectPicks.length || projectSkipped.length)) {
            projectPicks = []   // the one step Ctrl+Z takes back is the whole run
            projectSkipped = []
        }
        clicks = []
        gather = []
        dimFirst = null
        dimLine = 0
        picks = []
        start = null
        arcStart = null
        chainFirst = 0
    }

    // The mouse on the sketch plane, snapped (see the top): {u, v, point? | on, line?, label, shown}.
    function snapAt(x, y) {
        const p = sketch.plane
        if (!p)
            return null
        const r = selection.rayAt(x, y)
        const k = selection.perPixel()
        if (free) {                       // Ctrl held: exactly where the mouse is
            const raw = onPlane(x, y)
            return raw ? { u: raw.u, v: raw.v } : null
        }
        // A hidden sketch's own lines and points aren't snapped to (only the Origin and axes).
        const hit = geometryShown ? doc.pickSketch(sketch.sketchId, r.origin, r.direction, snapPixels * k, 0) : {}
        const all = doc.sketchGeometry(sketch.sketchId)
        const g = geometryShown ? all : { origin: all.origin, points: [], lines: [], arcs: [] }
        if (hit.kind === "sketch_point") {
            const found = g.points.find(q => q.id === hit.index)
            if (found)
                return pointSnap(found)
        }
        const at = onPlane(x, y)
        if (!at)
            return null
        return snapNear(at, snapPixels * k * at.far, g, null)   // reach: mm on the sketch under snapPixels
    }
    function pointSnap(q) {
        return { u: q.u, v: q.v, point: q.id, label: "Point" + q.id,
                 shown: { kind: "sketch_point", sketch: sketch.sketchId, index: q.id } }
    }
    // What's within `reach` (mm) of `at` on the sketch, in snapAt's order, leaving out
    // skip.points and skip.lines (a drag's own points and the lines that move with them);
    // {u, v} alone when nothing is.
    function snapNear(at, reach, g, skip) {
        const near = q => Math.hypot(q.u - at.u, q.v - at.v)
        if (skip) {
            let closest = null
            for (const q of g.points)
                if (!q.hidden && !skip.points.has(q.id) && near(q) <= (closest ? near(closest) : reach))
                    closest = q
            if (closest)
                return pointSnap(closest)
        }
        const left = skip ? g.lines.filter(l => !skip.lines.has(l.id)) : g.lines
        if (near(g.origin) <= reach)
            return { u: g.origin.u, v: g.origin.v, on: "origin", label: "Origin",
                     shown: { kind: "origin", name: "O", part: "point", index: -1 } }
        let best = null, bestGap = reach
        for (const l of left) {
            const a = g.points.find(q => q.id === l.a), b = g.points.find(q => q.id === l.b)
            const mid = { u: (a.u + b.u) / 2, v: (a.v + b.v) / 2 }
            if (near(mid) <= bestGap) {
                bestGap = near(mid)
                best = { u: mid.u, v: mid.v, on: "midpoint", line: l.id, label: "Midpoint of Line" + l.id }
            }
        }
        if (best)
            return best
        // Anywhere along a line (between its ends), or along an axis.
        bestGap = reach
        for (const l of left) {
            const a = g.points.find(q => q.id === l.a), b = g.points.find(q => q.id === l.b)
            const du = b.u - a.u, dv = b.v - a.v, len2 = du * du + dv * dv
            if (len2 < 1e-12)
                continue
            const s = Math.max(0, Math.min(1, ((at.u - a.u) * du + (at.v - a.v) * dv) / len2))
            const foot = { u: a.u + du * s, v: a.v + dv * s }
            if (near(foot) <= bestGap) {
                bestGap = near(foot)
                best = { u: foot.u, v: foot.v, on: "line", line: l.id, label: "On Line" + l.id }
            }
        }
        if (best)
            return best
        // Anywhere along an arc or a circle (held on it by Coincident).
        bestGap = reach
        for (const k of g.arcs) {
            if (skip && (skip.points.has(k.c) || skip.points.has(k.a)))
                continue
            const c = g.points.find(q => q.id === k.c), a = g.points.find(q => q.id === k.a), b = g.points.find(q => q.id === k.b)
            const away = Math.hypot(at.u - c.u, at.v - c.v)
            if (away < 1e-9 || Math.abs(away - k.r) > bestGap)
                continue
            const foot = { u: c.u + (at.u - c.u) / away * k.r, v: c.v + (at.v - c.v) / away * k.r }
            if (!k.circle && !M.ccwBetween(c, a, b, foot))
                continue
            bestGap = Math.abs(away - k.r)
            best = { u: foot.u, v: foot.v, on: "arc", arc: k.id, label: "On " + (k.circle ? "Circle" : "Arc") + k.id }
        }
        if (best)
            return best
        if (Math.abs(at.v - g.origin.v) <= reach)
            return { u: at.u, v: g.origin.v, on: "line", line: -1, label: "On " + axisName(-1) }
        if (Math.abs(at.u - g.origin.u) <= reach)
            return { u: g.origin.u, v: at.v, on: "line", line: -2, label: "On " + axisName(-2) }
        return { u: at.u, v: at.v }
    }

    // A Line's next end, snapped level or upright when it's within levelDegrees of it (and
    // hasn't already snapped onto something).
    function aim(at) {
        if (!at || free || tool !== "line" || !start || at.point || at.on)
            return at
        const du = at.u - start.u, dv = at.v - start.v
        if (!du && !dv)
            return at
        const degrees = Math.atan2(Math.abs(dv), Math.abs(du)) * 180 / Math.PI
        if (degrees <= levelDegrees)
            return { u: at.u, v: start.v, hold: "horizontal", label: "Horizontal" }
        if (degrees >= 90 - levelDegrees)
            return { u: start.u, v: at.v, hold: "vertical", label: "Vertical" }
        return at
    }

    // Lights what the mouse snapped to (nothing, when it snapped to nothing).
    function showSnap(at) {
        const part = at && at.shown ? at.shown : null
        if (!selection.same(part, selection.hovered) && (part || selection.hovered)) {
            selection.hovered = part
            selection.redraw()
        }
    }

    // The mouse on the sketch plane, not snapped: {u, v, far: how far from the eye}, or null.
    function onPlane(x, y) {
        const p = sketch.plane
        if (!p)
            return null
        const r = selection.rayAt(x, y)
        const along = r.direction.dotProduct(p.normal)
        if (Math.abs(along) < 1e-9)
            return null
        const t = p.origin.minus(r.origin).dotProduct(p.normal) / along
        if (t <= 0)
            return null
        const at = r.origin.plus(r.direction.times(t)).minus(p.origin)
        return { u: at.dotProduct(p.xAxis), v: at.dotProduct(p.yAxis), far: t * r.direction.length() }
    }

    function move(x, y) {
        if (tool === "shape")
            return shapeMove(x, y)
        if (tool === "project")
            return projectHover(x, y)
        if (picking)
            return selection.hoverAt(x, y)
        mouse = Qt.point(x, y)
        cursor = aim(snapAt(x, y))
        showSnap(cursor)
        if (tool === "arc" && arcStart && cursor) {
            const angle = Math.atan2(cursor.v - start.v, cursor.u - start.u)
            let step = angle - sweepAngle
            while (step > Math.PI) step -= 2 * Math.PI
            while (step <= -Math.PI) step += 2 * Math.PI
            sweep += step
            sweepAngle = angle
        }
    }
    // A Center Point Arc's far end: where the mouse points, at the arc's radius.
    function arcEnd() {
        const r = Math.hypot(arcStart.u - start.u, arcStart.v - start.v)
        const a = Math.atan2(cursor.v - start.v, cursor.u - start.u)
        return { u: start.u + r * Math.cos(a), v: start.v + r * Math.sin(a) }
    }

    // Sketch Dimension: a line on its own, or a point and then a point or a line. spot: where
    // on the sketch it was clicked (two lines' angle is on the clicked side of their corner).
    function measureWith(part, spot) {
        part = asSketchPart(part)
        if (part && part.why)
            return sketch.say(part.why)
        if (!part || !part.kind.startsWith("sketch_"))
            return
        const axis = part.kind === "sketch_line" && part.index < 0
        // An arc: its radius; a circle: its diameter (as in Fusion).
        if (part.kind === "sketch_arc") {
            dimLine = 0
            dimFirst = null
            const k = doc.sketchGeometry(sketch.sketchId).arcs.find(q => q.id === part.index)
            if (k)
                dimensionWanted(k.circle ? "diameter" : "radius", part.index, 0, false)
            return
        }
        // A line's length box is open and another line is clicked: the gap between the two
        // (an axis goes first, so the gap is measured from the line's middle); the angle
        // between them when they aren't parallel.
        if (dimLine && !dimFirst && part.kind === "sketch_line" && part.index !== dimLine) {
            const first = dimLine
            dimLine = 0
            if (!linesParallel(first, part.index)) {
                angleSpots = axis ? [spot, dimSpot] : [dimSpot, spot]
                return dimensionWanted("angle", axis ? part.index : first, axis ? first : part.index,
                                       axis ? angleOtherSide(part.index, first, spot, dimSpot)
                                            : angleOtherSide(first, part.index, dimSpot, spot))
            }
            dimensionWanted("lineline", axis ? part.index : first, axis ? first : part.index, false)
            return
        }
        dimLine = 0
        if (!dimFirst) {
            dimSpot = spot
            if (part.kind === "sketch_line" && !axis) {
                dimLine = part.index
                dimensionWanted("length", part.index, 0, false)
                sketch.say("Type its length, or click a second line for the angle between them.")
            } else {
                dimFirst = part
                selection.selected = lit([part])
                selection.redraw()
                sketch.say(axis ? "Now a point, or a line parallel to the axis." : "Now the second point, or a line.")
            }
            return
        }
        const first = dimFirst
        if (part.kind === first.kind && part.index === first.index)
            return
        const firstAxis = first.kind === "sketch_line"
        if (firstAxis && axis)
            return sketch.say("Now a point, or a line parallel to the axis.")
        if (firstAxis && part.kind === "sketch_line" && !linesParallel(first.index, part.index)) {
            dimFirst = null
            selection.clear()
            angleSpots = [dimSpot, spot]
            return dimensionWanted("angle", first.index, part.index, angleOtherSide(first.index, part.index, dimSpot, spot))
        }
        dimFirst = null
        selection.clear()
        if (firstAxis)
            dimensionWanted(part.kind === "sketch_point" ? "pointline" : "lineline",
                            part.kind === "sketch_point" ? part.index : first.index,
                            part.kind === "sketch_point" ? first.index : part.index, false)
        else
            dimensionWanted(part.kind === "sketch_point" ? "distance" : "pointline", first.index, part.index, false)
    }

    // Two lines' angle is the corner's on the side picked: the arms run from where the lines
    // cross towards the spots clicked (towards their middles when picked beforehand), as in
    // Fusion. true when the second line's arm runs against its drawing direction while the
    // first's doesn't, or the other way round (Document.addSketchDimension's otherSide).
    function angleOtherSide(l1, l2, s1, s2) {
        const g = doc.sketchGeometry(sketch.sketchId)
        const ends = id => {
            if (id === -1) return [M.P(0, 0), M.P(1, 0)]
            if (id === -2) return [M.P(0, 0), M.P(0, 1)]
            const l = g.lines.find(q => q.id === id)
            return l ? [g.points.find(q => q.id === l.a), g.points.find(q => q.id === l.b)] : null
        }
        const one = ends(l1), two = ends(l2)
        const x = one && two ? M.lineLine(one[0], one[1], two[0], two[1]) : null
        if (!x)
            return false
        const arm = (e, s) => M.dot(M.sub(s || M.mid(e[0], e[1]), x.at), M.sub(e[1], e[0])) < 0 ? -1 : 1
        return arm(one, s1) * arm(two, s2) < 0
    }

    // Whether two lines (or an axis) run the same way.
    function linesParallel(l1, l2) {
        const g = doc.sketchGeometry(sketch.sketchId)
        const dir = id => {
            if (id === -1) return M.P(1, 0)
            if (id === -2) return M.P(0, 1)
            const l = g.lines.find(q => q.id === id), a = g.points.find(q => q.id === l.a), b = g.points.find(q => q.id === l.b)
            return M.norm(M.sub(b, a))
        }
        return Math.abs(M.cross(dir(l1), dir(l2))) < 1e-6
    }

    // A picked part as the sketch sees it: the origin's point and in-sketch axes become the
    // sketch's Origin / axes ({kind: "sketch_…", index < 0, shown: the origin part, still lit
    // as itself}); an axis square to the sketch or a plane → {why}; anything else as it is.
    function asSketchPart(part, curveOk) {
        if (part && part.kind === "sketch_curve") {
            // Curvature works on a spline itself; the rest of the constraints take their points.
            const k = doc.sketchGeometry(sketch.sketchId).curves.find(q => q.id === part.index)
            if (curveOk && k && (k.type === "fitspline" || k.type === "controlspline"))
                return part
            return { why: curveOk ? "Curvature works on splines, lines and arcs: this curve isn't a spline."
                                  : "Splines, ellipses and text take constraints through their points: pick a point." }
        }
        if (!part || part.kind !== "origin")
            return part
        const as = (kind, index) => ({ kind: kind, sketch: sketch.sketchId, index: index, shown: part })
        if (part.part === "point")
            return as("sketch_point", -1)
        if (part.part === "axis") {
            const along = { X: Qt.vector3d(1, 0, 0), Y: Qt.vector3d(0, 1, 0), Z: Qt.vector3d(0, 0, 1) }[part.name]
            if (Math.abs(along.dotProduct(sketch.plane.xAxis)) > 0.9999)
                return as("sketch_line", -1)
            if (Math.abs(along.dotProduct(sketch.plane.yAxis)) > 0.9999)
                return as("sketch_line", -2)
            return { why: "The " + part.name + " axis points straight out of this sketch: pick one lying in it." }
        }
        return { why: "Pick a point, a line, an arc or a circle (the origin's point and axes count too)." }
    }
    // How picks are lit: the origin's parts as themselves.
    function lit(list) { return list.map(p => p.shown || p) }

    // What these picks make for a constraint tool: {done: {type, first, second, third, keep}},
    // {more: what to click next}, or {wrong: why that last pick doesn't fit}.
    function ruleFor(name, list) {
        const kinds = list.map(p => p.kind === "sketch_line" ? "L" : p.kind === "sketch_arc" ? "A" : p.kind === "sketch_curve" ? "C" : "P").join("")
        const id = i => list[i].index
        const label = toolNames[name]
        const g = () => doc.sketchGeometry(sketch.sketchId)
        const at = pid => pid === -1 ? g().origin : g().points.find(q => q.id === pid)
        if (name === "equal" && kinds[0] === "A") {
            if (kinds.length > 1 && kinds[1] !== "A")
                return { wrong: "Equal: two lines, or two arcs or circles." }
            return kinds.length < 2 ? { more: "Now the second arc or circle." }
                                    : { done: { type: "equalradius", first: id(0), second: id(1), third: 0, keep: 0 } }
        }
        if (name === "equal" && kinds.includes("A"))
            return { wrong: "Equal: two lines, or two arcs or circles." }
        if (name === "curvature") {
            // A spline's end runs on from a line, an arc or another spline's end (owner, 2026-11-02).
            if (kinds.includes("P"))
                return { wrong: "Curvature works on curves: click a spline, then the line, arc or spline it meets." }
            if (kinds.length < 2)
                return { more: kinds === "C" ? "Now the line, arc or spline it meets at its end." : "Now the spline." }
            if (!kinds.includes("C"))
                return { wrong: "Curvature needs a spline: click one." }
            if (kinds === "CC")
                return { done: { type: "curvature", first: id(1), second: id(0), third: 3, keep: 0 } }
            const sp = kinds === "CL" || kinds === "CA" ? 0 : 1
            const other = kinds.replace("C", "")
            return { done: { type: "curvature", first: id(sp), second: id(1 - sp), third: other === "L" ? 1 : 2, keep: 0 } }
        }
        if (name === "tangent") {
            if (kinds.includes("P"))
                return { wrong: "Tangent works on lines, arcs and circles: click one." }
            if (kinds.length < 2)
                return { more: kinds === "L" ? "Now the arc or circle." : "Now a line, arc or circle." }
            if (kinds === "LL")
                return { wrong: "Two lines can't be tangent: click an arc or circle." }
            if (kinds === "AA")
                return { done: { type: "tangentarcs", first: id(0), second: id(1), third: 0, keep: 0 } }
            const l = kinds === "LA" ? 0 : 1
            return { done: { type: "tangent", first: id(l), second: id(1 - l), third: 0, keep: kinds === "LA" ? 0 : 1 } }
        }
        if (name === "concentric") {
            if (kinds.includes("P") || kinds.includes("L"))
                return { wrong: "Concentric works on arcs and circles: click one." }
            return kinds.length < 2 ? { more: "Now the second arc or circle." }
                                    : { done: { type: "concentric", first: id(0), second: id(1), third: 0, keep: 0 } }
        }
        if (["parallel", "perpendicular", "equal", "collinear"].includes(name)) {
            if (kinds.includes("P") || kinds.includes("A"))
                return { wrong: label + " works on lines: click a line." }
            return kinds.length < 2 ? { more: "Now the second line." }
                                    : { done: { type: name, first: id(0), second: id(1), third: 0, keep: 0 } }
        }
        if (name === "hv") {
            if (kinds.includes("A"))
                return { wrong: "Horizontal/Vertical: click a line, or two points." }
            if (kinds === "L" && id(0) < 0)
                return { wrong: "The axes are already level and upright." }
            if (kinds === "L") {
                const l = g().lines.find(q => q.id === id(0)), a = at(l.a), b = at(l.b)
                return { done: { type: Math.abs(b.u - a.u) >= Math.abs(b.v - a.v) ? "horizontal" : "vertical",
                                 first: id(0), second: 0, third: 0, keep: 0 } }
            }
            if (kinds === "P")
                return { more: "Now the second point." }
            if (kinds === "PP") {
                const a = at(id(0)), b = at(id(1))
                return { done: { type: Math.abs(b.u - a.u) >= Math.abs(b.v - a.v) ? "horizontalpoints" : "verticalpoints",
                                 first: id(0), second: id(1), third: 0, keep: 0 } }
            }
            return { wrong: "Horizontal/Vertical: click a line, or two points." }
        }
        if (name === "coincident" || name === "midpoint") {
            if (kinds === "P")
                return { more: name === "midpoint" ? "Now the line." : "Now a point, or a line to put it on." }
            if (kinds === "L")
                return { more: "Now the point." }
            if (kinds === "PP" && name === "coincident")
                return { done: { type: "coincident", first: id(0), second: id(1), third: 0, keep: 0 } }
            if (kinds === "A")
                return name === "coincident" ? { more: "Now the point." } : { wrong: "Midpoint: a point and a line." }
            if ((kinds === "PA" || kinds === "AP") && name === "coincident") {
                const p = kinds === "PA" ? 0 : 1
                return { done: { type: "pointonarc", first: id(p), second: id(1 - p), third: 0, keep: kinds === "PA" ? 0 : 1 } }
            }
            if (kinds === "PL" || kinds === "LP") {
                const p = kinds === "PL" ? 0 : 1
                return { done: { type: name === "midpoint" ? "midpoint" : "pointonline",
                                 first: id(p), second: id(1 - p), third: 0, keep: kinds === "PL" ? 0 : 1 } }
            }
            return { wrong: name === "midpoint" ? "Midpoint: a point and a line." : "Coincident: a point, then a point or a line." }
        }
        if (name === "symmetry") {
            if (kinds.includes("A"))
                return { wrong: "Symmetry pairs two points or two lines." }
            if (kinds.length === 1)
                return { more: "Now the one that mirrors it." }
            if (kinds[0] !== kinds[1])
                return { wrong: "Symmetry pairs two points or two lines." }
            if (kinds.length === 2)
                return { more: "Now the line to mirror across." }
            if (kinds[2] !== "L")
                return { wrong: "Mirror across a line: click a line." }
            return { done: { type: kinds[0] === "P" ? "symmetry" : "symmetrylines",
                             first: id(0), second: id(1), third: id(2), keep: 0 } }
        }
        return { wrong: "" }
    }

    // A constraint asked for with things already selected (owner, 2026-11-02, t50u; Fusion's way): the
    // whole selection is read at once, never pair by pair. Returns {steps: [{type, first, second,
    // third, keep}]} to apply now, {more: text} when the selection is too little (the tool then waits
    // for clicks), or {wrong: text}. The first pick is the one the others follow (Equal follows a
    // line that already has a length, a Coincident the origin when it's among the points).
    //   Parallel / Collinear / Equal / Concentric   every other one follows the first
    //   Perpendicular, Tangent, Midpoint            exactly two
    //   Horizontal/Vertical   lines: all take the orientation most of them lean to; points: all line up with the first
    //   Coincident   points join the first (or the origin); one line, arc or circle among points: each point goes on it
    //   Symmetry     two points or two lines, then the line they mirror across (three picks)
    function batchPlan(name, list) {
        const parts = list.map(p => asSketchPart(p, name === "curvature"))
        const bad = parts.find(p => p && p.why)
        if (bad)
            return { wrong: bad.why }
        const ok = parts.filter(p => p && p.kind && p.kind.startsWith("sketch_"))
        if (ok.length === 0)
            return { more: "" }
        const label = toolNames[name]
        const lines = ok.filter(p => p.kind === "sketch_line"), arcs = ok.filter(p => p.kind === "sketch_arc")
        const points = ok.filter(p => p.kind === "sketch_point")
        const g = doc.sketchGeometry(sketch.sketchId)
        const at = pid => pid === -1 ? g.origin : g.points.find(q => q.id === pid)
        const pairs = (base, others) => {
            const steps = []
            for (const o of others) {
                const r = ruleFor(name, [base, o])
                if (r.wrong !== undefined)
                    return { wrong: r.wrong }
                if (r.done)
                    steps.push(r.done)
            }
            return { steps: steps }
        }
        const dimensioned = p => g.constraints.some(c => c.first === p.index
            && (p.kind === "sketch_line" ? c.type === "length" : c.type === "radius" || c.type === "diameter"))
        if (name === "parallel" || name === "collinear") {
            if (lines.length !== ok.length)
                return { wrong: label + " works on lines: select only lines." }
            if (lines.length < 2)
                return { more: "Now the second line." }
            return pairs(lines[0], lines.slice(1))
        }
        if (name === "perpendicular") {
            if (lines.length !== ok.length)
                return { wrong: label + " works on lines: select only lines." }
            if (lines.length < 2)
                return { more: "Now the second line." }
            if (lines.length > 2)
                return { wrong: "Perpendicular takes exactly two lines." }
            return pairs(lines[0], [lines[1]])
        }
        if (name === "equal") {
            const same = lines.length === ok.length ? lines : arcs.length === ok.length ? arcs : null
            if (!same)
                return { wrong: "Equal: only lines, or only arcs and circles." }
            if (same.length < 2)
                return { more: same === lines ? "Now the second line." : "Now the second arc or circle." }
            // When some already have a size, the others take it; with none, all take the first one's.
            const base = same.find(dimensioned) || same[0]
            return pairs(base, same.filter(p => p !== base))
        }
        if (name === "concentric") {
            if (arcs.length !== ok.length)
                return { wrong: "Concentric works on arcs and circles: select only those." }
            return arcs.length < 2 ? { more: "Now the second arc or circle." } : pairs(arcs[0], arcs.slice(1))
        }
        if (name === "curvature") {
            if (ok.length < 2)
                return { more: ok.length && ok[0].kind === "sketch_curve" ? "Now the line, arc or spline it meets at its end." : "Now the spline." }
            if (ok.length > 2)
                return { wrong: "Curvature takes two at a time: a spline and what it meets." }
            const r = ruleFor(name, ok)
            return r.done ? { steps: [r.done] } : r.wrong !== undefined ? { wrong: r.wrong } : { more: r.more }
        }
        if (name === "tangent") {
            if (ok.length < 2)
                return { more: lines.length ? "Now the arc or circle." : "Now a line, arc or circle." }
            if (ok.length > 2)
                return { wrong: "Tangent takes two at a time: a line, arc or circle and an arc or circle." }
            const r = ruleFor(name, ok)
            return r.done ? { steps: [r.done] } : r.wrong !== undefined ? { wrong: r.wrong } : { more: r.more }
        }
        if (name === "midpoint") {
            if (ok.length < 2)
                return { more: points.length ? "Now the line." : "Now the point." }
            if (!(ok.length === 2 && points.length === 1 && lines.length === 1))
                return { wrong: "Midpoint: a point and a line." }
            const r = ruleFor(name, ok)
            return r.done ? { steps: [r.done] } : { wrong: r.wrong || "Midpoint: a point and a line." }
        }
        if (name === "hv") {
            if (arcs.length)
                return { wrong: "Horizontal/Vertical: select lines, or points." }
            if (lines.length && points.length)
                return { wrong: "Horizontal/Vertical: select lines, or points, not both." }
            if (lines.length) {
                const real = lines.filter(p => p.index > 0)
                if (!real.length)
                    return { wrong: "The axes are already level and upright." }
                const level = l => { const k = g.lines.find(q => q.id === l.index), a = at(k.a), b = at(k.b); return Math.abs(b.u - a.u) >= Math.abs(b.v - a.v) }
                const flat = real.filter(level).length, upright = real.length - flat
                const horizontal = flat > upright ? true : upright > flat ? false : level(real[0])
                return { steps: real.map(l => ({ type: horizontal ? "horizontal" : "vertical", first: l.index, second: 0, third: 0, keep: 0 })) }
            }
            if (points.length < 2)
                return { more: "Now the second point." }
            const first = points[0], a = at(first.index)
            const rest = points.slice(1).filter(p => p.index !== first.index)
            const level = p => { const b = at(p.index); return Math.abs(b.u - a.u) >= Math.abs(b.v - a.v) }
            const flat = rest.filter(level).length
            const horizontal = flat * 2 > rest.length ? true : flat * 2 < rest.length ? false : level(rest[0])
            return { steps: rest.map(p => ({ type: horizontal ? "horizontalpoints" : "verticalpoints", first: first.index, second: p.index, third: 0, keep: 0 })) }
        }
        if (name === "coincident") {
            const curves = lines.length + arcs.length
            if (curves > 1)
                return { wrong: "Coincident: points to join, or points to put on one line, arc or circle." }
            if (curves === 1) {
                if (!points.length)
                    return { more: "Now the point." }
                const curve = lines[0] || arcs[0]
                return { steps: points.map(p => ruleFor(name, [p, curve]).done).filter(s => s) }
            }
            if (points.length < 2)
                return { more: "Now a point, or a line to put it on." }
            const base = points.find(p => p.index === -1) || points[0]
            return pairs(base, points.filter(p => p !== base))
        }
        if (name === "symmetry") {
            if (ok.length < 3)
                return { more: ok.length === 1 ? "Now the one that mirrors it." : "Now the line to mirror across." }
            if (ok.length > 3)
                return { wrong: "Symmetry takes two points or two lines, then the line to mirror across." }
            const r = ruleFor(name, ok)
            return r.done ? { steps: [r.done] } : { wrong: r.wrong || "Symmetry pairs two points or two lines." }
        }
        return { wrong: "" }
    }

    // A constraint tool's click (or a selected part handed over). Fix/UnFix: at once.
    function pickWith(part) {
        part = asSketchPart(part, tool === "curvature")
        if (part && part.why)
            return sketch.say(part.why)
        if (!part || !part.kind.startsWith("sketch_"))
            return
        if (tool === "fix" && part.index < 0)
            return sketch.say("The origin and its axes never move: they're fixed already.")
        if (tool === "fix") {
            const g = doc.sketchGeometry(sketch.sketchId)
            const l = part.kind === "sketch_line" ? g.lines.find(q => q.id === part.index) : null
            const k = part.kind === "sketch_arc" ? g.arcs.find(q => q.id === part.index) : null
            doc.fixSketchPoints(sketch.sketchId, l ? [l.a, l.b] : k ? [k.c, k.a, k.b] : [part.index])
            return
        }
        if (picks.some(p => p.kind === part.kind && p.index === part.index)) {
            // Clicking a pick again takes it back.
            picks = picks.filter(p => !(p.kind === part.kind && p.index === part.index))
            selection.selected = lit(picks)
            selection.redraw()
            return
        }
        const r = ruleFor(tool, picks.concat([part]))
        if (r.wrong !== undefined)
            return sketch.say(r.wrong)
        if (r.more) {
            picks = picks.concat([part])
            selection.selected = lit(picks)
            selection.redraw()
            sketch.say(r.more)
            return
        }
        picks = []
        selection.clear()
        applied++
        constraintWanted(r.done.type, r.done.first, r.done.second, r.done.third, r.done.keep)
    }

    function click(x, y) {
        if (tool === "shape")
            return shapeClick(x, y)
        if (tool === "project")
            return projectAt(x, y)
        if (tool === "dimension")
            return measureWith(selection.partAt(x, y), onPlane(x, y))
        if (picking)
            return pickWith(selection.partAt(x, y))
        const at = aim(snapAt(x, y))
        if (!at)
            return
        if (!start) {
            start = at
            chainFirst = at.point || 0
            return
        }
        // A point already drawn by its id; a new one where it goes, with what it snapped onto.
        const end = p => p.point ? p.point : { u: p.u, v: p.v, on: p.on || "", line: p.line || 0, arc: p.arc || 0 }
        if (tool === "line") {
            const made = doc.addSketchLine(sketch.sketchId, end(start), end(at), at.hold || "", construction)
            if (!made.line)
                return
            if (!chainFirst)
                chainFirst = made.from
            if (made.to === chainFirst) {           // closed the shape: start a new chain
                start = null
                chainFirst = 0
            } else {
                start = { u: at.u, v: at.v, point: made.to }
            }
        } else if (tool === "rectangle") {
            const made = doc.addSketchRectangle(sketch.sketchId, end(start), end(at), construction)
            if (made.lines)
                start = null
            else
                sketch.say("Pull the corners apart both ways.")
        } else if (tool === "circle") {
            const r = Math.hypot(at.u - start.u, at.v - start.v)
            if (r < 1e-6)
                return sketch.say("Click away from the centre: that sets the size.")
            const made = doc.addSketchShape(sketch.sketchId, {
                points: [end(start), { u: start.u + r, v: start.v }], arcs: [[0, 1, 1]],
                summary: "Drew a Center Diameter Circle", construction: construction, tool: "circlec" })
            if (made.arcs)
                start = null
        } else if (tool === "arc") {
            if (!arcStart) {
                if (Math.hypot(at.u - start.u, at.v - start.v) < 1e-6)
                    return sketch.say("Click away from the centre: that's where the arc starts.")
                arcStart = at
                sweep = 0
                sweepAngle = Math.atan2(at.v - start.v, at.u - start.u)
                return
            }
            if (Math.abs(sweep) < 1e-3)
                return sketch.say("Move round the centre to where the arc should end.")
            const far = arcEnd()
            // Counter-clockwise from a to b: swapped when the mouse went clockwise.
            const ends = sweep > 0 ? [1, 2] : [2, 1]
            const made = doc.addSketchShape(sketch.sketchId, {
                points: [end(start), end(arcStart), far], arcs: [[0, ends[0], ends[1]]],
                summary: "Drew a Center Point Arc", construction: construction, tool: "arcc" })
            if (made.arcs) {
                start = null
                arcStart = null
            }
        }
    }

    // The preview while a tool waits for its next click, as segment ends (engine coordinates).
    readonly property var preview: {
        if (!tool || picking || !sketch.plane)
            return []
        const p = sketch.plane
        const on = (u, v) => p.origin.plus(p.xAxis.times(u)).plus(p.yAxis.times(v))
        if (tool === "shape") {
            const out = []
            for (const path of shapePreview())
                for (let i = 0; i + 1 < path.length; i++)
                    out.push(on(path[i].u, path[i].v), on(path[i + 1].u, path[i + 1].v))
            return out
        }
        if (!start || !cursor)
            return []
        if (tool === "line")
            return [on(start.u, start.v), on(cursor.u, cursor.v)]
        // Round from angle `from` by `turn` (radians) at radius r about the start, as segment ends.
        const round = (r, from, turn) => {
            const out = [], steps = Math.max(8, Math.ceil(Math.abs(turn) / (2 * Math.PI) * 96))
            for (let i = 0; i < steps; i++) {
                const t0 = from + turn * i / steps, t1 = from + turn * (i + 1) / steps
                out.push(on(start.u + r * Math.cos(t0), start.v + r * Math.sin(t0)),
                         on(start.u + r * Math.cos(t1), start.v + r * Math.sin(t1)))
            }
            return out
        }
        if (tool === "circle")
            return round(Math.hypot(cursor.u - start.u, cursor.v - start.v), 0, 2 * Math.PI)
        if (tool === "arc") {
            if (!arcStart)
                return [on(start.u, start.v), on(cursor.u, cursor.v)]
            const r = Math.hypot(arcStart.u - start.u, arcStart.v - start.v)
            const from = Math.atan2(arcStart.v - start.v, arcStart.u - start.u)
            return [on(start.u, start.v), on(arcStart.u, arcStart.v)].concat(round(r, from, sweep))
        }
        const a = on(start.u, start.v), b = on(cursor.u, start.v), c = on(cursor.u, cursor.v), d = on(start.u, cursor.v)
        return [a, b, b, c, c, d, d, a]
    }

    // --- ShapeTools.js's tools (tool "shape") ------------------------------------------------
    // Starts one. What's selected counts: things to Mirror, Offset, pattern…, or corners to
    // Fillet / Chamfer (all of them at once, as in Fusion).
    function useShape(key) {
        const def = Shapes.tools[key]
        const chosen = selection.selected.filter(p => p.kind.startsWith("sketch_"))
        tool = "shape"
        shape = key
        clicks = []
        gather = []
        hoverPart = null
        cursorEnd = null
        cursor = null
        start = null
        if (!optionValues[key]) {
            const values = Object.assign({}, optionValues)
            values[key] = {}
            for (const o of def.options || [])
                values[key][o.key] = o.value
            optionValues = values
        }
        // A Text tool always starts with an empty box, never the words of the last one (owner, t83u).
        if (key === "text" || key === "textpath") {
            const values = Object.assign({}, optionValues)
            values[key] = Object.assign({}, values[key], { text: "" })
            optionValues = values
        }
        const first = def.steps[0]
        if (first.kind === "objects") {
            // A written text is never a path for the next text: starting a Text tool puts it down.
            const textTool = key === "text" || key === "textpath"
            const geo = textTool ? doc.sketchGeometry(sketch.sketchId) : null
            gather = objectsFrom(textTool ? chosen.filter(p => !(p.kind === "sketch_curve"
                            && geo.curves.some(c => c.id === p.index && c.type === "text"))) : chosen, first.chain)
            selection.selected = gather
            selection.redraw()
            sketch.say(gather.length ? gather.length + (gather.length === 1 ? " thing" : " things")
                                       + " picked: click more, or press Enter" : first.say)
            return
        }
        selection.clear()
        if (first.kind === "corner") {
            const corners = cornersFrom(chosen)
            if (corners.length) {
                clicks = [corners]
                return finishShape()
            }
        }
        sketch.say(first.say)
    }
    // One option's new value (ToolOptions): the preview follows.
    function setOption(key, value) {
        const values = Object.assign({}, optionValues)
        values[shape] = Object.assign({}, values[shape])
        values[shape][key] = value
        // Typing a Height means that size, not the box's (Text).
        if (shape === "text" && key === "height")
            values[shape].fit = false
        optionValues = values
    }

    // What the table's functions see (ShapeTools.js, top).
    function shapeState() {
        const g = doc.sketchGeometry(sketch.sketchId)
        const pts = {}, lines = {}, arcs = {}, curves = {}
        for (const q of g.points) pts[q.id] = q
        pts[-1] = g.origin
        for (const l of g.lines) lines[l.id] = l
        for (const k of g.arcs) arcs[k.id] = k
        for (const k of g.curves) curves[k.id] = k
        return {
            clicks: clicks, gather: gather, cursor: cursor, hover: hoverPart, cursorEnd: cursorEnd,
            opt: optionValues[shape] || {}, g: g, sweep: sweep,
            pt: id => M.P(pts[id].u, pts[id].v), line: id => lines[id], arc: id => arcs[id], curve: id => curves[id],
            textOutline: (text, height) => doc.sketchTextOutline(text, height),
            textOnPath: (text, font, flags, height, codes) => doc.sketchTextOnPath(sketch.sketchId, text, font, flags, height, codes),
            textInBox: (text, font, flags, height, w, h) => doc.sketchTextInBox(text, font, flags, height, w, h)
        }
    }
    // The preview as polylines ({u, v} lists), with Offset's arrow.
    function shapePreview() {
        drawnTick; clicks; gather; cursor; hoverPart; cursorEnd; optionValues; sweep; mmPerPixel
        if (!shapeDef || !sketch.active)
            return []
        // Nothing clicked yet: nothing to show, except the words Text will write.
        if (!clicks.length && !gather.length && step.kind === "point" && shape !== "text")
            return []
        try {
            const s = shapeState()
            const out = shapeDef.preview(s) || []
            const h = shapeDef.handle && shapeDef.handle(s)
            if (h) {
                const d = s.opt[h.key], sign = d < 0 ? -1 : 1, size = 36 * mmPerPixel
                const dir = M.mul(h.dir, sign), tail = M.add(h.base, M.mul(h.dir, d)), tip = M.add(tail, M.mul(dir, size))
                const back = M.sub(tip, M.mul(dir, size * 0.35)), side = M.mul(M.left(dir), size * 0.22)
                out.push([h.base, tail], [tail, tip], [M.add(back, side), tip, M.sub(back, side)])
            }
            return out.filter(path => path && path.length > 1)
        } catch (e) {
            console.warn("Preview of", shape, ":", e, (e.stack || "").split("\n")[0])
            return []
        }
    }

    function shapeMove(x, y) {
        mouse = Qt.point(x, y)
        const plain = onPlane(x, y)
        if (plain)
            mmPerPixel = selection.perPixel() * plain.far
        if (pointStep) {
            cursor = snapAt(x, y)
            showSnap(cursor)
            // Center Point Arc Slot: which way round, as the mouse went (like Center Point Arc).
            if (shape === "slotca" && clicks.length === 2 && cursor) {
                const c = clicks[0], angle = Math.atan2(cursor.v - c.v, cursor.u - c.u)
                let turn = angle - sweepAngle
                while (turn > Math.PI) turn -= 2 * Math.PI
                while (turn <= -Math.PI) turn += 2 * Math.PI
                sweep += turn
                sweepAngle = angle
            }
            return
        }
        cursor = plain ? { u: plain.u, v: plain.v } : null
        if (step.kind === "end") {
            cursorEnd = endAt(x, y)
            return showSnap(cursorEnd ? pointSnap({ id: cursorEnd.point, u: cursorEnd.u, v: cursorEnd.v }) : null)
        }
        if (step.kind === "corner") {
            const c = cornerAt(x, y)
            gather = c ? [c] : []
            return showSnap(c ? pointSnap({ id: c.point, u: c.u, v: c.v }) : null)
        }
        const part = selection.partAt(x, y)
        hoverPart = part && part.kind.startsWith("sketch_") ? part : null
        selection.hoverAt(x, y)
    }

    function shapeClick(x, y) {
        const st = step
        const at = onPlane(x, y)
        if (st.kind === "point") {
            const p = snapAt(x, y)
            if (!p)
                return
            if (shape === "slotca" && clicks.length === 1) {
                sweep = 0
                sweepAngle = Math.atan2(p.v - clicks[0].v, p.u - clicks[0].u)
            }
            if (shape === "slotca" && clicks.length === 2) {
                if (Math.abs(sweep) < 1e-3)
                    return sketch.say("Move round the centre to where the slot should end.")
                p.sweep = sweep
            }
            return nextStep(p)
        }
        if (st.kind === "points") {
            const p = snapAt(x, y)
            if (!p)
                return
            const last = gather.length ? gather[gather.length - 1] : null
            if (last && ((p.point && p.point === last.point) || Math.hypot(p.u - last.u, p.v - last.v) < 1e-6))
                return confirm()
            gather = gather.concat([p])
            sketch.say(gather.length + (gather.length === 1 ? " point" : " points") + ": click the next, or Enter to finish")
            return
        }
        if (st.kind === "pick") {
            let part = selection.partAt(x, y)
            if (part && part.kind === "origin")
                part = asSketchPart(part)
            // A line's or arc's end point: the line or arc (Trim, Extend… near an end).
            if (part && part.kind === "sketch_point" && !st.accept.includes("sketch_point"))
                part = ownerOf(part.index, at, st.accept)
            if (!part || part.why || !st.accept.includes(part.kind) || (part.index < 0 && !st.axes))
                return sketch.say(part && part.why ? part.why : st.say)
            selection.selected = lit(clicks.filter(c => c.part).map(c => c.part).concat([part]))
            selection.redraw()
            return nextStep({ part: part, at: at })
        }
        if (st.kind === "end") {
            const e = endAt(x, y)
            return e ? nextStep(e) : sketch.say(st.say)
        }
        if (st.kind === "corner") {
            const c = cornerAt(x, y)
            return c ? nextStep([c]) : sketch.say("Click a corner where two lines meet.")
        }
        // objects: a click adds or takes away (Offset: the whole joined shape at once).
        const part = selection.partAt(x, y)
        // A written text can't be a path to follow: clicking one (Text tools) picks THAT text, so its
        // edit panel stays (owner, t79u: the options went dead once a text was clicked with the tool on).
        if (part && part.kind === "sketch_curve" && (shape === "text" || shape === "textpath")) {
            const tc = doc.sketchGeometry(sketch.sketchId).curves.find(c => c.id === part.index)
            if (tc && tc.type === "text") {
                if (!gather.length) {
                    selection.selected = [part]
                    selection.redraw()
                    sketch.say("Text picked: change it above, or drag it along its path")
                }
                return
            }
        }
        const got = part ? objectsFrom([part], st.chain) : []
        // Clicking away from a text that is picked (Text tools, nothing being picked for a new one) puts it down.
        if (!got.length && (shape === "text" || shape === "textpath") && !gather.length && selection.selected.length === 1
                && selection.selected[0].kind === "sketch_curve") {
            selection.selected = []
            selection.redraw()
            return
        }
        if (!got.length)
            return
        const has = q => gather.some(p => p.kind === q.kind && p.index === q.index)
        gather = has(got[0]) ? gather.filter(p => !got.some(q => p.kind === q.kind && p.index === q.index))
                             : gather.concat(got.filter(q => !has(q)))
        selection.selected = gather
        selection.redraw()
        sketch.say(gather.length ? gather.length + (gather.length === 1 ? " thing" : " things") + " picked: click more, or press Enter"
                                 : st.say)
    }
    // The line or arc (of the kinds accepted) a point is an end of, nearest the mouse.
    function ownerOf(pointId, at, accept) {
        const g = doc.sketchGeometry(sketch.sketchId)
        const pt = id => g.points.find(q => q.id === id)
        let best = null
        const consider = (kind, index, mid) => {
            const gap = at ? Math.hypot(mid.u - at.u, mid.v - at.v) : 0
            if (accept.includes(kind) && (!best || gap < best.gap))
                best = { gap: gap, part: { kind: kind, sketch: sketch.sketchId, index: index } }
        }
        for (const l of g.lines)
            if (l.a === pointId || l.b === pointId)
                consider("sketch_line", l.id, M.mid(pt(l.a), pt(l.b)))
        for (const k of g.arcs)
            if (!k.circle && (k.a === pointId || k.b === pointId))
                consider("sketch_arc", k.id, M.mid(pt(k.a), pt(k.b)))
        return best ? best.part : null
    }
    function nextStep(done) {
        clicks = clicks.concat([done])
        if (clicks.length >= shapeDef.steps.length)
            return finishShape()
        sketch.say(shapeDef.steps[clicks.length].say)
    }
    // Enter: a gathering step is done (or a tool that's only options and a pick, made).
    function confirm() {
        if (tool !== "shape")
            return false
        const st = step
        if (st.kind === "objects" || st.kind === "points") {
            if (gather.length < (st.min || 1))
                return sketch.say(st.say), true
            const list = gather
            gather = []
            nextStep(list)
            return true
        }
        return false
    }
    function finishShape() {
        let made
        const done = clicks
        try {
            const s = shapeState()
            clicks = []                       // the preview lets go of what's about to change
            gather = []
            made = shapeDef.build(s)
        } catch (e) {
            console.warn("Building", shape, ":", e, (e.stack || "").split("\n")[0])
            made = { why: "That didn't work here." }
        }
        if (!made.why) {
            const spec = { points: made.points, lines: made.lines, arcs: made.arcs, curves: made.curves,
                           constraints: made.constraints, rewire: made.rewire, move: made.move, remove: made.remove,
                           helpers: made.helpers, values: made.values, steady: made.steady, summary: made.summary,
                           construction: construction, like: made.like, tool: shape }
            const result = doc.addSketchShape(sketch.sketchId, spec)
            made = result.why ? result : made
            if (!result.why && !result.lines)
                made = { why: "That didn't work here." }
        }
        if (made.why) {
            // Back one step, to try that last one again.
            const last = done[done.length - 1]
            clicks = done.slice(0, -1)
            if (Array.isArray(last) && shapeDef.steps[clicks.length].kind !== "corner")
                gather = last
            if (!clicks.length && shapeDef.steps[0].kind === "corner")
                gather = []
            return sketch.say(made.why)
        }
        clicks = []
        gather = []
        selection.clear()
        // A text is made once and the tool ends, as Project does on Done (owner, t82u, t85u): finished means
        // finished, nothing left picked (white text, dashed box); double-click it later to edit it.
        if (shape === "text" || shape === "textpath") {
            cancel()
            selection.selected = []
            selection.redraw()
            return sketch.say(made.summary + " (Ctrl+Z takes it back). Double-click the text to change it")
        }
        sketch.say(made.summary + " (Ctrl+Z takes it back). " + shapeDef.steps[0].say)
    }

    // Parts as a gathering step takes them: lines (not the axes), arcs, curves and points
    // (not the Origin); chain: whole joined shapes.
    function objectsFrom(parts, chain) {
        const out = []
        const add = p => { if (!out.some(q => q.kind === p.kind && q.index === p.index)) out.push(p) }
        for (const p of parts) {
            if (!((p.kind === "sketch_line" || p.kind === "sketch_point") && p.index > 0)
                    && p.kind !== "sketch_arc" && p.kind !== "sketch_curve")
                continue
            if (chain && (p.kind === "sketch_line" || p.kind === "sketch_arc")) {
                add(p)                       // the one clicked leads (Text on Path starts there)
                chainOf(p).forEach(add)
            }
            else if (!chain || p.kind !== "sketch_point")
                add(p)
        }
        return out
    }
    // Every line and arc joined to this one end to end (a circle stands alone).
    function chainOf(part) {
        const g = doc.sketchGeometry(sketch.sketchId)
        const items = g.lines.map(l => ({ kind: "sketch_line", index: l.id, ends: [l.a, l.b] }))
                     .concat(g.arcs.map(k => ({ kind: "sketch_arc", index: k.id, ends: k.circle ? [] : [k.a, k.b] })))
        const key = p => p.kind + p.index
        const seen = new Set([key(part)])
        const todo = [items.find(p => key(p) === key(part))]
        while (todo.length) {
            const at = todo.pop()
            if (!at)
                break
            for (const other of items)
                if (!seen.has(key(other)) && other.ends.some(e => at.ends.includes(e))) {
                    seen.add(key(other))
                    todo.push(other)
                }
        }
        return items.filter(p => seen.has(key(p))).map(p => ({ kind: p.kind, sketch: sketch.sketchId, index: p.index }))
    }
    // The end of a line or an arc under the mouse: {point, u, v, dir (outward), kind, id}.
    function endAt(x, y, forced) {
        const part = forced || selection.partAt(x, y), at = onPlane(x, y)
        if (!part || !at)
            return null
        const g = doc.sketchGeometry(sketch.sketchId)
        const pt = id => g.points.find(q => q.id === id)
        let ends = null
        if (part.kind === "sketch_line" && part.index > 0) {
            const l = g.lines.find(q => q.id === part.index), a = pt(l.a), b = pt(l.b)
            ends = [{ id: l.a, p: a, dir: M.norm(M.sub(a, b)) }, { id: l.b, p: b, dir: M.norm(M.sub(b, a)) }]
        } else if (part.kind === "sketch_arc") {
            const k = g.arcs.find(q => q.id === part.index)
            if (!k || k.circle)
                return null
            const c = pt(k.c), a = pt(k.a), b = pt(k.b)
            ends = [{ id: k.a, p: a, dir: M.mul(M.left(M.norm(M.sub(a, c))), -1) },
                    { id: k.b, p: b, dir: M.left(M.norm(M.sub(b, c))) }]
        } else if (part.kind === "sketch_point") {
            // A point that's the end of exactly one line or arc.
            const found = g.lines.filter(l => l.a === part.index || l.b === part.index)
            const arcs = g.arcs.filter(k => !k.circle && (k.a === part.index || k.b === part.index))
            if (found.length + arcs.length !== 1)
                return null
            return endAt(x, y, found.length ? { kind: "sketch_line", index: found[0].id }
                                                       : { kind: "sketch_arc", index: arcs[0].id })
        }
        if (!ends)
            return null
        const e = M.dist(at, ends[0].p) <= M.dist(at, ends[1].p) ? ends[0] : ends[1]
        return { point: e.id, u: e.p.u, v: e.p.v, dir: e.dir, kind: part.kind === "sketch_line" ? "line" : "arc", id: part.index }
    }
    // The corner nearest the mouse: a point where exactly two lines meet (no arcs).
    function cornerAt(x, y) {
        const at = onPlane(x, y)
        if (!at)
            return null
        const reach = snapPixels * 1.5 * selection.perPixel() * at.far
        let best = null
        for (const c of corners()) {
            const gap = Math.hypot(c.u - at.u, c.v - at.v)
            if (gap <= reach && (!best || gap < best.gap))
                best = Object.assign({ gap: gap, near: { u: at.u, v: at.v } }, c)
        }
        return best
    }
    function corners() {
        const g = doc.sketchGeometry(sketch.sketchId)
        const out = []
        for (const q of g.points) {
            if (q.hidden)
                continue
            const ls = g.lines.filter(l => l.a === q.id || l.b === q.id)
            if (ls.length === 2 && !g.arcs.some(k => k.c === q.id || k.a === q.id || k.b === q.id))
                out.push({ point: q.id, l1: ls[0].id, l2: ls[1].id, u: q.u, v: q.v })
        }
        return out
    }
    // Corners in a selection: selected corner points, and corners between two selected lines.
    function cornersFrom(parts) {
        const points = new Set(parts.filter(p => p.kind === "sketch_point").map(p => p.index))
        const lines = new Set(parts.filter(p => p.kind === "sketch_line").map(p => p.index))
        return corners().filter(c => points.has(c.point) || (lines.has(c.l1) && lines.has(c.l2)))
    }

    // Offset's arrow: dragged out or in, it sets the distance.
    property var handleGrab: null          // {base, dir, key, from (the distance), at (where it started)}
    function handleBegin(x, y) {
        if (!shapeDef.handle)
            return false
        const h = shapeDef.handle(shapeState()), at = onPlane(x, y)
        if (!h || !at)
            return false
        const d = optionValues[shape][h.key], size = 36 * mmPerPixel
        const from = M.add(h.base, M.mul(h.dir, Math.min(d, d + (d < 0 ? -size : size)))), to = M.add(h.base, M.mul(h.dir, Math.max(d, d + (d < 0 ? -size : size))))
        const along = Math.max(0, Math.min(1, M.dot(M.sub(at, from), M.sub(to, from)) / Math.max(1e-12, M.dot(M.sub(to, from), M.sub(to, from)))))
        if (M.dist(at, M.lerp(from, to, along)) > snapPixels * 1.5 * selection.perPixel() * at.far)
            return false
        handleGrab = { dir: h.dir, key: h.key, from: d, at: { u: at.u, v: at.v } }
        return true
    }
    function handleMove(x, y) {
        const at = onPlane(x, y)
        if (!at)
            return
        mouse = Qt.point(x, y)
        setOption(handleGrab.key, handleGrab.from + M.dot(M.sub(at, handleGrab.at), handleGrab.dir))
    }

    // Dragging (Navigation.leftDrag, through Main).
    property var grab: null                // {u, v} where the drag started on the plane
    property int slide: 0                  // a text on a path being slid along it: its curve id
    property var dragStarts: ({})          // point id → {u, v} at the start
    property var dragRim: null             // resizing a circle: {u, v} (its centre then), part
    property string dragSummary: ""
    // Drags snap like drawing (owner, 2026-10-30): the dragged point, or for a line or a
    // selection whichever of its points is nearest something, snaps onto a point (a rectangle's
    // corner too), the Origin, a line's middle, or along a line or axis; dropped there it's held
    // by Coincident / Midpoint, in the same step as the move.
    property var dragSkip: null            // {points, lines}: what moves with the drag, never a target
    property var dragSnap: null            // the snap now: snapNear's answer + dragged (the point id)

    // A box's way of working (owner, 2026-11-02, as Fusion's drag direction decides WHAT it takes):
    // a plain drag replaces the selection with it, Shift adds all of it, Ctrl takes whatever of it
    // is selected out again (it never selects anything new).
    function boxMode(modifiers) {
        return modifiers & Qt.ControlModifier ? "remove" : modifiers & Qt.ShiftModifier ? "add" : "replace"
    }
    // Is a point on (or within a third of the letter height of) the letters of a laid preview
    // (the first polyline is the frame, left out)?
    function previewHit(laid, at, height) {
        const band = Math.max(0.3 * height, 1e-3), b2 = band * band
        for (let i = 1; i < laid.length; i++) {
            const sh = laid[i]
            let inside = false
            for (let j = 0, n = sh.length; j < n; j++) {
                const p = sh[j], q = sh[(j + 1) % n]
                if ((p.v > at.v) !== (q.v > at.v) && at.u < (q.u - p.u) * (at.v - p.v) / (q.v - p.v) + p.u)
                    inside = !inside
                const dx = q.u - p.u, dy = q.v - p.v, len2 = dx * dx + dy * dy
                const t = len2 > 0 ? Math.max(0, Math.min(1, ((at.u - p.u) * dx + (at.v - p.v) * dy) / len2)) : 0
                const ex = p.u + t * dx - at.u, ey = p.v + t * dy - at.v
                if (ex * ex + ey * ey <= b2)
                    return true
            }
            if (inside)
                return true
        }
        return false
    }
    function dragBegin(x, y, modifiers) {
        if (tool === "shape") {
            // The words shown while the lines are still picked (before Enter) can be grabbed too:
            // the first drag writes them, then slides them along the path (owner, t71u/t76u: he
            // types the words, picks the shape and drags, never pressing Enter).
            if (sketch.active && shape === "textpath" && (gather.length || clicks.length) && String(optionValues.textpath && optionValues.textpath.text || "").trim()
                    && !(modifiers & (Qt.ControlModifier | Qt.ShiftModifier))) {
                const at = onPlane(x, y)
                const laid = at ? shapePreview() : []
                if (at && laid.length > 1 && previewHit(laid, at, optionValues.textpath.height || 6.35)) {
                    const before = doc.sketchGeometry(sketch.sketchId).curves.length
                    confirm()
                    const cs = doc.sketchGeometry(sketch.sketchId).curves
                    const made = cs.length > before ? cs[cs.length - 1] : null
                    if (made && made.type === "text" && made.path.length && doc.beginSketchTextSlide(sketch.sketchId, made.id, at.u, at.v)) {
                        slide = made.id
                        selection.selected = [{ kind: "sketch_curve", sketch: sketch.sketchId, index: made.id }]
                        selection.redraw()
                        return true
                    }
                }
            }
            // Text on Path stays on after a text is written: a drag that starts on a path text
            // still slides it along its path (owner, t61u) when nothing is picked yet.
            if (sketch.active && shape === "textpath" && !(modifiers & (Qt.ControlModifier | Qt.ShiftModifier))) {
                const hit = selection.partAt(x, y), at = hit ? onPlane(x, y) : null
                if (hit && at && hit.kind === "sketch_curve"
                        && doc.sketchGeometry(sketch.sketchId).curves.some(k => k.id === hit.index && k.type === "text" && k.path.length)
                        && doc.beginSketchTextSlide(sketch.sketchId, hit.index, at.u, at.v)) {
                    slide = hit.index
                    selection.selected = [hit]
                    selection.redraw()
                    return true
                }
            }
            return handleBegin(x, y)
        }
        if (tool === "project" && sketch.active) {
            box = { x: x, y: y, x2: x, y2: y, mode: boxMode(modifiers) }
            return true
        }
        if (!sketch.active && !tool && !sketch.picking) {
            box = { x: x, y: y, x2: x, y2: y, mode: boxMode(modifiers) }
            return true
        }
        if (!sketch.active || tool)
            return false
        const part = selection.partAt(x, y)
        if (!part || !part.kind.startsWith("sketch_") || (modifiers & (Qt.ControlModifier | Qt.ShiftModifier))) {
            if (!selection.sketchId)
                return false
            box = { x: x, y: y, x2: x, y2: y, mode: boxMode(modifiers) }
            return true
        }
        const at = onPlane(x, y)
        if (!at)
            return false
        const same = p => p.kind === part.kind && p.index === part.index
        let parts = selection.selected.filter(p => p.kind.startsWith("sketch_"))
        if (!parts.some(same)) {
            parts = [part]
            selection.selected = parts
            selection.redraw()
        }
        const g = doc.sketchGeometry(sketch.sketchId)
        // A circle on its own, grabbed by its edge, changes size: its centre stays put. When its
        // size can't change (a dimension, Equal to a dimensioned one, anything), the edge carries
        // the whole circle instead (as in Fusion): grabbing anything always does something, so a
        // drag shows what's still free (owner, 2026-10-30: a tangent circle goes round the other).
        const sized = id => g.constraints.some(c => (c.type === "radius" || c.type === "diameter") && c.first === id)
        const ring = parts.length === 1 && part.kind === "sketch_arc" && !sized(part.index)
                   ? g.arcs.find(a => a.id === part.index && a.circle) : null
        if (ring) {
            const c = g.points.find(q => q.id === ring.c)
            dragRim = { u: c.u, v: c.v, part: part }
            dragSummary = "Resized Circle" + ring.id
            grab = at
            doc.beginSketchDrag(sketch.sketchId)
            return true
        }
        // A text on a path slides along it (the justify then sits about where it is let go).
        // Whatever else is picked along with it (the lines it follows, say) is let go: pressing a
        // path text's letters always slides it (owner, t66u).
        const slidable = part.kind === "sketch_curve" && !(modifiers & (Qt.ControlModifier | Qt.ShiftModifier))
                       && g.curves.some(k => k.id === part.index && k.type === "text" && k.path.length)
        if (slidable && doc.beginSketchTextSlide(sketch.sketchId, part.index, at.u, at.v)) {
            slide = part.index
            selection.selected = [part]
            selection.redraw()
            return true
        }
        if (!moveFrom(parts, part, g))
            return false
        grab = at
        doc.beginSketchDrag(sketch.sketchId)
        return true
    }
    // A drag that moves things (not a resize): where each moving point starts, what it mustn't
    // snap onto, and the history's words. false when there's nothing to move.
    function moveFrom(parts, part, g) {
        const starts = {}
        for (const p of parts) {
            const ends = p.kind === "sketch_point" ? [p.index]
                       : p.kind === "sketch_arc" ? (a => a ? [a.c, a.a, a.b] : [])(g.arcs.find(a => a.id === p.index))
                       : p.kind === "sketch_curve" ? (k => k && !k.path.length ? k.points : [])(g.curves.find(k => k.id === p.index))
                       : (l => l ? [l.a, l.b] : [])(g.lines.find(l => l.id === p.index))
            for (const id of ends) {
                const q = g.points.find(q => q.id === id)
                if (q)
                    starts[id] = { u: q.u, v: q.v }
            }
        }
        // A circle's centre carries the circle: it moves at its size (as in Fusion).
        for (const a of g.arcs)
            if (a.circle && starts[a.c] && !starts[a.a]) {
                const q = g.points.find(q => q.id === a.a)
                if (q)
                    starts[a.a] = { u: q.u, v: q.v }
            }
        if (!Object.keys(starts).length)
            return false
        if (Object.keys(starts).every(id => g.points.find(q => q.id === parseInt(id)).fixed))
            sketch.say("Fixed where it is. Fix/UnFix lets it go.")
        const lines = parts.filter(p => p.kind === "sketch_line")
        dragSummary = parts.length === 1 ? "Moved " + ({ sketch_point: "Point", sketch_line: "Line", sketch_arc: "Arc", sketch_curve: "Curve" })[part.kind] + part.index
                    : lines.length === parts.length ? "Moved " + lines.length + " lines"
                    : "Moved " + parts.length + " items"
        dragStarts = starts
        const moving = new Set(Object.keys(starts).map(id => parseInt(id)))
        const skip = { points: new Set(moving), lines: new Set() }
        for (const l of g.lines)
            if (moving.has(l.a) || moving.has(l.b)) {
                skip.lines.add(l.id)
                skip.points.add(l.a)          // a line's other end: snapping onto it would squash the line
                skip.points.add(l.b)
            }
        for (const a of g.arcs)               // an arc's own points keep their shape
            if (moving.has(a.c) || moving.has(a.a) || moving.has(a.b))
                [a.c, a.a, a.b].forEach(id => skip.points.add(id))
        for (const c of g.constraints)        // points already held on a dragged one move with it
            if (c.type === "coincident" && (moving.has(c.first) || moving.has(c.second))) {
                skip.points.add(c.first)
                skip.points.add(c.second)
            }
        dragSkip = skip
        dragSnap = null
        return true
    }
    function dragMove(x, y) {
        if (box) {
            box = { x: box.x, y: box.y, x2: x, y2: y, mode: box.mode }
            return
        }
        if (handleGrab)
            return handleMove(x, y)
        if (slide) {
            const on = onPlane(x, y)
            if (on)
                doc.slideSketchText(sketch.sketchId, slide, on.u, on.v)
            return
        }
        if (!grab)
            return
        const at = onPlane(x, y)
        if (!at)
            return
        if (dragRim) {
            const r = Math.max(1e-3, Math.hypot(at.u - dragRim.u, at.v - dragRim.v))
            // Size first: the size the mouse asks for (it may shift to keep its constraints, as
            // a tangent circle does), unless its size is held.
            if (doc.dragSketchSize(sketch.sketchId, dragRim.part.index, r)) {
                mouse = Qt.point(x, y)
                return
            }
            // Its size is held: from here on the edge carries the circle.
            const part = dragRim.part
            dragRim = null
            if (!moveFrom([part], part, doc.sketchGeometry(sketch.sketchId)))
                return
        }
        let du = at.u - grab.u, dv = at.v - grab.v
        const g = doc.sketchGeometry(sketch.sketchId)
        const reach = snapPixels * selection.perPixel() * at.far
        let best = null
        for (const id in dragStarts) {
            const there = { u: dragStarts[id].u + du, v: dragStarts[id].v + dv }
            const s = snapNear(there, reach, g, dragSkip)
            if (!s.point && !s.on)
                continue
            const gap = Math.hypot(s.u - there.u, s.v - there.v)
            if (!best || gap < best.gap)
                best = { gap: gap, s: Object.assign({ dragged: parseInt(id) }, s), du: s.u - there.u, dv: s.v - there.v }
        }
        if (best) {
            du += best.du
            dv += best.dv
        }
        const moves = []
        for (const id in dragStarts)
            moves.push({ id: parseInt(id), u: dragStarts[id].u + du, v: dragStarts[id].v + dv })
        const moved = doc.dragSketch(sketch.sketchId, moves)
        // A snap counts only if the dragged point really got there (the constraints may not let it).
        if (best) {
            const q = moved && doc.sketchGeometry(sketch.sketchId).points.find(q => q.id === best.s.dragged)
            if (!q || Math.hypot(q.u - best.s.u, q.v - best.s.v) > 1e-4)
                best = null
        }
        mouse = Qt.point(x, y)
        dragSnap = best ? best.s : null
        showSnap(dragSnap)
    }
    function dragEnd() {
        if (box) {
            if (tool === "project")
                projectBox()
            else if (!sketch.active)
                selectModelBox()
            else
                selectBox()
            return
        }
        if (handleGrab) {
            handleGrab = null
            return
        }
        if (slide) {
            slide = 0
            if (doc.endSketchTextSlide(sketch.sketchId))
                sketch.say("Moved the text along its path (Ctrl+Z takes it back)")
            return
        }
        if (!grab)
            return
        grab = null
        dragRim = null
        dragStarts = {}
        const s = dragSnap
        dragSnap = null
        dragSkip = null
        showSnap(null)
        const hold = !s ? {} : { point: s.dragged, on: s.point ? "point" : s.on, target: s.point ? s.point : s.on === "arc" ? s.arc : (s.line || 0) }
        const made = doc.endSketchDrag(sketch.sketchId, dragSummary, hold)
        if (made)
            sketch.say(dragSummary + ", held by " + made + " (Ctrl+Z takes both back)")
    }

    // Delete: what's selected goes, whatever kind of line it is; areas are only ever selected, so
    // they stay. A selected point takes what's drawn from it along (as in Fusion). Points nothing
    // else uses go with their lines, shared ones stay; constraints on what's gone go too. One step.
    function deleteSelected() {
        const g = doc.sketchGeometry(sketch.sketchId)
        const chosen = (kind, id) => selection.selected.some(p => p.kind === kind && p.index === id)
        const points = new Set(selection.selected.filter(p => p.kind === "sketch_point").map(p => p.index))
        const on = ids => ids.some(p => points.has(p))
        const lines = g.lines.filter(l => chosen("sketch_line", l.id) || on([l.a, l.b]))
        const arcs = g.arcs.filter(a => chosen("sketch_arc", a.id) || on([a.c, a.a, a.b]))
        const curves = g.curves.filter(c => chosen("sketch_curve", c.id) || on(c.points))
        for (const l of lines) { points.add(l.a); points.add(l.b) }
        for (const a of arcs) { points.add(a.c); points.add(a.a); points.add(a.b) }
        for (const c of curves)
            for (const p of c.points)
                points.add(p)
        const count = lines.length + arcs.length + curves.length
        if (!count && !points.size)
            return
        const one = lines.length ? "Line" + lines[0].id : arcs.length ? "Arc" + arcs[0].id : curves.length ? "Curve" + curves[0].id : "Point"
        const result = doc.addSketchShape(sketch.sketchId, {
            summary: count > 1 ? "Deleted " + count + " pieces" : "Deleted " + one, tool: "delete",
            remove: { points: [...points], lines: lines.map(l => l.id), arcs: arcs.map(a => a.id), curves: curves.map(c => c.id) }
        })
        if (result.why)
            sketch.say(result.why)
        selection.clear()
    }

    // A box drag (Fusion's window select): left to right takes what's wholly inside, right to left
    // anything it touches. The points of what's taken stay out of the selection, so two lines
    // boxed are still two lines to Parallel, Equal and the like; a point on its own is taken.
    // Whether a set of on-screen paths is taken by the dragged box: wholly inside for a window
    // (left to right), any touch for a crossing (right to left).
    function boxHits() {
        const left = Math.min(box.x, box.x2), right = Math.max(box.x, box.x2)
        const top = Math.min(box.y, box.y2), bottom = Math.max(box.y, box.y2)
        const touching = box.x2 < box.x
        const inside = p => p.x >= left && p.x <= right && p.y >= top && p.y <= bottom
        const crosses = (a, b) => {                             // a piece of a path against the box
            let from = 0, to = 1
            const edges = [[a.x - b.x, a.x - left], [b.x - a.x, right - a.x], [a.y - b.y, a.y - top], [b.y - a.y, bottom - a.y]]
            for (const [d, room] of edges) {
                if (d === 0 && room < 0)
                    return false
                if (d < 0)
                    from = Math.max(from, room / d)
                else if (d > 0)
                    to = Math.min(to, room / d)
            }
            return from <= to
        }
        return paths => {
            if (!touching)
                return paths.every(path => path.every(inside))
            return paths.some(path => path.some(inside) || path.some((p, i) => i > 0 && crosses(path[i - 1], p)))
        }
    }
    function selectBox() {
        const hits = boxHits()
        const taken = doc.sketchOutlines(sketch.sketchId, selection.sketchAllow())
                         .filter(part => hits(part.paths.map(path => path.map(p => screenOf(p)))))
        const g = doc.sketchGeometry(sketch.sketchId)
        const owned = new Set()
        const has = (kind, id) => taken.some(p => p.kind === kind && p.index === id)
        for (const l of g.lines)
            if (has("sketch_line", l.id)) { owned.add(l.a); owned.add(l.b) }
        for (const a of g.arcs)
            if (has("sketch_arc", a.id)) { owned.add(a.c); owned.add(a.a); owned.add(a.b) }
        for (const c of g.curves)
            if (has("sketch_curve", c.id))
                for (const p of c.points)
                    owned.add(p)
        const parts = taken.filter(p => p.kind !== "sketch_point" || !owned.has(p.index))
                           .map(p => ({ kind: p.kind, sketch: sketch.sketchId, index: p.index }))
        const all = taken.map(p => ({ kind: p.kind, sketch: sketch.sketchId, index: p.index }))
        applyBox(box.mode, parts, all)
        box = null
        selection.redraw()
    }
    // What a finished box does to the selection: replace it, add `parts`, or take out of it whatever
    // of `all` (everything the box reached, owned points too) is in it. Returns how many changed.
    function applyBox(mode, parts, all) {
        const same = (p, q) => p.kind === q.kind && p.index === q.index
        const was = selection.selected
        if (mode === "remove") {
            selection.selected = was.filter(q => !all.some(p => same(p, q)))
            return was.length - selection.selected.length
        }
        if (mode === "add") {
            const fresh = parts.filter(p => !was.some(q => same(p, q)))
            selection.selected = was.concat(fresh)
            return fresh.length
        }
        selection.selected = parts
        return parts.length
    }

    // The whole chain this part is on, as selection parts: lines, arcs and curves joined end to
    // end through a shared point, a Coincident, or a point held on them (Point On, Midpoint). Two
    // lines that only cross are not joined. Normal and construction each only along their own kind.
    function profileOf(part) {
        const g = doc.sketchGeometry(sketch.sketchId)
        const pieces = [
            ...g.lines.map(l => ({ kind: "sketch_line", id: l.id, construction: l.construction, ends: [l.a, l.b] })),
            ...g.arcs.map(a => ({ kind: "sketch_arc", id: a.id, construction: a.construction, ends: a.circle ? [] : [a.a, a.b] })),
            ...g.curves.map(c => ({ kind: "sketch_curve", id: c.id, construction: c.construction,
                                    ends: c.type === "conic" ? c.points.slice(0, 2)
                                        : c.type.endsWith("spline") ? [c.points[0], c.points[c.points.length - 1]] : [] }))
        ]
        const tied = new Map()
        const point = p => tied.has(p) ? point(tied.get(p)) : p
        for (const c of g.constraints)
            if (c.type === "coincident" && point(c.first) !== point(c.second))
                tied.set(point(c.first), point(c.second))
        for (const c of g.constraints) {
            const kind = { pointonline: "sketch_line", midpoint: "sketch_line", pointonarc: "sketch_arc" }[c.type]
            const held = kind && pieces.find(q => q.kind === kind && q.id === c.second)
            if (held)
                held.ends.push(c.first)
        }
        const at = q => q.ends.map(point)
        const first = part.kind === "sketch_point" ? pieces.find(q => at(q).includes(point(part.index)))
                                                   : pieces.find(q => q.kind === part.kind && q.id === part.index)
        if (!first)
            return []
        const seen = new Set([first])
        const todo = [first]
        while (todo.length) {
            const ends = at(todo.pop())
            for (const other of pieces)
                if (!seen.has(other) && other.construction === first.construction && at(other).some(p => ends.includes(p))) {
                    seen.add(other)
                    todo.push(other)
                }
        }
        return [...seen].map(q => ({ kind: q.kind, sketch: sketch.sketchId, index: q.id }))
    }

    // The points of the selected lines and points (Fix/UnFix works on points).
    function selectedPoints() {
        const g = doc.sketchGeometry(sketch.sketchId)
        const ids = new Set()
        for (const p of selection.selected) {
            if (p.kind === "sketch_point")
                ids.add(p.index)
            else if (p.kind === "sketch_line") {
                const l = g.lines.find(q => q.id === p.index)
                if (l) { ids.add(l.a); ids.add(l.b) }
            } else if (p.kind === "sketch_arc") {
                const a = g.arcs.find(q => q.id === p.index)
                if (a) { ids.add(a.c); ids.add(a.a); ids.add(a.b) }
            }
        }
        return [...ids]
    }
}
