import QtQuick

// One symbol for a constraint (owner t51u) or a sketch tool (owner t53u): beside its name in the
// function search and on its step in the history timeline; the sketch badges use the same ones.
// The tool symbols are OmaMake's own take on what Fusion's icons show for the same thing (a
// shape with the points you give it as dots): line, rectangles (2-point, 3-point, centre),
// circles (centre, 2-point, 3-point, 2-tangent, 3-tangent), arcs, polygons, slots, splines,
// conic, point, ellipse, text, mirror, patterns, fillet, chamfers, offset, trim, extend, break,
// scale, blend, move/copy, delete, construction, project, create sketch; and in the model, extrude.
// Drawn here so it looks the same in every font. `kind` is a constraint type (any type the solver
// knows is mapped by `kindFor`), `ink` the colour. Size 18 by default (the glyph is drawn in 16); any other size
// paints the glyph at that size (GB-059), so it stays sharp.
//   horizontal —   vertical |   parallel //   perpendicular ⊥   equal =   collinear —•—
//   coincident ●   online ○   midpoint ▲   symmetry •|•   fix (a lock)   unfix (an open lock)
//   tangent (a ring on a line)   concentric ◎   pattern (three dots)
//   dimension (a measured span, arrows at both ends)   angle (two lines and an arc)
//   fx (parameters: the italic f and x)
//   hv (a level line and an upright one, for Horizontal/Vertical)
Canvas {
    id: icon
    property string kind: ""
    property color ink: "white"
    // Icon sizing standard (GB-060, owner pt162u: the Fillet / Chamfer size "would be the standard size for
    // sizing going forward"): where a symbol stands for a command or a step (the command search's rows, 27 px;
    // the Timeline's steps, 22 px, EyeIcon / ChainIcon / the move cross the same), `full` is set and every
    // symbol's artwork fills its box edge to edge as the model Chamfer / Fillet do (the others, drawn with a
    // 2 unit margin, are enlarged 1.22 about the centre, line weights kept). Off (18 px) for the small badges.
    property bool full: false
    width: 18
    height: 18
    onInkChanged: requestPaint()
    onKindChanged: requestPaint()
    onFullChanged: requestPaint()

    // A solver constraint type to the symbol that shows it.
    function kindFor(type) {
        return ({ horizontalpoints: "horizontal", verticalpoints: "vertical", pointonline: "online",
                  symmetrylines: "symmetry", tangentarcs: "tangent", equalradius: "equal", pointonarc: "online",
                  midpoints: "midpoint", rotation: "pattern", translation: "pattern",
                  length: "dimension", distance: "dimension", radius: "dimension", diameter: "dimension",
                  pointline: "dimension", lineline: "dimension", offset: "dimension", offsetarcs: "dimension",
                  angle: "angle" })[type] || type
    }

    onPaint: {
        const c = getContext("2d")
        c.reset()
        // Drawn in 16 units on an 18 box, scaled to the size it's given (GB-059: larger where it's shown
        // larger, painted at that size instead of a small picture stretched).
        const s = Math.min(width, height) / 18
        c.translate((width - 16 * s) / 2, (height - 16 * s) / 2)
        c.scale(s, s)
        const kind = icon.kind
        // (Extrude's block spans 2..14 x 1.6..13.5: 1.3 about its own middle fills the box the same.)
        const k = !icon.full || kind === "chamfermodel" || kind === "filletmodel" ? 1 : kind === "extrude" ? 1.3 : 1.22
        const my = kind === "extrude" ? 7.6 : 8
        if (k !== 1) { c.translate(8, 8); c.scale(k, k); c.translate(-8, -my) }
        c.strokeStyle = ink
        c.fillStyle = ink
        c.lineWidth = 1.6 / k
        c.lineCap = "round"
        const line = (x1, y1, x2, y2) => { c.beginPath(); c.moveTo(x1, y1); c.lineTo(x2, y2); c.stroke() }
        const dot = (x, y, r) => { c.beginPath(); c.arc(x, y, r, 0, Math.PI * 2); c.fill() }
        const ring = (x, y, r) => { c.beginPath(); c.arc(x, y, r, 0, Math.PI * 2); c.stroke() }
        const head = (x, y, dx) => { c.beginPath(); c.moveTo(x, y); c.lineTo(x + dx * 3, y - 2.2); c.lineTo(x + dx * 3, y + 2.2); c.closePath(); c.fill() }
        // ---- sketch tools ----
        const PI = Math.PI
        const thin = () => { c.lineWidth = 1.1 / k }
        const bold = () => { c.lineWidth = 1.5 / k }
        const dash = on => c.setLineDash(on ? [2, 1.6] : [])
        const path = pts => { c.beginPath(); pts.forEach((p, i) => i ? c.lineTo(p[0], p[1]) : c.moveTo(p[0], p[1])); c.stroke() }
        const arcAt = (x, y, r, a0, a1) => { c.beginPath(); c.arc(x, y, r, a0, a1); c.stroke() }
        const hex = (x, y, r, rot) => { c.beginPath(); for (let i = 0; i < 6; i++) { const a = rot + i * PI / 3; i ? c.lineTo(x + r * Math.cos(a), y + r * Math.sin(a)) : c.moveTo(x + r * Math.cos(a), y + r * Math.sin(a)) } c.closePath(); c.stroke() }
        const stadium = (x1, x2, y, r) => { c.beginPath(); c.moveTo(x1, y - r); c.lineTo(x2, y - r); c.arc(x2, y, r, -PI / 2, PI / 2); c.lineTo(x1, y + r); c.arc(x1, y, r, PI / 2, PI * 1.5); c.closePath(); c.stroke() }
        const oval = (x, y, rx, ry, rot) => { c.beginPath(); for (let i = 0; i <= 36; i++) { const t = i * PI / 18, ex = rx * Math.cos(t), ey = ry * Math.sin(t), px = x + ex * Math.cos(rot) - ey * Math.sin(rot), py = y + ex * Math.sin(rot) + ey * Math.cos(rot); i ? c.lineTo(px, py) : c.moveTo(px, py) } c.stroke() }
        const tip = (x, y, dx, dy, s) => { c.beginPath(); c.moveTo(x, y); c.lineTo(x - dx * s - dy * s * 0.5, y - dy * s + dx * s * 0.5); c.lineTo(x - dx * s + dy * s * 0.5, y - dy * s - dx * s * 0.5); c.closePath(); c.fill() }
        // fx (parameters): the italic "fx" function mark every spreadsheet and CAD program uses, drawn with strokes.
        if (kind === "fx") {
            bold(); c.lineJoin = "round"
            c.beginPath(); c.moveTo(4.6, 14); c.lineTo(6.6, 5.4); c.bezierCurveTo(7.1, 3.2, 8.4, 2.1, 10.4, 2.4); c.stroke()
            line(3.6, 8.2, 8.6, 8.2)
            line(9.6, 8.6, 14.6, 14); line(14.9, 8.6, 9.3, 14)
            return
        }
        if (kind === "line") { bold(); line(3, 13, 13, 3); dot(3, 13, 1.8); dot(13, 3, 1.8); return }
        if (kind === "midline") { bold(); line(2.5, 13.5, 13.5, 2.5); dot(8, 8, 2.4); return }
        if (kind === "rect2") { bold(); c.strokeRect(3, 4, 10, 8); dot(3, 4, 1.7); dot(13, 12, 1.7); return }
        if (kind === "rect3") { c.translate(8, 8); c.rotate(-0.55); bold(); c.strokeRect(-5, -3.2, 10, 6.4); dot(-5, 3.2, 1.6); dot(5, 3.2, 1.6); dot(5, -3.2, 1.6); return }
        if (kind === "rectc") { bold(); c.strokeRect(3, 4, 10, 8); dot(8, 8, 1.8); dot(13, 4, 1.5); return }
        if (kind === "circlec") { bold(); ring(8, 8, 5.6); dot(8, 8, 1.8); return }
        if (kind === "circle2") { bold(); ring(8, 8, 5.6); thin(); line(2.4, 8, 13.6, 8); dot(2.4, 8, 1.7); dot(13.6, 8, 1.7); return }
        if (kind === "circle3") { bold(); ring(8, 8, 5.6); for (const a of [-PI / 2, PI / 6, PI * 5 / 6]) dot(8 + 5.6 * Math.cos(a), 8 + 5.6 * Math.sin(a), 1.7); return }
        if (kind === "circle2t") { thin(); line(1.5, 4, 14.5, 4); line(1.5, 12, 14.5, 12); bold(); ring(8, 8, 4); return }
        if (kind === "circle3t") { thin(); path([[2, 13.5], [14, 13.5], [8, 2.5], [2, 13.5]]); bold(); ring(8, 9.9, 3.5); return }
        if (kind === "arc3") { bold(); arcAt(8, 11, 6, PI * 1.1, PI * 1.9); dot(8 + 6 * Math.cos(PI * 1.1), 11 + 6 * Math.sin(PI * 1.1), 1.6); dot(8 + 6 * Math.cos(PI * 1.9), 11 + 6 * Math.sin(PI * 1.9), 1.6); dot(8, 5, 1.6); return }
        if (kind === "arcc") { thin(); dash(true); line(4, 12, 4, 4); line(4, 12, 12, 12); dash(false); bold(); arcAt(4, 12, 8, -PI / 2, 0); dot(4, 12, 1.7); dot(4, 4, 1.5); dot(12, 12, 1.5); return }
        if (kind === "arctan") { bold(); line(2, 13, 7, 13); c.beginPath(); c.arc(7, 7, 6, PI / 2, 0, true); c.stroke(); dot(13, 7, 1.7); return }
        if (kind === "polyin") { thin(); ring(8, 8, 6.2); bold(); hex(8, 8, 6.2, -PI / 2); return }
        if (kind === "polyout") { thin(); ring(8, 8, 4.7); bold(); hex(8, 8, 5.5, -PI / 2); return }
        if (kind === "polyedge") { bold(); hex(8, 8, 6, 0); dot(8 + 6 * Math.cos(PI / 3), 8 + 6 * Math.sin(PI / 3), 1.8); dot(8 + 6 * Math.cos(2 * PI / 3), 8 + 6 * Math.sin(2 * PI / 3), 1.8); return }
        if (kind === "slotcc") { bold(); stadium(3.7, 12.3, 8, 3.7); dot(3.7, 8, 1.5); dot(12.3, 8, 1.5); return }
        if (kind === "slotov") { bold(); stadium(3.7, 12.3, 8, 3.7); dot(1.6, 8, 1.4); dot(14.4, 8, 1.4); return }
        if (kind === "slotcp") { bold(); stadium(3.7, 12.3, 8, 3.7); dot(8, 8, 1.7); dot(12.3, 8, 1.4); return }
        if (kind === "slot3a" || kind === "slotca") {
            const R = 8, w = 1.9, a0 = PI * 1.18, a1 = PI * 1.82
            bold(); c.beginPath(); c.arc(8, 14, R + w, a0, a1); c.arc(8 + R * Math.cos(a1), 14 + R * Math.sin(a1), w, a1, a1 - PI, true)
            c.arc(8, 14, R - w, a1, a0, true); c.arc(8 + R * Math.cos(a0), 14 + R * Math.sin(a0), w, a0 + PI, a0, true); c.closePath(); c.stroke()
            if (kind === "slotca") { dot(8, 14, 1.7) } else { dot(8 + R * Math.cos(a0), 14 + R * Math.sin(a0), 1.4); dot(8 + R * Math.cos(a1), 14 + R * Math.sin(a1), 1.4); dot(8, 14 - R, 1.4) }
            return
        }
        if (kind === "splinefit") { bold(); c.beginPath(); c.moveTo(2.5, 11.5); c.bezierCurveTo(5, 2, 8, 2, 8, 8); c.bezierCurveTo(8, 14, 11, 14, 13.5, 4.5); c.stroke(); dot(2.5, 11.5, 1.6); dot(8, 8, 1.6); dot(13.5, 4.5, 1.6); return }
        if (kind === "splinectl") { thin(); dash(true); path([[2.5, 12], [5.5, 3], [10.5, 13], [13.5, 4]]); dash(false); bold(); c.beginPath(); c.moveTo(2.5, 12); c.bezierCurveTo(5.5, 3, 10.5, 13, 13.5, 4); c.stroke(); for (const p of [[2.5, 12], [5.5, 3], [10.5, 13], [13.5, 4]]) c.fillRect(p[0] - 1.2, p[1] - 1.2, 2.4, 2.4); return }
        if (kind === "conic") { thin(); dash(true); path([[2.5, 13], [2.5, 3], [13, 3.5]]); dash(false); bold(); c.beginPath(); c.moveTo(2.5, 13); c.quadraticCurveTo(2.5, 3.5, 13, 3.5); c.stroke(); dot(2.5, 3, 1.5); return }
        if (kind === "point") { bold(); line(4, 4, 12, 12); line(12, 4, 4, 12); return }
        if (kind === "ellipse") { bold(); oval(8, 8, 6.3, 3.8, -0.45); dot(8, 8, 1.6); return }
        if (kind === "text") { bold(); line(3.5, 3.8, 12.5, 3.8); line(8, 3.8, 8, 13); line(6.2, 13, 9.8, 13); return }
        if (kind === "textpath") { thin(); c.beginPath(); c.moveTo(1.5, 13.5); c.bezierCurveTo(4, 5, 10, 14, 14.5, 6); c.stroke(); bold(); line(7, 3.2, 12, 3.2); line(9.5, 3.2, 9.5, 8.3); return }
        if (kind === "mirror") { thin(); dash(true); line(8, 1.5, 8, 14.5); dash(false); bold(); path([[2.5, 12], [2.5, 4], [6.5, 8], [2.5, 12]]); path([[13.5, 12], [13.5, 4], [9.5, 8], [13.5, 12]]); c.globalAlpha = 0.5; c.fill(); return }
        if (kind === "patternc") { bold(); for (let i = 0; i < 6; i++) { const a = -PI / 2 + i * PI / 3; dot(8 + 5.2 * Math.cos(a), 8 + 5.2 * Math.sin(a), 1.7) } return }
        if (kind === "patternr") { for (let i = 0; i < 3; i++) for (let j = 0; j < 3; j++) { c.globalAlpha = i + j ? 0.7 : 1; c.fillRect(2 + i * 4.8, 2 + j * 4.8, 3.2, 3.2) } return }
        if (kind === "fillet") { thin(); dash(true); path([[3, 8], [3, 13], [8, 13]]); dash(false); bold(); c.beginPath(); c.moveTo(3, 2.5); c.lineTo(3, 8); c.quadraticCurveTo(3, 13, 8, 13); c.lineTo(13.5, 13); c.stroke(); return }
        if (kind === "chamferequal") { thin(); dash(true); path([[3, 8.5], [3, 13], [8.5, 13]]); dash(false); bold(); path([[3, 2.5], [3, 8.5], [8.5, 13], [13.5, 13]]); thin(); line(1.4, 10.8, 4.6, 10.8); line(5.8, 11.4, 5.8, 14.6); return }
        if (kind === "chamferangle") { thin(); dash(true); path([[3, 6.5], [3, 13], [9, 13]]); dash(false); bold(); path([[3, 2.5], [3, 6.5], [9, 13], [13.5, 13]]); thin(); arcAt(9, 13, 3.4, PI, 3.97); return }
        if (kind === "chamfertwo") { thin(); dash(true); path([[3, 5.2], [3, 13], [10.5, 13]]); dash(false); bold(); path([[3, 2.5], [3, 5.2], [10.5, 13], [13.5, 13]]); return }
        if (kind === "offset") { bold(); c.strokeRect(2, 2.5, 12, 11); thin(); c.strokeRect(5.2, 5.6, 5.6, 4.8); return }
        if (kind === "trim") { thin(); line(8, 1.5, 8, 14.5); bold(); line(2, 8, 8, 8); thin(); dash(true); line(8, 8, 14, 8); dash(false); return }
        if (kind === "extend") { thin(); line(13, 1.5, 13, 14.5); bold(); line(2, 8, 8, 8); thin(); dash(true); line(8, 8, 12, 8); dash(false); tip(13, 8, 1, 0, 3.8); return }
        if (kind === "break") { bold(); line(2, 8, 6, 8); line(10, 8, 14, 8); thin(); line(5.2, 11, 7, 5); line(9, 11, 10.8, 5); return }
        if (kind === "scale") { thin(); dash(true); c.strokeRect(2.5, 2.5, 11, 11); dash(false); bold(); c.strokeRect(2.5, 8, 5.5, 5.5); line(6, 10, 12, 4); tip(12.8, 3.2, 0.7, -0.7, 3.8); return }
        if (kind === "blend") { bold(); line(3, 2.5, 3, 6); line(10, 13, 13.5, 13); c.beginPath(); c.moveTo(3, 6); c.bezierCurveTo(3, 11.5, 6, 13, 10, 13); c.stroke(); dot(3, 6, 1.4); dot(10, 13, 1.4); return }
        if (kind === "movecopy") { bold(); line(8, 3.5, 8, 12.5); line(3.5, 8, 12.5, 8); tip(8, 1.2, 0, -1, 3.6); tip(8, 14.8, 0, 1, 3.6); tip(1.2, 8, -1, 0, 3.6); tip(14.8, 8, 1, 0, 3.6); return }
        if (kind === "delete") { bold(); line(4, 4, 12, 12); line(12, 4, 4, 12); c.beginPath(); c.arc(8, 8, 6.4, 0, PI * 2); thin(); c.stroke(); return }
        if (kind === "construction") { bold(); dash(true); line(2.5, 13.5, 13.5, 2.5); dash(false); return }
        if (kind === "project") { bold(); line(8, 1.8, 8, 9); tip(8, 11, 0, 1, 3.8); line(2.5, 13.5, 13.5, 13.5); return }
        // Extrude: a flat shape pushed up into a block, the arrow showing the push.
        if (kind === "extrude") {
            thin(); c.globalAlpha = 0.5; path([[2, 7.5], [9, 7.5], [14, 4.5], [7, 4.5], [2, 7.5]]); line(2, 7.5, 2, 13.5); line(9, 7.5, 9, 13.5); line(14, 4.5, 14, 10.5); c.globalAlpha = 1
            bold(); c.beginPath(); c.moveTo(2, 13.5); c.lineTo(9, 13.5); c.lineTo(14, 10.5); c.lineTo(7, 10.5); c.closePath()
            c.globalAlpha = 0.45; c.fill(); c.globalAlpha = 1; c.stroke()
            line(8, 11, 8, 3.8); tip(8, 1.6, 0, -1, 3.4); return
        }
        // Model Chamfer and Fillet (GB-059, owner pt161u: "The angle for chanfer needs to be obvious and the radius
        // for fillet needs to be obvious at the smaller sizes"); facing as Fusion 360's (GB-060, owner pt162u: Fillet's
        // corner top right, Chamfer's top left, drawn mirrored). GB-062 (owner pt164u: "the profile of the object
        // they're trying to portray to be very defined, a very fine line ... I wanted the actual fillet to be larger,
        // and there to be more distance between the apex of the fillet round and the actual form"): the block's whole
        // end profile as one fine, even line (no glow, gradient or fade), a flat fill with a hard edge, the sharp
        // corner that was taken off faintly dashed. Fillet: a quarter round of 10.5 on the 14 x 13 block (its apex
        // 4.3 from the old corner), its centre and radius drawn finer still. Chamfer: a 7 x 7 45 degree flat, its
        // angle to the side marked.
        if (kind === "chamfermodel" || kind === "filletmodel") {
            const round = kind === "filletmodel"
            if (!round) { c.translate(16, 0); c.scale(-1, 1) }
            const L = 1, T = 1.5, Rt = 15, B = 14.5          // the block's end
            const R = round ? 10.5 : 7                       // how much is taken off its top right corner
            const profile = () => {
                c.beginPath(); c.moveTo(L, B); c.lineTo(L, T); c.lineTo(Rt - R, T)
                if (round) c.arc(Rt - R, T + R, R, -PI / 2, 0); else c.lineTo(Rt, T + R)
                c.lineTo(Rt, B); c.closePath()
            }
            profile(); c.globalAlpha = 0.22; c.fill(); c.globalAlpha = 1
            c.lineWidth = 0.9; c.globalAlpha = 0.55; dash(true); path([[Rt - R, T], [Rt, T], [Rt, T + R]]); dash(false); c.globalAlpha = 1
            c.lineJoin = "miter"; c.lineWidth = 1.3; profile(); c.stroke()
            c.lineWidth = 0.9
            if (round) {
                const cx = Rt - R, cy = T + R, q = (R - 2.2) * Math.SQRT1_2
                dot(cx, cy, 1.1); line(cx, cy, cx + q - 1.4, cy - q + 1.4); tip(cx + q + 0.6, cy - q - 0.6, Math.SQRT1_2, -Math.SQRT1_2, 2.6)
            } else
                arcAt(Rt, T + R, 3.8, -PI * 3 / 4, -PI / 2)
            return
        }
        if (kind === "sketch") { bold(); path([[2, 12], [4.5, 4], [14, 4], [11.5, 12], [2, 12]]); thin(); line(5.5, 9, 9, 6.5); dot(9, 6.5, 1.3); return }
        switch (icon.kindFor(icon.kind)) {
        case "horizontal": line(3.5, 8, 12.5, 8); break
        case "vertical": line(8, 3.5, 8, 12.5); break
        case "hv": line(2.5, 11.5, 9, 11.5); line(12, 3, 12, 12.5); break
        case "parallel": line(4, 12.5, 8, 3.5); line(8, 12.5, 12, 3.5); break
        case "perpendicular": line(8, 3.5, 8, 12); line(3.5, 12, 12.5, 12); break
        case "equal": line(4, 6, 12, 6); line(4, 10, 12, 10); break
        case "collinear": line(2.5, 8, 13.5, 8); dot(5.5, 8, 1.8); dot(10.5, 8, 1.8); break
        case "coincident": dot(8, 8, 3.2); break
        case "online": ring(8, 8, 3.6); break
        case "midpoint": c.beginPath(); c.moveTo(8, 3); c.lineTo(13, 12); c.lineTo(3, 12); c.closePath(); c.fill(); break
        case "symmetry": line(8, 3, 8, 13); dot(4.5, 8, 1.8); dot(11.5, 8, 1.8); break
        case "tangent": ring(8, 6.5, 3.6); line(2.5, 11.6, 13.5, 11.6); break
        case "curvature": line(2, 12, 7, 12); c.beginPath(); c.moveTo(7, 12); c.bezierCurveTo(11, 12, 13.5, 9, 13.5, 3); c.stroke(); dot(7, 12, 1.5); break
        case "concentric": ring(8, 8, 5.2); ring(8, 8, 2.2); break
        case "pattern": dot(4, 8, 1.8); dot(8, 8, 1.8); dot(12, 8, 1.8); break
        case "fix":
            c.beginPath(); c.arc(8, 7, 2.6, Math.PI, 0); c.stroke()
            c.fillRect(4.5, 7, 7, 6)
            break
        case "unfix":
            c.beginPath(); c.arc(8, 5.6, 2.6, Math.PI, Math.PI * 1.85); c.stroke()
            c.fillRect(4.5, 7, 7, 6)
            break
        case "dimension":
            line(2, 3, 2, 13); line(14, 3, 14, 13)       // the two edges measured
            line(3.5, 8, 12.5, 8)                        // the span
            head(2.5, 8, 1); head(13.5, 8, -1)
            break
        case "angle":
            line(2.5, 13, 13.5, 13); line(2.5, 13, 11, 3.5)
            c.beginPath(); c.arc(2.5, 13, 6.5, -0.85, 0); c.stroke()
            break
        }
    }
}
