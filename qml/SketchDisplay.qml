import QtQuick

// What a sketch shows (Fusion's Sketch Palette switches): a small Display button in the bottom
// right corner, while sketching, opens a list of tick boxes above it; the list goes again after
// a few seconds without the mouse on it. The list floats over the view, so nothing moves. The
// same switches are in the search. The Selection menu (what can be picked while sketching) is the
// same item with another title: a row with `enabled: false` is greyed out, for what doesn't exist yet.
Item {
    id: menu

    required property var theme
    required property var rows            // [{ name, on, flip, enabled (default true) }]
    property string title: "Display"
    property string idName: "display"     // objectName prefix: "<idName>List", "<idName>Button", "<idName> <row>"
    property int buttonWidth: 64
    property bool open: false
    // Rule (owner, 2026-11-01): a menu unfolds toward the middle of the view. This one sits at the
    // bottom, so a nest's rows appear above it (unfoldUp); a menu at a side unfolds downward.
    property bool unfoldUp: true
    property var unfolded: ({})           // names of the nests that are open
    // 0 none, 1 some, 2 all of what's switchable inside a row (a plain row: its own switch).
    function leaves(row) {
        if (!row.children || !row.children.length)
            return row.enabled === false ? [] : [row]
        return row.children.reduce((all, c) => all.concat(leaves(c)), [])
    }
    function stateOf(row) {
        const l = leaves(row), n = l.filter(r => r.on).length
        return n === 0 ? 0 : n === l.length ? 2 : 1
    }
    function pressed(row) {
        if (!row.children || !row.children.length)
            return row.enabled === false ? undefined : row.flip()
        const target = stateOf(row) !== 2
        leaves(row).forEach(r => { if (!!r.on !== target) r.flip() })
    }
    function fold(name) {
        const u = Object.assign({}, unfolded)
        u[name] = !u[name]
        unfolded = u
    }
    // What's showing now: every row whose nests above it are all open.
    readonly property var shown: {
        const out = []
        const add = (list, depth) => list.forEach(r => {
            const kids = r.children && r.children.length > 0
            const open = kids && !!unfolded[r.name]
            if (open && unfoldUp)
                add(r.children, depth + 1)
            out.push({ row: r, depth: depth, kids: kids, open: open, gap: r.divider ? 9 : 0,
                       state: kids ? stateOf(r) : (r.on ? 2 : 0) })
            if (open && !unfoldUp)
                add(r.children, depth + 1)
        })
        add(rows, 0)
        return out
    }

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
        objectName: menu.idName + "List"
        anchors.right: parent.right
        y: menu.open ? 0 : 8
        width: parent.width
        height: 6 + menu.shown.reduce((h, s) => h + 28 + s.gap, 0) + 6
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
                model: menu.shown
                delegate: Item {
                    id: slot
                    required property var modelData
                    width: menu.width - 12
                    height: 28 + modelData.gap
                    Rectangle {           // a line between the items above and this one (a row with `divider`)
                        visible: slot.modelData.gap > 0
                        x: 4
                        y: 4
                        width: parent.width - 8
                        height: 1
                        color: menu.theme.borderStrong
                    }
                    Rectangle {
                    id: rowBox
                    y: slot.modelData.gap
                    readonly property var modelData: slot.modelData
                    readonly property var row: modelData.row
                    objectName: menu.idName + " " + row.name
                    width: menu.width - 12
                    height: 28
                    radius: 5
                    color: rowArea.containsMouse && (row.enabled !== false || !!modelData.kids) ? menu.theme.controlHover : "transparent"
                    readonly property real startX: 4 + modelData.depth * 16
                    MouseArea {
                        id: rowArea
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: (rowBox.row.enabled === false && !rowBox.modelData.kids) ? Qt.ArrowCursor : Qt.PointingHandCursor
                        onClicked: menu.pressed(rowBox.row)
                    }
                    Item {                    // the fold arrow: bare, bright white in dark mode, very dark black in light
                        objectName: menu.idName + "Arrow " + rowBox.row.name
                        visible: !!rowBox.modelData.kids
                        x: rowBox.startX
                        anchors.verticalCenter: parent.verticalCenter
                        width: 18
                        height: 18
                        Text {
                            anchors.centerIn: parent
                            text: rowBox.modelData.open ? (menu.unfoldUp ? "▲" : "▼") : "▶"
                            color: menu.theme.light ? "#000000" : "#FFFFFF"
                            font.pixelSize: 13
                            font.bold: true
                        }
                        MouseArea {
                            anchors.fill: parent
                            anchors.margins: -3
                            cursorShape: Qt.PointingHandCursor
                            onClicked: menu.fold(rowBox.row.name)
                        }
                    }
                    Item {
                        id: tick
                        opacity: (rowBox.row.enabled === false && !rowBox.modelData.kids) ? 0.38 : 1
                        x: rowBox.startX + 22
                        anchors.verticalCenter: parent.verticalCenter
                        width: 16
                        height: 16
                        Rectangle {
                            anchors.fill: parent
                            radius: 3
                            color: rowBox.modelData.state > 0 ? menu.theme.accent : menu.theme.controlBg
                            border.color: rowBox.modelData.state > 0 ? menu.theme.accent : menu.theme.borderStrong
                        }
                        Text {
                            anchors.centerIn: parent
                            visible: rowBox.modelData.state === 2
                            text: "✓"
                            color: menu.theme.panelBg
                            font.pixelSize: 12
                            font.bold: true
                        }
                        Rectangle {           // some of what's inside is on
                            anchors.centerIn: parent
                            visible: rowBox.modelData.state === 1
                            width: 6
                            height: 6
                            radius: 3
                            color: menu.theme.panelBg
                        }
                    }
                    Text {
                        x: rowBox.startX + 46
                        anchors.verticalCenter: parent.verticalCenter
                        opacity: rowBox.row.enabled === false ? 0.38 : 1
                        text: rowBox.row.name
                        color: menu.theme.textPrimary
                        font.pixelSize: 12
                    }
                    }
                }
            }
        }
    }

    Rectangle {
        id: button
        objectName: menu.idName + "Button"
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        width: menu.buttonWidth
        height: 28
        radius: 14
        color: buttonArea.containsMouse ? menu.theme.controlHover : Qt.rgba(menu.theme.panelBg.r, menu.theme.panelBg.g, menu.theme.panelBg.b, 0.92)
        border.color: menu.open ? menu.theme.accent : menu.theme.border
        Text {
            anchors.centerIn: parent
            text: menu.title
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
