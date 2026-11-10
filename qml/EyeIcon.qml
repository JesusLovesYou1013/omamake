import QtQuick

// The eye of the Browser rows (EyeButton), without the button: open (something was shown) or
// shut with lashes (something was hidden). Used by the history row. Drawn in `ink`.
Canvas {
    id: eye
    property bool open: true
    property color ink: "white"
    // Drawn in 18 x 12 units, scaled to `width` (GB-060 sizing standard: 22 wide on the Timeline, as
    // wide as the Fillet / Chamfer symbols beside it).
    width: 18
    height: width * 12 / 18
    onWidthChanged: requestPaint()
    onInkChanged: requestPaint()
    onOpenChanged: requestPaint()
    onPaint: {
        const ctx = getContext("2d")
        ctx.reset()
        ctx.scale(width / 18, width / 18)
        const c = "rgb(" + Math.round(ink.r * 255) + "," + Math.round(ink.g * 255) + "," + Math.round(ink.b * 255) + ")"
        ctx.strokeStyle = c
        ctx.fillStyle = c
        ctx.lineWidth = 1.4
        ctx.lineCap = "round"
        ctx.beginPath()
        if (open) {
            ctx.moveTo(1, 6)
            ctx.quadraticCurveTo(9, -2, 17, 6)
            ctx.quadraticCurveTo(9, 14, 1, 6)
            ctx.stroke()
            ctx.beginPath()
            ctx.arc(9, 6, 2.4, 0, Math.PI * 2)
            ctx.fill()
        } else {
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
