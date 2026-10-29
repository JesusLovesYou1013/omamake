import QtQuick

// The Browser (Fusion's name): the design's tree on the left, top down. The root
// component holds Origin, Bodies, Sketches and further components, which can hold
// the same again, nested as deep as wanted (Fusion's "hybrid design", always on).
//   arrow tab, middle of the left edge   shows or hides the tree; it never moves
//   click a row with children    open or close it
//   round button on a component  make it the active one: new sketches and components go in it
// No panel behind it: each row is its own floating button, so the 3D space shows
// around them. Longer than the window, the list scrolls.
Item {
    id: browser

    required property var theme
    property bool shown: true

    readonly property int rowHeight: 28
    readonly property int treeWidth: 250
    readonly property int indent: 14

    // Open or closed, by node key. Unlisted: components and folders open, the rest closed.
    property var expanded: ({})

    function isOpen(node) {
        return node.key in expanded ? expanded[node.key] : (node.kind === "component" || node.kind === "folder")
    }

    function toggle(node) {
        const next = Object.assign({}, expanded)
        next[node.key] = !isOpen(node)
        expanded = next
    }

    // The tree as the rows on screen: [{node, depth}], open branches only.
    readonly property var rows: {
        const out = []
        const walk = (node, depth) => {
            out.push({ node: node, depth: depth })
            if (isOpen(node))
                for (const child of node.children)
                    walk(child, depth + 1)
        }
        walk(doc.browser, 0)
        return out
    }

    // The tab on the edge, halfway down: always in the same place, whatever the tree does.
    Rectangle {
        id: tab
        x: -8
        anchors.verticalCenter: parent.verticalCenter
        width: 26
        height: 40
        radius: 8
        color: tabArea.containsMouse ? browser.theme.controlHover
             : Qt.rgba(browser.theme.panelRaised.r, browser.theme.panelRaised.g, browser.theme.panelRaised.b, 0.9)
        border.color: browser.theme.borderStrong
        Behavior on color { ColorAnimation { duration: 120 } }

        Text {
            x: 8
            width: parent.width - 8
            height: parent.height
            horizontalAlignment: Text.AlignHCenter
            verticalAlignment: Text.AlignVCenter
            text: browser.shown ? "‹" : "›"
            color: tabArea.containsMouse ? browser.theme.textPrimary : browser.theme.textSecondary
            font.pixelSize: 20
        }
        MouseArea {
            id: tabArea
            anchors.fill: parent
            hoverEnabled: true
            cursorShape: Qt.PointingHandCursor
            onClicked: browser.shown = !browser.shown
        }
    }

    Flickable {
        id: list
        x: browser.shown ? tab.x + tab.width + 16 : -browser.treeWidth - 10   // a clear gap after the tab
        y: 14
        width: browser.treeWidth
        height: Math.min(contentHeight, browser.height - y - 70)
        contentHeight: column.implicitHeight
        interactive: contentHeight > height     // otherwise clicks and the wheel pass through
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        opacity: browser.shown ? 1 : 0
        visible: opacity > 0
        Behavior on x { NumberAnimation { duration: 180; easing.type: Easing.OutCubic } }
        Behavior on opacity { NumberAnimation { duration: 180 } }

        Column {
            id: column
            spacing: 3

            Repeater {
                model: browser.rows

                Rectangle {
                    id: row
                    required property var modelData
                    readonly property var node: modelData.node
                    readonly property bool parentNode: node.children.length > 0
                    readonly property bool isComponent: node.kind === "component"
                    readonly property bool active: isComponent && node.id === doc.activeComponent

                    x: modelData.depth * browser.indent
                    width: browser.treeWidth - x
                    height: browser.rowHeight
                    radius: 7
                    color: rowArea.containsMouse ? browser.theme.controlHover
                         : Qt.rgba(browser.theme.panelRaised.r, browser.theme.panelRaised.g, browser.theme.panelRaised.b, 0.88)
                    border.color: active ? browser.theme.accent
                                : Qt.rgba(browser.theme.border.r, browser.theme.border.g, browser.theme.border.b, 0.8)

                    MouseArea {
                        id: rowArea
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: row.parentNode ? Qt.PointingHandCursor : Qt.ArrowCursor
                        onClicked: if (row.parentNode) browser.toggle(row.node)
                    }

                    // Open/closed arrow.
                    Text {
                        id: arrow
                        x: 6
                        width: 12
                        height: parent.height
                        verticalAlignment: Text.AlignVCenter
                        text: row.parentNode ? (browser.isOpen(row.node) ? "▾" : "▸") : ""
                        color: browser.theme.textMuted
                        font.pixelSize: 11
                    }

                    BrowserIcon {
                        id: icon
                        x: arrow.x + arrow.width + 2
                        anchors.verticalCenter: parent.verticalCenter
                        theme: browser.theme
                        kind: row.node.kind
                        name: row.node.name
                    }

                    Text {
                        anchors.left: icon.right
                        anchors.leftMargin: 7
                        anchors.right: radio.left
                        anchors.rightMargin: 6
                        height: parent.height
                        verticalAlignment: Text.AlignVCenter
                        elide: Text.ElideRight
                        text: row.node.name
                        color: row.node.kind === "folder" || row.node.kind === "origin" ? browser.theme.textSecondary
                                                                                         : browser.theme.textPrimary
                        font.pixelSize: 13
                        font.bold: row.active
                    }

                    // Fusion's activate button, on components only.
                    Rectangle {
                        id: radio
                        anchors.right: parent.right
                        anchors.rightMargin: 8
                        anchors.verticalCenter: parent.verticalCenter
                        width: row.isComponent ? 14 : 0
                        height: 14
                        radius: 7
                        visible: row.isComponent
                        color: "transparent"
                        border.width: 1.5
                        border.color: row.active ? browser.theme.accent
                                    : radioArea.containsMouse ? browser.theme.textPrimary : browser.theme.textMuted
                        Rectangle {
                            anchors.centerIn: parent
                            width: 6
                            height: 6
                            radius: 3
                            color: browser.theme.accent
                            visible: row.active
                        }
                        MouseArea {
                            id: radioArea
                            anchors.fill: parent
                            anchors.margins: -5
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: doc.activeComponent = row.node.id
                        }
                    }
                }
            }
        }
    }
}
