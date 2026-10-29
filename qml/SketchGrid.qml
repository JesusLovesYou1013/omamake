import QtQuick
import QtQuick3D

// The grid on the plane being sketched on, with the sketch's X (red) and Y (green) axes.
// Lives in model space (engine coordinates), so the plane's numbers place it directly.
Model {
    id: grid

    required property var theme
    property var plane: null
    property real interval: 25.4         // one inch, like the floor grid
    property real fadeDistance: 4000
    readonly property real size: 2000    // the 100-unit rectangle grows to 200 m across

    visible: plane !== null
    source: "#Rectangle"
    position: plane ? plane.origin : Qt.vector3d(0, 0, 0)
    rotation: plane ? basis(plane.xAxis, plane.yAxis, plane.normal) : Qt.quaternion(1, 0, 0, 0)
    scale: Qt.vector3d(size, size, 1)
    depthBias: -100                      // drawn over the face it lies on
    castsShadows: false
    receivesShadows: false

    // The turn that takes the rectangle's own X, Y, Z onto the plane's axes.
    function basis(u, v, n) {
        const tr = u.x + v.y + n.z
        if (tr > 0) {
            const s = Math.sqrt(tr + 1) * 2
            return Qt.quaternion(s / 4, (v.z - n.y) / s, (n.x - u.z) / s, (u.y - v.x) / s)
        }
        if (u.x > v.y && u.x > n.z) {
            const s = Math.sqrt(1 + u.x - v.y - n.z) * 2
            return Qt.quaternion((v.z - n.y) / s, s / 4, (v.x + u.y) / s, (n.x + u.z) / s)
        }
        if (v.y > n.z) {
            const s = Math.sqrt(1 + v.y - u.x - n.z) * 2
            return Qt.quaternion((n.x - u.z) / s, (v.x + u.y) / s, s / 4, (n.y + v.z) / s)
        }
        const s = Math.sqrt(1 + n.z - u.x - v.y) * 2
        return Qt.quaternion((u.y - v.x) / s, (n.x + u.z) / s, (n.y + v.z) / s, s / 4)
    }

    materials: CustomMaterial {
        shadingMode: CustomMaterial.Unshaded
        cullMode: Material.NoCulling
        sourceBlend: CustomMaterial.One
        destinationBlend: CustomMaterial.OneMinusSrcAlpha
        vertexShader: "shaders/sketchgrid.vert"
        fragmentShader: "shaders/sketchgrid.frag"

        property real interval: grid.interval
        property real fadeDistance: grid.fadeDistance
        property real planeScale: grid.size
        property color gridMinor: grid.theme.gridMinor
        property color gridMajor: grid.theme.gridMajor
        property color axisX: grid.theme.axisX
        property color axisY: grid.theme.axisY
    }
}
