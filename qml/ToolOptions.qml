import QtQuick
import "Units.js" as Units

// A tool's options (owner, 2026-10-30: Fillet's radius, a polygon's sides, Offset's distance…),
// in a row under the sketch bar while the tool is on, in the dimension box's look. Each value
// works as soon as it's typed (the preview follows); a value that doesn't read turns its box red
// and the last good one stays. Enter carries on (Offset, a pattern: makes it); Esc stops the
// tool; Ctrl+Z leaves the box and takes back the last step. The first box opens selected, so
// typing replaces it. Lengths in inches unless a unit is typed, angles in degrees.
FocusScope {
    id: panel

    required property var theme
    property var fields: []               // ShapeTools options: [{key, label, kind, value, signed?}]
    property var values: ({})             // key → value now
    property string tool: ""              // which tool's options these are (a new tool refocuses)

    signal changed(string key, var value)
    signal confirmed()
    signal cancelled()
    signal undo()

    readonly property bool typing: activeFocus

    width: row.width
    height: 34
    visible: fields.length > 0

    onToolChanged: Qt.callLater(focusFirst)
    function focusFirst() {
        const first = boxes.itemAt(0)
        if (first && visible)
            first.focusIn()
    }
    function leave() {
        for (let i = 0; i < boxes.count; i++)
            if (boxes.itemAt(i))
                boxes.itemAt(i).focusOut()
        panel.focus = false
    }
    // Hidden (the tool stopped): the keys go back to the window.
    onVisibleChanged: if (!visible) leave()

    // value → text in its box; text → value, or undefined when it doesn't read.
    function shown(f, v) {
        if (f.kind === "length") return Units.plain(Math.abs(v), "in")
        if (f.kind === "angle") return Units.plain(v, "deg")
        if (f.kind === "count") return String(Math.round(v))
        if (f.kind === "number") return String(parseFloat(Number(v).toFixed(4)))
        return String(v)
    }
    function read(f, text, was) {
        if (f.kind === "length") {
            const v = Units.parse(text)
            return v && v.mm > 0 ? (f.signed && was < 0 ? -v.mm : v.mm) : undefined
        }
        if (f.kind === "angle") {
            const v = Units.parseAngle(text)
            return v && v.mm > 0 ? v.mm : undefined
        }
        if (f.kind === "count") {
            const n = parseInt(text)
            return n >= 1 && String(n) === String(text).trim() ? n : undefined
        }
        if (f.kind === "number") {
            const n = parseFloat(text)
            return isFinite(n) && /^\s*-?(\d+\.?\d*|\.\d+)\s*$/.test(text) ? n : undefined
        }
        return String(text).length ? text : undefined
    }

    Row {
        id: row
        spacing: 8
        Repeater {
            id: boxes
            model: panel.fields
            delegate: Rectangle {
                id: box
                required property var modelData
                required property int index
                readonly property bool toggle: modelData.kind === "toggle"
                property bool wrong: false
                function focusIn() { if (!toggle) { field.forceActiveFocus(); field.selectAll() } }
                function focusOut() { field.focus = false }

                width: toggle ? name.implicitWidth + 44 : name.implicitWidth + (modelData.kind === "text" ? 170 : 110)
                height: 34
                radius: 8
                color: Qt.rgba(panel.theme.panelBg.r, panel.theme.panelBg.g, panel.theme.panelBg.b, 0.96)
                border.color: wrong ? panel.theme.danger : field.activeFocus ? panel.theme.accent : panel.theme.border
                border.width: 1.5

                Text {
                    id: name
                    anchors.left: parent.left
                    anchors.leftMargin: 12
                    anchors.verticalCenter: parent.verticalCenter
                    text: box.modelData.label
                    color: panel.theme.textSecondary
                    font.pixelSize: 12
                }
                // A switch: a tick box, clicked.
                Rectangle {
                    visible: box.toggle
                    anchors.right: parent.right
                    anchors.rightMargin: 10
                    anchors.verticalCenter: parent.verticalCenter
                    width: 16
                    height: 16
                    radius: 4
                    color: panel.values[box.modelData.key] ? panel.theme.accent : "transparent"
                    border.color: panel.theme.accent
                    Text {
                        anchors.centerIn: parent
                        visible: !!panel.values[box.modelData.key]
                        text: "✓"
                        color: panel.theme.panelBg
                        font.pixelSize: 12
                        font.bold: true
                    }
                }
                MouseArea {
                    visible: box.toggle
                    anchors.fill: parent
                    cursorShape: Qt.PointingHandCursor
                    onClicked: panel.changed(box.modelData.key, !panel.values[box.modelData.key])
                }
                TextInput {
                    id: field
                    visible: !box.toggle
                    anchors.left: name.right
                    anchors.leftMargin: 10
                    anchors.right: parent.right
                    anchors.rightMargin: 12
                    anchors.verticalCenter: parent.verticalCenter
                    color: panel.theme.textPrimary
                    selectionColor: panel.theme.selection
                    selectedTextColor: panel.theme.windowBg
                    font.pixelSize: 14
                    clip: true
                    // Follows the value (Offset's arrow drags it) unless it's being typed in.
                    readonly property string outside: panel.shown(box.modelData, panel.values[box.modelData.key])
                    onOutsideChanged: if (!activeFocus) text = outside
                    Component.onCompleted: text = outside
                    onActiveFocusChanged: if (!activeFocus) { text = outside; box.wrong = false }
                    onTextEdited: {
                        const v = panel.read(box.modelData, text, panel.values[box.modelData.key])
                        box.wrong = v === undefined
                        if (v !== undefined)
                            panel.changed(box.modelData.key, v)
                    }
                    Keys.onReturnPressed: panel.confirmed()
                    Keys.onEnterPressed: panel.confirmed()
                    Keys.onEscapePressed: { panel.leave(); panel.cancelled() }
                    Keys.onTabPressed: {
                        const next = boxes.itemAt((box.index + 1) % boxes.count)
                        if (next) next.focusIn()
                    }
                    Keys.onPressed: event => {
                        if (event.matches(StandardKey.Undo)) {
                            event.accepted = true
                            panel.leave()
                            panel.undo()
                        }
                    }
                }
            }
        }
    }
}
