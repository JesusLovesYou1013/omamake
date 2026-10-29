import QtQuick

// Fusion-style view cube, top right:
//   click a face, edge or corner   fly to that view (and fit the model)
//   drag the cube                  orbit the camera around the model
//   house button                   back to the home view
// Drawn flat on a Canvas (no second 3D pass): an orthographic cube only needs
// each face's 2D corner positions, so this stays cheap.
Item {
    id: cube

    required property var theme
    required property var nav

    width: 150
    height: 150

    readonly property real half: 34          // half the cube's size, in pixels
    readonly property real band: 0.6         // past this (of 1) a click means edge or corner

    // Scene coordinates (Y up). The model's engine Z (up) is scene Y; engine Y is scene -Z.
    // u/v: which way the label's right and up point on that face.
    readonly property var faces: [
        { label: "FRONT",  n: [0, 0, 1],  u: [1, 0, 0],  v: [0, 1, 0] },
        { label: "BACK",   n: [0, 0, -1], u: [-1, 0, 0], v: [0, 1, 0] },
        { label: "RIGHT",  n: [1, 0, 0],  u: [0, 0, -1], v: [0, 1, 0] },
        { label: "LEFT",   n: [-1, 0, 0], u: [0, 0, 1],  v: [0, 1, 0] },
        { label: "TOP",    n: [0, 1, 0],  u: [1, 0, 0],  v: [0, 0, -1] },
        { label: "BOTTOM", n: [0, -1, 0], u: [1, 0, 0],  v: [0, 0, 1] }
    ]

    property var hovered: null               // region under the mouse, e.g. [1, 1, 0]
    property bool hot: mouse.containsMouse || homeArea.containsMouse
    property point pressedAt
    property point last
    property bool dragging: false

    opacity: hot || dragging ? 1 : 0.9
    Behavior on opacity { NumberAnimation { duration: 150 } }

    // Scene direction → screen offset from the cube's centre (x right, y down) + depth toward us.
    function project(p) {
        const yr = nav.yaw * Math.PI / 180, pr = nav.pitch * Math.PI / 180
        const x1 = p[0] * Math.cos(yr) - p[2] * Math.sin(yr)
        const z1 = p[0] * Math.sin(yr) + p[2] * Math.cos(yr)
        const y2 = p[1] * Math.cos(pr) + z1 * Math.sin(pr)
        const z2 = -p[1] * Math.sin(pr) + z1 * Math.cos(pr)
        return { x: x1 * half, y: -y2 * half, z: z2 }
    }

    function screenOf(f) {
        return { c: project(f.n), u: project(f.u), v: project(f.v) }
    }

    // Which part of the cube is under a canvas point: a direction like [1, 0, 1] (an edge).
    function regionAt(px, py) {
        const mx = px - canvas.width / 2, my = py - canvas.height / 2
        for (const f of faces) {
            const s = screenOf(f)
            if (s.c.z < 0.05)
                continue
            const ax = mx - s.c.x, ay = my - s.c.y
            const det = s.u.x * s.v.y - s.u.y * s.v.x
            const fs = (ax * s.v.y - ay * s.v.x) / det
            const ft = (s.u.x * ay - s.u.y * ax) / det
            if (Math.abs(fs) > 1 || Math.abs(ft) > 1)
                continue
            const step = q => q > band ? 1 : q < -band ? -1 : 0
            const a = step(fs), b = step(ft)
            return [0, 1, 2].map(i => f.n[i] + f.u[i] * a + f.v[i] * b)
        }
        return null
    }

    Connections {
        target: cube.nav
        function onYawChanged() { canvas.requestPaint() }
        function onPitchChanged() { canvas.requestPaint() }
    }
    Connections {
        target: cube.theme
        function onLightChanged() { canvas.requestPaint() }
    }
    onHoveredChanged: canvas.requestPaint()

    Canvas {
        id: canvas
        anchors.fill: parent

        // Canvas colours as CSS strings (it ignores colour objects with alpha).
        function rgba(c, a) {
            const k = v => Math.round(Math.max(0, Math.min(1, v)) * 255)
            return "rgba(" + k(c.r) + "," + k(c.g) + "," + k(c.b) + "," + a + ")"
        }

        // A rectangle on a face, from its own square (-1..1 each way) to the screen.
        function facePath(ctx, s, a0, a1, b0, b1) {
            const at = (a, b) => [s.c.x + s.u.x * a + s.v.x * b, s.c.y + s.u.y * a + s.v.y * b]
            const pts = [at(a0, b0), at(a1, b0), at(a1, b1), at(a0, b1)]
            ctx.beginPath()
            ctx.moveTo(pts[0][0], pts[0][1])
            for (let i = 1; i < 4; i++)
                ctx.lineTo(pts[i][0], pts[i][1])
            ctx.closePath()
        }

        // The axes start at the front-left-bottom corner: X red, Y green, Z blue (engine axes).
        function drawAxes(ctx, onTop) {
            const corner = [-1, -1, 1]
            const axes = [
                { d: [1, 0, 0],  color: cube.theme.axisX, label: "X", faces: [[0, -1, 0], [0, 0, 1]] },
                { d: [0, 0, -1], color: cube.theme.axisY, label: "Y", faces: [[0, -1, 0], [-1, 0, 0]] },
                { d: [0, 1, 0],  color: cube.theme.accent, label: "Z", faces: [[-1, 0, 0], [0, 0, 1]] }
            ]
            const o = cube.project(corner)
            for (const a of axes) {
                const visible = a.faces.some(n => cube.project(n).z > 0)
                if (visible !== onTop)
                    continue
                const tip = cube.project([0, 1, 2].map(i => corner[i] + a.d[i] * 2.45))
                ctx.strokeStyle = rgba(a.color, 1)
                ctx.lineWidth = 2
                ctx.beginPath()
                ctx.moveTo(o.x, o.y)
                ctx.lineTo(tip.x, tip.y)
                ctx.stroke()
                const lab = cube.project([0, 1, 2].map(i => corner[i] + a.d[i] * 2.8))
                ctx.fillStyle = rgba(a.color, 1)
                ctx.font = "bold 11px sans-serif"
                ctx.textAlign = "center"
                ctx.textBaseline = "middle"
                ctx.fillText(a.label, lab.x, lab.y)
            }
        }

        onPaint: {
            const ctx = getContext("2d")
            ctx.reset()
            ctx.clearRect(0, 0, width, height)
            ctx.translate(width / 2, height / 2)
            const t = cube.theme

            drawAxes(ctx, false)

            const visible = cube.faces
                .map(f => ({ f: f, s: cube.screenOf(f) }))
                .filter(e => e.s.c.z > 0.001)
            for (const e of visible) {
                const f = e.f, s = e.s
                // Faces turned toward us are a little lighter (dark look), or the ones
                // turned away a little darker (light look, so nothing goes white).
                const lift = t.light ? -0.1 * (1 - s.c.z) - 0.03 : 0.06 * s.c.z
                facePath(ctx, s, -1, 1, -1, 1)
                ctx.fillStyle = rgba(Qt.rgba(t.panelRaised.r + lift, t.panelRaised.g + lift * 1.2,
                                             t.panelRaised.b + lift * 1.6, 1), 1)
                ctx.fill()

                // Highlight the hovered face, edge or corner wherever it shows on this face.
                const h = cube.hovered
                if (h && [0, 1, 2].every(i => f.n[i] === 0 || h[i] === f.n[i])) {
                    const a = [0, 1, 2].reduce((sum, i) => sum + h[i] * f.u[i], 0)
                    const b = [0, 1, 2].reduce((sum, i) => sum + h[i] * f.v[i], 0)
                    const span = k => k === 0 ? [-cube.band, cube.band] : k > 0 ? [cube.band, 1] : [-1, -cube.band]
                    const sa = span(a), sb = span(b)
                    facePath(ctx, s, sa[0], sa[1], sb[0], sb[1])
                    ctx.fillStyle = rgba(t.accent, 0.55)
                    ctx.fill()
                }

                facePath(ctx, s, -1, 1, -1, 1)
                ctx.strokeStyle = rgba(t.borderStrong, 1)
                ctx.lineWidth = 1.2
                ctx.stroke()

                // The label, laid onto the face so it turns with it.
                if (s.c.z > 0.2) {
                    ctx.save()
                    ctx.setTransform(s.u.x / cube.half, s.u.y / cube.half, -s.v.x / cube.half, -s.v.y / cube.half,
                                     width / 2 + s.c.x, height / 2 + s.c.y)
                    ctx.fillStyle = rgba(t.textPrimary, Math.min(1, s.c.z * 1.3))
                    ctx.font = "bold 12px sans-serif"
                    ctx.textAlign = "center"
                    ctx.textBaseline = "middle"
                    ctx.fillText(f.label, 0, 0)
                    ctx.restore()
                }
            }

            drawAxes(ctx, true)
        }
    }

    MouseArea {
        id: mouse
        // Only the cube itself, not the empty corners of this item.
        anchors.centerIn: parent
        width: cube.half * 3.6
        height: cube.half * 3.6
        hoverEnabled: true
        cursorShape: cube.hovered ? Qt.PointingHandCursor : Qt.ArrowCursor
        acceptedButtons: Qt.LeftButton

        function toCanvas(m) { return mapToItem(canvas, m.x, m.y) }

        onPositionChanged: m => {
            const p = toCanvas(m)
            if (pressed) {
                if (!cube.dragging && Math.hypot(p.x - cube.pressedAt.x, p.y - cube.pressedAt.y) > 4) {
                    cube.dragging = true
                    cube.hovered = null
                }
                if (cube.dragging)
                    cube.nav.orbit(p.x - cube.last.x, p.y - cube.last.y)
                cube.last = p
            } else {
                cube.hovered = cube.regionAt(p.x, p.y)
            }
        }
        onPressed: m => {
            const p = toCanvas(m)
            cube.pressedAt = p
            cube.last = p
            cube.dragging = false
        }
        onReleased: m => {
            if (!cube.dragging) {
                const p = toCanvas(m)
                const r = cube.regionAt(p.x, p.y)
                if (r)
                    cube.nav.lookAlong({ x: r[0], y: r[1], z: r[2] })
            }
            cube.dragging = false
            cube.hovered = containsMouse ? cube.regionAt(toCanvas(m).x, toCanvas(m).y) : null
        }
        onExited: if (!pressed) cube.hovered = null
    }

    // Home: back to the starting angle (Fusion keeps a little house by the cube).
    Item {
        id: home
        x: 8
        y: 8
        width: 22
        height: 22
        opacity: cube.hot ? 1 : 0
        Behavior on opacity { NumberAnimation { duration: 150 } }

        Canvas {
            anchors.fill: parent
            property color ink: homeArea.containsMouse ? cube.theme.accent : cube.theme.textSecondary
            onInkChanged: requestPaint()
            onPaint: {
                const ctx = getContext("2d")
                ctx.reset()
                ctx.strokeStyle = ink
                ctx.lineWidth = 1.6
                ctx.lineJoin = "round"
                ctx.beginPath()
                ctx.moveTo(3, 11)
                ctx.lineTo(11, 4)
                ctx.lineTo(19, 11)
                ctx.moveTo(6, 9)
                ctx.lineTo(6, 18)
                ctx.lineTo(16, 18)
                ctx.lineTo(16, 9)
                ctx.stroke()
            }
        }
        MouseArea {
            id: homeArea
            anchors.fill: parent
            hoverEnabled: true
            cursorShape: Qt.PointingHandCursor
            onClicked: cube.nav.home()
        }
    }
}
