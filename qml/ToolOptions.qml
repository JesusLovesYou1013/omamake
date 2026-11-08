import QtQuick
import "Units.js" as Units

// A tool's options (owner, 2026-10-30: Fillet's radius, a polygon's sides, Offset's distance…),
// in a row under the sketch bar while the tool is on, in the dimension box's look. Each value
// works as soon as it's typed (the preview follows); a value that doesn't read turns its box red
// and the last good one stays. Enter carries on (Offset, a pattern: makes it); Esc stops the
// tool; Ctrl+Z leaves the box and takes back the last step (Ctrl+Y puts it back). The first box opens selected, so
// typing replaces it. Lengths in inches unless a unit is typed, angles in degrees.
FocusScope {
    id: panel

    required property var theme
    property var fields: []               // ShapeTools options: [{key, label, kind, value, signed?}]
    property var values: ({})             // key → value now
    property string tool: ""              // which tool's options these are (a new tool refocuses)
    property var fontList: []             // the installed font families (a "font" option's list)

    signal changed(string key, var value)
    signal confirmed()
    signal cancelled()
    signal undo()
    signal redo()

    readonly property bool typing: activeFocus

    width: Math.max(row.width, rowLow.visible ? rowLow.width : 0)
    height: row.height + (rowLow.visible ? rowLow.height + 8 : 0)
    visible: fields.length > 0

    onToolChanged: Qt.callLater(focusFirst)
    function focusFirst() {
        const first = boxAt(0)
        if (first && visible)
            first.focusIn()
    }
    function leave() {
        for (let i = 0; i < boxCount; i++)
            if (boxAt(i))
                boxAt(i).focusOut()
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
        if (f.kind === "choice") return f.choices[v] || ""
        if (f.kind === "font") return v ? String(v) : "Default"
        if (f.kind === "text") return String(v).replace(/\\n/g, "\n")    // a line break is a real new line in the box
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
        if (f.kind === "text") return String(text).length ? String(text) : undefined
        return String(text).length ? text : undefined
    }

    // Text tools show two rows (owner, t81u): words, font and size on top; Bold, Italic, Justify,
    // Flip (every switch and choice) below, with a Done button after the last one. Other tools: one row.
    property bool twoRows: false
    property string doneLabel: ""
    signal done()
    readonly property var topFields: twoRows ? fields.filter(f => f.kind !== "toggle" && f.kind !== "choice") : fields
    readonly property var lowFields: twoRows ? fields.filter(f => f.kind === "toggle" || f.kind === "choice") : []
    readonly property int boxCount: boxesA.count + boxesB.count
    function boxAt(i) { return i < boxesA.count ? boxesA.itemAt(i) : boxesB.itemAt(i - boxesA.count) }

    Row {
        id: row
        z: 1
        anchors.horizontalCenter: parent.horizontalCenter
        spacing: 8
        Repeater {
            id: boxesA
            model: panel.topFields
            delegate: boxComp
        }
    }
    Row {
        id: rowLow
        visible: panel.twoRows
        anchors.top: row.bottom
        anchors.topMargin: 8
        anchors.horizontalCenter: parent.horizontalCenter
        spacing: 8
        Repeater {
            id: boxesB
            model: panel.lowFields
            delegate: boxComp
        }
        Rectangle {
            id: doneButton
            visible: panel.doneLabel.length > 0
            width: doneName.implicitWidth + 36
            height: 34
            radius: 8
            color: doneArea.containsMouse ? Qt.lighter(panel.theme.accent, 1.15) : panel.theme.accent
            border.color: panel.theme.accent
            border.width: 1.5
            Text {
                id: doneName
                anchors.centerIn: parent
                text: panel.doneLabel
                color: panel.theme.panelBg
                font.pixelSize: 14
                font.bold: true
            }
            MouseArea {
                id: doneArea
                anchors.fill: parent
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                onClicked: panel.done()
            }
        }
    }
    Component {
        id: boxComp
        Rectangle {
                id: box
                required property var modelData
                required property int index
                readonly property bool toggle: modelData.kind === "toggle"
                readonly property bool choice: modelData.kind === "choice"   // a click moves to the next
                readonly property bool isFont: modelData.kind === "font"     // a click opens the font list
                readonly property bool typed: !toggle && !choice && !isFont
                property bool listOpen: false
                z: listOpen ? 10 : 0
                property bool wrong: false
                function focusIn() { if (typed) { field.forceActiveFocus(); field.selectAll() } }
                function focusOut() { field.focus = false }

                width: toggle ? name.implicitWidth + 44 : choice ? name.implicitWidth + 72 : isFont ? name.implicitWidth + 170
                       : name.implicitWidth + (modelData.kind === "text" ? 170 : 110)
                // A text box grows a line at a time as Enter adds lines (owner, t72u).
                height: modelData.kind === "text" ? Math.max(34, field.contentHeight + 16) : 34
                radius: 8
                color: Qt.rgba(panel.theme.panelBg.r, panel.theme.panelBg.g, panel.theme.panelBg.b, 0.96)
                border.color: wrong ? panel.theme.danger : field.activeFocus ? panel.theme.accent : panel.theme.border
                border.width: 1.5

                Text {
                    id: name
                    anchors.left: parent.left
                    anchors.leftMargin: 12
                    anchors.top: parent.top
                    anchors.topMargin: 9
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
                // A choice shows its pick; a click goes on to the next.
                Text {
                    visible: box.choice
                    anchors.left: name.right
                    anchors.leftMargin: 10
                    anchors.verticalCenter: parent.verticalCenter
                    text: panel.shown(box.modelData, panel.values[box.modelData.key])
                    color: panel.theme.textPrimary
                    font.pixelSize: 14
                }
                MouseArea {
                    visible: box.choice
                    anchors.fill: parent
                    cursorShape: Qt.PointingHandCursor
                    onClicked: panel.changed(box.modelData.key, ((panel.values[box.modelData.key] || 0) + 1) % box.modelData.choices.length)
                }
                // A font: its name written in that font; a click opens every installed family.
                Text {
                    visible: box.isFont
                    anchors.left: name.right
                    anchors.leftMargin: 10
                    anchors.right: parent.right
                    anchors.rightMargin: 24
                    anchors.verticalCenter: parent.verticalCenter
                    elide: Text.ElideRight
                    text: panel.shown(box.modelData, panel.values[box.modelData.key])
                    color: panel.theme.textPrimary
                    font.family: panel.values[box.modelData.key] || Qt.application.font.family
                    font.pixelSize: 14
                }
                Text {
                    visible: box.isFont
                    anchors.right: parent.right
                    anchors.rightMargin: 9
                    anchors.verticalCenter: parent.verticalCenter
                    text: "▾"
                    color: panel.theme.textSecondary
                    font.pixelSize: 12
                }
                MouseArea {
                    visible: box.isFont
                    anchors.fill: parent
                    cursorShape: Qt.PointingHandCursor
                    onClicked: { box.listOpen = !box.listOpen; if (box.listOpen) { filter.text = ""; filter.forceActiveFocus() } }
                }
                // The list: a search line and every family, each written in itself. It unfolds
                // downward, toward the middle of the view, and fades over 250 ms.
                Rectangle {
                    id: fontMenu
                    visible: opacity > 0
                    opacity: box.listOpen ? 1 : 0
                    Behavior on opacity { NumberAnimation { duration: 250 } }
                    y: box.height + 6
                    width: 300
                    height: 340
                    z: 30
                    radius: 8
                    color: Qt.rgba(panel.theme.panelBg.r, panel.theme.panelBg.g, panel.theme.panelBg.b, 0.98)
                    border.color: panel.theme.accent
                    border.width: 1.5
                    readonly property var shownFonts: {
                        const q = filter.text.toLowerCase()
                        return ["Default"].concat(panel.fontList).filter(f => !q || f.toLowerCase().indexOf(q) >= 0)
                    }
                    TextInput {
                        id: filter
                        x: 12; y: 9
                        width: parent.width - 24
                        height: 22
                        color: panel.theme.textPrimary
                        selectionColor: panel.theme.selection
                        selectedTextColor: panel.theme.windowBg
                        font.pixelSize: 14
                        clip: true
                        Text { visible: !filter.text.length; text: "Search fonts"; color: panel.theme.textSecondary; font: filter.font }
                        Keys.onEscapePressed: { box.listOpen = false; panel.leave(); panel.cancelled() }
                        Keys.onReturnPressed: if (fontMenu.shownFonts.length) { panel.changed(box.modelData.key, fontMenu.shownFonts[0] === "Default" ? "" : fontMenu.shownFonts[0]); box.listOpen = false }
                    }
                    Rectangle { x: 8; y: 36; width: parent.width - 16; height: 1; color: panel.theme.border }
                    MouseArea {                      // the wheel stops here, never zooming the view behind
                        anchors.fill: parent
                        acceptedButtons: Qt.NoButton
                        onWheel: wheel => wheel.accepted = true
                    }
                    ListView {
                        id: fontView
                        x: 4; y: 40
                        width: parent.width - 8
                        height: parent.height - 46
                        clip: true
                        model: fontMenu.shownFonts
                        boundsBehavior: Flickable.StopAtBounds
                        WheelHandler {
                            onWheel: event => {
                                const dy = event.pixelDelta.y !== 0 ? event.pixelDelta.y : event.angleDelta.y / 120 * 56
                                fontView.contentY = Math.max(0, Math.min(Math.max(0, fontView.contentHeight - fontView.height), fontView.contentY - dy))
                            }
                        }
                        delegate: Rectangle {
                            required property string modelData
                            readonly property bool current: (modelData === "Default" ? "" : modelData) === (panel.values[box.modelData.key] || "")
                            width: ListView.view.width
                            height: 28
                            radius: 5
                            color: pick.containsMouse ? panel.theme.controlHover : current ? Qt.rgba(panel.theme.accent.r, panel.theme.accent.g, panel.theme.accent.b, 0.22) : "transparent"
                            Text {
                                anchors.verticalCenter: parent.verticalCenter
                                x: 10
                                width: parent.width - 20
                                elide: Text.ElideRight
                                text: modelData
                                color: panel.theme.textPrimary
                                font.family: modelData === "Default" ? Qt.application.font.family : modelData
                                font.pixelSize: 15
                            }
                            MouseArea {
                                id: pick
                                anchors.fill: parent
                                hoverEnabled: true
                                onClicked: { panel.changed(box.modelData.key, modelData === "Default" ? "" : modelData); box.listOpen = false }
                            }
                        }
                    }
                }
                TextEdit {
                    id: field
                    textFormat: TextEdit.PlainText
                    wrapMode: TextEdit.NoWrap
                    visible: box.typed
                    anchors.left: name.right
                    anchors.leftMargin: 10
                    anchors.right: parent.right
                    anchors.rightMargin: 12
                    anchors.top: parent.top
                    anchors.topMargin: 8
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
                    function apply() {
                        const v = panel.read(box.modelData, text, panel.values[box.modelData.key])
                        box.wrong = v === undefined
                        if (v !== undefined)
                            panel.changed(box.modelData.key, v)
                    }
                    onTextChanged: if (activeFocus) apply()
                    // Text: Enter starts a new line (a real one in the box), Ctrl+Enter is done (owner, t60u).
                    function enter(event) {
                        if (box.modelData.kind === "text" && !(event.modifiers & Qt.ControlModifier)) {
                            event.accepted = false      // the box itself starts the new line
                        } else {
                            event.accepted = true
                            panel.confirmed()
                        }
                    }
                    Keys.onReturnPressed: event => enter(event)
                    Keys.onEnterPressed: event => enter(event)
                    Keys.onEscapePressed: { panel.leave(); panel.cancelled() }
                    Keys.onTabPressed: {
                        const next = panel.boxAt((box.index + (box.parent === rowLow ? boxesA.count : 0) + 1) % panel.boxCount)
                        if (next) next.focusIn()
                    }
                    Keys.onPressed: event => {
                        if (event.matches(StandardKey.Undo)) {
                            event.accepted = true
                            panel.leave()
                            panel.undo()
                        } else if (event.key === Qt.Key_Y && (event.modifiers & Qt.ControlModifier)) {
                            event.accepted = true      // Ctrl+Y: the same, for redo
                            panel.leave()
                            panel.redo()
                        }
                    }
                }
            }
    }
}
