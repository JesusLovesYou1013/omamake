import QtQuick

// The look menu: right-click the moon/sun button. Floats under it; a click
// anywhere else or Esc closes it.
Item {
    id: menu

    required property var theme
    property Item anchorItem          // the button it opens under
    property bool open: false

    anchors.fill: parent
    visible: open
    z: 50

    // Placed when shown: the button's position isn't known when this is created.
    function show() {
        const at = anchorItem ? anchorItem.mapToItem(menu, anchorItem.width, anchorItem.height)
                              : Qt.point(width - 10, 40)
        panel.x = at.x - panel.width
        panel.y = at.y + 6
        open = true
        forceActiveFocus()
    }
    function close() { open = false }

    Keys.onEscapePressed: close()

    MouseArea {
        anchors.fill: parent
        acceptedButtons: Qt.AllButtons
        onPressed: menu.close()
    }

    Rectangle {
        id: panel
        width: 250
        height: list.implicitHeight + 12
        radius: 10
        color: Qt.rgba(menu.theme.panelBg.r, menu.theme.panelBg.g, menu.theme.panelBg.b, 0.97)
        border.color: menu.theme.border

        // Swallow clicks on the panel's padding so they don't close it.
        MouseArea { anchors.fill: parent }

        Column {
            id: list
            x: 6
            y: 6
            width: parent.width - 12

            MenuRow {
                theme: menu.theme
                text: "Dark"
                mark: !menu.theme.light ? "●" : ""
                onActivated: menu.theme.light = false
            }
            MenuRow {
                theme: menu.theme
                text: "Light"
                mark: menu.theme.light ? "●" : ""
                onActivated: menu.theme.light = true
            }
            // Only offered on Omarchy.
            Rectangle {
                visible: omarchyTheme.onOmarchy
                width: parent.width
                height: 9
                color: "transparent"
                Rectangle { anchors.centerIn: parent; width: parent.width - 12; height: 1; color: menu.theme.border }
            }
            MenuRow {
                visible: omarchyTheme.onOmarchy
                theme: menu.theme
                text: "Follow Omarchy theme"
                detail: !omarchyTheme.available ? "No Omarchy theme found"
                      : omarchyTheme.name ? "Now: " + omarchyTheme.name : ""
                mark: menu.theme.followSystem ? "✓" : ""
                enabled: omarchyTheme.available
                onActivated: menu.theme.followSystem = !menu.theme.followSystem
            }
        }
    }

    component MenuRow: Rectangle {
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
}
