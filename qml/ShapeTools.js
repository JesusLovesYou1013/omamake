.pragma library
.import "SketchMath.js" as M

// Every Fusion sketch tool past Line, 2-Point Rectangle, Center Diameter Circle and Center
// Point Arc (owner, 2026-10-30: "make sure we have every function in terms of sketching that
// Fusion 360 has"; each variation its own tool; Mirror and Offset too; Fillet and Chamfer easy
// to reach). SketchTools runs them: it walks a tool's `steps`, shows `preview`, and hands
// `build`'s spec to Document::addSketchShape. A tool stays on for the next one until Esc.
//
// steps, one per click (the prompt in `say`):
//   point    a snapped click: {u, v, point? | on?, line?, arc?}
//   points   clicks until Enter (or clicking the last one again): a list of those
//   pick     a line / arc ({part, at}); `accept`: which kinds
//   end      the end of a line or arc: {point, u, v, dir (outward, along it), kind, id}
//   corner   a point where exactly two lines meet: [{point, l1, l2, near}] (what's selected
//            when the tool starts counts: every corner between selected lines, all at once)
//   objects  lines, arcs, curves and points: clicked or already selected; Enter goes on
// options: [{key, label, kind: "length" | "angle" | "count" | "number" | "text" | "toggle", value}]
// preview(s), build(s), handle(s): s = {clicks, gather, cursor, hover, opt, g, pt, line, arc,
// curve, sweep, textOutline}. build gives a spec (Document::addSketchShape) or {why}.

// A spec being put together. pt() takes a snapped click, a place {u, v} or a point id and
// gives its index; lines, arcs and curves join indexes; constraints take ids or "p0", "l1"…
function Spec(summary) {
    this.points = []; this.lines = []; this.arcs = []; this.curves = []; this.constraints = []
    this.rewire = []; this.move = []; this.remove = { points: [], lines: [], arcs: [], curves: [], constraints: [] }
    this.helpers = []; this.values = []; this.steady = []; this.summary = summary
    this.like = []
    this._ids = {}
}
Spec.prototype.pt = function (x, helper) {
    if (typeof x === "number" || (x && x.point)) {
        const id = typeof x === "number" ? x : x.point
        if (this._ids[id] === undefined) {
            this._ids[id] = this.points.length
            this.points.push(id)
        }
        return this._ids[id]
    }
    const p = { u: x.u, v: x.v }
    if (x.on) { p.on = x.on; p.line = x.line || 0; p.arc = x.arc || 0 }
    if (helper) p.helper = true
    this.points.push(p)
    return this.points.length - 1
}
Spec.prototype.line = function (i, j, helper) { this.lines.push(helper ? [i, j, true] : [i, j]); return "l" + (this.lines.length - 1) }
Spec.prototype.arc = function (c, a, b, helper) { this.arcs.push(helper ? [c, a, b, true] : [c, a, b]); return "a" + (this.arcs.length - 1) }
Spec.prototype.circle = function (ci, centre, r, helper) {
    const rim = this.pt({ u: centre.u + r, v: centre.v }, helper)
    return this.arc(ci, rim, rim, helper)
}
Spec.prototype.curve = function (type, idx, value, text) {
    this.curves.push({ type: type, points: idx, value: value || 0, text: text || "" })
    return "k" + (this.curves.length - 1)
}
Spec.prototype.con = function (type, first, second, third, extra) {
    const c = { type: type, first: first, second: second || 0, third: third || 0 }
    for (const k in extra || {}) c[k] = extra[k]
    this.constraints.push(c)
    return "c" + (this.constraints.length - 1)
}
Spec.prototype.p = function (i) { return "p" + i }
// `made` ("l2", "a0", "k1") is a copy of line / arc / curve `from`: construction exactly when it is.
Spec.prototype.copyOf = function (made, from) { this.like.push({ made: made, from: from }); return made }

const at = c => M.P(c.u, c.v)
const why = text => ({ why: text })
const tiny = 1e-6

// Polylines for previews.
function closed(pts) { return pts.concat([pts[0]]) }
function circleLine(c, r) { return M.arcPoints(c, r, 0, 2 * Math.PI) }

// --- Rectangles ------------------------------------------------------------------------------
function rect3Corners(a, b, w) {
    const n = M.left(M.norm(M.sub(b, a)))
    return [a, b, M.add(b, M.mul(n, w)), M.add(a, M.mul(n, w))]
}
function rectCCorners(c, k) {
    const dx = Math.abs(k.u - c.u), dy = Math.abs(k.v - c.v)
    return [M.P(c.u - dx, c.v - dy), M.P(c.u + dx, c.v - dy), M.P(c.u + dx, c.v + dy), M.P(c.u - dx, c.v + dy)]
}

// --- Circles tangent to lines -----------------------------------------------------------------
function lineEnds(s, id) { const l = s.line(id); return [s.pt(l.a), s.pt(l.b)] }
function tangent2(s, l1, l2, p) {
    const [a1, b1] = lineEnds(s, l1), [a2, b2] = lineEnds(s, l2)
    const x = M.lineLine(a1, b1, a2, b2)
    if (!x) {
        const gap = M.across(a2, a1, b1) / 2, mid = M.add(a1, M.mul(M.left(M.norm(M.sub(b1, a1))), gap))
        return { c: M.foot(p, mid, M.add(mid, M.sub(b1, a1))), r: Math.abs(gap) }
    }
    const d1 = M.norm(M.sub(b1, a1)), d2 = M.norm(M.sub(b2, a2))
    const towards = M.norm(M.sub(p, x.at))
    let best = null
    for (const b of [M.norm(M.add(d1, d2)), M.norm(M.sub(d1, d2))])
        for (const sign of [1, -1]) {
            const dir = M.mul(b, sign)
            if (!best || M.dot(towards, dir) > M.dot(towards, best))
                best = dir
        }
    const c = M.add(x.at, M.mul(best, Math.max(tiny, M.dot(M.sub(p, x.at), best))))
    return { c: c, r: Math.abs(M.across(c, a1, b1)) }
}
function tangent3(s, ids, p) {
    const rows = ids.map(id => {
        const [a, b] = lineEnds(s, id), n = M.left(M.norm(M.sub(b, a)))
        return { n: n, k: M.dot(n, a) }
    })
    let best = null
    for (const signs of [[1, 1, 1], [1, 1, -1], [1, -1, 1], [-1, 1, 1]]) {
        // n_i · x − s_i r = k_i, by Cramer's rule.
        const A = rows.map((r, i) => [r.n.u, r.n.v, -signs[i]]), B = rows.map(r => r.k)
        const det = m => m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) - m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0])
                       + m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0])
        const D = det(A)
        if (Math.abs(D) < 1e-12)
            continue
        const col = j => A.map((row, i) => row.map((v, k) => k === j ? B[i] : v))
        let x = det(col(0)) / D, y = det(col(1)) / D, r = det(col(2)) / D
        if (r < 0)
            r = -r
        if (r < tiny)
            continue
        const c = M.P(x, y)
        if (!best || M.dist(c, p) < M.dist(best.c, p))
            best = { c: c, r: r }
    }
    return best
}

// --- Slots --------------------------------------------------------------------------------------
function straightSlot(b, i1, i2, c1, c2, r) {
    const n = M.left(M.norm(M.sub(c2, c1))), off = M.mul(n, r)
    const t1 = b.pt(M.add(c1, off)), b1 = b.pt(M.sub(c1, off)), t2 = b.pt(M.add(c2, off)), b2 = b.pt(M.sub(c2, off))
    const top = b.line(t1, t2), bottom = b.line(b1, b2)
    const end2 = b.arc(i2, b2, t2), end1 = b.arc(i1, t1, b1)
    for (const l of [top, bottom])
        for (const a of [end1, end2])
            b.con("tangent", l, a)
    b.con("equalradius", end1, end2)
}
function straightSlotPreview(c1, c2, r) {
    const d = M.norm(M.sub(c2, c1)), n = M.left(d), off = M.mul(n, r)
    if (M.len(d) < tiny)
        return []
    return [[M.add(c1, off), M.add(c2, off)], [M.sub(c1, off), M.sub(c2, off)],
            M.arcPoints(c2, r, M.angleOf(n) - Math.PI, Math.PI), M.arcPoints(c1, r, M.angleOf(n), Math.PI)]
}
// An arc slot round C at radius R, from angle a0 by sweep, r wide each side.
function arcSlotShape(C, R, a0, sweep, r) {
    if (sweep < 0) { a0 += sweep; sweep = -sweep }
    const a1 = a0 + sweep
    return { C: C, R: R, a0: a0, a1: a1, r: r, s: M.polar(C, R, a0), e: M.polar(C, R, a1) }
}
function arcSlotPreview(k) {
    if (k.R <= k.r)
        return []
    const sw = k.a1 - k.a0
    return [M.arcPoints(k.C, k.R + k.r, k.a0, sw), M.arcPoints(k.C, k.R - k.r, k.a0, sw),
            M.arcPoints(k.s, k.r, k.a0 + Math.PI, Math.PI), M.arcPoints(k.e, k.r, k.a1, Math.PI)]
}
function arcSlot(b, ci, k) {
    if (k.R <= k.r)
        return why("The slot is wider than its arc: make it narrower.")
    const si = b.pt(k.s), ei = b.pt(k.e)
    const os = b.pt(M.polar(k.C, k.R + k.r, k.a0)), oe = b.pt(M.polar(k.C, k.R + k.r, k.a1))
    const is = b.pt(M.polar(k.C, k.R - k.r, k.a0)), ie = b.pt(M.polar(k.C, k.R - k.r, k.a1))
    const outer = b.arc(ci, os, oe), inner = b.arc(ci, is, ie)
    const capS = b.arc(si, is, os), capE = b.arc(ei, oe, ie)
    b.arc(ci, si, ei, true)                    // the centre line, holding both caps on it
    for (const cap of [capS, capE]) {
        b.con("tangentarcs", cap, outer)
        b.con("tangentarcs", cap, inner)
    }
    b.con("equalradius", capS, capE)
    return b
}

// --- Polygons -------------------------------------------------------------------------------------
function polygonPoints(c, first, n) {
    const out = []
    for (let k = 0; k < n; k++)
        out.push(M.rotate(first, c, 2 * Math.PI * k / n))
    return out
}
// Lines round the corners (the first one as clicked), held regular: every corner on a helper
// circle (or every side touching it) and every side equal.
function polygonSpec(b, centre, ci, corners, firstClick, r, touching) {
    const idx = corners.map((p, k) => k === 0 && firstClick ? b.pt(firstClick) : b.pt(p))
    const lines = idx.map((i, k) => b.line(i, idx[(k + 1) % idx.length]))
    const ring = b.circle(ci, centre, r, true)
    if (touching)
        lines.forEach(l => b.con("tangent", l, ring))
    else
        idx.forEach(i => b.con("pointonarc", b.p(i), ring))
    for (let k = 1; k < lines.length; k++)
        b.con("equal", lines[0], lines[k])
    return b
}

// --- Whole drawn things (Mirror, patterns, Move/Copy, Sketch Scale) ----------------------------------
// The points a list of parts is made of, and what joins them.
function partsOf(s, parts) {
    const out = { points: new Set(), lines: [], arcs: [], curves: [], lone: [] }
    for (const p of parts) {
        if (p.kind === "sketch_line" && p.index > 0) {
            const l = s.line(p.index)
            if (l) { out.lines.push(l); out.points.add(l.a); out.points.add(l.b) }
        } else if (p.kind === "sketch_arc") {
            const k = s.arc(p.index)
            if (k) { out.arcs.push(k); out.points.add(k.c); out.points.add(k.a); out.points.add(k.b) }
        } else if (p.kind === "sketch_curve") {
            const k = s.curve(p.index)
            if (k) { out.curves.push(k); k.points.forEach(q => out.points.add(q)) }
        } else if (p.kind === "sketch_point" && p.index > 0) {
            out.points.add(p.index)
            out.lone.push(p.index)
        }
    }
    return out
}
// A copy of `set` with every point put through `map` (id → place). keep(id) → true: the copy
// uses the same point (it's on the mirror line, or the pattern's centre). tie(id, index): the
// constraint holding a copied point to its original. Circles get a fresh rim and Equal radius.
function copyParts(s, b, set, map, keep, tie, flip) {
    const idx = {}
    const rims = new Set(set.arcs.filter(k => k.circle).map(k => k.a))
    for (const id of set.points) {
        if (rims.has(id))
            continue
        if (keep && keep(id)) {
            idx[id] = b.pt(id)
            continue
        }
        idx[id] = b.pt(map(id))
        if (tie)
            tie(id, idx[id])
    }
    for (const l of set.lines)
        b.copyOf(b.line(idx[l.a], idx[l.b]), l.id)
    for (const k of set.arcs) {
        if (k.circle) {
            const a = b.copyOf(b.circle(idx[k.c], b.points[idx[k.c]].u !== undefined ? b.points[idx[k.c]] : s.pt(k.c), k.r), k.id)
            if (tie)
                b.con("equalradius", k.id, a)
        } else {
            b.copyOf(b.arc(idx[k.c], flip ? idx[k.b] : idx[k.a], flip ? idx[k.a] : idx[k.b]), k.id)
        }
    }
    for (const k of set.curves)
        b.copyOf(b.curve(k.type, k.points.map(q => idx[q]), k.value, k.text), k.id)
    return idx
}
function partsPreview(s, set, map) {
    const out = []
    for (const l of set.lines)
        out.push([map(l.a), map(l.b)])
    for (const k of set.arcs) {
        const c = map(k.c)
        out.push(k.circle ? circleLine(c, k.r) : M.arcBetween(c, map(k.a), map(k.b)))
    }
    for (const k of set.curves)
        out.push(curvePreview(k.type, k.points.map(map), k.value))
    for (const id of set.lone) {
        const p = map(id)
        out.push(circleLine(p, 0.4))
    }
    return out
}
function curvePreview(type, pts, value) {
    if (type === "fitspline") return M.fitSpline(pts)
    if (type === "controlspline") return M.bspline(pts)
    if (type === "conic") return M.conic(pts[0], pts[1], pts[2], value)
    if (type === "ellipse") return M.ellipse(pts[0], pts[1], M.dist(pts[0], pts[2]))
    return pts
}

// --- Corners (Fillet, Chamfer) -----------------------------------------------------------------------
// The two lines meeting at a corner, from the corner out: {P, A, B, uA, uB, theta, l1, l2, endA, endB}.
function cornerOf(s, c) {
    const L1 = s.line(c.l1), L2 = s.line(c.l2)
    const P = s.pt(c.point), A = s.pt(L1.a === c.point ? L1.b : L1.a), B = s.pt(L2.a === c.point ? L2.b : L2.a)
    const uA = M.norm(M.sub(A, P)), uB = M.norm(M.sub(B, P))
    return { P: P, A: A, B: B, uA: uA, uB: uB, theta: Math.acos(Math.max(-1, Math.min(1, M.dot(uA, uB)))),
             l1: c.l1, l2: c.l2, endA: L1.a === c.point ? "a" : "b", endB: L2.a === c.point ? "a" : "b",
             lenA: M.dist(A, P), lenB: M.dist(B, P) }
}
function filletShape(k, r) {
    if (k.theta < 1e-3 || k.theta > Math.PI - 1e-3)
        return why("Those lines are in line: there's no corner to round.")
    const t = r / Math.tan(k.theta / 2)
    const T1 = M.add(k.P, M.mul(k.uA, t)), T2 = M.add(k.P, M.mul(k.uB, t))
    const c = M.add(k.P, M.mul(M.norm(M.add(k.uA, k.uB)), r / Math.sin(k.theta / 2)))
    return { t: t, T1: T1, T2: T2, c: c, ccw: M.cross(M.sub(T1, c), M.sub(T2, c)) > 0 }
}
// Chamfer sides along each line: {d1, d2} for kind "equal" | "two" | "angle".
function chamferSides(k, kind, opt) {
    if (kind === "equal")
        return { d1: opt.distance, d2: opt.distance }
    if (kind === "two")
        return { d1: opt.distance, d2: opt.distance2 }
    const rest = Math.PI - opt.angle - k.theta
    if (rest <= 1e-3)
        return why("That angle doesn't fit this corner.")
    return { d1: opt.distance, d2: opt.distance * Math.sin(opt.angle) / Math.sin(rest) }
}
function cornerTools(kind) {
    const options = kind === "fillet" ? [{ key: "radius", label: "Radius", kind: "length", value: 6.35 }]
                  : kind === "equal" ? [{ key: "distance", label: "Distance", kind: "length", value: 3.175 }]
                  : kind === "two" ? [{ key: "distance", label: "Distance 1", kind: "length", value: 3.175 },
                                      { key: "distance2", label: "Distance 2", kind: "length", value: 6.35 }]
                  : [{ key: "distance", label: "Distance", kind: "length", value: 3.175 },
                     { key: "angle", label: "Angle", kind: "angle", value: Math.PI / 4 }]
    const shape = (s, c) => {
        const k = cornerOf(s, c)
        if (kind === "fillet") {
            const f = filletShape(k, s.opt.radius)
            return f.why ? f : Object.assign(f, { k: k, a: f.t, b: f.t })
        }
        const d = chamferSides(k, kind, s.opt)
        if (d.why)
            return d
        // Two Distance: "Distance 1" along the line nearer the click.
        const swap = kind !== "equal" && c.near && M.dist(c.near, M.foot(c.near, k.P, k.B)) < M.dist(c.near, M.foot(c.near, k.P, k.A))
        const a = swap ? d.d2 : d.d1, b = swap ? d.d1 : d.d2
        return { k: k, a: a, b: b, T1: M.add(k.P, M.mul(k.uA, a)), T2: M.add(k.P, M.mul(k.uB, b)), swap: swap }
    }
    return {
        options: options,
        steps: [{ kind: "corner", say: kind === "fillet" ? "Click a corner to round it (or select lines first: every corner at once)"
                                                         : "Click a corner to cut it (or select lines first: every corner at once)" }],
        preview: s => {
            const out = []
            for (const c of s.clicks[0] || s.gather || []) {
                const f = shape(s, c)
                if (f.why)
                    continue
                out.push(kind === "fillet" ? M.arcBetween(f.c, f.ccw ? f.T1 : f.T2, f.ccw ? f.T2 : f.T1) : [f.T1, f.T2])
            }
            return out
        },
        build: s => {
            const corners = s.clicks[0]
            const b = new Spec(kind === "fillet" ? (corners.length > 1 ? "Rounded " + corners.length + " corners" : "Fillet at Point" + corners[0].point)
                               : (corners.length > 1 ? "Chamfered " + corners.length + " corners" : "Chamfer at Point" + corners[0].point))
            const used = {}                          // how much of each line the corners take
            let leader = null, leader2 = null
            for (const c of corners) {
                const f = shape(s, c)
                if (f.why)
                    return f
                used[f.k.l1] = (used[f.k.l1] || 0) + f.a
                used[f.k.l2] = (used[f.k.l2] || 0) + f.b
                for (const [l, length] of [[f.k.l1, f.k.lenA], [f.k.l2, f.k.lenB]])
                    if (used[l] >= length - tiny)
                        return why(kind === "fillet" ? "That radius is too big for Line" + l + ": make it smaller."
                                                     : "That's longer than Line" + l + ": make it smaller.")
                const t1 = b.pt(f.T1), t2 = b.pt(f.T2)
                b.rewire.push({ line: f.k.l1, end: f.k.endA, to: b.p(t1) }, { line: f.k.l2, end: f.k.endB, to: b.p(t2) })
                if (kind === "fillet") {
                    const ci = b.pt(f.c), arc = b.arc(ci, f.ccw ? t1 : t2, f.ccw ? t2 : t1)
                    b.con("tangent", f.k.l1, arc)
                    b.con("tangent", f.k.l2, arc)
                    const dim = b.con("radius", arc, 0, leader || 0, { value: s.opt.radius, unit: "in" })
                    leader = leader || dim
                    b.remove.points.push(c.point)
                    continue
                }
                // Distance And Angle: the cut runs the way that makes its angle with the line
                // the one typed (30°, not 150°).
                let flip = false
                if (kind === "angle") {
                    const L = s.line(f.swap ? f.k.l2 : f.k.l1)
                    flip = M.dot(M.sub(s.pt(L.b), s.pt(L.a)), M.sub(f.T2, f.T1)) < 0
                }
                const cut = flip ? b.line(t2, t1) : b.line(t1, t2)
                b.helpers.push(c.point)                // the lost corner stays, unseen, to measure from
                b.con("pointonline", c.point, f.k.l1)
                b.con("pointonline", c.point, f.k.l2)
                const d1 = b.con("distance", c.point, b.p(t1), leader || 0, { value: f.a, unit: "in" })
                leader = leader || d1
                if (kind === "equal") {
                    b.con("distance", c.point, b.p(t2), d1, { value: f.b, unit: "in" })
                } else if (kind === "two") {
                    const d2 = b.con("distance", c.point, b.p(t2), leader2 || 0, { value: f.b, unit: "in" })
                    leader2 = leader2 || d2
                } else {
                    // The angle between the first line (as it runs once cut) and the cut.
                    const near = f.swap ? f.T2 : f.T1, far = f.swap ? f.k.B : f.k.A
                    const endIsA = (f.swap ? f.k.endB : f.k.endA) === "a"
                    const dir = endIsA ? M.sub(far, near) : M.sub(near, far)
                    const cutDir = flip ? M.sub(f.T1, f.T2) : M.sub(f.T2, f.T1)
                    const angle = Math.atan2(Math.abs(M.cross(dir, cutDir)), M.dot(dir, cutDir))
                    b.con("angle", f.swap ? f.k.l2 : f.k.l1, cut, 0, { value: angle, unit: "deg" })
                }
            }
            return b
        }
    }
}

// --- Offset --------------------------------------------------------------------------------------------
// The picked lines and arcs as chains joined end to end: [{items: [{kind, id, fwd}], closed}].
function chains(s, parts) {
    const ents = [], circles = []
    for (const p of parts) {
        if (p.kind === "sketch_line" && p.index > 0) {
            const l = s.line(p.index)
            if (l) ents.push({ kind: "line", id: l.id, s: l.a, e: l.b })
        } else if (p.kind === "sketch_arc") {
            const k = s.arc(p.index)
            if (k && k.circle) circles.push(k)
            else if (k) ents.push({ kind: "arc", id: k.id, s: k.a, e: k.b, c: k.c })
        }
    }
    const at = {}
    ents.forEach((e, i) => { (at[e.s] = at[e.s] || []).push(i); (at[e.e] = at[e.e] || []).push(i) })
    const used = new Set(), out = []
    const walk = first => {
        const items = []
        let i = first, point = (at[ents[i].s].length === 1 || at[ents[i].e].length !== 1) ? ents[i].s : ents[i].e
        const start = point
        while (i !== undefined && !used.has(i)) {
            used.add(i)
            const fwd = ents[i].s === point
            items.push({ kind: ents[i].kind, id: ents[i].id, fwd: fwd })
            point = fwd ? ents[i].e : ents[i].s
            i = (at[point] || []).find(j => !used.has(j))
        }
        return { items: items, closed: point === start && items.length > 1 }
    }
    // Open chains from their loose ends first, then the closed loops.
    ents.forEach((e, i) => { if (!used.has(i) && (at[e.s].length === 1 || at[e.e].length === 1)) out.push(walk(i)) })
    ents.forEach((e, i) => { if (!used.has(i)) out.push(walk(i)) })
    for (const k of circles)
        out.push({ items: [{ kind: "circle", id: k.id, fwd: true }], closed: true })
    return out
}
// One chain's pieces as travelled: start, end, and the curve.
function travelled(s, item) {
    if (item.kind === "line") {
        const l = s.line(item.id), a = s.pt(l.a), b = s.pt(l.b)
        return item.fwd ? { kind: "line", id: item.id, p: a, q: b, ip: l.a, iq: l.b } : { kind: "line", id: item.id, p: b, q: a, ip: l.b, iq: l.a }
    }
    const k = s.arc(item.id), c = s.pt(k.c), a = s.pt(k.a), b = s.pt(k.b)
    if (item.kind === "circle")
        return { kind: "circle", id: item.id, c: c, r: k.r, ic: k.c }
    return item.fwd ? { kind: "arc", id: item.id, c: c, r: k.r, p: a, q: b, ccw: true, ic: k.c, ip: k.a, iq: k.b }
                    : { kind: "arc", id: item.id, c: c, r: k.r, p: b, q: a, ccw: false, ic: k.c, ip: k.b, iq: k.a }
}
function travelPath(t) {
    if (t.kind === "line") return [t.p, t.q]
    if (t.kind === "circle") return circleLine(t.c, t.r)
    const path = M.arcBetween(t.c, t.ccw ? t.p : t.q, t.ccw ? t.q : t.p)
    return t.ccw ? path : path.reverse()
}
// Every chain moved `d` (+ outward for a closed loop, + to the left for an open chain), corners
// joined where the moved pieces meet. [{pieces: [{t, o: moved {kind, p, q, c, r}}], joints, closed}]
function offsetChains(s, parts, d) {
    const out = []
    for (const ch of chains(s, parts)) {
        const ts = ch.items.map(it => travelled(s, it))
        let side = 1
        if (ch.closed) {
            const loop = [].concat(...ts.map(travelPath))
            side = M.area(loop) > 0 ? -1 : 1           // counter-clockwise: outward is to the right
        }
        const D = d * side
        const moved = ts.map(t => {
            if (t.kind === "line") {
                const n = M.mul(M.left(M.norm(M.sub(t.q, t.p))), D)
                return { kind: "line", p: M.add(t.p, n), q: M.add(t.q, n) }
            }
            const r = t.kind === "circle" ? t.r + d : t.ccw ? t.r - D : t.r + D
            if (t.kind === "circle")
                return { kind: "circle", c: t.c, r: r }
            return { kind: "arc", c: t.c, r: r, p: M.add(t.c, M.mul(M.norm(M.sub(t.p, t.c)), r)),
                     q: M.add(t.c, M.mul(M.norm(M.sub(t.q, t.c)), r)), ccw: t.ccw }
        })
        if (moved.some(m => m.r !== undefined && m.r <= tiny))
            return why("That's further in than a curve can go: make it smaller.")
        const meet = (a, b, near) => {
            if (M.dist(a.q, b.p) < tiny)
                return a.q
            let found = []
            if (a.kind === "line" && b.kind === "line") { const x = M.lineLine(a.p, a.q, b.p, b.q); found = x ? [x.at] : [] }
            else if (a.kind === "line") found = M.lineCircle(a.p, a.q, b.c, b.r).map(x => x.at)
            else if (b.kind === "line") found = M.lineCircle(b.p, b.q, a.c, a.r).map(x => x.at)
            else found = M.circleCircle(a.c, a.r, b.c, b.r)
            if (!found.length)
                return M.mid(a.q, b.p)
            return found.reduce((best, p) => M.dist(p, near) < M.dist(best, near) ? p : best)
        }
        const n = moved.length, joints = []
        if (moved[0].kind !== "circle")
            for (let i = 0; i < n; i++) {
                const j = (i + 1) % n
                if (j === 0 && !ch.closed)
                    break
                joints.push(meet(moved[i], moved[j], M.mid(moved[i].q, moved[j].p)))
            }
        out.push({ ts: ts, moved: moved, joints: joints, closed: ch.closed, side: side })
    }
    return out
}
function offsetEnds(o, i) {
    const n = o.moved.length
    const start = i > 0 ? o.joints[i - 1] : o.closed ? o.joints[n - 1] : o.moved[0].p
    const end = i < n - 1 || o.closed ? o.joints[i] : o.moved[n - 1].q
    return [start, end]
}
// Where Offset's arrow starts and which way + is: the middle of the first piece, outward.
function offsetHandle(s, parts) {
    const o = offsetChains(s, parts, 0)
    if (o.why || !o.length)
        return null
    const t = o[0].ts[0], side = o[0].side
    if (t.kind === "line")
        return { base: M.mid(t.p, t.q), dir: M.mul(M.left(M.norm(M.sub(t.q, t.p))), side) }
    if (t.kind === "circle")
        return { base: M.polar(t.c, t.r, Math.PI / 4), dir: M.P(Math.cos(Math.PI / 4), Math.sin(Math.PI / 4)) }
    const path = travelPath(t), m = path[Math.floor(path.length / 2)], out = M.norm(M.sub(m, t.c))
    return { base: m, dir: M.mul(out, t.ccw ? -side * -1 : side) }
}

// --- Trim, Extend, Break -------------------------------------------------------------------------------
// A line or arc as something to cut: {kind, id, a, b, c, r, from, sweep, circle}.
function cutOf(s, part) {
    if (part.kind === "sketch_line" && part.index > 0) {
        const l = s.line(part.index)
        return l && { kind: "line", id: l.id, a: s.pt(l.a), b: s.pt(l.b), ia: l.a, ib: l.b }
    }
    if (part.kind === "sketch_arc") {
        const k = s.arc(part.index)
        if (!k)
            return null
        const c = s.pt(k.c), a = s.pt(k.a), b = s.pt(k.b), from = M.angleOf(M.sub(a, c))
        return { kind: "arc", id: k.id, c: c, r: k.r, ic: k.c, ia: k.a, ib: k.b, a: a, b: b, from: from, circle: k.circle,
                 sweep: k.circle ? 2 * Math.PI : (M.wrap(M.angleOf(M.sub(b, c)) - from) || 2 * Math.PI) }
    }
    return null
}
function paramOf(e, p) {
    if (e.kind === "line") { const d = M.sub(e.b, e.a); return M.dot(M.sub(p, e.a), d) / M.dot(d, d) }
    return M.wrap(M.angleOf(M.sub(p, e.c)) - e.from)
}
function pointAt(e, t) { return e.kind === "line" ? M.lerp(e.a, e.b, t) : M.polar(e.c, e.r, e.from + t) }
function onRange(e, t, eps) { return e.kind === "line" ? t >= -eps && t <= 1 + eps : e.circle || t <= e.sweep + eps }
// Where everything else drawn crosses e's whole line / circle: [{t, at, other: {kind, id}}].
// within: only where it's on e itself (else along its extension too, for Extend).
function crossings(s, e, within) {
    const out = [], eps = 1e-9
    const add = (p, other) => {
        const t = paramOf(e, p)
        if (!within || onRange(e, t, eps))
            out.push({ t: t, at: p, other: other })
    }
    for (const l of s.g.lines) {
        if (e.kind === "line" && l.id === e.id)
            continue
        const a = s.pt(l.a), b = s.pt(l.b)
        const other = { kind: "line", id: l.id }
        if (e.kind === "line") {
            const x = M.lineLine(e.a, e.b, a, b)
            if (x && x.s >= -eps && x.s <= 1 + eps) add(x.at, other)
        } else {
            for (const x of M.lineCircle(a, b, e.c, e.r))
                if (x.t >= -eps && x.t <= 1 + eps) add(x.at, other)
        }
    }
    for (const k of s.g.arcs) {
        if (e.kind === "arc" && k.id === e.id)
            continue
        const o = cutOf(s, { kind: "sketch_arc", index: k.id }), other = { kind: "arc", id: k.id }
        const hits = e.kind === "line" ? M.lineCircle(e.a, e.b, o.c, o.r).map(x => x.at) : M.circleCircle(e.c, e.r, o.c, o.r)
        for (const p of hits)
            if (onRange(o, paramOf(o, p), eps)) add(p, other)
    }
    for (const k of s.g.curves) {
        if (k.type === "text")
            continue
        const path = curvePreview(k.type, k.points.map(id => s.pt(id)), k.value), other = { kind: "curve", id: k.id }
        for (let i = 0; i + 1 < path.length; i++) {
            const hits = e.kind === "line" ? (x => x && x.s >= -eps && x.s <= 1 + eps ? [x.at] : [])(M.lineLine(e.a, e.b, path[i], path[i + 1]))
                                           : M.lineCircle(path[i], path[i + 1], e.c, e.r).filter(x => x.t >= 0 && x.t <= 1).map(x => x.at)
            hits.forEach(p => add(p, other))
        }
    }
    return out
}
// The stretch of e the click is on: {lo, hi} crossings either side (null: an end, or none).
function stretchAt(s, e, click) {
    const tc = paramOf(e, click)
    const end = e.kind === "line" ? 1 : e.sweep
    const xs = crossings(s, e, true).filter(x => x.t > 1e-6 * (e.kind === "line" ? 1 : 1) && x.t < end - 1e-6)
                                   .sort((p, q) => p.t - q.t)
    if (e.circle && xs.length) {
        // Round a circle: the crossings either side, going round.
        const before = xs.filter(x => x.t < tc), after = xs.filter(x => x.t > tc)
        return { tc: tc, xs: xs, lo: before.length ? before[before.length - 1] : xs[xs.length - 1],
                 hi: after.length ? after[0] : xs[0] }
    }
    let lo = null, hi = null
    for (const x of xs) {
        if (x.t < tc) lo = x
        else if (!hi) hi = x
    }
    return { tc: tc, xs: xs, lo: lo, hi: hi }
}
function holdOn(b, pointIndex, other) {
    if (other.kind === "line") b.con("pointonline", b.p(pointIndex), other.id)
    else if (other.kind === "arc") b.con("pointonarc", b.p(pointIndex), other.id)
}
function stretchPreview(e, from, to) {
    if (e.kind === "line")
        return [pointAt(e, from), pointAt(e, to)]
    let sweep = to - from
    if (sweep <= 0) sweep += 2 * Math.PI
    return M.arcPoints(e.c, e.r, e.from + from, sweep)
}
function trimPiece(s, e, st) {
    const end = e.kind === "line" ? 1 : e.sweep
    if (e.circle && st.xs.length < 2)
        return null
    return { from: st.lo ? st.lo.t : 0, to: st.hi ? st.hi.t : end }
}
// How many drawn things use a point.
function uses(s, id) {
    let n = 0
    for (const l of s.g.lines) n += (l.a === id) + (l.b === id)
    for (const k of s.g.arcs) n += (k.c === id) + (k.a === id) + (k.b === id && !k.circle)
    for (const k of s.g.curves) n += k.points.filter(q => q === id).length
    return n
}

// --- The tools ------------------------------------------------------------------------------------------
const tools = {
    midline: {
        name: "Midpoint Line", group: "Create", words: "line middle", hint: "Click the middle, then one end",
        steps: [{ kind: "point", say: "Click the middle of the line" }, { kind: "point", say: "Now one end" }],
        preview: s => s.cursor && s.clicks.length === 1
            ? [[M.sub(M.mul(at(s.clicks[0]), 2), at(s.cursor)), at(s.cursor)]] : [],
        build: s => {
            const m = at(s.clicks[0]), e = at(s.clicks[1])
            if (M.dist(m, e) < tiny)
                return why("Click away from the middle.")
            const b = new Spec("Drew a Midpoint Line")
            const mi = b.pt(s.clicks[0]), ei = b.pt(s.clicks[1]), oi = b.pt(M.sub(M.mul(m, 2), e))
            const l = b.line(ei, oi)
            b.con("midpoint", b.p(mi), l)
            return b
        }
    },
    rect3: {
        name: "3-Point Rectangle", group: "Create", words: "rectangle three", hint: "Click two corners for one side, then how wide",
        steps: [{ kind: "point", say: "Click the first corner" }, { kind: "point", say: "Now the end of that side" },
                { kind: "point", say: "Now how wide it is" }],
        preview: s => {
            if (!s.cursor) return []
            if (s.clicks.length === 1) return [[at(s.clicks[0]), at(s.cursor)]]
            const a = at(s.clicks[0]), b = at(s.clicks[1])
            return [closed(rect3Corners(a, b, M.across(at(s.cursor), a, b)))]
        },
        build: s => {
            const a = at(s.clicks[0]), bb = at(s.clicks[1]), w = M.across(at(s.clicks[2]), a, bb)
            if (M.dist(a, bb) < tiny || Math.abs(w) < tiny)
                return why("Give it some width: click away from the first side.")
            const b = new Spec("Drew a 3-Point Rectangle"), q = rect3Corners(a, bb, w)
            const idx = [b.pt(s.clicks[0]), b.pt(s.clicks[1]), b.pt(s.clicks[2].point ? s.clicks[2] : q[2]), b.pt(q[3])]
            const l = idx.map((i, k) => b.line(i, idx[(k + 1) % 4]))
            b.con("perpendicular", l[0], l[1])
            b.con("parallel", l[0], l[2])
            b.con("parallel", l[1], l[3])
            return b
        }
    },
    rectc: {
        name: "Center Rectangle", group: "Create", words: "rectangle centre center", hint: "Click the centre, then a corner",
        steps: [{ kind: "point", say: "Click the centre" }, { kind: "point", say: "Now a corner" }],
        preview: s => s.cursor ? [closed(rectCCorners(at(s.clicks[0]), at(s.cursor)))] : [],
        build: s => {
            const c = at(s.clicks[0]), k = at(s.clicks[1])
            if (Math.abs(k.u - c.u) < tiny || Math.abs(k.v - c.v) < tiny)
                return why("Pull the corner away from the centre both ways.")
            const b = new Spec("Drew a Center Rectangle"), q = rectCCorners(c, k)
            const ci = b.pt(s.clicks[0])
            const mine = (k.u > c.u ? (k.v > c.v ? 2 : 1) : (k.v > c.v ? 3 : 0))
            const idx = q.map((p, i) => b.pt(i === mine ? s.clicks[1] : p))
            const l = idx.map((i, n) => b.line(i, idx[(n + 1) % 4]))
            // A rectangle by its own sides, free to turn (owner, 2026-10-30: never Horizontal/Vertical).
            b.con("perpendicular", l[0], l[1]); b.con("parallel", l[0], l[2]); b.con("parallel", l[1], l[3])
            b.con("midpoints", b.p(ci), b.p(idx[0]), b.p(idx[2]))
            return b
        }
    },
    circle2: {
        name: "2-Point Circle", group: "Create", words: "circle two diameter", hint: "Click both ends of a diameter",
        steps: [{ kind: "point", say: "Click one side of the circle" }, { kind: "point", say: "Now the opposite side" }],
        preview: s => s.cursor ? [circleLine(M.mid(at(s.clicks[0]), at(s.cursor)), M.dist(at(s.clicks[0]), at(s.cursor)) / 2)] : [],
        build: s => {
            const a = at(s.clicks[0]), e = at(s.clicks[1]), r = M.dist(a, e) / 2
            if (r < tiny)
                return why("Click away from the first side: that sets the size.")
            const b = new Spec("Drew a 2-Point Circle"), c = M.mid(a, e)
            const k = b.circle(b.pt(c), c, r)
            for (const p of s.clicks)
                if (p.point) b.con("pointonarc", p.point, k)
            return b
        }
    },
    circle3: {
        name: "3-Point Circle", group: "Create", words: "circle three", hint: "Click three places on the circle",
        steps: [{ kind: "point", say: "Click a place on the circle" }, { kind: "point", say: "Now a second" }, { kind: "point", say: "Now a third" }],
        preview: s => {
            if (!s.cursor) return []
            if (s.clicks.length === 1) return [[at(s.clicks[0]), at(s.cursor)]]
            const k = M.circumcircle(at(s.clicks[0]), at(s.clicks[1]), at(s.cursor))
            return k ? [circleLine(k.c, k.r)] : [[at(s.clicks[0]), at(s.clicks[1])]]
        },
        build: s => {
            const k = M.circumcircle(at(s.clicks[0]), at(s.clicks[1]), at(s.clicks[2]))
            if (!k)
                return why("Those three are in a line: no circle goes through them.")
            const b = new Spec("Drew a 3-Point Circle"), a = b.circle(b.pt(k.c), k.c, k.r)
            for (const p of s.clicks)
                if (p.point) b.con("pointonarc", p.point, a)
            return b
        }
    },
    circle2t: {
        name: "2-Tangent Circle", group: "Create", words: "circle tangent", hint: "Click two lines, then where the circle goes",
        steps: [{ kind: "pick", accept: ["sketch_line"], say: "Click the first line it touches" },
                { kind: "pick", accept: ["sketch_line"], say: "Now the second line" },
                { kind: "point", say: "Now where the circle goes (that sets its size)" }],
        preview: s => {
            if (s.clicks.length < 2 || !s.cursor) return []
            const k = tangent2(s, s.clicks[0].part.index, s.clicks[1].part.index, at(s.cursor))
            return k ? [circleLine(k.c, k.r)] : []
        },
        build: s => {
            const l1 = s.clicks[0].part.index, l2 = s.clicks[1].part.index
            const k = tangent2(s, l1, l2, at(s.clicks[2]))
            if (!k || k.r < tiny)
                return why("Click further from where the lines meet.")
            const b = new Spec("Drew a 2-Tangent Circle"), a = b.circle(b.pt(k.c), k.c, k.r)
            b.con("tangent", l1, a)
            b.con("tangent", l2, a)
            return b
        }
    },
    circle3t: {
        name: "3-Tangent Circle", group: "Create", words: "circle tangent three", hint: "Click three lines, then which circle",
        steps: [{ kind: "pick", accept: ["sketch_line"], say: "Click the first line it touches" },
                { kind: "pick", accept: ["sketch_line"], say: "Now the second line" },
                { kind: "pick", accept: ["sketch_line"], say: "Now the third line" },
                { kind: "point", say: "Click inside the circle you want (there can be several)" }],
        preview: s => {
            if (s.clicks.length < 3 || !s.cursor) return []
            const k = tangent3(s, s.clicks.slice(0, 3).map(c => c.part.index), at(s.cursor))
            return k ? [circleLine(k.c, k.r)] : []
        },
        build: s => {
            const ids = s.clicks.slice(0, 3).map(c => c.part.index)
            const k = tangent3(s, ids, at(s.clicks[3]))
            if (!k)
                return why("No circle touches all three of those lines.")
            const b = new Spec("Drew a 3-Tangent Circle"), a = b.circle(b.pt(k.c), k.c, k.r)
            ids.forEach(l => b.con("tangent", l, a))
            return b
        }
    },
    arc3: {
        name: "3-Point Arc", group: "Create", words: "arc three", hint: "Click the start, the end, then a place it passes",
        steps: [{ kind: "point", say: "Click where the arc starts" }, { kind: "point", say: "Now where it ends" },
                { kind: "point", say: "Now a place it passes through" }],
        preview: s => {
            if (!s.cursor) return []
            if (s.clicks.length === 1) return [[at(s.clicks[0]), at(s.cursor)]]
            const a = at(s.clicks[0]), e = at(s.clicks[1]), k = M.circumcircle(a, e, at(s.cursor))
            if (!k) return [[a, e]]
            return [M.ccwBetween(k.c, a, e, at(s.cursor)) ? M.arcBetween(k.c, a, e) : M.arcBetween(k.c, e, a)]
        },
        build: s => {
            const a = at(s.clicks[0]), e = at(s.clicks[1]), p = at(s.clicks[2]), k = M.circumcircle(a, e, p)
            if (!k)
                return why("Those three are in a line: click off the line for a curve.")
            const b = new Spec("Drew a 3-Point Arc")
            const ci = b.pt(k.c), ai = b.pt(s.clicks[0]), ei = b.pt(s.clicks[1])
            if (M.ccwBetween(k.c, a, e, p)) b.arc(ci, ai, ei)
            else b.arc(ci, ei, ai)
            return b
        }
    },
    arctan: {
        name: "Tangent Arc", group: "Create", words: "arc tangent", hint: "Click the end of a line or arc, then where the arc ends",
        steps: [{ kind: "end", say: "Click the end of a line or arc to carry on from" }, { kind: "point", say: "Now where the arc ends" }],
        shape: (s, q) => {
            const e = s.clicks[0], P = at(e), n = M.left(e.dir), den = 2 * M.dot(n, M.sub(q, P))
            if (Math.abs(den) < 1e-9)
                return null
            const t = M.dot(M.sub(q, P), M.sub(q, P)) / den
            return { c: M.add(P, M.mul(n, t)), left: t > 0, P: P }
        },
        preview: s => {
            if (!s.cursor || !s.clicks.length) return []
            const k = tools.arctan.shape(s, at(s.cursor))
            return k ? [k.left ? M.arcBetween(k.c, k.P, at(s.cursor)) : M.arcBetween(k.c, at(s.cursor), k.P)] : [[at(s.clicks[0]), at(s.cursor)]]
        },
        build: s => {
            const k = tools.arctan.shape(s, at(s.clicks[1]))
            if (!k)
                return why("That's straight on from it: draw a Line there instead.")
            const b = new Spec("Drew a Tangent Arc"), e = s.clicks[0]
            const ci = b.pt(k.c), pi = b.pt(e.point), qi = b.pt(s.clicks[1])
            const a = k.left ? b.arc(ci, pi, qi) : b.arc(ci, qi, pi)
            b.con(e.kind === "line" ? "tangent" : "tangentarcs", e.id, a)
            return b
        }
    },
    polyin: {
        name: "Inscribed Polygon", group: "Create", words: "polygon hexagon inscribed", hint: "Click the centre, then a corner",
        options: [{ key: "sides", label: "Sides", kind: "count", value: 6 }],
        steps: [{ kind: "point", say: "Click the centre" }, { kind: "point", say: "Now a corner (type the number of sides above)" }],
        preview: s => s.cursor ? [closed(polygonPoints(at(s.clicks[0]), at(s.cursor), Math.max(3, s.opt.sides)))] : [],
        build: s => {
            const c = at(s.clicks[0]), v = at(s.clicks[1]), n = Math.max(3, Math.round(s.opt.sides))
            if (M.dist(c, v) < tiny)
                return why("Click away from the centre: that sets the size.")
            const b = new Spec("Drew an Inscribed Polygon (" + n + " sides)")
            return polygonSpec(b, c, b.pt(s.clicks[0]), polygonPoints(c, v, n), s.clicks[1], M.dist(c, v), false)
        }
    },
    polyout: {
        name: "Circumscribed Polygon", group: "Create", words: "polygon hexagon circumscribed", hint: "Click the centre, then the middle of a side",
        options: [{ key: "sides", label: "Sides", kind: "count", value: 6 }],
        steps: [{ kind: "point", say: "Click the centre" }, { kind: "point", say: "Now the middle of a side" }],
        corners: (c, m, n) => {
            const a = M.dist(c, m), R = a / Math.cos(Math.PI / n)
            return polygonPoints(c, M.polar(c, R, M.angleOf(M.sub(m, c)) - Math.PI / n), n)
        },
        preview: s => s.cursor ? [closed(tools.polyout.corners(at(s.clicks[0]), at(s.cursor), Math.max(3, s.opt.sides)))] : [],
        build: s => {
            const c = at(s.clicks[0]), m = at(s.clicks[1]), n = Math.max(3, Math.round(s.opt.sides))
            if (M.dist(c, m) < tiny)
                return why("Click away from the centre: that sets the size.")
            const b = new Spec("Drew a Circumscribed Polygon (" + n + " sides)")
            return polygonSpec(b, c, b.pt(s.clicks[0]), tools.polyout.corners(c, m, n), null, M.dist(c, m), true)
        }
    },
    polyedge: {
        name: "Edge Polygon", group: "Create", words: "polygon hexagon edge", hint: "Click both ends of one side, then which way it goes",
        options: [{ key: "sides", label: "Sides", kind: "count", value: 6 }],
        steps: [{ kind: "point", say: "Click one end of a side" }, { kind: "point", say: "Now the other end" },
                { kind: "point", say: "Now which side the polygon goes" }],
        shape: (p0, p1, side, n) => {
            const e = M.sub(p1, p0), len = M.len(e), dir = M.across(side, p0, p1) >= 0 ? 1 : -1
            const c = M.add(M.mid(p0, p1), M.mul(M.left(M.norm(e)), dir * (len / 2) / Math.tan(Math.PI / n)))
            const turn = M.cross(M.sub(p0, c), M.sub(p1, c)) > 0 ? 1 : -1
            const corners = []
            for (let k = 0; k < n; k++) corners.push(M.rotate(p0, c, turn * 2 * Math.PI * k / n))
            return { c: c, corners: corners, r: M.dist(c, p0) }
        },
        preview: s => {
            if (!s.cursor) return []
            if (s.clicks.length === 1) return [[at(s.clicks[0]), at(s.cursor)]]
            return [closed(tools.polyedge.shape(at(s.clicks[0]), at(s.clicks[1]), at(s.cursor), Math.max(3, s.opt.sides)).corners)]
        },
        build: s => {
            const p0 = at(s.clicks[0]), p1 = at(s.clicks[1]), n = Math.max(3, Math.round(s.opt.sides))
            if (M.dist(p0, p1) < tiny)
                return why("Give the side some length.")
            const k = tools.polyedge.shape(p0, p1, at(s.clicks[2]), n)
            const b = new Spec("Drew an Edge Polygon (" + n + " sides)")
            const ci = b.pt(k.c, true)
            const corners = k.corners.slice()
            const idx = corners.map((p, i) => i === 0 ? b.pt(s.clicks[0]) : i === 1 ? b.pt(s.clicks[1]) : b.pt(p))
            const lines = idx.map((i, j) => b.line(i, idx[(j + 1) % n]))
            const ring = b.circle(ci, k.c, k.r, true)
            idx.forEach(i => b.con("pointonarc", b.p(i), ring))
            for (let j = 1; j < n; j++) b.con("equal", lines[0], lines[j])
            return b
        }
    },
    slotcc: {
        name: "Center To Center Slot", group: "Create", words: "slot", hint: "Click both arc centres, then how wide",
        steps: [{ kind: "point", say: "Click the first end's centre" }, { kind: "point", say: "Now the other end's centre" },
                { kind: "point", say: "Now how wide" }],
        preview: s => {
            if (!s.cursor) return []
            if (s.clicks.length === 1) return [[at(s.clicks[0]), at(s.cursor)]]
            const a = at(s.clicks[0]), e = at(s.clicks[1])
            return straightSlotPreview(a, e, Math.abs(M.across(at(s.cursor), a, e)))
        },
        build: s => {
            const a = at(s.clicks[0]), e = at(s.clicks[1]), r = Math.abs(M.across(at(s.clicks[2]), a, e))
            if (M.dist(a, e) < tiny || r < tiny)
                return why("Give the slot some length and width.")
            const b = new Spec("Drew a Center To Center Slot")
            straightSlot(b, b.pt(s.clicks[0]), b.pt(s.clicks[1]), a, e, r)
            return b
        }
    },
    slotov: {
        name: "Overall Slot", group: "Create", words: "slot", hint: "Click both far ends, then how wide",
        steps: [{ kind: "point", say: "Click one far end of the slot" }, { kind: "point", say: "Now the other far end" },
                { kind: "point", say: "Now how wide" }],
        centres: (a, e, r) => { const d = M.norm(M.sub(e, a)); return [M.add(a, M.mul(d, r)), M.sub(e, M.mul(d, r))] },
        preview: s => {
            if (!s.cursor) return []
            if (s.clicks.length === 1) return [[at(s.clicks[0]), at(s.cursor)]]
            const a = at(s.clicks[0]), e = at(s.clicks[1]), r = Math.abs(M.across(at(s.cursor), a, e))
            if (M.dist(a, e) <= 2 * r) return [[a, e]]
            const [c1, c2] = tools.slotov.centres(a, e, r)
            return straightSlotPreview(c1, c2, r)
        },
        build: s => {
            const a = at(s.clicks[0]), e = at(s.clicks[1]), r = Math.abs(M.across(at(s.clicks[2]), a, e))
            if (r < tiny || M.dist(a, e) <= 2 * r + tiny)
                return why("The slot has to be longer than it is wide.")
            const [c1, c2] = tools.slotov.centres(a, e, r)
            const b = new Spec("Drew an Overall Slot")
            straightSlot(b, b.pt(c1), b.pt(c2), c1, c2, r)
            return b
        }
    },
    slotcp: {
        name: "Center Point Slot", group: "Create", words: "slot centre center", hint: "Click the middle, then one end's centre, then how wide",
        steps: [{ kind: "point", say: "Click the middle of the slot" }, { kind: "point", say: "Now one end's centre" },
                { kind: "point", say: "Now how wide" }],
        preview: s => {
            if (!s.cursor) return []
            const m = at(s.clicks[0])
            if (s.clicks.length === 1) return [[M.sub(M.mul(m, 2), at(s.cursor)), at(s.cursor)]]
            const e = at(s.clicks[1]), a = M.sub(M.mul(m, 2), e)
            return straightSlotPreview(a, e, Math.abs(M.across(at(s.cursor), a, e)))
        },
        build: s => {
            const m = at(s.clicks[0]), e = at(s.clicks[1]), a = M.sub(M.mul(m, 2), e)
            const r = Math.abs(M.across(at(s.clicks[2]), a, e))
            if (M.dist(a, e) < tiny || r < tiny)
                return why("Give the slot some length and width.")
            const b = new Spec("Drew a Center Point Slot")
            const mi = b.pt(s.clicks[0]), ai = b.pt(a), ei = b.pt(s.clicks[1])
            straightSlot(b, ai, ei, a, e, r)
            b.con("midpoints", b.p(mi), b.p(ai), b.p(ei))
            return b
        }
    },
    slot3a: {
        name: "Three Point Arc Slot", group: "Create", words: "slot arc three", hint: "Click the start, the end, a place it passes, then how wide",
        steps: [{ kind: "point", say: "Click where the slot's arc starts" }, { kind: "point", say: "Now where it ends" },
                { kind: "point", say: "Now a place it passes through" }, { kind: "point", say: "Now how wide" }],
        shape: (a, e, p, w) => {
            const k = M.circumcircle(a, e, p)
            if (!k) return null
            const from = M.angleOf(M.sub(a, k.c)), toE = M.wrap(M.angleOf(M.sub(e, k.c)) - from)
            const sweep = M.ccwBetween(k.c, a, e, p) ? toE : toE - 2 * Math.PI
            return arcSlotShape(k.c, k.r, from, sweep, w ? Math.abs(M.dist(w, k.c) - k.r) : 0)
        },
        preview: s => {
            if (!s.cursor) return []
            if (s.clicks.length === 1) return [[at(s.clicks[0]), at(s.cursor)]]
            if (s.clicks.length === 2) {
                const k = tools.slot3a.shape(at(s.clicks[0]), at(s.clicks[1]), at(s.cursor))
                return k ? [M.arcPoints(k.C, k.R, k.a0, k.a1 - k.a0)] : []
            }
            const k = tools.slot3a.shape(at(s.clicks[0]), at(s.clicks[1]), at(s.clicks[2]), at(s.cursor))
            return k ? arcSlotPreview(k) : []
        },
        build: s => {
            const k = tools.slot3a.shape(at(s.clicks[0]), at(s.clicks[1]), at(s.clicks[2]), at(s.clicks[3]))
            if (!k || k.r < tiny)
                return why(k ? "Give the slot some width." : "Those three are in a line: click off the line for a curve.")
            const b = new Spec("Drew a Three Point Arc Slot")
            return arcSlot(b, b.pt(k.C), k)
        }
    },
    slotca: {
        name: "Center Point Arc Slot", group: "Create", words: "slot arc centre center", hint: "Click the centre, the start, the end, then how wide",
        sweepAround: 0,
        steps: [{ kind: "point", say: "Click the centre of the slot's arc" }, { kind: "point", say: "Now where it starts" },
                { kind: "point", say: "Now where it ends (it goes the way you move round)" }, { kind: "point", say: "Now how wide" }],
        shape: (s, w) => {
            const c = at(s.clicks[0]), a = at(s.clicks[1])
            return arcSlotShape(c, M.dist(c, a), M.angleOf(M.sub(a, c)), s.clicks[2] ? s.clicks[2].sweep : s.sweep,
                                w ? Math.abs(M.dist(w, c) - M.dist(c, a)) : 0)
        },
        preview: s => {
            if (!s.cursor) return []
            if (s.clicks.length === 1) return [[at(s.clicks[0]), at(s.cursor)]]
            const k = tools.slotca.shape(s, s.clicks.length === 3 ? at(s.cursor) : null)
            return s.clicks.length === 2 ? [M.arcPoints(k.C, k.R, k.a0, k.a1 - k.a0)] : arcSlotPreview(k)
        },
        build: s => {
            const k = tools.slotca.shape(s, at(s.clicks[3]))
            if (k.r < tiny || k.a1 - k.a0 < 1e-3)
                return why("Give the slot some length and width.")
            const b = new Spec("Drew a Center Point Arc Slot")
            return arcSlot(b, b.pt(s.clicks[0]), k)
        }
    },
    splinefit: {
        name: "Fit Point Spline", group: "Create", words: "spline curve", hint: "Click the points it goes through; Enter ends it",
        steps: [{ kind: "points", min: 2, say: "Click each point the curve goes through; Enter (or the last point again) ends it" }],
        preview: s => { const pts = (s.gather || []).map(at).concat(s.cursor ? [at(s.cursor)] : []); return pts.length > 1 ? [M.fitSpline(pts)] : [] },
        build: s => {
            const b = new Spec("Drew a Fit Point Spline")
            b.curve("fitspline", s.clicks[0].map(c => b.pt(c)))
            return b
        }
    },
    splinectl: {
        name: "Control Point Spline", group: "Create", words: "spline curve control", hint: "Click the points that pull it; Enter ends it",
        steps: [{ kind: "points", min: 2, say: "Click each control point; Enter (or the last point again) ends it" }],
        preview: s => {
            const pts = (s.gather || []).map(at).concat(s.cursor ? [at(s.cursor)] : [])
            return pts.length > 1 ? [M.bspline(pts), pts] : []
        },
        build: s => {
            const b = new Spec("Drew a Control Point Spline")
            b.curve("controlspline", s.clicks[0].map(c => b.pt(c)))
            return b
        }
    },
    conic: {
        name: "Conic Curve", group: "Create", words: "curve conic", hint: "Click the start, the end, then the corner it bends toward",
        options: [{ key: "rho", label: "Rho", kind: "number", value: 0.5 }],
        steps: [{ kind: "point", say: "Click where the curve starts" }, { kind: "point", say: "Now where it ends" },
                { kind: "point", say: "Now the corner it bends toward (Rho above: 0 flat to 1 sharp)" }],
        preview: s => {
            if (!s.cursor) return []
            if (s.clicks.length === 1) return [[at(s.clicks[0]), at(s.cursor)]]
            return [M.conic(at(s.clicks[0]), at(s.clicks[1]), at(s.cursor), s.opt.rho)]
        },
        build: s => {
            if (!(s.opt.rho > 0 && s.opt.rho < 1))
                return why("Rho goes between 0 and 1.")
            const b = new Spec("Drew a Conic Curve")
            b.curve("conic", [b.pt(s.clicks[0]), b.pt(s.clicks[1]), b.pt(s.clicks[2])], s.opt.rho)
            return b
        }
    },
    point: {
        name: "Point", group: "Create", words: "point", hint: "Click where the point goes",
        steps: [{ kind: "point", say: "Click where the point goes" }],
        preview: s => [],
        build: s => {
            if (s.clicks[0].point)
                return why("There's a point there already.")
            const b = new Spec("Drew a Point")
            b.pt(s.clicks[0])
            return b
        }
    },
    ellipse: {
        name: "Ellipse", group: "Create", words: "ellipse oval", hint: "Click the centre, the end of the long axis, then how wide",
        steps: [{ kind: "point", say: "Click the centre" }, { kind: "point", say: "Now the end of one axis" }, { kind: "point", say: "Now how wide" }],
        preview: s => {
            if (!s.cursor) return []
            const c = at(s.clicks[0])
            if (s.clicks.length === 1) return [[c, at(s.cursor)]]
            return [M.ellipse(c, at(s.clicks[1]), Math.abs(M.across(at(s.cursor), c, at(s.clicks[1]))))]
        },
        build: s => {
            const c = at(s.clicks[0]), m = at(s.clicks[1]), w = Math.abs(M.across(at(s.clicks[2]), c, m))
            if (M.dist(c, m) < tiny || w < tiny)
                return why("Give it some length and width.")
            const b = new Spec("Drew an Ellipse")
            const ci = b.pt(s.clicks[0]), mi = b.pt(s.clicks[1]), wi = b.pt(M.add(c, M.mul(M.left(M.norm(M.sub(m, c))), w)))
            b.curve("ellipse", [ci, mi, wi])
            return b
        }
    },
    text: {
        name: "Text", group: "Create", words: "text letters words", hint: "Type the words above, then click where they go",
        options: [{ key: "text", label: "Text", kind: "text", value: "Text" }, { key: "height", label: "Height", kind: "length", value: 6.35 }],
        steps: [{ kind: "point", say: "Type the text above, then click where its bottom left goes" }],
        preview: s => s.cursor && s.opt.text ? s.textOutline(s.opt.text, s.opt.height).map(path => path.map(p => M.add(p, at(s.cursor)))) : [],
        build: s => {
            if (!String(s.opt.text).trim())
                return why("Type some text first.")
            const b = new Spec("Wrote \"" + s.opt.text + "\"")
            b.curve("text", [b.pt(s.clicks[0])], s.opt.height, s.opt.text)
            return b
        }
    },
    mirror: {
        name: "Mirror", group: "Create", words: "mirror flip symmetry copy", hint: "Pick what to mirror, Enter, then the line to mirror across",
        steps: [{ kind: "objects", say: "Click what to mirror, then press Enter" },
                { kind: "pick", accept: ["sketch_line"], axes: true, say: "Now the line to mirror across (any line, or an axis)" }],
        axisOf: (s, id) => id === -1 ? [M.P(s.g.origin.u, s.g.origin.v), M.P(s.g.origin.u + 1, s.g.origin.v)]
                         : id === -2 ? [M.P(s.g.origin.u, s.g.origin.v), M.P(s.g.origin.u, s.g.origin.v + 1)] : lineEnds(s, id),
        preview: s => {
            if (s.clicks.length < 1 || !s.hover || s.hover.kind !== "sketch_line") return []
            const [a, b] = tools.mirror.axisOf(s, s.hover.index)
            return partsPreview(s, partsOf(s, s.clicks[0]), id => M.mirror(s.pt(id), a, b))
        },
        build: s => {
            const axis = s.clicks[1].part.index, set = partsOf(s, s.clicks[0])
            if (!set.points.size)
                return why("Pick something to mirror first.")
            if (set.lines.some(l => l.id === axis))
                return why("The mirror line can't be one of the things mirrored.")
            const [a, bb] = tools.mirror.axisOf(s, axis)
            const b = new Spec("Mirrored " + s.clicks[0].length + (s.clicks[0].length === 1 ? " thing" : " things"))
            copyParts(s, b, set, id => M.mirror(s.pt(id), a, bb), id => Math.abs(M.across(s.pt(id), a, bb)) < tiny,
                      (id, i) => b.con("symmetry", id, b.p(i), axis), true)
            return b
        }
    },
    patternc: {
        name: "Circular Pattern", group: "Create", words: "pattern circular round copies array", hint: "Pick what to copy, Enter, then the centre",
        options: [{ key: "count", label: "Quantity", kind: "count", value: 3 }, { key: "angle", label: "Total angle", kind: "angle", value: 2 * Math.PI }],
        steps: [{ kind: "objects", say: "Click what to copy round, then press Enter" }, { kind: "point", say: "Now the centre to copy round" }],
        turns: s => {
            const n = Math.max(2, Math.round(s.opt.count)), full = Math.abs(s.opt.angle - 2 * Math.PI) < 1e-6
            const step = full ? 2 * Math.PI / n : s.opt.angle / (n - 1), out = []
            for (let k = 1; k < n; k++) out.push(step * k)
            return out
        },
        preview: s => {
            if (s.clicks.length < 1 || !s.cursor) return []
            const set = partsOf(s, s.clicks[0]), c = at(s.cursor)
            return [].concat(...tools.patternc.turns(s).map(t => partsPreview(s, set, id => M.rotate(s.pt(id), c, t))))
        },
        build: s => {
            const set = partsOf(s, s.clicks[0])
            if (!set.points.size)
                return why("Pick something to copy first.")
            const c = at(s.clicks[1]), b = new Spec("Circular Pattern of " + Math.max(2, Math.round(s.opt.count)))
            const ci = b.pt(s.clicks[1])
            for (const t of tools.patternc.turns(s))
                copyParts(s, b, set, id => M.rotate(s.pt(id), c, t), id => s.clicks[1].point === id,
                          (id, i) => b.con("rotation", id, b.p(i), b.p(ci), { value: t }))
            return b
        }
    },
    patternr: {
        name: "Rectangular Pattern", group: "Create", words: "pattern rectangular grid copies array", hint: "Pick what to copy, set how many and how far, Enter",
        options: [{ key: "countX", label: "Across", kind: "count", value: 3 }, { key: "spaceX", label: "Spacing", kind: "length", value: 25.4 },
                  { key: "countY", label: "Up", kind: "count", value: 1 }, { key: "spaceY", label: "Spacing", kind: "length", value: 25.4 }],
        steps: [{ kind: "objects", say: "Click what to copy, set how many and how far apart above, then press Enter" }],
        moves: s => {
            const out = []
            for (let i = 0; i < Math.max(1, Math.round(s.opt.countX)); i++)
                for (let j = 0; j < Math.max(1, Math.round(s.opt.countY)); j++)
                    if (i || j) out.push(M.P(i * s.opt.spaceX, j * s.opt.spaceY))
            return out
        },
        preview: s => {
            const set = partsOf(s, s.gather || [])
            return [].concat(...tools.patternr.moves(s).map(d => partsPreview(s, set, id => M.add(s.pt(id), d))))
        },
        build: s => {
            const set = partsOf(s, s.clicks[0]), moves = tools.patternr.moves(s)
            if (!set.points.size || !moves.length)
                return why(set.points.size ? "Make it more than one across or up." : "Pick something to copy first.")
            const b = new Spec("Rectangular Pattern of " + (moves.length + 1))
            for (const d of moves)
                copyParts(s, b, set, id => M.add(s.pt(id), d), null, (id, i) => b.con("translation", id, b.p(i), 0, { u: d.u, v: d.v }))
            return b
        }
    },
    fillet: Object.assign({ name: "Fillet", group: "Modify", words: "fillet round corner radius", hint: "Click corners to round them (radius above)" }, cornerTools("fillet")),
    chamferequal: Object.assign({ name: "Equal Distance Chamfer", group: "Modify", words: "chamfer bevel corner", hint: "Click corners to cut them, the same each way" }, cornerTools("equal")),
    chamferangle: Object.assign({ name: "Distance And Angle Chamfer", group: "Modify", words: "chamfer bevel corner angle", hint: "Click corners to cut them at an angle" }, cornerTools("angle")),
    chamfertwo: Object.assign({ name: "Two Distance Chamfer", group: "Modify", words: "chamfer bevel corner", hint: "Click corners to cut them, a different distance each way" }, cornerTools("two")),
    offset: {
        name: "Offset", group: "Modify", words: "offset parallel inset outline", hint: "Click a shape (all of it joined), drag the arrow or type how far, Enter",
        options: [{ key: "distance", label: "Distance", kind: "length", value: 3.175, signed: true }],
        steps: [{ kind: "objects", chain: true, say: "Click a shape (its joined lines come along), drag the arrow or type how far, then Enter" }],
        handle: s => {
            const h = offsetHandle(s, s.gather || [])
            return h && { base: h.base, dir: h.dir, key: "distance" }
        },
        preview: s => {
            const o = offsetChains(s, s.gather || [], s.opt.distance)
            if (o.why) return []
            const out = []
            for (const ch of o)
                ch.moved.forEach((m, i) => {
                    if (m.kind === "circle") return out.push(circleLine(m.c, m.r))
                    const [p, q] = offsetEnds(ch, i)
                    out.push(m.kind === "line" ? [p, q] : m.ccw ? M.arcBetween(m.c, p, q) : M.arcBetween(m.c, q, p))
                })
            return out
        },
        build: s => {
            const d = s.opt.distance
            if (Math.abs(d) < tiny)
                return why("Drag the arrow out or in, or type how far.")
            const o = offsetChains(s, s.clicks[0], d)
            if (o.why) return o
            if (!o.length) return why("Pick lines, arcs or circles to offset.")
            const b = new Spec("Offset " + s.clicks[0].length + (s.clicks[0].length === 1 ? " piece" : " pieces"))
            let leader = 0
            const dim = (type, first, second, side) => {
                const c = b.con(type, first, second, leader, { value: Math.abs(d), unit: "in", u: side })
                leader = leader || c
            }
            for (const ch of o) {
                const n = ch.moved.length
                if (ch.moved[0].kind === "circle") {
                    const t = ch.ts[0], m = ch.moved[0]
                    dim("offsetarcs", t.id, b.copyOf(b.circle(b.pt(t.ic), t.c, m.r), t.id), Math.sign(m.r - t.r))
                    continue
                }
                // The new pieces' ends: shared corners where they meet, loose ends on an open chain.
                const corner = ch.joints.map(p => b.pt(p))
                const starts = [], ends = []
                for (let i = 0; i < n; i++) {
                    starts.push(i > 0 ? corner[i - 1] : ch.closed ? corner[n - 1] : b.pt(ch.moved[0].p))
                    ends.push(i < n - 1 || ch.closed ? corner[i] : b.pt(ch.moved[n - 1].q))
                }
                for (let i = 0; i < n; i++) {
                    const t = ch.ts[i], m = ch.moved[i], p = starts[i], q = ends[i]
                    if (t.kind === "line") {
                        // Same way round as the original, so the offset's side reads the same.
                        const L = s.line(t.id), A = s.pt(L.a), B = s.pt(L.b), fwd = L.a === t.ip
                        const P = b.points[fwd ? p : q], Q = b.points[fwd ? q : p]
                        if (M.dot(M.sub(Q, P), M.sub(B, A)) <= 0)
                            return why("That's further in than the shape allows: make it smaller.")
                        dim("offset", t.id, b.copyOf(b.line(fwd ? p : q, fwd ? q : p), t.id), Math.sign(M.across(M.mid(P, Q), A, B)))
                    } else {
                        const a = b.copyOf(t.ccw ? b.arc(b.pt(t.ic), p, q) : b.arc(b.pt(t.ic), q, p), t.id)
                        dim("offsetarcs", t.id, a, Math.sign(m.r - t.r))
                    }
                }
            }
            return b
        }
    },
    trim: {
        name: "Trim", group: "Modify", words: "trim cut remove", hint: "Click the bit of a line, arc or circle to cut away",
        steps: [{ kind: "pick", accept: ["sketch_line", "sketch_arc"], say: "Click the bit to cut away (up to where it crosses something)" }],
        preview: s => {
            const e = s.hover && cutOf(s, s.hover)
            if (!e || !s.cursor) return []
            const st = stretchAt(s, e, at(s.cursor)), piece = trimPiece(s, e, st)
            return piece ? [stretchPreview(e, piece.from, piece.to)] : []
        },
        build: s => {
            const e = cutOf(s, s.clicks[0].part), st = stretchAt(s, e, at(s.clicks[0].at))
            const b = new Spec((e.kind === "line" ? "Trimmed Line" : e.circle ? "Trimmed Circle" : "Trimmed Arc") + e.id)
            const name = e.kind === "line" ? "line" : "arc"
            if (e.circle) {
                if (st.xs.length === 1)
                    return why("A circle needs two crossings to trim between.")
                if (!st.xs.length) { b.remove.arcs.push(e.id); b.remove.points.push(e.ia, e.ic); return b }
                const hi = b.pt(st.hi.at), lo = b.pt(st.lo.at)
                b.rewire.push({ arc: e.id, end: "a", to: b.p(hi) }, { arc: e.id, end: "b", to: b.p(lo) })
                holdOn(b, hi, st.hi.other); holdOn(b, lo, st.lo.other)
                b.remove.points.push(e.ia)
                return b
            }
            if (!st.lo && !st.hi) {
                b[e.kind === "line" ? "remove" : "remove"][e.kind === "line" ? "lines" : "arcs"].push(e.id)
                b.remove.points.push(e.ia, e.ib)
                if (e.kind === "arc") b.remove.points.push(e.ic)
                return b
            }
            const r = {}; r[name] = e.id
            if (!st.lo) {
                const x = b.pt(st.hi.at)
                b.rewire.push(Object.assign({ end: "a", to: b.p(x) }, r))
                holdOn(b, x, st.hi.other)
                b.remove.points.push(e.ia)
            } else if (!st.hi) {
                const x = b.pt(st.lo.at)
                b.rewire.push(Object.assign({ end: "b", to: b.p(x) }, r))
                holdOn(b, x, st.lo.other)
                b.remove.points.push(e.ib)
            } else {
                const x1 = b.pt(st.lo.at), x2 = b.pt(st.hi.at), ob = b.pt(e.ib)
                b.rewire.push(Object.assign({ end: "b", to: b.p(x1) }, r))
                if (e.kind === "line") {
                    const l = b.copyOf(b.line(x2, ob), e.id)
                    b.con("collinear", e.id, l)
                } else {
                    const a = b.copyOf(b.arc(b.pt(e.ic), x2, ob), e.id)
                    b.con("equalradius", e.id, a)
                }
                holdOn(b, x1, st.lo.other); holdOn(b, x2, st.hi.other)
            }
            return b
        }
    },
    extend: {
        name: "Extend", group: "Modify", words: "extend lengthen", hint: "Click near the end of a line or arc to stretch it to the next thing",
        steps: [{ kind: "pick", accept: ["sketch_line", "sketch_arc"], say: "Click near the end to stretch out to the next thing it meets" }],
        target: (s, part, click) => {
            const e = cutOf(s, part)
            if (!e || e.circle) return null
            const tc = paramOf(e, click), atB = e.kind === "line" ? tc > 0.5 : tc > e.sweep / 2
            const xs = crossings(s, e, false)
            let best = null
            for (const x of xs) {
                const far = e.kind === "line" ? (atB ? x.t - 1 : -x.t) : (atB ? x.t - e.sweep : M.wrap(-x.t))
                if (far > 1e-6 && (e.kind === "line" || far < 2 * Math.PI - e.sweep) && (!best || far < best.far))
                    best = { far: far, x: x }
            }
            return best && { e: e, atB: atB, x: best.x }
        },
        preview: s => {
            const k = s.hover && s.cursor && tools.extend.target(s, s.hover, at(s.cursor))
            if (!k) return []
            const from = k.atB ? (k.e.kind === "line" ? k.e.b : k.e.b) : k.e.a
            return k.e.kind === "line" ? [[from, k.x.at]]
                 : [k.atB ? M.arcBetween(k.e.c, k.e.b, k.x.at) : M.arcBetween(k.e.c, k.x.at, k.e.a)]
        },
        build: s => {
            const k = tools.extend.target(s, s.clicks[0].part, at(s.clicks[0].at))
            if (!k) return why("Nothing further along to stretch it to.")
            const e = k.e, end = k.atB ? e.ib : e.ia
            const b = new Spec("Extended " + (e.kind === "line" ? "Line" : "Arc") + e.id)
            let x
            if (uses(s, end) === 1) {
                x = b.pt(end)
                b.move.push({ point: end, u: k.x.at.u, v: k.x.at.v })
                // Held on what it stopped at (Trim leaves that): it lets go, or it drags that along.
                for (const c of s.g.constraints)
                    if (((c.type === "pointonline" || c.type === "pointonarc" || c.type === "midpoint") && c.first === end)
                            || (c.type === "coincident" && (c.first === end || c.second === end)))
                        b.remove.constraints.push(c.id)
            } else {
                x = b.pt(k.x.at)
                const r = { end: k.atB ? "b" : "a", to: b.p(x) }; r[e.kind] = e.id
                b.rewire.push(r)
            }
            holdOn(b, x, k.x.other)
            b.steady.push(b.p(x))
            return b
        }
    },
    "break": {
        name: "Break", group: "Modify", words: "break split divide", hint: "Click a line, arc or circle to split it where things cross it",
        steps: [{ kind: "pick", accept: ["sketch_line", "sketch_arc"], say: "Click the bit to break off (it splits where things cross it)" }],
        preview: s => tools.trim.preview(s),
        build: s => {
            const e = cutOf(s, s.clicks[0].part), st = stretchAt(s, e, at(s.clicks[0].at))
            const b = new Spec("Broke " + (e.kind === "line" ? "Line" : e.circle ? "Circle" : "Arc") + e.id)
            if (e.circle) {
                if (st.xs.length < 2) return why("A circle needs two crossings to break between.")
                const lo = b.pt(st.lo.at), hi = b.pt(st.hi.at)
                b.rewire.push({ arc: e.id, end: "a", to: b.p(lo) }, { arc: e.id, end: "b", to: b.p(hi) })
                const a = b.copyOf(b.arc(b.pt(e.ic), hi, lo), e.id)
                b.con("equalradius", e.id, a)
                holdOn(b, lo, st.lo.other); holdOn(b, hi, st.hi.other)
                b.remove.points.push(e.ia)
                return b
            }
            const cuts = [st.lo, st.hi].filter(x => x)
            if (!cuts.length) return why("Nothing crosses it there, so there's nowhere to break it.")
            const xs = cuts.map(x => b.pt(x.at)), ob = b.pt(e.ib)
            const r = { end: "b", to: b.p(xs[0]) }; r[e.kind] = e.id
            b.rewire.push(r)
            const chain = xs.concat([ob])
            for (let i = 0; i + 1 < chain.length; i++) {
                if (e.kind === "line") b.con("collinear", e.id, b.copyOf(b.line(chain[i], chain[i + 1]), e.id))
                else b.con("equalradius", e.id, b.copyOf(b.arc(b.pt(e.ic), chain[i], chain[i + 1]), e.id))
            }
            cuts.forEach((x, i) => holdOn(b, xs[i], x.other))
            return b
        }
    },
    scale: {
        name: "Sketch Scale", group: "Modify", words: "scale resize bigger smaller", hint: "Pick what to scale, Enter, then the point it scales from",
        options: [{ key: "factor", label: "Scale factor", kind: "number", value: 2 }],
        steps: [{ kind: "objects", say: "Click what to scale, then press Enter" }, { kind: "point", say: "Now the point it grows from (factor above)" }],
        preview: s => {
            if (s.clicks.length < 1 || !s.cursor) return []
            const c = at(s.cursor)
            return partsPreview(s, partsOf(s, s.clicks[0]), id => M.add(c, M.mul(M.sub(s.pt(id), c), s.opt.factor)))
        },
        build: s => {
            const f = s.opt.factor, set = partsOf(s, s.clicks[0]), c = at(s.clicks[1])
            if (!(f > 0)) return why("The scale factor has to be more than 0.")
            if (!set.points.size) return why("Pick something to scale first.")
            const b = new Spec("Scaled " + s.clicks[0].length + (s.clicks[0].length === 1 ? " thing" : " things") + " by " + f)
            for (const id of set.points) {
                const p = M.add(c, M.mul(M.sub(s.pt(id), c), f))
                b.move.push({ point: id, u: p.u, v: p.v })
                b.steady.push(id)
            }
            // Dimensions wholly inside grow with it.
            const lineIn = id => set.lines.some(l => l.id === id), arcIn = id => set.arcs.some(k => k.id === id)
            for (const k of s.g.constraints) {
                if (k.follows) continue
                const inside = k.type === "length" ? lineIn(k.first)
                             : k.type === "distance" ? set.points.has(k.first) && set.points.has(k.second)
                             : k.type === "pointline" ? set.points.has(k.first) && lineIn(k.second)
                             : k.type === "lineline" || k.type === "offset" ? lineIn(k.first) && lineIn(k.second)
                             : k.type === "radius" || k.type === "diameter" ? arcIn(k.first)
                             : k.type === "offsetarcs" ? arcIn(k.first) && arcIn(k.second) : false
                if (inside) b.values.push({ constraint: k.id, value: k.value * f })
            }
            return b
        }
    },
    blend: {
        name: "Blend Curve", group: "Modify", words: "blend curve join smooth", hint: "Click the ends of two lines or arcs to join them smoothly",
        steps: [{ kind: "end", say: "Click the end of a line or arc" }, { kind: "end", say: "Now the end of another" }],
        shape: (e1, e2) => {
            const p0 = at(e1), p3 = at(e2), k = M.dist(p0, p3) / 3
            return [p0, M.add(p0, M.mul(e1.dir, k)), M.add(p3, M.mul(e2.dir, k)), p3]
        },
        preview: s => s.clicks.length === 1 && s.cursor && s.cursorEnd ? [M.bspline(tools.blend.shape(s.clicks[0], s.cursorEnd))] : [],
        build: s => {
            const [e1, e2] = s.clicks
            if (e1.point === e2.point) return why("Pick the end of a different line or arc.")
            const q = tools.blend.shape(e1, e2), b = new Spec("Drew a Blend Curve")
            const i0 = b.pt(e1.point), i1 = b.pt(q[1], true), i2 = b.pt(q[2], true), i3 = b.pt(e2.point)
            b.curve("controlspline", [i0, i1, i2, i3])
            // Smooth where it joins: its first and last legs run on along each end.
            for (const [e, from, to] of [[e1, i0, i1], [e2, i3, i2]]) {
                const leg = b.line(from, to, true)
                if (e.kind === "line") b.con("collinear", e.id, leg)
                else b.con("tangent", leg, e.id)
            }
            return b
        }
    },
    movecopy: {
        name: "Move/Copy", group: "Modify", words: "move copy duplicate rotate", hint: "Pick what to move, Enter, then from where to where",
        options: [{ key: "angle", label: "Turn", kind: "angle", value: 0 }, { key: "copy", label: "Copy", kind: "toggle", value: false }],
        steps: [{ kind: "objects", say: "Click what to move, then press Enter" }, { kind: "point", say: "Now where to move it from" },
                { kind: "point", say: "Now where it goes (turn it, or make a copy, above)" }],
        map: (s, from, to) => id => M.add(M.rotate(s.pt(id), from, s.opt.angle), M.sub(to, from)),
        preview: s => {
            if (s.clicks.length < 2 || !s.cursor) return []
            return partsPreview(s, partsOf(s, s.clicks[0]), tools.movecopy.map(s, at(s.clicks[1]), at(s.cursor)))
        },
        build: s => {
            const set = partsOf(s, s.clicks[0]), map = tools.movecopy.map(s, at(s.clicks[1]), at(s.clicks[2]))
            if (!set.points.size) return why("Pick something to move first.")
            const n = s.clicks[0].length + (s.clicks[0].length === 1 ? " thing" : " things")
            const b = new Spec((s.opt.copy ? "Copied " : "Moved ") + n)
            if (s.opt.copy) {
                copyParts(s, b, set, map, null, null, false)
                return b
            }
            for (const id of set.points) {
                const p = map(id)
                b.move.push({ point: id, u: p.u, v: p.v })
                b.steady.push(id)
            }
            return b
        }
    }
}

// Search order: Fusion's Create menu, then Modify.
const order = ["midline", "rect3", "rectc", "circle2", "circle3", "circle2t", "circle3t", "arc3", "arctan",
               "polyin", "polyout", "polyedge", "slotcc", "slotov", "slotcp", "slot3a", "slotca",
               "splinefit", "splinectl", "conic", "point", "ellipse", "text", "mirror", "patternc", "patternr",
               "fillet", "chamferequal", "chamferangle", "chamfertwo", "offset", "trim", "extend", "break",
               "scale", "blend", "movecopy"]
