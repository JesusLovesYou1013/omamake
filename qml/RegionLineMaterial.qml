import QtQuick
import QtQuick3D

// The bodies' edge lines with Extrude's cut region (shaders/bodyline.frag, GB-084 second correction): role 0
// the edges as they are, none inside the region; 1 the edges of the bodies as the cut leaves them, only there.
// The region's numbers as BodyMaterial's.
CustomMaterial {
    property int roleOf: 0
    property real role: roleOf
    property color tint: "black"
    property var region: null
    property TextureInput regionMask: TextureInput { }
    readonly property bool on: !!(region && region.on)
    property real regionOn: on ? 1 : 0
    property vector3d rO: on ? region.origin : Qt.vector3d(0, 0, 0)
    property vector3d rX: on ? region.x : Qt.vector3d(1, 0, 0)
    property vector3d rY: on ? region.y : Qt.vector3d(0, 1, 0)
    property vector3d rN: on ? region.normal : Qt.vector3d(0, 0, 1)
    property vector4d regionGrid: on ? region.grid : Qt.vector4d(0, 0, 1, 1)
    property real regionRange: on ? region.range : 1
    property vector4d s0: on ? region.side0 : Qt.vector4d(0, 0, 0, 0)
    property vector4d s1: on ? region.side1 : Qt.vector4d(0, 0, 0, 0)
    shadingMode: CustomMaterial.Unshaded
    vertexShader: "shaders/body.vert"
    fragmentShader: "shaders/bodyline.frag"
}
