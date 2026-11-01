import QtQuick

// The Alt+Space command search: slides down from the window's top edge.
// Type to filter, ↑ ↓ to choose, Enter to run, Esc to close.
// Shows nothing until something is typed, and ranks what fits the mode first (owner,
// 2026-10-30: "L" then Enter must draw a Line in a sketch, "R" a rectangle; the look, file and
// app commands only at the bottom, so typing more is what finds them).
// Tools that come in kinds show as one row (Families.js: "circle" → Circle): Enter runs its
// default kind, → (or the arrow on the row) opens every kind under it, ← closes them. A kind
// typed for itself ("3-point", "tangent circle") still shows on its own row.
Item {
    id: search

    required property var theme
    property var commands: []
    property bool open: false
    property int timesOpened: 0          // remembered (Main.qml Settings); the hint goes after 3
    property int current: 0
    property var families: []            // Families.families, each with `def`: its default kind's name now
    property string expanded: ""         // the family whose kinds are open under it

    signal used(string command)          // a command ran (a family's row: its default kind)

    // Groups that aren't the mode's own work (modelling or sketching): always below the rest.
    readonly property var asideGroups: ["Look", "File", "App"]

    // How well a command's name fits what's typed (0: not at all). Best first: the name starts
    // with it; a word in the name does ("r" → 2-Point Rectangle); the words' first letters do
    // ("hv" → Horizontal/Vertical, "sd" → Sketch Dimension; from 2 letters); one of the
    // command's extra `words` starts with it (other names people search by, e.g. "dark" for
    // Light Mode while it's dark); anywhere in the name (from 2 letters); a word of the
    // description starts with it (from 3 letters).
    // A command's own `letter` (in a sketch: the tool's own plain key, SketchKeys.js, so typing
    // "e" picks Ellipse as E does; plus h for Horizontal/Vertical) beats everything for that one
    // letter.
    function score(c, q) {
        if (c.letter && q === c.letter)
            return 120
        const name = c.name.toLowerCase()
        if (name.startsWith(q))
            return 100
        const words = name.split(/[\s\/-]+/).filter(w => w)
        if (words.some(w => w.startsWith(q)))
            return 80
        if (q.length >= 2 && words.map(w => w[0]).join("").startsWith(q))
            return 60
        if (c.words && c.words.split(" ").some(w => w.startsWith(q)))
            return 50
        if (q.length >= 2 && name.includes(q))
            return 40
        if (q.length >= 3 && c.hint.toLowerCase().split(/[^a-z0-9]+/).some(w => w.startsWith(q)))
            return 20
        return 0
    }
    // A row per family whose kinds are here (this mode): {family, kinds, def, …a command's own}.
    readonly property var familyRows: {
        const out = {}
        for (const f of families) {
            const kinds = f.members.map(n => commands.find(c => c.name === n)).filter(c => c)
            if (!kinds.length)
                continue
            const def = kinds.find(c => c.name === f.def) || kinds[0]
            out[f.name] = { name: f.name, letter: f.letter, words: f.words, keys: f.keys || "", group: def.group,
                            hint: "Enter: " + def.name + "  ·  → all " + kinds.length + " kinds",
                            family: f.name, kinds: kinds, def: def, run: def.run }
        }
        return out
    }
    readonly property var matches: {
        const q = input.text.trim().toLowerCase()
        if (q === "")
            return []
        // The commands in order, each family's row standing where its first kind is.
        const list = [], placed = {}
        for (const c of commands) {
            const f = families.find(f => f.members.includes(c.name))
            if (f && familyRows[f.name] && !placed[f.name]) {
                placed[f.name] = true
                list.push(familyRows[f.name])
            }
            list.push(f ? Object.assign({}, c, { member: f.name }) : c)
        }
        const scored = list.map((c, i) => ({ c: c, i: i, s: score(c, q), aside: asideGroups.includes(c.group) }))
        const found = {}                                     // families whose own row fits
        for (const m of scored)
            if (m.c.family && m.s > 0)
                found[m.c.family] = true
        const top = scored.filter(m => m.s > 0 && !(m.c.member && found[m.c.member]))
                          .sort((a, b) => (a.aside - b.aside) || (b.s - a.s) || (a.i - b.i))   // list order breaks ties
                          .map(m => m.c)
        const out = []
        for (const c of top) {
            out.push(c)
            if (c.family && c.family === expanded)
                for (const k of c.kinds)
                    out.push(Object.assign({}, k, { member: c.family, depth: 1, isDefault: k === c.def }))
        }
        return out
    }
    // Opens (or closes) a family's kinds under its row, keeping that row chosen.
    function toggleKinds(family) {
        expanded = expanded === family ? "" : family
        const at = matches.findIndex(m => m.family === family)
        if (at >= 0)
            current = at
    }

    anchors.fill: parent
    visible: panel.y > -panel.height

    function toggle() {
        open = !open
        if (open) {
            timesOpened++
            expanded = ""
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
        used(command.family ? command.def.name : command.name)
        command.run()
    }

    // Name with the typed part in bold: where a word starts with it if one does, else anywhere.
    function marked(name) {
        const q = input.text.trim().toLowerCase()
        if (q === "")
            return name
        const low = name.toLowerCase()
        let at = -1
        for (let i = 0; i < low.length && at < 0; i++)
            if ((i === 0 || /[\s\/-]/.test(low[i - 1])) && low.startsWith(q, i))
                at = i
        if (at < 0)
            at = low.indexOf(q)
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
                    // → opens the chosen family's kinds, ← closes them; otherwise they move the cursor.
                    Keys.onRightPressed: event => {
                        const m = search.matches[search.current]
                        event.accepted = !!(m && m.family && search.expanded !== m.family)
                        if (event.accepted)
                            search.toggleKinds(m.family)
                    }
                    Keys.onLeftPressed: event => {
                        const m = search.matches[search.current]
                        const family = m ? (m.family || (m.depth ? m.member : "")) : ""
                        event.accepted = !!family && search.expanded === family
                        if (event.accepted)
                            search.toggleKinds(family)
                    }

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
                        anchors.leftMargin: 14 + (modelData.depth ? 24 : 0)
                        anchors.right: parent.right
                        anchors.rightMargin: 96          // clear of the keys, "default" and the arrow
                        anchors.verticalCenter: parent.verticalCenter
                        spacing: 2
                        Text {
                            text: search.marked(modelData.name)
                            textFormat: Text.StyledText
                            color: theme.textPrimary
                            font.pixelSize: 15
                        }
                        Text {
                            width: parent.width
                            elide: Text.ElideRight
                            text: modelData.family ? modelData.hint : modelData.hint + "  ·  " + modelData.group
                            color: theme.textSecondary
                            font.pixelSize: 12
                        }
                    }
                    // A kind under its family: a thin line down its left.
                    Rectangle {
                        visible: !!modelData.depth
                        x: 22
                        width: 2
                        height: parent.height - 16
                        anchors.verticalCenter: parent.verticalCenter
                        radius: 1
                        color: theme.border
                    }
                    Text {
                        anchors.right: parent.right
                        anchors.rightMargin: modelData.family ? 50 : 14
                        anchors.verticalCenter: parent.verticalCenter
                        text: modelData.isDefault ? (modelData.keys ? modelData.keys + "  ·  default" : "default") : modelData.keys
                        color: theme.textMuted
                        font.pixelSize: 12
                    }
                    MouseArea {
                        anchors.fill: parent
                        hoverEnabled: true
                        onEntered: search.current = index
                        onClicked: search.run(index)
                    }
                    // A family's arrow: opens its kinds (turns down while they're open).
                    Rectangle {
                        visible: !!modelData.family
                        anchors.right: parent.right
                        anchors.rightMargin: 10
                        anchors.verticalCenter: parent.verticalCenter
                        width: 30
                        height: 30
                        radius: 8
                        color: arrowArea.containsMouse ? Qt.rgba(theme.accent.r, theme.accent.g, theme.accent.b, 0.22) : "transparent"
                        Text {
                            anchors.centerIn: parent
                            text: "›"
                            rotation: search.expanded === modelData.family ? 90 : 0
                            color: theme.textSecondary
                            font.pixelSize: 20
                        }
                        MouseArea {
                            id: arrowArea
                            anchors.fill: parent
                            hoverEnabled: true
                            onEntered: search.current = index
                            onClicked: search.toggleKinds(modelData.family)
                        }
                    }
                }
            }

            Text {
                visible: search.matches.length === 0 && input.text.trim() !== ""
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
                    text: "↑ ↓ choose     → kinds     Enter run     Esc close"
                    color: theme.textMuted
                    font.pixelSize: 12
                }
            }
        }
    }
}
