# OmaMake — Engine Handoff

The reference for every feature we have: what it does, where it lives, how it's reached in the UI,
whether it's built-in or a plugin, and how it differs between the two engines.
Rule: any engine change gets its entry here in the same commit.
This file documents engines, features and functions only.

## Entry format

```
#### <Feature name>                 [BUILT-IN | PLUGIN]   [STOCK | STOCK-CHANGED | OURS]
- What it does:
- UI: <button and where on screen> | search-only: "<words that find it>"
- Popup: "<popup name>": <every option and parameter>
- Interface hook: <our engine-interface function(s)>   ← plugins and UI only ever call this
- Plugin file: <plugins/<name>.lua>   (PLUGIN only)
- Heavy (exact surfaces):  <Class::method or function name> in <file>   | MISSING
- Lite (triangles):        <Class::method or function name> in <file>   | MISSING
- Engine differences: <what behaves differently between Heavy and Lite, or "none">
- Changes: <date: what we changed, in which engine, and why> (newest first)
```

- **Code locations are function/class names plus the file, never line numbers** (line numbers shift with every edit).
- **BUILT-IN** = compiled into the engine. **PLUGIN** = a Lua file loaded at startup. Plugins are listed in their own section below so they're never confused with built-ins.
- **MISSING** = that engine can't do the feature yet. Manifold3D only offers primitives, booleans, extrude/revolve and mesh operations, so most modelling features (fillet, chamfer, shell, thread...) start as MISSING or OURS on the Lite side.

## Engines

- **Heavy** (exact, smooth surfaces): OpenCascade, through the Dune 3D fork. Planned hard-fork name: **Open-Make-Heavy**.
- **Lite** (triangle meshes): Manifold3D. Planned hard-fork name: **Open-Make-Lite**.
- Long-term goal: both forked, optimized for speed, and gradually brought to the same features, so that the only difference is smooth surfaces vs triangles. Gradual, not all at once.

## Engine interface (planned)

Not written yet. The UI and every plugin call only this, never an engine directly.

## Built-in features

Not catalogued yet. Dune 3D 1.4 has one class per feature in `src/document/group/`:
extrude, revolve, lathe, fillet, chamfer, loft, sweep, pipe, circular sweep, mirror, mirror H/V,
linear array, polar array, clone, replicate, reference, sketch, solid-model operation,
local operation, exploded cluster. These get full entries once the source is checked out.

## Plugin features

None yet.
