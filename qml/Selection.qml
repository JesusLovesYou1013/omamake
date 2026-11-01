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

    property var hovered: null
    property var selected: []

    readonly property real pointPixels: 9      // how near the mouse must be, in pixels
    readonly property real sketchPointPixels: 11   // a sketch's points: a fifth easier to land on (owner, 2026-10-30)
    readonly property real edgePixels: 7
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

    function faceAt(x, y) {
        if (!pickable)
            return -1
        const r = rayAt(x, y)
        return doc.faceAt(r.origin, r.direction)
    }

    function partAt(x, y) {
        const r = rayAt(x, y)
        const k = perPixel()
        if (sketchId) {
            const drawn = doc.pickSketch(sketchId, r.origin, r.direction, sketchPointPixels * k, edgePixels * k)
            if (drawn.kind)
                return { kind: drawn.kind, sketch: sketchId, index: drawn.index }
        }
        const hit = pickable ? doc.pick(r.origin, r.direction, pointPixels * k, edgePixels * k)
                             : { kind: "", depth: -1 }
        const body = hit.kind ? { kind: hit.kind, index: hit.index } : null
        const surface = hit.depth > 0 ? hit.depth : Infinity
        const o = origin.hitAt(r.origin, r.direction, pointPixels * k, edgePixels * k)
        const near = o && o.t <= surface + o.reach * 2      // not hidden behind the model
        const asPart = o ? { kind: "origin", name: o.name, part: o.part, index: -1 } : null
        const area = profileSketches.length ? doc.pickProfile(profileSketches, r.origin, r.direction) : { kind: "" }
        const onTop = area.kind && area.depth <= surface * 1.0001 + 1e-3     // on or in front of the model
        const profile = onTop ? { kind: "profile", sketch: area.sketch, index: area.index, u: area.u, v: area.v } : null

        if (near && o.part === "point")
            return asPart
        if (hit.kind === "point")
            return body
        if (near && o.part === "axis" && hit.kind !== "edge")
            return asPart
        if (hit.kind === "edge")
            return body
        if (profile && !(o && o.part === "plane" && o.t < area.depth * 0.9999 - 1e-3))
            return profile
        if (o && o.part === "plane" && o.t < surface)
            return asPart
        return body
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

    function clickAt(x, y, modifiers) {
        const part = partAt(x, y)
        const adding = modifiers & (Qt.ControlModifier | Qt.ShiftModifier)
        if (!part) {
            if (!adding)
                selected = []
        } else if (adding) {
            const i = selected.findIndex(p => same(p, part))
            selected = i >= 0 ? selected.filter((p, k) => k !== i) : selected.concat([part])
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
    function redraw() {
        const radius = nav.distance * perPixel() * tubePixels
        const ofModel = p => p.kind !== "origin"
        const drawn = p => p.kind.startsWith("sketch_")
        const area = p => p.kind === "profile"
        const ofBody = p => ofModel(p) && !drawn(p) && !area(p)
        doc.fillProfileParts(profileSelectedGeometry, selected.filter(area))
        doc.fillProfileParts(profileHoverGeometry, hovered && area(hovered) && !selected.some(p => same(p, hovered)) ? [hovered] : [])
        // The camera may be far nearer the part than the pivot (zoomed in on a big model).
        const eye = ortho ? Qt.vector3d(0, 0, 0) : rayAt(view.width / 2, view.height / 2).origin
        const perMm = ortho ? 0 : perPixel() * tubePixels
        doc.fillParts(selectedGeometry, selected.filter(ofBody), radius, eye, perMm)
        doc.fillParts(hoverGeometry, hovered && ofBody(hovered) ? [hovered] : [], radius * 1.15, eye, perMm * 1.15)
        doc.fillParts(sketchSelectedGeometry, selected.filter(drawn), radius)
        doc.fillParts(sketchHoverGeometry, hovered && drawn(hovered) ? [hovered] : [], radius * 1.15)
        origin.hovered = hovered && !ofModel(hovered) ? hovered.name : ""
        origin.selected = selected.filter(p => !ofModel(p)).map(p => p.name)
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

    property Connections zoom: Connections {
        target: sel.nav
        function onDistanceChanged() {
            if (sel.hovered || sel.selected.length)
                sel.redraw()
        }
    }
    // Turning or panning changes how far an edge is from the camera, so its thickness too.
    property Connections moved: Connections {
        target: sel.camera
        function onScenePositionChanged() {
            const thin = p => p.kind === "edge" || p.kind === "point"
            if (!sel.ortho && ((sel.hovered && thin(sel.hovered)) || sel.selected.some(thin)))
                sel.redraw()
        }
    }
    // A rebuilt model has new parts: old numbers would point at the wrong ones.
    property Connections model: Connections {
        target: doc
        function onChanged() { sel.clear() }
    }
}
