.pragma library

// Tools that come in kinds, as one search row each (owner, 2026-10-30): "circle" finds Circle,
// Enter draws its default kind, → shows every kind. The default is Fusion's first kind until the
// owner's own use says otherwise (see `next`); a family's keyboard key (Fusion's: R, C) runs the
// same default (OmaMake's own where Fusion has none: A G U B, Alt+F; see SketchKeys.js). Kinds by
// their command names, in Fusion's menu order.
var families = [
    { name: "Rectangle", letter: "r", keys: "R", words: "rectangle square box",
      members: ["2-Point Rectangle", "3-Point Rectangle", "Center Rectangle"] },
    { name: "Circle", letter: "c", keys: "C", words: "circle",
      members: ["Center Diameter Circle", "2-Point Circle", "3-Point Circle", "2-Tangent Circle", "3-Tangent Circle"] },
    { name: "Arc", letter: "a", keys: "A", words: "arc curve",
      members: ["3-Point Arc", "Center Point Arc", "Tangent Arc"] },
    { name: "Polygon", letter: "g", keys: "G", words: "polygon hexagon sides",
      members: ["Circumscribed Polygon", "Inscribed Polygon", "Edge Polygon"] },
    { name: "Slot", letter: "u", keys: "U", words: "slot",
      members: ["Center To Center Slot", "Overall Slot", "Center Point Slot", "Three Point Arc Slot", "Center Point Arc Slot"] },
    { name: "Spline", letter: "b", keys: "B", words: "spline curve b-spline",
      members: ["Fit Point Spline", "Control Point Spline"] },
    { name: "Chamfer", keys: "Alt F", words: "chamfer bevel corner",
      members: ["Equal Distance Chamfer", "Distance And Angle Chamfer", "Two Distance Chamfer"] }
]

// How many uses of each family are remembered, how many in a row make a new default, and how
// many of the remembered ones make it a habit even when they weren't in a row.
var remembered = 20
var inARow = 4
var habit = 8

// The family a command belongs to, or null.
function of(command) {
    for (var i = 0; i < families.length; i++)
        if (families[i].members.indexOf(command) >= 0)
            return families[i]
    return null
}

// After `command` was used: {uses, defaults} with it added (both plain objects: family name →
// its last uses, oldest first / its default). A kind becomes the default when it was used the
// last 4 times in a row, or 8 of the last 20 times and more often than the default then.
function next(uses, defaults, command) {
    var f = of(command)
    if (!f)
        return { uses: uses, defaults: defaults }
    var u = Object.assign({}, uses), d = Object.assign({}, defaults)
    var mine = (u[f.name] || []).concat([command]).slice(-remembered)
    u[f.name] = mine
    var now = d[f.name] && f.members.indexOf(d[f.name]) >= 0 ? d[f.name] : f.members[0]
    var count = function (name) { return mine.filter(function (m) { return m === name }).length }
    var last = mine.slice(-inARow)
    if (command !== now && ((last.length === inARow && last.every(function (m) { return m === command }))
                            || (count(command) >= habit && count(command) > count(now))))
        d[f.name] = command
    return { uses: u, defaults: d }
}

// A family's default kind now.
function defaultOf(f, defaults) {
    var d = defaults[f.name]
    return d && f.members.indexOf(d) >= 0 ? d : f.members[0]
}
