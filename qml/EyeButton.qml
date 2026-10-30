import QtQuick

// The show/hide eye on a Browser row: open while shown, closed while hidden.
Item {
    id: eye

    required property var theme
    property bool open: true
    signal clicked()

    width: 20
    height: 16

    Canvas {
        id: picture
        anchors.centerIn: parent
        width: 18
        height: 12
        property color ink: area.containsMouse ? eye.theme.textPrimary
                          : eye.open ? eye.theme.textSecondary : eye.theme.textMuted
        property bool isOpen: eye.open
        onInkChanged: requestPaint()
        onIsOpenChanged: requestPaint()
        onPaint: {
            const ctx = getContext("2d")
            ctx.reset()
            const c = "rgb(" + Math.round(ink.r * 255) + "," + Math.round(ink.g * 255) + "," + Math.round(ink.b * 255) + ")"
            ctx.strokeStyle = c
            ctx.fillStyle = c
            ctx.lineWidth = 1.4
            ctx.lineCap = "round"
            ctx.beginPath()
            if (isOpen) {
                // Almond outline and pupil.
                ctx.moveTo(1, 6)
                ctx.quadraticCurveTo(9, -2, 17, 6)
                ctx.quadraticCurveTo(9, 14, 1, 6)
                ctx.stroke()
                ctx.beginPath()
                ctx.arc(9, 6, 2.4, 0, Math.PI * 2)
                ctx.fill()
            } else {
                // Shut lid with lashes.
                ctx.moveTo(1, 4)
                ctx.quadraticCurveTo(9, 10, 17, 4)
                ctx.moveTo(4.5, 6.4)
                ctx.lineTo(3.5, 9.5)
                ctx.moveTo(9, 7)
                ctx.lineTo(9, 10.5)
                ctx.moveTo(13.5, 6.4)
                ctx.lineTo(14.5, 9.5)
                ctx.stroke()
            }
        }
    }

    MouseArea {
        id: area
        anchors.fill: parent
        anchors.margins: -4
        hoverEnabled: true
        cursorShape: Qt.PointingHandCursor
        onClicked: eye.clicked()
    }
}
