.pragma library

// Sketch mode's keys (owner, 2026-10-30): every drawing tool and constraint has one. They work
// only inside a sketch and only reach sketch tools; a model tool never runs from a sketch, so a
// clash with one of Fusion's model keys doesn't matter (F is the sketch Fillet here).
//   plain letter   drawing and modifying (Fusion's own where it has one: L R C D T O X M)
//   Shift+letter   constraints
//   Alt+letter     the less used drawing and modifying tools
// Free on purpose: P (Fusion's Project), I (Fusion's Measure), V (Fusion's Toggle Visibility),
// for when those come; Alt+V is the desktop's dictation key. A family's key (Families.js)
// draws its default kind. `letter` is what that one letter typed in the search picks first.
var keys = {
    // drawing and modifying
    "Line": { keys: "L", letter: "l" },
    "Sketch Dimension": { keys: "D", letter: "d" },
    "Ellipse": { keys: "E", letter: "e" },
    "Conic Curve": { keys: "Q", letter: "q" },
    "Fillet": { keys: "F", letter: "f" },
    "Offset": { keys: "O", letter: "o" },
    "Trim": { keys: "T", letter: "t" },
    "Break": { keys: "K", letter: "k" },
    "Move/Copy": { keys: "M", letter: "m" },
    "Toggle Construction": { keys: "X", letter: "x" },
    "Midpoint Line": { keys: "Alt L" },
    "Point": { keys: "Alt P" },
    "Text": { keys: "Alt T" },
    "Mirror": { keys: "Alt M" },
    "Circular Pattern": { keys: "Alt C" },
    "Rectangular Pattern": { keys: "Alt R" },
    "Extend": { keys: "Alt E" },
    "Sketch Scale": { keys: "Alt S" },
    "Blend Curve": { keys: "Alt B" },
    "Find Profile Gaps": { keys: "Alt G" },
    // constraints
    "Horizontal/Vertical": { keys: "Shift H", letter: "h" },
    "Coincident": { keys: "Shift C", letter: "" },
    "Tangent": { keys: "Shift T", letter: "" },
    "Equal": { keys: "Shift E", letter: "" },
    "Parallel": { keys: "Shift P", letter: "" },
    "Perpendicular": { keys: "Shift R", letter: "" },     // a Right angle
    "Fix/UnFix": { keys: "Shift F", letter: "" },
    "Midpoint": { keys: "Shift M", letter: "" },
    "Concentric": { keys: "Shift O", letter: "" },        // O inside O
    "Collinear": { keys: "Shift I", letter: "" },         // In line
    "Symmetry": { keys: "Shift S", letter: "" }
}

// A command with its sketch key and search letter put in (the rest left as they are).
function apply(command) {
    var k = keys[command.name]
    if (!k)
        return command
    var out = Object.assign({}, command)
    out.keys = k.keys
    if (k.letter !== undefined)
        out.letter = k.letter || undefined
    return out
}

// "Shift H" (how keys are shown) → "Shift+H" (a Shortcut's sequence).
function sequence(keys) {
    return keys.split(" ").join("+")
}
