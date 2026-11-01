import QtQuick

// One constraint's badge (owner, 2026-10-30): a small tile with the constraint's sign, drawn
// here so it looks the same in every font. Hovering it names the constraint and what it ties;
// clicking it picks it (filled in the accent colour) for Delete.
//   horizontal —   vertical |   parallel //   perpendicular ⊥   equal =   collinear —•—
//   coincident ●   online ○ (a point held on a line or curve)   midpoint ▲   symmetry •|•   fix (a lock)
//   tangent (a ring on a line)   concentric ◎   pattern (three dots: a Circular or Rectangular Pattern copy)
Rectangle {
    id: badge

    required property var theme
    property string kind: ""
    property string label: ""
    property bool picked: false
    signal clicked()

    width: 18
    height: 18
    radius: 4
    color: picked ? theme.accent : Qt.rgba(theme.panelBg.r, theme.panelBg.g, theme.panelBg.b, 0.92)
    border.color: area.containsMouse ? theme.accent : theme.border

    Canvas {
        id: sign
        anchors.centerIn: parent
        width: 16
        height: 16
        readonly property color ink: badge.picked ? badge.theme.panelBg : badge.theme.accent
        onInkChanged: requestPaint()
        Connections {
            target: badge
            function onKindChanged() { sign.requestPaint() }
        }
        onPaint: {
            const c = getContext("2d")
            c.reset()
            c.strokeStyle = ink
            c.fillStyle = ink
            c.lineWidth = 1.6
            c.lineCap = "round"
            const line = (x1, y1, x2, y2) => { c.beginPath(); c.moveTo(x1, y1); c.lineTo(x2, y2); c.stroke() }
            const dot = (x, y, r) => { c.beginPath(); c.arc(x, y, r, 0, Math.PI * 2); c.fill() }
            switch (badge.kind) {
            case "horizontal": line(3.5, 8, 12.5, 8); break
            case "vertical": line(8, 3.5, 8, 12.5); break
            case "parallel": line(4, 12.5, 8, 3.5); line(8, 12.5, 12, 3.5); break
            case "perpendicular": line(8, 3.5, 8, 12); line(3.5, 12, 12.5, 12); break
            case "equal": line(4, 6, 12, 6); line(4, 10, 12, 10); break
            case "collinear": line(2.5, 8, 13.5, 8); dot(5.5, 8, 1.8); dot(10.5, 8, 1.8); break
            case "coincident": dot(8, 8, 3.2); break
            case "online": c.beginPath(); c.arc(8, 8, 3.6, 0, Math.PI * 2); c.stroke(); break
            case "midpoint": c.beginPath(); c.moveTo(8, 3); c.lineTo(13, 12); c.lineTo(3, 12); c.closePath(); c.fill(); break
            case "symmetry": line(8, 3, 8, 13); dot(4.5, 8, 1.8); dot(11.5, 8, 1.8); break
            case "tangent": c.beginPath(); c.arc(8, 6.5, 3.6, 0, Math.PI * 2); c.stroke(); line(2.5, 11.6, 13.5, 11.6); break
            case "concentric":
                c.beginPath(); c.arc(8, 8, 5.2, 0, Math.PI * 2); c.stroke()
                c.beginPath(); c.arc(8, 8, 2.2, 0, Math.PI * 2); c.stroke()
                break
            case "pattern": dot(4, 8, 1.8); dot(8, 8, 1.8); dot(12, 8, 1.8); break
            case "fix":
                c.beginPath(); c.arc(8, 7, 2.6, Math.PI, 0); c.stroke()
                c.fillRect(4.5, 7, 7, 6)
                break
            }
        }
    }
    MouseArea {
        id: area
        anchors.fill: parent
        hoverEnabled: true
        cursorShape: Qt.PointingHandCursor
        onClicked: badge.clicked()
    }
    Rectangle {
        visible: area.containsMouse
        x: parent.width + 6
        y: -2
        width: tip.implicitWidth + 16
        height: 22
        radius: 11
        color: Qt.rgba(badge.theme.panelBg.r, badge.theme.panelBg.g, badge.theme.panelBg.b, 0.95)
        border.color: badge.theme.accent
        Text {
            id: tip
            anchors.centerIn: parent
            text: badge.label + (badge.picked ? "  (picked: Delete takes it off)" : "  (click to pick, then Delete)")
            color: badge.theme.textPrimary
            font.pixelSize: 12
        }
    }
}
