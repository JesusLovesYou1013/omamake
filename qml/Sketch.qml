import QtQuick

// Sketch mode: drawing flat outlines on a plane, which later become or change bodies.
//   triple-click a flat face    sketch on that face
//   triple-click an origin plane  sketch on that plane
//   triple-click empty space    sketch on the origin plane that faces you most (XY, XZ or YZ)
// While sketching the origin planes are hidden; their eyes in the Browser show them again
// for this sketch (shownPlanes, by row key; not in the history).
//   Create Sketch (search)      the next click picks the face or empty space, Esc cancels
//   Finish Sketch, Esc          back to the model, keeping the sketch
//   double-click a sketch (its Timeline dot or Browser row)   back into it (Fusion's Edit
//                               Sketch; owner, 2026-10-29). From inside another sketch,
//                               that one is finished first.
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
    // Just before Finish Sketch or going into another sketch: the sketch's steps behind the Timeline's
    // marker come back first (owner, 2026-11-07: leaving puts the marker back at the end).
    signal leaving()

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
    // Showing and hiding things cleanly, and the camera waiting until the fade is over (Main.qml).
    property var crossfade: change => change()
    property var fadeBusy: () => false
    property int fadeMs: 250
    property Timer cameraWait: Timer {
        property var run: null
        interval: sketch.fadeMs + 30
        onTriggered: { if (run) run(); run = null }
    }
    // Move the camera now, or just after the fade that is running has finished.
    function cameraDo(move) {
        if (fadeBusy()) {
            cameraWait.run = move
            cameraWait.restart()
        } else {
            move()
        }
    }

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
        cameraDo(() => nav.lookAlong(Qt.vector3d(p.normal.x, p.normal.z, -p.normal.y)))   // engine → scene
    }

    // The sketch bar's look-at button (owner, 2026-10-30): face the sketch again after panning
    // or turning away. On a face: that face in the middle, filling the view; on an origin plane:
    // the whole model, as when the sketch opened. Not in the history.
    function lookAtSketch() {
        const p = plane
        if (!p)
            return
        const c = p.center, r = p.radius
        nav.lookAlong(Qt.vector3d(p.normal.x, p.normal.z, -p.normal.y),       // engine → scene
                      c && r > 0 ? { center: Qt.vector3d(c.x, c.z, -c.y), radius: r } : null)
    }

    // Edit Sketch: an action in the history (Ctrl+Z leaves again).
    // Going back into a sketch from the past takes the design back to when it was made: what
    // was made after it fades out (and fades back in when it's finished), then the camera turns.
    function edit(id) {
        if (active && sketchId === id)
            return
        const go = () => {
            if (active) {
                leaving()
                doc.finishSketch(sketchId)     // still active, so the view to go back to stays
            }
            doc.editSketch(id)
            open(id, doc.sketchPlane(id))
        }
        if (doc.rewinds(id) || (active && doc.rewinds(0)))
            crossfade(go)
        else
            go()
    }

    // Finish Sketch / Esc: an action in the history.
    function finish() {
        const go = () => {
            leaving()
            doc.finishSketch(sketchId)
            leave()
        }
        if (active && doc.rewinds(0))
            crossfade(go)
        else
            go()
    }

    // Out of sketch mode without recording anything (the sketch was undone).
    function leave() {
        active = false
        plane = null
        sketchId = 0
        const back = viewBefore
        viewBefore = null
        if (back)
            cameraDo(() => nav.restoreView(back))
    }

    property double messageAt: 0     // when it was said (ms)
    // `ms`: how long it stays; left out, the usual 2.6 s (a long failure says how long it needs to be read).
    function say(text, ms) {
        message = text
        messageAt = Date.now()
        messageTimer.interval = ms > 0 ? ms : 2600
        messageTimer.restart()
    }

    property Timer messageTimer: Timer {
        interval: 2600
        onTriggered: sketch.message = ""
    }
}
