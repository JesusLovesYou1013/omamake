import QtQuick

// Hovering and selecting parts of the model, Fusion style:
//   hover                   the face, edge or point under the mouse lights up (hover colour)
//   click                   select it (selection colour), replacing the selection
//   Ctrl- or Shift-click    add it to the selection, or take it out again
//   click empty space, Esc  clear the selection
// Points win over edges, edges over faces (Document::pick). A part is {kind, index}.
QtObject {
    id: sel

    required property var view
    required property var camera
    required property var nav
    required property var hoverGeometry
    required property var selectedGeometry

    property var hovered: null
    property var selected: []

    readonly property real pointPixels: 9      // how near the mouse must be, in pixels
    readonly property real edgePixels: 7
    readonly property real tubePixels: 2.6     // highlight thickness (radius), in pixels

    // Screen pixel → world size per unit of distance from the camera.
    function perPixel() {
        return 2 * Math.tan(nav.fov * Math.PI / 360) / Math.max(1, view.height)
    }

    // The ray under a window point, in engine coordinates (scene is Y up, engine Z up).
    function rayAt(x, y) {
        const k = perPixel()
        const d = camera.forward
            .plus(camera.right.times((x - view.width / 2) * k))
            .minus(camera.up.times((y - view.height / 2) * k))
        const o = camera.scenePosition
        return {
            origin: Qt.vector3d(o.x, -o.z, o.y),
            direction: Qt.vector3d(d.x, -d.z, d.y)
        }
    }

    function partAt(x, y) {
        const r = rayAt(x, y)
        const k = perPixel()
        const hit = doc.pick(r.origin, r.direction, pointPixels * k, edgePixels * k)
        return hit.kind ? { kind: hit.kind, index: hit.index } : null
    }

    function same(a, b) { return !!a && !!b && a.kind === b.kind && a.index === b.index }

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

    function clear() {
        selected = []
        hovered = null
        redraw()
    }

    // Tubes and balls keep the same on-screen thickness at any zoom.
    function redraw() {
        const radius = nav.distance * perPixel() * tubePixels
        doc.fillParts(selectedGeometry, selected, radius)
        doc.fillParts(hoverGeometry, hovered ? [hovered] : [], radius * 1.15)
    }

    property Connections zoom: Connections {
        target: sel.nav
        function onDistanceChanged() {
            if (sel.hovered || sel.selected.length)
                sel.redraw()
        }
    }
    // A rebuilt model has new parts: old numbers would point at the wrong ones.
    property Connections model: Connections {
        target: doc
        function onChanged() { sel.clear() }
    }
}
