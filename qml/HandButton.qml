import QtQuick

// The pan hand by the view cube: click to let a plain left-drag move the view
// (for mice and touchpads without a middle button). Click again or Esc to stop.
Item {
    id: hand

    required property var theme
    required property var nav

    width: 30
    height: 30

    readonly property bool on: nav.panMode

    Rectangle {
        anchors.fill: parent
        radius: width / 2
        color: hand.on ? hand.theme.accent
             : area.containsMouse ? hand.theme.controlHover
             : Qt.rgba(hand.theme.panelRaised.r, hand.theme.panelRaised.g, hand.theme.panelRaised.b, 0.9)
        border.color: hand.on ? hand.theme.accent : hand.theme.borderStrong
        Behavior on color { ColorAnimation { duration: 120 } }
    }

    Canvas {
        id: icon
        anchors.fill: parent
        property color ink: hand.on ? hand.theme.windowBg : area.containsMouse ? hand.theme.textPrimary : hand.theme.textSecondary
        onInkChanged: requestPaint()
        onPaint: {
            const ctx = getContext("2d")
            ctx.reset()
            const c = "rgb(" + Math.round(ink.r * 255) + "," + Math.round(ink.g * 255) + "," + Math.round(ink.b * 255) + ")"
            ctx.strokeStyle = c
            ctx.fillStyle = c
            ctx.lineCap = "round"
            ctx.lineJoin = "round"

            // Four fingers.
            ctx.lineWidth = 2.6
            const fingers = [[10.5, 9], [13.5, 7], [16.5, 7.5], [19.5, 10]]
            ctx.beginPath()
            for (const f of fingers) {
                ctx.moveTo(f[0], 16)
                ctx.lineTo(f[0], f[1])
            }
            ctx.stroke()

            // Thumb.
            ctx.beginPath()
            ctx.moveTo(10, 19)
            ctx.lineTo(7, 15.5)
            ctx.stroke()

            // Palm.
            ctx.beginPath()
            ctx.moveTo(9.2, 14)
            ctx.lineTo(20.8, 14)
            ctx.lineTo(20.8, 18.5)
            ctx.quadraticCurveTo(20.8, 23.5, 15.5, 23.5)
            ctx.lineTo(14, 23.5)
            ctx.quadraticCurveTo(9.2, 23.5, 9.2, 18.5)
            ctx.closePath()
            ctx.fill()
        }
    }

    MouseArea {
        id: area
        anchors.fill: parent
        hoverEnabled: true
        cursorShape: Qt.PointingHandCursor
        onClicked: hand.nav.panMode = !hand.nav.panMode
    }

    // A small label on hover, so the button explains itself.
    Rectangle {
        anchors.right: parent.left
        anchors.rightMargin: 8
        anchors.verticalCenter: parent.verticalCenter
        width: tip.implicitWidth + 16
        height: 24
        radius: 6
        color: Qt.rgba(hand.theme.panelBg.r, hand.theme.panelBg.g, hand.theme.panelBg.b, 0.92)
        border.color: hand.theme.border
        opacity: area.containsMouse ? 1 : 0
        visible: opacity > 0
        Behavior on opacity { NumberAnimation { duration: 120 } }
        Text {
            id: tip
            anchors.centerIn: parent
            text: hand.on ? "Stop panning   Esc" : "Pan with left-drag"
            color: hand.theme.textSecondary
            font.pixelSize: 12
        }
    }
}
