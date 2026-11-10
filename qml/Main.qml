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
        if (sketch.active && !["Toggle Construction", "Finish Sketch"].includes(command) && !command.startsWith("Show ")
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
    // The model is drawn and picked as one mesh of the bodies that are shown (their eyes, and a
    // past sketch's rewind): the document joins those; it's told whenever that changes.
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
                visible: root.bodyVisible && !rewind.showing && !rewind.framesShowing && !edgePreviewShown

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

            // Chamfer's preview: the model as the chamfer would leave it, in place of the model (ChamferTool).
            Node {
                visible: root.bodyVisible && !rewind.showing && !rewind.framesShowing && edgePreviewShown
                Model {
                    objectName: "chamferPreview"
                    geometry: MeshGeometry { id: chamferFaces }
                    materials: PrincipledMaterial {
                        baseColor: theme.model
                        metalness: navigation.simpleLook ? 0.08 : 0.85
                        roughness: navigation.simpleLook ? 0.42 : 0.28
                    }
                    castsShadows: true
                    receivesShadows: true
                }
                Model {
                    geometry: MeshGeometry { id: chamferEdges; lines: true }
                    depthBias: -40
                    materials: PrincipledMaterial {
                        lighting: PrincipledMaterial.NoLighting
                        baseColor: theme.modelEdge
                    }
                }
            }

            // The Timeline's rewind across an Extrude, Chamfer or Fillet (GB-057, GB-058): the model with the step
            // grown, as real solids, to where the marker is (Document::rewindFramesShow's frame), in place of the model.
            Node {
                id: rewindFrameNode
                visible: rewind.framesShowing     // its own bodies' eyes are in the frame (a first body isn't there yet)
                Model {
                    objectName: "rewindFrameModel"
                    geometry: MeshGeometry { id: rewindFrameFaces }
                    materials: PrincipledMaterial {
                        baseColor: theme.model
                        metalness: navigation.simpleLook ? 0.08 : 0.85
                        roughness: navigation.simpleLook ? 0.42 : 0.28
                    }
                    castsShadows: true
                    receivesShadows: true
                }
                Model {
                    geometry: MeshGeometry { id: rewindFrameEdges; lines: true }
                    depthBias: -40
                    materials: PrincipledMaterial {
                        lighting: PrincipledMaterial.NoLighting
                        baseColor: theme.modelEdge
                    }
                }
            }

            // The Timeline's rewind (owner, 2026-11-07): while the marker is part way across an extrude,
            // the design before it (the body's own geometry, which is the design now) and after it
            // (rewind's afterFaces), each cut where the rewind has got to (RewindMaterial).
            Node {
                visible: rewind.showing
                Model {
                    geometry: faces
                    materials: RewindMaterial {
                        keepInside: 0
                        origin: rewind.origin; axis: rewind.axis; low: rewind.low; high: rewind.high; glowWidth: rewind.glowWidth
                        xDir: rewind.xDir; yDir: rewind.yDir; box: rewind.box
                        tint: theme.model; glow: theme.accent
                        metal: navigation.simpleLook ? 0.08 : 0.85
                        rough: navigation.simpleLook ? 0.42 : 0.28
                    }
                }
                Model {
                    geometry: edges
                    depthBias: -40
                    materials: RewindMaterial {
                        lines: true; keepInside: 0
                        origin: rewind.origin; axis: rewind.axis; low: rewind.low; high: rewind.high; glowWidth: rewind.glowWidth
                        xDir: rewind.xDir; yDir: rewind.yDir; box: rewind.box
                        ink: theme.modelEdge; glow: theme.accent
                    }
                }
                Model {
                    geometry: MeshGeometry { id: afterFaces }
                    materials: RewindMaterial {
                        keepInside: 1
                        origin: rewind.origin; axis: rewind.axis; low: rewind.low; high: rewind.high; glowWidth: rewind.glowWidth
                        xDir: rewind.xDir; yDir: rewind.yDir; box: rewind.box
                        tint: theme.model; glow: theme.accent
                        metal: navigation.simpleLook ? 0.08 : 0.85
                        rough: navigation.simpleLook ? 0.42 : 0.28
                    }
                }
                Model {
                    geometry: MeshGeometry { id: afterEdges; lines: true }
                    depthBias: -40
                    materials: RewindMaterial {
                        lines: true; keepInside: 1
                        origin: rewind.origin; axis: rewind.axis; low: rewind.low; high: rewind.high; glowWidth: rewind.glowWidth
                        xDir: rewind.xDir; yDir: rewind.yDir; box: rewind.box
                        ink: theme.modelEdge; glow: theme.accent
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
            // What Project has picked so far (Selection.drawProject): faces shaded translucent purple,
            // edges and corners as thick lines drawn in front (owner, 2026-11-02).
            Model {
                objectName: "projectFaces"
                geometry: MeshGeometry { id: projectFaceParts }
                depthBias: -50
                castsShadows: false
                receivesShadows: false
                materials: PrincipledMaterial {
                    lighting: PrincipledMaterial.NoLighting
                    baseColor: Qt.rgba(0.62, 0.30, 0.95, 0.42)
                    alphaMode: PrincipledMaterial.Blend
                    cullMode: Material.NoCulling
                    depthDrawMode: Material.NeverDepthDraw
                }
            }
            Model {
                objectName: "projectEdges"
                geometry: MeshGeometry { id: projectEdgeParts }
                depthBias: -400
                castsShadows: false
                receivesShadows: false
                materials: PrincipledMaterial {
                    lighting: PrincipledMaterial.NoLighting
                    baseColor: theme.selection
                }
            }
            OriginShapes {
                id: originShapes
                theme: theme
                shown: root.originVisible
                styles: root.planeStyles
                size: 150         // fixed, half-width in mm (owner, 2026-11-08: it followed the model and resized while scrubbing)
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
                materials: ProfileMaterial { tint: Qt.rgba(theme.hover.r, theme.hover.g, theme.hover.b, 0.30) }
            }
            Model {
                objectName: "profileSelected"
                geometry: MeshGeometry { id: profileSelectedParts }
                depthBias: -25
                castsShadows: false
                receivesShadows: false
                materials: ProfileMaterial { tint: Qt.rgba(theme.selection.r, theme.selection.g, theme.selection.b, 0.40) }
            }
            // Extrude's preview per side: a faint fill, an outline (blue adds, red cuts), and the exact red region a cut removes.
            Model {
                objectName: "extrudePreviewA"
                geometry: MeshGeometry { id: extrudePreviewA }
                castsShadows: false
                receivesShadows: false
                materials: XrayMaterial { tint: extrudeTool.fillOf(0) }
            }
            Model {
                objectName: "extrudeWireA"
                geometry: MeshGeometry { id: extrudeWireA; lines: true }
                castsShadows: false
                receivesShadows: false
                materials: XrayMaterial { tint: extrudeTool.lineOf(0) }
            }
            Model {
                objectName: "extrudeCutA"
                geometry: MeshGeometry { id: extrudeCutA }
                castsShadows: false
                receivesShadows: false
                materials: XrayMaterial { tint: extrudeTool.cutColor }
            }
            Model {
                objectName: "extrudePreviewB"
                geometry: MeshGeometry { id: extrudePreviewB }
                castsShadows: false
                receivesShadows: false
                materials: XrayMaterial { tint: extrudeTool.fillOf(1) }
            }
            Model {
                objectName: "extrudeWireB"
                geometry: MeshGeometry { id: extrudeWireB; lines: true }
                castsShadows: false
                receivesShadows: false
                materials: XrayMaterial { tint: extrudeTool.lineOf(1) }
            }
            Model {
                objectName: "extrudeCutB"
                geometry: MeshGeometry { id: extrudeCutB }
                castsShadows: false
                receivesShadows: false
                materials: XrayMaterial { tint: extrudeTool.cutColor }
            }
            // Closed profiles, lightly shaded (Main.drawProfiles), under the lines.
            Model {
                objectName: "profileShading"
                geometry: MeshGeometry { id: sketchProfiles }
                depthBias: -20
                castsShadows: false
                receivesShadows: false
                materials: ProfileMaterial { tint: theme.sketchProfile }
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
                geometry: MeshGeometry { id: sketchProjectedDashed }
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
            if (!sketch.active && !sketchTools.tool) {
                const written = selection.textAt(x, y)       // a text in a finished sketch: back into it to edit
                if (written) {
                    root.editText(written.sketch, written.index)
                    return
                }
                selection.selectBodyAt(x, y)       // the whole body only, never its component
                return
            }
            if (!sketch.active || sketchTools.tool)
                return
            const part = selection.partAt(x, y)
            if (part && part.kind === "sketch_curve" && sketch.sketchId) {      // a text: open it for editing (t60u)
                const k = doc.sketchGeometry(sketch.sketchId).curves.find(c => c.id === part.index)
                if (k && k.type === "text") {
                    root.editText(sketch.sketchId, part.index)
                    return
                }
            }
            // The model in view while sketching: its whole body too, the clicked item let go (owner, t50u).
            if (part && (part.kind === "face" || part.kind === "edge" || part.kind === "point")) {
                selection.selectBodyAt(x, y)
                return
            }
            const chain = part && part.kind.startsWith("sketch_") ? sketchTools.profileOf(part) : []
            if (chain.length) {
                selection.selected = chain
                selection.redraw()
            }
        }
        leftDrag: ({
            begin: (x, y, modifiers) => {
                // DRAGDIAG (temporary, owner t72u): what a press-drag did, in the window's log.
                const pa = sketch.active ? selection.partAt(x, y) : null
                const r = sketchTools.dragBegin(x, y, modifiers)
                console.log("DRAGDIAG", Math.round(x), Math.round(y), "tool=" + sketchTools.tool, "shape=" + sketchTools.shape, "sketchActive=" + sketch.active,
                            "gather=" + sketchTools.gather.length, "sel=" + selection.selected.length, "part=" + (pa ? pa.kind + ":" + pa.index : "none"),
                            "slide=" + sketchTools.slide, "result=" + r)
                return r
            },
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
        visible: root.openSketchShown && root.showDimensions
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
                                            : root.keptNear(dragging ? dragSpot
                                                            : (root.labelSpots[index] || root.dimensionSpot(a, b, modelData.type)), a, b)
                // The dashed line from the value back to what it measures: from wherever on the
                // box is closest to wherever on the line is closest, so it stays short and tidy.
                readonly property var leader: {
                    if (look)
                        return null
                    const near = root.nearestOn(spot, a, b)
                    const at = Qt.point(Math.max(spot.x - tag.width / 2, Math.min(spot.x + tag.width / 2, near.x)),
                                        Math.max(spot.y - tag.height / 2, Math.min(spot.y + tag.height / 2, near.y)))
                    const to = root.nearestOn(at, a, b)
                    return Math.hypot(at.x - to.x, at.y - to.y) > 14 ? { x: at.x, y: at.y, x2: to.x, y2: to.y } : null
                }
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
                Shape {
                    objectName: "dimensionLeader"
                    visible: !!dim.leader
                    ShapePath {
                        strokeColor: theme.textSecondary
                        strokeWidth: 1
                        fillColor: "transparent"
                        strokeStyle: ShapePath.DashLine
                        dashPattern: [3, 3]
                        startX: dim.leader ? dim.leader.x : 0
                        startY: dim.leader ? dim.leader.y : 0
                        PathLine { x: dim.leader ? dim.leader.x2 : 0; y: dim.leader ? dim.leader.y2 : 0 }
                    }
                }
                // Says what a driven dimension is, for a few seconds after it appears (or when clicked), under it.
                Rectangle {
                    id: drivenToast
                    readonly property bool shown: !!dim.modelData.driven && root.drivenToastId === dim.modelData.id
                    visible: opacity > 0
                    opacity: shown ? 1 : 0
                    Behavior on opacity { NumberAnimation { duration: root.fadeMs } }
                    x: Math.round(Math.max(8, Math.min(root.width - width - 8, dim.spot.x - width / 2)))
                    y: Math.round(dim.spot.y + 18)
                    width: toastText.implicitWidth + 20
                    height: toastText.implicitHeight + 12
                    radius: 8
                    color: Qt.rgba(theme.panelBg.r, theme.panelBg.g, theme.panelBg.b, 0.95)
                    border.color: theme.warning
                    Text {
                        id: toastText
                        anchors.centerIn: parent
                        text: "Driven dimension: the other dimensions decide it,\nso it can't be set by hand"
                        color: theme.warning
                        font.pixelSize: 12
                        horizontalAlignment: Text.AlignHCenter
                    }
                }
                Rectangle {
                    id: tag
                    readonly property var tipTheme: theme
                    readonly property var hitArea: area
                    x: Math.round(dim.spot.x - width / 2)
                    y: Math.round(dim.spot.y - height / 2)
                    width: label.implicitWidth + 14
                    height: 22
                    radius: 6
                    readonly property bool picked: sketchTools.pickedConstraints.includes(dim.modelData.id)
                    color: picked ? theme.accent : Qt.rgba(theme.panelBg.r, theme.panelBg.g, theme.panelBg.b, 0.85)
                    border.color: dim.modelData.driven ? "transparent" : picked || area.containsMouse ? theme.accent : theme.border
                    // A driven dimension: a dotted border and amber italic text, so it can't be mistaken for one you set.
                    Shape {
                        visible: !!dim.modelData.driven
                        anchors.fill: parent
                        ShapePath {
                            strokeColor: parent.parent.picked || area.containsMouse ? theme.accent : theme.warning
                            strokeWidth: 1.5
                            fillColor: "transparent"
                            strokeStyle: ShapePath.DashLine
                            dashPattern: [1.5, 2.5]
                            capStyle: ShapePath.RoundCap
                            PathRectangle { x: 1; y: 1; width: tag.width - 2; height: tag.height - 2; radius: 6 }
                        }
                    }
                    Text {
                        id: label
                        anchors.centerIn: parent
                        text: (dim.modelData.driven ? "(" : "") + (dim.modelData.expr ? "Fx " : "") + Units.show(dim.look ? dim.look.shown : dim.modelData.value, dim.modelData.unit)
                              + (dim.modelData.driven ? ")" : "")
                        color: parent.picked ? theme.panelBg : dim.modelData.driven ? theme.warning : theme.textPrimary
                        font.pixelSize: 12
                        font.italic: !!dim.modelData.driven
                    }
                    // Fx: which parameter (or formula) gives this value (owner, 2026-11-02, t91u).
                    Loader {
                        active: !!dim.modelData.expr
                        sourceComponent: HoverTip {
                            theme: tag.tipTheme
                            area: tag.hitArea
                            title: dim.modelData.expr
                            text: "Parameter formula: open Parameters to change it"
                            delay: 600
                        }
                    }
                    // Click: pick it, like a constraint's badge, for Delete. Double-click: change the
                    // value. Drag: move it where you like (it stays there).
                    MouseArea {
                        id: area
                        property point pressAt
                        property bool wasDragged: false
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
                            wasDragged = dim.dragging
                            if (!dim.dragging)
                                return
                            const at = sketchTools.onPlane(dim.spot.x, dim.spot.y)
                            if (dim.look)
                                doc.placeDimensionLabel(sketch.sketchId, dim.modelData.id, dim.look.along1, dim.look.along2)
                            else if (at)
                                doc.placeDimensionLabel(sketch.sketchId, dim.modelData.id,
                                                        at.u - dim.modelData.mid_u, at.v - dim.modelData.mid_v)
                            dim.dragging = false
                        }
                        onClicked: if (!wasDragged) sketchTools.pickConstraint(dim.modelData.id, "Dimension")
                        onDoubleClicked: {
                            if (sketchTools.pickedConstraints.includes(dim.modelData.id))
                                sketchTools.pickConstraint(dim.modelData.id, "Dimension")
                            root.editDimension(dim.modelData, dim.look)
                        }
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
        projectEdgeGeometry: projectEdgeParts
        projectFaceGeometry: projectFaceParts
        projectPicks: sketchTools.tool === "project" ? sketchTools.projectPicks : []
        through: root.selectThrough
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
        held: historyTimeline.frozen
        onSketchEditRequested: id => root.editSketch(id)
        underlined: selection.treeKeys
        cutChoices: extrudeTool.open ? extrudeTool.cutChoices : null
        onCutToggled: (body, on) => extrudeTool.toggleCut(body, on)
        // Double-click on a body row: that body. On a component row: every body inside it, and in
        // the components within it (owner, 2026-11-02). Not while sketching.
        onSelectRequested: key => {
            if (sketch.active || sketchTools.tool)
                return
            const id = parseInt(key.slice(1))
            selection.selectBodies(key[0] === "b" ? [id] : doc.bodiesUnder(id))
        }
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
        crossfade: root.crossfade
        fadeBusy: () => fading.on
        fadeMs: root.fadeMs
        onActiveChanged: { sketchTools.cancel(); sketchTools.construction = false; root.gapsOn = false; root.gapsFading = false; root.gapAt = -1 }
    }
    SketchTools {
        id: sketchTools
        sketch: sketch
        selection: selection
        selectThrough: root.selectThrough
        geometryShown: root.openSketchShown
        free: navigation.ctrlHeld
        screenOf: p => root.toScreen(p)
    }
    // A box drag in a sketch: dashed takes what's wholly inside, solid anything it touches (owner, 2026-11-02).
    Rectangle {
        id: boxOverlay
        readonly property var box: sketchTools.box
        readonly property bool crossing: !!box && box.x2 < box.x
        readonly property bool dashed: !!box && !crossing   // left to right: wholly inside, dashed
        // Ctrl (taking off) is drawn in a colour that never matches the box for adding, whatever the
        // theme: red when that sits well away from the accent's hue, else the next of yellow, green,
        // else the accent's opposite (owner, 2026-11-02: a red Omarchy theme made the two alike).
        function hueGap(c) {
            const acc = theme.accent
            if (c.hslSaturation < 0.12 || acc.hslSaturation < 0.12)
                return c.hslSaturation < 0.12 && acc.hslSaturation < 0.12 ? 0 : 0.5
            const d = Math.abs(c.hslHue - acc.hslHue)
            return Math.min(d, 1 - d)
        }
        readonly property color removeTint: {
            const pool = [theme.danger, theme.warning, theme.success]
            for (const c of pool)
                if (hueGap(c) > 0.14)
                    return c
            return Qt.hsla((theme.accent.hslHue + 0.5) % 1, 0.8, theme.accent.hslLightness < 0.5 ? 0.6 : 0.4, 1)
        }
        readonly property color tint: box && box.mode === "remove" ? removeTint : theme.accent
        visible: !!box
        x: box ? Math.min(box.x, box.x2) : 0
        y: box ? Math.min(box.y, box.y2) : 0
        width: box ? Math.abs(box.x2 - box.x) : 0
        height: box ? Math.abs(box.y2 - box.y) : 0
        z: 50
        color: Qt.alpha(tint, dashed ? 0.08 : 0.14)
        border.color: tint
        border.width: dashed ? 0 : 1
        // Left to right takes only what is wholly inside: a dashed edge; right to left a solid one.
        Repeater {
            model: boxOverlay.dashed ? 4 : 0
            Item {
                required property int index
                readonly property bool horiz: index < 2
                x: horiz ? 0 : (index === 2 ? 0 : boxOverlay.width - 1)
                y: horiz ? (index === 0 ? 0 : boxOverlay.height - 1) : 0
                width: horiz ? boxOverlay.width : 1
                height: horiz ? 1 : boxOverlay.height
                Repeater {
                    model: Math.ceil((horiz ? boxOverlay.width : boxOverlay.height) / 7)
                    Rectangle {
                        required property int index
                        x: parent.horiz ? index * 7 : 0
                        y: parent.horiz ? 0 : index * 7
                        width: parent.horiz ? 4 : 1
                        height: parent.horiz ? 1 : 4
                        color: boxOverlay.tint
                    }
                }
            }
        }
    }

    // A constraint from the search (SketchTools.ruleFor): what's selected counts as the first
    // picks, in the order it was selected; if that's all it needs it's done at once, otherwise
    // the tool starts and waits for clicks, as in Fusion (owner, 2026-10-29).
    readonly property var constraintStarts: ({
        parallel: "Click the first line, then the second.", perpendicular: "Click the first line, then the second.",
        equal: "Click the first line, then the second.", collinear: "Click the first line, then the second.",
        hv: "Click a line, or two points.", coincident: "Click a point, then a point or a line.",
        midpoint: "Click a point and a line.", symmetry: "Click two points or two lines, then the line to mirror across.",
        tangent: "Click a line, arc or circle, then an arc or circle.", concentric: "Click two arcs or circles.",
        curvature: "Click a spline, then the line, arc or spline it meets at its end."
    })
    function constrain(type) {
        const parts = selection.selected.filter(p => p.kind.startsWith("sketch_") || p.kind === "origin")
        const plan = type === "fix" ? { more: "" } : sketchTools.batchPlan(type, parts)
        // The selection is all it needs: every constraint is made now, at once (Fusion with things
        // picked beforehand); the first pick is the one the others follow (SketchTools.batchPlan).
        if (plan.steps) {
            let made = 0, said = ""
            for (const s of plan.steps) {
                const why = doc.addSketchConstraint(sketch.sketchId, s.type, s.first, s.second, s.third, s.keep)
                if (!why)
                    made++
                else if (!said || !/already/.test(why))
                    said = said || why
            }
            selection.clear()
            if (made === 0 && !said)
                said = "Those already have that."
            sketch.say(said ? (made ? made + " made; " : "") + said
                            : made === 1 ? "Made it" : "Made " + made)
            return
        }
        if (plan.wrong) {
            sketch.say(plan.wrong)
            return
        }
        // Too little selected: the tool starts with those as its first picks and waits for clicks.
        sketchTools.use(type)
        for (const part of parts)
            sketchTools.pickWith(part)
        if (parts.length === 0)
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
        // An arc or circle with another arc, circle or a point: the distance from its centre.
        const arcs = parts.filter(p => p.kind === "sketch_arc")
        if (arcs.length && parts.length === 2 && lines.length === 0) {
            const g = doc.sketchGeometry(sketch.sketchId)
            const centre = p => p.kind === "sketch_arc" ? g.arcs.find(q => q.id === p.index).c : p.index
            return askDimension("distance", centre(parts[0]), centre(parts[1]))
        }
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
        // The other dimensions already decide it (owner, t88u): no value to type, it goes in as a driven
        // (reference) dimension showing what it is.
        if (doc.dimensionIsDriven(sketch.sketchId, type, first, second, !!otherSide)) {
            dimInput.close()
            const unit = type === "angle" ? "deg" : "in"
            const why = doc.addSketchDimension(sketch.sketchId, type, first, second, m.value, unit,
                                               Units.show(m.value, unit), !!otherSide)
            if (why)
                return sketch.say(why)
            sketchTools.dimLine = 0
            const drivenNow = doc.sketchDimensions(sketch.sketchId).filter(d => d.driven).pop()
            if (drivenNow)
                showDrivenToast(drivenNow.id)
            return sketch.say("The other dimensions already decide that one, so it's a driven dimension: shown in brackets, it follows them.")
        }
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
                                                   Units.show(v.mm, v.unit), !!otherSide, v.expr || "")
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

    // The driven dimension whose explanation is showing (fades after a few seconds).
    property int drivenToastId: 0
    function showDrivenToast(id) { drivenToastId = id; drivenToastTimer.restart() }
    Timer { id: drivenToastTimer; interval: 5000; onTriggered: root.drivenToastId = 0 }

    // An angle shown on the other side of its corner (angleLook) is typed as that side's angle.
    function editDimension(d, look) {
        if (d.driven) {
            showDrivenToast(d.id)
            return sketch.say("That one is driven by the other dimensions, so it can't be typed. Change those instead.")
        }
        const at = look ? look.spot : dimensionSpot(toScreen(d.a), toScreen(d.b), d.type)
        const flip = !!(look && look.flipped)
        dimInput.show(at.x, at.y, look ? look.shown : d.value, d.unit,
            v => {
                if (d.type === "angle" && !(v.mm > 1e-9 && v.mm < Math.PI - 1e-9))
                    return "An angle has to be more than 0° and less than 180°."
                return doc.setSketchDimension(sketch.sketchId, d.id, flip ? Math.PI - v.mm : v.mm, v.unit,
                                              Units.show(v.mm, v.unit), v.expr || "", !!v.expr && flip)
            }, d.expr || "")
    }

    // Engine coordinates → window pixels.
    function toScreen(p) {
        return view.mapFrom3DScene(modelSpace.mapPositionToScene(p))
    }
    // Where a dimension's value sits: halfway along, a little off to one side (above, for a
    // line's own length, so it doesn't sit on the line).
    // The closest place on the line a-b to p, in pixels.
    function nearestOn(p, a, b) {
        const dx = b.x - a.x, dy = b.y - a.y, l = dx * dx + dy * dy
        const t = l > 0 ? Math.max(0, Math.min(1, ((p.x - a.x) * dx + (p.y - a.y) * dy) / l)) : 0
        return Qt.point(a.x + dx * t, a.y + dy * t)
    }
    // A dimension's value stays within twice what it measures of it, so it can't lose its line.
    // 44 px at least, or a tiny dimension's value could never clear its line.
    function keptNear(p, a, b) {
        const near = nearestOn(p, a, b), far = Math.hypot(p.x - near.x, p.y - near.y)
        const most = Math.max(44, 1.98 * Math.hypot(b.x - a.x, b.y - a.y))
        return far <= most ? p : Qt.point(near.x + (p.x - near.x) * most / far, near.y + (p.y - near.y) * most / far)
    }
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
            sketch.say(why || (type === "curvature" ? "Curvature made: the spline now runs on from it with the same direction and bend" : ""))
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
            sketch.say("Fitted " + made.join(", ") + " in " + seconds.toFixed(1) + " s · at most " + (worstMm / 25.4).toFixed(4) + " in off · " + trianglesLeft.toLocaleString(Qt.locale(), "f", 0) + " triangles left as facets")
        }
        function onImportFinished(why, seconds, faces, triangles) {
            if (why)
                return sketch.say(why)
            navigation.fit()
            sketch.say("Imported " + doc.importName + " in " + seconds.toFixed(1) + " s · " + faces.toLocaleString(Qt.locale(), "f", 0) + " faces, " + triangles.toLocaleString(Qt.locale(), "f", 0) + " triangles")
        }
    }
    // While an extrude's solid work runs: the view still turns, so this only says it's busy.
    Rectangle {
        id: workingCard
        objectName: "workingCard"
        property double started: 0
        property real elapsed: 0
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.bottom: parent.bottom
        anchors.bottomMargin: 90
        width: 240
        height: 54
        radius: 12
        z: 900
        opacity: doc.working ? 1 : 0
        visible: opacity > 0
        Behavior on opacity { NumberAnimation { duration: 250 } }
        color: Qt.rgba(theme.panelBg.r, theme.panelBg.g, theme.panelBg.b, 0.92)
        Connections {
            target: doc
            function onWorkingChanged() {
                if (doc.working) {
                    workingCard.started = Date.now()
                    workingCard.elapsed = 0
                }
            }
        }
        Timer {
            interval: 100
            repeat: true
            running: doc.working
            onTriggered: workingCard.elapsed = (Date.now() - workingCard.started) / 1000
        }
        Text {
            x: 16; y: 10
            text: "Extruding"
            color: theme.textPrimary
            font.pixelSize: 14
            font.bold: true
        }
        Text {
            x: parent.width - 86; y: 10; width: 70
            horizontalAlignment: Text.AlignRight
            text: workingCard.elapsed.toFixed(1) + " s"
            color: theme.textPrimary
            font.pixelSize: 14
            font.features: { "tnum": 1 }
        }
        Rectangle {
            x: 16; y: 36; width: parent.width - 32; height: 4; radius: 2
            clip: true
            color: Qt.rgba(theme.textPrimary.r, theme.textPrimary.g, theme.textPrimary.b, 0.15)
            Rectangle {
                width: 60; height: 4; radius: 2
                color: theme.selection
                NumberAnimation on x {
                    from: -60; to: 210; duration: 1200
                    loops: Animation.Infinite
                    running: doc.working
                }
            }
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
            sketch.say("No gaps under 0.039 in · " + closed + (closed === 1 ? " closed profile" : " closed profiles"))
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
    // Fusion's Sketch Palette switches, for this run: Show Points and Show Projected Geometries
    // leave what's off neither drawn nor picked; profiles still count it, as snapping does.
    property bool showDimensions: true
    property bool showConstraints: true
    property bool showPoints: true
    property bool showProjected: true
    // The Selection menu (was Selectable): what a plain click or hover may pick while sketching (Selection.allow).
    property bool selLines: true
    property bool selPoints: true
    property bool selConstraints: true
    property bool selProfiles: true
    property bool selConstruction: true
    property bool selProjected: true
    property bool selOrigin: true
    property bool selFaces: true
    property bool selEdges: true
    property bool selBodyPoints: true
    property bool selOutside: true
    // Select Through (owner, 2026-11-02): global, in the Selection menu of both modes; what a box
    // takes, and what a click or hover can reach, no longer stops at the model's front.
    property bool selectThrough: false
    Binding {
        target: selection
        property: "allow"
        // Outside sketching the sketch-only switches are not in the menu, so they never hold anything back.
        value: ({ lines: !sketch.active || root.selLines, points: !sketch.active || root.selPoints,
                  construction: !sketch.active || root.selConstruction,
                  projected: !sketch.active || root.selProjected, profiles: root.selProfiles, origin: root.selOrigin,
                  bodies: root.selFaces || root.selEdges || root.selBodyPoints, faces: root.selFaces, edges: root.selEdges, bodyPoints: root.selBodyPoints,
                  outside: root.selOutside })
    }
    Binding {
        target: selection
        property: "filtered"
        value: !sketch.active || (!sketch.picking && !sketchTools.tool)
    }
    Binding {
        target: selection
        property: "bodyFiltered"
        value: !sketch.active || (!sketch.picking && (!sketchTools.tool || sketchTools.tool === "project"))
    }
    function sketchShows() {
        doc.setSketchShows(showPoints, showProjected)
        selection.clear()
        drawSketches()
    }
    function drawProfiles() {
        const ids = []
        for (const k in visibleKeys)
            if (k[0] === "s" && visibleKeys[k])
                ids.push(parseInt(k.slice(1)))
        doc.fillProfiles(sketchProfiles, ids)
        // Picking: every shown sketch's profiles; while sketching, the open sketch's only.
        selection.profileSketches = sketch.active ? (openSketchShown ? [sketch.sketchId] : []) : ids
        selection.outsideSketches = ids
    }
    function drawSketches() {
        const ids = []
        for (const k in visibleKeys)
            if (k[0] === "s" && visibleKeys[k])
                ids.push(parseInt(k.slice(1)))
        const radius = navigation.distance * selection.perPixel() * 1.1
        doc.fillSketches(sketchDrawn, sketchFull, sketchConstruction, sketchConstructionFull, sketchProjected, sketchProjectedDashed, ids, radius)
        doc.fillSegments(sketchPreview, sketchTools.preview, radius, sketchTools.construction)
    }
    Connections {
        target: doc
        function onSketchesChanged() {
            if (doc.takeLiveTween()) {
                root.liveT = 0
                liveGlide.restart()
            }
            root.sketchTick++
            selection.keepIn(sketch.active ? doc.sketchGeometry(sketch.sketchId) : { lines: [], points: [] })
            root.drawSketches()
            root.drawProfiles()
            root.refreshDimensions()
        }
    }
    onVisibleKeysChanged: {
        doc.showBodies(Object.keys(visibleKeys).filter(k => k[0] === "b" && visibleKeys[k]).map(k => Number(k.slice(1))))
        drawSketches(); drawProfiles()
    }
    Connections {
        target: navigation
        // While zooming, redraw the sketch lines at most every 50 ms (the last step always lands).
        function onDistanceChanged() {
            if (Date.now() - root.sketchDrawnAt >= 50) { root.sketchDrawnAt = Date.now(); sketchRedrawLater.stop(); root.drawSketches() }
            else sketchRedrawLater.restart()
        }
    }
    property double sketchDrawnAt: 0
    Timer { id: sketchRedrawLater; interval: 50; onTriggered: { root.sketchDrawnAt = Date.now(); root.drawSketches() } }
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
        id: sketchBar
        anchors.top: parent.top
        anchors.topMargin: 14
        guideBelow: Math.max(extrudeTool.rowsBottom, chamferTool.rowsBottom, filletTool.rowsBottom) - 14
        anchors.horizontalCenter: parent.horizontalCenter
        theme: theme
        sketch: sketch
        toolName: sketchTools.toolName
        toolCount: sketchTools.tool === "project" ? sketchTools.projectCount : -1
        toolSkipped: sketchTools.tool === "project" ? sketchTools.projectSkipped.length : 0
        onClearCount: sketchTools.projectClearAll()
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
        fontList: sketchTools.shape === "text" || sketchTools.shape === "textpath" ? doc.fontFamilies() : []
        fields: textEdit.curve ? [] : sketchTools.shapeDef && sketchTools.shapeDef.options ? sketchTools.shapeDef.options : []
        values: sketchTools.optionValues[sketchTools.shape] || ({})
        twoRows: sketchTools.shape === "text" || sketchTools.shape === "textpath"
        doneLabel: twoRows ? "Done" : ""
        // Done writes the text (as Enter does); the tool then ends. With nothing picked it simply ends the tool.
        onDone: {
            if (sketchTools.gather.length || sketchTools.clicks.length) {
                sketchTools.confirm()     // writes it and ends the tool; with no words typed it says so and waits
            } else {
                sketchTools.cancel()      // nothing picked: Done just ends the tool, like Project's Done
            }
        }
        onChanged: (key, value) => sketchTools.setOption(key, value)
        onConfirmed: sketchTools.confirm()
        onCancelled: sketchTools.cancel()
        onUndo: root.undoLast()
        onRedo: root.redoLast()
    }
    // The one text picked (nothing else, no tool on): its words, font, size and look, changed
    // here as they were made (owner, 2026-11-02). Each change is one undoable step.
    ToolOptions {
        id: textEdit
        // The edit session of a text is kept by the document (same text, nothing else recorded since); the
        // panel hiding for a moment (tool changed, a drag re-picking the text) must not end it.
        readonly property var curve: {
            sketchTools.drawnTick
            // Also while a text tool is on with nothing picked for it yet: a written text picked then
            // is edited here, in every detail (owner, t77u), not the tool's options for the next one.
            const textTool = sketchTools.tool === "shape" && (sketchTools.shape === "text" || sketchTools.shape === "textpath")
                           && !sketchTools.gather.length && !sketchTools.clicks.length
            if (!sketch.active || (sketchTools.tool !== "" && !textTool) || selection.selected.length !== 1)
                return null
            const p = selection.selected[0]
            if (p.kind !== "sketch_curve")
                return null
            const k = doc.sketchGeometry(sketch.sketchId).curves.find(c => c.id === p.index)
            return k && k.type === "text" ? k : null
        }
        anchors.top: parent.top
        anchors.topMargin: 14 + 116 + 6
        anchors.horizontalCenter: parent.horizontalCenter
        z: 9
        theme: theme
        tool: curve ? "text-edit" + curve.id : ""
        fontList: curve ? doc.fontFamilies() : []
        readonly property bool along: !!(curve && curve.path && curve.path.length)
        readonly property bool single: !!(curve && !along && curve.points.length < 2)    // older text: one point
        fields: !curve ? [] : single
                ? [{ key: "text", label: "Text", kind: "text" }, { key: "font", label: "Font", kind: "font" },
                   { key: "height", label: "Height", kind: "length" }, { key: "bold", label: "Bold", kind: "toggle" },
                   { key: "italic", label: "Italic", kind: "toggle" }]
                : along
                ? [{ key: "text", label: "Text", kind: "text" }, { key: "font", label: "Font", kind: "font" },
                   { key: "height", label: "Height", kind: "length" }, { key: "bold", label: "Bold", kind: "toggle" },
                   { key: "italic", label: "Italic", kind: "toggle" },
                   { key: "across", label: "Justify", kind: "choice", choices: ["Left", "Centre", "Right"] },
                   { key: "flip", label: "Flip", kind: "toggle" }]
                : [{ key: "text", label: "Text", kind: "text" }, { key: "font", label: "Font", kind: "font" },
                   { key: "height", label: "Height", kind: "length" }, { key: "fit", label: "Fit to box", kind: "toggle" },
                   { key: "bold", label: "Bold", kind: "toggle" }, { key: "italic", label: "Italic", kind: "toggle" },
                   { key: "across", label: "Across", kind: "choice", choices: ["Left", "Centre", "Right"] },
                   { key: "down", label: "Down", kind: "choice", choices: ["Top", "Middle", "Bottom"] }]
        values: curve ? { text: curve.text, font: curve.font, height: curve.value, fit: !!(curve.flags & 4), bold: !!(curve.flags & 1),
                          italic: !!(curve.flags & 2), across: (curve.flags >> 4) & 3, down: (curve.flags >> 6) & 3,
                          flip: !!(curve.flags & 256) } : ({})
        onChanged: (key, value) => {
            if (!curve)
                return
            const v = Object.assign({}, values)
            v[key] = value
            if (key === "height")
                v.fit = false
            const flags = (v.bold ? 1 : 0) | (v.italic ? 2 : 0) | (v.fit && !along ? 4 : 0) | ((v.across || 0) << 4)
                        | (along ? (v.flip ? 256 : 0) : (v.down || 0) << 6)
            const why = doc.editSketchText(sketch.sketchId, curve.id, { text: v.text, font: v.font, flags: flags, height: v.height })
            if (why)
                sketch.say(why)
        }
        twoRows: true
        doneLabel: "Done"
        // Done finishes the text: it stays as written, is put down (deselected), and the tool ends.
        onDone: textEdit.finish()
        function finish() {
            textEdit.leave()
            doc.closeTextEdit()
            selection.selected = []
            selection.redraw()
            if (sketchTools.tool !== "")
                sketchTools.cancel()
        }
        onConfirmed: textEdit.finish()
        onCancelled: textEdit.finish()   // Esc on a written text just puts it down; nothing is removed
        onUndo: root.undoLast()
        onRedo: root.redoLast()
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
            model: [{ key: "model", label: "Whole Model" }, { key: "face", label: "Face" }, { key: "edge", label: "Edges / Points" },
                    { key: "sketch", label: "Sketch Parts" }, { key: "wholesketch", label: "Whole Sketch" }]
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
        Rectangle {
            objectName: "projectDone"
            width: 80
            height: 34
            radius: 8
            color: theme.success
            Text {
                anchors.centerIn: parent
                text: "Done"
                color: theme.windowBg
                font.pixelSize: 12
                font.bold: true
            }
            MouseArea {
                anchors.fill: parent
                cursorShape: Qt.PointingHandCursor
                onClicked: sketchTools.cancel()
            }
        }
    }
    // Picking a line or shape in the view leaves the option boxes, so Enter then finishes the tool
    // (in the Text box Enter is a new line); owner t73u: his four picks were never turned into text.
    Connections {
        target: sketchTools
        // After a path is picked the cursor jumps into the Text box, so typing never reaches the shortcuts
        // (owner, t84u); other gathering tools keep the keys for Enter.
        function onGatherChanged() {
            if (sketchTools.gather.length > 0 && (sketchTools.shape === "text" || sketchTools.shape === "textpath"))
                Qt.callLater(toolOptions.focusFirst)
            else if (sketchTools.gather.length > 0) { toolOptions.leave(); enterKey.forceActiveFocus() }
        }
    }
    // Where the keys land once the option boxes are left: Enter finishes the tool even if the
    // window's shortcut doesn't see it.
    Item {
        id: enterKey
        Keys.onReturnPressed: event => { event.accepted = sketchTools.tool === "shape" && sketchTools.confirm() }
        Keys.onEnterPressed: event => { event.accepted = sketchTools.tool === "shape" && sketchTools.confirm() }
    }
    // Enter: a tool gathering things (Mirror, Offset, a spline's points…) carries on.
    Shortcut {
        sequences: ["Return", "Enter"]
        enabled: sketchTools.tool === "shape" && !search.open && !dimInput.open && !toolOptions.typing && !textEdit.typing
        onActivated: sketchTools.confirm()
    }

    SketchDisplay {
        objectName: "sketchDisplay"
        anchors.bottom: parent.bottom
        anchors.bottomMargin: 10
        anchors.right: parent.right
        anchors.rightMargin: 100
        visible: sketch.active
        z: 40
        theme: theme
        rows: [
            { name: "Show Points", on: root.showPoints, flip: () => { root.showPoints = !root.showPoints; root.sketchShows() } },
            { name: "Show Dimensions", on: root.showDimensions, flip: () => root.showDimensions = !root.showDimensions },
            { name: "Show Constraints", on: root.showConstraints, flip: () => root.showConstraints = !root.showConstraints },
            { name: "Show Projected Geometries", on: root.showProjected, flip: () => { root.showProjected = !root.showProjected; root.sketchShows() } }
        ]
    }
    SketchDisplay {
        objectName: "sketchSelectable"
        idName: "selectable"
        title: "Selection"
        buttonWidth: 84
        anchors.bottom: parent.bottom
        anchors.bottomMargin: 10
        anchors.right: parent.right
        anchors.rightMargin: 8
        visible: sketch.active
        z: 40
        theme: theme
        rows: [
            { name: "Lines", on: root.selLines, flip: () => root.selLines = !root.selLines },
            { name: "Points", on: root.selPoints, flip: () => root.selPoints = !root.selPoints },
            { name: "Constraints", on: root.selConstraints, flip: () => root.selConstraints = !root.selConstraints },
            { name: "Profile Areas", on: root.selProfiles, flip: () => root.selProfiles = !root.selProfiles },
            { name: "Construction Lines", on: root.selConstruction, flip: () => root.selConstruction = !root.selConstruction },
            { name: "Projections", on: root.selProjected, flip: () => root.selProjected = !root.selProjected },
            { name: "Origin", on: root.selOrigin, flip: () => root.selOrigin = !root.selOrigin },
            { name: "Components", on: false, enabled: false, flip: () => {}, children: [
                { name: "Bodies", on: false, flip: () => {}, children: [
                    { name: "Faces", on: root.selFaces, flip: () => root.selFaces = !root.selFaces },
                    { name: "Edges", on: root.selEdges, flip: () => root.selEdges = !root.selEdges },
                    { name: "Points", on: root.selBodyPoints, flip: () => root.selBodyPoints = !root.selBodyPoints }
                ] },
                { name: "Outside Sketches", on: root.selOutside, flip: () => root.selOutside = !root.selOutside }
            ] },
            { name: "Select Through", divider: true, on: root.selectThrough, flip: () => root.selectThrough = !root.selectThrough }
        ]
    }
    // The same menu outside sketching: only what can be selected there (owner, 2026-11-02).
    SketchDisplay {
        objectName: "modelSelection"
        idName: "modelSelect"
        title: "Selection"
        buttonWidth: 84
        anchors.bottom: parent.bottom
        anchors.bottomMargin: 10
        anchors.right: parent.right
        anchors.rightMargin: 8
        visible: !sketch.active
        z: 40
        theme: theme
        rows: [
            { name: "Profile Areas", on: root.selProfiles, flip: () => root.selProfiles = !root.selProfiles },
            { name: "Origin", on: root.selOrigin, flip: () => root.selOrigin = !root.selOrigin },
            { name: "Components", on: false, enabled: false, flip: () => {}, children: [
                { name: "Bodies", on: false, flip: () => {}, children: [
                    { name: "Faces", on: root.selFaces, flip: () => root.selFaces = !root.selFaces },
                    { name: "Edges", on: root.selEdges, flip: () => root.selEdges = !root.selEdges },
                    { name: "Points", on: root.selBodyPoints, flip: () => root.selBodyPoints = !root.selBodyPoints }
                ] }
            ] },
            { name: "Select Through", divider: true, on: root.selectThrough, flip: () => root.selectThrough = !root.selectThrough }
        ]
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
            { name: "Parameters", words: "parameter variables formulas equations values fx user named expressions", hint: "Named values and formulas that dimensions can use", group: "Look", modes: both, keys: "", run: () => Qt.callLater(parametersWindow.openIt) },
            { name: theme.light ? "Dark Mode" : "Light Mode", words: "dark light theme colours", hint: "Switch the look", group: "Look", modes: both, keys: "", run: () => theme.setLight(!theme.light) },
            { name: "Follow Omarchy Theme", words: "colours", onlyOmarchy: true, hint: theme.followSystem ? "Back to OmaMake's own colours" : "Take colours from the current Omarchy theme", group: "Look", modes: both, keys: "", run: () => theme.followSystem = !theme.followSystem },
            { name: "Light Follows View", hint: root.lightFollowsView ? "On: the lamp turns with you. Turn off to keep it fixed in the room" : "Off: the lamp stays put in the room. Turn on to have it turn with you", group: "Look", modes: ["model"], keys: "", run: () => root.lightFollowsView = !root.lightFollowsView },
            { name: "Grid", hint: navigation.gridVisible ? "Hide the floor grid" : "Show the floor grid", group: "Look", modes: ["model"], keys: "", run: () => navigation.gridVisible = !navigation.gridVisible },
            { name: "Simple Look", words: "fast performance effects", hint: navigation.simpleLook ? "Back to full shading and shadows" : "Fewer effects, for slower computers", group: "Look", modes: ["model"], keys: "", run: () => navigation.simpleLook = !navigation.simpleLook },
            { name: "Extrude", words: "extrude push pull pad boss cut join profile area face distance symmetric two sides new body", hint: "Push profiles or flat faces out into a body, a join, or a cut", group: "Create", modes: ["model"], keys: "E", letter: "e", run: () => Qt.callLater(extrudeTool.begin) },
            { name: "Chamfer", words: "chamfer bevel edge corner equal distance two distances angle miter blend tangent chain", hint: "Bevel edges: equal, two distances or distance and angle; runs on through where it's too big", group: "Modify", modes: ["model"], keys: "Shift C", run: () => { filletTool.cancel(); Qt.callLater(chamferTool.begin) } },
            { name: "Fillet", sym: "filletmodel", words: "fillet round radius edge corner constant chord length tangent chain blend", hint: "Round edges: a constant radius or chord length; runs on through where it's too big", group: "Modify", modes: ["model"], keys: "Shift F", run: () => { chamferTool.cancel(); Qt.callLater(filletTool.begin) } },
            { name: "New Component", hint: "An empty component inside the active one, made active", group: "Assemble", modes: ["model"], keys: "", run: () => doc.newComponent() },
            { name: "Browser", hint: browserTree.shown ? "Hide the design tree" : "Show the design tree", group: "Look", modes: both, keys: "", run: () => browserTree.shown = !browserTree.shown },
            { name: "Origin Planes: Cross", hint: "All planes full size, through the centre (how OmaMake opens)", group: "Look", modes: ["model"], keys: "", run: () => root.setPlaneStyle("", "cross") },
            { name: "Origin Planes: Cube Corner", hint: "All planes smaller, plus side only, apart (how OmaMake opens)", group: "Look", modes: ["model"], keys: "", run: () => root.setPlaneStyle("", "corner") },
            { name: "Create Sketch", hint: "Pick a flat face, or empty space for an origin plane (or triple-click one)", group: "Create", modes: ["model"], keys: "", run: () => sketch.picking = true },
            { name: "Line", letter: "l", hint: "Click the start, then each next point; Esc stops", group: "Create", modes: ["sketch"], keys: "L", run: () => sketchTools.use("line") },
            { name: "2-Point Rectangle", hint: "Click one corner, then the opposite one", group: "Create", modes: ["sketch"], keys: "", run: () => sketchTools.use("rectangle") },
            { name: "Project", letter: "p", words: "project geometry include silhouette outline profile shadow", hint: "Choose Whole Model, Face or Edges / Points, click the model as often as you like, then Done", group: "Create", modes: ["sketch"], keys: "", run: () => sketchTools.use("project") },
            { name: "Center Diameter Circle", hint: "Click the centre, then the edge", group: "Create", modes: ["sketch"], keys: "", run: () => sketchTools.use("circle") },
            { name: "Center Point Arc", words: "arc", hint: "Click the centre, where it starts, then where it ends", group: "Create", modes: ["sketch"], keys: "", run: () => sketchTools.use("arc") },
            { name: "Curvature", words: "g2 smooth blend continuity spline bend match", hint: "Click a spline, then the line, arc or spline it meets at its end: they run on with the same direction and bend", group: "Constraints", modes: ["sketch"], keys: "", run: () => root.constrain("curvature") },
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
            { name: "Show Points", words: "sketch palette display hide", hint: root.showPoints ? "Hide the sketch's points" : "Show the sketch's points", group: "Look", modes: ["sketch"], keys: "", run: () => { root.showPoints = !root.showPoints; root.sketchShows() } },
            { name: "Show Dimensions", words: "sketch palette display hide", hint: root.showDimensions ? "Hide the dimensions" : "Show the dimensions", group: "Look", modes: ["sketch"], keys: "", run: () => root.showDimensions = !root.showDimensions },
            { name: "Show Constraints", words: "sketch palette display hide badges", hint: root.showConstraints ? "Hide the constraint badges" : "Show the constraint badges", group: "Look", modes: ["sketch"], keys: "", run: () => root.showConstraints = !root.showConstraints },
            { name: "Show Projected Geometries", words: "sketch palette display hide purple", hint: root.showProjected ? "Hide what's projected from the model" : "Show what's projected from the model", group: "Look", modes: ["sketch"], keys: "", run: () => { root.showProjected = !root.showProjected; root.sketchShows() } },
            { name: "Toggle Construction", letter: "x", words: "construction normal reference guide dashed", hint: "Turn the selected lines construction or back; with nothing selected or while drawing, what you draw next", group: "Sketch", modes: ["sketch"], keys: "X", keepsTool: true, run: () => root.toggleConstruction() },
            { name: "Find Profile Gaps", words: "gap gaps break broken open leak profile closed close not closing", hint: "Rings in red, for 10 s (Esc sooner), where lines come within 0.039 in without being joined", group: "Sketch", modes: ["sketch"], keys: "", keepsTool: true, run: () => root.showGaps() },
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

    // Esc, the app's one (owner pt158u, GB-056: "clicking escape should just escape out of any tool that I am
    // using, period"): whatever focus has, the top layer goes. A question or menu first, then the
    // dimension box, Create Sketch's pick, Find Profile Gaps' rings, panning, a model tool (Extrude: back
    // from picking an object first; Chamfer, Fillet: nothing applied), a sketch drawing tool, picked
    // constraint badges, the sketch itself (Finish Sketch, owner 2026-10-29), and last the selection.
    // The tools' boxes don't take Esc themselves. A tool-options box being typed in (its list, a text's
    // words) and the Parameters window keep their own Esc.
    Shortcut {
        id: escKey
        sequence: "Esc"
        context: Qt.ApplicationShortcut
        enabled: !parametersWindow.open && !toolOptions.typing && !textEdit.typing
        onActivated: root.escTop()
    }
    function escTop() {
        if (breakAsk.shown)
            breakAsk.shown = false
        else if (search.open)
            search.open = false
        else if (modeMenu.open)
            modeMenu.close()
        else if (planeMenu.open)
            planeMenu.close()
        else if (dimInput.open)
            dimInput.cancel()
        else if (sketch.picking)
            sketch.picking = false
        else if (root.gapsOn)
            root.hideGaps()
        else if (navigation.panMode)
            navigation.panMode = false
        else if (extrudeTool.open)
            extrudeTool.cancel()
        else if (chamferTool.open || filletTool.open) {
            chamferTool.cancel()
            filletTool.cancel()
        } else if (sketchTools.tool)
            sketchTools.cancel()                 // a drawing tool stops first, then the sketch
        else if (sketchTools.pickedConstraints.length)
            sketchTools.pickedConstraints = []
        else if (sketch.active)
            sketch.finish()
        else
            selection.clear()
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
    // E: Extrude, as in Fusion, with whatever profile or faces are selected. Model only: in a sketch
    // E is Ellipse (SketchKeys.js), and a model tool never runs from a sketch.
    Shortcut {
        id: extrudeKey
        sequence: "E"
        enabled: !sketch.active && !sketch.picking && !search.open && !modeMenu.open && !planeMenu.open
                 && !dimInput.open && !toolOptions.typing && !extrudeTool.open && !chamferTool.open && !filletTool.open && !doc.working
        onActivated: {
            noteUse("Extrude")
            extrudeTool.begin()
        }
    }
    // Shift+F Fillet, Shift+C Chamfer (owner, pt157u, GB-055: Fusion has F for Fillet and none for Chamfer;
    // F is the sketch's Fillet here, and in a sketch Shift+F / Shift+C stay Fix/UnFix and Coincident).
    // Model only, as E; not while a box is being typed in (a text box takes its own keys first).
    readonly property bool modelKeysOn: !sketch.active && !sketch.picking && !search.open && !modeMenu.open && !planeMenu.open
                                        && !dimInput.open && !toolOptions.typing && !extrudeTool.open && !doc.working
    Shortcut {
        id: filletKey
        sequence: "Shift+F"
        enabled: root.modelKeysOn && !filletTool.open
        onActivated: {
            noteUse("Fillet")
            chamferTool.cancel()
            filletTool.begin()
        }
    }
    Shortcut {
        id: chamferKey
        sequence: "Shift+C"
        enabled: root.modelKeysOn && !chamferTool.open
        onActivated: {
            noteUse("Chamfer")
            filletTool.cancel()
            chamferTool.begin()
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
    // Delete (or Backspace): constraints picked on their badges go first, and only them, so the
    // lines selected to reach those badges stay (owner, 2026-10-30); otherwise what's selected.
    Shortcut {
        sequences: ["Delete", "Backspace"]
        enabled: sketch.active && !sketchTools.tool
                 && (sketchTools.pickedConstraints.length > 0 || selection.selected.some(p => p.kind.startsWith("sketch_")))
                 && !search.open && !modeMenu.open && !planeMenu.open && !dimInput.open
        onActivated: sketchTools.pickedConstraints.length ? sketchTools.deleteConstraints() : sketchTools.deleteSelected()
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
        const run = () => {
            const action = doc.undo()
            if (action.type === "finish_sketch")
                sketch.open(action.sketch, doc.sketchPlane(action.sketch))
            else if (action.type === "sketch" && sketch.active && sketch.sketchId === action.id)
                sketch.leave()
            else if (action.type === "edit_sketch" && sketch.active && sketch.sketchId === action.sketch)
                sketch.leave()
        }
        // Going back into or out of a sketch from the past fades what was made after it.
        if (next && ((next.type === "finish_sketch" && doc.rewinds(next.sketch))
                     || (next.type === "edit_sketch" && doc.rewinds(0))))
            crossfade(run)
        else
            run()
    }

    // Fusion's Edit Sketch, from a double-click on a sketch in the Timeline or the Browser.
    // A text opened for editing (double click): into its sketch if need be, picked, its words ready to type.
    property var pendingText: null
    function editText(sketchId, curveId) {
        pendingText = { sketch: sketchId, curve: curveId }
        if (!(sketch.active && sketch.sketchId === sketchId))
            editSketch(sketchId)
        pendingTextTimer.restart()
    }
    Timer {
        id: pendingTextTimer
        interval: 60
        repeat: true
        property int tries: 0
        onTriggered: {
            const w = root.pendingText
            if (!w || ++tries > 40) {
                stop()
                tries = 0
                root.pendingText = null
                return
            }
            if (!(sketch.active && sketch.sketchId === w.sketch))
                return
            stop()
            tries = 0
            root.pendingText = null
            sketchTools.cancel()
            selection.selected = [{ kind: "sketch_curve", index: w.curve, sketch: w.sketch }]
            selection.redraw()
            Qt.callLater(() => textEdit.focusFirst())
        }
    }
    // Ctrl+Y (owner, 2026-11-07): put back the newest action Ctrl+Z took back, exactly as it was.
    // The window follows, the other way round from undo: redoing a sketch's start or an Edit Sketch
    // goes into it, redoing a finish leaves it.
    function redoLast() {
        sketch.picking = false
        sketchTools.reset()
        const next = doc.redoNext()
        if (!next.type)
            return
        const run = () => {
            const action = doc.redo()
            if (action.type === "finish_sketch" && sketch.active)
                sketch.leave()
            else if (action.type === "sketch")
                sketch.open(action.id, doc.sketchPlane(action.id))
            else if (action.type === "edit_sketch")
                sketch.open(action.sketch, doc.sketchPlane(action.sketch))
        }
        // Into or out of a sketch, or showing or hiding something: faded, as undo does.
        if (next.type === "visibility" || next.type === "finish_sketch" || next.type === "edit_sketch")
            crossfade(run)
        else
            run()
    }
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
        id: redoKey
        sequence: "Ctrl+Y"
        enabled: undoKey.enabled
        onActivated: root.redoLast()
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
        visible: root.openSketchShown && root.showConstraints
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
                        enabled: root.selConstraints || !!sketchTools.tool
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
    ExtrudeTool {
        id: extrudeTool
        anchors.fill: parent
        theme: theme
        selection: selection
        previewA: extrudePreviewA
        previewB: extrudePreviewB
        wireA: extrudeWireA
        wireB: extrudeWireB
        cutA: extrudeCutA
        cutB: extrudeCutB
        project: p => root.toScreen(p)
        fadeMs: root.fadeMs
        z: 35
        onSay: (text, ms) => sketch.say(text, ms)
    }
    // Model Chamfer and Model Fillet: one tool (EdgeTool.qml), one at a time, sharing the preview's meshes.
    EdgeTool {
        id: chamferTool
        feature: "chamfer"
        anchors.fill: parent
        theme: theme
        selection: selection
        previewFaces: chamferFaces
        previewEdges: chamferEdges
        project: p => root.toScreen(p)
        fadeMs: root.fadeMs
        z: 35
        onSay: (text, ms) => sketch.say(text, ms)
    }
    EdgeTool {
        id: filletTool
        feature: "fillet"
        anchors.fill: parent
        theme: theme
        selection: selection
        previewFaces: chamferFaces
        previewEdges: chamferEdges
        project: p => root.toScreen(p)
        fadeMs: root.fadeMs
        z: 35
        onSay: (text, ms) => sketch.say(text, ms)
    }
    readonly property bool edgePreviewShown: chamferTool.previewShown || filletTool.previewShown
    Binding { target: doc; property: "chamferPreviewShown"; value: root.edgePreviewShown }
    ParametersWindow {
        id: parametersWindow
        theme: theme
        fadeMs: root.fadeMs
        z: 40
        onSay: text => sketch.say(text)
        onImportRequested: {
            parameterImportDialog.currentFolder = doc.filePath ? "file://" + doc.filePath.substring(0, doc.filePath.lastIndexOf("/")) : saveFolder
            dialogLook.setDark(!theme.light)
            parameterImportDialog.open()
        }
        onExportRequested: {
            parameterExportDialog.currentFolder = doc.filePath ? "file://" + doc.filePath.substring(0, doc.filePath.lastIndexOf("/")) : saveFolder
            parameterExportDialog.selectedFile = parameterExportDialog.currentFolder + "/Parameters.ods"
            dialogLook.setDark(!theme.light)
            parameterExportDialog.open()
        }
    }
    // Parameters as spreadsheets (owner, t95u/t96u): OpenDocument first, CSV for Fusion 360.
    FileDialog {
        id: parameterImportDialog
        title: "Import Parameters"
        fileMode: FileDialog.OpenFile
        nameFilters: ["Spreadsheets (*.ods *.csv *.xlsx)", "OpenDocument spreadsheet (*.ods)", "CSV (Fusion 360) (*.csv)", "Excel (*.xlsx)"]
        onAccepted: parametersWindow.importFrom(selectedFile.toString())
    }
    // Picking the other file type in the dialog changes the ending of the name shown.
    Connections {
        target: parameterExportDialog.selectedNameFilter
        function onIndexChanged() {
            const name = parameterExportDialog.selectedFile.toString()
            const want = parameterExportDialog.selectedNameFilter.index === 1 ? ".csv" : ".ods"
            if (/\.(ods|csv)$/i.test(name) && !name.toLowerCase().endsWith(want))
                parameterExportDialog.selectedFile = name.replace(/\.(ods|csv)$/i, want)
        }
    }
    FileDialog {
        id: parameterExportDialog
        title: "Export Parameters"
        fileMode: FileDialog.SaveFile
        nameFilters: ["OpenDocument spreadsheet (*.ods)", "CSV for Fusion 360 (*.csv)"]
        defaultSuffix: "ods"
        onAccepted: {
            let path = selectedFile.toString()
            if (!/\.(ods|csv)$/i.test(path))
                path += selectedNameFilter.index === 1 ? ".csv" : ".ods"
            parametersWindow.exportTo(path)
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
        future: doc.future
        totalSteps: doc.presentCount + doc.future.length
        presentCount: doc.presentCount
        // What can be scrubbed to (owner, 2026-11-07: a translucent fill, newest to oldest, while it's worked out):
        // in a sketch, the history from doc.sketchReadyFrom on (and this opening's own steps, always).
        readyCount: sketch.active ? doc.sketchReadyFrom : doc.modelReadyFrom
        readyDone: sketch.active ? doc.sketchWalkDone : doc.modelBuildDone
        builtCount: sketch.active ? -1 : Math.max(doc.modelBuiltTo, doc.modelBuildingTo)   // the progress fill follows the rebuild itself (GB-062)
        marker: rewind.pos
        markerEnabled: !extrudeTool.open && !chamferTool.open && !filletTool.open && !sketchTools.tool && !doc.working
        frozen: markerHeld || settling.running
        onMarkerDragged: position => rewind.goTo(position)
        onMarkerDropped: rewind.settle()
        sketchId: sketch.active ? sketch.sketchId : 0
        onSketchEditRequested: id => root.editSketch(id)
        onBreakLinkRequested: (sketchId, link) => { breakAsk.sketchId = sketchId; breakAsk.link = link; breakAsk.shown = true }
    }

    // The Timeline's marker (owner, 2026-11-07; history/2026-11-07_timeline-rewind.md): where it is (in the
    // design row's items, in between while dragged), and the design following it. Going over an item
    // takes it back (or puts it back) at once, as Ctrl+Z / Ctrl+Y do, with nothing built again.
    QtObject {
        id: rewind
        property real pos: 0
        property bool moving: false      // the design is being moved by the marker (not Ctrl+Z, a new step...)
        property real glide: 0           // where the settle animation has got to
        // Part way across an extrude: how far it is built (0 none, 1 all), and the split's place in the
        // scene (Y up): the extrude's plane and direction, and how far from it the split is.
        property real built: 0
        property var step: null          // doc.rewindStep for item `stepItem`
        property int stepItem: -1
        property vector3d origin: Qt.vector3d(0, 0, 0)
        property vector3d axis: Qt.vector3d(0, 1, 0)
        property vector3d xDir: Qt.vector3d(1, 0, 0)
        property vector3d yDir: Qt.vector3d(0, 0, -1)
        property vector4d box: Qt.vector4d(-1e9, 1e9, -1e9, 1e9)
        property real spanLow: 0          // the extrude's span along its axis, mm (negative: below its plane)
        property real spanHigh: 0
        readonly property real reach: spanHigh - spanLow
        readonly property real low: built * spanLow
        readonly property real high: built * spanHigh
        readonly property real glowWidth: Math.max(0.3, reach * 0.04)
        readonly property bool showing: built > 0 && built < 1 && stepItem >= 0 && reach > 0 && !framesStep
        // Part way across an Extrude, Chamfer or Fillet (GB-057 pt159u, GB-058 pt160u: "represent the actual object
        // in real time, in real space"): the step grown to `built` as real solids (frames off the main thread,
        // Document::rewindFramesBegin). The plane split above is left for the other steps.
        property bool framesStep: false
        property int frameShown: 0         // the frame drawn (0: none, the design as it is shows)
        readonly property bool framesShowing: built > 0 && built < 1 && stepItem >= 0 && framesStep && frameShown > 0
        function showFrame() {
            frameShown = framesStep && built > 0 && built < 1 ? doc.rewindFramesShow(built, rewindFrameFaces, rewindFrameEdges) : 0
        }
        onBuiltChanged: if (framesStep) showFrame()
        function prepare(k) {
            if (stepItem === k)
                return
            stepItem = k
            const n = historyTimeline.countAt(k + 1) - historyTimeline.countAt(k)
            const st = doc.rewindStep(n)
            step = st
            framesStep = doc.rewindFramesBegin(n)
            if (!st.axis) {
                spanLow = 0
                spanHigh = 0
                return
            }
            const toScene = v => Qt.vector3d(v.x, v.z, -v.y)      // engine (Z up) to the scene (Y up)
            origin = toScene(st.origin)
            axis = toScene(st.axis)
            if (st.box) {
                xDir = toScene(st.xAxis)
                yDir = toScene(st.yAxis)
                box = Qt.vector4d(st.box[0], st.box[1], st.box[2], st.box[3])
            } else {
                box = Qt.vector4d(-1e9, 1e9, -1e9, 1e9)
            }
            spanLow = st.low
            spanHigh = st.high
            doc.fillBodies(afterFaces, afterEdges, st.after)
        }
        Component.onCompleted: Qt.callLater(() => pos = historyTimeline.presentItems)
        onGlideChanged: if (settling.running) goTo(glide)
        // In a sketch (its own row): part way across a step, the sketch is drawn part way to how that step
        // leaves it (Document::setSketchTween), eased so it starts and lands gently.
        readonly property bool inSketch: historyTimeline.sketchId !== 0
        property bool sketchBetween: false
        function drawBetween(k, f) {
            const n = k >= 0 ? historyTimeline.countAt(k + 1) - historyTimeline.countAt(k) : 0
            const e = f * f * (3 - 2 * f)
            const was = sketchBetween
            sketchBetween = k >= 0 && doc.setSketchTween(historyTimeline.sketchId, n, e)
            if (sketchBetween || was) {
                root.drawSketches()
                root.drawProfiles()
                root.refreshDimensions()
            }
        }
        // A sketch opened again (owner, 2026-11-07: the marker moves over all its steps): before this
        // opening the document stays where it is and the sketch is only drawn as it was
        // (Document::prepareSketchPast, worked out once per opening and history).
        property int pastFor: 0
        property int pastAt: -1
        property int pastReady: -1        // doc.sketchReadyFrom when it was worked out
        property int pastCount: 0         // how many of the newest gaps before this opening are known
        property bool pastShowing: false
        // The first gap the marker can go to: before this opening, as far back as the background walk has got.
        readonly property int firstReady: inSketch ? historyTimeline.firstMovable + 1 - Math.max(1, pastCount) : 0
        function preparePast() {
            const live = historyTimeline.firstMovable
            if (!inSketch || live === 0) {
                pastCount = 0
                return
            }
            if (pastFor === historyTimeline.sketchId && pastAt === historyTimeline.countAt(live) && pastReady === doc.sketchReadyFrom)
                return
            const counts = []
            for (let k = 0; k <= live; ++k)
                counts.push(historyTimeline.countAt(k))
            pastFor = historyTimeline.sketchId
            pastAt = historyTimeline.countAt(live)
            pastReady = doc.sketchReadyFrom
            pastCount = doc.prepareSketchPast(pastFor, counts)
        }
        function clearPast() {
            pastFor = 0
            pastAt = -1
            pastReady = -1
            pastCount = 0
            if (pastShowing) {
                pastShowing = false
                doc.showSketchPast(0, -1, 0)
                root.drawSketches()
                root.drawProfiles()
                root.refreshDimensions()
            }
        }
        // Leaving the sketch: its steps behind the marker come back first.
        function sketchToEnd() {
            if (!inSketch)
                return
            settling.stop()
            clearPast()
            const end = historyTimeline.countAt(historyTimeline.items.length)
            if (doc.presentCount < end) {
                moving = true
                doc.rewindTo(end, true)
                moving = false
            }
            drawBetween(-1, 0)
            pos = historyTimeline.presentItems
        }
        function goTo(p) {
            const n = historyTimeline.items.length
            const live = historyTimeline.firstMovable
            if (inSketch && p < live)
                preparePast()
            p = Math.max(inSketch ? Math.min(live, firstReady) : historyTimeline.readyItem, Math.min(n, p))
            if (inSketch && p < live) {
                // Before this opening: the document at its start, the sketch drawn as it was.
                const start = historyTimeline.countAt(live)
                if (doc.presentCount !== start) {
                    moving = true
                    doc.rewindTo(start, true)
                    moving = false
                }
                pos = p
                const j = Math.floor(p), f = p - j
                sketchBetween = false
                doc.setSketchTween(0, 0, 0)
                pastShowing = doc.showSketchPast(historyTimeline.sketchId, j, f * f * (3 - 2 * f))
                root.drawSketches()
                root.drawProfiles()
                root.refreshDimensions()
                return
            }
            if (pastShowing) {
                pastShowing = false
                doc.showSketchPast(0, -1, 0)
                root.drawSketches()
                root.drawProfiles()
                root.refreshDimensions()
            }
            const k = Math.min(n, Math.floor(p))
            const want = historyTimeline.countAt(k)
            if (doc.presentCount !== want) {
                moving = true
                const got = doc.rewindTo(want, inSketch)
                moving = false
                if (got !== want) {
                    // Stopped at a step that can't be taken back or put back: the marker stays there.
                    let at = 0
                    while (at < n && historyTimeline.countAt(at) < got)
                        ++at
                    p = at
                }
                stepItem = -1            // the design moved: the next step's shapes are another's
            }
            pos = p
            const f = p - Math.floor(p)
            if (inSketch) {
                drawBetween(f > 0 && Math.floor(p) < n && doc.presentCount === historyTimeline.countAt(Math.floor(p)) ? Math.floor(p) : -1, f)
                return
            }
            if (f > 0 && Math.floor(p) < n && doc.presentCount === historyTimeline.countAt(Math.floor(p))) {
                prepare(Math.floor(p))
                built = f
            } else {
                built = 0
            }
        }
        function settle() {
            settling.stop()
            glide = pos
            settling.to = Math.round(pos)
            settling.start()
        }
    }
    NumberAnimation {
        id: settling
        target: rewind
        property: "glide"
        duration: 140
        easing.type: Easing.OutCubic
    }
    // Ctrl+Z, Ctrl+Y, a new step, a file opened: the marker goes to where the design now is. So it does
    // when the row changes under it (a sketch opened or left, a run of moves opened out or folded).
    Connections {
        target: doc
        function onRewindFrameReady() { rewind.showFrame() }
        function onHistoryChanged() {
            if (!rewind.moving && !historyTimeline.markerHeld && !settling.running) {
                rewind.stepItem = -1
                rewind.built = 0
                rewind.clearPast()
                Qt.callLater(() => rewind.pos = historyTimeline.presentItems)   // once the Timeline has the new history
            }
        }
    }
    Connections {
        target: historyTimeline
        function onItemsChanged() {
            // Resting on a sketch's earlier steps (only drawn as they were) it stays where it is.
            if (!rewind.moving && !historyTimeline.frozen && !rewind.pastShowing)
                Qt.callLater(() => { if (!historyTimeline.frozen && !rewind.pastShowing) rewind.pos = historyTimeline.presentItems })
        }
        function onFrozenChanged() { doc.holdLive(historyTimeline.frozen && rewind.inSketch) }
    }
    Connections {
        target: sketch
        function onLeaving() { rewind.sketchToEnd() }
    }
    // While sketching, what a constraint, a dimension or Ctrl+Z moves glides there (owner, 2026-11-07: "not
    // taking more than 250 ms"): Document::liveTween, drawn each frame of this.
    property real liveT: 1
    NumberAnimation {
        id: liveGlide
        target: root
        property: "liveT"
        from: 0
        to: 1
        duration: 220
        easing.type: Easing.OutCubic
    }
    onLiveTChanged: {
        if (!liveGlide.running && liveT < 1)
            return
        doc.liveTween(liveT)
        drawSketches()
        drawProfiles()
        refreshDimensions()
    }

    // "Break the link?" (owner, 2026-11-01): the app's own yes / no, in the theme. Asked after
    // Break Link in the timeline is clicked; Yes adds a step to the sketch's history.
    Item {
        id: breakAsk
        property int sketchId: 0
        property int link: 0
        property bool shown: false
        anchors.fill: parent
        z: 90
        opacity: shown ? 1 : 0
        visible: opacity > 0
        focus: shown
        Behavior on opacity { NumberAnimation { duration: 150 } }
        function answer(yes) {
            shown = false
            if (!yes)
                return
            const r = doc.breakLink(sketchId, link)
            sketch.say(r.summary || r.why)
        }
        Rectangle {
            anchors.fill: parent
            color: Qt.rgba(0, 0, 0, 0.35)
            MouseArea { anchors.fill: parent }          // nothing behind it can be clicked
        }
        Rectangle {
            anchors.centerIn: parent
            width: 360
            height: askText.implicitHeight + askSub.implicitHeight + 98
            radius: 12
            color: theme.panelBg
            border.color: theme.borderStrong
            Text {
                id: askText
                x: 22
                y: 20
                width: parent.width - 44
                text: "Do you want to break the link for this projected geometry?"
                color: theme.textPrimary
                font.pixelSize: 14
                font.bold: true
                wrapMode: Text.WordWrap
            }
            Text {
                id: askSub
                x: 22
                y: askText.y + askText.implicitHeight + 8
                width: parent.width - 44
                text: "It stays on the sketch as ordinary geometry and stops following where it came from. Ctrl+Z puts the link back."
                color: theme.textSecondary
                font.pixelSize: 12
                wrapMode: Text.WordWrap
            }
            Row {
                anchors.right: parent.right
                anchors.rightMargin: 18
                anchors.bottom: parent.bottom
                anchors.bottomMargin: 16
                spacing: 10
                Repeater {
                    model: [{ label: "No", yes: false }, { label: "Yes, break it", yes: true }]
                    delegate: Rectangle {
                        required property var modelData
                        width: modelData.yes ? 120 : 70
                        height: 32
                        radius: 8
                        color: area.containsMouse ? theme.controlHover : Qt.rgba(theme.panelRaised.r, theme.panelRaised.g, theme.panelRaised.b, 1)
                        border.width: 1.5
                        border.color: modelData.yes ? theme.danger : theme.border
                        Text {
                            anchors.centerIn: parent
                            text: parent.modelData.label
                            color: parent.modelData.yes ? theme.danger : theme.textPrimary
                            font.pixelSize: 12
                            font.bold: true
                        }
                        MouseArea {
                            id: area
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: breakAsk.answer(parent.modelData.yes)
                        }
                    }
                }
            }
        }
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
        opacity: search.timesOpened >= 3 || search.open || sketch.active || sketch.picking || extrudeTool.open || chamferTool.open || filletTool.open ? 0 : 0.85
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
