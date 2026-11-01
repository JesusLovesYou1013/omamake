import QtQuick

// Find in Window on a Browser row (owner, 2026-10-30): a "my location" mark (a ring with a
// dot, and ticks out to the four sides). Clicked, the view flies to the row's things.
Item {
    id: find

    required property var theme
    signal clicked()

    width: 18
    height: 16

    Canvas {
        anchors.centerIn: parent
        width: 16
        height: 16
        property color ink: area.containsMouse ? find.theme.textPrimary : find.theme.textSecondary
        onInkChanged: requestPaint()
        onPaint: {
            const ctx = getContext("2d")
            ctx.reset()
            const c = "rgb(" + Math.round(ink.r * 255) + "," + Math.round(ink.g * 255) + "," + Math.round(ink.b * 255) + ")"
            ctx.strokeStyle = c
            ctx.fillStyle = c
            ctx.lineWidth = 1.4
            ctx.lineCap = "round"
            ctx.beginPath()
            ctx.arc(8, 8, 4.6, 0, Math.PI * 2)
            ctx.moveTo(8, 0.8); ctx.lineTo(8, 3.4)
            ctx.moveTo(8, 12.6); ctx.lineTo(8, 15.2)
            ctx.moveTo(0.8, 8); ctx.lineTo(3.4, 8)
            ctx.moveTo(12.6, 8); ctx.lineTo(15.2, 8)
            ctx.stroke()
            ctx.beginPath()
            ctx.arc(8, 8, 2, 0, Math.PI * 2)
            ctx.fill()
        }
    }

    MouseArea {
        id: area
        anchors.fill: parent
        anchors.margins: -3
        hoverEnabled: true
        cursorShape: Qt.PointingHandCursor
        onClicked: find.clicked()
    }

    HoverTip {
        theme: find.theme
        area: area
        title: "Find in Window"
        text: "Turn, pan and zoom the view until this is in full view"
    }
}
