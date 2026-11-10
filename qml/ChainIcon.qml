import QtQuick

// A chain: two links hooked together (a projection that stays tied to its source, owner
// 2026-11-01), or with `broken` the same two links pulled apart with a crack between them
// (Break Link). Drawn in `ink`, size 18 by default.
Canvas {
    id: chain
    property bool broken: false
    property color ink: "white"
    width: 18
    height: 18
    onWidthChanged: requestPaint()
    onInkChanged: requestPaint()
    onBrokenChanged: requestPaint()
    onPaint: {
        const c = getContext("2d")
        c.reset()
        c.strokeStyle = ink
        c.fillStyle = ink
        c.lineWidth = 1.6
        c.lineCap = "round"
        const len = 9.5, w = 5.4, r = w / 2, off = broken ? 5.9 : 3.1
        const stadium = cx => {
            c.beginPath()
            c.moveTo(cx - len / 2 + r, -r)
            c.lineTo(cx + len / 2 - r, -r)
            c.arc(cx + len / 2 - r, 0, r, -Math.PI / 2, Math.PI / 2)
            c.lineTo(cx - len / 2 + r, r)
            c.arc(cx - len / 2 + r, 0, r, Math.PI / 2, Math.PI * 1.5)
            c.closePath()
            c.stroke()
        }
        c.translate(width / 2, height / 2)
        c.scale(Math.min(width, height) / 18, Math.min(width, height) / 18)   // drawn in 18, scaled to its size (GB-060)
        c.rotate(-Math.PI / 4)
        stadium(-off)
        stadium(off)
        if (broken) {                       // the crack: two short ticks across the gap
            c.lineWidth = 1.3
            c.beginPath()
            c.moveTo(-0.6, -4.2); c.lineTo(0.6, -1.6)
            c.moveTo(0.6, 4.2); c.lineTo(-0.6, 1.6)
            c.stroke()
        }
    }
}
