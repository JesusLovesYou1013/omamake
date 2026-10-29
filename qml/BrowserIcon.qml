import QtQuick

// The small picture before each Browser row, drawn from plain shapes.
Item {
    id: icon

    required property var theme
    required property string kind
    property string name: ""

    width: 14
    height: 14

    readonly property color axisColor: name === "X" ? theme.axisX : name === "Y" ? theme.axisY : theme.accent

    // Component: two stacked boxes.
    Rectangle {
        visible: icon.kind === "component"
        x: 3; y: 0; width: 11; height: 11; radius: 2
        color: "transparent"
        border.color: icon.theme.accent
        border.width: 1.5
    }
    Rectangle {
        visible: icon.kind === "component"
        x: 0; y: 3; width: 11; height: 11; radius: 2
        color: Qt.rgba(icon.theme.accent.r, icon.theme.accent.g, icon.theme.accent.b, 0.35)
        border.color: icon.theme.accent
        border.width: 1.5
    }
    // Folder.
    Rectangle {
        visible: icon.kind === "folder"
        x: 0; y: 2; width: 6; height: 4; radius: 1
        color: icon.theme.textMuted
    }
    Rectangle {
        visible: icon.kind === "folder"
        x: 0; y: 4; width: 14; height: 9; radius: 2
        color: icon.theme.textMuted
    }
    // Body: a solid block in the model's colour.
    Rectangle {
        visible: icon.kind === "body"
        x: 1; y: 1; width: 12; height: 12; radius: 2
        color: icon.theme.model
        border.color: icon.theme.modelEdge
    }
    // Sketch: an outline in the Finish Sketch green.
    Rectangle {
        visible: icon.kind === "sketch"
        x: 1; y: 1; width: 12; height: 12; radius: 2
        color: "transparent"
        border.color: icon.theme.success
        border.width: 1.5
    }
    Rectangle {
        visible: icon.kind === "sketch"
        x: 4; y: 4; width: 6; height: 6; radius: 3
        color: icon.theme.success
    }
    // Origin and its centre point: a ring, filled for the point.
    Rectangle {
        visible: icon.kind === "origin" || icon.kind === "point"
        x: 2; y: 2; width: 10; height: 10; radius: 5
        color: icon.kind === "point" ? icon.theme.textSecondary : "transparent"
        border.color: icon.theme.textSecondary
        border.width: 1.5
    }
    // Axis: a line in the axis colour.
    Rectangle {
        visible: icon.kind === "axis"
        x: 0; y: 6; width: 14; height: 2; radius: 1
        color: icon.axisColor
    }
    // Plane: a tilted square.
    Rectangle {
        visible: icon.kind === "plane"
        x: 2; y: 2; width: 10; height: 10; radius: 1
        rotation: 45
        color: Qt.rgba(icon.theme.warning.r, icon.theme.warning.g, icon.theme.warning.b, 0.3)
        border.color: icon.theme.warning
    }
}
