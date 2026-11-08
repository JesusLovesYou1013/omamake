import QtQuick

// The Parameters window (owner, 2026-11-02, t91u): named values and formulas that dimensions (and later
// extrudes and other actions) can use. Opened by searching "Parameters", in or out of a sketch.
// A new row asks for each part in turn: Name, Unit, Expression, Comments; Enter goes to the next,
// Shift+Enter in Comments is a line break and Enter alone finishes the parameter. Every cell is
// checked as it is typed (Document.checkParameters), and only a valid row is saved.
Item {
    id: win

    required property var theme
    property int fadeMs: 250
    property bool open: false
    property bool auto: true                 // Automatic Compute: changes apply as each cell is left
    property string message: ""              // why something was refused (footer)
    property string filterText: ""
    property int focusRow: -1                // the row whose cell has the keys
    property int focusCol: -1
    property int menuRow: -1                 // the row whose unit list is showing
    property int nextKey: 1
    signal closed()
    signal say(string text)
    signal importRequested()
    signal exportRequested()
    property string note: ""              // what an import or export did (footer)

    anchors.fill: parent
    visible: opacity > 0.001
    opacity: open ? 1 : 0
    Behavior on opacity { NumberAnimation { duration: win.fadeMs } }

    readonly property var unitList: [
        { key: "in", label: "in" }, { key: "ft", label: "ft" }, { key: "mm", label: "mm" }, { key: "cm", label: "cm" },
        { key: "m", label: "m" }, { key: "deg", label: "deg" }, { key: "rad", label: "rad" }, { key: "none", label: "No Units" }]
    function unitLabel(key) { for (const u of unitList) if (u.key === key) return u.label; return key }

    ListModel { id: rows }
    property alias model: rows

    // ---- the draft: what the table shows, checked against the document --------------------
    function rowObject(i) {
        const r = rows.get(i)
        return { id: r.pid, name: r.name, unit: r.unit, expr: r.expr, comment: r.comment }
    }
    function draftList() {
        const out = []
        for (let i = 0; i < rows.count; ++i) out.push(rowObject(i))
        return out
    }
    function recheck() {
        const r = doc.checkParameters(draftList())
        for (let i = 0; i < r.length && i < rows.count; ++i) {
            rows.setProperty(i, "nameError", r[i].nameError)
            rows.setProperty(i, "exprError", r[i].exprError)
            rows.setProperty(i, "shown", r[i].shown)
            rows.setProperty(i, "ok", r[i].ok)
        }
    }
    function load() {
        rows.clear()
        for (const p of doc.parameters())
            rows.append({ pid: p.id, key: nextKey++, name: p.name, refName: p.name, unit: p.unit, expr: p.expr, comment: p.comment,
                          nameError: "", exprError: "", shown: p.shown, ok: p.ok, tried: false })
        recheck()
    }
    // The rows that can be saved now: valid ones as typed; an old row that is half-edited keeps its saved form.
    function commitList() {
        const saved = {}
        for (const p of doc.parameters()) saved[p.id] = p
        const out = []
        for (let i = 0; i < rows.count; ++i) {
            const r = rows.get(i)
            if (r.ok && r.nameError === "")
                out.push(rowObject(i))
            else if (r.pid && saved[r.pid])
                out.push({ id: saved[r.pid].id, name: saved[r.pid].name, unit: saved[r.pid].unit, expr: saved[r.pid].expr, comment: saved[r.pid].comment })
        }
        return out
    }
    function commit() {
        if (!auto)
            return true
        const why = doc.applyParameters(commitList())
        if (why) {
            message = why
            return false
        }
        message = ""
        // new rows now have ids
        const saved = doc.parameters()
        for (let i = 0; i < rows.count; ++i) {
            const r = rows.get(i)
            if (!r.pid && r.ok) {
                const hit = saved.find(p => p.name === r.name)
                if (hit) { rows.setProperty(i, "pid", hit.id); rows.setProperty(i, "refName", hit.name) }
            }
        }
        recheck()
        return true
    }

    // ---- actions -------------------------------------------------------------------------
    function openIt() {
        message = ""
        note = ""
        filterText = ""
        menuRow = -1
        load()
        doc.beginParameterSession()
        open = true
        panel.forceActiveFocus()
    }
    function closeIt(fromOk) {
        menuRow = -1
        if (fromOk && !closable()) {
            message = "Fix or delete the red rows first."
            return
        }
        let left = 0
        for (let i = 0; i < rows.count; ++i) if (!rows.get(i).ok || rows.get(i).nameError !== "") ++left
        if (!auto) {
            const why = doc.applyParameters(commitList())
            if (why) { message = why; return }
        } else if (!commit()) {
            if (fromOk) return
            say(message)
        }
        if (!fromOk && left > 0)
            say("Left out " + left + " unfinished parameter" + (left === 1 ? "" : "s") + ".")
        doc.endParameterSession()
        open = false
        closed()
    }
    // ---- spreadsheet files (.ods by default, .csv for Fusion 360; import also reads .xlsx) ----
    function exportTo(url) {
        const why = doc.exportParameters(url, draftList())
        message = why
        note = why ? "" : "Exported " + rows.count + (rows.count === 1 ? " parameter" : " parameters") + " to " + url.toString().split("/").pop()
    }
    function importFrom(url) {
        const r = doc.importParameters(url)
        message = ""
        note = ""
        if (r.error) { message = r.error; return }
        let added = 0, changed = 0
        for (const p of r.rows) {
            let at = -1
            for (let k = 0; k < rows.count; ++k) if (rows.get(k).name === p.name) at = k
            if (at >= 0) {
                rows.setProperty(at, "unit", p.unit); rows.setProperty(at, "expr", p.expr); rows.setProperty(at, "comment", p.comment)
                ++changed
            } else {
                rows.append({ pid: 0, key: nextKey++, name: p.name, refName: p.name, unit: p.unit, expr: p.expr, comment: p.comment,
                              nameError: "", exprError: "", shown: "", ok: false, tried: false })
                ++added
            }
        }
        recheck()
        let bad = 0
        for (let k = 0; k < rows.count; ++k)
            if (!rows.get(k).ok || rows.get(k).nameError !== "") { rows.setProperty(k, "tried", true); ++bad }
        commit()
        let text = "Imported " + added + " new" + (changed ? " and updated " + changed : "") + "."
        if (bad) text += " " + bad + (bad === 1 ? " row needs" : " rows need") + " fixing (red)."
        if (r.skipped.length) text += " Skipped: " + r.skipped.join("; ") + "."
        note = text
        Qt.callLater(() => list.positionViewAtEnd())
    }
    function closable() {
        for (let i = 0; i < rows.count; ++i)
            if (!rows.get(i).ok || rows.get(i).nameError !== "") return false
        return true
    }
    function addRow() {
        rows.append({ pid: 0, key: nextKey++, name: "", refName: "", unit: "in", expr: "", comment: "",
                      nameError: "", exprError: "", shown: "", ok: false, tried: false })
        recheck()
        Qt.callLater(() => { list.positionViewAtEnd(); focusCell(rows.count - 1, 0) })
    }
    function removeRow(i) {
        const r = rows.get(i)
        if (r.pid) {
            const keep = commitList().filter(p => p.id !== r.pid)
            if (auto) {
                const why = doc.applyParameters(keep)
                if (why) {
                    message = "Can't delete " + r.name + ": " + why
                    return
                }
            }
        }
        rows.remove(i)
        message = ""
        recheck()
    }
    function focusCell(i, c) {
        const item = list.itemAtIndex(i)
        if (item) item.focusCol(c)
    }
    // Enter in a cell: on to the next, unless this one isn't right yet.
    function advance(i, c) {
        const r = rows.get(i)
        rows.setProperty(i, "tried", true)
        if (c === 0 && r.nameError !== "") return
        if (c === 2 && r.exprError !== "") return
        focusCell(i, c + 1)
    }
    function finishRow(i) {
        const r = rows.get(i)
        rows.setProperty(i, "tried", true)
        if (r.nameError !== "") return focusCell(i, 0)
        if (r.exprError !== "") return focusCell(i, 2)
        commit()
        panel.forceActiveFocus()
    }
    // A name being typed: other rows' formulas follow it (rename), as long as the new name is a good one.
    function nameTyped(i) {
        const r = rows.get(i)
        if (r.refName !== "" && r.name !== r.refName && doc.parameterNameProblem(r.name) === "" && !hasName(r.name, i)) {
            for (let k = 0; k < rows.count; ++k)
                if (k !== i)
                    rows.setProperty(k, "expr", doc.renameInExpression(rows.get(k).expr, r.refName, r.name))
            rows.setProperty(i, "refName", r.name)
        }
        recheck()
    }
    function hasName(n, except) {
        for (let k = 0; k < rows.count; ++k) if (k !== except && rows.get(k).name === n) return true
        return false
    }
    function matches(i) {
        const f = filterText.trim().toLowerCase()
        if (!f) return true
        const r = rows.get(i)
        return r.name.toLowerCase().indexOf(f) >= 0 || r.expr.toLowerCase().indexOf(f) >= 0 || r.comment.toLowerCase().indexOf(f) >= 0
    }
    // The line under the table: why the cell being typed in isn't right, or why something was refused.
    readonly property bool footerBad: message !== "" || footerError !== ""
    readonly property string footerError: {
        if (focusRow >= 0 && focusRow < rows.count) {
            const r = rows.get(focusRow)
            if (r && focusCol === 0 && r.nameError) return r.nameError
            if (r && (focusCol === 2 || focusCol === 1) && r.exprError) return r.exprError
        }
        return ""
    }
    readonly property string footer: {
        if (message) return message
        if (focusRow >= 0 && focusRow < rows.count) {
            const r = rows.get(focusRow)
            if (r && focusCol === 0 && r.nameError) return r.nameError
            if (r && (focusCol === 2 || focusCol === 1) && r.exprError) return r.exprError
        }
        if (note) return note
        if (!auto) return "Automatic Compute is off: changes are saved when you press OK."
        return ""
    }
    onAutoChanged: if (auto && open) commit()

    // ---- the look --------------------------------------------------------------------------
    MouseArea {                               // modal: nothing behind it takes clicks or wheel
        anchors.fill: parent
        hoverEnabled: true
        acceptedButtons: Qt.AllButtons
        onWheel: wheel => wheel.accepted = true
    }
    Rectangle { anchors.fill: parent; color: Qt.rgba(0, 0, 0, 0.45) }

    Rectangle {
        id: panel
        focus: true
        anchors.centerIn: parent
        width: Math.min(1040, win.width - 80)
        height: Math.min(660, win.height - 80)
        radius: 10
        color: win.theme.panelBg
        border.color: win.theme.borderStrong
        Keys.onEscapePressed: win.closeIt(false)

        readonly property real inner: width - 32
        function cw(c) { return c === 0 ? inner * 0.20 : c === 1 ? inner * 0.11 : c === 2 ? inner * 0.27 : c === 3 ? inner * 0.13 : inner * 0.29 - 40 }
        function cx(c) { let x = 0; for (let k = 0; k < c; ++k) x += cw(k); return x }

        MouseArea { anchors.fill: parent; onClicked: { win.menuRow = -1; panel.forceActiveFocus() } }

        // title
        Item {
            id: title
            width: parent.width; height: 44
            Text {
                x: 16; anchors.verticalCenter: parent.verticalCenter
                text: "PARAMETERS"; color: win.theme.textPrimary; font.pixelSize: 14; font.bold: true; font.letterSpacing: 1
            }
            Rectangle {
                id: closeX
                width: 30; height: 30; radius: 6; anchors.right: parent.right; anchors.rightMargin: 10; anchors.verticalCenter: parent.verticalCenter
                color: xArea.containsMouse ? win.theme.controlHover : "transparent"
                Text { anchors.centerIn: parent; text: "\u2715"; color: win.theme.textSecondary; font.pixelSize: 14 }
                MouseArea { id: xArea; anchors.fill: parent; hoverEnabled: true; onClicked: win.closeIt(false) }
            }
            Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: win.theme.border }
        }

        // toolbar
        Item {
            id: bar
            y: title.height; x: 16; width: panel.inner; height: 56
            Rectangle {
                id: newBtn
                anchors.verticalCenter: parent.verticalCenter
                width: newText.implicitWidth + 28; height: 32; radius: 7
                color: newArea.containsMouse ? win.theme.controlHover : win.theme.controlBg
                border.color: win.theme.border
                Text { id: newText; anchors.centerIn: parent; text: "+  New parameter"; color: win.theme.textPrimary; font.pixelSize: 13 }
                MouseArea { id: newArea; anchors.fill: parent; hoverEnabled: true; onClicked: win.addRow() }
            }
            Rectangle {
                x: newBtn.width + 14; anchors.verticalCenter: parent.verticalCenter
                width: 270; height: 32; radius: 7
                color: win.theme.controlBg
                border.color: filter.activeFocus ? win.theme.accent : win.theme.border
                TextInput {
                    id: filter
                    anchors.fill: parent; anchors.leftMargin: 10; anchors.rightMargin: 10
                    verticalAlignment: TextInput.AlignVCenter
                    color: win.theme.textPrimary; font.pixelSize: 13; clip: true
                    selectionColor: win.theme.selection; selectedTextColor: win.theme.windowBg
                    onTextChanged: win.filterText = text
                    Keys.onEscapePressed: { text = ""; panel.forceActiveFocus() }
                    Text { visible: !filter.text && !filter.activeFocus; anchors.verticalCenter: parent.verticalCenter
                           text: "Filter all parameters"; color: win.theme.textMuted; font.pixelSize: 13 }
                }
            }
            Row {
                id: fileButtons
                anchors.right: autoRow.left; anchors.rightMargin: 22; anchors.verticalCenter: parent.verticalCenter; spacing: 8
                Repeater {
                    model: [{ label: "Import", tip: "Read parameters from a spreadsheet (.ods, .csv or .xlsx)", send: 0 },
                            { label: "Export", tip: "Save the parameters as a spreadsheet (.ods) or a CSV Fusion 360 can read", send: 1 }]
                    Rectangle {
                        required property var modelData
                        width: fileText.implicitWidth + 28; height: 32; radius: 7
                        color: fileArea.containsMouse ? win.theme.controlHover : win.theme.controlBg
                        border.color: win.theme.border
                        Text { id: fileText; anchors.centerIn: parent; text: modelData.label; color: win.theme.textPrimary; font.pixelSize: 13 }
                        MouseArea {
                            id: fileArea; anchors.fill: parent; hoverEnabled: true
                            onClicked: { win.menuRow = -1; if (modelData.send === 0) win.importRequested(); else win.exportRequested() }
                        }
                    }
                }
            }
            Row {
                id: autoRow
                anchors.right: parent.right; anchors.verticalCenter: parent.verticalCenter; spacing: 8
                Rectangle {
                    width: 18; height: 18; radius: 4; anchors.verticalCenter: parent.verticalCenter
                    color: win.auto ? win.theme.accent : win.theme.controlBg; border.color: win.theme.borderStrong
                    Text { anchors.centerIn: parent; visible: win.auto; text: "\u2713"; color: win.theme.windowBg; font.pixelSize: 13; font.bold: true }
                    MouseArea { anchors.fill: parent; onClicked: win.auto = !win.auto }
                }
                Text { text: "Automatic Compute"; color: win.theme.textPrimary; font.pixelSize: 13; anchors.verticalCenter: parent.verticalCenter
                       MouseArea { anchors.fill: parent; onClicked: win.auto = !win.auto } }
            }
        }

        // table
        Rectangle {
            id: table
            x: 16; y: title.height + bar.height; width: panel.inner
            height: panel.height - title.height - bar.height - foot.height
            radius: 6; color: win.theme.panelRaised; border.color: win.theme.border; clip: true

            Row {
                id: head
                height: 30; width: parent.width
                Repeater {
                    model: ["Name", "Unit", "Expression", "Value", "Comments"]
                    Item {
                        required property string modelData
                        required property int index
                        width: panel.cw(index) + (index === 4 ? 40 : 0); height: 30
                        Text { x: 10; anchors.verticalCenter: parent.verticalCenter; text: modelData; color: win.theme.textSecondary; font.pixelSize: 12 }
                    }
                }
                Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: win.theme.border }
            }
            Text {
                visible: rows.count === 0
                anchors.centerIn: parent
                width: parent.width - 80; horizontalAlignment: Text.AlignHCenter; wrapMode: Text.WordWrap
                text: "No parameters yet. Press + New parameter, then give it a name, a unit and a value or formula such as Width*2."
                color: win.theme.textMuted; font.pixelSize: 13
            }
            ListView {
                id: list
                y: head.height; width: parent.width; height: parent.height - head.height
                model: rows
                clip: true
                boundsBehavior: Flickable.StopAtBounds
                cacheBuffer: 2000
                delegate: FocusScope {
                    id: row
                    required property int index
                    required property int pid
                    required property string name
                    required property string unit
                    required property string expr
                    required property string comment
                    required property string nameError
                    required property string exprError
                    required property string shown
                    required property bool ok
                    required property bool tried
                    width: list.width
                    visible: win.matches(index)
                    height: visible ? Math.max(42, commentEdit.contentHeight + 22) : 0
                    function focusCol(c) {
                        if (c === 0) nameField.forceActiveFocus()
                        else if (c === 1) unitCell.forceActiveFocus()
                        else if (c === 2) exprField.forceActiveFocus()
                        else commentEdit.forceActiveFocus()
                    }
                    readonly property bool bad: !ok || nameError !== ""
                    Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: win.theme.border; opacity: 0.6 }

                    // Name
                    Rectangle {
                        x: panel.cx(0) + 6; y: 6; width: panel.cw(0) - 12; height: 30; radius: 5
                        color: win.theme.controlBg
                        border.width: nameField.activeFocus ? 1.5 : 1
                        border.color: row.nameError && (row.tried || !nameField.activeFocus) && row.name !== "" || row.nameError && row.tried
                                      ? win.theme.danger : nameField.activeFocus ? win.theme.accent : win.theme.border
                        TextInput {
                            id: nameField
                            anchors.fill: parent; anchors.leftMargin: 8; anchors.rightMargin: 8
                            verticalAlignment: TextInput.AlignVCenter
                            color: win.theme.textPrimary; font.pixelSize: 13; clip: true
                            selectionColor: win.theme.selection; selectedTextColor: win.theme.windowBg
                            text: row.name
                            onTextEdited: { rows.setProperty(row.index, "name", text); win.nameTyped(row.index) }
                            onActiveFocusChanged: if (activeFocus) { win.focusRow = row.index; win.focusCol = 0 } else win.cellLeft(row.index)
                            Keys.onReturnPressed: win.advance(row.index, 0)
                            Keys.onEnterPressed: win.advance(row.index, 0)
                            Keys.onTabPressed: win.advance(row.index, 0)
                            Keys.onEscapePressed: win.closeIt(false)
                        }
                    }
                    // Unit
                    Rectangle {
                        id: unitCell
                        x: panel.cx(1) + 6; y: 6; width: panel.cw(1) - 12; height: 30; radius: 5
                        color: win.theme.controlBg; activeFocusOnTab: true
                        border.width: activeFocus ? 1.5 : 1
                        border.color: activeFocus ? win.theme.accent : win.theme.border
                        Text { x: 8; anchors.verticalCenter: parent.verticalCenter; text: win.unitLabel(row.unit); color: win.theme.textPrimary; font.pixelSize: 13 }
                        Text { anchors.right: parent.right; anchors.rightMargin: 7; anchors.verticalCenter: parent.verticalCenter
                               text: "\u25BE"; color: win.theme.textMuted; font.pixelSize: 11 }
                        MouseArea {
                            anchors.fill: parent
                            onClicked: { unitCell.forceActiveFocus(); win.openUnitMenu(row.index, unitCell) }
                        }
                        function step(d) {
                            let k = 0
                            for (let u = 0; u < win.unitList.length; ++u) if (win.unitList[u].key === row.unit) k = u
                            k = (k + d + win.unitList.length) % win.unitList.length
                            win.setUnit(row.index, win.unitList[k].key)
                        }
                        onActiveFocusChanged: if (activeFocus) { win.focusRow = row.index; win.focusCol = 1 } else win.cellLeft(row.index)
                        Keys.onUpPressed: step(-1)
                        Keys.onDownPressed: step(1)
                        Keys.onReturnPressed: { win.menuRow = -1; win.advance(row.index, 1) }
                        Keys.onEnterPressed: { win.menuRow = -1; win.advance(row.index, 1) }
                        Keys.onTabPressed: { win.menuRow = -1; win.advance(row.index, 1) }
                        Keys.onEscapePressed: win.closeIt(false)
                        Keys.onPressed: event => {
                            if (event.text.length === 1 && /[a-zA-Z]/.test(event.text)) {
                                const ch = event.text.toLowerCase()
                                const hits = []
                                for (const u of win.unitList) if (u.label.toLowerCase().startsWith(ch)) hits.push(u.key)
                                if (hits.length) {
                                    const at = hits.indexOf(row.unit)
                                    win.setUnit(row.index, hits[(at + 1) % hits.length])
                                }
                                event.accepted = true
                            }
                        }
                    }
                    // Expression
                    Rectangle {
                        x: panel.cx(2) + 6; y: 6; width: panel.cw(2) - 12; height: 30; radius: 5
                        color: win.theme.controlBg
                        border.width: exprField.activeFocus ? 1.5 : 1
                        border.color: row.exprError && (row.tried || !exprField.activeFocus) && row.expr !== "" || row.exprError && row.tried
                                      ? win.theme.danger : exprField.activeFocus ? win.theme.accent : win.theme.border
                        TextInput {
                            id: exprField
                            anchors.fill: parent; anchors.leftMargin: 8; anchors.rightMargin: 8
                            verticalAlignment: TextInput.AlignVCenter
                            color: win.theme.textPrimary; font.pixelSize: 13; clip: true
                            selectionColor: win.theme.selection; selectedTextColor: win.theme.windowBg
                            text: row.expr
                            onTextEdited: { rows.setProperty(row.index, "expr", text); win.recheck() }
                            onActiveFocusChanged: if (activeFocus) { win.focusRow = row.index; win.focusCol = 2 } else win.cellLeft(row.index)
                            Keys.onReturnPressed: win.advance(row.index, 2)
                            Keys.onEnterPressed: win.advance(row.index, 2)
                            Keys.onTabPressed: win.advance(row.index, 2)
                            Keys.onEscapePressed: win.closeIt(false)
                        }
                    }
                    // Value (worked out, not typed)
                    Text {
                        x: panel.cx(3) + 12; y: 6; height: 30; width: panel.cw(3) - 16
                        verticalAlignment: Text.AlignVCenter; elide: Text.ElideRight
                        text: row.ok && row.nameError === "" ? row.shown : "\u2013"
                        color: row.ok && row.nameError === "" ? win.theme.textPrimary : win.theme.danger
                        font.pixelSize: 13
                    }
                    // Comments: Shift+Enter is a new line, Enter finishes
                    Rectangle {
                        x: panel.cx(4) + 6; y: 6; width: panel.cw(4) - 12; height: commentEdit.contentHeight + 12 < 30 ? 30 : commentEdit.contentHeight + 12
                        radius: 5; color: win.theme.controlBg
                        border.width: commentEdit.activeFocus ? 1.5 : 1
                        border.color: commentEdit.activeFocus ? win.theme.accent : win.theme.border
                        TextEdit {
                            id: commentEdit
                            x: 8; y: 6; width: parent.width - 16
                            color: win.theme.textPrimary; font.pixelSize: 13; wrapMode: TextEdit.Wrap
                            selectionColor: win.theme.selection; selectedTextColor: win.theme.windowBg
                            text: row.comment
                            onTextChanged: if (activeFocus && text !== row.comment) { rows.setProperty(row.index, "comment", text) }
                            onActiveFocusChanged: if (activeFocus) { win.focusRow = row.index; win.focusCol = 3 } else win.cellLeft(row.index)
                            Keys.onPressed: event => {
                                if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter) {
                                    if (event.modifiers & Qt.ShiftModifier) insert(cursorPosition, "\n")
                                    else win.finishRow(row.index)
                                    event.accepted = true
                                } else if (event.key === Qt.Key_Escape) {
                                    win.closeIt(false)
                                    event.accepted = true
                                }
                            }
                        }
                    }
                    // delete: big, red, so it can't be missed (owner, t95u)
                    Rectangle {
                        x: panel.cx(4) + panel.cw(4) + 2; y: 5; width: 34; height: 32; radius: 7
                        color: delArea.containsMouse ? win.theme.danger : Qt.rgba(win.theme.danger.r, win.theme.danger.g, win.theme.danger.b, 0.22)
                        border.color: win.theme.danger
                        Text { anchors.centerIn: parent; text: "\u2715"; font.pixelSize: 18; font.bold: true
                               color: delArea.containsMouse ? win.theme.windowBg : win.theme.danger }
                        MouseArea { id: delArea; anchors.fill: parent; hoverEnabled: true; onClicked: win.removeRow(row.index) }
                    }
                }
            }
        }

        // footer
        Item {
            id: foot
            x: 16; y: panel.height - height; width: panel.inner; height: 54
            Text {
                anchors.verticalCenter: parent.verticalCenter
                width: parent.width - 120; wrapMode: Text.WordWrap; maximumLineCount: 2; elide: Text.ElideRight
                text: win.footer
                color: win.footerBad ? win.theme.danger : win.theme.textSecondary
                font.pixelSize: 12
            }
            Rectangle {
                anchors.right: parent.right; anchors.verticalCenter: parent.verticalCenter
                width: 90; height: 34; radius: 7
                color: okArea.containsMouse ? win.theme.hover : win.theme.accent
                Text { anchors.centerIn: parent; text: "OK"; color: win.theme.windowBg; font.pixelSize: 14; font.bold: true }
                MouseArea { id: okArea; anchors.fill: parent; hoverEnabled: true; onClicked: win.closeIt(true) }
            }
        }

        // the unit list
        Rectangle {
            id: unitMenu
            visible: win.menuRow >= 0
            z: 20
            width: 110; height: win.unitList.length * 30 + 8
            radius: 7; color: win.theme.panelRaised; border.color: win.theme.borderStrong
            Column {
                x: 4; y: 4
                Repeater {
                    model: win.unitList
                    Rectangle {
                        required property var modelData
                        width: 102; height: 30; radius: 5
                        color: itemArea.containsMouse ? win.theme.controlHover : "transparent"
                        Text { x: 10; anchors.verticalCenter: parent.verticalCenter; text: modelData.label; color: win.theme.textPrimary; font.pixelSize: 13 }
                        MouseArea {
                            id: itemArea; anchors.fill: parent; hoverEnabled: true
                            onClicked: { const r = win.menuRow; win.setUnit(r, modelData.key); win.menuRow = -1; win.focusCell(r, 2) }
                        }
                    }
                }
            }
        }
    }

    function openUnitMenu(i, cell) {
        const at = cell.mapToItem(panel, 0, cell.height + 2)
        unitMenu.x = Math.max(8, Math.min(panel.width - unitMenu.width - 8, at.x))
        unitMenu.y = Math.min(panel.height - unitMenu.height - 8, at.y)
        menuRow = menuRow === i ? -1 : i
    }
    function setUnit(i, key) {
        rows.setProperty(i, "unit", key)
        recheck()
    }
    // A cell lost the keys: an old row's change is saved now; a new row once the keys leave the row.
    function cellLeft(i) {
        Qt.callLater(() => {
            if (!open || i >= rows.count) return
            const r = rows.get(i)
            if (!r) return
            if (r.pid) { commit(); return }
            if (!activeCell(i) && r.ok && r.nameError === "") commit()
        })
    }
    function activeCell(i) {
        const item = list.itemAtIndex(i)
        return item ? item.activeFocus : false
    }
}
