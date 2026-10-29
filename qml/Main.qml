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
    }

    // Only the space behind the model follows OmaSkins' transparency; models stay solid.
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
            probeExposure: theme.light ? 0.5 : 0.6

        }

        // Camera rig: pivot (what we look at) → yaw → pitch → camera at a distance.
        Node {
            id: pivot
            Node {
                id: yawNode
                eulerRotation.y: navigation.yaw

                // Key light: close to the model (a lamp, not the sun), up and to the viewer's
                // left. Because it's near, its angle changes across each face, so every face
                // runs from brighter to darker instead of one flat colour. Placed at a multiple
                // of the camera distance so the look is the same at any zoom.
                PointLight {
                    id: keyLight
                    // How far the lamp is from what we look at (it moves with the zoom).
                    readonly property real reach: navigation.distance * 0.5 * 1.42
                    position: Qt.vector3d(-0.7, 1.0, 0.75).times(navigation.distance * 0.5)
                    brightness: theme.light ? 1.25 : 1.35
                    // Light fades with distance: full strength at the pivot, brighter on the
                    // near edge of a face, dimmer on the far edge. (Qt scales linearFade by 0.01.)
                    constantFade: 0.2
                    linearFade: 80 / reach
                    quadraticFade: 0
                    castsShadow: !navigation.simpleLook
                    shadowFactor: 35
                    shadowMapQuality: Light.ShadowMapQualityHigh
                    shadowMapFar: navigation.distance * 6
                }
                // Rim light from behind on the right: lifts the dark side's edges off the background.
                DirectionalLight {
                    eulerRotation: Qt.vector3d(-25, 150, 0)
                    brightness: 0.45
                }
                // Soft fill from the viewer's right, so the shadow side keeps its colour.
                DirectionalLight {
                    eulerRotation: Qt.vector3d(-10, 35, 0)
                    brightness: 0.22
                }
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

        FloorGrid {
            visible: navigation.gridVisible
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
        }
    }

    Navigation {
        id: navigation
        anchors.fill: parent
        view: view
        pivot: pivot
        camera: camera
        onHoverAt: (x, y) => selection.hoverAt(x, y)
        onClickAt: (x, y, modifiers) => selection.clickAt(x, y, modifiers)
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
        // onlyOmarchy commands are left out on other systems.
        commands: [
            { name: "Fit to View", hint: "Show the whole model", group: "View", keys: "Double-click middle", run: () => navigation.fit() },
            { name: "Home View", hint: "Back to the starting angle", group: "View", keys: "", run: () => navigation.home() },
            { name: "Pan", hint: navigation.panMode ? "Stop left-drag panning" : "Left-drag moves the view (Esc stops)", group: "View", keys: "", run: () => navigation.panMode = !navigation.panMode },
            { name: theme.light ? "Dark Mode" : "Light Mode", hint: "Switch the look", group: "View", keys: "", run: () => theme.light = !theme.light },
            { name: "Follow Omarchy Theme", onlyOmarchy: true, hint: theme.followSystem ? "Back to OmaMake's own colours" : "Take colours from the current Omarchy theme", group: "View", keys: "", run: () => theme.followSystem = !theme.followSystem },
            { name: "Grid", hint: navigation.gridVisible ? "Hide the floor grid" : "Show the floor grid", group: "View", keys: "", run: () => navigation.gridVisible = !navigation.gridVisible },
            { name: "Simple Look", hint: navigation.simpleLook ? "Back to full shading and shadows" : "Fewer effects, for slower computers", group: "View", keys: "", run: () => navigation.simpleLook = !navigation.simpleLook },
            { name: "Quit OmaMake", hint: "Close the window", group: "App", keys: "Ctrl Q", run: () => Qt.quit() }
        ].filter(c => !c.onlyOmarchy || omarchyTheme.onOmarchy)
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

    // Esc: closes the search or menu first (they handle it), then stops panning,
    // then clears the selection.
    Shortcut {
        sequence: "Esc"
        enabled: (navigation.panMode || selection.selected.length > 0) && !search.open && !modeMenu.open
        onActivated: {
            if (navigation.panMode)
                navigation.panMode = false
            else
                selection.clear()
        }
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
