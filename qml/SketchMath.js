.pragma library

// Flat geometry on the sketch (u, v in mm) for the drawing and modifying tools (owner,
// 2026-10-30: every Fusion sketch tool). Points are {u, v}. Nothing here touches the document.

function P(u, v) { return { u: u, v: v } }
function add(a, b) { return P(a.u + b.u, a.v + b.v) }
function sub(a, b) { return P(a.u - b.u, a.v - b.v) }
function mul(a, k) { return P(a.u * k, a.v * k) }
function dot(a, b) { return a.u * b.u + a.v * b.v }
function cross(a, b) { return a.u * b.v - a.v * b.u }
function len(a) { return Math.hypot(a.u, a.v) }
function dist(a, b) { return Math.hypot(a.u - b.u, a.v - b.v) }
function norm(a) { const l = len(a); return l < 1e-12 ? P(0, 0) : P(a.u / l, a.v / l) }
function left(a) { return P(-a.v, a.u) }                      // a turned a quarter counter-clockwise
function mid(a, b) { return P((a.u + b.u) / 2, (a.v + b.v) / 2) }
function lerp(a, b, t) { return P(a.u + (b.u - a.u) * t, a.v + (b.v - a.v) * t) }
function angleOf(a) { return Math.atan2(a.v, a.u) }
function polar(c, r, angle) { return P(c.u + r * Math.cos(angle), c.v + r * Math.sin(angle)) }
function rotate(p, c, angle) {
    const cs = Math.cos(angle), sn = Math.sin(angle), d = sub(p, c)
    return P(c.u + cs * d.u - sn * d.v, c.v + sn * d.u + cs * d.v)
}
// An angle brought into [0, 2π).
function wrap(a) { a %= 2 * Math.PI; return a < 0 ? a + 2 * Math.PI : a }

// Signed distance from p to the line through a and b (+ on the left, seen from a to b).
function across(p, a, b) { const d = norm(sub(b, a)); return cross(d, sub(p, a)) }
// The foot of the square from p on the line through a and b.
function foot(p, a, b) { const d = norm(sub(b, a)); return add(a, mul(d, dot(sub(p, a), d))) }
function mirror(p, a, b) { const f = foot(p, a, b); return P(2 * f.u - p.u, 2 * f.v - p.v) }

// Where the lines through a1–a2 and b1–b2 cross: {at, t, s} (t along a, s along b, 0 to 1
// between the ends), or null when they're parallel.
function lineLine(a1, a2, b1, b2) {
    const r = sub(a2, a1), s = sub(b2, b1), den = cross(r, s)
    if (Math.abs(den) < 1e-12)
        return null
    const t = cross(sub(b1, a1), s) / den, u = cross(sub(b1, a1), r) / den
    return { at: add(a1, mul(r, t)), t: t, s: u }
}
// Where the line through a and b meets the circle (c, r): [{at, t}] (t along a → b).
function lineCircle(a, b, c, r) {
    const d = sub(b, a), f = sub(a, c)
    const A = dot(d, d), B = 2 * dot(f, d), C = dot(f, f) - r * r
    const disc = B * B - 4 * A * C
    if (A < 1e-18 || disc < 0)
        return []
    const sq = Math.sqrt(disc), out = []
    for (const t of disc < 1e-12 ? [-B / (2 * A)] : [(-B - sq) / (2 * A), (-B + sq) / (2 * A)])
        out.push({ at: add(a, mul(d, t)), t: t })
    return out
}
function circleCircle(c1, r1, c2, r2) {
    const d = dist(c1, c2)
    if (d < 1e-12 || d > r1 + r2 + 1e-9 || d < Math.abs(r1 - r2) - 1e-9)
        return []
    const a = (r1 * r1 - r2 * r2 + d * d) / (2 * d), h = Math.sqrt(Math.max(0, r1 * r1 - a * a))
    const e = norm(sub(c2, c1)), m = add(c1, mul(e, a))
    return h < 1e-9 ? [m] : [add(m, mul(left(e), h)), sub(m, mul(left(e), h))]
}
// The circle through three points: {c, r}, or null when they're in a line.
function circumcircle(a, b, c) {
    const d = 2 * (a.u * (b.v - c.v) + b.u * (c.v - a.v) + c.u * (a.v - b.v))
    if (Math.abs(d) < 1e-12)
        return null
    const a2 = a.u * a.u + a.v * a.v, b2 = b.u * b.u + b.v * b.v, c2 = c.u * c.u + c.v * c.v
    const centre = P((a2 * (b.v - c.v) + b2 * (c.v - a.v) + c2 * (a.v - b.v)) / d,
                     (a2 * (c.u - b.u) + b2 * (a.u - c.u) + c2 * (b.u - a.u)) / d)
    return { c: centre, r: dist(centre, a) }
}
// Whether going counter-clockwise round c from a, you reach p before b.
function ccwBetween(c, a, b, p) {
    const from = angleOf(sub(a, c))
    return wrap(angleOf(sub(p, c)) - from) < wrap(angleOf(sub(b, c)) - from)
}

// Round about c from angle `from` by `sweep` (radians, + counter-clockwise), as a polyline.
function arcPoints(c, r, from, sweep) {
    const out = [], steps = Math.max(8, Math.ceil(Math.abs(sweep) / (2 * Math.PI) * 96))
    for (let i = 0; i <= steps; i++)
        out.push(polar(c, r, from + sweep * i / steps))
    return out
}
// An arc stored as {c, a, b} (counter-clockwise a → b), as a polyline.
function arcBetween(c, a, b) {
    const from = angleOf(sub(a, c)), sweep = wrap(angleOf(sub(b, c)) - from) || 2 * Math.PI
    return arcPoints(c, dist(c, a), from, sweep)
}

// Curves as polylines (the same shapes SketchGeometry::curvePath draws).
function bspline(ctrl) {
    const n = ctrl.length
    if (n < 2)
        return ctrl.slice()
    const p = Math.min(3, n - 1), knots = []
    for (let i = 0; i <= p; i++) knots.push(0)
    for (let i = 1; i < n - p; i++) knots.push(i / (n - p))
    for (let i = 0; i <= p; i++) knots.push(1)
    const out = [], samples = 24 * n
    for (let s = 0; s <= samples; s++) {
        const t = Math.min(s / samples, 1 - 1e-12)
        let k = p
        while (k < n - 1 && t >= knots[k + 1]) k++
        const d = []
        for (let j = 0; j <= p; j++) d.push(ctrl[k - p + j])
        for (let r = 1; r <= p; r++)
            for (let j = p; j >= r; j--) {
                const i = k - p + j, span = knots[i + p - r + 1] - knots[i]
                d[j] = lerp(d[j - 1], d[j], span < 1e-12 ? 0 : (t - knots[i]) / span)
            }
        out.push(s === samples ? ctrl[n - 1] : d[p])
    }
    return out
}
function fitSpline(pts) {
    if (pts.length < 2)
        return pts.slice()
    const q = [sub(mul(pts[0], 2), pts[1])].concat(pts, [sub(mul(pts[pts.length - 1], 2), pts[pts.length - 2])])
    const knot = (a, b) => Math.max(1e-6, Math.sqrt(dist(a, b)))
    const at = (a, b, ta, tb, t) => lerp(a, b, Math.abs(tb - ta) < 1e-12 ? 0 : (t - ta) / (tb - ta))
    const out = []
    for (let i = 1; i + 2 < q.length; i++) {
        const p0 = q[i - 1], p1 = q[i], p2 = q[i + 1], p3 = q[i + 2]
        const t1 = knot(p0, p1), t2 = t1 + knot(p1, p2), t3 = t2 + knot(p2, p3)
        for (let s = i === 1 ? 0 : 1; s <= 24; s++) {
            const t = t1 + (t2 - t1) * s / 24
            const a1 = at(p0, p1, 0, t1, t), a2 = at(p1, p2, t1, t2, t), a3 = at(p2, p3, t2, t3, t)
            out.push(at(at(a1, a2, 0, t2, t), at(a2, a3, t1, t3, t), t1, t2, t))
        }
    }
    return out
}
function conic(a, b, apex, rho) {
    const w = Math.min(0.99, Math.max(0.01, rho)) / (1 - Math.min(0.99, Math.max(0.01, rho))), out = []
    for (let i = 0; i <= 48; i++) {
        const t = i / 48, b0 = (1 - t) * (1 - t), b1 = 2 * t * (1 - t) * w, b2 = t * t, s = b0 + b1 + b2
        out.push(P((b0 * a.u + b1 * apex.u + b2 * b.u) / s, (b0 * a.v + b1 * apex.v + b2 * b.v) / s))
    }
    return out
}
function ellipse(c, major, minorLength) {
    const a = len(sub(major, c)), e = norm(sub(major, c)), n = left(e), out = []
    for (let i = 0; i <= 96; i++) {
        const t = 2 * Math.PI * i / 96
        out.push(add(c, add(mul(e, a * Math.cos(t)), mul(n, minorLength * Math.sin(t)))))
    }
    return out
}

// Signed area of a closed polyline (+ when it goes counter-clockwise).
function area(pts) {
    let s = 0
    for (let i = 0; i < pts.length; i++) {
        const a = pts[i], b = pts[(i + 1) % pts.length]
        s += a.u * b.v - b.u * a.v
    }
    return s / 2
}
