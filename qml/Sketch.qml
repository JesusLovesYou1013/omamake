import QtQuick

// Sketch mode: drawing flat outlines on a plane, which later become or change bodies.
//   triple-click a flat face    sketch on that face
//   triple-click empty space    sketch on the origin plane that faces you most (XY, XZ or YZ)
//   Create Sketch (search)      the next click picks the face or empty space, Esc cancels
//   Finish Sketch               back to the model
// On entering, the view turns square onto the plane (Fusion's Look At). A plane is
// {origin, normal, xAxis, yAxis} in engine coordinates (mm, Z up); the normal faces the viewer.
QtObject {
    id: sketch

    required property var nav
    required property var selection

    property bool active: false
    property bool picking: false     // Create Sketch is waiting for a click
    property var plane: null
    property string message: ""      // a short note when a pick can't start a sketch

    // Fusion's origin planes, each with its own right and up as seen from the front side.
    readonly property var originPlanes: [
        { name: "XY", normal: Qt.vector3d(0, 0, 1),  xAxis: Qt.vector3d(1, 0, 0), yAxis: Qt.vector3d(0, 1, 0) },
        { name: "XZ", normal: Qt.vector3d(0, -1, 0), xAxis: Qt.vector3d(1, 0, 0), yAxis: Qt.vector3d(0, 0, 1) },
        { name: "YZ", normal: Qt.vector3d(1, 0, 0),  xAxis: Qt.vector3d(0, 1, 0), yAxis: Qt.vector3d(0, 0, 1) }
    ]

    // The origin plane most square to the view, turned to face the viewer.
    function originPlane() {
        const toViewer = selection.rayAt(selection.view.width / 2, selection.view.height / 2).direction.times(-1)
        let best = originPlanes[0], bestDot = 0
        for (const p of originPlanes) {
            const d = p.normal.dotProduct(toViewer)
            if (Math.abs(d) > Math.abs(bestDot)) {
                best = p
                bestDot = d
            }
        }
        const flip = bestDot < 0 ? -1 : 1   // seen from behind: mirror X so Y stays up
        return {
            origin: Qt.vector3d(0, 0, 0),
            normal: best.normal.times(flip),
            xAxis: best.xAxis.times(flip),
            yAxis: best.yAxis
        }
    }

    // The plane under a window point, or null (with a message) if it can't hold a sketch.
    function planeAt(x, y) {
        const part = selection.partAt(x, y)
        if (!part)
            return originPlane()
        if (part.kind === "face") {
            const p = doc.facePlane(part.index)
            if (p.flat)
                return p
            say("That face is curved. Pick a flat face, or empty space.")
            return null
        }
        say("Pick a flat face, or empty space.")
        return null
    }

    function startAt(x, y) {
        const p = planeAt(x, y)
        if (p)
            start(p)
    }

    function start(p) {
        picking = false
        message = ""
        plane = p
        active = true
        selection.clear()
        nav.lookAlong(Qt.vector3d(p.normal.x, p.normal.z, -p.normal.y))   // engine → scene
    }

    function finish() {
        active = false
        plane = null
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
