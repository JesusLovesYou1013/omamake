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

    SketchIcon {
        anchors.centerIn: parent
        kind: badge.kind
        ink: badge.picked ? badge.theme.panelBg : badge.theme.accent
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
