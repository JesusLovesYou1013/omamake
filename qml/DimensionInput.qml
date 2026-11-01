import QtQuick
import "Units.js" as Units

// The box a dimension's value is typed into (Fusion's Sketch Dimension box), floating where
// the dimension goes. It opens with the length as it is now, all selected, so typing
// replaces it. Inches unless a unit is typed (Units.js). Enter sets it; Esc or a click
// elsewhere leaves it. A value that isn't a length turns the box red and stays open.
Item {
    id: box

    required property var theme
    property bool open: false
    property bool wrong: false
    property bool angle: false        // an angle: degrees, not a length

    // Called with Units.parse's {mm, unit} when Enter is pressed on a good value. Returns ""
    // when it's done, or a reason to show (the box then stays open).
    property var apply: null
    property string reason: ""
    property alias text: field.text

    signal closed()
    signal cancelled()                // Esc

    width: 190
    height: 34 + (reason ? 30 : 0)
    visible: open

    function show(x, y, mm, unit, then) {
        box.x = Math.round(x - width / 2)
        box.y = Math.round(y - 17)
        apply = then
        angle = unit === "deg"
        wrong = false
        reason = ""
        field.text = Units.plain(mm, unit)
        open = true
        field.forceActiveFocus()
        field.selectAll()
    }
    function close() {
        if (!open)
            return
        open = false
        apply = null
        // Let go of the keys: a hidden box that kept them would take Ctrl+Z as its own undo,
        // and the app's Ctrl+Z wouldn't happen (owner, 2026-10-29: "not very consistently").
        field.focus = false
        closed()
    }
    function accept() {
        const v = angle ? Units.parseAngle(field.text) : Units.parse(field.text)
        if (!v || !(v.mm > 0)) {
            wrong = true
            reason = angle ? "Type an angle in degrees: 45, 30.5…" : "Type a length: 2.5, 1 1/2, 3 in, 60 mm…"
            return
        }
        const why = apply ? apply(v) : ""
        if (why) {
            wrong = true
            reason = why
            return
        }
        close()
    }

    Rectangle {
        width: parent.width
        height: 34
        radius: 8
        color: Qt.rgba(box.theme.panelBg.r, box.theme.panelBg.g, box.theme.panelBg.b, 0.96)
        border.color: box.wrong ? box.theme.danger : box.theme.accent
        border.width: 1.5

        TextInput {
            id: field
            anchors.fill: parent
            anchors.leftMargin: 12
            anchors.rightMargin: 12
            verticalAlignment: TextInput.AlignVCenter
            color: box.theme.textPrimary
            selectionColor: box.theme.selection
            selectedTextColor: box.theme.windowBg
            font.pixelSize: 14
            clip: true
            onTextEdited: { box.wrong = false; box.reason = "" }
            onActiveFocusChanged: if (!activeFocus) box.close()
            Keys.onReturnPressed: box.accept()
            Keys.onEnterPressed: box.accept()
            Keys.onEscapePressed: { box.cancelled(); box.close() }
        }
    }
    Rectangle {
        visible: box.reason !== ""
        y: 38
        anchors.horizontalCenter: parent.horizontalCenter
        width: why.implicitWidth + 20
        height: 26
        radius: 13
        color: Qt.rgba(box.theme.panelBg.r, box.theme.panelBg.g, box.theme.panelBg.b, 0.95)
        border.color: box.theme.danger
        Text {
            id: why
            anchors.centerIn: parent
            text: box.reason
            color: box.theme.textPrimary
            font.pixelSize: 12
        }
    }
}
