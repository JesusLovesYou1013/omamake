import QtQuick
import QtQuick3D

// Fusion's origin, drawn in model space (engine coordinates, mm, Z up): the centre
// point O, the X (red), Y (green) and Z (blue) axes, and the XY, XZ and YZ planes as
// see-through squares. shown: {O, X, Y, Z, XY, XZ, YZ} → true/false (the fade is the
// view's crossfade, Main.qml). Lines keep the same on-screen thickness at any zoom.
// Each plane has a style (PlaneStyleMenu): "cross" is full size through the centre, out
// to both sides; "corner" is smaller, on the plus side only, apart from the axes and the
// other planes, so the three read as the corner of a cube without touching.
// Every part can be hovered and selected (hitAt, used by Selection.qml); hovered and
// selected parts light up in the same colours as the model's.
Node {
    id: origin

    required property var theme
    property var shown: ({})
    property real size: 150              // half the planes' width, in mm
    property real lineRadius: 1          // set from the view distance
    property var styles: ({})            // {XY, XZ, YZ} → "cross" / "corner"
    property string hovered: ""          // part name under the mouse
    property var selected: []            // selected part names

    readonly property real axisLength: size * 2.6

    // A plane's span along both of its axes: -size…size, or a smaller square standing off
    // from the axes on the plus side.
    function span(corner) {
        return corner ? [size * 0.12, size * 0.72] : [-size, size]
    }

    function lit(name) {
        return hovered === name || selected.includes(name)
    }
    function colorOf(name, normal) {
        return selected.includes(name) ? theme.selection : hovered === name ? theme.hover : normal
    }

    // The shown part under a ray (engine coordinates), or null: {part: "point"/"axis"/"plane",
    // name, t (distance along the ray), reach (how far off it may be there)}. The point wins
    // over the axes, the axes over the planes; planes go by the nearest.
    // pointReach, edgeReach: allowed miss per unit of distance from the camera.
    function hitAt(o, dir, pointReach, edgeReach) {
        const d = dir.normalized()
        const axes = { X: Qt.vector3d(1, 0, 0), Y: Qt.vector3d(0, 1, 0), Z: Qt.vector3d(0, 0, 1) }

        if (shown.O) {
            const t = o.times(-1).dotProduct(d)
            const reach = Math.max(t, 1) * pointReach
            if (t > 0 && o.plus(d.times(t)).length() <= reach)
                return { part: "point", name: "O", t: t, reach: reach }
        }

        let best = null
        for (const name of ["X", "Y", "Z"]) {
            if (!shown[name])
                continue
            // Closest approach between the ray and the axis line, the axis clamped to its length.
            const a = axes[name]
            const b = d.dotProduct(a)
            const denom = 1 - b * b
            let s = denom > 1e-9 ? (a.dotProduct(o) - b * d.dotProduct(o)) / denom : 0
            s = Math.max(-axisLength / 2, Math.min(axisLength / 2, s))
            const onAxis = a.times(s)
            const t = onAxis.minus(o).dotProduct(d)
            if (t <= 0)
                continue
            const gap = o.plus(d.times(t)).minus(onAxis).length()
            const reach = Math.max(t, 1) * edgeReach
            if (gap <= reach && (!best || gap / reach < best.score))
                best = { part: "axis", name: name, t: t, reach: reach, score: gap / reach }
        }
        if (best)
            return best

        const planes = { XY: ["x", "y", "z"], XZ: ["x", "z", "y"], YZ: ["y", "z", "x"] }
        for (const name in planes) {
            if (!shown[name])
                continue
            const [u, v, n] = planes[name]
            if (Math.abs(d[n]) < 1e-9)
                continue
            const t = -o[n] / d[n]
            if (t <= 0 || (best && t >= best.t))
                continue
            const p = o.plus(d.times(t))
            const [lo, hi] = span(styles[name] === "corner")
            if (p[u] >= lo && p[u] <= hi && p[v] >= lo && p[v] <= hi)
                best = { part: "plane", name: name, t: t, reach: 0 }
        }
        return best
    }


    component Line: Model {
        property string along: "x"
        property real length: 100
        property real radius: 1
        property color color: "white"
        source: "#Cylinder"              // 100 long along its own Y, radius 50
        eulerRotation: along === "x" ? Qt.vector3d(0, 0, 90) : along === "z" ? Qt.vector3d(90, 0, 0) : Qt.vector3d(0, 0, 0)
        scale: Qt.vector3d(radius / 50, length / 100, radius / 50)
        castsShadows: false
        receivesShadows: false
        materials: PrincipledMaterial {
            lighting: PrincipledMaterial.NoLighting
            baseColor: color
        }
    }

    component Part: Node {
        property bool on: false
        visible: on
    }

    // A plane: a see-through square with an outline. a, b: the two axes it spans.
    component Plane: Part {
        id: plane
        property string a: "x"
        property string b: "y"
        property string name
        property bool corner: false
        readonly property real lo: origin.span(corner)[0]
        readonly property real hi: origin.span(corner)[1]
        readonly property color edge: origin.colorOf(name, origin.theme.warning)
        readonly property bool lit: origin.lit(name)
        readonly property real mid: (lo + hi) / 2
        readonly property real len: hi - lo
        function at(u, v) {
            return Qt.vector3d(a === "x" ? u : b === "x" ? v : 0,
                               a === "y" ? u : b === "y" ? v : 0,
                               a === "z" ? u : b === "z" ? v : 0)
        }
        Model {
            source: "#Rectangle"         // 100 × 100 in its own XY plane
            position: plane.at(plane.mid, plane.mid)
            eulerRotation: plane.a === "x" && plane.b === "z" ? Qt.vector3d(90, 0, 0)
                         : plane.a === "y" ? Qt.vector3d(0, 90, 0) : Qt.vector3d(0, 0, 0)
            scale: Qt.vector3d(plane.len / 100, plane.len / 100, 1)
            castsShadows: false
            receivesShadows: false
            materials: PrincipledMaterial {
                lighting: PrincipledMaterial.NoLighting
                cullMode: Material.NoCulling
                baseColor: Qt.rgba(plane.edge.r, plane.edge.g, plane.edge.b, plane.lit ? 0.3 : 0.16)
                alphaMode: PrincipledMaterial.Blend
            }
        }
        Line { along: plane.a; position: plane.at(plane.mid, plane.lo); length: plane.len; radius: origin.lineRadius * (plane.lit ? 0.9 : 0.6); color: plane.edge }
        Line { along: plane.a; position: plane.at(plane.mid, plane.hi); length: plane.len; radius: origin.lineRadius * (plane.lit ? 0.9 : 0.6); color: plane.edge }
        Line { along: plane.b; position: plane.at(plane.lo, plane.mid); length: plane.len; radius: origin.lineRadius * (plane.lit ? 0.9 : 0.6); color: plane.edge }
        Line { along: plane.b; position: plane.at(plane.hi, plane.mid); length: plane.len; radius: origin.lineRadius * (plane.lit ? 0.9 : 0.6); color: plane.edge }
    }

    Part {
        on: !!origin.shown.O
        Model {
            source: "#Sphere"            // radius 50
            scale: Qt.vector3d(1, 1, 1).times(origin.lineRadius * (origin.lit("O") ? 4.2 : 3) / 50)
            castsShadows: false
            materials: PrincipledMaterial {
                lighting: PrincipledMaterial.NoLighting
                baseColor: origin.colorOf("O", origin.theme.textPrimary)
            }
        }
    }
    Part { on: !!origin.shown.X; Line { along: "x"; length: origin.axisLength; radius: origin.lineRadius * (origin.lit("X") ? 1.5 : 1); color: origin.colorOf("X", origin.theme.axisX) } }
    Part { on: !!origin.shown.Y; Line { along: "y"; length: origin.axisLength; radius: origin.lineRadius * (origin.lit("Y") ? 1.5 : 1); color: origin.colorOf("Y", origin.theme.axisY) } }
    Part { on: !!origin.shown.Z; Line { along: "z"; length: origin.axisLength; radius: origin.lineRadius * (origin.lit("Z") ? 1.5 : 1); color: origin.colorOf("Z", origin.theme.accent) } }

    Plane { name: "XY"; on: !!origin.shown.XY; a: "x"; b: "y"; corner: origin.styles.XY === "corner" }
    Plane { name: "XZ"; on: !!origin.shown.XZ; a: "x"; b: "z"; corner: origin.styles.XZ === "corner" }
    Plane { name: "YZ"; on: !!origin.shown.YZ; a: "y"; b: "z"; corner: origin.styles.YZ === "corner" }
}
