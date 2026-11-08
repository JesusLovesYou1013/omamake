import QtQuick
import QtQuick3D

// The model's look, cut where the Timeline's rewind has got to (shaders/rewind.frag, rewindline.frag):
// `lines` for the outline, else the shaded faces. Positions are the scene's (Y up).
CustomMaterial {
    property bool lines: false
    property vector3d origin: Qt.vector3d(0, 0, 0)
    property vector3d axis: Qt.vector3d(0, 1, 0)
    property real low: 0
    property vector3d xDir: Qt.vector3d(1, 0, 0)
    property vector3d yDir: Qt.vector3d(0, 0, -1)
    property vector4d box: Qt.vector4d(-1e9, 1e9, -1e9, 1e9)   // u0, u1, v0, v1 along xDir, yDir
    property real high: 0
    property real keepInside: 1
    property real glowWidth: 1
    property real glowOn: 1
    property color tint: "grey"
    property color ink: "black"
    property color glow: "white"
    property real metal: 0.85
    property real rough: 0.28
    shadingMode: lines ? CustomMaterial.Unshaded : CustomMaterial.Shaded
    cullMode: Material.NoCulling
    vertexShader: lines ? "shaders/rewindline.vert" : ""
    fragmentShader: lines ? "shaders/rewindline.frag" : "shaders/rewind.frag"
}
