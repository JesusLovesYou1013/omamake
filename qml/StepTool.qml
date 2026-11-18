import QtQuick

// Export STEP (owner, GB-082): no longer the whole design at once. It asks first what to export: a body clicked
// in the view is that body, as its row in the Browser is; a component row is every body inside it and in the
// components within it, and the design's own row (the top one) is the whole design. The bodies go into the
// file where they are now, joined as one solid as Export always wrote them (Document::exportTo); a component
// takes all its bodies, hidden ones too. Export then opens the shared file picker, the name being what's
// picked. Esc twice quickly or Cancel leaves (Main.escExit); one Esc only stops the view's highlight.
Item {
    id: tool
    required property var theme
    required property var selection
    property int fadeMs: 250
    property bool open: false
    property var picked: null            // {key: "b3" | "c0", name, bodies: [ids]}
    readonly property var pickedMap: { const m = {}; if (picked) m[picked.key] = true; return m }
    signal say(string text, int ms)
    signal exportRequested(var bodies, string name)

    visible: open || fadeOut.running
    opacity: open ? 1 : 0
    Behavior on opacity { NumberAnimation { id: fadeOut; duration: tool.fadeMs } }

    readonly property string guide: !picked
        ? "Click a body in the view, or a body or component in the Browser. A component exports every body inside it; the top one is the whole design."
        : picked.bodies.length + (picked.bodies.length === 1 ? " body" : " bodies") + ", where they are now. Click another to change it, then Export."

    // The Browser's row by its key ("b3", "c0"), from doc.browser (the tree it shows).
    function nodeOf(key, node) {
        node = node || doc.browser
        if (!node)
            return null
        if (node.key === key)
            return node
        for (const c of node.children || []) {
            const f = nodeOf(key, c)
            if (f)
                return f
        }
        return null
    }
    function begin() {
        picked = null
        open = true
        // A body already selected is the first guess (a face, edge or point of one counts as that body).
        const bodies = []
        for (const p of selection.selected) {
            const b = p.kind === "body" ? p.index : (p.kind === "face" || p.kind === "edge" || p.kind === "point") ? (p.body || doc.currentBody()) : 0
            if (b && !bodies.includes(b))
                bodies.push(b)
        }
        if (bodies.length === 1)
            pick("b" + bodies[0])
        else
            selection.clear()
    }
    function cancel() {
        if (!open)
            return
        open = false
        picked = null
        selection.clearHover()
        selection.clear()
    }
    // A body ("b<id>") or a component ("c<id>", every body under it).
    function pick(key, name) {
        const node = nodeOf(key)
        const id = parseInt(key.slice(1))
        const bodies = key[0] === "b" ? [id] : doc.bodiesUnder(id).map(b => Number(b))
        // The file's name: the body's or component's; the design's own row (the top one) by the design's
        // file name, "Design" while it's unsaved ("(Unsaved)" isn't a name).
        const top = doc.browser && doc.browser.key === key
        const file = doc.filePath ? doc.filePath.split("/").pop().replace(/\.omamake$/i, "") : "Design"
        picked = { key: key, name: top ? file : name || (node ? node.name : key[0] === "b" ? "Body" : "Component"), bodies: bodies }
        selection.selectBodies(bodies)
        if (!bodies.length)
            say(picked.name + " has no bodies to export.", 0)
    }
    // The body under the pointer (0: none): its face, edge or corner there, or the first face the ray meets
    // when something else is in front (an origin plane, a sketch) - only bodies can be exported.
    function bodyAt(x, y) {
        const part = selection.partAt(x, y)
        if (part && (part.kind === "face" || part.kind === "edge" || part.kind === "point" || part.kind === "body"))
            return part.kind === "body" ? part.index : (part.body || doc.currentBody())
        const r = selection.rayAt(x, y)
        const face = doc.faceAt(r.origin, r.direction)
        return face >= 0 ? doc.bodyOfPart("face", face) : 0
    }
    // A click in the view (Main's Navigation.clickAt): the body under it, as its Browser row.
    function click(x, y) {
        const b = bodyAt(x, y)
        if (b)
            pick("b" + b)
        else if (picked)
            selection.selectBodies(picked.bodies)     // a click on nothing keeps what's picked
    }
    // The pointer over the view: the whole body under it lights up.
    function move(x, y) {
        const b = bodyAt(x, y)
        const now = selection.hovered
        if (b && !(now && now.kind === "body" && now.index === b)) {
            selection.hovered = { kind: "body", index: b, name: "Body" }
            selection.redraw()
        } else if (!b && now)
            selection.clearHover()
    }
    // One Esc (GB-081): only the view's highlight goes; what's picked stays.
    function stopHover() {
        if (!selection.hovered)
            return false
        selection.clearHover()
        return true
    }
    function done() {
        if (!picked || !picked.bodies.length)
            return say("Click a body or a component first.", 0)
        exportRequested(picked.bodies, picked.name)
    }

    component Chip: Rectangle {
        id: chip
        property string label
        property bool on: false
        property bool now: false
        property bool usable: true
        property color fill: Qt.rgba(tool.theme.panelBg.r, tool.theme.panelBg.g, tool.theme.panelBg.b, 0.92)
        property color ink: on || now ? tool.theme.textPrimary : tool.theme.textSecondary
        signal clicked()
        height: 34
        width: 120
        radius: 8
        color: fill
        opacity: usable ? 1 : 0.45
        border.color: now ? tool.theme.accent : on ? tool.theme.success : chipArea.containsMouse && usable ? tool.theme.textSecondary : tool.theme.border
        border.width: now ? 2.5 : 1.5
        Text {
            anchors.fill: parent
            anchors.leftMargin: 10
            anchors.rightMargin: 10
            horizontalAlignment: Text.AlignHCenter
            verticalAlignment: Text.AlignVCenter
            elide: Text.ElideRight
            text: chip.label
            color: chip.ink
            font.pixelSize: 12
            font.bold: chip.now
        }
        MouseArea { id: chipArea; anchors.fill: parent; hoverEnabled: true; enabled: chip.usable; cursorShape: Qt.PointingHandCursor; onClicked: chip.clicked() }
    }
    Column {
        objectName: "stepOptions"
        visible: tool.open
        anchors.top: parent.top
        anchors.topMargin: 14
        anchors.horizontalCenter: parent.horizontalCenter
        spacing: 8
        Row {
            anchors.horizontalCenter: parent.horizontalCenter
            spacing: 8
            Chip {
                objectName: "stepPicked"; width: 300
                label: tool.picked ? "✓  Export:  " + tool.picked.name : "Body or component"
                now: !tool.picked; on: !!tool.picked
            }
            Chip { objectName: "stepCancel"; width: 80; label: "Cancel"; onClicked: tool.cancel() }
            Chip {
                objectName: "stepExport"; width: 90; usable: !!tool.picked && tool.picked.bodies.length > 0
                label: "Export"
                fill: tool.theme.success
                ink: tool.theme.windowBg
                onClicked: tool.done()
            }
        }
        Rectangle {
            objectName: "stepGuide"
            anchors.horizontalCenter: parent.horizontalCenter
            width: 620; height: 52; radius: 10
            color: Qt.rgba(tool.theme.panelBg.r, tool.theme.panelBg.g, tool.theme.panelBg.b, 0.95)
            border.color: tool.theme.accent
            border.width: 1.5
            Text {
                anchors.fill: parent; anchors.margins: 10
                verticalAlignment: Text.AlignVCenter
                wrapMode: Text.WordWrap
                maximumLineCount: 2
                elide: Text.ElideRight
                text: tool.guide
                color: tool.theme.textPrimary
                font.pixelSize: 13
            }
        }
    }
}
