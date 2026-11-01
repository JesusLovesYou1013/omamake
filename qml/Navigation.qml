import QtQuick

// Moving around the 3D space, Fusion 360 style:
//   middle-drag         pan
//   Shift+middle-drag   orbit
//   wheel               zoom, toward the cursor
//   inertia (owner, 2026-10-30): a pan or orbit let go while moving glides on and soon stops;
//   a wheel notch's zoom eases in. One knob, `inertia`; zoom always gets half of it
//   double-click middle fit the model to the window
//   hand button (by the view cube) left-drag pans too, until clicked again or Esc
//   double-click left   doubleClickAt (in a sketch: the whole connected shape)
//   triple-click left   tripleClickAt (Main.qml starts a sketch there)
//   left-drag           handed to `leftDrag` when it takes it (dragging sketch lines)
// The left button selects (signals hoverAt / clickAt / hoverLeft, handled by Selection.qml),
// or pans while the hand is on. The view cube (top right) uses orbit() and lookFrom() below.
MouseArea {
    id: nav

    required property var view
    required property var pivot
    required property var camera
    property var surfaceAt: null          // (x, y) => distance to the body along that ray, or 0

    property real yaw: 0
    property real pitch: 0
    property real distance: 1000
    property real fov: 40
    property bool gridVisible: true
    property bool simpleLook: false

    readonly property real orbitDegreesPerPixel: 0.3
    readonly property real zoomPerNotch: 0.88
    readonly property real nearest: 0.1       // how close the view can get to its centre (mm)

    property bool panMode: false
    property bool ctrlHeld: false        // Ctrl down at the mouse's last move or press (free drawing)

    // How long (s) a thrown pan or orbit takes to lose about two thirds of its speed (it's
    // still after about three times that); zoom eases in over half of it, so turning the knob
    // scales all three together. 0: no inertia.
    property real inertia: 0.15
    readonly property real zoomInertia: inertia * 0.5
    property var samples: []              // the drag's last moves: [{t, dx, dy}] (ms, pixels)
    property string dragKind: ""          // what the drag in progress does: "pan" | "orbit"
    property string coasting: ""          // what's gliding after it: "pan" | "orbit" | ""
    property real vx: 0                   // its speed, pixels per second
    property real vy: 0
    property real zoomLeft: 0             // zoom still to come (log of the factor)
    property point zoomAt                 // toward where

    signal hoverAt(real x, real y)
    signal clickAt(real x, real y, int modifiers)
    signal hoverLeft()
    signal doubleClickAt(real x, real y)
    signal tripleClickAt(real x, real y)

    // Something that wants plain left-drags: {begin(x, y, modifiers) → bool, move(x, y), end()}.
    // begin is asked when the mouse first moves past clickSlop; false leaves the drag alone.
    property var leftDrag: null
    property bool dragging: false

    // Clicks in a row: each within the system's double-click time of the last, near the first.
    property int clicksInRow: 0
    property real lastClickTime: 0
    property point rowAt

    acceptedButtons: Qt.LeftButton | Qt.MiddleButton
    hoverEnabled: true
    readonly property real clickSlop: 4    // pixels a click may move and still be a click
    property point pressAt
    property bool moved: false
    cursorShape: !panMode ? Qt.ArrowCursor : (pressedButtons & Qt.LeftButton) ? Qt.ClosedHandCursor : Qt.OpenHandCursor
    property point last

    // Model bounds come in engine coordinates (mm, Z up); the scene is Y up.
    function sceneBounds() {
        const lo = doc.boundsMin, hi = doc.boundsMax
        return {
            center: Qt.vector3d((lo.x + hi.x) / 2, (lo.z + hi.z) / 2, -(lo.y + hi.y) / 2),
            // An empty design: room for the origin's planes.
            radius: Math.max(lo.x === hi.x && lo.y === hi.y && lo.z === hi.z ? 150 : 1,
                             Qt.vector3d(hi.x - lo.x, hi.y - lo.y, hi.z - lo.z).length() / 2)
        }
    }

    function fit() {
        const b = sceneBounds()
        fitTo(b.center, b.radius)
    }
    // Centre on a ball (scene coordinates) and come back far enough to see all of it.
    function fitTo(center, radius) {
        stopGlide()
        pivot.position = center
        distance = radius / Math.sin(fov * Math.PI / 360) * 1.6
    }

    function home() {
        lookFrom(35, -28)
    }

    // Turn the view by a mouse movement, in pixels. Shared by Shift+middle-drag
    // and dragging the view cube, so both feel the same.
    function orbit(dx, dy) {
        flight.stop()
        flightBack.stop()
        stopGlide()
        dragKind = "orbit"
        note(dx, dy)
        turn(dx, dy)
    }
    function turn(dx, dy) {
        yaw -= dx * orbitDegreesPerPixel
        pitch = Math.max(-90, Math.min(90, pitch - dy * orbitDegreesPerPixel))
    }
    function pan(dx, dy) {
        const u = unitsPerPixel()
        pivot.position = pivot.position
            .minus(camera.right.times(dx * u))
            .plus(camera.up.times(dy * u))
    }
    // Put the centre at the depth of the surface under (x, y), the camera staying put: zooming
    // and panning are scaled to the centre, so this is what lets them reach and follow what's
    // being looked at, however far in (Fusion does the same).
    function focusOn(x, y) {
        const along = surfaceAt ? surfaceAt(x, y) : 0
        if (along <= 0 || view.camera !== camera)
            return
        const reach = Math.tan(fov * Math.PI / 360) / (height / 2)
        const depth = along / Math.hypot(1, (x - width / 2) * reach, (y - height / 2) * reach)
        pivot.position = pivot.position.plus(camera.forward.times(depth - distance))
        distance = Math.max(nearest, depth)
    }
    // Zoom by `factor`, keeping the point under (x, y) still.
    function zoomBy(factor, x, y) {
        const u = unitsPerPixel() * (1 - factor)
        pivot.position = pivot.position
            .plus(camera.right.times((x - width / 2) * u))
            .minus(camera.up.times((y - height / 2) * u))
        distance = Math.max(nearest, distance * factor)
    }

    // Inertia: the drag's moves are noted; letting go throws the view on at the speed of
    // the last 80 ms, unless the mouse had already stopped.
    function note(dx, dy) {
        const t = Date.now()
        samples = samples.filter(s => t - s.t < 80).concat([{ t: t, dx: dx, dy: dy }])
    }
    function letGo() {
        const t = Date.now(), recent = samples.filter(s => t - s.t < 80), kind = dragKind
        samples = []
        dragKind = ""
        if (inertia <= 0 || !kind || !recent.length || t - recent[recent.length - 1].t > 40)
            return
        const seconds = Math.max(0.016, (t - recent[0].t) / 1000 + 0.016)
        let sx = 0, sy = 0
        for (const s of recent) { sx += s.dx; sy += s.dy }
        const speed = Math.hypot(sx, sy) / seconds, cap = Math.min(1, 5000 / Math.max(1, speed))
        if (speed < 80)
            return
        vx = sx / seconds * cap
        vy = sy / seconds * cap
        coasting = kind
    }
    function stopGlide() {
        coasting = ""
        zoomLeft = 0
    }
    FrameAnimation {
        running: nav.coasting !== "" || nav.zoomLeft !== 0
        onTriggered: {
            const dt = Math.min(0.05, frameTime)
            if (nav.coasting) {
                // Exactly what a speed fading as e^(-t / inertia) covers in dt.
                const keep = Math.exp(-dt / nav.inertia), along = nav.inertia * (1 - keep)
                if (nav.coasting === "pan") nav.pan(nav.vx * along, nav.vy * along)
                else nav.turn(nav.vx * along, nav.vy * along)
                nav.vx *= keep
                nav.vy *= keep
                if (Math.hypot(nav.vx, nav.vy) < 15)
                    nav.coasting = ""
            }
            if (nav.zoomLeft !== 0) {
                const part = Math.abs(nav.zoomLeft) < 1e-4 ? nav.zoomLeft : nav.zoomLeft * (1 - Math.exp(-dt / nav.zoomInertia))
                nav.zoomLeft -= part
                nav.zoomBy(Math.exp(part), nav.zoomAt.x, nav.zoomAt.y)
            }
        }
    }

    // Fly to a view angle and fit the model (or `around`: {center, radius}), by the shortest
    // way round.
    function lookFrom(toYaw, toPitch, around) {
        flight.stop()
        flightBack.stop()
        stopGlide()
        yaw = ((yaw % 360) + 540) % 360 - 180
        let target = ((toYaw % 360) + 540) % 360 - 180
        if (target - yaw > 180) target -= 360
        if (target - yaw < -180) target += 360
        flyYaw.to = target
        flyPitch.to = toPitch
        if (around)
            fitTo(around.center, around.radius)
        else
            fit()
        flight.start()
    }

    // Look from a direction in scene coordinates (Y up), e.g. (0, 1, 0) for the top.
    function lookAlong(dir, around) {
        const flat = Math.hypot(dir.x, dir.z)
        const toYaw = flat > 1e-6 ? Math.atan2(dir.x, dir.z) * 180 / Math.PI : 0
        lookFrom(toYaw, -Math.atan2(dir.y, flat) * 180 / Math.PI, around)
    }

    // The whole view as it is now (angle, zoom, centre), to come back to later.
    function viewState() {
        const c = pivot.position   // copied: a vector read from a property follows later changes
        return { yaw: yaw, pitch: pitch, distance: distance, center: Qt.vector3d(c.x, c.y, c.z) }
    }

    // Find in Window: fly to a Browser row's things (key) so they're all in view, turning
    // round when a body hides them from here (Document::findInWindow). Returns its answer.
    function findInWindow(key) {
        const p = pitch * Math.PI / 180, y = yaw * Math.PI / 180
        const from = Qt.vector3d(Math.sin(y) * Math.cos(p), -Math.sin(p), Math.cos(y) * Math.cos(p))
        const half = Math.atan(Math.tan(fov * Math.PI / 360) * Math.min(1, width / Math.max(1, height))) * 180 / Math.PI
        const r = doc.findInWindow(key, from, half)
        if (!r.found)
            return r
        const d = r.direction, flat = Math.hypot(d.x, d.z)
        restoreView({
            yaw: r.turned ? (flat > 1e-6 ? Math.atan2(d.x, d.z) * 180 / Math.PI : yaw) : yaw,
            pitch: r.turned ? -Math.atan2(d.y, flat) * 180 / Math.PI : pitch,
            distance: Math.max(nearest, r.distance),
            center: r.center
        })
        return r
    }

    // Fly back to a saved view, turning the shortest way round.
    function restoreView(s) {
        flight.stop()
        flightBack.stop()
        stopGlide()
        yaw = ((yaw % 360) + 540) % 360 - 180
        let target = ((s.yaw % 360) + 540) % 360 - 180
        if (target - yaw > 180) target -= 360
        if (target - yaw < -180) target += 360
        backYaw.to = target
        backPitch.to = s.pitch
        backDistance.to = s.distance
        backCenter.to = s.center
        flightBack.start()
    }

    ParallelAnimation {
        id: flightBack
        NumberAnimation { id: backYaw; target: nav; property: "yaw"; duration: 380; easing.type: Easing.OutCubic }
        NumberAnimation { id: backPitch; target: nav; property: "pitch"; duration: 380; easing.type: Easing.OutCubic }
        NumberAnimation { id: backDistance; target: nav; property: "distance"; duration: 380; easing.type: Easing.OutCubic }
        Vector3dAnimation { id: backCenter; target: nav.pivot; property: "position"; duration: 380; easing.type: Easing.OutCubic }
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

    onPressed: mouse => {
        ctrlHeld = !!(mouse.modifiers & Qt.ControlModifier)
        coasting = ""                    // a press catches a gliding view
        samples = []
        last = Qt.point(mouse.x, mouse.y)
        pressAt = last
        moved = false
        if (mouse.button === Qt.MiddleButton)
            hoverLeft()
        if (mouse.button === Qt.MiddleButton ? !(mouse.modifiers & Qt.ShiftModifier) : panMode)
            focusOn(mouse.x, mouse.y)
    }
    onReleased: mouse => {
        if (dragKind)
            letGo()
        if (dragging && mouse.button === Qt.LeftButton) {
            dragging = false
            leftDrag.end()
            return
        }
        if (mouse.button !== Qt.LeftButton || panMode || moved)
            return
        clickAt(mouse.x, mouse.y, mouse.modifiers)
        const now = Date.now()
        const inRow = now - lastClickTime <= Qt.styleHints.mouseDoubleClickInterval
                      && Math.hypot(mouse.x - rowAt.x, mouse.y - rowAt.y) <= clickSlop * 2
        clicksInRow = inRow ? clicksInRow + 1 : 1
        lastClickTime = now
        if (clicksInRow === 1)
            rowAt = Qt.point(mouse.x, mouse.y)
        if (clicksInRow === 2)
            doubleClickAt(mouse.x, mouse.y)
        if (clicksInRow === 3) {
            clicksInRow = 0
            tripleClickAt(mouse.x, mouse.y)
        }
    }
    onCanceled: {
        if (dragging) {
            dragging = false
            leftDrag.end()
        }
    }
    onExited: hoverLeft()

    onPositionChanged: mouse => {
        ctrlHeld = !!(mouse.modifiers & Qt.ControlModifier)
        if (!pressed) {
            hoverAt(mouse.x, mouse.y)
            return
        }
        if (!moved && Math.hypot(mouse.x - pressAt.x, mouse.y - pressAt.y) > clickSlop) {
            moved = true
            if ((mouse.buttons & Qt.LeftButton) && !panMode && leftDrag)
                dragging = leftDrag.begin(pressAt.x, pressAt.y, mouse.modifiers)
        }
        if (dragging) {
            leftDrag.move(mouse.x, mouse.y)
            return
        }
        // A plain left-drag (no hand) is kept for window selection later.
        if ((mouse.buttons & Qt.MiddleButton) === 0 && !panMode)
            return
        const dx = mouse.x - last.x
        const dy = mouse.y - last.y
        last = Qt.point(mouse.x, mouse.y)
        if ((mouse.modifiers & Qt.ShiftModifier) && (mouse.buttons & Qt.MiddleButton)) {
            orbit(dx, dy)
        } else {
            dragKind = "pan"
            note(dx, dy)
            pan(dx, dy)
        }
    }

    onDoubleClicked: mouse => {
        if (mouse.button === Qt.MiddleButton)
            fit()
    }

    onWheel: wheel => {
        const factor = Math.pow(zoomPerNotch, wheel.angleDelta.y / 120)
        // Eased in (inertia), toward the cursor; the whole notch's zoom still comes.
        zoomAt = Qt.point(wheel.x, wheel.y)
        focusOn(wheel.x, wheel.y)
        if (zoomInertia > 0)
            zoomLeft += Math.log(factor)
        else
            zoomBy(factor, wheel.x, wheel.y)
        hoverAt(wheel.x, wheel.y)
    }
}
