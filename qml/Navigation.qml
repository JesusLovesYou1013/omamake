import QtQuick

// Moving around the 3D space, Fusion 360 style:
//   middle-drag         pan
//   Shift+middle-drag   orbit
//   wheel               zoom, toward the cursor
//   double-click middle fit the model to the window
// The left button is left free for selecting.
MouseArea {
    id: nav

    required property var view
    required property var pivot
    required property var camera

    property real yaw: 0
    property real pitch: 0
    property real distance: 1000
    property real fov: 40
    property bool gridVisible: true
    property bool simpleLook: false

    readonly property real orbitDegreesPerPixel: 0.3
    readonly property real zoomPerNotch: 0.88

    acceptedButtons: Qt.MiddleButton
    property point last

    // Model bounds come in engine coordinates (mm, Z up); the scene is Y up.
    function sceneBounds() {
        const lo = doc.boundsMin, hi = doc.boundsMax
        return {
            center: Qt.vector3d((lo.x + hi.x) / 2, (lo.z + hi.z) / 2, -(lo.y + hi.y) / 2),
            radius: Math.max(1, Qt.vector3d(hi.x - lo.x, hi.y - lo.y, hi.z - lo.z).length() / 2)
        }
    }

    function fit() {
        const b = sceneBounds()
        pivot.position = b.center
        distance = b.radius / Math.sin(fov * Math.PI / 360) * 1.6
    }

    function home() {
        yaw = 35
        pitch = -28
        fit()
    }

    // Scene units covered by one screen pixel at the pivot's depth.
    function unitsPerPixel() {
        return 2 * distance * Math.tan(fov * Math.PI / 360) / Math.max(1, height)
    }

    onPressed: mouse => last = Qt.point(mouse.x, mouse.y)

    onPositionChanged: mouse => {
        const dx = mouse.x - last.x
        const dy = mouse.y - last.y
        last = Qt.point(mouse.x, mouse.y)
        if (mouse.modifiers & Qt.ShiftModifier) {
            yaw -= dx * orbitDegreesPerPixel
            pitch = Math.max(-89, Math.min(89, pitch - dy * orbitDegreesPerPixel))
        } else {
            const u = unitsPerPixel()
            pivot.position = pivot.position
                .minus(camera.right.times(dx * u))
                .plus(camera.up.times(dy * u))
        }
    }

    onDoubleClicked: mouse => {
        if (mouse.button === Qt.MiddleButton)
            fit()
    }

    onWheel: wheel => {
        const factor = Math.pow(zoomPerNotch, wheel.angleDelta.y / 120)
        // Keep the point under the cursor still while zooming.
        const u = unitsPerPixel() * (1 - factor)
        const dx = wheel.x - width / 2
        const dy = wheel.y - height / 2
        pivot.position = pivot.position
            .plus(camera.right.times(dx * u))
            .minus(camera.up.times(dy * u))
        distance = Math.max(5, distance * factor)
    }
}
