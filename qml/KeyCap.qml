import QtQuick

// A keyboard key, e.g. "Alt".
Rectangle {
    required property var theme
    property alias text: label.text

    anchors.verticalCenter: parent ? parent.verticalCenter : undefined
    width: label.implicitWidth + 14
    height: 22
    radius: 6
    color: theme.controlBg
    border.color: theme.borderStrong

    Text {
        id: label
        anchors.centerIn: parent
        color: theme.textPrimary
        font.family: "monospace"
        font.pixelSize: 12
    }
}
