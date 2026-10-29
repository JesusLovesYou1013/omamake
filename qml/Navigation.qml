import QtQuick

// Moving around the 3D space, Fusion 360 style:
//   middle-drag         pan
//   Shift+middle-drag   orbit
//   wheel               zoom, toward the cursor
//   double-click middle fit the model to the window
//   hand button (by the view cube) left-drag pans too, until clicked again or Esc
// The left button is left free for selecting. The view cube (top right) uses
// orbit() and lookFrom() below.
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

    property bool panMode: false

    acceptedButtons: panMode ? (Qt.LeftButton | Qt.MiddleButton) : Qt.MiddleButton
    cursorShape: !panMode ? Qt.ArrowCursor : (pressedButtons & Qt.LeftButton) ? Qt.ClosedHandCursor : Qt.OpenHandCursor
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
        lookFrom(35, -28)
    }

    // Turn the view by a mouse movement, in pixels. Shared by Shift+middle-drag
    // and dragging the view cube, so both feel the same.
    function orbit(dx, dy) {
        flight.stop()
        yaw -= dx * orbitDegreesPerPixel
        pitch = Math.max(-90, Math.min(90, pitch - dy * orbitDegreesPerPixel))
    }

    // Fly to a view angle and fit the model, by the shortest way round.
    function lookFrom(toYaw, toPitch) {
        flight.stop()
        yaw = ((yaw % 360) + 540) % 360 - 180
        let target = ((toYaw % 360) + 540) % 360 - 180
        if (target - yaw > 180) target -= 360
        if (target - yaw < -180) target += 360
        flyYaw.to = target
        flyPitch.to = toPitch
        fit()
        flight.start()
    }

    // Look from a direction in scene coordinates (Y up), e.g. (0, 1, 0) for the top.
    function lookAlong(dir) {
        const flat = Math.hypot(dir.x, dir.z)
        const toYaw = flat > 1e-6 ? Math.atan2(dir.x, dir.z) * 180 / Math.PI : 0
        lookFrom(toYaw, -Math.atan2(dir.y, flat) * 180 / Math.PI)
    }

    ParallelAnimation {
        id: flight
        NumberAnimation { id: flyYaw; target: nav; property: "yaw"; duration: 380; easing.type: Easing.OutCubic }
        NumberAnimation { id: flyPitch; target: nav; property: "pitch"; duration: 380; easing.type: Easing.OutCubic }
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
        if ((mouse.modifiers & Qt.ShiftModifier) && (mouse.buttons & Qt.MiddleButton)) {
            orbit(dx, dy)
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
