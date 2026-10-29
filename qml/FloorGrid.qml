import QtQuick
import QtQuick3D

// Our own floor grid (Qt's built-in one can't change colour, so it vanished in the light look).
Model {
    id: grid

    required property var theme
    property real interval: 25.4         // one inch
    property real fadeDistance: 4000

    source: "#Rectangle"                 // 100 × 100 units in its own XY plane
    eulerRotation.x: -90
    scale: Qt.vector3d(2000, 2000, 1)    // 200 m across; the fade hides the edge
    castsShadows: false
    receivesShadows: false

    materials: CustomMaterial {
        shadingMode: CustomMaterial.Unshaded
        cullMode: Material.NoCulling
        sourceBlend: CustomMaterial.One
        destinationBlend: CustomMaterial.OneMinusSrcAlpha
        vertexShader: "shaders/grid.vert"
        fragmentShader: "shaders/grid.frag"

        property real interval: grid.interval
        property real fadeDistance: grid.fadeDistance
        property color gridMinor: grid.theme.gridMinor
        property color gridMajor: grid.theme.gridMajor
        property color axisX: grid.theme.axisX
        property color axisY: grid.theme.axisY
    }
}
