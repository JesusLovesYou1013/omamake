import QtQuick

// Top centre, only while sketching or picking a sketch plane:
//   sketching   "SKETCH" and the Finish Sketch button (Fusion's green tick)
//   picking     what to click, and Cancel
// A short note shows under it when a pick can't start a sketch. Fixed size, so
// switching between the two never moves or resizes anything.
Item {
    id: bar

    required property var theme
    required property var sketch

    width: 400
    height: 40 + 8 + 30

    Rectangle {
        id: pill
        width: parent.width
        height: 40
        radius: height / 2
        color: Qt.rgba(bar.theme.panelBg.r, bar.theme.panelBg.g, bar.theme.panelBg.b, 0.92)
        border.color: bar.sketch.active ? bar.theme.success : bar.theme.borderStrong
        opacity: bar.sketch.active || bar.sketch.picking ? 1 : 0
        visible: opacity > 0
        Behavior on opacity { NumberAnimation { duration: 150 } }

        Text {
            anchors.left: parent.left
            anchors.leftMargin: 18
            anchors.right: button.left
            anchors.rightMargin: 12
            anchors.verticalCenter: parent.verticalCenter
            elide: Text.ElideRight
            text: bar.sketch.active ? "<b>SKETCH</b>" : "Click a flat face, or empty space"
            textFormat: Text.StyledText
            color: bar.sketch.active ? bar.theme.textPrimary : bar.theme.textSecondary
            font.pixelSize: 13
        }

        Rectangle {
            id: button
            anchors.right: parent.right
            anchors.rightMargin: 5
            anchors.verticalCenter: parent.verticalCenter
            width: 140
            height: 30
            radius: height / 2
            color: bar.sketch.active ? (area.containsMouse ? Qt.lighter(bar.theme.success, 1.15) : bar.theme.success)
                                     : (area.containsMouse ? bar.theme.controlHover : bar.theme.controlBg)
            Behavior on color { ColorAnimation { duration: 120 } }

            Text {
                anchors.centerIn: parent
                text: bar.sketch.active ? "✓  Finish Sketch" : "Cancel   Esc"
                color: bar.sketch.active ? bar.theme.windowBg : bar.theme.textPrimary
                font.pixelSize: 13
                font.bold: bar.sketch.active
            }
            MouseArea {
                id: area
                anchors.fill: parent
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                onClicked: bar.sketch.active ? bar.sketch.finish() : bar.sketch.picking = false
            }
        }
    }

    Rectangle {
        anchors.top: pill.bottom
        anchors.topMargin: 8
        anchors.horizontalCenter: parent.horizontalCenter
        width: note.implicitWidth + 24
        height: 30
        radius: height / 2
        color: Qt.rgba(bar.theme.panelBg.r, bar.theme.panelBg.g, bar.theme.panelBg.b, 0.92)
        border.color: bar.theme.warning
        opacity: bar.sketch.message !== "" ? 1 : 0
        visible: opacity > 0
        Behavior on opacity { NumberAnimation { duration: 150 } }
        Text {
            id: note
            anchors.centerIn: parent
            text: bar.sketch.message
            color: bar.theme.textPrimary
            font.pixelSize: 13
        }
    }
}
