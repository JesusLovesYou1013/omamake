import QtQuick
import QtQuick3D

// A flat, see-through colour for sketch profiles, drawn over the face they lie on without
// flickering (see shaders/profile.vert).
CustomMaterial {
    property color tint: "white"
    // Kept in step with Selection.profileMargin.
    property real pull: 0.1
    property real share: 0.002
    shadingMode: CustomMaterial.Unshaded
    cullMode: Material.NoCulling
    sourceBlend: CustomMaterial.One
    destinationBlend: CustomMaterial.OneMinusSrcAlpha
    depthDrawMode: Material.NeverDepthDraw
    vertexShader: "shaders/profile.vert"
    fragmentShader: "shaders/xray.frag"
}
