import QtQuick

// How the origin planes look: right-click Origin (or one of its planes) in the Browser.
//   Cross         full size, through the centre, out to both sides
//   Cube Corner   smaller, on the plus side only, standing apart like the corner of a cube
// The top choice sets every plane and becomes how OmaMake opens; the rows below set one
// plane each (say XY as Cross for a ground, XZ and YZ as Cube Corner so they don't cut it).
// Floats where it was asked for; a click anywhere else or Esc closes it.
Item {
    id: menu

    required property var theme
    property var styles: ({})             // {XY, XZ, YZ} → "cross" / "corner"
    property bool open: false

    signal chose(string plane, string style)   // plane "" = all planes (the default)

    readonly property var names: ({ cross: "Cross", corner: "Cube Corner" })
    readonly property bool allSame: styles.XY === styles.XZ && styles.XZ === styles.YZ

    anchors.fill: parent
    visible: open
    z: 50

    function show(x, y) {
        panel.x = Math.min(x, width - panel.width - 8)
        panel.y = Math.min(y, height - panel.height - 8)
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
        width: 290
        height: list.implicitHeight + 12
        radius: 10
        color: Qt.rgba(menu.theme.panelBg.r, menu.theme.panelBg.g, menu.theme.panelBg.b, 0.97)
        border.color: menu.theme.border

        MouseArea { anchors.fill: parent }

        Column {
            id: list
            x: 6
            y: 6
            width: parent.width - 12

            Caption { text: "Origin planes: all"; detail: "How they look every time OmaMake opens" }
            MenuRow {
                theme: menu.theme
                text: "Cross"
                detail: "Full size, through the centre"
                mark: menu.allSame && menu.styles.XY === "cross" ? "●" : ""
                onActivated: { menu.chose("", "cross"); menu.close() }
            }
            MenuRow {
                theme: menu.theme
                text: "Cube Corner"
                detail: "Smaller, plus side only, apart"
                mark: menu.allSame && menu.styles.XY === "corner" ? "●" : ""
                onActivated: { menu.chose("", "corner"); menu.close() }
            }
            Rectangle {
                width: parent.width
                height: 9
                color: "transparent"
                Rectangle { anchors.centerIn: parent; width: parent.width - 12; height: 1; color: menu.theme.border }
            }
            Caption { text: "Each plane" }
            Repeater {
                model: ["XY", "XZ", "YZ"]
                PlaneRow { required property string modelData; plane: modelData }
            }
        }
    }

    component Caption: Column {
        property string text
        property string detail
        x: 10
        width: parent ? parent.width - 20 : 0
        topPadding: 4
        bottomPadding: 4
        Text { text: parent.text; color: menu.theme.textSecondary; font.pixelSize: 11; font.bold: true }
        Text { visible: parent.detail !== ""; text: parent.detail; color: menu.theme.textMuted; font.pixelSize: 11 }
    }

    // "XY plane" and the two styles side by side; the chosen one lit. Both buttons are as
    // wide as the longer name, so choosing never moves anything.
    component PlaneRow: Item {
        id: planeRow
        property string plane
        width: parent ? parent.width : 0
        height: 34

        Text {
            x: 10
            anchors.verticalCenter: parent.verticalCenter
            text: planeRow.plane + " plane"
            color: menu.theme.textPrimary
            font.pixelSize: 13
        }
        Row {
            anchors.right: parent.right
            anchors.rightMargin: 4
            anchors.verticalCenter: parent.verticalCenter
            spacing: 4
            Repeater {
                model: ["cross", "corner"]
                Rectangle {
                    id: choice
                    required property string modelData
                    readonly property bool on: menu.styles[planeRow.plane] === modelData
                    width: 92
                    height: 26
                    radius: 6
                    color: on ? Qt.rgba(menu.theme.accent.r, menu.theme.accent.g, menu.theme.accent.b, 0.25)
                         : choiceArea.containsMouse ? menu.theme.controlHover : "transparent"
                    border.color: on ? menu.theme.accent : menu.theme.border
                    Text {
                        anchors.centerIn: parent
                        text: menu.names[choice.modelData]
                        color: choice.on ? menu.theme.textPrimary : menu.theme.textSecondary
                        font.pixelSize: 12
                    }
                    MouseArea {
                        id: choiceArea
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: menu.chose(planeRow.plane, choice.modelData)
                    }
                }
            }
        }
    }
}
