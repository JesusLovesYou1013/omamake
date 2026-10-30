import QtQuick

// Sketch mode: drawing flat outlines on a plane, which later become or change bodies.
//   triple-click a flat face    sketch on that face
//   triple-click an origin plane  sketch on that plane
//   triple-click empty space    sketch on the origin plane that faces you most (XY, XZ or YZ)
// While sketching the origin planes are hidden; their eyes in the Browser show them again
// for this sketch (shownPlanes, by row key; not in the history).
//   Create Sketch (search)      the next click picks the face or empty space, Esc cancels
//   Finish Sketch, Esc          back to the model, keeping the sketch
// Leaving (Finish Sketch, Esc, or Ctrl+Z) flies the view back to exactly how it was
// before the sketch was started or reopened (owner, 2026-10-29).
// Starting and finishing are both actions in the document's history, so Ctrl+Z (Main.qml)
// steps back through them: undoing a finish reopens the sketch, undoing a start removes it.
// On entering, the view turns square onto the plane (Fusion's Look At). A plane is
// {origin, normal, xAxis, yAxis} in engine coordinates (mm, Z up); the normal faces the viewer.
QtObject {
    id: sketch

    required property var nav
    required property var selection

    property bool active: false
    property bool picking: false     // Create Sketch is waiting for a click
    property var plane: null
    property int sketchId: 0         // the open sketch's id in the document
    property var viewBefore: null    // the view to go back to on leaving
    property string message: ""      // a short note when a pick can't start a sketch
    property var shownPlanes: ({})   // origin plane rows shown again during this sketch

    function showPlane(key, show) {
        const next = Object.assign({}, shownPlanes)
        if (show)
            next[key] = true
        else
            delete next[key]
        shownPlanes = next
    }

    // Fusion's origin planes, each with its own right and up as seen from the front side.
    readonly property var originPlanes: [
        { name: "XY", normal: Qt.vector3d(0, 0, 1),  xAxis: Qt.vector3d(1, 0, 0), yAxis: Qt.vector3d(0, 1, 0) },
        { name: "XZ", normal: Qt.vector3d(0, -1, 0), xAxis: Qt.vector3d(1, 0, 0), yAxis: Qt.vector3d(0, 0, 1) },
        { name: "YZ", normal: Qt.vector3d(1, 0, 0),  xAxis: Qt.vector3d(0, 1, 0), yAxis: Qt.vector3d(0, 0, 1) }
    ]

    // An origin plane, turned to face the viewer.
    function facing(p) {
        const toViewer = selection.rayAt(selection.view.width / 2, selection.view.height / 2).direction.times(-1)
        const flip = p.normal.dotProduct(toViewer) < 0 ? -1 : 1   // seen from behind: mirror X so Y stays up
        return {
            label: "the " + p.name + " plane",
            origin: Qt.vector3d(0, 0, 0),
            normal: p.normal.times(flip),
            xAxis: p.xAxis.times(flip),
            yAxis: p.yAxis
        }
    }

    // The origin plane most square to the view.
    function originPlane() {
        const toViewer = selection.rayAt(selection.view.width / 2, selection.view.height / 2).direction.times(-1)
        let best = originPlanes[0]
        for (const p of originPlanes)
            if (Math.abs(p.normal.dotProduct(toViewer)) > Math.abs(best.normal.dotProduct(toViewer)))
                best = p
        return facing(best)
    }

    // The plane under a window point, or null (with a message) if it can't hold a sketch.
    function planeAt(x, y) {
        const part = selection.partAt(x, y)
        if (!part)
            return originPlane()
        if (part.kind === "face") {
            const p = doc.facePlane(part.index)
            if (p.flat) {
                p.label = "a flat face"
                return p
            }
            say("That face is curved. Pick a flat face, a plane, or empty space.")
            return null
        }
        if (part.kind === "origin" && part.part === "plane")
            return facing(originPlanes.find(p => p.name === part.name))
        say("Pick a flat face, a plane, or empty space.")
        return null
    }

    function startAt(x, y) {
        const p = planeAt(x, y)
        if (p)
            start(p)
    }

    function start(p) {
        open(doc.addSketch(p), p)
    }

    // Into a sketch that exists (new, or reopened by undoing its finish).
    function open(id, p) {
        picking = false
        message = ""
        if (!active) {
            viewBefore = nav.viewState()
            shownPlanes = {}
        }
        plane = p
        sketchId = id
        active = true
        selection.clear()
        nav.lookAlong(Qt.vector3d(p.normal.x, p.normal.z, -p.normal.y))   // engine → scene
    }

    // Finish Sketch / Esc: an action in the history.
    function finish() {
        doc.finishSketch(sketchId)
        leave()
    }

    // Out of sketch mode without recording anything (the sketch was undone).
    function leave() {
        active = false
        plane = null
        sketchId = 0
        if (viewBefore)
            nav.restoreView(viewBefore)
        viewBefore = null
    }

    function say(text) {
        message = text
        messageTimer.restart()
    }

    property Timer messageTimer: Timer {
        interval: 2600
        onTriggered: sketch.message = ""
    }
}
