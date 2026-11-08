import QtQuick

// Hovering and selecting parts of the model, Fusion style:
//   hover                   the face, edge or point under the mouse lights up (hover colour)
//   click                   select it (selection colour), replacing the selection
//   Ctrl- or Shift-click    add it to the selection, or take it out again
//   click empty space, Esc  clear the selection
// Points win over edges, edges over faces (Document::pick). A part is {kind, index}.
// The origin's point, axes and planes (OriginShapes) can be picked too, as
// {kind: "origin", name: "O"/"X"/…/"XY", part: "point"/"axis"/"plane"}: its point and
// axes count like the model's points and edges, a plane like a face (nearest wins).
// In a sketch, its own points and lines come before anything else:
// {kind: "sketch_point" | "sketch_line" | "sketch_arc", sketch, index}.
// Closed profiles (owner, 2026-10-30; Fusion's profile picking, for Extrude later):
// {kind: "profile", sketch, index, u, v}, the area under the mouse in a shown sketch (only the
// open one while sketching). Points and edges win over it; it wins over a face or an origin
// plane it lies on or in front of. (u, v) is the spot clicked, so it's found again after
// the sketch changes (profiles are renumbered).
QtObject {
    id: sel

    required property var view
    required property var camera
    required property var nav
    property bool ortho: false               // the view has no perspective (Main.sketchOrtho)
    required property var hoverGeometry
    required property var selectedGeometry
    // Sketch lines and points light up flat (unlit), like the lines themselves.
    required property var sketchHoverGeometry
    required property var sketchSelectedGeometry
    required property var profileHoverGeometry
    required property var profileSelectedGeometry
    property var profileSketches: []         // sketches whose profiles can be picked (Main.drawProfiles)
    required property var origin             // OriginShapes
    property int sketchId: 0                 // the open sketch: its points and lines come first
    property bool pickable: true             // false while the body is hidden
    // The Selection menu (was Selectable) (owner, 2026-11-01): what a plain click or hover may pick while sketching.
    // It applies only when `filtered` is on (Main: sketching with no tool running), so a tool can
    // still pick whatever it needs. A kind that's off is skipped, so what's behind it is found.
    property bool filtered: false
    // The model's own switches (Bodies, and under it Faces, Edges, Points) also apply while the
    // Project tool runs, so it can reach one edge or corner through a face (owner, 2026-11-01).
    property bool bodyFiltered: false
    property var allow: ({ lines: true, points: true, construction: true, projected: true, profiles: true,
                           origin: true, bodies: true, faces: true, edges: true, bodyPoints: true, outside: true })
    // The sketch part kinds that count, as the bits of Document::sketchPartClass.
    function sketchAllow() {
        if (!filtered)
            return 15
        return (allow.lines ? 1 : 0) | (allow.points ? 2 : 0) | (allow.construction ? 4 : 0) | (allow.projected ? 8 : 0)
    }
    // Whether a picked part is still allowed (after a switch went off).
    function allowed(p) {
        if (p.kind === "body")
            return !bodyFiltered || allow.bodies
        if (p.kind === "face" || p.kind === "edge" || p.kind === "point") {
            if (!bodyFiltered)
                return true
            return allow.bodies && (p.kind === "face" ? allow.faces : p.kind === "edge" ? allow.edges : allow.bodyPoints)
        }
        if (!filtered)
            return true
        if (p.kind.startsWith("sketch_"))
            return (doc.sketchPartClass(p.sketch, p.kind, p.index) & sketchAllow()) !== 0
        switch (p.kind) {
        case "profile": return allow.profiles
        case "origin": return allow.origin
        }
        return true
    }
    function refilter() {
        selected = selected.filter(allowed)
        if (hovered && !allowed(hovered))
            hovered = null
        redraw()
    }
    onFilteredChanged: refilter()
    onBodyFilteredChanged: refilter()
    onAllowChanged: refilter()

    // Select Through (owner, 2026-11-02): edges and corners behind the model can be picked and hovered.
    property bool through: false
    onThroughChanged: { clearHover(); }
    // What the Project tool has picked so far, shown while it runs (owner, 2026-11-02): edges and
    // corners as thick lines in front, faces shaded translucent purple. Main binds `projectPicks`.
    property var projectPicks: []
    property var projectEdgeGeometry: null
    property var projectFaceGeometry: null
    onProjectPicksChanged: projectRedraw(true)

    property var hovered: null
    property var selected: []

    // Select the whole body (double-click on the model, owner 2026-11-02): only that body, never the
    // component around it. `ids` are body numbers; those not drawn yet still count as picked.
    function selectBodies(ids) {
        selected = ids.map(id => ({ kind: "body", index: id, name: "Body" }))
        hovered = null
        redraw()
    }
    function selectBodyAt(x, y) {
        const part = partAt(x, y)
        if (!part || (part.kind !== "face" && part.kind !== "edge" && part.kind !== "point" && part.kind !== "body"))
            return false
        const body = part.kind === "body" ? part.index : (part.body || doc.currentBody())
        if (!body)
            return false
        selectBodies([body])
        return true
    }
    // Which Browser rows to underline with dots: each selected thing's own row (body or sketch) and
    // the components above it, not the design itself.
    readonly property var treeKeys: {
        const out = ({})
        const add = key => { for (const k of doc.lineageOf(key)) out[k] = true }
        for (const p of selected) {
            if (p.kind === "face" || p.kind === "edge" || p.kind === "point" || p.kind === "body")
                add("b" + (p.kind === "body" ? p.index : (p.body || doc.currentBody())))
            else if (p.kind === "profile" || p.kind.startsWith("sketch_"))
                add("s" + p.sketch)
        }
        return out
    }

    readonly property real pointPixels: 9      // how near the mouse must be, in pixels
    readonly property real sketchPointPixels: 11   // a sketch's points: a fifth easier to land on (owner, 2026-10-30)
    readonly property real edgePixels: 7
    readonly property real onProfilePixels: 3   // a corner or edge over a profile: only when right on it
    // How far in front of a face a profile on it counts as on top, in mm: `pull` plus `share` of the
    // distance (ProfileMaterial draws it over the face by the same).
    readonly property var profileMargin: ({ pull: 0.1, share: 0.002 })
    readonly property real tubePixels: 2.6     // highlight thickness (radius), in pixels

    // Screen pixel → world size per unit of distance from the camera.
    function perPixel() {
        return 2 * Math.tan(nav.fov * Math.PI / 360) / Math.max(1, view.height)
    }

    // The ray under a window point, in engine coordinates (scene is Y up, engine Z up).
    // Without perspective (ortho) every ray runs straight ahead, from the spot beside the camera.
    function rayAt(x, y) {
        const k = perPixel()
        const side = camera.right.times((x - view.width / 2) * k), down = camera.up.times((y - view.height / 2) * k)
        const d = ortho ? camera.forward : camera.forward.plus(side).minus(down)
        const o = ortho ? camera.scenePosition.plus(side.times(nav.distance)).minus(down.times(nav.distance))
                        : camera.scenePosition
        return {
            origin: Qt.vector3d(o.x, -o.z, o.y),
            direction: Qt.vector3d(d.x, -d.z, d.y)
        }
    }

    // How far along the ray through (x, y) the body's surface is; 0 where there's none.
    function surfaceAt(x, y) {
        if (!pickable)
            return 0
        const r = rayAt(x, y)
        return Math.max(0, doc.pick(r.origin, r.direction, 0, 0).depth)
    }

    // The face under a point. `anyFace`: the Faces switch doesn't matter (the whole model is meant).
    function faceAt(x, y, anyFace) {
        if (!pickable || (bodyFiltered && (!allow.bodies || (!anyFace && !allow.faces))))
            return -1
        const r = rayAt(x, y)
        return doc.faceAt(r.origin, r.direction)
    }

    // The shown sketches made before the open one, whose lines, arcs and points Project can take
    // (Outside Sketches switch); Main keeps the list.
    property var outsideSketches: []
    // The nearest part of one of them under a point: {kind, sketch, index}, or null.
    function outsideSketchAt(x, y) {
        if (!sketchId || (bodyFiltered && !allow.outside))
            return null
        const earlier = doc.earlierSketches(sketchId)
        const r = rayAt(x, y), k = perPixel()
        let best = null
        for (const id of outsideSketches) {
            if (earlier.indexOf(id) < 0)
                continue
            const hit = doc.pickSketch(id, r.origin, r.direction, sketchPointPixels * k, edgePixels * k, 15)
            if (hit.kind && (!best || hit.depth < best.depth))
                best = { kind: hit.kind, sketch: id, index: hit.index, depth: hit.depth }
        }
        return best
    }

    // A text (in any shown sketch, the open one too) under a point: {sketch, index}, or null.
    function textAt(x, y) {
        const r = rayAt(x, y), k = perPixel()
        const ids = outsideSketches.slice()
        if (sketchId && ids.indexOf(sketchId) < 0)
            ids.push(sketchId)
        let best = null
        for (const id of ids) {
            const hit = doc.pickSketch(id, r.origin, r.direction, sketchPointPixels * k, edgePixels * k, 15)
            if (hit.kind !== "sketch_curve" || (best && hit.depth >= best.depth))
                continue
            const c = doc.sketchGeometry(id).curves.find(q => q.id === hit.index)
            if (c && c.type === "text")
                best = { sketch: id, index: hit.index, depth: hit.depth }
        }
        return best
    }

    // An edge or corner of the model under a point (Project's third choice), if those are switched on.
    function bodyEdgeOrPointAt(x, y) {
        if (!pickable || (bodyFiltered && !allow.bodies))
            return null
        const r = rayAt(x, y), k = perPixel()
        const hit = doc.pick(r.origin, r.direction, !bodyFiltered || allow.bodyPoints ? pointPixels * k : 0,
                             !bodyFiltered || allow.edges ? edgePixels * k : 0, through)
        return hit.kind === "point" || hit.kind === "edge" ? { kind: hit.kind, index: hit.index, body: hit.body } : null
    }

    function partAt(x, y) {
        const r = rayAt(x, y)
        const k = perPixel()
        if (sketchId) {
            const drawn = doc.pickSketch(sketchId, r.origin, r.direction, sketchPointPixels * k, edgePixels * k, sketchAllow())
            if (drawn.kind)
                return { kind: drawn.kind, sketch: sketchId, index: drawn.index }
        }
        // Bodies off: the model is left out altogether, hides nothing and can't be picked.
        const hit = pickable && (!bodyFiltered || allow.bodies)
                    ? doc.pick(r.origin, r.direction, !bodyFiltered || allow.bodyPoints ? pointPixels * k : 0,
                               !bodyFiltered || allow.edges ? edgePixels * k : 0, through)
                    : { kind: "", depth: -1 }
        // A body part that's switched off is skipped, but the body still hides what's behind it.
        const body = hit.kind && (!bodyFiltered || (hit.kind !== "face" || allow.faces)) ? { kind: hit.kind, index: hit.index, body: hit.body } : null
        const surface = hit.depth > 0 ? hit.depth : Infinity
        const o = filtered && !allow.origin ? null : origin.hitAt(r.origin, r.direction, pointPixels * k, edgePixels * k)
        const near = o && o.t <= surface + o.reach * 2      // not hidden behind the model
        const asPart = o ? { kind: "origin", name: o.name, part: o.part, index: -1 } : null
        const area = profileSketches.length && (!filtered || allow.profiles) ? doc.pickProfile(profileSketches, r.origin, r.direction) : { kind: "" }
        // On or in front of the model: a profile on a face is picked before the face, with the same
        // margin its shading is drawn over the face with (ProfileMaterial); otherwise the face's own
        // triangles, a hair off its plane, kept taking the click (owner, 2026-11-03).
        const onTop = area.kind && area.depth <= surface + profileMargin.pull + surface * profileMargin.share
        const profile = onTop ? { kind: "profile", sketch: area.sketch, index: area.index, u: area.u, v: area.v } : null

        // A profile under the mouse wins over the model's corners and edges and the origin's unless
        // the mouse is right on one: a converted body has them all over a face, and with their usual
        // reach they took nearly every click meant for a profile on it (owner, 2026-11-03).
        // Ones behind the profile (down a hole it spans) never do. A shown origin plane through the
        // part doesn't either: it's picked where there's no profile.
        if (profile) {
            const front = area.depth + profileMargin.pull + area.depth * profileMargin.share
            if (body && (hit.kind === "point" || hit.kind === "edge")) {
                const tight = doc.pick(r.origin, r.direction, !bodyFiltered || allow.bodyPoints ? onProfilePixels * k : 0,
                                       !bodyFiltered || allow.edges ? onProfilePixels * k : 0, through)
                if ((tight.kind === "point" || tight.kind === "edge") && tight.depth <= front)
                    return { kind: tight.kind, index: tight.index, body: tight.body }
            }
            if (near && o.part !== "plane") {
                const right = origin.hitAt(r.origin, r.direction, onProfilePixels * k, onProfilePixels * k)
                if (right && right.part !== "plane" && right.t <= front)
                    return { kind: "origin", name: right.name, part: right.part, index: -1 }
            }
            return profile
        }
        if (near && o.part === "point")
            return asPart
        if (hit.kind === "point")
            return body
        if (near && o.part === "axis" && hit.kind !== "edge")
            return asPart
        if (hit.kind === "edge")
            return body
        if (o && o.part === "plane" && o.t < surface)
            return asPart
        return body
    }

    // The Project tool's picks: lines twice as thick as a selection, faces (or the whole model) apart.
    function drawProject(radius, eye, perMm, withFaces) {
        if (!projectEdgeGeometry || !projectFaceGeometry)
            return
        const lines = [], faces = []
        const add = p => {
            if (p.what === "box")
                p.items.forEach(add)
            else if (p.what === "edge")
                lines.push({ kind: p.kind, index: p.index })
            else if (p.what === "face")
                faces.push({ kind: "face", index: p.face })
            else if (p.what === "model")
                for (let i = 0, n = doc.faceTotal(); i < n; ++i)
                    faces.push({ kind: "face", index: i })
            else if (p.what === "sketchpart")
                lines.push({ kind: p.kind, sketch: p.sketch, index: p.index })
        }
        projectPicks.forEach(add)
        // Thick, but in proportion to the model (owner, 2026-11-02): zoomed out the lines shrink on
        // screen (pixel thickness follows the square root of the zoom, between 0.3x and 1.2x).
        const ext = doc.boundsMax.minus(doc.boundsMin).length()
        const fit = Math.max(ext, 1) * 1.8
        const k = Math.min(1.2, Math.max(0.3, Math.sqrt(fit / Math.max(nav.distance, 1e-6))))
        doc.fillParts(projectEdgeGeometry, lines, radius * 2 * k, eye, perMm * 2 * k, true)
        if (withFaces)
            doc.fillParts(projectFaceGeometry, faces, radius, eye, perMm)
    }

    function same(a, b) { return !!a && !!b && a.kind === b.kind && a.index === b.index && a.name === b.name && a.sketch === b.sketch }

    function hoverAt(x, y) {
        const part = partAt(x, y)
        if (!same(part, hovered) && (part || hovered)) {
            hovered = part
            redraw()
        }
    }

    function clearHover() {
        if (hovered) {
            hovered = null
            redraw()
        }
    }

    // A tool that decides what a click means (Extrude: Ctrl/Shift for its areas, a plain click for
    // the object to go up to) sets this; it gets the part under the pointer, or null, and whether
    // Ctrl or Shift was held, and the selection is left to it.
    property var clickHook: null

    function clickAt(x, y, modifiers) {
        const part = partAt(x, y)
        const adding = modifiers & (Qt.ControlModifier | Qt.ShiftModifier)
        if (clickHook) {
            clickHook(part, !!adding)
            redraw()
            return
        }
        if (!part) {
            if (!adding)
                selected = []
        } else if (adding) {
            const i = selected.findIndex(p => same(p, part))
            selected = i >= 0 ? selected.filter((p, k) => k !== i) : selected.concat([part])
        } else if (selected.some(p => same(p, part))) {
            // Clicking what's already selected un-selects it, anywhere in the app (owner, 2026-11-01).
            selected = selected.filter(p => !same(p, part))
        } else {
            selected = [part]
        }
        redraw()
    }

    // After a sketch changed: drop its parts that are gone, and light the rest where they are now.
    // A picked profile is found again by its spot; gone when no closed area holds it any more.
    function keepIn(geometry) {
        const refind = p => p.kind !== "profile" ? p
                          : Object.assign({}, p, { index: doc.profileAt(p.sketch, p.u, p.v) })
        selected = selected.map(refind).filter(p => p.kind !== "profile" || p.index >= 0)
        if (hovered && hovered.kind === "profile") {
            hovered = refind(hovered)
            if (hovered.index < 0)
                hovered = null
        }
        const there = p => p.kind === "sketch_line" ? geometry.lines.some(l => l.id === p.index)
                         : p.kind === "sketch_point" ? geometry.points.some(q => q.id === p.index)
                         : p.kind === "sketch_arc" ? geometry.arcs.some(a => a.id === p.index)
                         : p.kind === "sketch_curve" ? (geometry.curves || []).some(k => k.id === p.index)
                         : true
        selected = selected.filter(there)
        if (hovered && !there(hovered))
            hovered = null
        redraw()
    }

    function clear() {
        selected = []
        hovered = null
        redraw()
    }

    // Tubes and balls keep the same on-screen thickness at any zoom.
    // The model's parts are drawn here; the origin lights its own (OriginShapes).
    // The picked parts are rebuilt only when the picks change, or (after the view moved) shortly after
    // it stopped when there are many: hovering rebuilt them every time (owner, 2026-11-02, t47u).
    property var selectedBuilt: null
    function redrawSelected() {
        selectedBuilt = selected
        const radius = nav.distance * perPixel() * tubePixels
        const ofModel = p => p.kind !== "origin"
        const drawn = p => p.kind.startsWith("sketch_")
        const area = p => p.kind === "profile"
        const ofBody = p => ofModel(p) && !drawn(p) && !area(p)
        doc.fillProfileParts(profileSelectedGeometry, selected.filter(area))
        const eye = ortho ? Qt.vector3d(0, 0, 0) : rayAt(view.width / 2, view.height / 2).origin
        const perMm = ortho ? 0 : perPixel() * tubePixels
        doc.fillParts(selectedGeometry, selected.filter(ofBody), radius, eye, perMm)
        doc.fillParts(sketchSelectedGeometry, selected.filter(drawn), radius)
        origin.selected = selected.filter(p => !ofModel(p)).map(p => p.name)
    }
    property Timer selectedResize: Timer {
        interval: 120
        onTriggered: sel.redrawSelected()
    }
    // The view moved (zoom or turn): a small pick list follows at once, a big one a moment later.
    function viewRedraw() {
        if (selected.length > 30) {
            if (hovered)
                redrawHover()
            selectedResize.restart()
        } else {
            redraw(true)
        }
    }
    function redrawHover() {
        const radius = nav.distance * perPixel() * tubePixels
        const ofModel = p => p.kind !== "origin"
        const drawn = p => p.kind.startsWith("sketch_")
        const area = p => p.kind === "profile"
        const ofBody = p => ofModel(p) && !drawn(p) && !area(p)
        doc.fillProfileParts(profileHoverGeometry, hovered && area(hovered) && !selected.some(p => same(p, hovered)) ? [hovered] : [])
        const eye = ortho ? Qt.vector3d(0, 0, 0) : rayAt(view.width / 2, view.height / 2).origin
        const perMm = ortho ? 0 : perPixel() * tubePixels
        doc.fillParts(hoverGeometry, hovered && ofBody(hovered) ? [hovered] : [], radius * 1.15, eye, perMm * 1.15)
        doc.fillParts(sketchHoverGeometry, hovered && drawn(hovered) ? [hovered] : [], radius * 1.15)
        origin.hovered = hovered && ofModel(hovered) === false ? hovered.name : ""
    }
    function redraw(rebuildSelected) {
        if (rebuildSelected === true || selectedBuilt !== selected)
            redrawSelected()
        redrawHover()
    }

    // A profile whose sketch was hidden, or isn't the open one while sketching, goes.
    onProfileSketchesChanged: {
        const stays = p => p.kind !== "profile" || profileSketches.includes(p.sketch)
        if (!selected.every(stays) || (hovered && !stays(hovered))) {
            selected = selected.filter(stays)
            if (hovered && !stays(hovered))
                hovered = null
            redraw()
        }
    }
    // Hidden parts can't stay lit or selected.
    onPickableChanged: {
        if (pickable)
            return
        const stays = p => p.kind === "origin" || p.kind.startsWith("sketch_") || p.kind === "profile"
        selected = selected.filter(stays)
        if (hovered && !stays(hovered))
            hovered = null
        redraw()
    }
    property Connections originShown: Connections {
        target: sel.origin
        function onShownChanged() {
            const shown = sel.origin.shown
            const stays = p => p.kind !== "origin" || !!shown[p.name]
            if (!sel.selected.every(stays) || (sel.hovered && !stays(sel.hovered))) {
                sel.selected = sel.selected.filter(stays)
                if (sel.hovered && !stays(sel.hovered))
                    sel.hovered = null
                sel.redraw()
            }
        }
    }

    // The Project highlight keeps its thickness in pixels while zooming or turning (owner, 2026-11-02):
    // its edges are rebuilt shortly after the view moves (at most every 40 ms); its faces needn't be.
    // Hovering, selecting or turning never rebuilds the Project highlight (owner, 2026-11-02, t46u: with
    // hundreds of picks that took seconds). It is built when the picks change, and its edges again
    // once a zoom has stopped for a moment (nothing is rebuilt while the wheel is still turning).
    function projectRedraw(withFaces) {
        const radius = nav.distance * perPixel() * tubePixels
        const eye = ortho ? Qt.vector3d(0, 0, 0) : rayAt(view.width / 2, view.height / 2).origin
        drawProject(radius, eye, ortho ? 0 : perPixel() * tubePixels, withFaces)
    }
    property Timer projectResize: Timer {
        interval: 180
        onTriggered: sel.projectRedraw(false)
    }
    function projectViewMoved() {
        if (projectPicks.length)
            projectResize.restart()
    }
    property Connections zoom: Connections {
        target: sel.nav
        function onDistanceChanged() {
            sel.projectViewMoved()
            if (sel.hovered || sel.selected.length)
                sel.viewRedraw()
        }
    }
    // Turning or panning changes how far an edge is from the camera, so its thickness too.
    property Connections moved: Connections {
        target: sel.camera
        function onScenePositionChanged() {
            const thin = p => p.kind === "edge" || p.kind === "point"
            if (!sel.ortho && ((sel.hovered && thin(sel.hovered)) || sel.selected.some(thin)))
                sel.viewRedraw()
        }
    }
    // A rebuilt model has new parts: old numbers would point at the wrong ones.
    property Connections model: Connections {
        target: doc
        function onChanged() { sel.clear() }
    }
}
