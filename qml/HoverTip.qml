import QtQuick

// What a button does, under it, once the mouse has rested on it a while (owner, 2026-10-30:
// about 2 s, so it never gets in the way once the buttons are known). Gone when the mouse
// leaves or clicks, and stays gone until the mouse comes back. Shown on the window itself, over
// everything else.
Rectangle {
    id: tip

    required property var theme
    required property MouseArea area     // the button's, with hoverEnabled
    property string title: ""
    property string text: ""
    property int delay: 2000

    property bool wanted: false
    Timer {
        id: wait
        interval: tip.delay
        onTriggered: {
            const w = tip.parent, at = tip.area.mapToItem(w, tip.area.width / 2, tip.area.height)
            tip.x = Math.max(8, Math.min(w.width - tip.width - 8, at.x - tip.width / 2))
            tip.y = at.y + 8
            tip.wanted = true
        }
    }
    Connections {
        target: tip.area
        function onContainsMouseChanged() {
            tip.wanted = false
            if (tip.area.containsMouse) wait.restart(); else wait.stop()
        }
        function onPressed() { tip.wanted = false; wait.stop() }
    }

    parent: area.Window.contentItem
    z: 1000
    width: Math.min(280, Math.max(heading.implicitWidth, words.implicitWidth) + 24)
    height: lines.height + 16
    radius: 8
    color: theme.panelRaised
    border.color: theme.border
    opacity: wanted && area.containsMouse && area.visible ? 1 : 0
    visible: opacity > 0
    Behavior on opacity { NumberAnimation { duration: 120 } }

    Column {
        id: lines
        x: 12
        y: 8
        width: tip.width - 24
        spacing: 3
        Text {
            id: heading
            width: parent.width
            text: tip.title
            visible: text !== ""
            color: tip.theme.textPrimary
            font.pixelSize: 12
            font.bold: true
            wrapMode: Text.Wrap
        }
        Text {
            id: words
            width: parent.width
            text: tip.text
            visible: text !== ""
            color: tip.theme.textSecondary
            font.pixelSize: 12
            wrapMode: Text.Wrap
        }
    }
}
