import QtQuick
import QtQuick.Window
import QtQuick3D
import QtCore
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

        FloorGrid {
            visible: navigation.gridVisible
            theme: theme
            fadeDistance: Math.max(3000, navigation.distance * 4)
        }

        DirectionalLight {
            eulerRotation: Qt.vector3d(-50, -35, 0)
            brightness: 1.1
            castsShadow: !navigation.simpleLook
            shadowFactor: 45
            shadowMapQuality: Light.ShadowMapQualityHigh
        }
        DirectionalLight {
            eulerRotation: Qt.vector3d(-20, 140, 0)
            brightness: 0.45
        }
        DirectionalLight {
            eulerRotation: Qt.vector3d(70, 0, 0)
            brightness: 0.2
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
        anchors.top: parent.top
        anchors.right: parent.right
        anchors.margins: 10
        theme: theme
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
        commands: [
            { name: "Fit to View", hint: "Show the whole model", group: "View", keys: "Double-click middle", run: () => navigation.fit() },
            { name: "Home View", hint: "Back to the starting angle", group: "View", keys: "", run: () => navigation.home() },
            { name: "Pan", hint: navigation.panMode ? "Stop left-drag panning" : "Left-drag moves the view (Esc stops)", group: "View", keys: "", run: () => navigation.panMode = !navigation.panMode },
            { name: theme.light ? "Dark Mode" : "Light Mode", hint: "Switch the look", group: "View", keys: "", run: () => theme.light = !theme.light },
            { name: "Grid", hint: navigation.gridVisible ? "Hide the floor grid" : "Show the floor grid", group: "View", keys: "", run: () => navigation.gridVisible = !navigation.gridVisible },
            { name: "Simple Look", hint: navigation.simpleLook ? "Back to full shading and shadows" : "Fewer effects, for slower computers", group: "View", keys: "", run: () => navigation.simpleLook = !navigation.simpleLook },
            { name: "Quit OmaMake", hint: "Close the window", group: "App", keys: "Ctrl Q", run: () => Qt.quit() }
        ]
    }

    Shortcut {
        sequence: "Alt+Space"
        onActivated: search.toggle()
    }
    Shortcut {
        sequence: "Esc"
        enabled: navigation.panMode && !search.open
        onActivated: navigation.panMode = false
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
