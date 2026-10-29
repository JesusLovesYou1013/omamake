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

## Engine interface

`omamake::GeometryEngine` in `src/engine/engine.h`. The UI and every plugin call only this, never an engine directly.
Units are millimetres, Z up. A body is a `BodyId` (0 = none); operations return new bodies and leave their inputs alone.

| Hook | What it does |
|------|--------------|
| `name()` | The engine's display name |
| `box(x, y, z)` | A box with one corner at the origin |
| `extrude(profile, height)` | A flat profile on the XY plane pushed up along +Z (first loop outside, others holes) |
| `combine(a, b, Join \| Cut \| Intersect)` | Boolean of two bodies |
| `translate(body, x, y, z)` | A moved copy |
| `mesh(body)` | Triangles, normals and outline edges, ready to draw, plus the topology (faces, edges, points) used for hovering and selecting |
| `release(body)` | Frees a body the document no longer needs |

Engines are created by a factory: `makeLiteEngine()` (Heavy will get `makeHeavyEngine()`).

## Built-in features

### Solids

#### Box                                    [BUILT-IN]   [OURS]
- What it does: makes a box from three sizes. For now it builds the 12 in placeholder cube at startup.
- UI: none yet (startup only). Will be search: "box", "cube".
- Popup: none yet.
- Interface hook: `box(x, y, z)`, then `translate` to sit it centred on the floor.
- Heavy (exact surfaces): MISSING
- Lite (triangles): `LiteEngine::box` in `src/engine/lite/liteengine.cpp` (Manifold `Manifold::Cube`)
- Engine differences: none expected (flat faces are exact in both).
- Changes: 2026-10-29: added.

#### Extrude                                [BUILT-IN]   [OURS]
- What it does: pushes a flat profile up into a solid.
- UI: not wired yet. Planned: search "extrude", floating panel "Extrude": Distance, Direction (One side / Symmetric / Two sides), Result (Join / Cut / Intersect / New).
- Interface hook: `extrude(profile, height)`
- Heavy (exact surfaces): MISSING
- Lite (triangles): `LiteEngine::extrude` in `src/engine/lite/liteengine.cpp` (Manifold `Manifold::Extrude`, even-odd holes)
- Engine differences: Lite turns curved profile edges into straight segments.
- Changes: 2026-10-29: added.

#### Combine                                [BUILT-IN]   [OURS]
- What it does: joins, cuts or intersects two bodies.
- UI: not wired yet. Planned: the Result choice in other panels, and search "join", "cut", "intersect".
- Interface hook: `combine(a, b, op)`
- Heavy (exact surfaces): MISSING
- Lite (triangles): `LiteEngine::combine` in `src/engine/lite/liteengine.cpp` (Manifold `+`, `-`, `^`)
- Engine differences: none known yet.
- Changes: 2026-10-29: added.

#### Move                                   [BUILT-IN]   [OURS]
- What it does: a moved copy of a body.
- UI: none yet.
- Interface hook: `translate(body, x, y, z)`
- Heavy (exact surfaces): MISSING
- Lite (triangles): `LiteEngine::translate` in `src/engine/lite/liteengine.cpp`
- Engine differences: none.
- Changes: 2026-10-29: added.

### Display

#### Mesh for drawing                       [BUILT-IN]   [OURS]
- What it does: turns a body into triangles with flat face normals, plus outline edges wherever neighbouring triangles bend more than 20°.
- UI: every model on screen.
- Interface hook: `mesh(body)`
- Heavy (exact surfaces): MISSING
- Lite (triangles): `LiteEngine::mesh` in `src/engine/lite/liteengine.cpp`
- Engine differences: Heavy will tessellate exact surfaces to a chosen accuracy; Lite already is triangles.
- Changes: 2026-10-29: added. Flat shading only for now (curved faces will need smooth normals).

#### Topology (faces, edges, points)       [BUILT-IN]   [OURS]
- What it does: tells which triangles make up each face, which outline segments make up each edge, and where the corner points are, so a user can hover and select a whole face, edge or point the way CAD users expect (a cube: 6 faces, 12 edges, 8 points).
- UI: hover anything on the model (pale highlight); click selects it (strong highlight); Ctrl- or Shift-click adds or removes; click empty space or Esc clears. Points win over edges, edges over faces.
- Popup: none.
- Interface hook: `mesh(body)`, fields `faceCount`, `triangleFace`, `edges` (polylines), `points` of `omamake::Mesh`.
- Heavy (exact surfaces): MISSING (will come straight from the exact model's own faces, edges and vertices).
- Lite (triangles): `LiteEngine::mesh` in `src/engine/lite/liteengine.cpp`. A mesh has no true faces, so they are rebuilt: triangles are joined across every edge that bends less than 20° (the same rule as outline edges), so a cylinder's facets become one face. Edges are chains of outline segments between points; closed chains (a circle) have no points. Points are vertices where 1 or 3+ outline segments meet.
- Engine differences: Lite's faces depend on the 20° rule: a very gentle crease between two flat areas joins them into one face, and a very coarse curve could split into several. Heavy will not have this limit.
- Changes: 2026-10-29: added.

### Stock Dune 3D features (Heavy, not checked out yet)

Dune 3D 1.4 has one class per feature in `src/document/group/`: Dune 3D 1.4 has one class per feature in `src/document/group/`:
extrude, revolve, lathe, fillet, chamfer, loft, sweep, pipe, circular sweep, mirror, mirror H/V,
linear array, polar array, clone, replicate, reference, sketch, solid-model operation,
local operation, exploded cluster. These get full entries once the source is checked out.

## Plugin features

None yet.
