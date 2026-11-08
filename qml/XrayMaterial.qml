import QtQuick
import QtQuick3D

// A flat colour drawn over everything, whatever is in front of it (see shaders/xray.vert).
CustomMaterial {
    property color tint: "red"
    shadingMode: CustomMaterial.Unshaded
    cullMode: Material.BackFaceCulling
    sourceBlend: CustomMaterial.One
    destinationBlend: CustomMaterial.OneMinusSrcAlpha
    depthDrawMode: Material.NeverDepthDraw
    vertexShader: "shaders/xray.vert"
    fragmentShader: "shaders/xray.frag"
}
