import QtQuick

// What a sketch shows (Fusion's Sketch Palette switches): a small Display button in the bottom
// right corner, while sketching, opens a list of tick boxes above it; the list goes again after
// a few seconds without the mouse on it. The list floats over the view, so nothing moves. The
// same switches are in the search.
Item {
    id: menu

    required property var theme
    required property var rows            // [{ name, on, flip }]
    property bool open: false

    width: 214
    height: list.height + 8 + button.height
    onVisibleChanged: open = false

    HoverHandler { id: hover }
    Timer {
        running: menu.open && !hover.hovered
        interval: 3000
        onTriggered: menu.open = false
    }

    Rectangle {
        id: list
        objectName: "displayList"
        anchors.right: parent.right
        y: menu.open ? 0 : 8
        width: parent.width
        height: 6 + menu.rows.length * 28 + 6
        visible: opacity > 0
        opacity: menu.open ? 1 : 0
        radius: 8
        color: Qt.rgba(menu.theme.panelBg.r, menu.theme.panelBg.g, menu.theme.panelBg.b, 0.92)
        border.color: menu.theme.border
        Behavior on opacity { NumberAnimation { duration: 140 } }
        Behavior on y { NumberAnimation { duration: 140; easing.type: Easing.OutCubic } }

        MouseArea {                       // clicks here never reach the sketch behind
            anchors.fill: parent
            acceptedButtons: Qt.AllButtons
        }
        Column {
            x: 6
            y: 6
            Repeater {
                model: menu.rows
                delegate: Rectangle {
                    required property var modelData
                    objectName: "display " + modelData.name
                    width: menu.width - 12
                    height: 28
                    radius: 5
                    color: rowArea.containsMouse ? menu.theme.controlHover : "transparent"
                    Rectangle {
                        x: 6
                        anchors.verticalCenter: parent.verticalCenter
                        width: 16
                        height: 16
                        radius: 3
                        color: modelData.on ? menu.theme.accent : menu.theme.controlBg
                        border.color: modelData.on ? menu.theme.accent : menu.theme.borderStrong
                        Text {
                            anchors.centerIn: parent
                            visible: modelData.on
                            text: "✓"
                            color: menu.theme.panelBg
                            font.pixelSize: 12
                            font.bold: true
                        }
                    }
                    Text {
                        x: 30
                        anchors.verticalCenter: parent.verticalCenter
                        text: modelData.name
                        color: menu.theme.textPrimary
                        font.pixelSize: 12
                    }
                    MouseArea {
                        id: rowArea
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: modelData.flip()
                    }
                }
            }
        }
    }

    Rectangle {
        id: button
        objectName: "displayButton"
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        width: 64
        height: 28
        radius: 14
        color: buttonArea.containsMouse ? menu.theme.controlHover : Qt.rgba(menu.theme.panelBg.r, menu.theme.panelBg.g, menu.theme.panelBg.b, 0.92)
        border.color: menu.open ? menu.theme.accent : menu.theme.border
        Text {
            anchors.centerIn: parent
            text: "Display"
            color: menu.theme.textPrimary
            font.pixelSize: 12
        }
        MouseArea {
            id: buttonArea
            anchors.fill: parent
            hoverEnabled: true
            cursorShape: Qt.PointingHandCursor
            onClicked: menu.open = !menu.open
        }
    }
}
