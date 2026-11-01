.pragma library

// Lengths as typed and shown (owner, 2026-10-29). Inches unless a unit is typed; the unit
// may follow the number with or without a space, short or spelled out:
//   2.5   2.5in   2.5 in   2.5"   2.5 inches   3/4   1 1/2   1-1/2 in   1' 6"   1 ft 6
//   60mm  60 millimeters   6.5 cm   1.2 m   2 feet
// Several parts add up (1' 6" = 18 in); a part with no unit of its own takes the next
// unit typed after it, and trailing ones are inches. The first unit typed becomes the
// dimension's main unit; the other system shows after it in brackets (HANDOFF "Units").

const units = {
    in: { mm: 25.4, names: ["in", "inch", "inches", "\"", "″", "''"], decimals: 3, imperial: true },
    ft: { mm: 304.8, names: ["ft", "foot", "feet", "'", "′"], decimals: 3, imperial: true },
    mm: { mm: 1, names: ["mm", "millimeter", "millimeters", "millimetre", "millimetres"], decimals: 2, imperial: false },
    cm: { mm: 10, names: ["cm", "centimeter", "centimeters", "centimetre", "centimetres"], decimals: 3, imperial: false },
    m:  { mm: 1000, names: ["m", "meter", "meters", "metre", "metres"], decimals: 4, imperial: false }
}

function unitNamed(word) {
    for (const key in units)
        if (units[key].names.indexOf(word) >= 0)
            return key
    return ""
}

// "1 1/2 in" → {mm: 38.1, unit: "in"}, or null when it isn't a length.
function parse(text) {
    let s = String(text).trim().toLowerCase().replace(/(\d)\s*-\s*(\d+\s*\/)/g, "$1 $2")
    if (!s)
        return null
    const part = /^\s*(\d+(?:\.\d*)?|\.\d+)(?:\s*\/\s*(\d+(?:\.\d*)?|\.\d+))?\s*(''|[a-z]+|["″'′])?/
    let pending = []          // numbers still waiting for a unit
    let total = 0, first = ""
    while (s.length) {
        const m = part.exec(s)
        if (!m)
            return null
        let n = parseFloat(m[1])
        if (m[2] !== undefined) {
            const d = parseFloat(m[2])
            if (!(d > 0))
                return null
            n /= d
        }
        pending.push(n)
        if (m[3] !== undefined) {
            const unit = unitNamed(m[3])
            if (!unit)
                return null
            if (!first)
                first = unit
            for (const v of pending)
                total += v * units[unit].mm
            pending = []
        }
        s = s.slice(m[0].length).replace(/^\s*[,+]?\s*/, "")
    }
    for (const v of pending)
        total += v * units.in.mm
    return { mm: total, unit: first || "in" }
}

// Angles (Sketch Dimension between two lines, Distance And Angle Chamfer, patterns): degrees
// as typed ("45", "45°", "45 deg"), kept in radians; unit "deg".
function parseAngle(text) {
    const m = /^\s*(-?\d+(?:\.\d*)?|-?\.\d+)\s*(°|deg|degs|degree|degrees)?\s*$/.exec(String(text).toLowerCase())
    return m ? { mm: parseFloat(m[1]) * Math.PI / 180, unit: "deg" } : null
}

function inUnit(mm, unit) {
    if (unit === "deg")
        return String(parseFloat((mm * 180 / Math.PI).toFixed(2))) + "°"
    const u = units[unit] || units.in
    const v = mm / u.mm
    const fixed = v.toFixed(u.decimals)
    // Imperial keeps its three places (1.000 in); metric drops the trailing zeros (25.4 mm).
    return (u.imperial ? fixed : String(parseFloat(fixed))) + " " + (units[unit] ? unit : "in")
}

// The dimension as shown: main unit, then the other system in brackets.
function show(mm, unit) {
    if (unit === "deg")
        return inUnit(mm, unit)
    const main = units[unit] ? unit : "in"
    return inUnit(mm, main) + " (" + inUnit(mm, units[main].imperial ? "mm" : "in") + ")"
}

// Just the main unit (what the input box starts with).
function plain(mm, unit) {
    if (unit === "deg")
        return String(parseFloat((mm * 180 / Math.PI).toFixed(2)))
    return inUnit(mm, units[unit] ? unit : "in")
}
