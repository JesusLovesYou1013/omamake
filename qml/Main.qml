import QtQuick
import QtQuick.Window
import QtQuick3D
import QtCore
import QtQuick3D.Helpers
import OmaMake

// OmaMake's window: nothing but the 3D space, edge to edge. Every command lives
// behind Alt+Space. Anything added later floats over the viewport and hides away.
Window {
    id: root
    width: 1400
    height: 900
    visible: true
    title: "OmaMake"
    color: "transparent"

    Theme { id: theme }

    // Remembered between launches.
    Settings {
        location: StandardPaths.writableLocation(StandardPaths.ConfigLocation) + "/omamake/omamake.conf"
        category: "look"
        property alias light: theme.light
        property alias followSystem: theme.followSystem
        property alias lightFollowsView: root.lightFollowsView
        property alias browserShown: browserTree.shown
    }

    // Only the space behind the model follows OmaSkins' transparency; models stay solid.
    // The lamp turns with the view, or stays put in the room (search "Light Follows View").
    property bool lightFollowsView: true

    readonly property real spaceOpacity: root.active ? transparency.focused : transparency.unfocused

    View3D {
        id: view
        anchors.fill: parent
        camera: camera

        environment: SceneEnvironment {
            backgroundMode: SceneEnvironment.Color
            clearColor: Qt.rgba(theme.windowBg.r, theme.windowBg.g, theme.windowBg.b, root.spaceOpacity)
            antialiasingMode: navigation.simpleLook ? SceneEnvironment.NoAA : SceneEnvironment.MSAA
            antialiasingQuality: SceneEnvironment.High
            aoEnabled: !navigation.simpleLook
            aoStrength: 60
            aoDistance: 40
            aoSoftness: 40
            // Soft studio light from all around (sky above, ground below): gives every
            // face a gentle glow and sheen. Only light comes from it; the background stays ours.
            lightProbe: navigation.simpleLook ? null : studio
            probeExposure: theme.light ? 0.3 : 0.35   // low: flat all-round light would wash out the lamp's gradient

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
                        clipNear: 1
                        clipFar: 200000
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
                textureQuality: ProceduralSkyTextureData.SkyTextureQualityLow
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

            Model {
                geometry: MeshGeometry { id: faces }
                materials: PrincipledMaterial {
                    baseColor: theme.model
                    metalness: 0.08
                    roughness: 0.42
                }
                castsShadows: true
                receivesShadows: true
            }
            // Selected parts, then the hovered part on top (Selection.qml fills these).
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
            Model {
                geometry: MeshGeometry { id: edges; lines: true }
                depthBias: -40
                materials: PrincipledMaterial {
                    lighting: PrincipledMaterial.NoLighting
                    baseColor: theme.modelEdge
                }
            }
            SketchGrid {
                theme: theme
                plane: sketch.plane
                fadeDistance: Math.max(3000, navigation.distance * 4)
            }
        }
    }

    Navigation {
        id: navigation
        anchors.fill: parent
        view: view
        pivot: pivot
        camera: camera
        onHoverAt: (x, y) => selection.hoverAt(x, y)
        onClickAt: (x, y, modifiers) => sketch.picking ? sketch.startAt(x, y) : selection.clickAt(x, y, modifiers)
        onTripleClickAt: (x, y) => { if (!sketch.active && !sketch.picking) sketch.startAt(x, y) }
        onHoverLeft: selection.clearHover()
    }

    Selection {
        id: selection
        view: view
        camera: camera
        nav: navigation
        hoverGeometry: hoverParts
        selectedGeometry: selectedParts
    }

    Browser {
        id: browserTree
        anchors.left: parent.left
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        width: 300
        theme: theme
    }

    Sketch {
        id: sketch
        nav: navigation
        selection: selection
    }

    SketchBar {
        anchors.top: parent.top
        anchors.topMargin: 14
        anchors.horizontalCenter: parent.horizontalCenter
        theme: theme
        sketch: sketch
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
    }

    CommandSearch {
        id: search
        theme: theme
        // Things you DO, not ways to look around: view moves stay on the mouse and the
        // view cube (owner, 2026-10-29). Each command says which modes it shows in (Model,
        // Sketch); onlyOmarchy commands are left out on other systems. Names are Fusion's
        // exact ones wherever Fusion has the function (history/DUNE-VS-FUSION.md).
        commands: [
            { name: theme.light ? "Dark Mode" : "Light Mode", hint: "Switch the look", group: "Look", modes: both, keys: "", run: () => theme.light = !theme.light },
            { name: "Follow Omarchy Theme", onlyOmarchy: true, hint: theme.followSystem ? "Back to OmaMake's own colours" : "Take colours from the current Omarchy theme", group: "Look", modes: both, keys: "", run: () => theme.followSystem = !theme.followSystem },
            { name: "Light Follows View", hint: root.lightFollowsView ? "On: the lamp turns with you. Turn off to keep it fixed in the room" : "Off: the lamp stays put in the room. Turn on to have it turn with you", group: "Look", modes: both, keys: "", run: () => root.lightFollowsView = !root.lightFollowsView },
            { name: "Grid", hint: navigation.gridVisible ? "Hide the floor grid" : "Show the floor grid", group: "Look", modes: ["model"], keys: "", run: () => navigation.gridVisible = !navigation.gridVisible },
            { name: "Simple Look", hint: navigation.simpleLook ? "Back to full shading and shadows" : "Fewer effects, for slower computers", group: "Look", modes: both, keys: "", run: () => navigation.simpleLook = !navigation.simpleLook },
            { name: "New Component", hint: "An empty component inside the active one, made active", group: "Assemble", modes: ["model"], keys: "", run: () => doc.newComponent() },
            { name: "Browser", hint: browserTree.shown ? "Hide the design tree" : "Show the design tree", group: "Look", modes: both, keys: "", run: () => browserTree.shown = !browserTree.shown },
            { name: "Create Sketch", hint: "Pick a flat face, or empty space for an origin plane (or triple-click one)", group: "Create", modes: ["model"], keys: "", run: () => sketch.picking = true },
            { name: "Finish Sketch", hint: "Back to the model", group: "Sketch", modes: ["sketch"], keys: "", run: () => sketch.finish() },
            { name: "Quit OmaMake", hint: "Close the window", group: "App", modes: both, keys: "Ctrl Q", run: () => Qt.quit() }
        ].filter(c => (!c.onlyOmarchy || omarchyTheme.onOmarchy) && c.modes.includes(sketch.active ? "sketch" : "model"))
        readonly property var both: ["model", "sketch"]
    }

    Shortcut {
        sequence: "Alt+Space"
        onActivated: search.toggle()
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
                 && !search.open && !modeMenu.open
        onActivated: {
            if (sketch.picking)
                sketch.picking = false
            else if (navigation.panMode)
                navigation.panMode = false
            else if (sketch.active)
                sketch.finish()
            else
                selection.clear()
        }
    }
    // Ctrl+Z in a sketch: undo its creation, leaving no sketch in the history (owner, 2026-10-29).
    Shortcut {
        id: undoKey
        sequence: "Ctrl+Z"
        enabled: sketch.active && !search.open && !modeMenu.open
        onActivated: sketch.undo()
    }
    Shortcut {
        sequence: "Ctrl+Q"
        onActivated: Qt.quit()
    }

    // A faint hint until the search has been opened once.
    Rectangle {
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.bottom: parent.bottom
        anchors.bottomMargin: 26
        width: hintRow.implicitWidth + 28
        height: 34
        radius: height / 2
        color: Qt.rgba(theme.panelBg.r, theme.panelBg.g, theme.panelBg.b, 0.6)
        border.color: Qt.rgba(theme.border.r, theme.border.g, theme.border.b, 0.6)
        opacity: search.usedOnce ? 0 : 0.85
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
