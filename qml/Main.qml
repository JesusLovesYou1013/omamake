import QtQuick
import QtQuick.Window
import QtQuick3D
import QtCore
import QtQuick3D.Helpers
import QtQuick.Dialogs
import QtQuick.Shapes
import OmaMake
import "Units.js" as Units
import "ShapeTools.js" as Shapes
import "Families.js" as Families
import "SketchKeys.js" as SketchKeys

// OmaMake's window: nothing but the 3D space, edge to edge. Every command lives
// behind Alt+Space. Anything added later floats over the viewport and hides away.
Window {
    id: root
    width: 1400
    height: 900
    visible: true
    title: "OmaMake"
    color: "transparent"

    Theme {
        id: theme
        locked: saveDialog.visible || loadDialog.visible || exportDialog.visible
    }

    // Remembered between launches.
    Settings {
        location: StandardPaths.writableLocation(StandardPaths.ConfigLocation) + "/omamake/omamake.conf"
        category: "look"
        property alias light: theme.light
        property alias followSystem: theme.followSystem
        property alias lightFollowsView: root.lightFollowsView
        property alias browserShown: browserTree.shown
        property alias timelineShown: historyTimeline.shown
        property alias searchTimesOpened: search.timesOpened
        property alias planeStyle: root.planeStyle
        property alias planeStyleOverrides: root.planeStyleOverrides
    }
    Settings {
        location: StandardPaths.writableLocation(StandardPaths.ConfigLocation) + "/omamake/omamake.conf"
        category: "tools"
        property alias uses: root.toolUses
        property alias defaults: root.toolDefaults
    }

    // Which kind of each tool family (Families.js) is its default: what the search's Enter and
    // the family's key run. JSON: family → its last 20 uses / the kind that became its default.
    property string toolUses: "{}"
    property string toolDefaults: "{}"
    function readJson(text) {
        try {
            const v = JSON.parse(text)
            return v && typeof v === "object" ? v : {}
        } catch (e) {
            return {}
        }
    }
    readonly property var familiesNow: {
        const defaults = readJson(toolDefaults)
        return Families.families.map(f => Object.assign({}, f, { def: Families.defaultOf(f, defaults) }))
    }
    function noteUse(command) {
        if (sketch.active && !["Toggle Construction", "Finish Sketch"].includes(command)
                && search.commands.some(c => c.name === command && c.modes.length === 1))
            lastSketchTool = command
        const n = Families.next(readJson(toolUses), readJson(toolDefaults), command)
        toolUses = JSON.stringify(n.uses)
        toolDefaults = JSON.stringify(n.defaults)
    }

    // How the origin planes look (PlaneStyleMenu): "cross" or "corner". planeStyle is the
    // default, set by the menu's "all" choice; single planes chosen apart from it are kept
    // in planeStyleOverrides (JSON, {XY: "corner"}), until "all" is chosen again.
    property string planeStyle: "cross"
    property string planeStyleOverrides: "{}"
    readonly property var planeStyles: {
        let over = {}
        try { over = JSON.parse(planeStyleOverrides) } catch (e) {}
        const out = {}
        for (const p of ["XY", "XZ", "YZ"])
            out[p] = over[p] === "cross" || over[p] === "corner" ? over[p] : planeStyle
        return out
    }
    function setPlaneStyle(plane, style) {
        if (plane === "" ? (planeStyle === style && planeStyleOverrides === "{}") : planeStyles[plane] === style)
            return
        crossfade(() => {
            if (plane === "") {
                planeStyle = style
                planeStyleOverrides = "{}"
            } else {
                const over = {}
                for (const p of ["XY", "XZ", "YZ"]) {
                    const s = p === plane ? style : planeStyles[p]
                    if (s !== planeStyle)
                        over[p] = s
                }
                planeStyleOverrides = JSON.stringify(over)
            }
        })
    }

    // Only the space behind the model follows OmaSkins' transparency; models stay solid.
    // The lamp turns with the view, or stays put in the room (search "Light Follows View").
    property bool lightFollowsView: true

    // What the Browser's eyes leave visible, by node key (a row counts as visible only
    // when every row above it is shown too).
    readonly property var visibleKeys: {
        const out = {}
        const walk = node => {
            out[node.key] = node.visible
            for (const child of node.children)
                walk(child)
        }
        walk(doc.browser)
        return out
    }
    // Is any body visible? One mesh holds the model until bodies are separate meshes.
    readonly property bool bodyVisible: Object.keys(visibleKeys).some(k => k.startsWith("b") && visibleKeys[k])
    // Each origin part, shown if it's visible in any component (they share one place for now).
    // While sketching, the planes are hidden unless shown again with their eye during the
    // sketch (sketch.shownPlanes; owner, 2026-10-29); the point and axes stay as they are.
    readonly property var originVisible: {
        const out = {}
        for (const k in visibleKeys) {
            const part = k.split("/")[1]
            const plane = ["XY", "XZ", "YZ"].includes(part)
            if (plane && sketch.active ? !!sketch.shownPlanes[k]
                                       : visibleKeys[k] && (plane || ["O", "X", "Y", "Z"].includes(part)))
                out[part] = true
        }
        return out
    }

    property int fadeMs: 250                 // how long showing or hiding takes (owner, 2026-10-29)

    // Show or hide something cleanly: a snapshot of the view as it is now goes on top, the
    // change happens underneath at once, and the snapshot fades away. Everything (body,
    // edges, origin lines and planes, grid, shading) goes from before to after together,
    // with no part of it changing ahead of the rest. Moving the view ends it early, so the
    // snapshot never lags behind the camera.
    function crossfade(change) {
        view.grabToImage(result => {
            beforeShot.source = result.url
            fading.progress = 0
            fading.on = true
            change()
            fadeAway.restart()
        })
    }

    readonly property real spaceOpacity: root.active ? transparency.focused : transparency.unfocused

    View3D {
        id: view
        anchors.fill: parent
        camera: root.sketchOrtho ? orthoCamera : camera

        environment: SceneEnvironment {
            backgroundMode: SceneEnvironment.Color
            clearColor: Qt.rgba(theme.windowBg.r, theme.windowBg.g, theme.windowBg.b, root.spaceOpacity)
            antialiasingMode: navigation.simpleLook ? SceneEnvironment.NoAA : SceneEnvironment.MSAA
            antialiasingQuality: SceneEnvironment.High
            // Soft studio light from all around (sky above, ground below): gives every
            // face a gentle glow and sheen. Only light comes from it; the background stays ours.
            lightProbe: navigation.simpleLook ? null : studio
            probeExposure: 1.1   // bright: metal shows the studio sky, its reflections are most of its look

        }

        // Camera rig: pivot (what we look at) → yaw → pitch → camera at a distance.
        Node {
            id: pivot
            Node {
                id: yawNode
                eulerRotation.y: navigation.yaw

                Node {
                    id: pitchNode
                    eulerRotation.x: navigation.pitch
                    PerspectiveCamera {
                        id: camera
                        z: navigation.distance
                        fieldOfView: navigation.fov
                        // Near the part when zoomed right in, or the camera cuts away what it's looking at.
                        clipNear: Math.min(1, navigation.distance / 10)
                        clipFar: 200000
                    }
                    // The same view without perspective, as big as the perspective one is at
                    // the pivot (so swapping between them doesn't jump): see sketchOrtho.
                    OrthographicCamera {
                        id: orthoCamera
                        z: navigation.distance
                        clipNear: Math.min(1, navigation.distance / 10)
                        clipFar: 200000
                        horizontalMagnification: view.height / (2 * navigation.distance * Math.tan(navigation.fov * Math.PI / 360))
                        verticalMagnification: horizontalMagnification
                    }
                }
            }
        }

        // Lights: a lamp near the model, set at a distance from the model's own size (not the
        // camera's), so light falls off naturally across each face: strong on the edge nearest
        // the lamp, fading toward the far edge (inverse-square, softened). The rig sits on the
        // model's centre and turns with the view (lightFollowsView) or stays put in the room.
        Node {
            id: lightRig
            readonly property var bounds: navigation.sceneBounds()
            position: bounds.center
            eulerRotation.y: root.lightFollowsView ? navigation.yaw : 35   // 35 = the home view's angle

            PointLight {
                id: keyLight
                readonly property real reach: lightRig.bounds.radius * 2.2   // lamp ↔ model centre
                // Up, and to the left of the viewer (at the home view when fixed).
                position: Qt.vector3d(-0.62, 0.62, 0.48).normalized().times(reach)
                brightness: theme.light ? 1.35 : 1.45
                // Strength = 1 / (0.4 + 0.6·(d/reach)²): full at the model's centre, about
                // 1.6× on the nearest corner, about 0.8× on the far side. (Qt scales
                // quadraticFade by 0.0001.)
                constantFade: 0.4
                linearFade: 0
                quadraticFade: 0.6 * 10000 / (reach * reach)
                castsShadow: !navigation.simpleLook
                shadowFactor: 35
                shadowMapQuality: Light.ShadowMapQualityHigh
                shadowMapFar: reach * 4
            }
            // Rim light from behind on the right: lifts the dark side's edges off the background.
            DirectionalLight {
                eulerRotation: Qt.vector3d(-25, 150, 0)
                brightness: 0.35
            }
            // A little fill from the right, so the shadow side keeps its colour.
            DirectionalLight {
                eulerRotation: Qt.vector3d(-10, 35, 0)
                brightness: 0.12
            }
        }

        FloorGrid {
            visible: navigation.gridVisible && !sketch.active
            theme: theme
            fadeDistance: Math.max(3000, navigation.distance * 4)
        }

        Texture {
            id: studio
            textureData: ProceduralSkyTextureData {
                textureQuality: ProceduralSkyTextureData.SkyTextureQualityHigh
                skyTopColor: "#dfe7f2"
                skyHorizonColor: "#9ca8ba"
                skyEnergy: 1.0
                groundHorizonColor: "#6b7380"
                groundBottomColor: "#2a2f37"
                groundEnergy: 1.0
                sunEnergy: 0
            }
        }

        // The model, in engine coordinates (mm, Z up) turned to Quick 3D's Y up.
        Node {
            id: modelSpace
            eulerRotation.x: -90

            // The body, hidden by its eye in the Browser (the fade is the view's crossfade).
            Node {
                visible: root.bodyVisible

                Model {
                    geometry: MeshGeometry { id: faces }
                    materials: PrincipledMaterial {
                        baseColor: theme.model
                        metalness: navigation.simpleLook ? 0.08 : 0.85   // Simple Look has no sky to reflect
                        roughness: navigation.simpleLook ? 0.42 : 0.28
                    }
                    castsShadows: true
                    receivesShadows: true
                }
                Model {
                    geometry: MeshGeometry { id: edges; lines: true }
                    depthBias: -40
                    materials: PrincipledMaterial {
                        lighting: PrincipledMaterial.NoLighting
                        baseColor: theme.modelEdge
                    }
                }
            }

            // Selected parts, then the hovered part on top (Selection.qml fills these): the body's,
            // and lines and points in sketches, so outside the body's node.
            Model {
                geometry: MeshGeometry { id: selectedParts }
                depthBias: -60
                materials: PrincipledMaterial {
                    baseColor: theme.selection
                    metalness: 0.05
                    roughness: 0.45
                }
            }
            Model {
                geometry: MeshGeometry { id: hoverParts }
                depthBias: -80
                materials: PrincipledMaterial {
                    baseColor: theme.hover
                    metalness: 0.05
                    roughness: 0.45
                }
            }
            OriginShapes {
                id: originShapes
                theme: theme
                shown: root.originVisible
                styles: root.planeStyles
                size: Math.max(100, navigation.sceneBounds().radius * 0.75)
                lineRadius: navigation.distance * 0.0011
            }
            SketchGrid {
                theme: theme
                plane: sketch.plane
                fadeDistance: Math.max(3000, navigation.distance * 4)
            }
            // A profile under the mouse and picked ones (Selection), over the shading, under the lines.
            Model {
                objectName: "profileHover"
                geometry: MeshGeometry { id: profileHoverParts }
                depthBias: -25
                castsShadows: false
                receivesShadows: false
                materials: PrincipledMaterial {
                    lighting: PrincipledMaterial.NoLighting
                    baseColor: Qt.rgba(theme.hover.r, theme.hover.g, theme.hover.b, 0.30)
                    alphaMode: PrincipledMaterial.Blend
                    cullMode: Material.NoCulling
                    depthDrawMode: Material.NeverDepthDraw
                }
            }
            Model {
                objectName: "profileSelected"
                geometry: MeshGeometry { id: profileSelectedParts }
                depthBias: -25
                castsShadows: false
                receivesShadows: false
                materials: PrincipledMaterial {
                    lighting: PrincipledMaterial.NoLighting
                    baseColor: Qt.rgba(theme.selection.r, theme.selection.g, theme.selection.b, 0.40)
                    alphaMode: PrincipledMaterial.Blend
                    cullMode: Material.NoCulling
                    depthDrawMode: Material.NeverDepthDraw
                }
            }
            // Closed profiles, lightly shaded (Main.drawProfiles), under the lines.
            Model {
                objectName: "profileShading"
                geometry: MeshGeometry { id: sketchProfiles }
                depthBias: -20
                castsShadows: false
                receivesShadows: false
                materials: PrincipledMaterial {
                    lighting: PrincipledMaterial.NoLighting
                    baseColor: theme.sketchProfile
                    alphaMode: PrincipledMaterial.Blend
                    cullMode: Material.NoCulling
                    depthDrawMode: Material.NeverDepthDraw
                }
            }
            // What's drawn in sketches (Main.drawSketches), and a tool's preview.
            Model {
                geometry: MeshGeometry { id: sketchDrawn }
                depthBias: -40
                castsShadows: false
                receivesShadows: false
                materials: PrincipledMaterial {
                    lighting: PrincipledMaterial.NoLighting
                    baseColor: theme.sketchFree
                }
            }
            Model {
                geometry: MeshGeometry { id: sketchFull }
                depthBias: -40
                castsShadows: false
                receivesShadows: false
                materials: PrincipledMaterial {
                    lighting: PrincipledMaterial.NoLighting
                    baseColor: theme.sketchFull
                }
            }
            Model {
                geometry: MeshGeometry { id: sketchConstruction }
                depthBias: -40
                castsShadows: false
                receivesShadows: false
                materials: PrincipledMaterial {
                    lighting: PrincipledMaterial.NoLighting
                    baseColor: theme.sketchConstruction
                }
            }
            Model {
                geometry: MeshGeometry { id: sketchProjected }
                depthBias: -40
                castsShadows: false
                receivesShadows: false
                materials: PrincipledMaterial {
                    lighting: PrincipledMaterial.NoLighting
                    baseColor: theme.sketchProjected
                }
            }
            Model {
                geometry: MeshGeometry { id: sketchConstructionFull }
                depthBias: -40
                castsShadows: false
                receivesShadows: false
                materials: PrincipledMaterial {
                    lighting: PrincipledMaterial.NoLighting
                    baseColor: theme.sketchConstructionFull
                }
            }
            Model {
                geometry: MeshGeometry { id: sketchSelectedParts }
                depthBias: -60
                castsShadows: false
                receivesShadows: false
                materials: PrincipledMaterial {
                    lighting: PrincipledMaterial.NoLighting
                    baseColor: theme.selection
                }
            }
            Model {
                geometry: MeshGeometry { id: sketchHoverParts }
                depthBias: -80
                castsShadows: false
                receivesShadows: false
                materials: PrincipledMaterial {
                    lighting: PrincipledMaterial.NoLighting
                    baseColor: theme.hover
                }
            }
            Model {
                geometry: MeshGeometry { id: sketchPreview }
                depthBias: -50
                castsShadows: false
                receivesShadows: false
                materials: PrincipledMaterial {
                    lighting: PrincipledMaterial.NoLighting
                    baseColor: sketchTools.construction ? theme.sketchConstruction : theme.accent
                }
            }
        }
    }

    // The crossfade itself: the snapshot and the live view (kept off screen meanwhile),
    // mixed by a small shader.
    Image {
        id: beforeShot
        anchors.fill: view
        cache: false
        visible: false
    }
    ShaderEffectSource {
        id: afterLive
        anchors.fill: view
        sourceItem: fading.on ? view : null
        hideSource: true
        visible: false
    }
    ShaderEffect {
        id: fading
        property bool on: false
        property real progress: 0
        property var before: beforeShot
        property var after: afterLive
        anchors.fill: view
        visible: on
        fragmentShader: "shaders/crossfade.frag.qsb"
        NumberAnimation on progress {
            id: fadeAway
            running: false
            to: 1
            duration: root.fadeMs
            easing.type: Easing.InOutQuad
            onFinished: fading.on = false
        }
    }
    Connections {
        target: navigation
        function onYawChanged() { root.stopCrossfade() }
        function onPitchChanged() { root.stopCrossfade() }
        function onDistanceChanged() { root.stopCrossfade() }
    }
    function stopCrossfade() {
        if (fading.on) {
            fadeAway.stop()
            fading.on = false
        }
    }

    Navigation {
        id: navigation
        anchors.fill: parent
        surfaceAt: (x, y) => selection.surfaceAt(x, y)
        view: view
        pivot: pivot
        camera: camera
        onHoverAt: (x, y) => sketchTools.tool ? sketchTools.move(x, y) : selection.hoverAt(x, y)
        onClickAt: (x, y, modifiers) => sketch.picking ? sketch.startAt(x, y)
                                      : sketchTools.tool ? sketchTools.click(x, y)
                                      : selection.clickAt(x, y, modifiers)
        onDoubleClickAt: (x, y) => {
            if (!sketch.active || sketchTools.tool)
                return
            const part = selection.partAt(x, y)
            if (part && part.kind === "sketch_line") {
                selection.selected = sketchTools.profileOf(part.index)
                selection.redraw()
            }
        }
        leftDrag: ({
            begin: (x, y, modifiers) => sketchTools.dragBegin(x, y, modifiers),
            move: (x, y) => sketchTools.dragMove(x, y),
            end: () => sketchTools.dragEnd()
        })
        onTripleClickAt: (x, y) => { if (!sketch.active && !sketch.picking) sketch.startAt(x, y) }
        onHoverLeft: selection.clearHover()
    }

    // An angle dimension's marks (angleLook): its arc and the lines out to it dashed, an
    // arrow at each end.
    component AngleMarks: Shape {
        property var look: null
        visible: !!look
        anchors.fill: parent
        ShapePath {
            strokeColor: theme.textSecondary
            strokeWidth: 1
            fillColor: "transparent"
            strokeStyle: ShapePath.DashLine
            dashPattern: [3, 3]
            PathMultiline { paths: look ? [look.arc].concat(look.ext) : [] }
        }
        ShapePath {
            strokeColor: "transparent"
            fillColor: theme.textSecondary
            PathMultiline { paths: look ? look.arrows : [] }
        }
    }
    // A new angle's, while its value is typed (askDimension).
    AngleMarks { look: dimInput.open ? root.newAngle : null }

    // Find Profile Gaps: a red ring round each place lines nearly meet (root.profileCheck), big
    // enough to hold both sides of the gap however far the view is zoomed in. Fades out.
    Item {
        anchors.fill: parent
        visible: root.openSketchShown && opacity > 0
        opacity: root.gapsOn ? 1 : 0
        Behavior on opacity { NumberAnimation { duration: 300 } }
        onOpacityChanged: if (opacity === 0) root.gapsFading = false
        Repeater {
            model: root.profileCheck.gaps
            delegate: Rectangle {
                required property var modelData
                required property int index
                readonly property bool current: index === root.gapAt     // the one stepped to
                readonly property var a: { root.viewTick; root.width; return root.toScreen(modelData.a.at) }
                readonly property var b: { root.viewTick; root.width; return root.toScreen(modelData.b.at) }
                objectName: "gapRing"
                width: 2 * Math.max(14, Math.hypot(b.x - a.x, b.y - a.y) / 2 + 8)
                height: width
                radius: width / 2
                x: (a.x + b.x) / 2 - width / 2
                y: (a.y + b.y) / 2 - height / 2
                color: Qt.rgba(theme.danger.r, theme.danger.g, theme.danger.b, current ? 0.22 : 0.12)
                border.color: theme.danger
                border.width: current ? 3.5 : 2
            }
        }
    }

    // The open sketch's dimension values, over the view but under every panel: double-click
    // one to change it.
    // Distances between points get a thin line between them; a line's length sits beside it.
    Item {
        anchors.fill: parent
        visible: root.openSketchShown
        Repeater {
            model: root.dimensions
            delegate: Item {
                id: dim
                required property var modelData
                required property int index
                readonly property var a: { root.viewTick; width; return root.toScreen(modelData.a) }
                readonly property var b: { root.viewTick; width; return root.toScreen(modelData.b) }
                property bool dragging: false
                property point dragSpot
                // An angle's arc, arrows and value (root.angleLook); it follows a drag round the corner.
                readonly property var look: modelData.type !== "angle" ? null
                                          : dragging ? root.angleLook(modelData, sketchTools.onPlane(dragSpot.x, dragSpot.y))
                                          : root.angleLooks[index] || null
                readonly property point spot: look ? look.spot
                                            : dragging ? dragSpot
                                            : (root.labelSpots[index] || root.dimensionSpot(a, b, modelData.type))
                anchors.fill: parent

                AngleMarks { look: dim.look }
                Rectangle {
                    visible: dim.modelData.type !== "length" && !dim.look
                    x: dim.a.x
                    y: dim.a.y
                    width: Math.hypot(dim.b.x - dim.a.x, dim.b.y - dim.a.y)
                    height: 1
                    transformOrigin: Item.TopLeft
                    rotation: Math.atan2(dim.b.y - dim.a.y, dim.b.x - dim.a.x) * 180 / Math.PI
                    color: theme.textSecondary
                    opacity: 0.8
                }
                Rectangle {
                    x: Math.round(dim.spot.x - width / 2)
                    y: Math.round(dim.spot.y - height / 2)
                    width: label.implicitWidth + 14
                    height: 22
                    radius: 6
                    color: Qt.rgba(theme.panelBg.r, theme.panelBg.g, theme.panelBg.b, 0.85)
                    border.color: area.containsMouse ? theme.accent : theme.border
                    Text {
                        id: label
                        anchors.centerIn: parent
                        text: Units.show(dim.look ? dim.look.shown : dim.modelData.value, dim.modelData.unit)
                        color: theme.textPrimary
                        font.pixelSize: 12
                    }
                    // Double-click: change the value. Drag: move it where you like (it stays there).
                    MouseArea {
                        id: area
                        property point pressAt
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: dim.dragging ? Qt.ClosedHandCursor : Qt.PointingHandCursor
                        onPressed: m => pressAt = Qt.point(m.x, m.y)
                        onPositionChanged: m => {
                            if (!pressed || (!dim.dragging && Math.hypot(m.x - pressAt.x, m.y - pressAt.y) < 4))
                                return
                            const p = mapToItem(dim, m.x, m.y)
                            dim.dragSpot = Qt.point(p.x - pressAt.x + width / 2, p.y - pressAt.y + height / 2)
                            dim.dragging = true
                        }
                        onReleased: {
                            if (!dim.dragging)
                                return
                            const at = sketchTools.onPlane(dim.dragSpot.x, dim.dragSpot.y)
                            if (dim.look)
                                doc.placeDimensionLabel(sketch.sketchId, dim.modelData.id, dim.look.along1, dim.look.along2)
                            else if (at)
                                doc.placeDimensionLabel(sketch.sketchId, dim.modelData.id,
                                                        at.u - dim.modelData.mid_u, at.v - dim.modelData.mid_v)
                            dim.dragging = false
                        }
                        onDoubleClicked: root.editDimension(dim.modelData, dim.look)
                    }
                }
            }
        }
    }

    Selection {
        id: selection
        view: view
        camera: camera
        nav: navigation
        ortho: root.sketchOrtho
        hoverGeometry: hoverParts
        selectedGeometry: selectedParts
        sketchHoverGeometry: sketchHoverParts
        sketchSelectedGeometry: sketchSelectedParts
        profileHoverGeometry: profileHoverParts
        profileSelectedGeometry: profileSelectedParts
        origin: originShapes
        sketchId: root.openSketchShown ? sketch.sketchId : 0
        pickable: root.bodyVisible
    }

    Browser {
        id: browserTree
        anchors.left: parent.left
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        width: 300
        theme: theme
        crossfade: root.crossfade
        sketch: sketch
        onSketchEditRequested: id => root.editSketch(id)
        onFindRequested: (key, name) => {
            const r = navigation.findInWindow(key)
            if (!r.found)
                sketch.say(name + " has nothing to show yet")
            else if (r.inside)
                sketch.say(name + " is inside a body: zoomed to where it is")
        }
        onPlaneMenuRequested: (x, y) => {
            const at = mapToItem(root.contentItem, x, y)
            planeMenu.show(at.x + 4, at.y + 4)
        }
    }

    Sketch {
        id: sketch
        nav: navigation
        selection: selection
        onActiveChanged: { sketchTools.cancel(); sketchTools.construction = false; root.gapsOn = false; root.gapsFading = false; root.gapAt = -1 }
    }
    SketchTools {
        id: sketchTools
        sketch: sketch
        selection: selection
        geometryShown: root.openSketchShown
        free: navigation.ctrlHeld
    }

    // A constraint from the search (SketchTools.ruleFor): what's selected counts as the first
    // picks, in the order it was selected; if that's all it needs it's done at once, otherwise
    // the tool starts and waits for clicks, as in Fusion (owner, 2026-10-29).
    readonly property var constraintStarts: ({
        parallel: "Click the first line, then the second.", perpendicular: "Click the first line, then the second.",
        equal: "Click the first line, then the second.", collinear: "Click the first line, then the second.",
        hv: "Click a line, or two points.", coincident: "Click a point, then a point or a line.",
        midpoint: "Click a point and a line.", symmetry: "Click two points or two lines, then the line to mirror across.",
        tangent: "Click a line, arc or circle, then an arc or circle.", concentric: "Click two arcs or circles."
    })
    function constrain(type) {
        const parts = selection.selected.filter(p => p.kind.startsWith("sketch_") || p.kind === "origin")
        sketchTools.use(type)
        const before = sketchTools.applied
        for (const part of parts)
            sketchTools.pickWith(part)
        // The selection was all it needed: done, like Fusion with things picked beforehand.
        if (sketchTools.applied > before)
            sketchTools.cancel()
        else if (parts.length === 0)
            sketch.say(constraintStarts[type])
    }

    // Sketch Dimension from the search: a selected line → its length (the tool stays on, so a
    // click on a parallel line turns it into the gap); two parallel lines → the gap; two
    // points, or a point and a line → the distance; one point → waits for the second;
    // nothing → waits for a pick.
    function dimensionSelected() {
        // The origin's point and axes count too, as the sketch's own (SketchTools.asSketchPart).
        const chosen = selection.selected.filter(p => p.kind.startsWith("sketch_") || p.kind === "origin")
        const parts = chosen.map(p => sketchTools.asSketchPart(p)).filter(p => p && !p.why)
        const lines = parts.filter(p => p.kind === "sketch_line")
        const points = parts.filter(p => p.kind === "sketch_point")
        if (parts.length === 1) {
            sketchTools.use("dimension")
            return sketchTools.measureWith(chosen[0])
        }
        if (lines.length === 2 && points.length === 0) {
            const axisFirst = lines[1].index < 0     // an axis first: the gap is measured from the line
            if (!sketchTools.linesParallel(lines[0].index, lines[1].index))
                return askDimension("angle", lines[axisFirst ? 1 : 0].index, lines[axisFirst ? 0 : 1].index,
                                    sketchTools.angleOtherSide(lines[axisFirst ? 1 : 0].index, lines[axisFirst ? 0 : 1].index))
            return askDimension("lineline", lines[axisFirst ? 1 : 0].index, lines[axisFirst ? 0 : 1].index)
        }
        if (points.length === 2 && lines.length === 0)
            return askDimension("distance", points[0].index, points[1].index)
        if (points.length === 1 && lines.length === 1)
            return askDimension("pointline", points[0].index, lines[0].index)
        if (parts.length === 0 && chosen.length === 0) {
            sketchTools.use("dimension")
            sketch.say("Click a line, an arc or circle, or a point and then another point or a line.")
            return
        }
        sketch.say("Sketch Dimension: a line, an arc or circle, two points, a point and a line, or two lines.")
    }

    // The value box, where the new dimension will show, starting at what it measures now.
    // otherSide: an angle on the corner's other side (SketchTools.angleOtherSide).
    function askDimension(type, first, second, otherSide) {
        const m = doc.sketchMeasure(sketch.sketchId, type, first, second, !!otherSide)
        if (m.why) {
            dimInput.close()
            return sketch.say(m.why)
        }
        if (m.value === undefined)
            return sketch.say("Those can't be dimensioned.")
        // An angle shows on the side of the corner where its lines were clicked (between the
        // spots, SketchTools.angleSpots), as in Fusion; picked beforehand: towards their middles.
        let look = null
        if (type === "angle") {
            const d = { frame: m.frame, side: otherSide ? -1 : 1, value: m.value }, spots = sketchTools.angleSpots
            sketchTools.angleSpots = null
            let toward = null
            if (spots && m.frame && !m.frame.parallel) {
                const X = m.frame.cross, unit = s => { const l = Math.hypot(s.u - X[0], s.v - X[1]) || 1; return [(s.u - X[0]) / l, (s.v - X[1]) / l] }
                const a = unit(spots[0]), b = unit(spots[1])
                toward = { u: X[0] + a[0] + b[0], v: X[1] + a[1] + b[1], sideOnly: true }
            }
            look = angleLook(d, toward, 0)
        }
        newAngle = look
        const at = look ? look.spot : dimensionSpot(toScreen(m.a), toScreen(m.b), type)
        // Later, so a closing search box doesn't take the typing focus straight back.
        Qt.callLater(() => dimInput.show(at.x, at.y, m.value, type === "angle" ? "deg" : "in",
            v => {
                if (type === "angle" && !(v.mm > 1e-9 && v.mm < Math.PI - 1e-9))
                    return "An angle has to be more than 0° and less than 180°."
                const why = doc.addSketchDimension(sketch.sketchId, type, first, second, v.mm, v.unit,
                                                   Units.show(v.mm, v.unit), !!otherSide)
                if (!why)
                    sketchTools.dimLine = 0
                if (!why && look) {
                    const made = doc.sketchDimensions(sketch.sketchId).filter(d => d.type === "angle").pop()
                    if (made)
                        doc.placeDimensionLabel(sketch.sketchId, made.id, look.along1, look.along2, false)
                }
                return why
            }))
    }
    // A new angle's arc while its value box is open (askDimension).
    property var newAngle: null
    Connections {
        target: dimInput
        function onClosed() { root.newAngle = null }
    }

    // An angle shown on the other side of its corner (angleLook) is typed as that side's angle.
    function editDimension(d, look) {
        const at = look ? look.spot : dimensionSpot(toScreen(d.a), toScreen(d.b), d.type)
        const flip = !!(look && look.flipped)
        dimInput.show(at.x, at.y, look ? look.shown : d.value, d.unit,
            v => {
                if (d.type === "angle" && !(v.mm > 1e-9 && v.mm < Math.PI - 1e-9))
                    return "An angle has to be more than 0° and less than 180°."
                return doc.setSketchDimension(sketch.sketchId, d.id, flip ? Math.PI - v.mm : v.mm, v.unit,
                                              Units.show(v.mm, v.unit))
            })
    }

    // Engine coordinates → window pixels.
    function toScreen(p) {
        return view.mapFrom3DScene(modelSpace.mapPositionToScene(p))
    }
    // Where a dimension's value sits: halfway along, a little off to one side (above, for a
    // line's own length, so it doesn't sit on the line).
    function dimensionSpot(a, b, type) {
        const mx = (a.x + b.x) / 2, my = (a.y + b.y) / 2
        if (type !== "length")
            return Qt.point(mx, my)
        let nx = -(b.y - a.y), ny = b.x - a.x
        const len = Math.hypot(nx, ny) || 1
        if (ny > 0 || (ny === 0 && nx > 0)) { nx = -nx; ny = -ny }
        return Qt.point(mx + nx / len * 20, my + ny / len * 20)
    }

    // A sketch place (u, v mm) → window pixels.
    function planeToScreen(u, v) {
        const p = sketch.plane
        return toScreen(p.origin.plus(p.xAxis.times(u)).plus(p.yAxis.times(v)))
    }
    // Where each dimension's value sits (owner, 2026-10-30): where it was dragged to if it was;
    // otherwise its usual spot, moved off any point you'd click (a corner, a centre, a line's
    // middle, the Origin) and off the values already placed. It may sit on a line. It slides
    // along what it measures first for a radius or diameter (it stays on its line), out to the
    // side first for the rest.
    FontMetrics {
        id: dimMetrics
        font.pixelSize: 12
    }
    readonly property var labelPlaces: { viewTick; width; height; return placeLabels(dimensions) }
    readonly property var labelSpots: labelPlaces.spots || []
    readonly property var angleLooks: labelPlaces.looks || []
    function placeLabels(dims) {
        if (!sketch.active || !sketch.plane || !dims.length)
            return {}
        const g = doc.sketchGeometry(sketch.sketchId)
        const avoid = [planeToScreen(0, 0)]
        for (const p of g.points)
            if (!p.rim && !p.hidden)
                avoid.push(planeToScreen(p.u, p.v))
        for (const l of g.lines) {
            const a = g.points.find(q => q.id === l.a), b = g.points.find(q => q.id === l.b)
            if (a && b)
                avoid.push(planeToScreen((a.u + b.u) / 2, (a.v + b.v) / 2))
        }
        const taken = [], spots = [], looks = []
        const clear = (c, w, h) => avoid.every(q => Math.abs(q.x - c.x) >= w / 2 + 8 || Math.abs(q.y - c.y) >= h / 2 + 8)
                                   && taken.every(r => Math.abs(r.x - c.x) >= (r.w + w) / 2 + 4 || Math.abs(r.y - c.y) >= (r.h + h) / 2 + 4)
        for (const d of dims) {
            let look = d.type === "angle" ? angleLook(d, null, 0) : null
            const w = dimMetrics.advanceWidth(Units.show(look ? look.shown : d.value, d.unit)) + 14, h = 22
            const a = toScreen(d.a), b = toScreen(d.b)
            let spot
            if (look) {
                // An angle's value stays in the middle of its arc: the arc grows (or shrinks) to
                // take it clear.
                if (!d.placed)
                    for (let k = 0; k <= 14 && !clear(look.spot, w, h); k++)
                        for (const g of k ? [k * 10, -k * 10] : [0]) {
                            const next = angleLook(d, null, g)
                            if (clear(next.spot, w, h)) {
                                look = next
                                k = 99
                                break
                            }
                        }
                spot = look.spot
            } else if (d.placed) {
                spot = planeToScreen(d.mid_u + d.label_u, d.mid_v + d.label_v)
            } else {
                const base = dimensionSpot(a, b, d.type)
                const len = Math.hypot(b.x - a.x, b.y - a.y)
                const along = len > 1e-6 ? Qt.point((b.x - a.x) / len, (b.y - a.y) / len) : Qt.point(1, 0)
                const side = Qt.point(-along.y, along.x), back = p => Qt.point(-p.x, -p.y)
                // A length's spot is already out to one side: further out that way first.
                const mx = (a.x + b.x) / 2, my = (a.y + b.y) / 2, off = Math.hypot(base.x - mx, base.y - my)
                const out = off > 1 ? Qt.point((base.x - mx) / off, (base.y - my) / off) : side
                const ways = d.type === "radius" || d.type === "diameter" ? [along, back(along), out, back(out)]
                                                                         : [out, back(out), along, back(along)]
                spot = base
                search: for (let k = 0; k <= 14; k++)
                    for (const dir of k ? ways : [Qt.point(0, 0)]) {
                        const c = Qt.point(base.x + dir.x * k * 10, base.y + dir.y * k * 10)
                        if (clear(c, w, h)) {
                            spot = c
                            break search
                        }
                    }
            }
            taken.push({ x: spot.x, y: spot.y, w: w, h: h })
            spots.push(spot)
            looks.push(look)
        }
        return { spots: spots, looks: looks }
    }

    // An angle dimension's look (owner, 2026-10-30): a dashed arc round the corner, from one
    // line to the other, an arrow at each end, dashed lines out from a line's end when the arc
    // is past it (or short of it), and the value in the middle of the arc. Which of the
    // corner's four sides it's on is where it was dragged to (`at`, a sketch place; with
    // sideOnly, just the side), else where it was left (label_u/v: along the first line and the
    // second from the corner), else the side it was made on, towards the lines' middles. Between the arms it shows that
    // side's angle: so the inside of a corner shows the inside angle, and dragged across a
    // line it shows the other one (180° less it). Its radius is how far out it was dragged;
    // left alone, 60% of the way to the nearer middle, 90 to 150 px (plus `grow` px when a
    // value's in the way; never under 50 px). Returns {arc, arrows, ext (screen polylines), spot, shown (the
    // value it shows, radians), flipped (shown = 180° less the stored value), along1, along2}
    // or null when the lines don't cross.
    function angleLook(d, at, grow) {
        const f = d.frame
        if (!f || f.parallel || !sketch.plane)
            return null
        const X = { u: f.cross[0], v: f.cross[1] }, e1 = { u: f.e1[0], v: f.e1[1] }, e2 = { u: f.e2[0], v: f.e2[1] }
        const den = e1.u * e2.v - e1.v * e2.u
        const toPx = (u, v) => { const q = planeToScreen(u, v); return Qt.point(q.x, q.y) }
        const x0 = toPx(X.u, X.v), x1 = toPx(X.u + e1.u * 10, X.v + e1.v * 10)
        const perMm = Math.max(1e-6, Math.hypot(x1.x - x0.x, x1.y - x0.y) / 10)
        const mid = e => ({ u: (e[0] + e[2]) / 2, v: (e[1] + e[3]) / 2 })
        const m1 = mid(f.ends1), m2 = mid(f.ends2)
        const t1 = (m1.u - X.u) * e1.u + (m1.v - X.v) * e1.v, t2 = (m2.u - X.u) * e2.u + (m2.v - X.v) * e2.v
        let s1, s2, r = Math.min(150, Math.max(90, Math.min(Math.abs(t1), Math.abs(t2)) * perMm * 0.6)) / perMm
        // label_u/v without `placed`: only which side it was made on (askDimension), not where.
        const w = at ? { u: at.u - X.u, v: at.v - X.v }
                : d.label_u || d.label_v ? { u: e1.u * d.label_u + e2.u * d.label_v, v: e1.v * d.label_u + e2.v * d.label_v } : null
        if (w && Math.hypot(w.u, w.v) > 1e-9) {
            const a = (w.u * e2.v - w.v * e2.u) / den, b = (e1.u * w.v - e1.v * w.u) / den
            s1 = a < 0 ? -1 : 1
            s2 = b < 0 ? -1 : 1
            if (at ? !at.sideOnly : d.placed)
                r = Math.hypot(w.u, w.v)
        } else if (Math.abs(t1) >= Math.abs(t2)) {      // towards the middle of the line further out
            s1 = t1 < 0 ? -1 : 1
            s2 = s1 * d.side
        } else {
            s2 = t2 < 0 ? -1 : 1
            s1 = s2 * d.side
        }
        r = Math.max(50 / perMm, r + (grow || 0) / perMm)
        const a1 = { u: e1.u * s1, v: e1.v * s1 }, a2 = { u: e2.u * s2, v: e2.v * s2 }
        const from = Math.atan2(a1.v, a1.u)
        const sweep = Math.atan2(a1.u * a2.v - a1.v * a2.u, a1.u * a2.u + a1.v * a2.v)
        const arc = []
        for (let i = 0; i <= 40; i++) {
            const t = from + sweep * i / 40
            arc.push(toPx(X.u + r * Math.cos(t), X.v + r * Math.sin(t)))
        }
        const arrow = (tip, back) => {
            const l = Math.hypot(tip.x - back.x, tip.y - back.y) || 1
            const dx = (tip.x - back.x) / l, dy = (tip.y - back.y) / l
            const w1 = Qt.point(tip.x - dx * 8 - dy * 3.5, tip.y - dy * 8 + dx * 3.5)
            const w2 = Qt.point(tip.x - dx * 8 + dy * 3.5, tip.y - dy * 8 - dx * 3.5)
            return [tip, w1, w2, tip]
        }
        // From a line's nearer end to just past the arc, when the arc isn't on the line itself.
        const ext = []
        for (const [ends, arm, axis] of [[f.ends1, a1, f.axis1], [f.ends2, a2, f.axis2]]) {
            if (axis)
                continue
            const ta = (ends[0] - X.u) * arm.u + (ends[1] - X.v) * arm.v, tb = (ends[2] - X.u) * arm.u + (ends[3] - X.v) * arm.v
            const lo = Math.min(ta, tb), hi = Math.max(ta, tb), over = 3 / perMm
            const run = r > hi ? [hi, r + over] : r < lo ? [r - over, lo] : null
            if (run && run[1] - run[0] > over * 1.5)
                ext.push([toPx(X.u + arm.u * run[0], X.v + arm.v * run[0]), toPx(X.u + arm.u * run[1], X.v + arm.v * run[1])])
        }
        const flipped = s1 * s2 !== d.side
        const half = from + sweep / 2
        const along = { u: X.u + r * Math.cos(half) - X.u, v: X.v + r * Math.sin(half) - X.v }
        return {
            arc: arc, ext: ext,
            arrows: [arrow(arc[0], arc[1]), arrow(arc[40], arc[39])],
            spot: toPx(X.u + along.u, X.v + along.v),
            shown: flipped ? Math.PI - d.value : d.value, flipped: flipped,
            along1: (along.u * e2.v - along.v * e2.u) / den, along2: (e1.u * along.v - e1.v * along.u) / den
        }
    }

    // Fusion's "Perspective with Ortho Faces", in Sketch mode only (owner, 2026-10-30): while
    // the view looks straight at the open sketch (as it opens, and through any pan or zoom)
    // there's no perspective, so zooming never seems to tilt it; turned off square (the cube,
    // orbiting) it's perspective again, and flat again once square. Within about 0.8°.
    readonly property bool sketchOrtho: {
        viewTick
        if (!sketch.active || !sketch.plane)
            return false
        const n = sketch.plane.normal, f = camera.forward
        return Math.abs(f.x * n.x + f.y * n.z - f.z * n.y) > 0.9999      // engine → scene: (x, z, -y)
    }

    // The open sketch's dimensions, and a tick whenever the view moves (their labels follow).
    property var dimensions: []
    property int viewTick: 0
    function refreshDimensions() {
        dimensions = sketch.active ? doc.sketchDimensions(sketch.sketchId) : []
    }
    Connections {
        target: camera
        function onScenePositionChanged() { root.viewTick++ }
        function onSceneRotationChanged() { root.viewTick++ }
    }
    Connections {
        target: sketchTools
        function onDimensionWanted(type, first, second, otherSide) { root.askDimension(type, first, second, otherSide) }
        function onConstraintWanted(type, first, second, third, keep) {
            const why = doc.addSketchConstraint(sketch.sketchId, type, first, second, third, keep)
            sketch.say(why)
        }
    }

    // Fusion's Save / Save As. Save asks for a name only the first time.
    function save() {
        if (!doc.filePath)
            return saveAs()
        const why = doc.saveTo(doc.filePath)
        sketch.say(why ? why : "Saved " + doc.filePath.split("/").pop())
    }
    function saveAs() {
        saveDialog.currentFolder = doc.filePath ? "file://" + doc.filePath.substring(0, doc.filePath.lastIndexOf("/")) : saveFolder
        saveDialog.selectedFile = saveDialog.currentFolder + "/" + (doc.filePath ? doc.filePath.split("/").pop() : "Untitled.omamake")
        dialogLook.setDark(!theme.light)
        saveDialog.open()
    }
    FileDialog {
        id: saveDialog
        title: "Save As"
        fileMode: FileDialog.SaveFile
        nameFilters: ["OmaMake designs (*.omamake)"]
        defaultSuffix: "omamake"
        onAccepted: {
            const why = doc.saveTo(selectedFile.toString())
            sketch.say(why ? why : "Saved " + doc.filePath.split("/").pop())
        }
    }
    // Load (Fusion's Open; owner, 2026-10-30: Load and Save, "open" still finds it): into this
    // window while it holds nothing yet, else a new window (so nothing here is lost, like
    // Fusion's new tab).
    function load() {
        loadDialog.currentFolder = doc.filePath ? "file://" + doc.filePath.substring(0, doc.filePath.lastIndexOf("/")) : saveFolder
        dialogLook.setDark(!theme.light)
        loadDialog.open()
    }
    function loadFile(url) {
        if (!doc.untouched()) {
            doc.openInNewWindow(url)
            return
        }
        sketch.picking = false
        sketchTools.cancel()
        selection.clear()
        const why = doc.openFrom(url)
        if (why)
            return sketch.say(why)
        navigation.home()
        sketch.say("Loaded " + doc.filePath.split("/").pop())
    }
    FileDialog {
        id: loadDialog
        title: "Load"
        fileMode: FileDialog.OpenFile
        nameFilters: ["OmaMake designs (*.omamake)"]
        onAccepted: root.loadFile(selectedFile.toString())
    }
    // Export (owner, 2026-10-31): STEP for now, the first of the formats it will offer.
    function exportBody() {
        exportDialog.currentFolder = doc.filePath ? "file://" + doc.filePath.substring(0, doc.filePath.lastIndexOf("/")) : saveFolder
        exportDialog.selectedFile = exportDialog.currentFolder + "/" + doc.exportName() + ".step"
        dialogLook.setDark(!theme.light)
        exportDialog.open()
    }
    FileDialog {
        id: exportDialog
        title: "Export"
        fileMode: FileDialog.SaveFile
        nameFilters: ["STEP files (*.step *.stp)"]
        defaultSuffix: "step"
        onAccepted: {
            const why = doc.exportTo(selectedFile.toString())
            sketch.say(why ? why : "Exported " + selectedFile.toString().split("/").pop())
        }
    }
    // Import STEP and Mesh (owner, 2026-10-30): reads in the background, the panel below shows how far
    // it's got and for how long; the view flies to the model when it lands.
    FileDialog {
        id: stepDialog
        title: "Import STEP"
        fileMode: FileDialog.OpenFile
        nameFilters: ["STEP files (*.step *.stp *.STEP *.STP)"]
        onAccepted: doc.importFile(selectedFile.toString())
    }
    FileDialog {
        id: meshDialog
        title: "Import Mesh"
        fileMode: FileDialog.OpenFile
        nameFilters: ["Mesh files (*.stl *.obj *.STL *.OBJ)"]
        onAccepted: doc.importFile(selectedFile.toString())
    }
    function importFrom(dialog) {
        dialog.currentFolder = doc.filePath ? "file://" + doc.filePath.substring(0, doc.filePath.lastIndexOf("/")) : saveFolder
        dialogLook.setDark(!theme.light)
        dialog.open()
    }
    Connections {
        target: doc
        function onImportChanged() {
            if (doc.importing) {
                importPanel.started = Date.now()
                importPanel.elapsed = 0
            }
        }
        function onFitFinished(why, seconds, cylinders, cones, spheres, trianglesLeft, worstMm) {
            if (why)
                return sketch.say(why)
            const made = [[cylinders, "cylinder"], [cones, "cone"], [spheres, "sphere"]].filter(k => k[0]).map(k => k[0] + " " + k[1] + (k[0] > 1 ? "s" : ""))
            sketch.say("Fitted " + made.join(", ") + " in " + seconds.toFixed(1) + " s · at most " + worstMm.toFixed(3) + " mm off · " + trianglesLeft.toLocaleString(Qt.locale(), "f", 0) + " triangles left as facets")
        }
        function onImportFinished(why, seconds, faces, triangles) {
            if (why)
                return sketch.say(why)
            navigation.fit()
            sketch.say("Imported " + doc.importName + " in " + seconds.toFixed(1) + " s · " + faces.toLocaleString(Qt.locale(), "f", 0) + " faces, " + triangles.toLocaleString(Qt.locale(), "f", 0) + " triangles")
        }
    }
    // Over everything, and takes the mouse, while the model is on its way.
    MouseArea {
        anchors.fill: parent
        z: 1000
        visible: doc.importing
        hoverEnabled: true
        acceptedButtons: Qt.AllButtons
        onWheel: wheel => wheel.accepted = true
        Rectangle {
            id: importPanel
            property double started: 0
            property real elapsed: 0
            objectName: "importPanel"
            anchors.centerIn: parent
            width: 380
            height: 92
            radius: 14
            color: Qt.rgba(theme.panelBg.r, theme.panelBg.g, theme.panelBg.b, 0.92)
            Timer {
                interval: 100
                repeat: true
                running: doc.importing
                onTriggered: importPanel.elapsed = (Date.now() - importPanel.started) / 1000
            }
            Text {
                x: 20; y: 14; width: parent.width - 100
                text: doc.importName
                elide: Text.ElideMiddle
                color: theme.textPrimary
                font.pixelSize: 15
                font.bold: true
            }
            Text {
                objectName: "importTime"
                x: parent.width - 90; y: 14; width: 70
                horizontalAlignment: Text.AlignRight
                text: importPanel.elapsed.toFixed(1) + " s"
                color: theme.textPrimary
                font.pixelSize: 15
                font.features: { "tnum": 1 }
            }
            Text {
                objectName: "importStage"
                x: 20; y: 38
                text: doc.importStage
                color: theme.textPrimary
                opacity: 0.7
                font.pixelSize: 13
            }
            Rectangle {
                x: 20; y: 66; width: parent.width - 40; height: 4; radius: 2
                clip: true
                color: Qt.rgba(theme.textPrimary.r, theme.textPrimary.g, theme.textPrimary.b, 0.15)
                Rectangle {
                    width: 90; height: 4; radius: 2
                    color: theme.selection
                    NumberAnimation on x {
                        from: -90; to: 340; duration: 1400
                        loops: Animation.Infinite
                        running: doc.importing
                    }
                }
            }
        }
    }
    Shortcut {
        sequence: "Ctrl+O"
        enabled: !search.open && !dimInput.open
        onActivated: root.load()
    }
    Shortcut {
        sequence: "Ctrl+S"
        enabled: !search.open && !dimInput.open
        onActivated: root.save()
    }
    Shortcut {
        sequence: "Ctrl+Shift+S"
        enabled: !search.open && !dimInput.open
        onActivated: root.saveAs()
    }

    // Toggle Construction (Fusion's Normal/Construction; X or its sketch bar button, owner
    // 2026-10-30): the selected lines, arcs, circles and curves all turn construction, a mixed
    // selection too (or all back to normal, when they all are); with nothing selected, or while a
    // tool is on, it flips the Construction switch for what's drawn next.
    function toggleConstruction() {
        const parts = sketchTools.tool ? []
                    : selection.selected.filter(p => ["sketch_line", "sketch_arc", "sketch_curve"].includes(p.kind))
        if (parts.length) {
            doc.toggleSketchConstruction(sketch.sketchId, parts)
            return
        }
        sketchTools.construction = !sketchTools.construction
        sketch.say(sketchTools.construction ? "What you draw now is construction (X again for normal)" : "What you draw now is normal again")
    }

    // Find Profile Gaps (owner, 2026-10-30; not in Fusion): one click rings in red, for 10 s,
    // every place two lines come within 1 mm without being joined (profiles.h), kept up to date
    // as lines are drawn and moved meanwhile. Esc, or leaving the sketch, takes them away
    // sooner; clicking again starts the 10 s again. Not a switch (owner). The sketch bar's
    // stepper shows how many, and its arrows fly the view to each in turn: once stepping, the
    // rings stay until Esc (owner, round 19).
    property bool gapsOn: false                                 // the rings are showing
    property int gapAt: -1                                      // the gap stepped to, -1 before stepping
    property bool gapsFading: false                             // ...or still fading out
    readonly property real gapLimit: 1.0                        // mm
    property int sketchTick: 0                                  // counts sketch changes
    readonly property var profileCheck: { sketchTick; return (gapsOn || gapsFading) && sketch.active ? doc.sketchProfiles(sketch.sketchId, gapLimit)
                                                                                                    : { profiles: [], gaps: [] } }
    readonly property int gapCount: profileCheck.gaps.length
    onGapCountChanged: {                                        // a gap closed or opened
        if (gapAt >= gapCount)
            gapAt = gapCount - 1
        if (gapsOn && gapCount === 0)
            gapsTimer.restart()                                 // all closed: end in 10 s as usual
        if (gapsOn)
            tellGaps()
    }
    Timer { id: gapsTimer; interval: 10000; onTriggered: root.hideGaps() }
    function showGaps() {
        gapsOn = true
        gapsFading = false
        if (gapAt < 0)                                          // stepping: no timer
            gapsTimer.restart()
        tellGaps()
    }
    // Fly to the next (by +1) or previous (-1) gap, keeping the view's direction, close enough
    // that the gap shows clearly inside its ring. Stops the 10 s.
    function stepGap(by) {
        const n = gapCount
        if (!gapsOn || n === 0)
            return
        gapsTimer.stop()
        gapAt = gapAt < 0 ? (by > 0 ? 0 : n - 1) : (gapAt + by + n) % n
        const g = profileCheck.gaps[gapAt], a = g.a.at, b = g.b.at
        const radius = Math.max(3, g.size * 10)                 // mm round the gap
        navigation.restoreView({ yaw: navigation.yaw, pitch: navigation.pitch,
                                 distance: radius / Math.sin(navigation.fov * Math.PI / 360) * 1.6,
                                 center: Qt.vector3d((a.x + b.x) / 2, (a.z + b.z) / 2, -(a.y + b.y) / 2) })
    }
    function hideGaps() {
        if (!gapsOn)
            return
        gapsTimer.stop()
        gapAt = -1
        gapsFading = true
        gapsOn = false
    }
    // Gaps found: the stepper says how many. None: the guide says so.
    function tellGaps() {
        const closed = profileCheck.profiles.length
        if (!gapCount)
            sketch.say("No gaps under 1 mm · " + closed + (closed === 1 ? " closed profile" : " closed profiles"))
    }

    function fixSelected() {
        const points = sketchTools.selectedPoints()
        if (!points.length) {
            sketchTools.use("fix")
            sketch.say("Click points or lines to fix them, or to let them go.")
            return
        }
        doc.fixSketchPoints(sketch.sketchId, points)
    }

    // The lines and points drawn in sketches: every shown sketch. The open one too unless its
    // eye hid it (owner, 2026-10-30: hidden to see the model behind it while still in it); then
    // its values and constraint badges go too, and nothing of it can be picked. Tools still
    // work, and what they draw stays hidden with it until its eye shows it again.
    // Their thickness stays the same on screen at any zoom.
    readonly property bool openSketchShown: sketch.active && visibleKeys["s" + sketch.sketchId] !== false
    readonly property string hiddenNote: "This sketch is hidden; you're still in it. Its eye in the Browser shows it again."
    onOpenSketchShownChanged: {
        if (sketch.active && !openSketchShown) {
            selection.clear()
            sketch.say(hiddenNote)
        } else if (sketch.message === hiddenNote) {
            sketch.say("")
        }
    }
    // Every shown sketch's closed profiles, lightly shaded (Fusion's profile shading): only when
    // a sketch changes or is shown or hidden, never on a zoom.
    function drawProfiles() {
        const ids = []
        for (const k in visibleKeys)
            if (k[0] === "s" && visibleKeys[k])
                ids.push(parseInt(k.slice(1)))
        doc.fillProfiles(sketchProfiles, ids)
        // Picking: every shown sketch's profiles; while sketching, the open sketch's only.
        selection.profileSketches = sketch.active ? (openSketchShown ? [sketch.sketchId] : []) : ids
    }
    function drawSketches() {
        const ids = []
        for (const k in visibleKeys)
            if (k[0] === "s" && visibleKeys[k])
                ids.push(parseInt(k.slice(1)))
        const radius = navigation.distance * selection.perPixel() * 1.1
        doc.fillSketches(sketchDrawn, sketchFull, sketchConstruction, sketchConstructionFull, sketchProjected, ids, radius)
        doc.fillSegments(sketchPreview, sketchTools.preview, radius, sketchTools.construction)
    }
    Connections {
        target: doc
        function onSketchesChanged() {
            root.sketchTick++
            selection.keepIn(sketch.active ? doc.sketchGeometry(sketch.sketchId) : { lines: [], points: [] })
            root.drawSketches()
            root.drawProfiles()
            root.refreshDimensions()
        }
    }
    onVisibleKeysChanged: { drawSketches(); drawProfiles() }
    Connections {
        target: navigation
        function onDistanceChanged() { root.drawSketches() }
    }
    Connections {
        target: sketchTools
        function onPreviewChanged() { doc.fillSegments(sketchPreview, sketchTools.preview, navigation.distance * selection.perPixel() * 1.1, sketchTools.construction) }
        function onConstructionChanged() { onPreviewChanged() }
    }
    Connections {
        target: sketch
        function onActiveChanged() { root.drawSketches(); root.drawProfiles(); root.refreshDimensions(); dimInput.close() }
    }

    SketchBar {
        anchors.top: parent.top
        anchors.topMargin: 14
        anchors.horizontalCenter: parent.horizontalCenter
        theme: theme
        sketch: sketch
        toolName: sketchTools.toolName
        toolHint: (search.commands.find(c => c.name === sketchTools.toolName) || {}).hint || ""
        construction: sketchTools.construction
        onToggleConstruction: root.toggleConstruction()
        gaps: root.gapsOn
        gapCount: root.gapCount
        gapAt: root.gapAt
        gapNote: root.profileCheck.profiles.length + (root.profileCheck.profiles.length === 1 ? " closed profile" : " closed profiles")
        onFindGaps: root.showGaps()
        onStepGap: by => root.stepGap(by)
    }

    // The tool's options (ToolOptions), under the sketch bar.
    ToolOptions {
        id: toolOptions
        anchors.top: parent.top
        anchors.topMargin: 14 + 116 + 6
        anchors.horizontalCenter: parent.horizontalCenter
        z: 9
        theme: theme
        tool: sketchTools.shape
        fields: sketchTools.shapeDef && sketchTools.shapeDef.options ? sketchTools.shapeDef.options : []
        values: sketchTools.optionValues[sketchTools.shape] || ({})
        onChanged: (key, value) => sketchTools.setOption(key, value)
        onConfirmed: sketchTools.confirm()
        onCancelled: sketchTools.cancel()
        onUndo: root.undoLast()
    }
    // Project's two choices, where a tool's options go.
    Row {
        anchors.top: parent.top
        anchors.topMargin: 14 + 116 + 6
        anchors.horizontalCenter: parent.horizontalCenter
        z: 9
        spacing: 8
        visible: sketchTools.tool === "project"
        Repeater {
            model: [{ key: "model", label: "Whole Model" }, { key: "face", label: "Face" }]
            delegate: Rectangle {
                required property var modelData
                readonly property bool on: sketchTools.projectWhat === modelData.key
                width: 110
                height: 34
                radius: 8
                color: Qt.rgba(theme.panelBg.r, theme.panelBg.g, theme.panelBg.b, 0.92)
                border.color: on ? theme.accent : theme.border
                border.width: 1.5
                Text {
                    anchors.centerIn: parent
                    text: parent.modelData.label
                    color: parent.on ? theme.textPrimary : theme.textSecondary
                    font.pixelSize: 12
                }
                MouseArea {
                    anchors.fill: parent
                    cursorShape: Qt.PointingHandCursor
                    onClicked: { sketchTools.projectWhat = parent.modelData.key; selection.clearHover() }
                }
            }
        }
    }
    // Enter: a tool gathering things (Mirror, Offset, a spline's points…) carries on.
    Shortcut {
        sequences: ["Return", "Enter"]
        enabled: sketchTools.tool === "shape" && !search.open && !dimInput.open && !toolOptions.typing
        onActivated: sketchTools.confirm()
    }

    ViewCube {
        id: viewCube
        anchors.top: parent.top
        anchors.topMargin: 26
        anchors.right: parent.right
        anchors.rightMargin: 16
        theme: theme
        nav: navigation
    }

    HandButton {
        anchors.right: viewCube.left
        anchors.rightMargin: -4
        anchors.top: viewCube.top
        anchors.topMargin: 4
        theme: theme
        nav: navigation
    }

    ModeButton {
        id: modeButton
        anchors.top: parent.top
        anchors.right: parent.right
        anchors.margins: 10
        theme: theme
        onMenuRequested: modeMenu.show()
    }

    Connections {
        target: doc
        function onChanged() { doc.fill(faces, edges) }
    }

    Component.onCompleted: {
        doc.fill(faces, edges)
        navigation.home()
        if (openNote)
            sketch.say(openNote)
    }

    CommandSearch {
        id: search
        theme: theme
        families: root.familiesNow
        onUsed: command => root.noteUse(command)
        // Things you DO, not ways to look around: view moves stay on the mouse and the
        // view cube (owner, 2026-10-29). Each command says which modes it shows in (Model,
        // Sketch); onlyOmarchy commands are left out on other systems. Names are Fusion's
        // exact ones wherever Fusion has the function (history/DUNE-VS-FUSION.md).
        commands: [
            { name: theme.light ? "Dark Mode" : "Light Mode", words: "dark light theme colours", hint: "Switch the look", group: "Look", modes: both, keys: "", run: () => theme.setLight(!theme.light) },
            { name: "Follow Omarchy Theme", words: "colours", onlyOmarchy: true, hint: theme.followSystem ? "Back to OmaMake's own colours" : "Take colours from the current Omarchy theme", group: "Look", modes: both, keys: "", run: () => theme.followSystem = !theme.followSystem },
            { name: "Light Follows View", hint: root.lightFollowsView ? "On: the lamp turns with you. Turn off to keep it fixed in the room" : "Off: the lamp stays put in the room. Turn on to have it turn with you", group: "Look", modes: ["model"], keys: "", run: () => root.lightFollowsView = !root.lightFollowsView },
            { name: "Grid", hint: navigation.gridVisible ? "Hide the floor grid" : "Show the floor grid", group: "Look", modes: ["model"], keys: "", run: () => navigation.gridVisible = !navigation.gridVisible },
            { name: "Simple Look", words: "fast performance effects", hint: navigation.simpleLook ? "Back to full shading and shadows" : "Fewer effects, for slower computers", group: "Look", modes: ["model"], keys: "", run: () => navigation.simpleLook = !navigation.simpleLook },
            { name: "New Component", hint: "An empty component inside the active one, made active", group: "Assemble", modes: ["model"], keys: "", run: () => doc.newComponent() },
            { name: "Browser", hint: browserTree.shown ? "Hide the design tree" : "Show the design tree", group: "Look", modes: both, keys: "", run: () => browserTree.shown = !browserTree.shown },
            { name: "Origin Planes: Cross", hint: "All planes full size, through the centre (how OmaMake opens)", group: "Look", modes: ["model"], keys: "", run: () => root.setPlaneStyle("", "cross") },
            { name: "Origin Planes: Cube Corner", hint: "All planes smaller, plus side only, apart (how OmaMake opens)", group: "Look", modes: ["model"], keys: "", run: () => root.setPlaneStyle("", "corner") },
            { name: "Create Sketch", hint: "Pick a flat face, or empty space for an origin plane (or triple-click one)", group: "Create", modes: ["model"], keys: "", run: () => sketch.picking = true },
            { name: "Line", letter: "l", hint: "Click the start, then each next point; Esc stops", group: "Create", modes: ["sketch"], keys: "L", run: () => sketchTools.use("line") },
            { name: "2-Point Rectangle", hint: "Click one corner, then the opposite one", group: "Create", modes: ["sketch"], keys: "", run: () => sketchTools.use("rectangle") },
            { name: "Project", letter: "p", words: "project geometry include silhouette outline profile shadow", hint: "Pick Whole Model or Face above, then click the model", group: "Create", modes: ["sketch"], keys: "", run: () => sketchTools.use("project") },
            { name: "Center Diameter Circle", hint: "Click the centre, then the edge", group: "Create", modes: ["sketch"], keys: "", run: () => sketchTools.use("circle") },
            { name: "Center Point Arc", words: "arc", hint: "Click the centre, where it starts, then where it ends", group: "Create", modes: ["sketch"], keys: "", run: () => sketchTools.use("arc") },
            { name: "Tangent", hint: "Click a line, arc or circle, then an arc or circle: they touch smoothly", group: "Constraints", modes: ["sketch"], keys: "", run: () => root.constrain("tangent") },
            { name: "Concentric", hint: "Click two arcs or circles: they share a centre", group: "Constraints", modes: ["sketch"], keys: "", run: () => root.constrain("concentric") },
            // Fillet right after the basic shapes, so "fi" finds it before Fix/UnFix (owner: easy to find).
            { name: "Fillet", letter: "f", words: Shapes.tools.fillet.words, hint: Shapes.tools.fillet.hint, group: "Modify", modes: ["sketch"], keys: "", run: () => sketchTools.use("fillet") },
            { name: "Parallel", hint: "Click two lines (or select them first): the second turns to match the first", group: "Constraints", modes: ["sketch"], keys: "", run: () => root.constrain("parallel") },
            { name: "Perpendicular", hint: "Click two lines (or select them first): the second turns square to the first", group: "Constraints", modes: ["sketch"], keys: "", run: () => root.constrain("perpendicular") },
            { name: "Sketch Dimension", letter: "d", hint: "A line's length, an arc's radius, a circle's diameter, the distance between two points, a point and a line, or two parallel lines, or the angle between two lines; inches unless you type a unit", group: "Create", modes: ["sketch"], keys: "D", run: () => root.dimensionSelected() },
            { name: "Equal", letter: "e", hint: "Click two lines (or two arcs or circles): the second matches the first", group: "Constraints", modes: ["sketch"], keys: "", run: () => root.constrain("equal") },
            { name: "Horizontal/Vertical", letter: "h", hint: "Click a line to make it level or upright (whichever it's nearer), or two points to line them up", group: "Constraints", modes: ["sketch"], keys: "", run: () => root.constrain("hv") },
            { name: "Coincident", hint: "Click a point, then a point to join it to, or a line, arc or circle to put it on", group: "Constraints", modes: ["sketch"], keys: "", run: () => root.constrain("coincident") },
            { name: "Midpoint", letter: "m", hint: "Click a point and a line (either first): the point goes to the line's middle", group: "Constraints", modes: ["sketch"], keys: "", run: () => root.constrain("midpoint") },
            { name: "Collinear", hint: "Click two lines (or select them first): the second lines up along the first", group: "Constraints", modes: ["sketch"], keys: "", run: () => root.constrain("collinear") },
            { name: "Symmetry", hint: "Click two points or two lines, then the line to mirror them across", group: "Constraints", modes: ["sketch"], keys: "", run: () => root.constrain("symmetry") },
            { name: "Toggle Construction", letter: "x", words: "construction normal reference guide dashed", hint: "Turn the selected lines construction or back; with nothing selected or while drawing, what you draw next", group: "Sketch", modes: ["sketch"], keys: "X", keepsTool: true, run: () => root.toggleConstruction() },
            { name: "Find Profile Gaps", words: "gap gaps break broken open leak profile closed close not closing", hint: "Rings in red, for 10 s (Esc sooner), where lines come within 1 mm without being joined", group: "Sketch", modes: ["sketch"], keys: "", keepsTool: true, run: () => root.showGaps() },
            { name: "Fix/UnFix", hint: "Click points or lines (or select them first) to hold them where they are, or let them go", group: "Constraints", modes: ["sketch"], keys: "", run: () => root.fixSelected() },
            // Every other Fusion sketch tool (ShapeTools.js), in Fusion's menu order.
            ...Shapes.order.filter(key => key !== "fillet").map(key => {
                const t = Shapes.tools[key]
                return { name: t.name, letter: ({ fillet: "f", offset: "o", trim: "t" })[key], words: t.words, hint: t.hint,
                         group: t.group, modes: ["sketch"], keys: "", run: () => sketchTools.use(key) }
            }),
            { name: "Finish Sketch", hint: "Back to the model", group: "Sketch", modes: ["sketch"], keys: "", run: () => sketch.finish() },
            { name: "Load", words: "open file design", hint: "Load an .omamake design (in a new window if this one holds work)", group: "File", modes: both, keys: "Ctrl O", run: () => root.load() },
            { name: "Save", hint: doc.filePath ? "Save to " + doc.filePath.split("/").pop() : "Save the design as an .omamake file", group: "File", modes: both, keys: "Ctrl S", run: () => root.save() },
            { name: "Import STEP", words: "insert open stp step cad solid model file", hint: "Bring in a STEP file as a solid", group: "File", modes: ["model"], keys: "", run: () => root.importFrom(stepDialog) },
            { name: "Import Mesh", words: "insert open stl obj mesh triangles 3d print model file", hint: "Bring in an STL or OBJ as a solid, flats merged", group: "File", modes: ["model"], keys: "", run: () => root.importFrom(meshDialog) },
            { name: "Export", words: "save step stp cad file send fusion", hint: "Write the body to a file: STEP for now", group: "File", modes: ["model"], keys: "", run: () => root.exportBody() },
            { name: "Fit Curved Surfaces", words: "facets mesh smooth round cylinder cone sphere convert repair triangles", hint: "Turn facets that follow a curve into true curved surfaces; undoable", group: "Modify", modes: ["model"], keys: "", run: () => doc.fitSurfaces() },
            { name: "Save As", hint: "Save the design under a new name", group: "File", modes: both, keys: "Ctrl Shift S", run: () => root.saveAs() },
            { name: "Quit OmaMake", hint: "Close the window", group: "App", modes: both, keys: "Ctrl Q", run: () => Qt.quit() }
        ].filter(c => (!c.onlyOmarchy || omarchyTheme.onOmarchy) && c.modes.includes(sketch.active ? "sketch" : "model"))
         .map(c => c.modes.includes("model") ? c : SketchKeys.apply(c))   // sketch tools' keys (SketchKeys.js)
        readonly property var both: ["model", "sketch"]
    }

    Shortcut {
        sequence: "Alt+Space"
        onActivated: search.toggle()
    }
    PlaneStyleMenu {
        id: planeMenu
        theme: theme
        styles: root.planeStyles
        onChose: (plane, style) => root.setPlaneStyle(plane, style)
    }
    ModeMenu {
        id: modeMenu
        theme: theme
        anchorItem: modeButton
    }

    // Esc: closes the search or menu first (they handle it), then cancels Create Sketch's
    // pick, then stops panning, then finishes the sketch (same as Finish Sketch, owner
    // 2026-10-29), then clears the selection.
    Shortcut {
        id: escKey
        sequence: "Esc"
        enabled: (sketch.picking || sketch.active || navigation.panMode || selection.selected.length > 0)
                 && !search.open && !modeMenu.open && !planeMenu.open && !dimInput.open
        onActivated: {
            if (sketch.picking)
                sketch.picking = false
            else if (root.gapsOn)
                root.hideGaps()                      // Find Profile Gaps' rings go first
            else if (navigation.panMode)
                navigation.panMode = false
            else if (sketchTools.tool)
                sketchTools.cancel()                 // a drawing tool stops first, then the sketch
            else if (sketchTools.pickedConstraints.length)
                sketchTools.pickedConstraints = []   // then picked constraint badges
            else if (sketch.active)
                sketch.finish()
            else
                selection.clear()
        }
    }
    // Sketch mode's keys (SketchKeys.js; owner, 2026-10-30): every sketch tool's and
    // constraint's `keys`, and each family's (Families.js). A family's key runs its default
    // kind, the same one the search's Enter runs, and counts as a use of it. Only in a sketch,
    // only sketch tools (the search holds nothing else there), and never while a box is being
    // typed in. Ctrl keys (Save, Load…) are their own Shortcuts below.
    readonly property bool sketchKeysOn: sketch.active && !sketch.picking && !search.open && !modeMenu.open
                                         && !planeMenu.open && !dimInput.open && !toolOptions.typing
    readonly property var oneKeyTools: sketch.active ? search.commands.concat(Object.values(search.familyRows))
                                                       .filter(c => c.keys && !c.keys.startsWith("Ctrl")) : []
    Instantiator {
        id: oneKeys
        model: root.oneKeyTools
        delegate: Shortcut {
            required property var modelData
            sequence: SketchKeys.sequence(modelData.keys)
            enabled: root.sketchKeysOn
            onActivated: root.runKey(modelData)
        }
    }
    // Space: the last sketch tool or constraint again (Fusion's Repeat Last Command).
    property string lastSketchTool: ""
    Shortcut {
        id: repeatKey
        sequence: "Space"
        enabled: root.sketchKeysOn && root.lastSketchTool !== ""
        onActivated: {
            const c = search.commands.find(c => c.name === root.lastSketchTool)
            if (c)
                root.runKey(c)
        }
    }
    // S: the command search (Fusion's S opens its shortcuts box; Alt+Space works everywhere).
    Shortcut {
        id: searchKey
        sequence: "S"
        enabled: root.sketchKeysOn
        onActivated: search.toggle()
    }
    function runKey(command) {
        if (sketchTools.tool && !command.keepsTool) {
            sketchTools.cancel()                 // the tool that was on stops first, prompt and all
            sketch.say("")
        }
        const kind = command.family ? command.def : command
        noteUse(kind.name)
        kind.run()
    }
    // Delete (or Backspace): takes the constraints picked on their badges off the sketch; the
    // lines and points stay (owner, 2026-10-30).
    Shortcut {
        sequences: ["Delete", "Backspace"]
        enabled: sketch.active && sketchTools.pickedConstraints.length > 0
                 && !search.open && !modeMenu.open && !planeMenu.open && !dimInput.open
        onActivated: sketchTools.deleteConstraints()
    }
    // Ctrl+Z: take back the newest action in the document's history, in Model and Sketch
    // mode alike, one at a time in the order they were done (owner, 2026-10-29). The
    // window follows: undoing a finish reopens that sketch, undoing a start leaves it.
    function undoLast() {
        sketch.picking = false
        sketchTools.reset()
        const history = doc.history
        const next = history.length ? history[history.length - 1] : null
        if (next && next.type === "visibility") {
            crossfade(() => doc.undo())
            return
        }
        const action = doc.undo()
        if (action.type === "finish_sketch")
            sketch.open(action.sketch, doc.sketchPlane(action.sketch))
        else if (action.type === "sketch" && sketch.active && sketch.sketchId === action.id)
            sketch.leave()
        else if (action.type === "edit_sketch" && sketch.active && sketch.sketchId === action.sketch)
            sketch.leave()
    }

    // Fusion's Edit Sketch, from a double-click on a sketch in the Timeline or the Browser.
    function editSketch(id) {
        sketchTools.cancel()
        dimInput.close()
        sketch.picking = false
        sketch.edit(id)
    }
    Shortcut {
        id: undoKey
        sequence: "Ctrl+Z"
        enabled: !search.open && !modeMenu.open && !planeMenu.open && !dimInput.open && !toolOptions.typing
        onActivated: root.undoLast()
    }
    Shortcut {
        sequence: "Ctrl+Q"
        onActivated: Qt.quit()
    }

    // The history along the bottom (owner, 2026-10-29): the design's, or the open sketch's own.
    // The constraints on what's selected (SketchTools.badges): a point's in a row just below and
    // right of it, a line's in a row beside its middle, on the side that's up on screen.
    Item {
        anchors.fill: parent
        z: 8
        visible: root.openSketchShown
        Repeater {
            model: sketchTools.badges
            delegate: Row {
                id: group
                required property var modelData
                readonly property var plane: sketch.plane
                readonly property var look: theme
                function screen(q) {
                    return root.toScreen(plane.origin.plus(plane.xAxis.times(q.u)).plus(plane.yAxis.times(q.v)))
                }
                readonly property point spot: {
                    root.viewTick
                    root.width
                    if (!plane)
                        return Qt.point(-100, -100)
                    if (modelData.at === "point") {
                        const p = screen(modelData)
                        return Qt.point(p.x + 9, p.y + 7)
                    }
                    const a = screen(modelData.a), b = screen(modelData.b)
                    let nx = -(b.y - a.y), ny = b.x - a.x
                    const len = Math.hypot(nx, ny) || 1
                    if (ny > 0) { nx = -nx; ny = -ny }       // the side that's up on screen
                    const gap = 14 + height / 2
                    return Qt.point((a.x + b.x) / 2 + nx / len * gap - width / 2,
                                    (a.y + b.y) / 2 + ny / len * gap - height / 2)
                }
                x: Math.round(spot.x)
                y: Math.round(spot.y)
                spacing: 3
                Repeater {
                    model: group.modelData.badges
                    delegate: ConstraintBadge {
                        required property var modelData
                        theme: group.look
                        kind: modelData.kind
                        label: modelData.label
                        picked: modelData.picked
                        onClicked: sketchTools.pickConstraint(modelData.id, modelData.label)
                    }
                }
            }
        }
    }
    // The mark beside a spot on a line (SketchTools.glyph): a small triangle at a line's middle
    // (Midpoint), a small ring anywhere else along it (on the line), as the owner asked
    // (2026-10-30). Just above and right of the spot, so the point itself stays in view.
    Canvas {
        id: snapMark
        readonly property var mark: sketchTools.glyph
        readonly property point spot: {
            root.viewTick
            root.width
            const p = sketch.plane
            return mark && p ? root.toScreen(p.origin.plus(p.xAxis.times(mark.u)).plus(p.yAxis.times(mark.v))) : Qt.point(0, 0)
        }
        readonly property string kind: mark ? mark.kind : ""
        readonly property color ink: theme.accent
        visible: kind !== ""
        x: Math.round(spot.x + 7)
        y: Math.round(spot.y - 21)
        z: 9
        width: 14
        height: 14
        onKindChanged: requestPaint()
        onInkChanged: requestPaint()
        onPaint: {
            const c = getContext("2d")
            c.reset()
            c.fillStyle = ink
            c.strokeStyle = ink
            c.lineWidth = 2
            c.beginPath()
            if (kind === "midpoint") {
                c.moveTo(7, 1.5)
                c.lineTo(13, 12.5)
                c.lineTo(1, 12.5)
                c.closePath()
                c.fill()
            } else if (kind === "online") {
                c.arc(7, 7, 4.5, 0, Math.PI * 2)
                c.stroke()
            }
        }
    }
    // What the drawing tool snapped to, beside the mouse ("Midpoint of Line3", "Horizontal"…).
    Rectangle {
        visible: sketchTools.snapLabel !== ""
        x: sketchTools.mouse.x + 16
        y: sketchTools.mouse.y + 18
        z: 9
        width: snapText.implicitWidth + 16
        height: 22
        radius: 11
        color: Qt.rgba(theme.panelBg.r, theme.panelBg.g, theme.panelBg.b, 0.95)
        border.color: theme.accent
        Text {
            id: snapText
            anchors.centerIn: parent
            text: sketchTools.snapLabel
            color: theme.textPrimary
            font.pixelSize: 12
        }
    }
    DimensionInput {
        id: dimInput
        theme: theme
        z: 10
        onCancelled: sketchTools.dimLine = 0
    }

    HistoryTimeline {
        id: historyTimeline
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.bottom: parent.bottom
        width: parent.width - 160
        theme: theme
        history: doc.history
        sketchId: sketch.active ? sketch.sketchId : 0
        onSketchEditRequested: id => root.editSketch(id)
    }

    // A faint hint at the top until the search has been opened 3 times (owner, 2026-10-29:
    // learnt by then). It steps aside for the sketch bar and the search itself, which
    // use the same spot.
    Rectangle {
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.top: parent.top
        anchors.topMargin: 17
        width: hintRow.implicitWidth + 28
        height: 34
        radius: height / 2
        color: Qt.rgba(theme.panelBg.r, theme.panelBg.g, theme.panelBg.b, 0.6)
        border.color: Qt.rgba(theme.border.r, theme.border.g, theme.border.b, 0.6)
        opacity: search.timesOpened >= 3 || search.open || sketch.active || sketch.picking ? 0 : 0.85
        visible: opacity > 0
        Behavior on opacity { NumberAnimation { duration: 400 } }

        Row {
            id: hintRow
            anchors.centerIn: parent
            spacing: 8
            KeyCap { theme: theme; text: "Alt" }
            KeyCap { theme: theme; text: "Space" }
            Text {
                anchors.verticalCenter: parent.verticalCenter
                text: "search every command"
                color: theme.textSecondary
                font.pixelSize: 13
            }
        }
    }
}
