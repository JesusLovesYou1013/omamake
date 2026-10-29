import QtQuick

// Light/dark switch, top right: a moon while dark, a sun while light.
// Right-click asks for the look menu (Main.qml shows ModeMenu).
Item {
    id: mode

    required property var theme
    signal menuRequested()

    width: 30
    height: 30

    Rectangle {
        anchors.fill: parent
        radius: width / 2
        color: area.containsMouse ? mode.theme.controlHover
             : Qt.rgba(mode.theme.panelRaised.r, mode.theme.panelRaised.g, mode.theme.panelRaised.b, 0.9)
        border.color: mode.theme.borderStrong
        Behavior on color { ColorAnimation { duration: 120 } }
    }

    Canvas {
        anchors.fill: parent
        property color ink: area.containsMouse ? mode.theme.textPrimary : mode.theme.textSecondary
        property bool light: mode.theme.light
        onInkChanged: requestPaint()
        onLightChanged: requestPaint()
        onPaint: {
            const ctx = getContext("2d")
            ctx.reset()
            const c = "rgb(" + Math.round(ink.r * 255) + "," + Math.round(ink.g * 255) + "," + Math.round(ink.b * 255) + ")"
            ctx.fillStyle = c
            ctx.strokeStyle = c
            ctx.lineCap = "round"
            const cx = width / 2, cy = height / 2
            if (light) {
                // Sun: a disc and eight rays.
                ctx.beginPath()
                ctx.arc(cx, cy, 4, 0, Math.PI * 2)
                ctx.fill()
                ctx.lineWidth = 1.7
                ctx.beginPath()
                for (let i = 0; i < 8; i++) {
                    const a = i * Math.PI / 4
                    ctx.moveTo(cx + Math.cos(a) * 6.8, cy + Math.sin(a) * 6.8)
                    ctx.lineTo(cx + Math.cos(a) * 9.2, cy + Math.sin(a) * 9.2)
                }
                ctx.stroke()
            } else {
                // Moon: a disc with a second disc cut out of it.
                ctx.beginPath()
                ctx.arc(cx, cy, 7.5, 0, Math.PI * 2)
                ctx.fill()
                ctx.globalCompositeOperation = "destination-out"
                ctx.beginPath()
                ctx.arc(cx + 4.5, cy - 3.5, 6.2, 0, Math.PI * 2)
                ctx.fill()
                ctx.globalCompositeOperation = "source-over"
            }
        }
    }

    MouseArea {
        id: area
        anchors.fill: parent
        hoverEnabled: true
        cursorShape: Qt.PointingHandCursor
        acceptedButtons: Qt.LeftButton | Qt.RightButton
        onClicked: m => {
            if (m.button === Qt.RightButton)
                mode.menuRequested()
            else
                mode.theme.light = !mode.theme.light
        }
    }
}
