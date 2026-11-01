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

- **Heavy** (exact, smooth surfaces): OpenCascade, used directly (`opencascade` package, 7.9). The default engine since 2026-10-30; `OMAMAKE_ENGINE=lite` runs Lite instead. Planned hard-fork name: **Open-Make-Heavy**.
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
- Heavy (exact surfaces): `HeavyEngine::box` in `src/engine/heavy/heavyengine.cpp` (`BRepPrimAPI_MakeBox`)
- Lite (triangles): `LiteEngine::box` in `src/engine/lite/liteengine.cpp` (Manifold `Manifold::Cube`)
- Engine differences: none expected (flat faces are exact in both).
- Changes: 2026-10-29: added.

#### Extrude                                [BUILT-IN]   [OURS]
- What it does: pushes a flat profile up into a solid.
- UI: not wired yet. Planned: search "extrude", floating panel "Extrude": Distance, Direction (One side / Symmetric / Two sides), Result (Join / Cut / Intersect / New).
- Interface hook: `extrude(profile, height)`
- Heavy (exact surfaces): `HeavyEngine::extrude` in `src/engine/heavy/heavyengine.cpp` (`BRepPrimAPI_MakePrism`; straight-sided loops for now, as the interface hands it points)
- Lite (triangles): `LiteEngine::extrude` in `src/engine/lite/liteengine.cpp` (Manifold `Manifold::Extrude`, even-odd holes)
- Engine differences: Lite turns curved profile edges into straight segments.
- Changes: 2026-10-29: added.

#### Combine                                [BUILT-IN]   [OURS]
- What it does: joins, cuts or intersects two bodies.
- UI: not wired yet. Planned: the Result choice in other panels, and search "join", "cut", "intersect".
- Interface hook: `combine(a, b, op)`
- Heavy (exact surfaces): `HeavyEngine::combine` in `src/engine/heavy/heavyengine.cpp` (`BRepAlgoAPI_Fuse`, `_Cut`, `_Common`)
- Lite (triangles): `LiteEngine::combine` in `src/engine/lite/liteengine.cpp` (Manifold `+`, `-`, `^`)
- Engine differences: none known yet.
- Changes: 2026-10-29: added.

#### Move                                   [BUILT-IN]   [OURS]
- What it does: a moved copy of a body.
- UI: none yet.
- Interface hook: `translate(body, x, y, z)`
- Heavy (exact surfaces): `HeavyEngine::translate` in `src/engine/heavy/heavyengine.cpp` (`BRepBuilderAPI_Transform`)
- Lite (triangles): `LiteEngine::translate` in `src/engine/lite/liteengine.cpp`
- Engine differences: none.
- Changes: 2026-10-29: added.

### Display

#### Perspective with Ortho Faces (Sketch mode)  [BUILT-IN]   [OURS]
- What it does: Fusion's view style of that name, in Sketch mode only for now: while the view looks straight at the open sketch (within about 0.8°: as the sketch opens, and through any pan or zoom) it has no perspective, so zooming never seems to tilt the drawing; turned off square (the cube, orbiting) it's perspective again, and flat again once it faces the sketch. The flat view is exactly as big as the perspective one at the pivot, so the swap doesn't jump (under 2 px at the edges). Model mode stays perspective.
- UI: automatic in Sketch mode.
- Popup: none.
- Interface hook: QML `Main.sketchOrtho`, `orthoCamera` (an OrthographicCamera beside the PerspectiveCamera, magnification = view height / (2 · distance · tan(fov/2))); `Selection.ortho` makes `rayAt` cast straight rays from beside the camera (clicks, picking and snapping all go through it).
- Heavy / Lite: engine-independent
- Changes: 2026-10-30: added.

#### Import STEP                            [BUILT-IN]   [OURS]
- What it does: brings a STEP file (.step, .stp, any case) into an empty design as one solid body named after the file, at the file's own position and orientation, sized in mm whatever units the file uses. Faces split from one surface are merged, so a hole written as two half cylinders becomes one face with whole rims; the surfaces themselves don't change. It reads in the background: a panel in the middle of the window shows the file's name, the stage (Reading the file, Building the solid, Making the triangles to draw) and a running time, and holds the mouse off until it's done; then the view fits the model and a note gives the time, faces and triangles. Ctrl+Z takes it back as one step. The STEP file itself is packed into the saved design, so a saved design reopens without the original. A second import is refused while a body exists.
- UI: search "Import STEP" (Model mode; also "insert", "open", "stp"), then the file dialog. Also: a .step file named on the command line.
- Popup: the file dialog; the import panel.
- Interface hook: `importStep(path, why, stage)`; `Document::importFile` (runs on a `QThread`; `importing`, `importStage`, `importName`, `importFinished(why, seconds, faces, triangles)`; undo, save and load wait while importing); saved as `step_file` (qCompress, base64) with a history action `import_step`; `Main.importStep`, `objectName: "importPanel"`.
- Heavy (exact surfaces): `HeavyEngine::importStep` in `src/engine/heavy/heavyengine.cpp` (`STEPControl_Reader`, `ShapeUpgrade_UnifySameDomain`)
- Lite (triangles): MISSING (says so)
- Engine differences: Lite can't read STEP.
- Changes: 2026-10-30: designs now save the exact solid (`body_file`, OpenCascade binary, packed), so reopening takes milliseconds on the main thread instead of re-reading the file; `step_file` is still read from older designs. 2026-10-30: added. Not yet: a real percentage, cancel, several bodies, outlines left out where faces meet smoothly (a model of thousands of tiny flat faces draws thousands of outlines).

#### Import Mesh                            [BUILT-IN]   [OURS]
- What it does: brings an STL (binary or text) or OBJ into an empty design as a solid, never as a mesh: points are joined, triangles facing the wrong way are turned to agree with their neighbours, and joined triangles lying in one plane (within 1 degree of the area's first triangle and 0.02 mm of its plane, or a hundred-thousandth of the model's size) become one true flat face on their averaged plane. Corners and edges are shared directly, so no sewing step. The mesh's points are kept exactly: curved areas stay facets until Fit Curved Surfaces is asked for. A mesh with open edges stays an open shell and can't become a solid. Saved as the solid, as with STEP.
- UI: search "Import Mesh" (Model mode; also "stl", "obj", "insert"), then the file dialog. Also: a .stl or .obj named on the command line.
- Popup: the file dialog; the import panel.
- Interface hook: `importMesh(path, why, stage)`; `saveBody(body)` and `loadBody(bytes)` for the design file; `Document::importFile` picks by file ending; history action `import_mesh`.
- Heavy (exact surfaces): `HeavyEngine::importMesh` in `src/engine/heavy/heavyengine.cpp`; `meshsolid::read`, `meshsolid::solid` in `src/engine/heavy/meshsolid.cpp` (`RWStl::ReadFile`, own OBJ reader, `build`, `assemble`)
- Lite (triangles): MISSING (says so)
- Engine differences: Lite can't read mesh files.
- Changes: 2026-10-30: added. Not yet: 3MF, several bodies, closing gaps in open meshes.

#### Fit Curved Surfaces                     [BUILT-IN]   [OURS]
- What it does: asked for, never automatic. Finds facets following a cylinder, cone or sphere and replaces them with that true surface. Every source point stays within 1% of the surface's radius, capped at 0.25 mm (or 0.02% of the model's size, if larger); the worst actual distance is reported. Patches grow from a seed, stop at corners (a bend half again sharper than the local facet-to-facet bend, 40 degrees at most), and refit as they grow; the simplest surface that holds wins. The largest surfaces then reclaim what small ones took. Full rounds are cut in two to build and merged afterwards; part rounds keep the wrap-around line behind them. Anything that won't build as a valid face goes back to facets and the rest is rebuilt, so the result is always a valid solid or nothing changes. Helical thread flanks, torus fillets and free-form areas stay facets: only thread crests and roots that are true cylinders convert. A second fit is refused until the first is undone, because it would work from its own drawing triangles. Rebuilds the whole body from its triangles, so surfaces that were already true (from a STEP file) are refitted too, within the same tolerance.
- UI: search "Fit Curved Surfaces" (Model mode; also "facets", "smooth", "round", "cylinder"). No button.
- Popup: none; the import panel shows it working and a message reports surfaces, time, worst distance and triangles left as facets.
- Interface hook: `fitSurfaces(body, share, result, why, stage)`; `Document::fitSurfaces` (on a `QThread`), `fitFinished(why, seconds, cylinders, cones, spheres, trianglesLeft, worstMm)`; history action `fit_surfaces`; undo restores the kept body; `kFitShare` in `src/document/document.cpp`.
- Heavy (exact surfaces): `HeavyEngine::fitSurfaces`; `meshsolid::fit`, `fitRound`, `tidy`, `curved` in `src/engine/heavy/meshsolid.cpp`
- Lite (triangles): MISSING (says so)
- Engine differences: Heavy only.
- Changes: 2026-10-30: added. Past 20,000 faces the halves of full rounds aren't merged (`kMergeFaces`): the merge took over 20 minutes on 80,000 faces, so such a hole stays two faces with two half rims. Not yet: helical surfaces for threads, torus, free-form. Undo doesn't survive saving and reopening, as with the rest of history.

#### Mesh for drawing                       [BUILT-IN]   [OURS]
- What it does: turns a body into triangles with flat face normals, plus outline edges wherever neighbouring triangles bend more than 20°.
- UI: every model on screen.
- Interface hook: `mesh(body)`
- Heavy (exact surfaces): `HeavyEngine::mesh` in `src/engine/heavy/heavyengine.cpp` (`BRepMesh_IncrementalMesh`, Delabella mesher; within the model's size / 50000, kept to 0.01-0.5 mm, and 0.2 rad; smooth normals from each true surface)
- Lite (triangles): `LiteEngine::mesh` in `src/engine/lite/liteengine.cpp`
- Engine differences: Heavy will tessellate exact surfaces to a chosen accuracy; Lite already is triangles.
- Changes: 2026-10-29: added. Flat shading only for now (curved faces will need smooth normals).

#### Topology (faces, edges, points)       [BUILT-IN]   [OURS]
- What it does: tells which triangles make up each face, which outline segments make up each edge, and where the corner points are, so a user can hover and select a whole face, edge or point the way CAD users expect (a cube: 6 faces, 12 edges, 8 points).
- UI: hover anything on the model (pale highlight); click selects it (strong highlight); Ctrl- or Shift-click adds or removes; click empty space or Esc clears. Points win over edges, edges over faces.
- Popup: none.
- Interface hook: `mesh(body)`, fields `faceCount`, `triangleFace`, `edges` (polylines), `points` of `omamake::Mesh`.
- Highlight thickness: edges and points keep their on-screen width by their OWN distance from the camera (`Document::fillParts` eye, perMm; `Selection.redraw`, redrawn as the camera moves), not the pivot's: zoomed in on a big model the pivot can be far behind them, which drew a fat tube bulging out of a hole's rim. Tubes (`addTube`) wind outward; they were inside out, so culling hid their near side and a thin highlight half sunk in a surface showed only its joint balls, as dots.
- Heavy (exact surfaces): `HeavyEngine::mesh` in `src/engine/heavy/heavyengine.cpp`: the shape's real faces, edges and corners. A cylinder's seam is no edge; a lone circle's start is no point.
- Lite (triangles): `LiteEngine::mesh` in `src/engine/lite/liteengine.cpp`. A mesh has no true faces, so they are rebuilt: triangles are joined across every edge that bends less than 20° (the same rule as outline edges), so a cylinder's facets become one face. Edges are chains of outline segments between points; closed chains (a circle) have no points. Points are vertices where 1 or 3+ outline segments meet.
- Engine differences: Lite's faces depend on the 20° rule: a very gentle crease between two flat areas joins them into one face, and a very coarse curve could split into several. Heavy will not have this limit.
- Changes: 2026-10-29: added.

### Sketch

Sketches don't go through either engine: what's drawn in a sketch and its constraint solver
live in the document (`SketchGeometry` in `src/document/sketchgeometry.cpp`, reached through
`Document` in `src/document/document.cpp`), so both engines behave the same. Points are
stored in the sketch's own flat coordinates (u, v in mm); lines that meet share their point.
Every action below is one step in that sketch's own history (its own row in the Timeline) and
is undone with Ctrl+Z.

#### Create Sketch                          [BUILT-IN]   [OURS]
- What it does: starts a new sketch on an origin plane, a flat face of a body, or empty space, and goes into Sketch mode: the view turns square onto the sketch, the origin planes hide (the origin point and axes stay, to draw from and constrain to), and the search only offers sketch tools. The new sketch goes into the active component ("Sketch1", numbered per component). One step in the design's history ("Sketch1 on the XY plane"); Ctrl+Z on it takes the sketch away again.
- UI: triple-click a plane, a flat face or empty space; or search "Create Sketch" (Model mode), then click one. Clicking the origin point or an axis says to pick a flat face, a plane or empty space instead. The Browser's eye on a hidden origin plane shows it again for this sketch only.
- Popup: none.
- Interface hook: `Document::addSketch(plane)`; QML `qml/Sketch.qml` (`startAt`, `start`, `open`)
- Heavy / Lite: engine-independent
- Changes: 2026-10-29: added.

#### Finish Sketch                          [BUILT-IN]   [OURS]
- What it does: leaves Sketch mode; the view flies back to where it was before the sketch was opened. One history step; Ctrl+Z on it goes back into the sketch.
- UI: the "Finish Sketch" button in the bar at the top centre while sketching; search "Finish Sketch"; or Esc, once nothing else is waiting for Esc (Esc stops, in order: Create Sketch's plane picking, pan mode, a drawing or constraint tool, picked constraint badges, then the sketch).
- Popup: none.
- Interface hook: `Document::finishSketch(id)`; `Sketch.finish`
- Heavy / Lite: engine-independent
- Changes: 2026-10-30: picked constraint badges are let go before Esc finishes the sketch. 2026-10-29: Esc finishes. 2026-10-29: added.

#### Sketch bar, tool name and guide        [BUILT-IN]   [OURS]
- What it does: the sketch bar along the top holds SKETCH, its small buttons (Look at Sketch, Toggle Construction, Find Profile Gaps, room for more) and Finish Sketch; the tool in use is no longer squeezed into it. Under it, in fixed places that never move: the tool's whole name in its own small pill, which slides down and fades in when a tool starts and goes again when it stops (Esc, Finish Sketch); below that the guide: the tool's own instruction for as long as it's on, a step's note for a moment when there is one (then back to the instruction), and notes after something is done. Stopping a tool takes its guide with it, unless the note was what the tool just made. Both pills keep their words while fading out. A tool's options row sits below them. Fusion shows the tool's name and prompts in the tool's own dialog and beside the mouse instead.
- UI: top centre, in a sketch.
- Popup: none.
- Interface hook: `qml/SketchBar.qml` (toolName, toolHint, guide, toolPill, guidePill); Main.qml passes the search command's hint as toolHint; `Sketch.messageAt`.
- Heavy / Lite: engine-independent
- Changes: 2026-10-30: tool name moved to its own pill; steady guide per tool.

#### Profile finder                         [BUILT-IN]   [OURS]
- What it does: works out which areas a sketch's lines, arcs, circles and curves close off, as Fusion's profiles: two overlapping circles make three profiles; a circle inside a rectangle makes the disc and the ring round it (an island is a hole in the area round it and its own profile too); text letters make one profile each, with their holes. Lines are split wherever they cross or one ends on another; ends closer than 0.0001 mm count as joined; loose ends and lines leading nowhere are left out. Construction lines and hidden helpers (a polygon's circle, a slot's centre line) are never part of a profile. It also finds the gaps that keep a profile from closing (see Find Profile Gaps). Each profile can be cut into triangles (its holes left out) for drawing. Picking profiles and Extrude come later.
- UI: none of its own; Profile shading and Find Profile Gaps use it.
- Popup: none.
- Interface hook: `profiles::find(paths, gapMax, touching, groups)` in `src/document/profiles.h` (gapMax 0: no gap search), `profiles::triangles(profile)` (Manifold's `TriangulateIdx`, the Lite engine's library: a Heavy-only build would need its own), `Document::profilePaths` (plain point chains in the sketch's flat coordinates, knows nothing of the engine or solver); `Document::sketchProfiles(sketch, gapMax)` returns `{profiles: [{outer, holes, area}], gaps: [{a, b, size}]}` (a and b also carry `at`, the 3D point).
- Heavy / Lite: engine-independent
- Changes: 2026-10-30: triangles for the shading. 2026-10-30: added. Arcs and curves are followed as short straight pieces (arcPath/curvePath), so areas are a hair under the true ones (a 10 mm circle: 314 mm² either way at this precision). 30 letters of text: 35 profiles in 3 ms.

#### Profile shading                        [BUILT-IN]   [OURS]
- What it does: every closed profile of every shown sketch is filled with a light see-through wash, so a closed shape can be told from an open one at a glance: the area between two overlapping circles, a ring round a circle, each letter of text with its holes left clear. It changes as you draw, drag and delete (about 2 ms for a dozen profiles with text), and only then: never on a pan or zoom. It shows in Sketch mode and in Model mode alike, as in Fusion, and goes with the sketch's eye. Lines, points and the model show through it; a body in front hides it. Fusion shades closed profiles pale orange; here the wash is the sketch-line colour (white in dark themes, dark in light ones), because the origin planes are already orange and the two looked the same.
- UI: in the view, on the sketch's plane.
- Popup: none.
- Interface hook: `Document::fillProfiles(target, sketches)`; Main.qml `drawProfiles()` (on `sketchesChanged` and the eyes), Model `objectName: "profileShading"` (blended, both sides, no depth writes, depthBias -20 so lines stay on top); `Theme.sketchProfile`.
- Heavy / Lite: engine-independent (triangles from Manifold's triangulator, see Profile finder)
- Changes: 2026-10-30: added. Not yet: a different tint for the open sketch. Fusion's Show Profile switch is left out on purpose (owner: no use for it). Hovering and picking: see Selecting profiles. 2026-11-01: the wash could lag the sketch by one change (Toggle Construction left it shaded, then bare; a first Project left it bare), while picking was right. QML handlers run before C++ connections to a signal, so `drawProfiles()` read the old `m_profiles`; every sketch change now goes through `Document::sketchesEdited()`, which clears them before `sketchesChanged`.

#### Selecting profiles                     [BUILT-IN]   [OURS]
- What it does: resting the mouse inside a closed area lights just that area (the hover colour), and a click selects it (the selection colour); Ctrl-click or Shift-click adds it to or takes it from the selection, as with lines and faces. It is what Extrude will work on. Lines, points and dimensions come first: near a line the line is picked, near a point (a small circle's centre too) the point; inside an area away from them, the area. A body in front hides the profiles behind it; a profile drawn on a body's face wins over that face. In Sketch mode only the open sketch's profiles can be picked (only its lines can be, too); in Model mode every shown sketch's can. A selected profile stays selected while the sketch changes as long as there is still a profile at the spot it was picked (an area split in two keeps the half that was clicked); it goes when the area is opened up, or when its sketch is hidden. Dragging inside an area moves nothing. Esc in a sketch still finishes the sketch, as before. Fusion's tints are its own blue; here they are the theme's hover and selection colours.
- UI: in the view, on the sketch's plane.
- Popup: none.
- Interface hook: `Document::pickProfile(sketches, origin, direction)` (nearest along the ray, gives `{kind: "profile", sketch, index, u, v, depth}`), `Document::profileAt(sketch, u, v)` (find it again after a change), `Document::fillProfileParts(target, parts)`; `profiles::contains`; `Selection.profileSketches` (set by Main.qml `drawProfiles()`), `Selection.partAt` (profile after lines and points, before the origin planes), `keepIn`; Models `objectName: "profileHover"` / `"profileSelected"` (depthBias -25). Profiles are found once per sketch change and kept (`Document::profilesOf`).
- Heavy / Lite: engine-independent
- Changes: 2026-10-30: added. Not yet: Extrude, a box drag (window select) taking profiles, a profile's area in the info line.

#### Find Profile Gaps                      [BUILT-IN]   [OURS]
- What it does: one click rings in red, for 10 seconds, every place two lines, arcs, circles or curves come within 1 mm of each other without being joined: the breaks that keep the profile you are drawing from closing. Any two count: a line end short of a corner or of a circle, two circles 0.3 mm apart, two parallel lines, an arc short of a line. Where two lines run close for a while (two arcs side by side) that's one ring, at the narrowest place. A loose line end near another line is always one, even where its line meets that one further off (a V whose tips nearly touch); two parallel lines get a ring at each open end. Not counted: lines that cross, touch or are joined right there (tangent circles, a line tangent to a circle, a line crossing a curve at a shallow angle, two circles with a short line between them, including the close stretch either side of where they meet; so a thin closed slot isn't ringed), a curve's own next stretch, the far side of a tiny circle, one piece of text's own letters, and anything 1 mm or more apart. It guesses nothing about what you meant to draw. The rings follow the view (never smaller than 28 px, bigger when zoomed in close enough to see the gap) and keep up with drawing for the 10 s, a ring going as soon as its gap is closed, with a note of how many closed profiles there are. While gaps are ringed, the guide's place under the sketch bar holds the gap stepper: ‹ [ 5 ] › "gaps under 1 mm, not joined · 3 closed profiles". The box only shows the count (it isn't typed in); the arrows fly the view, keeping its direction, to each gap in turn (left to right across the sketch, round and round), zoomed so the gap shows clearly inside its ring, which is drawn thicker; the box then reads "2 of 5". Once stepping, the 10 s stops: the rings stay until Esc (or until every gap is closed, then 10 s as usual). The stepper never changes size. With no gaps, the guide says "No gaps under 1 mm · 3 closed profiles". Esc takes the rings away at once (that Esc does only that); clicking again starts the 10 s again (not while stepping); leaving the sketch ends it. It is a momentary check, not a switch (owner). Fusion has no such tool (its only hint is a profile that doesn't shade).
- UI: the round button right of Toggle Construction in the sketch bar (two line ends with a red ring round the gap, ringed red while the rings show; hover tip after 2 s), search "Find Profile Gaps" (Sketch mode, also "gap", "break", "open", "leak"), or Alt+G.
- Popup: none; a note says how many gaps there are and how many profiles are closed.
- Interface hook: `Main.showGaps` / `hideGaps` (`gapsTimer`, 10 s), `Main.stepGap(by)` / `gapAt` (flies with `Navigation.restoreView`, radius max(3 mm, 10 × the gap)), `SketchBar.gapCount` / `gapAt` / `gapNote` / `stepGap`, stepper `objectName: "gapStepper"` (arrows "gapBack", "gapNext", box text "gapCountText"), `Main.gapsOn`, `Main.profileCheck` (recomputed on `sketchTick`, bumped by `Document.sketchesChanged`), `Main.gapLimit` (1 mm); rings `objectName: "gapRing"`, fading out over 0.3 s; Esc in `escKey`; `SketchBar.gaps` / `findGaps`, button `objectName: "findGaps"`; key in `qml/SketchKeys.js`. The rule is in `profiles::find` step 3: a pair of pieces 0.0001 to 1 mm apart whose way round along the lines is over twice the gap, gathered into runs along the two paths; runs with a crossing, touch or join in them are dropped; then each loose end is checked on its own against every line not joined to it there. Gaps come out left to right. Curves are followed as straight pieces, so each piece gets slack for how far the true curve bows from it (1.5·L·θ/8); text pieces share a group and are never a gap to each other.
- Heavy / Lite: engine-independent
- Changes: 2026-10-30: gap stepper with arrows (stepping stops the 10 s); joined-nearby stretches dropped, loose ends checked on their own (V tips), gaps sorted left to right. 2026-10-30: one click, 10 s, Esc (was a switch); any two lines, not only loose ends (two circles 0.3 mm apart weren't found); touching curves and shallow crossings no longer ringed; text's own letters skipped. 2026-10-30: added.

#### Look at Sketch                         [BUILT-IN]   [OURS]
- What it does: turns the view square onto the open sketch again and centres it, after turning, panning or zooming away. A sketch on an origin plane gets the whole design fitted, as when it was opened; a sketch on a body's face gets that face centred and fitted. Changes only the view, so it is no step in the history. Fusion has no such button; its nearest are Look At and the ViewCube's Home.
- UI: the small round button just right of "SKETCH" in the bar at the top centre while sketching (a square with a dotted cross). Resting the mouse on it for 2 s shows what it does (see Hover tips).
- Popup: none.
- Interface hook: `Sketch.lookAtSketch`; `Navigation.lookAlong(dir, around)` and `Navigation.fitTo(center, radius)`; the face's centre and size come from `Document` facePlane ("center", "radius"). Sketches saved before 2026-10-30 have no face size, so they fit the whole design instead.
- Heavy / Lite: engine-independent
- Changes: 2026-10-30: hover tip. 2026-10-30: added.

#### Toggle Construction                    [BUILT-IN]   [OURS]
- What it does: construction lines, arcs, circles and curves are for reference only. They are drawn dashed in the origin planes' colour (dash, gap, dot, gap, the same on screen at any zoom), toned down once fully constrained like other lines. They can be picked, snapped to, constrained, dimensioned and used by every tool like any other line, but are never part of a profile. Two ways to make them, both Fusion's: (1) select lines, arcs, circles or curves and use the command: all of them become construction (a mixed selection too: nobody wants the two kinds swapped), or all normal again when they already all were; one history step ("Made Line6 construction"; "Made 3 pieces normal"); (2) the Construction switch: with nothing selected, or while a tool is on (it stays on), the command flips it, and everything drawn from then on is construction, from any tool, until it's flipped back or the sketch is closed. The bar shows "Construction" while it's on and a tool's preview is drawn dashed in the same colour. Copies follow their originals whatever the switch says: Trim's and Break's pieces, Offset, Mirror, the Circular and Rectangular Patterns, Move/Copy's copies. Double-clicking a line picks the lines joined to it of its own kind only (normal or construction). Saved in the .omamake file ("construction" on each line, arc and curve).
- UI: the round button right of Look at Sketch in the sketch bar (a dash-dot diagonal in the construction colour, ringed in it while the switch is on; hover tip after 2 s), search "Toggle Construction" (Sketch mode), or X (Fusion's key). Fusion calls it Normal/Construction.
- Popup: none; a short note says when the switch flips.
- Interface hook: `Document::toggleSketchConstruction(sketch, parts)`; `construction` on `addSketchLine`, `addSketchRectangle` and `addSketchShape`'s spec, plus the spec's `like` (copies); `SketchGeometry::constructionLines / constructionArcs / constructionCurves`; drawn by `Document::fillSketches` into its two construction targets (`addDashed`); `SketchTools.construction`; `Main.toggleConstruction`; colours `Theme.sketchConstruction` / `sketchConstructionFull`.
- Heavy / Lite: engine-independent
- Projected lines: construction keeps them violet, in the same dash-dot (`sketchProjectedDashed`, the `projectedDashed` target of `fillSketches`). Owner's choice of colour.
- Changes: 2026-10-30: renamed from Normal/Construction; sketch bar button. 2026-10-30: added. Not yet: Extrude (the profile finder already skips construction), a selected or hovered construction line is highlighted as a solid line, the polygon's circle and other helpers stay hidden rather than shown as construction as Fusion does.

#### Selecting in a sketch                  [BUILT-IN]   [OURS]
- What it does: the open sketch's own points and lines are found before anything else under the mouse, then the origin, then the model. A point counts from 11 px away, a line from 7 px; points win over lines.
- UI: hovering lights a point or line; click selects it; Ctrl- or Shift-click adds or removes; click empty space to clear. Double-click a line, arc, curve or point selects its whole chain: everything joined end to end through a shared point, a Coincident, or a point held on it (Point On, Midpoint), normal and construction each only along their own kind. Lines that only cross are not joined, though they can still close a profile. Circles and ellipses have no ends, so they select alone unless something is held on them. In Fusion a double-click selects a loop and stops at branches; here a branch is followed. Drag from empty space for a box (Fusion's window select): left to right, a solid box takes what is wholly inside; right to left, a dashed box takes anything it touches; Ctrl or Shift adds; Esc drops it. The end points of what is taken stay out of the selection (Fusion takes them too), so two boxed lines still count as two lines for Parallel and Equal; a point on its own is taken. Fusion also boxes profile areas; not here yet. While a drawing tool is on, the mouse snaps instead (see Snapping while drawing) and nothing is selected.
- Popup: none.
- Interface hook: `Document::pickSketch(sketch, origin, direction, pointReach, edgeReach)`; QML `Selection.partAt` / `clickAt` (`sketchPointPixels` 11, `edgePixels` 7); parts `{kind: "sketch_point" | "sketch_line", sketch, index}`
- Heavy / Lite: engine-independent
- Changes: 2026-11-01: box select added (`SketchTools.selectBox`, `Document::sketchOutlines(sketch)`). 2026-11-01: double-click extended from lines to arcs, curves and points, and through Coincident, Point On and Midpoint (`SketchTools.profileOf(part)`). 2026-10-30: point reach 9 → 11 px, about a fifth easier to land on. 2026-10-29: added.

#### Show Points, Show Dimensions, Show Constraints, Show Projected Geometries  [BUILT-IN]   [OURS]
- What it does: Fusion's Sketch Palette switches. Show Points and Show Projected Geometries leave what's off neither drawn nor picked (nor boxed), so a click near a hidden point lands on its line. Show Dimensions hides the values; Show Constraints hides the badges. All on by default, kept for the run, the same for every sketch.
- UI: the Display button in the bottom right corner while sketching opens a list of tick boxes above it; the list closes 3 s after the mouse leaves it (`qml/SketchDisplay.qml`). Named Display, not Fusion's Sketch Palette: palette reads as colours (owner). Also search rows in Sketch mode.
- Popup: none.
- Interface hook: Main.qml `showPoints`, `showDimensions`, `showConstraints`, `showProjected`, `sketchShows()`; `Document::setSketchShows(points, projected)`, `lineHidden`, `pointHidden`.
- Heavy / Lite: engine-independent
- Fusion divergences: Fusion's palette has more rows (Sketch Grid, Snap, Slice, Show Profile, 3D Sketch, Look At); ours has these four. Not saved with the design. Hidden projected lines still close profiles and still snap; hidden points still snap. Show Profile left out on purpose (owner).
- Changes: 2026-11-01: added.

#### Box select: what it leaves out
- End points of taken pieces: a boxed line, arc or curve does not select its own end points, and an arc or circle does not select its centre. Fusion selects them too. Why: Parallel, Equal and the like read the selection, so two boxed lines must stay two lines. Candidate option: a switch to take points as well.
- Spline and conic points: same rule, the points of a taken curve stay out.
- Profile areas: a box takes only lines, arcs, curves and points, never a closed area. Fusion's can take them. Needed once Extrude exists, so a box can pick several profiles.
- Constraint badges and dimensions: a box never takes them. They are picked by clicking. Candidate option: box-pick badges to delete many at once.
- Lone points: taken, including a point that belongs to something not taken (a crossing box catching one end of a line takes the line instead, a window box round one end takes that end point alone).
- Model and origin: a box in a sketch takes nothing of the model, the origin or its axes, and there is no box in Model mode at all.
- Hidden pieces: helper lines and arcs, rim points and hidden centres are never taken.

#### Snapping while drawing                 [BUILT-IN]   [OURS]
- What it does: while Line or 2-Point Rectangle waits for a click, the mouse snaps (within 12 px, in this order) onto a point already drawn (the new line shares that point), the Origin (the new point is held there by Coincident), the middle of a line (held there by Midpoint, without using the Midpoint tool), or anywhere along a line or an axis (held on it by Coincident, free to slide along it until a dimension places it). The first and the second click snap alike. A Line within 2° of level or upright snaps exactly level / upright and is held Horizontal / Vertical; it doesn't do this when its end has snapped onto something. The constraints are made with the line or rectangle, in the same History step, so Ctrl+Z takes them away together. Everything is placed exactly where it snapped, so nothing else moves.
- UI: a point or the Origin it snapped to lights up; a line's middle shows a small filled triangle and a spot along a line a small ring, just above and right of the spot (the line itself isn't lit, so it doesn't look selected). The preview ends there, and a small tag beside the mouse names it: "Point4", "Origin", "Midpoint of Line2", "On Line6", "On the X axis", "Horizontal", "Vertical". Hovering a point already held at a line's middle shows the triangle, and one held on a line the ring, with or without a tool on.
- Popup: none.
- Interface hook: `SketchTools.snapAt` / `aim` / `showSnap` / `snapLabel` / `glyph` / `heldMark` / `levelDegrees` (2); a new point's {u, v, on: "origin" | "midpoint" | "line", line}; `Document::addSketchLine(sketch, from, to, hold)`, `Document::holdSnap`; the mark is a Canvas in Main.qml (`snapMark`).
- Heavy / Lite: engine-independent
- Projected geometry snaps like any other (its points and middles). Planned: Circle and the other shapes use the same snapping as they're added.
- Changes: 2026-10-30: dragging existing points and lines snaps the same way (see Drag in a sketch). 2026-10-30: reach 10 → 12 px. 2026-10-30: anywhere along a line or axis; triangle / ring marks instead of lighting the whole line. 2026-10-30: added.

#### Constraint badges                      [BUILT-IN]   [OURS]
- What it does: shows which constraints hold what's selected in a sketch, so it's clear why something won't move the way you expect. Clicking a badge picks that constraint for deleting (see Delete constraints). A selected point shows its own constraints; a selected line shows its own and its end points'. Each constraint also shows on the other things it ties (select one of two parallel lines and both show Parallel). Midpoint and Coincident-on-a-line show on the point only. Dimensions aren't badged; they show their own values.
- UI: a row of small square tiles with a sign in the accent colour: a point's just below and right of it, a line's beside its middle on the side that's up on screen. Signs: Horizontal —, Vertical |, Parallel //, Perpendicular ⊥, Equal =, Collinear —•—, Coincident ●, on a line ○, Midpoint ▲, Symmetry •|•, Fix (a padlock). Hovering a tile names the constraint and everything it ties, e.g. "Parallel: Line1, Line6", "Coincident: Point2, the Origin". They follow the view and the drawing as it moves, and go away when nothing is selected. A picked badge is filled in the accent colour with its sign in the panel colour; badges of picked constraints stay up even when what they're on isn't selected.
- Popup: none.
- Interface hook: `SketchTools.badges` ([{at: "point" | "line", id, u, v | a, b, badges: [{id, kind, label, picked}]}]), `constraintNames`, `constraintKinds`; drawn by `qml/ConstraintBadge.qml` (signal `clicked`) from a Repeater in Main.qml.
- Heavy / Lite: engine-independent
- Planned: Tangent, Concentric and Smooth with arcs and circles.
- Changes: 2026-10-30: a click picks the constraint. 2026-10-30: added.

#### Delete (sketch)                         [BUILT-IN]   [OURS]
- What it does: Delete or Backspace removes what's selected in a sketch: lines, arcs, circles, curves and points, normal, construction or projected alike. A selected point takes what's drawn from it along. Points nothing else uses go with their lines; shared corners stay. Constraints and dimensions on what's gone go too. Selected profile areas stay. Picked constraint badges come first: while any are picked, only they go, as in Delete constraints. One history step; Ctrl+Z puts it all back.
- UI: select, then press Delete or Backspace, in Sketch mode with no tool on.
- Popup: none.
- Interface hook: `SketchTools.deleteSelected()`, the Delete shortcut in Main.qml, the `remove` part of `Document::addSketchShape`.
- Heavy / Lite: engine-independent
- Helpers: a helper piece or point (a polygon's circle, a chamfer's lost corner) goes when a delete leaves it tied to nothing seen, or when its centre or rim point is deleted. Sweep in `Document::addSketchShape`, only for helpers that were tied before.
- Changes: 2026-11-01: helpers tied to the delete; dimension labels can be picked. 2026-11-01: added.

#### Delete constraints                     [BUILT-IN]   [OURS]
- What it does: takes constraints off a sketch without touching its lines and points, so a design can be changed without redrawing it. Nothing moves when a constraint goes; the shape is just free to move that way again (and stops being grey if it was fully constrained). Every picked constraint goes in one history step ("Deleted Parallel", "Deleted 3 constraints"); Ctrl+Z puts them all back exactly.
- UI: select a point or line so its badges show (see Constraint badges), then click a badge to pick it; click again to let it go. No keys needed to pick more: select other lines or points and click their badges too, and the ones already picked stay picked and visible. Delete (or Backspace) deletes the picked constraints only, even with lines selected. Clicking empty space or Esc lets every pick go. The bar at the top says what's picked ("Parallel picked: Delete takes it off, the lines stay"), and a badge's hover label ends "click to pick, then Delete".
- Popup: none.
- Interface hook: `Document::deleteSketchConstraints(sketch, ids, summary)` (history type "delete_constraints", each removed constraint saved whole for Ctrl+Z); QML `SketchTools.pickedConstraints`, `pickConstraint`, `deleteConstraints`; the Delete shortcut in Main.qml.
- Heavy / Lite: engine-independent
- Dimensions: click a value to pick it (it fills in the accent colour), Delete takes it off like a badge; double-click still edits, a drag still moves it.
- Changes: 2026-10-30: added.

#### Line                                   [BUILT-IN]   [OURS]
- What it does: straight lines, click to click; each click ends one line and starts the next from there; clicking the chain's first point closes it; Esc stops. Snaps (see Snapping while drawing).
- UI: key L (Fusion's own shortcut; only in a sketch, not while a box is typed in; stops any other tool), or the search: "Line"
- Popup: none.
- Interface hook: `Document::addSketchLine(sketch, from, to, hold)`; tool in `qml/SketchTools.qml`; key: `keys: "L"` on the Line command (Main.qml `oneKeyTools`)
- Heavy (exact surfaces): engine-independent
- Lite (triangles): engine-independent
- Engine differences: none.
- Changes: 2026-10-30: snaps to the Origin and line middles, and level / upright within 2°. 2026-10-29: added.

#### 2-Point Rectangle                      [BUILT-IN]   [OURS]
- What it does: four lines between two clicked opposite corners, drawn square to the sketch's axes but held only as a rectangle: one Perpendicular corner and its opposite sides Parallel, never Horizontal or Vertical. So it can be turned to any angle by dragging it (a corner drag turns as well as resizes); a Horizontal or Vertical on one side, or an angle dimension, holds its turn. Its two clicked corners snap like Line's ends.
- UI: search-only (Sketch mode): "2-Point Rectangle"
- Popup: none.
- Interface hook: `Document::addSketchRectangle(sketch, corner, opposite)`
- Heavy (exact surfaces): engine-independent
- Lite (triangles): engine-independent
- Engine differences: none.
- Changes: 2026-10-30: held by Perpendicular + Parallel again (no Horizontal/Vertical) so it can be turned. 2026-10-29: added (first held by Parallel + Perpendicular; changed the same day to Horizontal/Vertical because a corner drag turned the whole rectangle).

How every constraint tool is picked (`SketchTools.ruleFor` says what each list of picks makes): from the search, whatever is already selected counts as the first picks, in the order it was selected. If that is all the constraint needs, it is done at once and the tool ends (as in Fusion). Otherwise the tool starts and waits for clicks; a click that doesn't fit is refused with what to click instead, and the picks so far are kept. After a constraint is made the tool stays on for the next one until Esc. The first thing picked stays put where it can; the rest moves to meet it. Every constraint is refused with a reason (nothing moves) when it already holds, or can't hold with the constraints already there. No keyboard shortcuts yet. Saved in the .omamake file's sketch "constraints" list with "type", "first", "second" and, for Symmetry, "third" (the mirror line).

#### Center Diameter Circle                 [BUILT-IN]   [OURS]
- What it does: click the centre, then any spot on the rim (that sets the size). Both clicks snap like Line's. The circle keeps one hidden point on its rim, held due right of the centre, so its size is one number the solver can change.
- UI: search-only (Sketch mode): "Center Diameter Circle" (found by "c" or "circle"). While drawing, a preview circle follows the mouse.
- Popup: none.
- Interface hook: `Document::addSketchShape(sketch, spec)` with one arc whose two ends are the same point; tool "circle" in `qml/SketchTools.qml`
- Heavy / Lite: engine-independent
- Changes: 2026-10-30: added.

#### Center Point Arc                       [BUILT-IN]   [OURS]
- What it does: click the centre, then where the arc starts (that sets the radius), then where it ends. It goes round the way the mouse went (either direction, up to nearly a full turn); the end lands on the radius wherever the mouse points. The centre and start snap like Line's.
- UI: search-only (Sketch mode): "Center Point Arc" (found by "arc"). Preview: the radius line, then the arc following the mouse.
- Popup: none.
- Interface hook: `Document::addSketchShape`; tool "arc" in `qml/SketchTools.qml` (`sweep` adds up the mouse's turning)
- Heavy / Lite: engine-independent
- Changes: 2026-10-30: added.

#### Arcs and circles                       [BUILT-IN]   [OURS]
- What it does: the curve every round shape is built from. An arc is a centre and two end points, counter-clockwise from the first to the second; a circle is an arc whose ends are one point. The solver keeps both ends the same distance from the centre. Picked and highlighted like lines; a lone circle dragged by its edge changes size (centre stays), an arc or a selection with arcs moves whole. Saved in the file's sketch "arcs" list (id, centre, a, b, circle, radius_mm, fully_constrained); a circle's rim point is marked "circle_rim". Tangent, Concentric, Equal (radius), Coincident (a point on the curve) and Sketch Dimension (an arc's radius, a circle's diameter) take arcs and circles; drawing and dragging snap anywhere along them ("On Arc3", held by Coincident on a curve).
- Interface hook: `SketchGeometry::Arc`, `SketchGeometry::arcs`, `isRim`, `radius`; `Document::addSketchShape(sketch, {points, lines, arcs, constraints, summary})` (one history step, undone as one); selection kind `sketch_arc`
- Heavy / Lite: engine-independent
- Changes: 2026-10-30: constraints, dimensions and snapping on arcs. 2026-10-30: added.

#### Sketch tool engine and options box     [BUILT-IN]   [OURS]
- What it does: runs every tool in the table: its clicks in turn (a snapped point, a pick of a line/arc, the end of a line or arc, a corner, several points, or several things), the preview, then one history step built from the tool's spec (new points/lines/arcs/curves, ends rewired, points moved or removed, constraints, dimension values). A failed step says why and goes back one click to try again. What's selected when a tool starts counts (things to Mirror, Offset…; corners for Fillet/Chamfer).
- UI: Enter finishes a gathering step (Mirror's things, a spline's points) or makes an Offset / Rectangular Pattern. The options box sits under the sketch bar in the dimension box's look, one field per option (lengths in inches unless a unit is typed, angles in degrees, whole numbers, numbers, text, tick boxes); its first field opens selected; a value that doesn't read turns red and the last good one stays; Enter there carries on, Esc stops the tool, Ctrl+Z leaves it and undoes. Values are kept per tool while OmaMake runs.
- Popup: the options box.
- Interface hook: `qml/SketchTools.qml` (`useShape`, `shapeClick`, `shapeMove`, `confirm`, `finishShape`, `shapePreview`, `handleBegin`/`handleMove` for Offset's arrow), `qml/ToolOptions.qml`, `qml/SketchMath.js` (plane geometry), `Document::addSketchShape` (spec: points, lines, arcs, curves, constraints, rewire, move, remove, helpers, values, steady, summary)
- Heavy / Lite: engine-independent
- Changes: 2026-10-30: added.

#### Curves (splines, conic, ellipse, text) [BUILT-IN]   [OURS]
- What it does: drawn things defined by their points: Fit Point Spline, Control Point Spline, Conic Curve (with rho), Ellipse (centre, axis end, width point) and Text (anchor, words, height). Picked, highlighted and dragged (their points move) like lines. Saved in the file's sketch "curves" list (type, points, rho / text + height_mm).
- Interface hook: `SketchGeometry::Curve`, `curves`, `curvePath` (polylines for drawing and picking), `textOutline`; selection kind `sketch_curve`; `Document::sketchTextOutline`
- Heavy / Lite: engine-independent
- Changes: 2026-10-30: added.

#### Helper geometry                        [BUILT-IN]   [OURS]
- What it does: points, lines and arcs a tool needs to hold its shape but that aren't part of the drawing (a polygon's circle, an arc slot's centre line, a chamfer's lost corner, a blend's legs). Solved like the rest but never drawn, picked or snapped to. Saved as "helper" flags.
- Interface hook: `SketchGeometry::helperPoints / helperLines / helperArcs`, `isHidden`
- Heavy / Lite: engine-independent
- Changes: 2026-10-30: added.

#### Tangent                                [BUILT-IN]   [OURS]
- What it does: a line and an arc/circle, or two arcs/circles, touch smoothly. For two curves it keeps whichever touch is nearer now (outside or inside). The first picked stays; the second circle or arc slides over whole, keeping its size, straight towards (or away from) the first until they touch, so it stays at the angle it was at (with a line first, it slides square to the line). Not when one of its points is Fixed; then the solve finds a way. Afterwards a tangent circle dragged by its centre (or a selected arc dragged whole) goes round the other, touching, both keeping their sizes; nothing places it round the other until you add more (a line between the centres with an angle, distances to the Origin).
- UI: search-only (Sketch mode): "Tangent". Click (or select) a line, arc or circle, then an arc or circle. Badge: a ring on a line.
- Popup: none.
- Interface hook: constraint types `tangent` (LA) and `tangentarcs` (AA, u = 1 outside / 0 inside) in `SketchGeometry`; `SketchGeometry::touch` (the slide); `Document::addSketchConstraint`; `SketchGeometry::drag`
- Heavy / Lite: engine-independent
- Changes: 2026-10-30: slides the circle whole instead of growing it; dragging goes round. 2026-10-30: added.

#### Concentric                             [BUILT-IN]   [OURS]
- What it does: two arcs or circles share a centre.
- UI: search-only (Sketch mode): "Concentric". Click two arcs or circles. Badge: two rings.
- Popup: none.
- Interface hook: constraint type `concentric` (AA)
- Heavy / Lite: engine-independent
- Changes: 2026-10-30: added.

#### Constraints the tools make             [BUILT-IN]   [OURS]
- What it does: kept by the solver like the rest and shown as badges: Coincident on a curve (`pointonarc`, a point on an arc or circle; also from Coincident and from snapping onto an arc), Equal radius (`equalradius`, from Equal on two arcs), three-point Midpoint (`midpoints`), Circular Pattern (`rotation`: a point is another turned about a centre by a set angle), Rectangular Pattern (`translation`: a point is another moved by a set step), Offset (`offset` between lines, `offsetarcs` between arcs), radius / diameter / angle dimensions. A dimension can follow another (`third` = its leader): Fillet's arcs, Offset's pieces and Equal Distance Chamfer's second side follow one value.
- Interface hook: `SketchGeometry` residuals, `kinds()`, `Constraint::follows()`, `valueOf`
- Heavy / Lite: engine-independent
- Changes: 2026-10-30: added.

#### Midpoint Line                          [BUILT-IN]   [OURS]
- What it does: click the middle, then one end; the other end mirrors it. Held by Midpoint (the first click at the line's middle).
- UI: search-only (Sketch mode): "Midpoint Line" ("line middle"). Each click snaps like Line's; a preview follows the mouse; the prompt under the sketch bar says what to click next. Stays on for the next one until Esc; Ctrl+Z takes back each one whole.
- Popup: none.
- Interface hook: `qml/ShapeTools.js` (the tool table), run by the "shape" engine in `qml/SketchTools.qml`; one `Document::addSketchShape` step; key in `Shapes.order`
- Heavy / Lite: engine-independent
- Changes: 2026-10-30: added.

#### 3-Point Rectangle                      [BUILT-IN]   [OURS]
- What it does: click two corners for one side (any angle), then how wide. Held by Perpendicular and two Parallels, so it stays a rectangle at any angle.
- UI: search-only (Sketch mode): "3-Point Rectangle". Each click snaps like Line's; a preview follows the mouse; the prompt under the sketch bar says what to click next. Stays on for the next one until Esc; Ctrl+Z takes back each one whole.
- Popup: none.
- Interface hook: `qml/ShapeTools.js` (the tool table), run by the "shape" engine in `qml/SketchTools.qml`; one `Document::addSketchShape` step; key in `Shapes.order`
- Heavy / Lite: engine-independent
- Changes: 2026-10-30: added.

#### Center Rectangle                       [BUILT-IN]   [OURS]
- What it does: click the centre, then a corner. Drawn level and upright, held as a rectangle only (one Perpendicular, two Parallel, so it can be turned) with the centre at the middle of a diagonal (a three-point Midpoint).
- UI: search-only (Sketch mode): "Center Rectangle". Each click snaps like Line's; a preview follows the mouse; the prompt under the sketch bar says what to click next. Stays on for the next one until Esc; Ctrl+Z takes back each one whole.
- Popup: none.
- Interface hook: `qml/ShapeTools.js` (the tool table), run by the "shape" engine in `qml/SketchTools.qml`; one `Document::addSketchShape` step; key in `Shapes.order`
- Heavy / Lite: engine-independent
- Changes: 2026-10-30: Perpendicular + Parallel instead of Horizontal/Vertical. 2026-10-30: added.

#### 2-Point Circle                         [BUILT-IN]   [OURS]
- What it does: click both ends of a diameter. Clicks on existing points are held on the circle (Coincident on a curve).
- UI: search-only (Sketch mode): "2-Point Circle". Each click snaps like Line's; a preview follows the mouse; the prompt under the sketch bar says what to click next. Stays on for the next one until Esc; Ctrl+Z takes back each one whole.
- Popup: none.
- Interface hook: `qml/ShapeTools.js` (the tool table), run by the "shape" engine in `qml/SketchTools.qml`; one `Document::addSketchShape` step; key in `Shapes.order`
- Heavy / Lite: engine-independent
- Changes: 2026-10-30: added.

#### 3-Point Circle                         [BUILT-IN]   [OURS]
- What it does: click three places on the circle; refused when they're in a line. Existing points clicked are held on it.
- UI: search-only (Sketch mode): "3-Point Circle". Each click snaps like Line's; a preview follows the mouse; the prompt under the sketch bar says what to click next. Stays on for the next one until Esc; Ctrl+Z takes back each one whole.
- Popup: none.
- Interface hook: `qml/ShapeTools.js` (the tool table), run by the "shape" engine in `qml/SketchTools.qml`; one `Document::addSketchShape` step; key in `Shapes.order`
- Heavy / Lite: engine-independent
- Changes: 2026-10-30: added.

#### 2-Tangent Circle                       [BUILT-IN]   [OURS]
- What it does: click two lines, then where the circle goes: it sits in the angle nearest the mouse, its size set by how far out the mouse is. Held Tangent to both lines.
- UI: search-only (Sketch mode): "2-Tangent Circle". Each click snaps like Line's; a preview follows the mouse; the prompt under the sketch bar says what to click next. Stays on for the next one until Esc; Ctrl+Z takes back each one whole.
- Popup: none.
- Interface hook: `qml/ShapeTools.js` (the tool table), run by the "shape" engine in `qml/SketchTools.qml`; one `Document::addSketchShape` step; key in `Shapes.order`
- Heavy / Lite: engine-independent
- Changes: 2026-10-30: added.

#### 3-Tangent Circle                       [BUILT-IN]   [OURS]
- What it does: click three lines, then inside the circle you want (of the ones touching all three, the one whose centre is nearest). Held Tangent to all three.
- UI: search-only (Sketch mode): "3-Tangent Circle". Each click snaps like Line's; a preview follows the mouse; the prompt under the sketch bar says what to click next. Stays on for the next one until Esc; Ctrl+Z takes back each one whole.
- Popup: none.
- Interface hook: `qml/ShapeTools.js` (the tool table), run by the "shape" engine in `qml/SketchTools.qml`; one `Document::addSketchShape` step; key in `Shapes.order`
- Heavy / Lite: engine-independent
- Changes: 2026-10-30: added.

#### 3-Point Arc                            [BUILT-IN]   [OURS]
- What it does: click where the arc starts, where it ends, then a place it passes through; refused when the three are in a line.
- UI: search-only (Sketch mode): "3-Point Arc". Each click snaps like Line's; a preview follows the mouse; the prompt under the sketch bar says what to click next. Stays on for the next one until Esc; Ctrl+Z takes back each one whole.
- Popup: none.
- Interface hook: `qml/ShapeTools.js` (the tool table), run by the "shape" engine in `qml/SketchTools.qml`; one `Document::addSketchShape` step; key in `Shapes.order`
- Heavy / Lite: engine-independent
- Changes: 2026-10-30: added.

#### Tangent Arc                            [BUILT-IN]   [OURS]
- What it does: click the end of a line or arc (or its end point), then where the arc ends: it carries on smoothly from there, turning left or right as the mouse says. Held Tangent to the line (or to the arc).
- UI: search-only (Sketch mode): "Tangent Arc". Each click snaps like Line's; a preview follows the mouse; the prompt under the sketch bar says what to click next. Stays on for the next one until Esc; Ctrl+Z takes back each one whole.
- Popup: none.
- Interface hook: `qml/ShapeTools.js` (the tool table), run by the "shape" engine in `qml/SketchTools.qml`; one `Document::addSketchShape` step; key in `Shapes.order`
- Heavy / Lite: engine-independent
- Changes: 2026-10-30: added.

#### Inscribed Polygon                      [BUILT-IN]   [OURS]
- What it does: click the centre, then a corner. Sides from the options box (default 6). Its corners sit on a hidden helper circle and its sides are held Equal, so it stays regular.
- UI: search-only (Sketch mode): "Inscribed Polygon" ("polygon", "hexagon"). Each click snaps like Line's; a preview follows the mouse; the prompt under the sketch bar says what to click next. Stays on for the next one until Esc; Ctrl+Z takes back each one whole.
- Popup: the options box under the sketch bar: Sides (count).
- Interface hook: `qml/ShapeTools.js` (the tool table), run by the "shape" engine in `qml/SketchTools.qml`; one `Document::addSketchShape` step; key in `Shapes.order`
- Heavy / Lite: engine-independent
- Changes: 2026-10-30: added.

#### Circumscribed Polygon                  [BUILT-IN]   [OURS]
- What it does: click the centre, then the middle of a side. Its sides touch a hidden helper circle (Tangent) and are held Equal.
- UI: search-only (Sketch mode): "Circumscribed Polygon". Each click snaps like Line's; a preview follows the mouse; the prompt under the sketch bar says what to click next. Stays on for the next one until Esc; Ctrl+Z takes back each one whole.
- Popup: the options box under the sketch bar: Sides (count).
- Interface hook: `qml/ShapeTools.js` (the tool table), run by the "shape" engine in `qml/SketchTools.qml`; one `Document::addSketchShape` step; key in `Shapes.order`
- Heavy / Lite: engine-independent
- Changes: 2026-10-30: added.

#### Edge Polygon                           [BUILT-IN]   [OURS]
- What it does: click both ends of one side, then which side of it the polygon goes. Corners on a hidden helper circle (its centre hidden too), sides Equal.
- UI: search-only (Sketch mode): "Edge Polygon". Each click snaps like Line's; a preview follows the mouse; the prompt under the sketch bar says what to click next. Stays on for the next one until Esc; Ctrl+Z takes back each one whole.
- Popup: the options box under the sketch bar: Sides (count).
- Interface hook: `qml/ShapeTools.js` (the tool table), run by the "shape" engine in `qml/SketchTools.qml`; one `Document::addSketchShape` step; key in `Shapes.order`
- Heavy / Lite: engine-independent
- Changes: 2026-10-30: added.

#### Center To Center Slot                  [BUILT-IN]   [OURS]
- What it does: click the two end-arc centres, then how wide. Two lines and two half-circle ends, held Tangent where they meet and the ends Equal.
- UI: search-only (Sketch mode): "Center To Center Slot" ("slot"). Each click snaps like Line's; a preview follows the mouse; the prompt under the sketch bar says what to click next. Stays on for the next one until Esc; Ctrl+Z takes back each one whole.
- Popup: none.
- Interface hook: `qml/ShapeTools.js` (the tool table), run by the "shape" engine in `qml/SketchTools.qml`; one `Document::addSketchShape` step; key in `Shapes.order`
- Heavy / Lite: engine-independent
- Changes: 2026-10-30: added.

#### Overall Slot                           [BUILT-IN]   [OURS]
- What it does: click both far ends of the slot, then how wide; refused when it would be wider than long. Same holds as Center To Center Slot.
- UI: search-only (Sketch mode): "Overall Slot". Each click snaps like Line's; a preview follows the mouse; the prompt under the sketch bar says what to click next. Stays on for the next one until Esc; Ctrl+Z takes back each one whole.
- Popup: none.
- Interface hook: `qml/ShapeTools.js` (the tool table), run by the "shape" engine in `qml/SketchTools.qml`; one `Document::addSketchShape` step; key in `Shapes.order`
- Heavy / Lite: engine-independent
- Changes: 2026-10-30: added.

#### Center Point Slot                      [BUILT-IN]   [OURS]
- What it does: click the middle, one end's centre, then how wide. As Center To Center Slot, plus the middle held halfway between the end centres.
- UI: search-only (Sketch mode): "Center Point Slot". Each click snaps like Line's; a preview follows the mouse; the prompt under the sketch bar says what to click next. Stays on for the next one until Esc; Ctrl+Z takes back each one whole.
- Popup: none.
- Interface hook: `qml/ShapeTools.js` (the tool table), run by the "shape" engine in `qml/SketchTools.qml`; one `Document::addSketchShape` step; key in `Shapes.order`
- Heavy / Lite: engine-independent
- Changes: 2026-10-30: added.

#### Three Point Arc Slot                   [BUILT-IN]   [OURS]
- What it does: click the start, the end and a place the slot's centre line passes through, then how wide. Inner and outer arcs share the centre; the two round ends are held Tangent to both (on the right side each) and Equal; a hidden helper arc keeps the end centres on the centre line.
- UI: search-only (Sketch mode): "Three Point Arc Slot". Each click snaps like Line's; a preview follows the mouse; the prompt under the sketch bar says what to click next. Stays on for the next one until Esc; Ctrl+Z takes back each one whole.
- Popup: none.
- Interface hook: `qml/ShapeTools.js` (the tool table), run by the "shape" engine in `qml/SketchTools.qml`; one `Document::addSketchShape` step; key in `Shapes.order`
- Heavy / Lite: engine-independent
- Changes: 2026-10-30: added.

#### Center Point Arc Slot                  [BUILT-IN]   [OURS]
- What it does: click the centre, where the slot starts, where it ends (it goes the way the mouse went round), then how wide. Same holds as Three Point Arc Slot.
- UI: search-only (Sketch mode): "Center Point Arc Slot". Each click snaps like Line's; a preview follows the mouse; the prompt under the sketch bar says what to click next. Stays on for the next one until Esc; Ctrl+Z takes back each one whole.
- Popup: none.
- Interface hook: `qml/ShapeTools.js` (the tool table), run by the "shape" engine in `qml/SketchTools.qml`; one `Document::addSketchShape` step; key in `Shapes.order`
- Heavy / Lite: engine-independent
- Changes: 2026-10-30: added.

#### Fit Point Spline                       [BUILT-IN]   [OURS]
- What it does: click the points the curve passes through; Enter, or clicking the last point again, ends it. A smooth curve through every point (centripetal Catmull-Rom); drag a point and the curve follows.
- UI: search-only (Sketch mode): "Fit Point Spline" ("spline"). Each click snaps like Line's; a preview follows the mouse; the prompt under the sketch bar says what to click next. Stays on for the next one until Esc; Ctrl+Z takes back each one whole.
- Popup: none.
- Interface hook: `qml/ShapeTools.js` (the tool table), run by the "shape" engine in `qml/SketchTools.qml`; one `Document::addSketchShape` step; key in `Shapes.order`
- Heavy / Lite: engine-independent
- Changes: 2026-10-30: added.

#### Control Point Spline                   [BUILT-IN]   [OURS]
- What it does: click the points that pull the curve; Enter or the last point again ends it. A clamped cubic B-spline: it starts and ends on the first and last points and is pulled towards the rest, which show as its control polygon while drawing.
- UI: search-only (Sketch mode): "Control Point Spline". Each click snaps like Line's; a preview follows the mouse; the prompt under the sketch bar says what to click next. Stays on for the next one until Esc; Ctrl+Z takes back each one whole.
- Popup: none.
- Interface hook: `qml/ShapeTools.js` (the tool table), run by the "shape" engine in `qml/SketchTools.qml`; one `Document::addSketchShape` step; key in `Shapes.order`
- Heavy / Lite: engine-independent
- Changes: 2026-10-30: added.

#### Conic Curve                            [BUILT-IN]   [OURS]
- What it does: click the start, the end, then the corner it bends toward. Rho (0 to 1) from the options box sets how sharp it is: 0.5 is a parabola, below an ellipse, above a hyperbola (a rational quadratic Bézier).
- UI: search-only (Sketch mode): "Conic Curve". Each click snaps like Line's; a preview follows the mouse; the prompt under the sketch bar says what to click next. Stays on for the next one until Esc; Ctrl+Z takes back each one whole.
- Popup: the options box under the sketch bar: Rho (number, default 0.5).
- Interface hook: `qml/ShapeTools.js` (the tool table), run by the "shape" engine in `qml/SketchTools.qml`; one `Document::addSketchShape` step; key in `Shapes.order`
- Heavy / Lite: engine-independent
- Changes: 2026-10-30: added.

#### Point                                  [BUILT-IN]   [OURS]
- What it does: click where the point goes; refused on an existing point. Snaps like Line (so it can be held on a line, arc, middle or the Origin).
- UI: search-only (Sketch mode): "Point". Each click snaps like Line's; a preview follows the mouse; the prompt under the sketch bar says what to click next. Stays on for the next one until Esc; Ctrl+Z takes back each one whole.
- Popup: none.
- Interface hook: `qml/ShapeTools.js` (the tool table), run by the "shape" engine in `qml/SketchTools.qml`; one `Document::addSketchShape` step; key in `Shapes.order`
- Heavy / Lite: engine-independent
- Changes: 2026-10-30: added.

#### Ellipse                                [BUILT-IN]   [OURS]
- What it does: click the centre, the end of one axis, then how wide. The minor axis point is kept square to the major one by the solver.
- UI: search-only (Sketch mode): "Ellipse". Each click snaps like Line's; a preview follows the mouse; the prompt under the sketch bar says what to click next. Stays on for the next one until Esc; Ctrl+Z takes back each one whole.
- Popup: none.
- Interface hook: `qml/ShapeTools.js` (the tool table), run by the "shape" engine in `qml/SketchTools.qml`; one `Document::addSketchShape` step; key in `Shapes.order`
- Heavy / Lite: engine-independent
- Changes: 2026-10-30: added.

#### Text                                   [BUILT-IN]   [OURS]
- What it does: type the text in the options box, then click where its bottom left goes; the outline of the letters follows the mouse first. Stored as one anchor point plus the text and height; drawn as letter outlines (the system's sans-serif font). Drag the anchor to move it.
- UI: search-only (Sketch mode): "Text". Each click snaps like Line's; a preview follows the mouse; the prompt under the sketch bar says what to click next. Stays on for the next one until Esc; Ctrl+Z takes back each one whole.
- Popup: the options box under the sketch bar: Text, Height (length, default 0.25 in).
- Interface hook: `qml/ShapeTools.js` (the tool table), run by the "shape" engine in `qml/SketchTools.qml`; one `Document::addSketchShape` step; key in `Shapes.order`
- Heavy / Lite: engine-independent
- Changes: 2026-10-30: added.

#### Mirror                                 [BUILT-IN]   [OURS]
- What it does: click what to mirror (lines, arcs, circles, curves, points; what's selected first counts), Enter, then the line to mirror across: any line in the sketch or an axis. Each new point is held by Symmetry to the one it mirrors, across that line; points on the mirror line are shared, not copied; mirrored arcs turn the other way round; a mirrored circle gets its own rim and an Equal radius. Move the original and the copy follows.
- UI: search-only (Sketch mode): "Mirror" ("flip", "symmetry"). Each click snaps like Line's; a preview follows the mouse; the prompt under the sketch bar says what to click next. Stays on for the next one until Esc; Ctrl+Z takes back each one whole.
- Popup: none.
- Interface hook: `qml/ShapeTools.js` (the tool table), run by the "shape" engine in `qml/SketchTools.qml`; one `Document::addSketchShape` step; key in `Shapes.order`
- Heavy / Lite: engine-independent
- Changes: 2026-10-30: added.

#### Circular Pattern                       [BUILT-IN]   [OURS]
- What it does: click what to copy, Enter, then the centre to copy round. Quantity (default 3) and Total angle (default 360°, spaced evenly; less than 360° puts the last copy at that angle) from the options box. Each copied point is held by a Circular Pattern constraint (the original turned by its angle about the centre); copied circles keep an Equal radius.
- UI: search-only (Sketch mode): "Circular Pattern" ("pattern"). Each click snaps like Line's; a preview follows the mouse; the prompt under the sketch bar says what to click next. Stays on for the next one until Esc; Ctrl+Z takes back each one whole.
- Popup: the options box under the sketch bar: Quantity (count), Total angle (angle).
- Interface hook: `qml/ShapeTools.js` (the tool table), run by the "shape" engine in `qml/SketchTools.qml`; one `Document::addSketchShape` step; key in `Shapes.order`
- Heavy / Lite: engine-independent
- Changes: 2026-10-30: added.

#### Rectangular Pattern                    [BUILT-IN]   [OURS]
- What it does: click what to copy (or have it selected), set how many across and up and how far apart in the options box, Enter. The preview shows every copy. Each copied point is held by a Rectangular Pattern constraint (the original moved by that step).
- UI: search-only (Sketch mode): "Rectangular Pattern". Each click snaps like Line's; a preview follows the mouse; the prompt under the sketch bar says what to click next. Stays on for the next one until Esc; Ctrl+Z takes back each one whole.
- Popup: the options box under the sketch bar: Across (count, default 3), Spacing (length, 1 in), Up (count, default 1), Spacing (length, 1 in).
- Interface hook: `qml/ShapeTools.js` (the tool table), run by the "shape" engine in `qml/SketchTools.qml`; one `Document::addSketchShape` step; key in `Shapes.order`
- Heavy / Lite: engine-independent
- Changes: 2026-10-30: added.

#### Fillet                                 [BUILT-IN]   [OURS]
- What it does: click a corner where two lines meet: it's rounded with an arc Tangent to both lines, and the lines are shortened to meet it; the corner point goes. Radius from the options box (default 0.25 in), kept for the next corner. Select lines first (for example double-click a shape) and every corner between selected lines is rounded at once. All the arcs of one go share one radius dimension: change one value and they all follow. Refused when the radius doesn't fit the lines.
- UI: search-only (Sketch mode): "Fillet" ("f"). Each click snaps like Line's; a preview follows the mouse; the prompt under the sketch bar says what to click next. Stays on for the next one until Esc; Ctrl+Z takes back each one whole.
- Popup: the options box under the sketch bar: Radius (length).
- Interface hook: `qml/ShapeTools.js` (the tool table), run by the "shape" engine in `qml/SketchTools.qml`; one `Document::addSketchShape` step; key in `Shapes.order`
- Heavy / Lite: engine-independent
- Changes: 2026-10-30: added.

#### Equal Distance Chamfer                 [BUILT-IN]   [OURS]
- What it does: click a corner: it's cut by a new line the same distance along each line. The lost corner stays as a hidden point held on both lines, so the cut is a distance from it; the two distances are one dimension (the second follows the first). Works on many corners at once like Fillet.
- UI: search-only (Sketch mode): "Equal Distance Chamfer" ("chamfer"). Each click snaps like Line's; a preview follows the mouse; the prompt under the sketch bar says what to click next. Stays on for the next one until Esc; Ctrl+Z takes back each one whole.
- Popup: the options box under the sketch bar: Distance (length, default 0.125 in).
- Interface hook: `qml/ShapeTools.js` (the tool table), run by the "shape" engine in `qml/SketchTools.qml`; one `Document::addSketchShape` step; key in `Shapes.order`
- Heavy / Lite: engine-independent
- Changes: 2026-10-30: added.

#### Distance And Angle Chamfer             [BUILT-IN]   [OURS]
- What it does: click a corner: cut the distance along the line nearer the click, at the typed angle to it. Held by a distance and an angle dimension (shown as the angle typed, e.g. 30°).
- UI: search-only (Sketch mode): "Distance And Angle Chamfer". Each click snaps like Line's; a preview follows the mouse; the prompt under the sketch bar says what to click next. Stays on for the next one until Esc; Ctrl+Z takes back each one whole.
- Popup: the options box under the sketch bar: Distance (length), Angle (angle, default 45°).
- Interface hook: `qml/ShapeTools.js` (the tool table), run by the "shape" engine in `qml/SketchTools.qml`; one `Document::addSketchShape` step; key in `Shapes.order`
- Heavy / Lite: engine-independent
- Changes: 2026-10-30: added.

#### Two Distance Chamfer                   [BUILT-IN]   [OURS]
- What it does: click a corner: Distance 1 along the line nearer the click, Distance 2 along the other. Two dimensions.
- UI: search-only (Sketch mode): "Two Distance Chamfer". Each click snaps like Line's; a preview follows the mouse; the prompt under the sketch bar says what to click next. Stays on for the next one until Esc; Ctrl+Z takes back each one whole.
- Popup: the options box under the sketch bar: Distance 1, Distance 2 (lengths).
- Interface hook: `qml/ShapeTools.js` (the tool table), run by the "shape" engine in `qml/SketchTools.qml`; one `Document::addSketchShape` step; key in `Shapes.order`
- Heavy / Lite: engine-independent
- Changes: 2026-10-30: added.

#### Offset                                 [BUILT-IN]   [OURS]
- What it does: click a shape: the lines and arcs joined to it end to end come along (a closed loop, an open chain, or a circle; selected ones count, Ctrl-less clicks add or take away). An arrow on the shape shows which way is out: drag it out or in, or type the distance in the options box (typing keeps the side the arrow is on); Enter makes it. Each new piece is held at the distance from its original (lines Parallel at that gap, arcs sharing the centre with the radius changed by that much), all following one dimension; corners meet where the moved pieces cross. Refused when it goes in further than the shape allows.
- UI: search-only (Sketch mode): "Offset". Each click snaps like Line's; a preview follows the mouse; the prompt under the sketch bar says what to click next. Stays on for the next one until Esc; Ctrl+Z takes back each one whole.
- Popup: the options box under the sketch bar: Distance (signed length, default 0.125 in).
- Interface hook: `qml/ShapeTools.js` (the tool table), run by the "shape" engine in `qml/SketchTools.qml`; one `Document::addSketchShape` step; key in `Shapes.order`
- Heavy / Lite: engine-independent
- Changes: 2026-10-30: added.

#### Trim                                   [BUILT-IN]   [OURS]
- What it does: click the part of a line, arc or circle to cut away: it goes up to where something else crosses it (lines, arcs, circles, splines). A part between two crossings splits the line in two (held Collinear) or the arc (held Equal radius); the new ends are held on what crossed them. With nothing crossing, the whole thing goes. A circle needs two crossings and becomes an arc. The part that will go is shown first.
- UI: search-only (Sketch mode): "Trim". Each click snaps like Line's; a preview follows the mouse; the prompt under the sketch bar says what to click next. Stays on for the next one until Esc; Ctrl+Z takes back each one whole.
- Popup: none.
- Interface hook: `qml/ShapeTools.js` (the tool table), run by the "shape" engine in `qml/SketchTools.qml`; one `Document::addSketchShape` step; key in `Shapes.order`
- Heavy / Lite: engine-independent
- Changes: 2026-10-30: added.

#### Extend                                 [BUILT-IN]   [OURS]
- What it does: click near the end of a line or arc: it stretches to the next thing it meets and is held on it. An end shared with other lines gets a new point instead of moving them. An end held on another line lets go of it.
- UI: search-only (Sketch mode): "Extend". Each click snaps like Line's; a preview follows the mouse; the prompt under the sketch bar says what to click next. Stays on for the next one until Esc; Ctrl+Z takes back each one whole.
- Popup: none.
- Interface hook: `qml/ShapeTools.js` (the tool table), run by the "shape" engine in `qml/SketchTools.qml`; one `Document::addSketchShape` step; key in `Shapes.order`
- Heavy / Lite: engine-independent
- Changes: 2026-11-01: an end held on something (Point On, Coincident, Midpoint; Trim leaves such a hold) lets go when extended; it used to drag that line along to the new stop. 2026-10-30: added.

#### Break                                  [BUILT-IN]   [OURS]
- What it does: click a line, arc or circle: it's split where things cross it, around the click (the pieces held Collinear / Equal radius, and on what crossed them).
- UI: search-only (Sketch mode): "Break". Each click snaps like Line's; a preview follows the mouse; the prompt under the sketch bar says what to click next. Stays on for the next one until Esc; Ctrl+Z takes back each one whole.
- Popup: none.
- Interface hook: `qml/ShapeTools.js` (the tool table), run by the "shape" engine in `qml/SketchTools.qml`; one `Document::addSketchShape` step; key in `Shapes.order`
- Heavy / Lite: engine-independent
- Changes: 2026-10-30: added.

#### Sketch Scale                           [BUILT-IN]   [OURS]
- What it does: click what to scale, Enter, then the point it grows from. Factor from the options box (default 2). Dimensions wholly inside what's scaled scale with it.
- UI: search-only (Sketch mode): "Sketch Scale" ("scale"). Each click snaps like Line's; a preview follows the mouse; the prompt under the sketch bar says what to click next. Stays on for the next one until Esc; Ctrl+Z takes back each one whole.
- Popup: the options box under the sketch bar: Scale factor (number).
- Interface hook: `qml/ShapeTools.js` (the tool table), run by the "shape" engine in `qml/SketchTools.qml`; one `Document::addSketchShape` step; key in `Shapes.order`
- Heavy / Lite: engine-independent
- Changes: 2026-10-30: added.

#### Blend Curve                            [BUILT-IN]   [OURS]
- What it does: click the ends of two lines or arcs: a smooth curve joins them, carrying on along each (a control-point spline whose first and last legs are hidden helper lines held Collinear with the line, or Tangent to the arc).
- UI: search-only (Sketch mode): "Blend Curve". Each click snaps like Line's; a preview follows the mouse; the prompt under the sketch bar says what to click next. Stays on for the next one until Esc; Ctrl+Z takes back each one whole.
- Popup: none.
- Interface hook: `qml/ShapeTools.js` (the tool table), run by the "shape" engine in `qml/SketchTools.qml`; one `Document::addSketchShape` step; key in `Shapes.order`
- Heavy / Lite: engine-independent
- Changes: 2026-10-30: added.

#### Move/Copy                              [BUILT-IN]   [OURS]
- What it does: click what to move, Enter, then where to move it from and to. Turn (angle) turns it about the from point; Copy leaves the original and makes an unconstrained copy instead.
- UI: search-only (Sketch mode): "Move/Copy" ("move", "copy"). Each click snaps like Line's; a preview follows the mouse; the prompt under the sketch bar says what to click next. Stays on for the next one until Esc; Ctrl+Z takes back each one whole.
- Popup: the options box under the sketch bar: Turn (angle), Copy (tick box).
- Interface hook: `qml/ShapeTools.js` (the tool table), run by the "shape" engine in `qml/SketchTools.qml`; one `Document::addSketchShape` step; key in `Shapes.order`
- Heavy / Lite: engine-independent
- Changes: 2026-10-30: added.

#### Origin and axes in sketches           [BUILT-IN]   [OURS]
- What it does: every constraint and Sketch Dimension can use the model's origin point and the axes lying in the sketch, which never move. Tying a sketch to them is what locks it in place: once nothing is left free, its lines turn grey (fully constrained). In a sketch on a face, the Origin is where the model's origin falls square onto the face, and an axis counts if it lies along the sketch's X or Y.
- UI: click (or select) the origin's point or an axis like any sketch point or line, in any constraint or Sketch Dimension. A sketch point sitting on top of the origin is picked first. Refused, with a reason: an axis pointing straight out of the sketch, Fix/UnFix on them ("fixed already"), Equal with an axis or Midpoint of one (no length), Horizontal/Vertical on an axis, an axis as one of a Symmetry pair (it can be the mirror line), and the origin and an axis together. The History names them "the Origin", "the X axis" (the model's axis it lies along).
- Popup: none.
- Interface hook: `SketchGeometry::Origin` (point -1), `XAxis` / `YAxis` (lines -1, -2), `SketchGeometry::origin` (set from the plane when the sketch is made), `point(id)` / `line(id)` / `hasPoint` / `hasLine`; QML `SketchTools.asSketchPart` turns an origin pick into those ids. Saved like any other id in the constraint list.
- Heavy / Lite: engine-independent
- Tied to both: a connected piece of a sketch must not be tied both to projected geometry and to the origin or axes: if the body moved, the two would fight. Constraints and dimensions are checked when made and refused with a reason; a snap while drawing leaves the point there, untied. See Project.
- Changes: 2026-10-30: added.

#### Parallel, Perpendicular               [BUILT-IN]   [OURS]
- What it does: makes the second selected line parallel / perpendicular to the first; the first stays put where it can. Refused with a reason when it already holds or can't hold with the constraints already there (nothing moves).
- UI: search-only (Sketch mode): "Parallel", "Perpendicular". Two lines (see above).
- Popup: none.
- Interface hook: `Document::addSketchConstraint(sketch, type, first, second)`
- Heavy (exact surfaces): engine-independent
- Lite (triangles): engine-independent
- Engine differences: none.
- Changes: 2026-10-29: can be started first and the lines clicked after, as in Fusion. 2026-10-29: when the second line is held (e.g. a rectangle's side), the first line turns instead of refusing. 2026-10-29: added.

#### Equal                                  [BUILT-IN]   [OURS]
- What it does: makes the second selected line as long as the first (the first changes instead when the second is held). Two arcs or circles: the second takes the first's radius.
- UI: search-only (Sketch mode): "Equal". Like Parallel: two lines selected, or start it and click two lines (a selected line counts as the first); stays on until Esc.
- Popup: none.
- Interface hook: `Document::addSketchConstraint(sketch, "equal", first, second)`
- Heavy / Lite: engine-independent
- Changes: 2026-10-30: arcs and circles. 2026-10-29: can be started first and the lines clicked after. 2026-10-29: added.

#### Tool families in the search            [OURS]
- What it does: tools that come in kinds show as one search row: Rectangle (2-Point, 3-Point, Center), Circle (Center Diameter, 2-Point, 3-Point, 2-Tangent, 3-Tangent), Arc (3-Point, Center Point, Tangent), Polygon (Circumscribed, Inscribed, Edge), Slot (the five), Spline (Fit Point, Control Point), Chamfer (the three). Enter on the row runs its default kind; → (or the arrow on the row) opens every kind under it, the default marked; ← closes them. A kind typed for itself ("3-point", "tangent circle") shows on its own row. The default starts as Fusion's first kind and changes with use: the last 20 uses of each family are remembered; a kind becomes the default when it was used 4 times in a row, or 8 of the last 20 times and more often than the default. A family's one-key shortcut runs the same default and counts as a use: R (Rectangle) and C (Circle), Fusion's own keys; shown on the family's row. "c" finds Circle, "r" Rectangle.
- UI: Alt+Space search, Sketch mode.
- Popup: none.
- Interface hook: `qml/Families.js` (the families, `next` for the default rule, `defaultOf`); `CommandSearch.families` / `familyRows` / `toggleKinds` / `used`; Main.qml `familiesNow`, `noteUse`, Settings category "tools" (uses, defaults as JSON); one-key shortcuts: Main.qml `oneKeyTools` (a command's or family's single-letter `keys`) and `runKey`.
- Heavy / Lite: engine-independent
- Changes: 2026-10-30: keys R and C. 2026-10-30: added.

#### Sketch Dimension                       [BUILT-IN]   [OURS]
- What it does: holds a line's length, the distance between two points, the distance from a point to a line (measured square to the line), or the gap between two parallel lines (from the middle of the second, square to the first) at a typed value. An arc: its radius; a circle: its diameter. Two lines that aren't parallel: the angle between them, typed in degrees (`45`, `30.5°`), on the side of the corner that was clicked: from where the lines cross (or would), towards the spots clicked on each (towards their middles when they were selected first), as in Fusion. So a corner shows its inside angle whichever way its lines were drawn. Moves as few points as it can (ring by ring from the measured points). Refused with a reason when the constraints already there don't allow it, or when that pair is already dimensioned.
- UI: key D (Fusion's own shortcut; only in a sketch, not while a box is typed in; stops any other tool), or the search: "Sketch Dimension". With a line selected: its length. Two lines selected: the gap between them. Two points, or a point and a line: their distance. One point: waits for the second point or a line. Nothing: waits for a click on a line, or on a point and then a point or a line (the tool stays on until Esc). While a line's length box is open, clicking a second line dimensions the gap instead (parallel), or the angle between them (not parallel); the prompt says so. An angle shows as a dashed arc round the corner from one line to the other, an arrow at each end, its value in the middle of the arc; dashed lines run out from a line's end when the arc lies past it (never from an axis). It's drawn on the side of the corner where the lines were clicked (towards their middles when selected first), and the arc shows while its value is typed. Dragging an angle's value round the corner picks which of the four sides it measures: between the arms it shows that side's angle, so the inside of a corner reads the inside angle and across a line it reads the other one (180° less it), live as it's dragged; how far out it's dropped is the arc's size. It's only how it's shown: the lines don't move. Double-click it to type that side's angle (more than 0°, under 180°). The value shows beside the dimension (double-click it to change it); a gap's value sits a quarter of the way along the lines, so a rectangle's two gaps don't overlap. A value never sits on a point (a circle's centre, a rectangle's corner, a line's middle, the Origin) or on another value: it steps out from its spot (a radius or diameter slides along its own line first, the rest out to the side) until it's clear; sitting on a line is fine. Drag a value to put it anywhere; it stays there, measured from the middle of what it measures, so it follows when the sketch moves.
- Popup: a value box where the dimension goes, starting at the length as it is now. Inches unless a unit is typed, with or without a space, short or spelled out: in/inch/inches/", ft/foot/feet/', mm, cm, m (and spelled-out metric names); fractions (3/4, 1 1/2, 1-1/2) and parts that add up (1' 6"). The first unit typed is the dimension's main unit; the other system shows in brackets ("1.000 in (25.4 mm)", "60 mm (2.362 in)"). Imperial shows 3 places.
- Interface hook: `Document::addSketchDimension`, `setSketchDimension`, `sketchMeasure`, `sketchDimensions`; parsing and showing in `qml/Units.js`; constraint types "length", "distance", "pointline", "lineline", "radius", "diameter", "angle" in `SketchGeometry` (`SketchGeometry::parallel` for the check). An angle's side: `otherSide` on `sketchMeasure` / `addSketchDimension`, saved as u = -1 (the second line taken end to start); QML `SketchTools.angleOtherSide`. A dragged value: `Document::placeDimensionLabel(sketch, constraint, u, v, placed)` (offset from the measured middle, no history step; saved as "label_offset_mm"; an angle's u, v run along its first line and its second from the corner, saved as "label_along_lines_mm", and placed false keeps only the side it was made on); `sketchDimensions` gives mid_u/mid_v, placed, label_u/label_v; the dodging is QML `Main.placeLabels`. An angle's corner: `sketchDimensions` / `sketchMeasure` give "frame" (cross, e1, e2, ends1, ends2, axis1/2; `angleFrame` in document.cpp) and "side"; its look is QML `Main.angleLook` (arc, arrows, ext, spot, shown, flipped) drawn by `AngleMarks`; the clicked spots come from `SketchTools.angleSpots`.
- Heavy / Lite: engine-independent
- Value box leash: once a value is dragged clear of what it measures, a dashed line ties it back, from the closest place on the box to the closest place on the line, so it re-routes as it moves. The box stays within 2 times the measured length of it (`nearestOn`, `keptNear` in Main.qml), 44 px at least so a tiny dimension can still clear its own line; the limit also applies when a line is shortened later, on screen only until the value is next moved. Angles keep their own arc and marks, with no leash or limit. Fusion divergence: Fusion has no such limit (not confirmed).
- Changes: 2026-11-01: dashed leash and distance limit for moved values (owner). 2026-10-30: an angle shows as an arc with arrows, and dragging its value round the corner picks the side. 2026-10-30: values dodge points and can be dragged. 2026-10-30: key D. 2026-10-30: an angle is on the clicked side of the corner. 2026-10-30: radius, diameter and angle. 2026-10-29: gap between two parallel lines added. 2026-10-29: added.

#### Horizontal/Vertical                    [BUILT-IN]   [OURS]
- What it does: a line: holds it along the sketch's X or Y axis, whichever it's nearer. Two points: lines them up level or one above the other, whichever they're nearer (the first stays put).
- UI: search-only (Sketch mode): "Horizontal/Vertical". A line, or two points (see above). 2-Point Rectangle also makes these on its sides.
- Popup: none.
- Interface hook: `Document::addSketchConstraint(sketch, type, first, second)`; constraint types "horizontal" / "vertical" (a line) and "horizontalpoints" / "verticalpoints" (two points) in `SketchGeometry`
- Heavy / Lite: engine-independent
- Changes: 2026-10-29: in the search, and for two points. 2026-10-29: added.

#### Coincident                             [BUILT-IN]   [OURS]
- What it does: two points: joins them (the second moves onto the first). A point and a line: puts the point on the line (or on its extension, as in Fusion). A point and an arc or circle: puts it on the curve. Refused for the two ends of one line, or a line's own end.
- UI: search-only (Sketch mode): "Coincident". A point, then a point or a line; or a line, then a point (the line stays put).
- Popup: none.
- Interface hook: `Document::addSketchConstraint(sketch, type, first, second, 0, keep)` (`keep` 1: the second id was picked first); constraint types "coincident" (point, point) and "pointonline" (point, line) in `SketchGeometry`
- Heavy / Lite: engine-independent
- Changes: 2026-10-30: onto arcs and circles. 2026-10-29: added.

#### Midpoint                               [BUILT-IN]   [OURS]
- What it does: holds a point at the middle of a line. Refused for one of the line's own ends.
- UI: search-only (Sketch mode): "Midpoint". A point and a line, either first (whichever was picked first stays put).
- Popup: none.
- Interface hook: `Document::addSketchConstraint(sketch, "midpoint", point, line, 0, keep)`; constraint type "midpoint" in `SketchGeometry`
- Heavy / Lite: engine-independent
- Changes: 2026-10-29: added.

#### Collinear                              [BUILT-IN]   [OURS]
- What it does: puts the second line on the same infinite line as the first (both its ends on the first's extension).
- UI: search-only (Sketch mode): "Collinear". Two lines (see above).
- Popup: none.
- Interface hook: `Document::addSketchConstraint(sketch, "collinear", first, second)`; constraint type "collinear" in `SketchGeometry`
- Heavy / Lite: engine-independent
- Changes: 2026-10-29: added.

#### Symmetry                               [BUILT-IN]   [OURS]
- What it does: makes two points, or two lines, mirror images of each other across a third line (its infinite extension). For two lines, each end is paired with the nearer mirrored end (saved as "ends_swapped" when the second line's ends pair the other way round). The mirror line and the first pick stay put where they can. Refused when the mirror line is one of the pair, a point to mirror lies on the mirror line, or the mirror line has no length.
- UI: search-only (Sketch mode): "Symmetry". Two points or two lines, then the line to mirror across (see above).
- Popup: none.
- Interface hook: `Document::addSketchConstraint(sketch, "symmetry", first, second, mirrorLine)`; constraint types "symmetry" (points) and "symmetrylines" (lines) in `SketchGeometry`, with the mirror line in `third`
- Heavy / Lite: engine-independent
- Changes: 2026-10-29: added.

#### Fix/UnFix                              [BUILT-IN]   [OURS]
- What it does: holds the selected points (or the ends of the selected lines) where they are; if all of them are already fixed, lets them go.
- UI: search-only (Sketch mode): "Fix/UnFix". With points or lines selected: done at once. Nothing selected: the tool starts; each click on a point or line fixes it or lets it go, until Esc.
- Popup: none.
- Interface hook: `Document::fixSketchPoints(sketch, pointIds)`
- Heavy / Lite: engine-independent
- Changes: 2026-10-29: can be started first and clicked after. 2026-10-29: added.

#### Edit Sketch                            [BUILT-IN]   [OURS]
- What it does: goes back into a finished sketch to change it. From inside another sketch, that one is finished first. A hidden sketch shows again as it opens (in the same step; Ctrl+Z hides it again). One step in the design's history ("Edited Sketch1"); Ctrl+Z on it leaves the sketch again. What's done inside joins that sketch's own history.
- UI: double-click the sketch's dot in the Timeline, or its row in the Browser (as in Fusion). The Timeline dot's tip says "double-click to edit".
- Popup: none.
- Interface hook: `Document::editSketch(id)`; history entry type "edit_sketch" ("was_hidden": it was shown to be edited)
- Heavy / Lite: engine-independent
- Changes: 2026-10-30: a hidden sketch shows as it's edited. 2026-10-29: added.

#### Drag in a sketch                       [BUILT-IN]   [OURS]
- What it does: left-drag a point (its lines turn and stretch; a circle's centre carries the circle at its size; the edge resizes it, unless its size can't change (a radius or diameter dimension, Equal to a dimensioned one, anything else): then the edge carries the whole circle, so grabbing anything always does something), a line (it moves whole), or anything selected (it all moves together). Drags snap the way drawing does (see Snapping while drawing): the dragged point, or for a line or a selection whichever of its points comes nearest something, snaps onto another point (a line's end, a rectangle's corner), the Origin, a line's middle, or anywhere along a line or axis. Let go there and it's held by Coincident (a point, the Origin, along a line) or Midpoint, in the same history step as the move ("Moved Point2 (Coincident)"), so one Ctrl+Z takes back both. A drag never snaps onto itself: its own points, the lines that move with them, those lines' other ends, and points already Coincident with a dragged one are left out. A snap only counts if the constraints really let the point get there; a constraint that's already there isn't made twice. Double-click a line, arc, curve or point selects its whole chain: everything joined end to end through a shared point, a Coincident, or a point held on it (Point On, Midpoint), normal and construction each only along their own kind. Lines that only cross are not joined, though they can still close a profile. Circles and ellipses have no ends, so they select alone unless something is held on them. In Fusion a double-click selects a loop and stops at branches; here a branch is followed. Drag from empty space for a box (Fusion's window select): left to right, a solid box takes what is wholly inside; right to left, a dashed box takes anything it touches; Ctrl or Shift adds; Esc drops it. The end points of what is taken stay out of the selection (Fusion takes them too), so two boxed lines still count as two lines for Parallel and Equal; a point on its own is taken. Fusion also boxes profile areas; not here yet. A drag changes size first, then angles and shape, and moves things last: a free circle's edge resizes it (`SketchGeometry::resize`: a passing radius hold, the rim alone first, then its neighbours); for points, first only the dragged points, the ends of their lines and the arcs they touch may move, with every other line holding its heading (so a rectangle's corner drag stretches its sides and keeps its turn; once its sizes are held, it turns), then the same without the headings, then the ring grows one step at a time (with headings first, then without). Fixed points never move. One history step per drag.
- UI: mouse, Sketch mode, no tool active. While snapped, the same marks and tag as drawing show: the target point lights up, a line's middle shows ▲, a spot along a line ○, and the tag by the mouse names it ("Point3", "Midpoint of Line1", "On Line4", "Origin", "On the X axis"). After the drop, the bar at the top says what holds it ("Moved Point2, held by Coincident").
- Interface hook: `Document::beginSketchDrag`, `dragSketch(sketch, moves)`, `dragSketchSize(sketch, arc, radius)`, `endSketchDrag(sketch, summary, hold)` (hold = {point, on: "point" | "origin" | "midpoint" | "line", target}; returns the constraint's name or ""); `SketchGeometry::drag`; QML `SketchTools.dragBegin` / `dragMove` / `dragEnd`, `snapNear(at, reach, geometry, skip)`, `dragSkip`, `dragSnap`
- Heavy / Lite: engine-independent
- Changes: 2026-10-30: size first, then angles/shape, position last (owner's order). 2026-10-30: the edge of any circle whose size is held carries it (tries the resize, falls back to moving; `SketchTools.moveFrom`). 2026-10-30: a sized circle's edge carries it (so a drag shows what's still free, as in Fusion). 2026-10-30: a circle's centre carries the circle; a tangent circle goes round the other. 2026-10-30: a lone circle dragged by its edge resizes; arcs move whole. 2026-10-30: snaps like drawing, and holds where it was dropped. 2026-10-29: added.

#### Project                                [BUILT-IN]   [OURS]
- What it does: Fusion's Project. Throws the model's shadow straight onto the sketch's plane as lines, arcs and circles, in a violet no other line uses. Whole Model: the outline and anything open right through it. Face: that one face's outline, flat or curved. A flat face seen edge on gives one line. Stays on for more clicks; the Done button or Esc stops. Projecting the same thing again says so and adds nothing, and corners shared with an earlier projection are one point.
- UI: search "Project" (Sketch mode; also "project geometry", "silhouette", "outline"), or P. Whole Model and Face sit under the sketch bar; then click the model. In Face, hovering lights the face.
- Popup: none; the two choices above, and the guide reports what was made.
- Interface hook: `Document::projectIntoSketch(sketch, face)` (face -1: whole model), `Document::faceAt`; `projection::outline` in `src/document/projection.cpp`; `SketchGeometry::projectedPoints`, `tiedToBoth`; `SketchTools.projectWhat`; `Theme.sketchProjected`. History: a "shape" step, undone by snapshot. Saved as a `projected` flag on points.
- Heavy / Lite: engine-independent: it works from the drawn triangles (`Mesh`), joined by Manifold's `CrossSection` (Clipper2), in 64 x 64 cells then group by group: 16 s as one union on a threaded part, 0.2 s this way. A whole model takes only triangles facing one way.
- Reading the outline: a loop that stays on one circle is a Circle; otherwise, from its sharpest corner, the longest straight run or arc wins. An arc needs five points, one bend direction and no corner over 40 degrees. Tolerance 0.012 mm or size / 25000, twice how far drawn triangles may stray. An ellipse comes out as a chain of arcs within that tolerance. Arc centres sit exactly midway between their ends. Only a circle's centre shows; an arc's stays hidden, as a shallow one's floats outside the outline.
- Held: projected points act like the origin: nothing solves for them, dragging, resizing and Fix/UnFix don't apply. They count as fully constrained and form closed profiles.
- Fusion divergences: Fusion's choices are Specified Entities and Bodies, and it projects a face's edges; ours are Whole Model and Face and use the shadow, so a curved face gets a true outline. Holes that don't go through are not drawn. The link isn't live: a projection stays where it was made, as nothing can move a body yet; nothing is linked or re-linked. No choice to break the link; delete and project again.
- Not yet: a body picker for several bodies (the app holds one model). Projecting single edges and points. A line drawn directly between an existing point tied to the origin and an existing projected point isn't refused.
- Changes: 2026-11-01: Done button. The shadow is grown and shrunk by half the tolerance to seal zero-width cracks between triangles, which read as lines inside the outline. 2026-10-31: added. 2026-11-01: no one step may take over 40% of an arc: a long straight with short bends at both ends passed as a bulging arc, on Speaker Mount.step an 11 mm edge bowed out 1 mm.

#### Constraint solver                      [BUILT-IN]   [OURS]
- What it does: moves points as little as possible (weighted, measured from where they started) until every constraint holds: Gauss–Newton, each step x = x0 + W Jᵀ (J W Jᵀ)⁻¹ (J (x − x0) − r), Jacobian by central differences, tolerance 1e-8; a solve that has stopped improving with under 1e-6 left also counts as done (repeated constraints leave a hair). The damping on the diagonal is scaled to the matrix. Residuals: an arc's end-to-centre distances differing (every arc and circle), sine of the angle between lines (Parallel), its cosine (Perpendicular), sine to the axis (Horizontal/Vertical), distance in mm (Fix), mm off (dimensions, Equal). Ring-by-ring solving (`SketchGeometry::solveRings`) is shared by dragging and dimensions. `SketchGeometry::solveOnly(ring, steady)` lets only the given points move and gives up (putting everything back) if anything else would. Passing holds used only while dragging: "heading" (a line keeps its direction (u, v): residual its cross product with the line, over the length) and a radius hold on one arc (id INT_MAX), never saved.
- Interface hook: `SketchGeometry::solve`, `solveOnly`, `solveRings`, `SketchGeometry::drag`, `resize`
- Heavy / Lite: engine-independent
- Changes: 2026-10-30: `solveOnly`, passing "heading" and radius holds. 2026-10-29: added.

#### Fully constrained                      [BUILT-IN]   [OURS]
- What it does: finds the points that can't move any more (no movement keeps every constraint and moves them: the constraints' Jacobian is row-reduced and a coordinate counts as pinned when no free direction touches it). A line is fully constrained when both ends are. Lines that can still move draw in the text colour (white in dark themes); fully constrained ones in the same, drained of colour and toned about halfway to the background (grey: 50% of the text colour's lightness in dark looks, 60% of the way to white in light looks; `Theme.sketchFull`).
- UI: the colour of every sketch line and point.
- Interface hook: `SketchGeometry::fullyConstrained`; `Document::fillSketches(target, fullTarget, sketches, radius)`; `full` flags in `Document::sketchGeometry`
- Heavy / Lite: engine-independent
- Changes: 2026-10-30: the grey made much further from the free colour (it was 68% / 45%, too close to tell on thin lines). 2026-10-29: added.

### File and history

#### Save, Save As                          [BUILT-IN]   [OURS]
- What it does: writes the design to an `.omamake` file: indented JSON, readable by people as well as the app (components, bodies with mesh counts and bounds, sketches with every point, line and constraint and whether each holds, hidden items, and the whole history). The design takes the file's name. Each constraint also carries its raw u, v and each sketch its origin_uv, so Open rebuilds it exactly.
- UI: search "Save" / "Save As" (both modes); Ctrl+S, Ctrl+Shift+S. The first Save asks for a name like Save As. The system file dialog follows the app's light/dark look; the look can't be switched while it's open.
- Popup: the system Save dialog, in `~/Documents/OmaMake` at first. It lists the same bookmarks as the file manager (GTK's, `~/.config/gtk-3.0/bookmarks`, which Nautilus keeps).
- Interface hook: `Document::saveTo(path)`, `Document::toJson()`
- Heavy / Lite: engine-independent
- Changes: 2026-10-30: raw constraint values and sketch origins written for Open. 2026-10-29: added.

#### Load                                   [BUILT-IN]   [OURS]
- What it does: reads an `.omamake` file back: components, bodies (made again from the history), sketches exactly as saved (points, lines, construction, arcs, curves, every constraint and dimension, where dimension values were dragged to), what's hidden, and the history, shown in the Timeline. Opens into this window if nothing has been done in it yet, otherwise into a new window, so unsaved work is never lost (Fusion opens a new tab). Steps from before it was saved can't be taken back with Ctrl+Z (as in Fusion, where undo starts fresh on opening); new work can. Opens with no sketch being edited. A file that isn't an OmaMake design, is missing or was saved by a newer version is refused with a note, and what's open stays. Files saved before 2026-10-30 open too (an angle dimension's side may come back flipped).
- UI: search "Load" (both modes; typing "open" finds it too, shown as Load); Ctrl+O. Named Load, not Fusion's "Open" (owner: Load and Save go together). Also a file named on the command line (`omamake design.omamake`), which is what double-clicking one in the file manager will do once OmaMake is installed with its file type.
- Popup: the system file dialog titled "Load", in the last file's folder or `~/Documents/OmaMake`, showing only `.omamake` files; same bookmarks and light/dark look as Save.
- Interface hook: `Document::openFrom(path)`, `Document::untouched()`, `Document::openInNewWindow(path)`; `Main.load` / `Main.loadFile` / `loadDialog`; `main.cpp` (argument, `openNote`).
- Heavy / Lite: engine-independent
- Changes: 2026-10-30: added; renamed Open → Load.

#### Undo and the Timeline                  [BUILT-IN]   [OURS]
- What it does: one history for the whole design. Every action (a sketch, a line, a constraint, a drag, a deleted constraint, showing or hiding something) is a step; Ctrl+Z takes back the newest, one at a time, in Model and Sketch mode alike. Steps done inside a sketch belong to that sketch. No Redo yet.
- UI: Ctrl+Z. The Timeline along the bottom shows one dot per step (hover for its name): the design's steps outside a sketch, only that sketch's own steps inside one. Double-click a sketch's dot to edit it. The tab in the middle of the bottom edge hides it.
- Popup: none.
- Interface hook: `Document::undo()`, `Document::record(action, summary)`, `Document::history`; `qml/HistoryTimeline.qml`
- Heavy / Lite: engine-independent
- Changes: 2026-10-30: deleting constraints is undoable. 2026-10-29: added.

### Interface

#### Hover tips                              [BUILT-IN]   [OURS]
- What it does: resting the mouse on a button for 2 s shows a small note under it with the button's name and what it does; it goes when the mouse leaves or clicks, and the 2 s start again when it comes back. Shown over everything else in the window. Slower than Fusion's tips on purpose (owner: once the buttons are known, they shouldn't get in the way).
- UI: the Look at Sketch and Toggle Construction buttons in the sketch bar (so far).
- Popup: the tip itself.
- Interface hook: `qml/HoverTip.qml` (theme, area: the button's MouseArea, title, text, delay).
- Heavy / Lite: engine-independent
- Changes: 2026-10-30: added.

#### Sketch keys                            [BUILT-IN]   [OURS]
- What it does: every sketch drawing tool, tool family and constraint has a key, working only inside a sketch and only reaching sketch tools: no model tool can run from a sketch, and no sketch key does anything outside one (owner: Fusion's F jumping out of a sketch to fillet a body is never wanted). Plain letters draw and modify (Fusion's own where it has one: L R C D T O X M; ours: F Fillet, A Arc, G Polygon, E Ellipse, U Slot, B Spline, Q Conic Curve, K Break); Shift+letter makes constraints (H Horizontal/Vertical, C Coincident, T Tangent, E Equal, P Parallel, R Perpendicular, F Fix/UnFix, M Midpoint, O Concentric, I Collinear, S Symmetry); Alt+letter the less used tools (L Midpoint Line, P Point, T Text, M Mirror, C Circular Pattern, R Rectangular Pattern, E Extend, S Sketch Scale, B Blend Curve, F Chamfer, G Find Profile Gaps). A family's key draws its default kind. Space runs the last sketch tool or constraint again (Fusion's Repeat Last Command); S opens the command search (Fusion's S opens its shortcuts box). Holding Ctrl while drawing places points exactly under the mouse with no snapping, so nothing is attached or held level (Fusion: no inferred constraints). The search's one-letter picks follow the plain keys ("e" Ellipse, "m" Move/Copy; "h" stays Horizontal/Vertical). Left free for Fusion's sketch keys still to come: P Project, I Measure, V Toggle Visibility. Keys are off while a box is typed in, the search or a menu is open. Divergences: Fusion's F, E, M, A, Q, B are model tools outside a sketch, and Fusion has no keys for constraints or most drawing tools.
- UI: keyboard, in a sketch; each command's keys show beside it in the search.
- Popup: none.
- Interface hook: `qml/SketchKeys.js` (the map, `apply`, `sequence`); `Families.js` family keys; Main.qml `oneKeys`, `repeatKey`, `searchKey`, `lastSketchTool`; `SketchTools.free` ← `Navigation.ctrlHeld`.
- Heavy / Lite: engine-independent
- Changes: 2026-10-30: Alt+G Find Profile Gaps. 2026-10-30: all sketch tools and constraints given keys; Space, S, Ctrl-held free drawing.

#### Find in Window                         [BUILT-IN]   [OURS]
- What it does: brings a Browser row's things back into full view, for when a flick has sent the view far off. The view flies (0.38 s) to centre on it and zooms in or out until all of it fits, so a small part of a big design fills the window. If a shown body hides it from where the camera is, the view also turns, by the least it can, to a way round that sees it (at least 90%, or as much as any way does), keeping 25° clear of where it would be hidden so a sketch on top of a body isn't shown edge-on. If it's wholly inside a body, it doesn't turn and only zooms to where it is (the camera ends up inside the body, whose walls then don't show). Works on every row: the design (everything), components (what they hold; an empty one, its origin), Bodies and Sketches folders, bodies, sketches, the Origin and each of its points, axes and planes. Hidden things are found too; hidden bodies don't hide anything. Stops any gliding view. Fusion has the same command, by the same name, on the Browser's right-click menu; here it's a button on each row and also turns round blocking bodies, which Fusion's doesn't.
- UI: the target button left of each Browser row's eye (on the top row too, where there's no eye). Hover 2 s for its tip.
- Popup: a note only when it's inside a body, or there's nothing to show.
- Interface hook: `Document::findInWindow(key, from, halfAngle)` (the framing and the which-way-round search: 300 ways round a ball, a ray from each of up to 200 of the thing's points towards the camera, against the shown body's triangles); `Navigation.findInWindow(key)`; `qml/FindButton.qml`; Browser `findRequested(key, name)`, handled in Main.qml.
- Heavy / Lite: engine-independent (reads the built mesh)
- Changes: 2026-10-30: added.

#### Show/Hide in the Browser               [BUILT-IN]   [OURS]
- What it does: the eye on a row shows or hides it and everything inside it; one history step each ("Hid Sketch1 in (Unsaved)"). The sketch that's open can be hidden too (to see the model behind it while still in it): its lines, points, values and constraint badges go, nothing of it can be picked, dragged or snapped to (the Origin and axes still can), and a note says it's hidden and how to show it. Tools still work; what they draw stays hidden with the sketch until its eye shows it. Hiding a folder above it (Sketches, a component) hides it the same way. While sketching, the origin planes' eyes are the sketch's own (see Create Sketch).
- UI: the eye on every Browser row but the top.
- Popup: none.
- Interface hook: `Document::setShown(key, shown)`, history entry type "visibility"; QML `Main.visibleKeys`, `Main.openSketchShown` (drawing, `Selection.sketchId`, the values' and badges' layers, `SketchTools.geometryShown` for snapping), `Browser` `EyeButton`
- Heavy / Lite: engine-independent
- Changes: 2026-10-30: the open sketch can be hidden. 2026-10-29: added.

#### Command search                         [BUILT-IN]   [OURS]
- What it does: every command lives here (the screen starts empty on purpose). It only offers what fits the mode you're in (Model or Sketch), shows nothing until you type, and ranks what you type: a name starting with it first, then a word of the name starting with it (so one letter is usually enough: in a sketch "L" finds Line and "R" 2-Point Rectangle), then the words' first letters ("hv" Horizontal/Vertical, "sd" Sketch Dimension), then a few other words people search by ("dark" finds Light Mode while it's dark), then anywhere in the name (from 2 letters), then the description (from 3 letters). The mode's own tools always come before the look, file and app commands, so typing more is what brings those up. Ties keep the list's order, most-used tools first.
- UI: Alt+Space opens it at the top centre; type, ↑ ↓ to choose, Enter runs the top (or chosen) one, Esc or a click outside closes it. The typed part shows in bold.
- Popup: none.
- Interface hook: `qml/CommandSearch.qml` (`score`, `matches`, `asideGroups`); the command list in `qml/Main.qml` (name, words, hint, group, modes, keys, run). Look commands in both modes: Light/Dark Mode, Follow Omarchy Theme, Browser; Model mode only: Grid, Light Follows View, Simple Look, Origin Planes.
- Heavy / Lite: engine-independent
- Changes: 2026-11-01: rows appearing under a still mouse no longer take the choice; only a real move or the arrow keys do (`hoverRow`). 2026-10-30: empty until typed, ranked by how well it fits and by mode; Light Follows View, Simple Look and Origin Planes moved to Model mode only. 2026-10-29: added.

#### View inertia                           [BUILT-IN]   [OURS]
- What it does: a pan or orbit let go while still moving glides on in the same direction and slows to a stop (its speed fades by about two thirds every 0.15 s; a quick flick is still after under a second). Letting go after holding the mouse still doesn't glide. Any click catches a gliding view; the view cube's faces, Home and fitting stop it too. A wheel notch's zoom eases in over about half that time instead of jumping, toward the cursor, and adds up to exactly the same zoom as before. One knob scales all three together, zoom always at half. Fusion has no view inertia (it stops dead on release); this is ours.
- UI: middle-drag pan, Shift+middle-drag orbit, left-drag with the hand on, dragging the view cube, the mouse wheel.
- Popup: none.
- Interface hook: `qml/Navigation.qml`: `inertia` (seconds; 0 turns it all off), `zoomInertia` (= inertia × 0.5), `note()` / `letGo()` / `stopGlide()`, a FrameAnimation; `ViewCube.qml` calls `nav.letGo()` on release.
- Heavy / Lite: engine-independent
- Changes: 2026-10-30: added.

### Stock Dune 3D features (Heavy, not checked out yet)

Dune 3D 1.4 has one class per feature in `src/document/group/`: Dune 3D 1.4 has one class per feature in `src/document/group/`:
extrude, revolve, lathe, fillet, chamfer, loft, sweep, pipe, circular sweep, mirror, mirror H/V,
linear array, polar array, clone, replicate, reference, sketch, solid-model operation,
local operation, exploded cluster. These get full entries once the source is checked out.

## Plugin features

None yet.

#### Fit Curved Surfaces: the method as a whole
- Status: ours. Proven on one threaded part of 18,542 triangles (459 faces, valid, closed, about 7 s). Everything else is untested since the fillet, thread and tidy work.
- Stock: OpenCascade gives the surfaces, faces and checks. It has no mesh recognition.
- Order, each stage only taking what's left: flat areas; sections along the part's long axis (cylinders, cones, threads, and a restart on a mesh cut along the threads); surfaces grown from seeds; rounded edges from flat outlines, rolling ball, then rows, coaxial cones and scraps; `tidy`; faces, solid and edges; merging halves. Each section below covers one part.
- Failure: a surface whose faces come out invalid is dropped and its triangles stay flat facets. Up to 12 rounds. A bad surface costs its smoothness, not the fit.
- Limits today: one section axis; several limits in fixed mm, tuned at about 90 mm; one body; no gap closing; no free-form surfaces; no triangle limit.
- Planned: refuse above about 300,000 triangles (decorative figurines don't belong here), a check box to keep a low-poly model's intended facets, 3MF, several bodies, an open-shell warning.

#### Fit Curved Surfaces: rounded edges (fillets)

- Status: ours. Where: Body right-click, Fit Curved Surfaces. No popup or options; runs as the stage "Reading rounded edges".
- Finds rolling-ball fillets beside large flats. Height against the turn from the flat is straight and its slope is the radius; each triangle moved in by that radius traces the ball's centre line. Straight lines become cylinders, circles become tori. A rounded outline is taken off piece by piece, the piece with the most triangles first.
- The flat's outline is read first: it is walked and cut into lines and circles, which are exact where other mesh points only sit near a fillet. A stub lying on a neighbouring circle joins it. The older search over triangle centres only handles what's left.
- Small surfaces (under 0.2% of the model's size squared, in mm) give way to a fillet, so a ball that nearly fits a tight torus is replaced. A fillet also takes rows from a wall's cone or cylinder when it fits them at least 0.005 mm better, and joins a fillet beside it when its triangles lie on that surface.
- Triangles whose corners all sit within 0.01 mm of the surface may tilt up to 65 degrees from it, 35 otherwise.
- Thread cuts stay within one pitch of the thread's height. A screw surface runs on forever, so one edge at the top of a model could lie on it by chance, get cut and be pushed out to thread radius, notching whatever surface was there.
- A fillet may end in a sharp point where it meets another in a corner of a flat. The tidy pass used to trim such a point to a facet as a spike; it now keeps one whose corners sit within 0.005 mm of a cylinder or torus.
- Scraps go last. Before the build, loose triangles that aren't on a big flat join a curved surface beside them if they're within 1.75 times the tolerance, or any cylinder or torus if they're within 0.005 mm. The tilt rule above still applies.
- Rows on a ring: a coarse ring reads as two straight rows between grooves, each taken by a cylinder. A fillet cylinder whose triangles all sit within half the tolerance of a torus of the same tube radius goes to that torus.
- Cones around a known axis: a patch of 12 or more loose triangles is tried against every cylinder, cone and thread axis on the part, as a straight line of radius against height. This finds chamfers too coarse to grow from. A close line makes a new cone or cylinder.
- The tidy pass also keeps a sharp point on a cone within 0.035 mm.
- The fillet is fitted tangent to its flat. Points on the flat's edge give the centre line exactly, leaving only the radius, taken as a median so wall points beside it don't move it.
- Fillets never take triangles of a large flat (at the edge they are the same height), and run after the step where surfaces take back neighbours, for the same reason.
- A torus is drawn as an arc turned about its axis (OpenCascade reports a surface of revolution), because a plain torus has its seam on the outer equator, where a top fillet ends.
- Edges: a chain of points with a corner over 45 degrees is never a circle or smooth curve, since any three points in a zigzag sit on a circle. It goes point to point. Smooth curves are checked against their points and fall back the same way if they stray over 0.02 mm. Helix edges of 320 points or more skip that check.
- Straight edges on cylinders and tori get their surface curve drawn directly between their ends, as the automatic projection wanders near a tangent.
- Navigation: scroll and pan fix the pivot on the surface under the pointer, zoom can get as close as 0.1 mm, and the near clip follows the view distance.
- Thread outlines: an outline piece that is nearly straight is made exactly straight. Spline edges use at most 320 points.

#### Fit Curved Surfaces: sections, threads, broken rounds (OURS)
Stock OpenCascade has no mesh-to-surface recognition; all of this is in `meshsolid.cpp`. UI: the Fit Curved Surfaces command, no popup options.
- Slices along the part's long axis read arcs, runs of arcs become cylinders and cones (`sectionSurfaces`).
- `findBrokenCircles`: corners of a slice that share one circle, however many grooves break it. Most must be neighbours under 27 degrees apart, so a hexagon stays a hexagon.
- Threads: `Screw` surfaces, one per face of the outline; `cutAlongThread` cuts the mesh along the helices between them, then the fit reruns on the cut mesh.
- `tidy` keeps a strip's square end and only removes sharp spikes. Cones steeper than slope 3 are refused.
- Heavy keeps an imported mesh's own triangles for its body (`m_imported`), used by Fit; a saved, reopened or moved body falls back to meshing the solid, with poorer results.
- Screw surfaces space their rows evenly by length along the outline (`outlineAt`, `outlineShare`); unevenly spaced rows overshot and folded the surface, drawn as grey back faces.
- Heavy's drawing mesh sets DeflectionInterior and MinSize to 5x Deflection: 2.6 million triangles to about 100k on a threaded part, no open edges.

#### Body look
- Location: every body in the view.
- Stock: Qt Quick 3D PrincipledMaterial, lit by the lamp and a procedural studio sky.
- Changed: metal by default: metalness 0.85, roughness 0.28, probe exposure 1.1, sky texture High.
- Simple Look keeps metalness 0.08, roughness 0.42, as it has no sky to reflect.
- Removed: ambient occlusion. It traces the display triangles, so it drew blotches on fillets and triangles on cones that are smooth in the file.
- Not yet: appearance settings, with matte and metal choices as Fusion has.

#### Export (STEP)
- Location: Export in the command list, group File, model mode only. It opens a save dialog with STEP as the only format for now and the body's name filled in.
- Stock: OpenCascade's STEPControl_Writer, AsIs, units mm.
- Changed: revolved faces are written as splines (ShapeCustom::ConvertToBSpline, revolution only). A torus whose tube is wider than its ring, written as a revolved full circle, came into Fusion 360 dented inward. Rings that weren't dented go the same way, because a single face can't be swapped without breaking the shared edges.
- Ours: GeometryEngine::exportStep; the Lite engine says STEP needs Heavy. A name with no .step or .stp gets .step.

#### Coincident: a free shape slides over

**Problem:** Coincident between a corner of a 10-sided polygon and a corner of a square was refused with "That can't hold with the constraints already there", although nothing stood in the way. The polygon's Equal and Tangent constraints repeat each other, so the solver wandered for its 200 rounds and gave up. Smaller steps and firmer damping didn't help.

**Resolution:** When the solve fails, `SketchGeometry::slideTogether` slides the whole shape so the point lands, then solves again. It moves the second pick's shape, or the first's if that one can't move. It applies only to a shape that isn't tied to the other point, the origin, a Fix or projected points. Stock: none, ours. UI: Sketch > Coincident, no popup or options.

