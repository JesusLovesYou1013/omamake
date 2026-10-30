import QtQuick

// One row of a floating menu (ModeMenu, PlaneStyleMenu): a mark (● or ✓), the text, and
// an optional smaller line under it.
Rectangle {
    id: row
    required property var theme
    property string text
    property string detail
    property string mark
    signal activated()

    width: parent ? parent.width : 0
    height: !visible ? 0 : detail ? 44 : 32
    radius: 6
    color: rowArea.containsMouse && enabled ? theme.controlHover : "transparent"
    opacity: enabled ? 1 : 0.5

    Text {
        x: 10
        width: 16
        anchors.verticalCenter: parent.verticalCenter
        text: row.mark
        color: row.theme.accent
        font.pixelSize: 12
    }
    Column {
        x: 30
        anchors.verticalCenter: parent.verticalCenter
        Text { text: row.text; color: row.theme.textPrimary; font.pixelSize: 13 }
        Text { visible: row.detail !== ""; text: row.detail; color: row.theme.textMuted; font.pixelSize: 11 }
    }
    MouseArea {
        id: rowArea
        anchors.fill: parent
        hoverEnabled: true
        enabled: row.enabled
        cursorShape: Qt.PointingHandCursor
        onClicked: row.activated()
    }
}
