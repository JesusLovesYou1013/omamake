import QtQuick

// The Alt+Space command search: slides down from the window's top edge.
// Type to filter, ↑ ↓ to choose, Enter to run, Esc to close.
Item {
    id: search

    required property var theme
    property var commands: []
    property bool open: false
    property int timesOpened: 0          // remembered (Main.qml Settings); the hint goes after 3
    property int current: 0

    readonly property var matches: {
        const q = input.text.trim().toLowerCase()
        return commands.filter(c => q === "" || c.name.toLowerCase().includes(q) || c.hint.toLowerCase().includes(q))
    }

    anchors.fill: parent
    visible: panel.y > -panel.height

    function toggle() {
        open = !open
        if (open) {
            timesOpened++
            input.text = ""
            current = 0
            input.forceActiveFocus()
        }
    }

    function run(index) {
        const command = matches[index]
        if (!command)
            return
        open = false
        command.run()
    }

    // Name with the typed part in bold.
    function marked(name) {
        const q = input.text.trim()
        const at = q === "" ? -1 : name.toLowerCase().indexOf(q.toLowerCase())
        if (at < 0)
            return name
        return name.slice(0, at) + "<b>" + name.slice(at, at + q.length) + "</b>" + name.slice(at + q.length)
    }

    // Clicking outside the panel closes it.
    MouseArea {
        anchors.fill: parent
        enabled: search.open
        onClicked: search.open = false
    }

    Rectangle {
        id: panel
        width: Math.min(620, parent.width - 32)
        height: column.implicitHeight
        anchors.horizontalCenter: parent.horizontalCenter
        y: search.open ? 0 : -height - 20
        Behavior on y { NumberAnimation { duration: 160; easing.type: Easing.OutCubic } }
        color: Qt.rgba(theme.panelBg.r, theme.panelBg.g, theme.panelBg.b, 0.97)
        border.color: theme.border
        bottomLeftRadius: 14
        bottomRightRadius: 14

        MouseArea { anchors.fill: parent }   // clicks inside don't close it

        Column {
            id: column
            width: parent.width

            Item {
                width: parent.width
                height: 56
                TextInput {
                    id: input
                    anchors.fill: parent
                    anchors.leftMargin: 20
                    anchors.rightMargin: 20
                    verticalAlignment: TextInput.AlignVCenter
                    color: theme.textPrimary
                    selectionColor: theme.accent
                    font.pixelSize: 18
                    focus: search.open
                    onTextChanged: search.current = 0
                    Keys.onEscapePressed: search.open = false
                    Keys.onUpPressed: search.current = Math.max(0, search.current - 1)
                    Keys.onDownPressed: search.current = Math.min(search.matches.length - 1, search.current + 1)
                    Keys.onReturnPressed: search.run(search.current)
                    Keys.onEnterPressed: search.run(search.current)

                    Text {
                        anchors.verticalCenter: parent.verticalCenter
                        visible: input.text === ""
                        text: "Search every command…"
                        color: theme.textMuted
                        font.pixelSize: 18
                    }
                }
            }

            Rectangle { width: parent.width; height: 1; color: theme.border }

            Repeater {
                model: search.matches
                delegate: Rectangle {
                    required property var modelData
                    required property int index
                    width: column.width - 12
                    x: 6
                    height: 52
                    radius: 10
                    color: index === search.current ? Qt.rgba(theme.accent.r, theme.accent.g, theme.accent.b, 0.16) : "transparent"

                    Column {
                        anchors.left: parent.left
                        anchors.leftMargin: 14
                        anchors.verticalCenter: parent.verticalCenter
                        spacing: 2
                        Text {
                            text: search.marked(modelData.name)
                            textFormat: Text.StyledText
                            color: theme.textPrimary
                            font.pixelSize: 15
                        }
                        Text {
                            text: modelData.hint + "  ·  " + modelData.group
                            color: theme.textSecondary
                            font.pixelSize: 12
                        }
                    }
                    Text {
                        anchors.right: parent.right
                        anchors.rightMargin: 14
                        anchors.verticalCenter: parent.verticalCenter
                        text: modelData.keys
                        color: theme.textMuted
                        font.pixelSize: 12
                    }
                    MouseArea {
                        anchors.fill: parent
                        hoverEnabled: true
                        onEntered: search.current = index
                        onClicked: search.run(index)
                    }
                }
            }

            Text {
                visible: search.matches.length === 0
                width: parent.width
                height: 48
                horizontalAlignment: Text.AlignHCenter
                verticalAlignment: Text.AlignVCenter
                text: "Nothing matches yet"
                color: theme.textMuted
                font.pixelSize: 13
            }

            Rectangle { width: parent.width; height: 1; color: theme.border }

            Item {
                width: parent.width
                height: 36
                Text {
                    anchors.left: parent.left
                    anchors.leftMargin: 20
                    anchors.verticalCenter: parent.verticalCenter
                    text: "↑ ↓ choose     Enter run     Esc close"
                    color: theme.textMuted
                    font.pixelSize: 12
                }
            }
        }
    }
}
