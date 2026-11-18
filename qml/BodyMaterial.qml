import QtQuick
import QtQuick3D

// The body's faces, and the lit or picked parts over them (shaders/body.frag): the look PrincipledMaterial
// gave them, plus Extrude's cut region (GB-084 corrections): inside it the bodies are drawn as the cut will leave them. `region`
// is ExtrudeTool's (Document::extrudeRegion), null or {on: false} for none.
CustomMaterial {
    id: m
    property int roleOf: 0                 // 0 the body (a hole at the cut), 1 the cut's result there only, 2 a part
    property real role: roleOf
    property color tint: "grey"
    property real metal: 0.85
    property real rough: 0.28
    property var region: null
    property TextureInput regionMask: TextureInput { }      // Main gives the cut mask's texture
    readonly property bool on: !!(region && region.on)
    property real regionOn: on ? 1 : 0
    property vector3d rO: on ? region.origin : Qt.vector3d(0, 0, 0)
    property vector3d rX: on ? region.x : Qt.vector3d(1, 0, 0)
    property vector3d rY: on ? region.y : Qt.vector3d(0, 1, 0)
    property vector3d rN: on ? region.normal : Qt.vector3d(0, 0, 1)
    property vector4d regionGrid: on ? region.grid : Qt.vector4d(0, 0, 1, 1)      // ("grid" alone never reached the shader)
    property real regionRange: on ? region.range : 1
    property vector4d s0: on ? region.side0 : Qt.vector4d(0, 0, 0, 0)
    property vector4d s1: on ? region.side1 : Qt.vector4d(0, 0, 0, 0)
    shadingMode: CustomMaterial.Shaded
    vertexShader: "shaders/body.vert"
    fragmentShader: "shaders/body.frag"
}
