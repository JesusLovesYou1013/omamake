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

    // The parameters whose names fit what is being typed, once the text stops being a plain number
    // (owner, 2026-11-02, t94u: the moment a letter goes in, it searches parameters).
    property var picks: []
    property int pickAt: 0
    property int tokenStart: 0
    function lookUp() {
        const t = field.text
        const plain = angle ? Units.parseAngle(t) : Units.parse(t)
        const m = /[A-Za-z0-9_\u00C0-\uFFFF]+$/.exec(t)
        if (plain || !m || !/[A-Za-z_\u00C0-\uFFFF]/.test(m[0]) || /^[0-9.]+\s*$/.test(t)) { picks = []; return }
        const word = m[0].toLowerCase()
        const found = doc.parameters().filter(p => p.ok && p.name.toLowerCase().indexOf(word) >= 0
                                               && (angle ? p.unit === "deg" || p.unit === "rad" || p.unit === "none" : p.unit !== "deg" && p.unit !== "rad"))
        found.sort((a, b) => (b.name.toLowerCase().startsWith(word) ? 1 : 0) - (a.name.toLowerCase().startsWith(word) ? 1 : 0))
        tokenStart = m.index
        picks = found.slice(0, 6)
        pickAt = 0
    }
    function pick(i) {
        if (i < 0 || i >= picks.length) return
        field.text = field.text.slice(0, tokenStart) + picks[i].name
        field.cursorPosition = field.text.length
        picks = []
        wrong = false
        reason = ""
    }

    function show(x, y, mm, unit, then, formula) {
        picks = []
        box.x = Math.round(x - width / 2)
        box.y = Math.round(y - 17)
        apply = then
        angle = unit === "deg"
        wrong = false
        reason = ""
        field.text = formula ? formula : Units.plain(mm, unit)
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
        let v = angle ? Units.parseAngle(field.text) : Units.parse(field.text)
        if (v) {
            v = { mm: v.mm, unit: v.unit, expr: "" }
        } else if (/[A-Za-z_\u00C0-\uFFFF]/.test(field.text) || /[-+*\/^()]/.test(field.text)) {
            // A formula: parameters, maths and units (Document.evaluateExpression).
            const r = doc.evaluateExpression(field.text.trim(), angle ? "angle" : "length")
            if (!r.ok) {
                wrong = true
                reason = r.error
                return
            }
            // Only a formula that uses a parameter stays a formula; "2*3" is just 6.
            v = { mm: r.value, unit: r.unit, expr: r.names.length ? field.text.trim() : "" }
        }
        if (!v || !(v.mm > 0)) {
            wrong = true
            reason = angle ? "Type an angle in degrees: 45, 30.5, or a formula with parameters" : "Type a length: 2.5, 1 1/2, 3 in, 60 mm, or a formula with parameters"
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
            onTextEdited: { box.wrong = false; box.reason = ""; box.lookUp() }
            onActiveFocusChanged: if (!activeFocus) box.close()
            // With the parameter list showing, Enter or Tab takes the highlighted name first; once the
            // name is complete, Enter sets the dimension.
            function enterKey() {
                if (box.picks.length && box.picks[box.pickAt].name.toLowerCase() !== field.text.slice(box.tokenStart).toLowerCase())
                    box.pick(box.pickAt)
                else
                    box.accept()
            }
            Keys.onReturnPressed: enterKey()
            Keys.onEnterPressed: enterKey()
            Keys.onTabPressed: if (box.picks.length) box.pick(box.pickAt)
            Keys.onDownPressed: if (box.picks.length) box.pickAt = (box.pickAt + 1) % box.picks.length
            Keys.onUpPressed: if (box.picks.length) box.pickAt = (box.pickAt + box.picks.length - 1) % box.picks.length
            Keys.onEscapePressed: { box.cancelled(); box.close() }
        }
    }
    // The parameters that fit what is being typed.
    Rectangle {
        visible: box.picks.length > 0 && box.reason === ""
        y: 38
        width: Math.max(box.width, listCol.implicitWidth + 12)
        height: listCol.implicitHeight + 8
        radius: 8
        color: Qt.rgba(box.theme.panelBg.r, box.theme.panelBg.g, box.theme.panelBg.b, 0.97)
        border.color: box.theme.border
        Column {
            id: listCol
            x: 4; y: 4
            Repeater {
                model: box.picks
                Rectangle {
                    required property var modelData
                    required property int index
                    width: Math.max(box.width - 8, rowText.implicitWidth + 20); height: 26; radius: 5
                    color: index === box.pickAt ? box.theme.controlHover : "transparent"
                    Text {
                        id: rowText
                        x: 8; anchors.verticalCenter: parent.verticalCenter
                        text: "Fx  " + modelData.name + "  =  " + modelData.shown + (modelData.unit === "none" ? "" : " " + modelData.unit)
                        color: box.theme.textPrimary; font.pixelSize: 12
                    }
                    MouseArea {
                        anchors.fill: parent; hoverEnabled: true
                        onEntered: box.pickAt = parent.index
                        onPressed: mouse => { box.pick(parent.index); field.forceActiveFocus() }
                    }
                }
            }
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
