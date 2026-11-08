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
- What it does: pushes sketch profiles, or flat faces on one plane, into a solid along the plane's normal. Fusion's extrude: one side, symmetric or two sides; each side's extent is a distance, up to an object, or through all; each side can taper; each side is Auto, Join, Cut, Intersect, New Body or New Component.
- UI: E in Model mode (in a sketch E stays Ellipse), or search: "extrude", "push", "pull", "pad", "boss", "cut"; its icon (a solid face with a box pushed up from it) shows in the search list and the history. Profiles or faces selected first are what it starts with. No side panel: rows of chips centred at the top of the view, a distance box on the profile and a drag arrow (`qml/ExtrudeTool.qml`).
- Popup: none. Rows: (1) Profiles: N selected ✕ (Faces / Areas when faces are in it; the chip lights while clicks go to it, clicking it sends them back there, ✕ clears them) / One Side / Symmetric / Two Sides / Distance per area / Done. (2) per side: Distance / To Object / All, then a Taper box (degrees, positive widens like Fusion). (3) per side: Auto / Join / Cut / Intersect / New Body / New Component. Clicks in the view: the first profile or flat face needs a plain click; after that Ctrl or Shift adds one or takes it away (Fusion), and a plain click on anything else (a point, edge, face, another sketch's profile, line or point, an origin part) makes it the one object to extrude up to, replacing any before it, and that side goes To Object; a click on nothing changes nothing. Ctrl or Shift on something that can't be extruded (an edge, a point, a curved face, a face off the first area's plane) is turned away with a hint. The To Object chip also waits for one click in the view (a face, edge, corner point, sketch point or line, a sketch area or an origin part); the distance box then holds the offset past it ("±"); the chip names it (To Face, To Edge…) with a ✕ that clears it and waits for another click. All shows the length it came to; clicking All again goes the other way if a body lies there. Dragging the arrow goes back to Distance from where it was. Esc while picking the object goes back to Distance. A cut or intersect reaching several bodies lists them as checkboxes in the browser.
- Interface hook: `extrudeOn(profile, placement, from, to, taper)`, `extrudeFace(body, face, placement, from, to, taper)` (a body's own face, exactly), `combine(a, b, op)`, `mesh(body)`
- Heavy (exact surfaces): `HeavyEngine::extrudeOn` in `src/engine/heavy/heavyengine.cpp`: `BRepPrimAPI_MakePrism`, or `LocOpe_DPrism` with a taper (`pushed`). A picked face of a body goes through `HeavyEngine::extrudeFace`: the face itself, copied and moved to the start, pushed the same way, so every edge and curve is the body's own (no sampling); its traced outline (`faceProfile`, within 0.001 mm) is only for the mesh estimates and the edges-moved try. Runs of sketch points on one circle (`Profile::centres`) are built as true arcs (`GC_MakeArcOfCircle`).
- Lite (triangles): `LiteEngine::extrudeOn` in `src/engine/lite/liteengine.cpp` (Manifold `Manifold::Extrude`, even-odd holes). No taper: the Document refuses a taper on Lite with a message.
- Engine differences: Lite turns arcs into straight segments and can't taper; it has no faces, so a picked face is pushed as its outline.
- Document side (ours): `readExtrude` (areas, sides, extents, taper), `extrudeReach` (To Object), `planExtrude` (Auto: points of the extrude tested against each body's mesh), `computeExtrude` (the solid work, off the main thread through `extrudeInBackground`), `applyExtrude` (bodies, history).
- Fusion differences: a plain click picks the object to extrude up to without choosing To Object first, and edges and points can be picked (Fusion: faces, planes and bodies only; owner's wish). To Object ends flat where the object lies (a face where the line through the areas' middle meets it, else its middle; an edge or sketch line at its point farthest from the profile's plane, so a slanted edge is reached all along, and the offset brings it back). Following the object when an earlier step is edited needs the history to be re-run, which OmaMake doesn't do yet; the object is kept in the step's spec for when it does. Fusion follows a slanted or curved face. An origin plane must be parallel and an axis must lie across. Symmetric with To Object goes the object's distance both ways. Start (Profile Plane / Offset / Object) and Thin aren't there yet.
- Changes (newest first):
  - 2026-11-07: Fuzzy profile is automatic: tried only when the exact extrude failed at a Boolean; the tick box removed; a note gives the accuracy when it was needed. See "Extrude: Fuzzy profile, tried automatically when the exact one fails".
  - 2026-11-03: a picked face is pushed exactly (`extrudeFace`); the outline is never moved as a whole any more (that changed the size); see "Extrude: a join back onto a face left a step".
  - 2026-11-03: a plain click picks the object (To Object), Ctrl or Shift the areas; an edge or sketch line is reached at its farthest point (was: where it passes nearest the middle). See "Extrude: a plain click picks the object".
  - 2026-11-03: the Profiles chip with its count and ✕, the To Object ✕, clicks that add instead of replace, and E working after an Extrude. See "Extrude: a click swapped the profile".
  - 2026-11-03: E key and icon; a selected profile is easy to click on a body's face, so selecting then pressing E works. See "Profile shading flickered and was hard to pick".
  - 2026-11-03: outline edges lying on the body's walls are moved 0.05 mm off them before the boolean (into empty space for a cut or intersect, into the body for a join), which leaves the same shape; faces left in one plane after a boolean are merged; a sketch on a flat face sits on the engine's exact plane. See "Extrude: a filled hole left a circle".
  - 2026-11-03: every untapered join, cut and intersect checked against an overlap worked out from the body's mesh; a result far off is redone with the outline moved 0.01 mm (then 0.05 mm) off the body's walls, and the note says so. See "Extrude: a cut of a projected outline did nothing".
  - 2026-11-03: Extent (Distance, To Object, All), Taper and Intersect added. A curved face clicked during Extrude is passed over instead of becoming an area that can't be pushed.
  - 2026-11-03: true arcs in the tool; booleans in parallel with oriented boxes; the solid work moved off the main thread; Auto worked out from meshes. See the entries under Plugin features.
  - 2026-11-02: Extrude tool, multi-body documents, `extrudeOn`.
  - 2026-10-29: `extrude(profile, height)` added.

#### Combine                                [BUILT-IN]   [OURS]
- What it does: joins, cuts or intersects two bodies.
- UI: Extrude's operation row (Join, Cut, Intersect). No combine command of its own yet.
- Interface hook: `combine(a, with, op, fuzzy)`, `looseness(body)`
- Heavy (exact surfaces): `HeavyEngine::combine` in `src/engine/heavy/heavyengine.cpp` (`BRepAlgoAPI_Fuse`, `_Cut`, `_Common`; changed: first run in parallel with oriented bounding boxes and non-destructive; every result checked for a valid solid and a volume that fits the operation; a bad one redone plainly on copies; faces in one plane merged within 0.01 mm, kept only if still valid; each attempt logged as an `engine:` line)
- Lite (triangles): `LiteEngine::combine` in `src/engine/lite/liteengine.cpp` (Manifold `+`, `-`, `^`)
- Engine differences: Heavy merges faces left lying in one plane after each boolean (`ShapeUpgrade_UnifySameDomain`, within 0.01 mm), and a result of the right volume that OpenCascade's check calls broken only because a few edges lie a hair further off their faces than their tolerance says is repaired (`BOPTools_AlgoTools::CorrectCurveOnSurface`, never past the loosest tolerance either input already has); Lite has no faces to merge or repair.
- Changes: 2026-11-07: `combine` takes a fuzzy value, used only on Extrude's automatic second try and only after the exact Boolean failed (see "Extrude: Fuzzy profile, tried automatically when the exact one fails"); `looseness(body)` added. 2026-11-03: edges just outside their tolerance repaired instead of the whole result thrown away (see "Extrude: a join back onto a face left a step"). 2026-11-03: faces in one plane merged after each boolean; the third try (one thread) dropped. 2026-10-29: added.

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
- Heavy (exact surfaces): `HeavyEngine::mesh` in `src/engine/heavy/heavyengine.cpp` (`BRepMesh_IncrementalMesh`, Delabella mesher, then the standard Watson mesher for any face Delabella left bare; within the model's size / 50000, kept to 0.01-0.5 mm, and 0.2 rad; smooth normals from each true surface)
- Lite (triangles): `LiteEngine::mesh` in `src/engine/lite/liteengine.cpp`
- Engine differences: Heavy will tessellate exact surfaces to a chosen accuracy; Lite already is triangles.
- Changes: 2026-10-29: added. Flat shading only for now (curved faces will need smooth normals). 2026-11-03: faces Delabella leaves without triangles are meshed again with Watson (see "A pushed face's walls didn't show").

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
- Interface hook: `Document::fillProfiles(target, sketches)`; Main.qml `drawProfiles()` (on `sketchesChanged` and the eyes), Model `objectName: "profileShading"` (blended, both sides, no depth writes, drawn with `qml/ProfileMaterial.qml`: each point pulled 0.1 mm plus 0.2 % of its distance toward the camera, so a profile on a face never fights the face); `Theme.sketchProfile`.
- Heavy / Lite: engine-independent (triangles from Manifold's triangulator, see Profile finder)
- Changes: 2026-10-30: added. Not yet: a different tint for the open sketch. Fusion's Show Profile switch is left out on purpose (owner: no use for it). Hovering and picking: see Selecting profiles. 2026-11-01: the wash could lag the sketch by one change (Toggle Construction left it shaded, then bare; a first Project left it bare), while picking was right. QML handlers run before C++ connections to a signal, so `drawProfiles()` read the old `m_profiles`; every sketch change now goes through `Document::sketchesEdited()`, which clears them before `sketchesChanged`.

#### Selecting profiles                     [BUILT-IN]   [OURS]
- What it does: resting the mouse inside a closed area lights just that area (the hover colour), and a click selects it (the selection colour); Ctrl-click or Shift-click adds it to or takes it from the selection, as with lines and faces. It is what Extrude will work on. Lines, points and dimensions come first: near a line the line is picked, near a point (a small circle's centre too) the point; inside an area away from them, the area. A body in front hides the profiles behind it; a profile drawn on a body's face wins over that face. In Sketch mode only the open sketch's profiles can be picked (only its lines can be, too); in Model mode every shown sketch's can. A selected profile stays selected while the sketch changes as long as there is still a profile at the spot it was picked (an area split in two keeps the half that was clicked); it goes when the area is opened up, or when its sketch is hidden. Dragging inside an area moves nothing. Esc in a sketch still finishes the sketch, as before. Fusion's tints are its own blue; here they are the theme's hover and selection colours.
- UI: in the view, on the sketch's plane.
- Popup: none.
- Interface hook: `Document::pickProfile(sketches, origin, direction)` (nearest along the ray, gives `{kind: "profile", sketch, index, u, v, depth}`), `Document::profileAt(sketch, u, v)` (find it again after a change), `Document::fillProfileParts(target, parts)`; `profiles::contains`; `Selection.profileSketches` (set by Main.qml `drawProfiles()`), `Selection.partAt` (a profile in front, or on a face within ProfileMaterial's pull, wins unless a point, edge or origin axis lies within 3 px of the cursor and no deeper than the profile; it beats the origin planes), `keepIn`; Models `objectName: "profileHover"` / `"profileSelected"` (ProfileMaterial). Profiles are found once per sketch change and kept (`Document::profilesOf`).
- Heavy / Lite: engine-independent
- Changes: 2026-11-03: picking a profile on a body's face no longer loses to the face's edges and corners or the origin planes (see "Profile shading flickered and was hard to pick"). 2026-10-30: added. Not yet: a box drag (window select) taking profiles, a profile's area in the info line.

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
- UI: hovering lights a point or line; click selects it; Ctrl- or Shift-click adds or removes; click empty space to clear. Double-click a line, arc, curve or point selects its whole chain: everything joined end to end through a shared point, a Coincident, or a point held on it (Point On, Midpoint), normal and construction each only along their own kind. Lines that only cross are not joined, though they can still close a profile. Circles and ellipses have no ends, so they select alone unless something is held on them. In Fusion a double-click selects a loop and stops at branches; here a branch is followed. Drag from empty space for a box (Fusion's window select): left to right, a dashed box takes what is wholly inside; right to left, a solid box takes anything it touches; Ctrl or Shift adds; Esc drops it. The end points of what is taken stay out of the selection (Fusion takes them too), so two boxed lines still count as two lines for Parallel and Equal; a point on its own is taken. Fusion also boxes profile areas; not here yet. While a drawing tool is on, the mouse snaps instead (see Snapping while drawing) and nothing is selected.
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

#### Selection: what a click may pick (sketch mode and model mode)  [BUILT-IN]   [OURS]
- What it does: a second tick-box list beside Display. Each row switches one kind of thing on or off for plain hovering, clicking, double-click chains and box select while sketching. A kind that's off is skipped, so what's behind it is found instead (switch Body Faces off and a click lands on the sketch's own lines, or the other way round, which makes picking a body's edge or point to project easier). Turning a row off also drops anything of that kind already selected. Every row is on by default and kept for the run.
- Rows: Lines (lines, arcs, circles, curves, text), Points, Constraints (the badges), Profile Areas (closed areas, for Extrude and the like later), Construction Lines, Projections (anything projected from the model), Origin (its point, axes and planes), a nest Components (not real yet, so its name is greyed) holding the nest Bodies (Faces, Edges and Points, the model's corner points) and Outside Sketches (greyed out: for projecting from an earlier sketch, later). Nests start folded; a bare arrow (no box: bright white in dark mode, very dark black in light mode) opens one. The Selectable menu sits at the bottom, so a nest's rows appear above it (`SketchDisplay.unfoldUp`, the default); a menu at the side would unfold downward. A nest's box shows a tick when everything switchable inside is on, a dot when only some, and clicking it switches all inside on, or all off if all were on. With Faces, Edges and Points all off the model is ignored altogether and hides nothing behind it.
- The sketch rows apply only when no sketch tool is running, so Trim, Extend, Dimension and the rest can still pick what they need; the Bodies, Faces, Edges and Points rows also apply while Project runs (`Selection.bodyFiltered`). A line, arc or curve counts as a Projection if it's held from the model, else a Construction Line if it's construction, else a Line.
- UI: the Selection button (was Selectable), bottom right while sketching; Display moved left of it (`qml/SketchDisplay.qml` is used for both, with a `title`, and rows with `enabled: false` greyed out). No search rows yet.
- Model mode (2026-11-02): the same menu, titled Selection, shows outside a sketch with Profile Areas, Origin, Components > Bodies > Faces / Edges / Points (the sketch-only rows are not shown and never block picking there). The switches are shared with the sketch menu.
- Select Through (2026-11-02): the last row in both menus, under a separating line (a row with `divider: true`). Off (default): hover, click and box select take only what the camera can see. On: hidden edges, points and faces can be hovered, clicked and boxed (`Document::pick(..., through)`, `SketchTools.selectThrough`).
- Interface hook: Main.qml `selLines`, `selPoints`, `selConstraints`, `selProfiles`, `selConstruction`, `selProjected`, `selOrigin`, `selFaces`, `selEdges`, `selBodyPoints`; `Selection.allow`, `Selection.filtered`, `Selection.sketchAllow()`; `Document::pickSketch(..., allow)`, `Document::sketchOutlines(sketch, allow)`, `Document::sketchPartClass(sketch, kind, index)` (1 line, 2 point, 4 construction, 8 projected).
- Heavy / Lite: engine-independent
- Fusion divergences: Fusion's Selection Filter is a separate panel with different names and works outside sketches; ours is sketch-only for now. Dimensions aren't a row yet.
- Changes: 2026-11-01: added. 2026-11-01: Body Faces, Body Edges and Body Points became Faces, Edges and Points under Bodies, and those the model-only switches that apply while Project runs. 2026-11-01: nests (SketchDisplay rows take `children`; folded, arrow, dot for some, parent sets all); Components > Bodies, Outside Sketches added. 2026-11-01: the row reads Points (was Vertices), to match Project's Edges / Points.

#### Click again to unselect
- Anywhere the selection is used (Selection.clickAt), a plain click on something already selected takes just that one off the selection, without Ctrl or Shift; a click on empty space still clears all. The constraint tools (Parallel and the rest) do the same with their picks. Not yet: the Dimension tool's own steps, and Model mode tools that don't exist yet will need to follow this rule. 2026-11-01.

#### Box select: what it leaves out
- Drawing: the dragged box is a plain tinted rectangle (dashed edge for a window, solid for a crossing). It was a Shape before, which did not show on screen; the logic was fine. 2026-11-01.
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
- What it does: left-drag a point (its lines turn and stretch; a circle's centre carries the circle at its size; the edge resizes it, unless its size can't change (a radius or diameter dimension, Equal to a dimensioned one, anything else): then the edge carries the whole circle, so grabbing anything always does something), a line (it moves whole), or anything selected (it all moves together). Drags snap the way drawing does (see Snapping while drawing): the dragged point, or for a line or a selection whichever of its points comes nearest something, snaps onto another point (a line's end, a rectangle's corner), the Origin, a line's middle, or anywhere along a line or axis. Let go there and it's held by Coincident (a point, the Origin, along a line) or Midpoint, in the same history step as the move ("Moved Point2 (Coincident)"), so one Ctrl+Z takes back both. A drag never snaps onto itself: its own points, the lines that move with them, those lines' other ends, and points already Coincident with a dragged one are left out. A snap only counts if the constraints really let the point get there; a constraint that's already there isn't made twice. Double-click a line, arc, curve or point selects its whole chain: everything joined end to end through a shared point, a Coincident, or a point held on it (Point On, Midpoint), normal and construction each only along their own kind. Lines that only cross are not joined, though they can still close a profile. Circles and ellipses have no ends, so they select alone unless something is held on them. In Fusion a double-click selects a loop and stops at branches; here a branch is followed. Drag from empty space for a box (Fusion's window select): left to right, a dashed box takes what is wholly inside; right to left, a solid box takes anything it touches; Ctrl or Shift adds; Esc drops it. The end points of what is taken stay out of the selection (Fusion takes them too), so two boxed lines still count as two lines for Parallel and Equal; a point on its own is taken. Fusion also boxes profile areas; not here yet. A drag changes size first, then angles and shape, and moves things last: a free circle's edge resizes it (`SketchGeometry::resize`: a passing radius hold, the rim alone first, then its neighbours); for points, first only the dragged points, the ends of their lines and the arcs they touch may move, with every other line holding its heading (so a rectangle's corner drag stretches its sides and keeps its turn; once its sizes are held, it turns), then the same without the headings, then the ring grows one step at a time (with headings first, then without). Fixed points never move. One history step per drag.
- UI: mouse, Sketch mode, no tool active. While snapped, the same marks and tag as drawing show: the target point lights up, a line's middle shows ▲, a spot along a line ○, and the tag by the mouse names it ("Point3", "Midpoint of Line1", "On Line4", "Origin", "On the X axis"). After the drop, the bar at the top says what holds it ("Moved Point2, held by Coincident").
- Interface hook: `Document::beginSketchDrag`, `dragSketch(sketch, moves)`, `dragSketchSize(sketch, arc, radius)`, `endSketchDrag(sketch, summary, hold)` (hold = {point, on: "point" | "origin" | "midpoint" | "line", target}; returns the constraint's name or ""); `SketchGeometry::drag`; QML `SketchTools.dragBegin` / `dragMove` / `dragEnd`, `snapNear(at, reach, geometry, skip)`, `dragSkip`, `dragSnap`
- Heavy / Lite: engine-independent
- Changes: 2026-10-30: size first, then angles/shape, position last (owner's order). 2026-10-30: the edge of any circle whose size is held carries it (tries the resize, falls back to moving; `SketchTools.moveFrom`). 2026-10-30: a sized circle's edge carries it (so a drag shows what's still free, as in Fusion). 2026-10-30: a circle's centre carries the circle; a tangent circle goes round the other. 2026-10-30: a lone circle dragged by its edge resizes; arcs move whole. 2026-10-30: snaps like drawing, and holds where it was dropped. 2026-10-29: added.

#### Project                                [BUILT-IN]   [OURS]
- What it does: Fusion's Project. Throws the model's shadow straight onto the sketch's plane as lines, arcs and circles, in a violet no other line uses. Whole Model: the outline and anything open right through it. Face: that one face's outline, flat or curved. A flat face seen edge on gives one line. Stays on for more clicks; the Done button or Esc stops. Projecting the same thing again says so and adds nothing, and corners shared with an earlier projection are one point.
- UI: search "Project" (Sketch mode; also "project geometry", "silhouette", "outline"), or P. Whole Model, Face and Edges / Points sit under the sketch bar; then click the model. In Face, hovering lights the face; in Edges / Points it lights the edge or corner under the pointer. The Selectable menu's Bodies, Faces, Edges and Points rows apply while Project runs (Bodies off ignores the model entirely; Faces off stops Face mode and lets Edges / Points reach through to edges and corners).
- Popup: none; the two choices above, and the guide reports what was made.
- Interface hook: `Document::projectIntoSketch(sketch, face)` (face -1: whole model), `Document::faceAt`; `projection::outline` in `src/document/projection.cpp`; `SketchGeometry::projectedPoints`, `tiedToBoth`; `SketchTools.projectWhat`; `Theme.sketchProjected`. History: a "shape" step, undone by snapshot. Saved as a `projected` flag on points.
- Heavy / Lite: engine-independent: it works from the drawn triangles (`Mesh`), joined by Manifold's `CrossSection` (Clipper2), in 64 x 64 cells then group by group: 16 s as one union on a threaded part, 0.2 s this way. A whole model takes only triangles facing one way.
- Reading the outline: a loop that stays on one circle is a Circle; otherwise, from its sharpest corner, the longest straight run or arc wins. An arc needs five points, one bend direction and no corner over 40 degrees. Tolerance 0.012 mm or size / 25000, twice how far drawn triangles may stray. An ellipse comes out as a chain of arcs within that tolerance. Arc centres sit exactly midway between their ends. Only a circle's centre shows; an arc's stays hidden, as a shallow one's floats outside the outline.
- Held: projected points act like the origin: nothing solves for them, dragging, resizing and Fix/UnFix don't apply. They count as fully constrained and form closed profiles.
- Fusion divergences: Fusion's choices are Specified Entities and Bodies, and it projects a face's edges; ours are Whole Model and Face and use the shadow, so a curved face gets a true outline. Holes that don't go through are not drawn. The link isn't live: a projection stays where it was made, as nothing can move a body yet; nothing is linked or re-linked. No choice to break the link; delete and project again.
- Edges / Points (2026-11-01): one edge or one corner of the model. A straight edge is a line, an edge on one circle an arc (counter-clockwise from a to b) or a full circle, any other curve a fit-point spline through up to 24 of its points (ends included); an edge seen end-on, or a corner, is a point. Same tolerance as above. Same dedupe as Whole Model: a corner already projected is reused, and projecting the same thing again says "That's already projected." `Document::projectPartIntoSketch(sketch, "edge"|"point", index)`, index into the model's edges / corner points (what `Document::pick` returns); `Selection.bodyEdgeOrPointAt`. Round edges are tested against a circle through three points, within twice the tolerance, so an oblique circle (really an ellipse) becomes a spline.
- Edge-on and huge circles (2026-11-01): a circle seen exactly edge-on lands flat as a line drawn out and back. A closed edge whose points all lie on one line is now a plain line between its two farthest-apart points. A circle is also refused (it becomes a spline) when its radius is over 4 times the model's size, or when a closed one doesn't span about its own diameter. Before this, such an edge came out as a circle with a radius over a million mm, drawn as a line to infinity.
- Not yet: a body picker for several bodies (the app holds one model). A line drawn directly between an existing point tied to the origin and an existing projected point isn't refused.
- Count (2026-11-01): while Project runs, a small badge to the right of the tool name counts what this run has put on the sketch: a whole-model or face projection is 1, so is each edge or point picked by hand (so a body projected line by line counts every line). A click that projects nothing new ("already projected") does not count. It restarts at 0 each time Project starts. `SketchTools.projectCount`, `SketchBar.toolCount`.
- Take back (2026-11-01): clicking something already projected in this run takes it off again (Whole Model, a face, an edge or a point), and the x beside the count takes everything of this run off. Each click or box is one step in the document's history, so taking one off is: undo all of the run's steps, then put the others back in order (`SketchTools.projectRetake`).
- Box (2026-11-01): with Edges / Points chosen, dragging a box projects every edge and corner point of the model it takes (window, dashed box: wholly inside, left to right; crossing, solid box: any touch, right to left), as one history step, honouring the Selectable Edges and Points rows. It does not look at what hides what: edges behind the model are taken too. Whole Model and Face say a box takes edges and points. `Document::bodyOutlines`, `Document::projectPartsIntoSketch`.
- Exact (Heavy, 2026-11-06 to 2026-11-07): `Engine::shadow` and `Engine::flatPart`; Heavy: `projectexact::shadow`, `projectexact::edge`, `projectexact::corner` in `src/engine/heavy/projectexact.cpp`; Lite: MISSING (keeps the triangle way above). Everything comes from the model's own edges and surfaces, never its triangles; target accuracy 0.00001 in (0.000254 mm), spline fits within `kProjectFit` (1e-5 mm).
  - Edge / corner: the edge's own curve laid flat. A line stays a line, a circle parallel to the sketch stays that circle, one at a slant is its exact ellipse, one edge-on is a line, anything else a spline within `kProjectFit`.
  - One flat face seen square on: its own wires laid flat in wire order (outer anticlockwise, holes the other way). Exact by construction.
  - Whole model / curved face: (1) every edge laid flat; (2) the outline of each curved face that folds over as seen from the sketch (OpenCascade hidden-line outlines, `HLRBRep`, ShowAll); (3) all of them cut wherever they meet; (4) each piece judged by stepping just to each side of its middle: outline when a line straight down meets the model on one side only; (5) pieces lying along one another count once; (6) the kept pieces chained into closed loops, each piece the exact curve it came from.
  - Side test (step 4): looked at from 10 x the local looseness down to `kSideStep` (1e-4 mm), never farther out than half the piece's length (or twice the looseness): a longer step could reach round a corner. Model on both sides at a step no finer than the looseness puts the piece inside. A side point in a crack narrower than the looseness (model nearer the piece and farther out, same side) counts as covered: a loosely made model only meets that well.
  - Copies (step 5): piece i is a copy of piece j when i lies along j's length (foot inside j, both ends along j), runs the same way (within 30 degrees), and either sits on j's shadow side with model between them, or is within the two pieces' looseness added together (then whichever side the hair puts it). Coincident, partly overlapping pieces are judged the same way (no "too close to tell" skip).
  - Does the model meet a line straight down (`meetsModel`): flat faces analytically (the height from the plane's equation, then the face's own (u, v)); other surfaces by `GeomAPI_IntCS`; then `FaceInside`. Faces that stand straight up (a flat face edge-on, an upright cylinder, a profile extruded straight up) are skipped: from above they are only a curve and cover nothing, and a line running along such a wall made `IntCS` crawl (a body with 9 upright spline walls: 759 s of side tests, now 0.25 s).
  - `FaceInside` (OURS, replaces OpenCascade's face classifiers here): each face's boundary curves on its surface cut into chords within 1e-7 mm on the model (`GCPnts_TangentialDeflection`, no finer than 2 x `Precision::Confusion`), each boundary loop walked in wire order (`BRepTools_WireExplorer`) with the gap between one edge's end and the next edge's start closed straight across; inside = an odd count of chord crossings along u, with v-bins for speed; within the fit of a chord = "on the boundary, can't tell". The counting line is moved by a fraction of the fit (never enough to change the answer) so it never passes through a chord's end: a circle's start and end differ by a rounding hair, and a line through them miscounts that loop (two such loops on one line cancel, so an even total can't catch it). Why ours: `IntCurvesFace_Intersector`, `BRepClass_FaceClassifier`, `IntTools_FClass2d` and `BRepTopAdaptor_FClass2d` all answered wrongly up to about 0.0027 mm from a long spline edge, ten times the accuracy asked for.
  - Tried and set aside (2026-11-07): building each face's flat shadow and merging them with OpenCascade's fuzzy General Fuse / pairwise `BRepAlgoAPI_Fuse` + `ShapeUpgrade_UnifySameDomain`, taking the rim as the edges with one face. On a loosely made model it left slivers lying over their neighbours, took over 60 s where the side test takes 2 to 25 s, and once threw from `gp_Vec::Normalize`. Removed from the code.
  - Checked by a probe over 274 test bodies (parts, threads, fillets, loose STEP imports) in several directions each, plus a loosely made speaker mount (452 faces, 1070 edges, edges loose by up to 0.05 mm) in 9 directions, 100 of its faces one by one and 600 of its edges: no piece with model on both sides or neither, no open end, the enclosed area matching a dense grid of straight-down lines. On a loose model, points closer to the outline than the face's own edge tolerance are not judged (its edge in space and its edge on its surface are only that close; both describe the face).
- Changes: 2026-11-07: Heavy Project made exact (see Exact above): `FaceInside` (own face classifier, gaps between edges closed, counting line kept off chord ends), upright walls skipped, copy rule (looseness of both pieces, no skip for coincident pieces), crack rule in the side test; the fuzzy-fuse trial removed. 2026-11-01: Edges / Points choice added. 2026-11-01: count badge beside the tool name. 2026-11-01: Done button. The shadow is grown and shrunk by half the tolerance to seal zero-width cracks between triangles, which read as lines inside the outline. 2026-10-31: added. 2026-11-01: no one step may take over 40% of an arc: a long straight with short bends at both ends passed as a bulging arc, on Speaker Mount.step an 11 mm edge bowed out 1 mm.

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

#### Linked sketch projection, Break Link, sketch rewind   [BUILT-IN]   [OURS]
- What it does: a sketch can project the lines, arcs, circles and points of an EARLIER sketch (Project, "Sketch Parts" or "Whole Sketch"; "Outside Sketches" in the selection menu switches picking them on or off). The projection stays linked: when the source sketch changes the projected geometry follows. Only some parts projected: new source geometry is NOT added. Whole sketch projected: new source lines, arcs and points are added, and a fillet or chamfer on a mapped corner comes in (the sync re-points a mapped line to the source line's current ends). Helper and construction flags are mirrored. Arcs project only between parallel planes; splines, ellipses and text are skipped. Projecting from the model body records a link too (source 0), but nothing syncs it yet (no Extrude).
- Sync rules: derived and repeatable (`Document::syncLink` from `sketchesEdited`), so undo and redo of either sketch converge. A projected item the user deleted stays deleted. Moved geometry is re-solved. Link data lives inside `SketchGeometry` (`Link` struct), so undo restores it with the sketch snapshot.
- Broken links (shown in the timeline, nothing is deleted): yellow = the geometry moved but the constraints can't all hold; red = a source line, arc or point the link follows is gone, or the source sketch is gone (red wins). The sketch row shows the worst issue of its links; hover shows the text. Better handling is planned after the owner's production testing.
- Break Link: only in the sketch history. Hover a linked projection step for 1.5 s: a button rises above it and can be clicked after 450 ms more; it hides 0.5 s after the mouse leaves. A themed yes/no card asks; yes adds a new undoable history step "Broke the link to ..." and keeps the geometry as ordinary geometry.
- Project runs and box select (2026-11-02): a Project run (start to Done) is ONE history step: `Document::beginProjectRun` / `endProjectRun` join the run's projection steps, keeping the first step's snapshot, with `links` and `parts` on the step; Break Link on it breaks all its links ("Broke N links"). A box drag now works in every Project kind (Edges / Points, Faces, Whole Model, Sketch Parts, Whole Sketch; left to right = wholly inside, right to left = touching) and, in the model with no sketch open, selects faces, edges and corners. `Document::faceOutlines()` gives each face's rim for the test. Select-through (hidden parts count).
- Box select modifiers (2026-11-02): Shift-drag adds the box's take to the selection as one batch; Ctrl-drag removes whatever the box takes from the selection (it never selects anything new); a plain drag replaces. Direction rules as before (left to right = wholly inside, right to left = touching). Ctrl-drag in Project un-projects the matching items. The box turns red for Ctrl. A Shift/Ctrl drag that starts on a sketch part draws a box instead of moving the part. `SketchTools.applyBox`, `boxMode`.
- Visible only (2026-11-02): box select (edges, points, faces, whole model) takes only what the camera can see; things hidden behind the model (an internal thread seen from outside) are not taken. `Document::hiddenFrom(points, eye, ortho)` casts a ray from each point to the camera against a lazily built bounding-volume tree of the mesh (cleared whenever the mesh changes); `SketchTools.seenOutlines` / `seenFaces` filter box hits with it (an edge is sampled at 9 points: a crossing box needs one seen sample in the box, a window box at least half). `SketchTools.selectThrough` (default false) is the plug-in point for a later Select Through switch. A box over the whole view costs about 0.3 s.
- Project run is one item (2026-11-02, supersedes the end-of-run merge above): `Document::record()` adds a Project run's later projections into the run's first history item (no separate steps, nothing merged afterwards); `foldProjectRun` words it, `endProjectRun` closes the run, `undoProjectRun` takes it back for a retake. The same thing is never projected twice into one sketch (quietly skipped, fitted curves included). The Ctrl-drag box colour avoids the accent's hue (`boxOverlay.removeTint`).
- Project run item appears at Done (2026-11-02): the run's item is stored with a `pending` flag and `history()` hides it until `endProjectRun` (Done, Esc, another tool, leaving the sketch) or any other recorded action commits it; saving strips the flag. Picks left out as already projected are counted in a second bubble beside the count. History entries for hiding and showing use the Browser's eye icon, and hiding then showing the same item with nothing between them (or the reverse) is removed from the history instead of added (`Document::setShown`).
- Icons: projection steps show a chain-link icon (faded once broken); Break Link steps show the same chain broken apart.
- Rewind: opening an earlier sketch (timeline or Browser) takes the document back to when that sketch was made. What was made later fades out over 250 ms (the same as the eye) and back in on finish; the camera waits until the fade ends, so the turn stays smooth. Model projection is refused while the body is from the future; later sketches can't be projected into an earlier one. Eyes of future rows are disabled.
- Timeline: a folded run of moves opens after a 1.5 s hover (folds 3 s after leaving).
- UI: Project popup choices: Whole Model, Face, Edges / Points, Sketch Parts, Whole Sketch.
- Interface hook: `Document::projectSketchIntoSketch(sketch, source, parts)`, `breakLink(sketch, link)`, `earlierSketches(sketch)`, `rewinds(sketch)`, `futureKeys`; history entries carry `link`, `break_link`, `link_broken`, `issue`, `issue_text`.
- Heavy / Lite: engine-independent
- Fix 2026-11-01: edge arcs projected with Edges / Points ran the long way round (a fillet showed as 270 degrees, like a full circle); the direction now comes from the edge's own points, and an edge that folds back on itself becomes a curve instead of an arc.
- Changes: 2026-11-01: added. Tested pass the same day: a whole projection after a parts projection shares the existing items instead of duplicating them; a link with every point fixed is judged by its worst residual (< 1e-3) so single-precision noise doesn't show a false yellow; the Outside Sketches switch now also works while Project is active.

#### Undo and the Timeline                  [BUILT-IN]   [OURS]
- What it does: one history for the whole design. Every action (a sketch, a line, a constraint, a drag, a deleted constraint, showing or hiding something) is a step; Ctrl+Z takes back the newest, one at a time, in Model and Sketch mode alike. Steps done inside a sketch belong to that sketch. Ctrl+Y redoes (owner, 2026-11-07): it puts back the newest step Ctrl+Z took back, exactly as it was, and any number can be redone in turn; doing anything new after an undo ends what can be redone (as in Fusion).
- UI: Ctrl+Z, Ctrl+Y (also from inside a tool's typing box: it leaves the box first, as Ctrl+Z does). The Timeline along the bottom shows one dot per step (hover for its name): the design's steps outside a sketch, only that sketch's own steps inside one. Double-click a sketch's dot to edit it. The tab in the middle of the bottom edge hides it.
- Icons and runs of moves (2026-11-01): a step can have an icon of its own instead of a dot. So far only a move (dragging a point, line, arc or curve): a cross with an arrow head at each end, drawn in the sketch's green. Later the same for the other steps and for the design's row. Moves done one after another would swamp the row (a drag is one step), so two or more in a row fold into ONE move icon with a count badge. Hovering it opens the run out into its separate moves (hover each for its name); the run folds again 3 seconds after the mouse has left it. A single move stays a single icon. Nothing is removed from the history: Ctrl+Z still takes the moves back one at a time. Not yet: clicking a step to go back to it.
- Popup: none.
- Redo, how (2026-11-07): each undo first keeps the whole document as it is (`Document::Redo`: components, bodies, sketches, hidden rows, open sketch, parameters, the kept solids of extrudes and fits, the imported file). The solids and meshes the undo lets go of are parked in it instead of released, so a redo puts everything back at once: nothing is built or meshed again, an extrude included. Redo restores that state and the step's history entry; the step can be undone again afterwards. `record` (anything new), cancelling a Project run and opening a file end what can be redone and release the parked solids (`clearRedo`). Window: redoing a sketch's start or Edit Sketch goes into it, redoing a finish leaves it; sketch changes and showing or hiding fade, as undo does (`Main.qml` `redoLast`). Test: `history/tools/probes/suiteredo` (13 checks: sketch start, rectangle, circle, finish, extrude new/cut/join, hide, edit, finish again, each undone and redone twice and compared whole; 6 back then 6 forward; a new step ends redo; Ctrl+Y bound).
- Interface hook: `Document::undo()`, `Document::redo()`, `Document::redoNext()`, `canRedo`, `Document::record(action, summary)`, `Document::history`; `qml/HistoryTimeline.qml` (`items` folds the runs, `openRun`, `foldDelay`, the `Canvas` move icon)
- Heavy / Lite: engine-independent
- The marker (2026-11-07): a line with a ball on top between the Timeline's items. Drag it left and right to move the design back and forward through its history; let go and it settles on the nearest gap (140 ms). Items behind it are greyed and kept; moving it forward puts them back as Ctrl+Y does (nothing built again: every body's mesh stays cached while any step can show it). Part way across an extrude, the extrude is drawn part done in 3D from any view (`RewindMaterial`: the before and after shapes split along the extrude's direction over its areas' box, a glowing band where they meet). A step made while the marker is back blocks moving forward and saving until it is taken back with Ctrl+Z (inserting it with a rebuild is not there yet). Not yet: saving the steps behind the marker in the file.
- The marker inside a sketch (2026-11-07): on the open sketch's own row, in the sketch's green, over the steps of this time in the sketch only (going further back would leave it). The sketch stays open. Part way across a step the sketch is drawn part way to how that step leaves it (`Document::setSketchTween`, eased): new lines, rectangles, circles and curves grow from where they were started (a shape drawn on from an existing point grows from that point, else from its own first point); what a constraint or dimension moved glides there, its dimension and the shading following; what a step took away shrinks back. Text simply appears. Finish Sketch or Edit Sketch with the marker back brings every step behind it back first. A sketch opened again (Edit Sketch): the marker still moves over all its steps. The design can't go back past that opening without leaving the sketch, so before it the sketch is only drawn as it was (`Document::prepareSketchPast`, `showSketchPast`), lines, dimensions and shading alike; the document stays at the start of this opening.
- Sketch states in the background (2026-11-07): what the marker needs inside a sketch is worked out from the history's own records, never saved in the file and never waited for. A walk starts from the sketches as they are and takes the steps back one by one, newest first, from each record's undo data (no engine, no document undo), in slices of at most 4 ms between frames (`Document::walkSome`, a 12 ms timer), so drawing, sketching and extruding carry on meanwhile. Each step keeps the sketches it changed as they were just before it; that depends only on the steps before it, so a new step on top only adds its own and nothing already known is worked out again. It works the same for a sketch made this session, a file just opened and a sketch opened again. Shape tools (Trim, Offset, Mirror, deleting, and the other tools that change many things at once) keep a small `before` diff in their record (what they changed, as it was), so they too can be taken back after opening a file; a file saved before this stops the walk at the oldest such step. The open sketch's Timeline row shows a translucent fill from the newest item back as far as the walk has got; the marker goes only where it is filled; faint while it grows, stronger when it covers the whole row. - The design's steps of an opened file, built again in the background (2026-11-07): a file keeps only each body as it is now and each step's record, so the marker couldn't go back over a step made before the file was opened. Now, once the sketches' states are known, a second document with its own engine, on a low-priority thread, makes every step again from its record, oldest first (sketches as they were from the walk above; components, hiding and showing, extrudes by their spec), and keeps the bodies after each step that changed them. The bodies it ends with must be the file's own (matched by name and component), else nothing is used and the reason goes to the log. The earlier solids come back to the window's engine a few at a time (each slice at most a few ms, never while an extrude or import is running) with their meshes, so nothing is meshed again. From then on the marker and Ctrl+Z go back over the file's steps as over this session's: a step that changed bodies goes back to the rebuilt design before it, any other step by its own record. Nothing is saved. The design's row shows a faint fill growing from the oldest end while it builds; the marker can go back only over this session's steps until the whole row is filled. Extrude records now also keep which body became which solid (`swaps`), so a later step that names a body (Skip, hiding it) can be followed. Imports are built again from the imported solid: the body itself when nothing has changed it since (it's in the file), the file's last import (`body_file`), or, only when neither holds it, a copy kept in the import's own record (`solid`, written at save). Fit Curved Surfaces works from the triangles a mesh came in as, which a file doesn't keep, so the rebuild takes the fitted body as it was made, found the same way (its record now has `body`). Every new kind of step must be built again here too, with a reopened-file test (owner, 2026-11-07: "as we add more features, the ability to scrub needs to be part of that build"). Opening a file now also carries every body id in the records (made, swapped, skipped, hidden) over to this window's ids; a body gone before the save gets its id negated, so it never stands for one of this window's.
- Gliding while sketching (2026-11-07): whatever moves the open sketch other than the mouse (a constraint, a dimension, Ctrl+Z, Ctrl+Y) glides to its new place in 220 ms (at most 250 ms) instead of jumping (`Document::takeLiveTween` / `liveTween`, `Main.qml` `liveGlide`). Dragging is followed at once. A new change during a glide starts from where the glide is.
- Interface hook (marker): `Document::rewindTo(count, inSketch)`, `future`, `presentCount`, `futureStale`, `rewindStep`, `fillBodies`, `setSketchTween`, `prepareSketchPast`, `showSketchPast`, `takeLiveTween`, `liveTween`, `holdLive`; `qml/HistoryTimeline.qml` (`markerItem`, `countAt`, `firstMovable`, `slotX`), `Main.qml` `rewind`, `qml/RewindMaterial.qml`, `qml/shaders/rewind*.{frag,vert}`. `sketchReadyFrom`, `sketchWalkDone`, `modelBuiltTo`, `modelReadyFrom`, `modelBuildDone`, `buildWhy`, `startBuild` / `loadSome` / `builtAt` / `applyBuilt`, `walkedSketchPoints` / `shownSketchPoints` (tests); `HistoryTimeline.qml` `readyCount`, `readyDone`, `readyItem`, `timelineReady`. Tests: `history/tools/probes/suitetimeline`, `suitesketchscrub` (19 checks, moves at most 5 ms), `suitemarkermouse` (real mouse drags, a sketch opened again), `suitesketchwalk` (a saved file opened again: the walk while the app keeps drawing, every state checked, the marker dragged back; an older file saved again unchanged), `suitemodelbuild` (an opened file's design row: rebuilt, every step back and forward checked, a mouse drag), `suitedemoreopen` (three models, 19 extrudes, opened again), `suiteimportbuild` (STEP and STL imports, a cut, Fit Curved Surfaces: opened again, scrubbed, Ctrl+Z / Ctrl+Y all the way).
- Changes: 2026-11-07: an opened file's design steps built again in the background (the marker goes back over them). 2026-11-07: sketch states worked out in the background (opened files too) with the Timeline fill. 2026-11-07: the Timeline marker, in the design and in a sketch; sketch glides. 2026-11-07: Redo (Ctrl+Y). 2026-10-30: deleting constraints is undoable. 2026-10-29: added.

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
- Upright walls (`regularize`, 2026-11-05): a fit reads the mesh's float noise and uneven top and bottom triangle rows as a lean. On the test speaker mount, every round base wall came out leaning 0.0025 to 0.40 degrees, though its STL corners sit on one upright circle (r 15.825 to 0.000001 mm). Then a cut and a pull back left a line where the walls met. After fitting, biggest first, each plane, cylinder and cone tries the world axes and directions already settled, within 1 degree (or square to one of them). A cone tries to be a cylinder, and a round wall tries a bigger wall's axis line. Then side-by-side surfaces that are one become the very same surface, and a round touches the flat it blends into exactly. Each try is kept only if every mesh point of the surface still lies within the fit tolerance, so a real draft stays: the test part's prongs keep their 0.4000-degree taper, which their long triangles show exactly. A surface that touches a thread is left as fitted, because the mesh was cut along the thread against it. Turned upright, the 45-degree lead-in cone at the test part's bore no longer met the thread, and the solid came out open with a 15 mm gap. Result on the test part: one valid solid, all 46 cylinders exactly upright, outer wall r 15.825000, volume 35461 mm3 (Fusion 35444.5; before this fix 33636). Cut to 1.27 mm under the top and pulled back: no edge at the cut height, and 451 of 498 faces identical; the rest are the thread bore, which the profile fills.
- Short-wall rule (owner, 2026-11-05): a wall under 25 mm (1 in) long that is under 0.5 degrees off upright, square, or a plain cylinder is made exactly that, even when its triangles show the lean. The length is measured the way the wall leans: up a round wall's axis, or along a flat in the direction it tilts. The build tolerance grows to cover how far such a wall then sits off its points. Walls 25 mm and longer keep a lean their points show. Tested on drafted blocks (30 mm square): 20 mm tall at 0.3 degrees comes out upright; 20 mm at 0.7 degrees, and 40 mm at 0.3 or 0.7 degrees, keep their draft. The speaker mount is unchanged (base walls already upright, the 64 mm plug keeps 0.4 degrees).
- Known limit, found while testing the rule (older than it): `sectionSurfaces` slices only along the mesh's longest spread. A squat round part (a 30 mm post under about 21 mm tall) is sliced across instead, so a slightly drafted one (0.2 to 0.5 degrees) is never found and stays 96 facets; the patch fitter misses it too. An undrafted one is found. Not fixed yet: slicing along each of the three main directions would find it, at up to three times the cost of that stage.

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

#### Circumscribed Polygon: even side counts stay regular

**Problem:** An even polygon held by Equal sides and Tangent to its helper circle could still swap long and short corners, as a square leans into a rhombus. Horizontal on one side of a 10-sided polygon left corners at 40.07 and 22.90 mm from the centre.

**Resolution:** A new even polygon gets one Rotation constraint between two neighbouring corners about the centre (`polygonSpec`). Polygons already drawn don't get it, so redraw them. UI: Sketch > Circumscribed Polygon, no new options.

#### Solver: short-step retry

**Problem:** The solver heads straight for the nearest answer. A big turn of a shape whose constraints repeat each other wandered until it gave up, so Horizontal on a 10- or 12-sided polygon was refused.

**Resolution:** `solveWeighted` tries again in steps of at most 2 mm, each from where it has got to. Refusals take about twice as long. Coincident skips this at first (`directOnly`), because it can bend an older polygon: it solves directly, then slides a free shape, and uses the retry last. Stock: none, ours.
- Project highlight (2026-11-02): while the Project tool runs, picked edges and corner points are drawn thick and in front (large negative depth bias), picked faces and the whole model are shaded translucent purple, so what will be projected is clear before Done. `Selection.projectPicks`.
- Project highlight size (2026-11-02): the thick edge highlight keeps a pixel thickness that follows the square root of the zoom (0.3x to 1.2x of the base), so zoomed out it stays in proportion to the model; it is rebuilt shortly after zooming or turning (at most every 40 ms).
- Coincident picks (2026-11-02): a pick whose outline lands on lines this Project run already made (far and near edges seen end-on) is still taken, highlighted and counted; it adds no new line. Only a pick an earlier run of the same sketch already put on it is left out, and only those count in the skipped bubble.
- Edge picking (2026-11-02): among edges about equally near the pointer, the one nearer the camera wins (matters with Select Through).

- Performance with many Project picks (2026-11-02): the highlight is built when the picks change and once after a zoom stops (not on hover, pan or orbit); highlight edges use lean 5-sided tubes with small balls only at the ends (`fillParts(..., lean)`); sketch-line redraw on zoom is throttled to 50 ms.

- Faster Project picking (2026-11-02): faces are projected in one pass (`projectFacesIntoSketch`: one scan, one history entry, one redraw) instead of one by one; choosing Project again ends the open run so each run is its own history item; selected parts are rebuilt only when the selection changes, and, for big selections, shortly after a zoom or turn stops.
- Whole-body selection (2026-11-02): double-clicking the model in the 3D view selects that body only (never its component); double-clicking a body row in the tree selects that body, and a component row selects every body inside it and its nested components. Not while sketching.
- Tree underline (2026-11-02): rows holding something selected (a body, or its face, edge or point; a sketch or profile) get a dotted underline under the name, and so do the components above them, but not the top design.

- Constraints from the search menu now work on whatever is already picked: parallel, collinear and concentric follow the first pick; perpendicular and tangent take exactly two; equal follows an already dimensioned item, else the first pick; horizontal/vertical uses the majority orientation; coincident joins points to the first (or origin) or puts them on one curve; midpoint takes a point and a line; symmetry takes three picks.
- Sketch Dimension on two circles or arcs gives a centre distance; double-clicking a model face or edge while sketching selects the whole body.

- Every constraint has a small symbol (one shared icon file): on its badge in the sketch, left of its name in the function search, and as its step in the sketch history timeline. Dimensions, angles, Fix and Unfix have symbols too.

- The constraint symbols in the function search are drawn half again as large.

- Every sketch tool has its own small icon (and its own for each kind: centre rectangle, 3-point circle, tangent arc, the slots, the chamfers and so on) in the function search and on its step in the sketch history timeline. The search always leaves room for the icon so names never shift. Steps made by a shape tool now record which tool made them.

- Text is placed with two clicks that make a box; it wraps to the box, can be fit to it, and has font, height, bold, italic and left/centre/right and top/middle/bottom alignment. The font choice lists the families installed on the system, each shown in its own face. A selected text can be edited afterward with the same fields; each edit is one undoable history step.
- Text on Path writes the words along the lines, arcs, circles and curves picked, end to end (a square's four sides, for example), with left, centre or right justification and a Flip option. The text follows its path when the path is moved or edited.

- The font list scrolls on its own without zooming the view behind it, and Text on Path takes a whole joined shape from a single click (its first-clicked side is where the text starts).

- Text on a path can be dragged along its path (also around a closed shape); the justify (left, centre, right) is then measured about where it was let go, and Flip keeps it in place. Letters that fall across a sharp corner are placed whole on the side their middle is nearer to, instead of being stretched.
- Text can have several lines: Enter in the text field starts a new line (Ctrl+Enter finishes), and double-clicking a text, in an open or a finished sketch, opens it for editing.

- Dragging a text on a path works while the Text on Path tool is still on, not only after leaving the tool.

- A text on a path slides when any of its letters is pressed and dragged, with the tool on or off and whatever else is picked; a press within a third of the letter height of a letter counts as on the text.

- The text option box holds real lines: Enter starts a new line in the box, Ctrl+Enter finishes, and the box grows with its lines.

- Text on Path: the words can be dragged along the path before they are finished (the first drag writes them); several lines stack on the path, each justified on its own.
- Text on Path: additional lines stack away from the path on the text's up side (last line on the baseline); Flip mirrors this automatically.
- Text edit panel (all text properties) also shows while a text tool is active with nothing gathered and one text selected; the tool options panel hides then. Shortcut Enter-to-confirm is disabled while typing in that panel.
- History rule: an edit session of one item (e.g. typing a text) is one history item. `Document::editSketchText` updates the newest item while the same text stays selected (`closeTextEdit()` ends the session); any other recorded action also ends it. One undo takes the whole session back. New live-editing panels must follow this.
- Text edit session: edits and slides of the same selected text share ONE history item (`Document::recordTextEdit`), ended by selecting another item or any other recorded action.
- With Text / Text on Path active, clicking a written text selects it (it is never a path to follow), so its edit panel stays.
- Text edit session is owned by the document only (same sketch + same text + newest history item); UI visibility changes of the text panel never end it.
- Text tools' option panels are two centered rows (words/font/size; then switches and choices) with a Done button; Done writes or puts down the text. Clicking away puts a picked text down.
- Text tools are one-shot: confirming writes the text, ends the tool and selects the new text for editing. Each start has an empty Text box, and after a path is picked the cursor moves into it so typing never triggers shortcuts. A written text is never a path for another text.
- Finishing a text (Done or confirm) leaves it deselected, in its normal finished look; double-click re-opens it for editing.
- LOCKED (owner-approved): the text-tool finish logic above (one-shot, empty start, cursor in the Text box after a pick, Done = finished and deselected, double-click to reopen). Keep it unchanged and reuse the same pattern for similar tools.
- Driven dimensions: a new dimension whose measurement the existing constraints already fix (no extra rank in the constraint Jacobian) is stored driven: the solver ignores it, it shows the live measured value in brackets, cannot be edited, and is saved/loaded with a driven flag.
- A newly added driven dimension shows a short fading toast beneath it explaining it is driven and cannot be set by hand; driven dimensions use a dotted border and a distinct text colour.
- Curvature (G2) constraint: joins a spline to a line, arc or another spline where their ends meet, so they leave the join in opposite directions (same tangent line) with the same curvature vector; the spline moves and the other part stays. Refused when the ends do not meet, the parts fold back over each other, or the constraints already forbid it. It is saved, undoable, shown as a badge on the joint, and removed when either part is deleted.
- Parameters: a document-wide list of named values (name, unit, expression, comment). Expressions support numbers, units, + - * / ^, parentheses, functions, constants and other parameters; they are evaluated in base units with length/angle tracking, so unit mismatches, unknown names, cycles and non-positive dimensions are refused with a plain reason. Names may start with a digit or contain unit text, but not be a bare unit word or look like a value. A dimension can hold a formula; it re-evaluates whenever a parameter changes, on load and on undo. Applying a list is one history item.
- The dimension value box takes formulas, and once the text stops being a plain number it suggests matching parameters; a formula dimension shows "Fx" before its value with a hover tip.

- Parameters can be exported to and imported from spreadsheets: Import and Export buttons sit left of the Automatic Compute checkbox. Export defaults to OpenDocument (.ods) and can write CSV (Fusion 360 layout: Name, Unit, Expression, Value, Comments, Favorite). Import reads .ods, .csv and .xlsx and merges by name in one undoable step.
- The delete X in the Parameters table is larger and red.
- Parameter import detects the file type from its contents (a spreadsheet saved with the wrong ending still reads), and refuses binary files without adding anything.
- Parameters use a drawn "fx" icon in the function search and on parameter steps in the model history.
- A Parameters window session is one history item however many edits are made; no item if nothing changed or everything was reverted.

- A document holds several bodies. Each body keeps its own solid and name and belongs to a component; the view mesh is the merge of every shown body, with maps from merged faces, edges and points back to their body. Each body is saved as its own solid.
- Engine interface additions: a placement (origin and two axes) and `extrudeOn(profile, placement, from, to)` that builds a prism on any plane; `volume(body)`; `gap(a, b, search)` (zero when bodies overlap or touch); `faceProfile(body, face, placement, profile)` for planar faces (heavy engine only).
- Extrude: picks sketch profiles or coplanar faces; per side (One Side, Symmetric, Two Sides) the operation is Auto (new body, join or cut by overlap and contact tests), Join, Cut, New Body or New Component, chosen independently per side. A cut can be limited to chosen bodies. The whole extrude is one history item; undo restores the replaced solids while the session lasts (not after reopening the file).
- The extrude preview is drawn in front of everything with a flat-colour material: blue for additions, red for cuts, with the exact removed region computed after a short pause so dragging and orbiting stay smooth.

- Extrude tool: with nothing picked it shows a prompt and auto-picks a lone closed profile; a preselected profile or face is used; the drag arrow has a minimum on-screen length.

#### Extrude: froze on start

**Problem:** Starting Extrude on a sketch projected from a threaded body froze the app. Picking the only profile went through `sketchProfiles`, which also hunts for gaps in the outline.

**Resolution:** `Document::profileSpots` gives each cached profile's middle, with no gap search. Extrude now starts in about 4 ms. Stock: none, ours.

#### Extrude: Auto worked out from meshes

**Problem:** Auto (new body, join or cut) intersected the extrude with every body to decide. On a threaded body this took 3.4 s, and over a minute at times, every time the distance changed.

**Resolution:** `planExtrude` tests points inside the profile at five depths against each body's mesh (a ray's crossings, odd means inside). A point inside means Cut; a point just behind the start inside means Join. It takes about 3 ms. The red cut shape is the extrude itself, no longer the exact removed part. Stock: none, ours.

#### Extrude: the arrow wouldn't drag

**Problem:** The arrow list was rebuilt on every view move and every distance change, so the arrow being dragged was destroyed under the mouse. A drag also measured from the profile, so a short distance jumped on the first move.

**Resolution:** The arrows are counted, not listed, so they stay put. A drag counts from where it was grabbed. About 10 ms per move. Stock: none, ours.

#### Extrude: true arcs in the tool

**Problem:** A cut through the threaded base took 42 s. Sketch arcs reach the engine as points, so the extrude's round sides were hundreds of flat faces, each cut against the thread.

**Resolution:** `Document::arcCentres` marks each profile point with the centre of the arc it lies on. `HeavyEngine::extrudeOn` turns each run on one centre into one true arc (a whole circle as two halves). The booleans also run in parallel with oriented bounding boxes. The same cut now takes 0.5 s. Stock-changed: `BRepAlgoAPI_BooleanOperation` settings.

#### Extrude: the window stays live

**Problem:** The solid work ran on the main thread, so the window stood still until it was done. After it, the new body was meshed a second time on the main thread, because swapping the body redrew it before the mesh made off the thread was stored. That was 5.6 s for a tapered cut.

**Resolution:** `Document::extrudeInBackground` runs `computeExtrude` (solids and meshes) on a thread, with a working card at the bottom. `applyExtrude` stores the meshes before swapping bodies. Undo, save, open and another extrude wait for it. Stock: none, ours.

#### Extrude: Extent, Taper and Intersect

**Problem:** Extrude had only a typed distance. Fusion also extrudes up to a picked object or through everything, tapers, and intersects.

**Resolution:** See the Extrude entry above. Taper uses OpenCascade's `LocOpe_DPrism`, which measures height along the slanted side (we pass height / cos(angle)) and narrows for a positive angle (we flip the sign to Fusion's). A tapered symmetric extrude is two prisms, each tapering away from the profile. Timings on the threaded base, with the checks below: To Object 0.8 s, All 0.8 s, Intersect 1.5 s, Join 1.3 to 1.8 s, Join with taper 1.5 s, widening taper cut 1.6 s. A narrowing taper cut through the thread comes back as a broken solid and is refused (see KNOWN-ISSUES.md). Stock: `LocOpe_DPrism` (links TKFeat). The rest is ours.

#### Extrude: Join made the body vanish

**Problem:** Joining an extrude of the whole projected outline (outer wall with finger notches) up to a face left the body in the browser with nothing to draw, and hiding and showing it did nothing. The fast boolean (parallel, oriented boxes, non-destructive) reported success and returned an empty solid, which replaced the body. The same join could come back whole on another run. A narrowing taper cut, earlier reported as working, was also returning a broken solid.

**Resolution:** `HeavyEngine::combine` checks every result: a valid solid (`BRepCheck_Analyzer`), with a volume that fits the operation. A bad result is redone plainly on copies, with threads, then on one thread. Copies are needed because plain mode may adjust tolerances in its inputs, and the old solid is kept for undo. If nothing passes, `computeExtrude` leaves the body as it was and says so. Each attempt is logged as an `engine:` line in the window's log. The failing join now takes 2 s (quick try refused, plain one used). Stock-changed: `BRepAlgoAPI_BooleanOperation` settings and checks.

#### Extrude: a cut of a projected outline did nothing

**Problem:** Cutting an extrude of the whole projected outline (the body's own outer wall, notches and all) up to a face removed 18 mm³ of the 13,000 it should have, and reported success. Every try passed the volume check, because a cut that removes almost nothing still "fits" a cut. Plain and one-thread runs gave the same, and an intersect of the same shapes gave nothing. Moving the extrude's sides off the body's walls fixed it; moving only its end off the base face did not. OpenCascade can't settle sides that lie right on sides, on a body with loose (0.063 mm) edge tolerances.

**Resolution:** Before the solid work, `Document::estimateExtrude` works out how much of each body every straight area overlaps, from the body's mesh: columns along the extrude through a 48 × 48 grid inside the profile, each adding up its lengths inside the body (`overlapEstimate`, 0 to 60 ms). `computeExtrude` checks each join, cut and single-area intersect against it, within a tenth of the extrude's volume. A result far off is redone with the outline moved 0.01 mm, then 0.05 mm, off the walls (`offsetProfile`: out into the air for a cut or intersect, in into material already there for a join; arc points move along their radius). The note says how far it was moved. If neither comes right, the body is left as it was and the note says so. Each step is logged as an `extrude:` line. The outline cut now removes 13,115 mm³ in 2.2 s (1.5 s wrong try, 0.7 s moved one). Tapered extrudes aren't checked this way yet. Ours.

#### Extrude: a filled hole left a circle

**Problem:** Joining the projected outline to fill the speaker mount's thread worked, but the base kept a circle where the hole had been, plus a dozen hairline faces along its rim. The sketch had been placed from the face's triangles, whose average sat 0.0001 mm above the real face, so the extrude's bottom was a separate face 0.0001 mm off the base. OpenCascade's own face merge after a boolean only joins faces that match exactly. With the sketch on the exact plane, the plain boolean came back broken instead, and moving the whole outline 0.01 mm off the walls still failed; it took 0.05 mm, after 6 s of failed tries.

**Resolution:**
- `GeometryEngine::planeOfFace` (Heavy: the face's `gp_Pln`; Lite: none). `Document::facePlane` uses it for a face that is a true plane: the sketch sits exactly on the face, and a merged face whose edge triangles tip slightly still counts as flat.
- `HeavyEngine::combine` merges faces left in one plane after a boolean (`ShapeUpgrade_UnifySameDomain`, 0.01 mm, 1e-4 rad) and keeps the merge only if the result is still a valid solid of fitting volume. 0.15 to 0.6 s per boolean. The third try (plain, one thread) was dropped: it never came out different and cost up to 5 s.
- `SheetView` files the body's triangles under a grid over the profile; `overlapEstimate` and `wallEdges` both use it. `wallEdges` marks each outline edge whose middle, half way up, has the body on one side and nothing on the other, and which side is empty (0.03 mm either side). `moveWallEdges` moves only those edges 0.05 mm toward the empty side (cut, intersect) or the body (join); the other edges stay exact, so the shape is the same and no note is needed. Points between a moved and an unmoved edge slide along the unmoved one.
- Order in `computeExtrude`: with edges on walls, moved edges first, then as drawn, then the whole outline 0.05 mm (which does change the size, and the note says so). Without, as drawn, then the whole outline 0.01 mm and 0.05 mm.

The fill now leaves one flat base face: join 1.4 s, the outline cut 0.95 s, 0.03 to 0.04 s of mesh work before them. Stock: `ShapeUpgrade_UnifySameDomain`, `BRepAdaptor_Surface`. The rest is ours.

#### Profile shading flickered and was hard to pick

**Problem:** A sketch drawn on a body's face (the projected outline on the speaker mount's base) had its shading flicker against the face as the view turned: triangles of wash and face showing through each other. `depthBias` on a Qt Quick 3D Model only changes the drawing order, not the depth test, so it never stopped this. The same profile was hard to click: the face's many edges and corners, which lie in the profile's own plane, were picked within the usual pixel reach first, and so were the origin planes. Across 48 spots on the base, viewed from three angles, the profile won 1 to 12.

**Resolution:**
- `qml/ProfileMaterial.qml` with `qml/shaders/profile.vert`: the profile's shading, hover and selection are drawn with every point pulled toward the camera by 0.1 mm plus 0.2 % of its distance (never more than half the way), so they always pass the depth test over the face they lie on, and a body really in front still hides them.
- `Selection.partAt`: when the pointer is over a profile (in front, or on a face within that same margin, `profileMargin`), a point or edge wins only within 3 px (`onProfilePixels`) and only if it isn't behind the profile; an origin axis or point likewise within 3 px; an origin plane never. Otherwise the profile.
- E opens Extrude in Model mode (`Shortcut extrudeKey`, off in sketches, menus, typing and while a solid job runs), and the command has an icon (`SketchIcon` kind `extrude`, used by the search list and the history).

The profile now wins 44 to 47 of the 48 spots; the misses are right on an origin axis in front of it. Ours.

#### Extrude: a click swapped the profile

**Problem:** With a profile picked in Extrude, clicking a face (meant as the face to extrude up to, without first choosing To Object) dropped the profile and made the face the area instead: a plain click replaced the selection, as it does outside any tool. Nothing showed how many areas were picked or gave a way to clear them, or the object. Separately, E did nothing after an Extrude had been used once: the hidden distance box kept the keyboard and took the letter.

**Resolution:**
- `Selection.clicksAdd` and `Selection.clickFilter`, set by `ExtrudeTool` while its areas are being picked (`takingAreas`): a plain click adds or takes away, a click on nothing keeps the list, and `takesArea` turns away anything but a profile or flat face with a hint. `areasPicked` turns away a new area off the first one's plane (checked with `extrudeFrame`) and puts the selection back.
- A Profiles chip (`extrudeAreas`) at the start of the first row with the count (`areasLabel`) and a ✕ (`clearAreas`); the To Object chip gets a ✕ (`clearTarget`) once an object is picked. `Chip.clearSlot` keeps room for the ✕ so the label never moves.
- `ExtrudeTool.onOpenChanged` drops the keyboard focus from its boxes on close. `pickOnlyProfile` gives the profile's index, so clicking the profile it picked takes it away instead of adding it twice.

Ours. Fusion shows these as rows in its Extrude window; here they stay chips over the view.

#### Extrude: a plain click picks the object

**Problem:** Picking what to extrude up to took two steps (the To Object chip, then a click), and a plain click on a face or edge meant something else (it changed the areas). The owner's wish: Ctrl or Shift for more areas, as in Fusion, and any plain click on a point, edge, face or profile is the object to extrude up to, one object per extrude. An edge at a slant should be reached at its far end, with the offset to come back from it.

**Resolution:**
- `Selection.clickHook` replaces `clicksAdd` and `clickFilter`: while Extrude is open every click in the view goes to `ExtrudeTool.viewClicked(part, adding)`. With Ctrl or Shift, or when nothing is picked yet, it adds or takes away a profile or flat face; otherwise it calls `setTarget`, which sets the side (the one whose To Object chip is waiting, else side 1) To Object, checks the reach with `extrudeFrame`, and puts the old object back with the reason if it can't be reached. A plain click on an area already picked says how to take it away.
- `Document::extrudeReach`: edges, sketch lines and arcs use `farthestOf` (the point farthest from the profile's plane, either side) instead of the nearest to the line through the middle.

A side edge of the speaker mount is now reached at 86.0 mm, its top end. Ours.

#### Extrude: a join back onto a face left a step

**Problem:** The owner cut the speaker mount's base away to 1 mm under the base's top face, then picked the new bottom face and extruded it back down to the original sketch as a join. The base came back 0.05 mm smaller all round (outline area 773.1 mm² against 777.8, all the way down), a step at the seam, and sliver faces along it. Two causes:
- A picked face was pushed as an outline traced from its edges (`faceProfile`, a point every 0.01 mm of curve error): every curve became flats, up to 0.01 mm (0.0004 in) off, in the exact model and so in any export.
- OpenCascade's join of that tool came back the right volume, but its check called it broken: 7 new edges at the seam, most 0.04 mm long, lay up to 0.0101 mm off their faces against 0.0091 allowed. Neither input had a loose edge. The whole result was thrown away, and the fallback that moved the whole outline 0.05 mm inward was used, with a note.

**Resolution:**
- `extrudeFace` (engine interface; Heavy: the face itself, copied, moved to the start and pushed; Lite: none, the outline is used) and `Document::extrudeArea`, which every extrude tool now goes through. `ExtrudeJob::faces` says which areas are a body's face. A body's face is tried as drawn first, then with edges moved off walls.
- `HeavyEngine::combine`: a result of the right volume that fails only the check is repaired with `BOPTools_AlgoTools::CorrectCurveOnSurface` (the edges' tolerance widened to fit, never past the loosest tolerance either input already has), checked again, and used.
- The whole-outline fallbacks (0.01 and 0.05 mm) and `offsetProfile` are gone: a wrong size is never right. When nothing else works, the extrude fails and says so.
- `faceProfile` traces within 0.001 mm (0.00004 in).

The owner's join now builds as drawn and valid with no repair, 1.2 s (was about 7 s), 37,945.694 mm³; the new base's outline is 777.811 mm² all the way down, against the body's 777.78 to 777.83 at the seam; no sliver faces at the seam (were 415 with the traced outline). Still open: cutting from a body's own face straight through the body fails (18.6 s), because the converted base's walls flare 0.017 mm toward the bottom and the exact cut leaves skins thinner than that; before, it only worked with the outline grown 0.05 mm. Stock: `BRepPrimAPI_MakePrism`, `LocOpe_DPrism`, `BOPTools_AlgoTools::CorrectCurveOnSurface`. The rest is ours.

#### A pushed face's walls didn't show

**Problem:** The owner cut the speaker mount's base away to 1 mm under its top face, then extruded the new bottom face back down as a join. On screen the base looked removed: only its edges showed, like a cage. The model itself was right (valid, one solid, 37,945.696 mm³, outline 777.811 mm² all the way down). The base's 46 new side walls are surfaces swept from ellipse arcs (the converted body's face edges are ellipses), and OpenCascade's Delabella mesher left every one of them with no triangles, without any error. `HeavyEngine::mesh` skips a face with no triangles, so they weren't drawn, and the overlap and wall checks that read the meshes didn't see them either.

**Resolution:** After meshing, `HeavyEngine::mesh` meshes each face still without triangles again with OpenCascade's standard (Watson) mesher, at the same accuracy. Delabella stays for the rest (ten times quicker on a face full of holes); the extra pass only runs when a face was left bare, and handles all 46 of these. Stock: `BRepMesh_IncrementalMesh`, Delabella and Watson. The fallback pass is ours.

#### Extrude: every Extrude starts blank

**Problem:** Extrude kept what was typed and picked the last time: an offset of -1 mm used to stop a cut short came back on the next Extrude, so a join meant to reach a sketch stopped 1 mm short of it.

**Resolution:** `blank()` in `qml/ExtrudeTool.qml`, run by `begin()`: one side, 10 mm, Auto, Distance, no object, offset 0, taper 0°, distance per area off, nothing skipped. Fusion also opens Extrude fresh. Ours.

#### Extrude: a line where a join meets the body's tapered walls

**Problem:** After cutting the speaker mount's base away to 1 mm under its top and pushing that face back down as a join, a line runs round the base where the two meet. The new walls go straight down; the body's own walls above the line taper about 0.04° (Fusion's own STEP of the part has the same taper: outline 779.5 mm² at the bottom, 777.9 at the top of the base). Where they meet, the surfaces differ by 0.0002° to 0.037°, so they're two faces with a real edge between them. The rebuilt base is the right size at the line (within 0.03 mm² of outline) but up to about 0.02 mm (0.0008 in) smaller at the bottom than the tapered original.

**Resolution:** None yet. On an exact body with straight walls the same push and join leaves no line: a circle pushed makes a cylinder, and the faces in one surface are merged into one (checked: 0 edges at the seam). A straight extrude can't continue a tapered wall, so this one is a real edge, not the rounding fault in the next entry (that fix doesn't change it: same 37,945.694 mm³ and the same edge). Possible: an option that carries the neighbouring walls on down as their own surfaces, which would give back the original exactly with no line.

#### Extrude: a join up to a face or sketch stopped 0.000003 mm short and left a line

**Problem:** The owner made a box (a 76.2 mm square on the YZ plane, extruded 127 mm), sketched a smaller rectangle on its end face, cut the ring round it back 76.2 mm, then extruded the shoulder face back up to the sketch as a join. A line stayed round the small rectangle: the body was 11 faces where a box has 6. The reach "up to the object" was worked out in single precision (the sketch plane, the profile's middle, the mesh), so the rebuilt end face landed at x = 126.999996948 instead of 127. Two faces 0.000003 mm apart don't merge, and OpenCascade kept them as two with four sliver walls (0.0002 mm² each) between them. It happens on any join that has to land on existing geometry, at whatever spot the rounding falls.

**Resolution:**
- `Document::facePlane` adds `exact` (origin and both axes, 9 doubles) when the engine knows the face's exact plane (`planeOfFace`); a sketch made on that face keeps it, in the file too. `planeAxes` reads it (or the old single-precision plane for sketches made before).
- `Document::readExtrude` works out the start and direction in doubles from the placements, and `Document::extrudeReach` measures in doubles: up to a flat face from the engine's exact plane, up to a sketch point, line, arc or profile from the sketch's exact plane. Edges and points of a body, and projected sketch geometry, still come from the mesh (single precision).
- `HeavyEngine::combine`: `SetFuzzyValue(kTouching)` with `kTouching` = 0.00001 mm (Parasolid's session precision): faces closer than that count as the same place in a join, cut or intersect. Backstop for the sketch solver (about 0.0000005 mm) and the cases above still measured in single precision. Earlier a large fuzz made the converted speaker mount vanish; at this size it doesn't (same results as before on it).

The owner's box: the join now ends at exactly 127, 6 faces, 737,417.880 mm³ (the box before any cut), 0.06 s. Also checked with a cut of 37.3 mm and a join up to the end face instead of the sketch: 6 faces. Stock: `BOPAlgo_Options::SetFuzzyValue`, `BRepAdaptor_Surface` (exact plane). The rest is ours.

#### Join, cut, intersect: checked once; faces merged only within 0.0001 mm

**Problem:** Two things in `HeavyEngine::combine`, found by comparing with FreeCAD, CadQuery and build123d's code.
- After every boolean, the full validity check (`BRepCheck_Analyzer`) ran on the result, then the faces were merged and the check ran again on that. FreeCAD (`FCBRepAlgoAPI_BooleanOperation`, `TopoShape::makeElementBoolean`) and CadQuery run no full check after a boolean at all. On the speaker mount the two checks were about 0.3 s of a 1 s join.
- Faces of one surface were merged (`ShapeUpgrade_UnifySameDomain`) within 0.01 mm and 0.0001 rad. A merge keeps one face's surface, so a face could move up to 0.01 mm (0.0004 in), past the 0.0001 in the model must hold; and over a 300 mm face, 0.0001 rad is 0.03 mm. FreeCAD (its own `modelRefine.cpp`: planes, cylinders, B-splines within 0.0000001 mm), CadQuery and build123d (UnifySameDomain at its defaults) and SolveSpace (`SShell::MergeCoincidentSurfaces`, planes within 0.000001 mm) all merge far tighter. The 0.01 mm was for a sketch on a converted body's face sitting 0.0001 mm off it; sketches on faces now use the face's exact plane, and joins treat faces within 0.00001 mm as touching.

**Resolution:**
- The result is merged first, then checked once (quick volume check, then `BRepCheck_Analyzer`). Only if that fails is the unmerged result checked, repaired if it needs it (`BOPTools_AlgoTools::CorrectCurveOnSurface`, as before), merged again and checked. A result is never used unchecked.
- Merge within `kMergeLinear` 0.0001 mm and `kMergeAngular` 0.000001 rad (0.001 mm over a metre).
- Tried and left out: CadQuery's glue mode for a join that only touches the body (`SetGlue`, about twice as quick in a probe). It is only right when the two pieces don't overlap at all, and our touch test comes from the drawn mesh, which can miss an overlap of 0.01 mm; a wrong glue gives a broken solid.

Speaker mount: cut 0.8 to 1.0 s, join 0.7 to 0.86 s (were about 0.94 and 1.0), same volumes. Owner's box: still 6 faces after the cut and join. Stock: `BRepAlgoAPI_Fuse/Cut/Common`, `ShapeUpgrade_UnifySameDomain`, `BRepCheck_Analyzer`. The order and tolerances are ours.

#### Extrude: up to a curved face, an edge or a corner, measured on the exact shape

**Problem:** Extruding up to an object measured how far from the drawn mesh for anything but a flat face or a sketch: single precision, and up to 0.01 mm off a curve (a cylinder's mesh is flats). FreeCAD (`FeatureExtrude::generateSingleExtrusionSide` -> `TopoShape::makeElementPrismUntil` -> `BRepFeat_MakePrism::Perform(face)`), CadQuery (`BRepFeat_MakePrism`) and build123d (`Solid.extrude_until`: extrude past, split by the target's surface) never measure a distance at all: the extrude ends on the target's own surface.

**Resolution:** `GeometryEngine::reach` (Heavy: on the exact shape; Lite: none). A face: where a line from the profile's middle along the extrude first meets it (`IntCurvesFace_ShapeIntersector`). An edge: its farthest point either way along the extrude (`BRepExtrema_DistShapeShape` against a plane across the extrude, far ahead and far back). A corner: its point. `Document::extrudeReach` uses it before the mesh, which is now only the fallback. The edges and corners are numbered by the same lists the mesh is made from (`shownEdges`, `shownCorners`), so a picked edge is always the same edge. Checked: up to a box's corner and edge, 127.000000000 mm exactly; up to a cylinder's side, 174.600196851 (exact answer 174.600196851); up to its top circle, 225.400000000.

Still different from Fusion: an extrude up to a curved face ends flat, at the point where its middle meets the face. Fusion (and FreeCAD, through `BRepFeat_MakePrism`) end it on the curved face itself. Stock: `IntCurvesFace_ShapeIntersector`, `BRepExtrema_DistShapeShape`. The rest is ours.

#### Extrude lengths in inches

**Problem:** Extrude started at "10 mm", showed All's reach in mm, and a dragged length was rounded to 0.1 mm (0.004 in), while every sketch dimension is in inches. The owner works in inches and thousandths.

**Resolution:** `ExtrudeTool.lenText`: lengths are shown in inches to 0.001 in (`Units.plain`); Extrude starts at 1.000 in, offsets at 0.000 in; a dragged length is that shown value exactly. A plain number typed is inches, as everywhere; "25 mm" still works. The profile gap finder and Mesh to Solid's fit say inches too. Ours.

#### Extrude: exact sketch curves, one boolean per body, no nudges or retries

**Problem:** A spline, ellipse or conic in a sketch reached the engine as its drawn points, so a 300-point spline cut made 7,204 side faces and took 58 s. The extrude also carried workarounds from chasing an angled-wall problem: a 0.05 mm nudge and retry, edge repair, a wall-edge mover, and a volume estimate checked against the result. And each area went through its own boolean.

**Resolution:**
- `Profile` carries each curve exactly (`Profile::Curve`: B-spline poles, knots, weights, or an ellipse). `Document::exactCurves` tags every profile step lying on one curve. A fit spline is its centripetal Catmull-Rom curve written exactly as Bezier spans, a control spline is its clamped cubic B-spline, a conic is its rational quadratic Bezier. `extrudeOn` makes one exact edge per curve run, and puts corners where the curves truly cross (`Geom2dAPI_InterCurveCurve`).
- `GeometryEngine::combine(body, tools, op)`: every area of a side goes into one OpenCascade boolean (a list of tools), as FreeCAD and CadQuery do.
- Whether the boolean did anything comes from its own history (`IsDeleted`, `Modified`), not from volumes. OpenCascade's plain volume of a face with a long spline edge can be out by about 1.3% (a fixed number of sample points per edge). When nothing changed, a point inside each tool is classified to confirm it (`BRepClass3d_SolidClassifier`).
- Removed: the nudge and retry, the edge repair, `wallEdges`/`moveWallEdges`, `overlapEstimate`/`estimateExtrude`.

300-point spline cut: 8 faces, about 5 s in the app. Stock: `BRepAlgoAPI_Fuse/Cut/Common` with a tool list, `Geom2d_BSplineCurve`, `Geom2d_Ellipse`. Ours: the exact curve forms and the tagging. UI: Extrude, any sketch with curves.

#### Extrude: a taper with spline sides

**Problem:** A taper used `LocOpe_DPrism`. On a spline profile it came out invalid (100 faces) or threw. OpenCascade's other draft tools can't do it either: `BRepOffsetAPI_DraftAngle` only leans flat and round sides (`Draft_Modification_1.cxx` returns "face recomputation" for any other curve), and `BRepOffsetAPI_MakeDraft` crashed on a spline. FreeCAD lofts to an offset copy of the outline (approximate corners), and CadQuery uses `LocOpe_DPrism`. `LocOpe_DPrism` was also wrong on every widening taper: a 10 mm square, 8 mm at +5 degrees gave 916.089 mm³, where the exact figure is 917.211.

**Resolution:** `taper::prism` (`src/engine/heavy/taper.cpp`), drafting each side on its own as a commercial kernel does:
- A straight edge leans as a plane, an arc as a cone, and a spline or ellipse as the ruled surface its leaning lines sweep. That last is approximated to 0.0000001 mm (`AdvApprox_ApproxAFunction`, cut at the curve's own knots), with each bottom point paired with its own top point.
- Each corner runs up where the two sides beside it truly meet. It is followed by Newton's method on the two grown outlines, then fitted to 0.0000001 mm. Corners where the sides carry on smoothly run straight up. A side may run past its edge's end (its natural continuation) to reach a corner.
- The edges' curves on each face are written directly, not projected.
- Refused, with the reason given: an edge shrinking to nothing before the top, sides running into each other (`BOPAlgo_CheckerSI` to level 4, edges through faces), and a top outline crossing itself (`BRepCheck_Analyzer`). Level 5 is left out: it intersects every curved face with itself over its whole parameter box, so a closed spline side with a sharp start always fails at its own seam (`BOPAlgo_CheckerSI.cxx`, `CheckFaceSelfIntersection`).
- Positive widens the shape as it goes (Fusion's way, as before).

Checked against exact volumes: square, circle (cone frustum), slot, and a D shape (sharp plane/cone corners), each at ±10 and ±30 degrees, all within 0.00000001 relative. Spline flower at ±3 and −10 degrees within 0.00004 against dense offset points. Star cut with a 5 degree taper in the app: 173,578.39 mm³, exact. 3 to 50 ms per side set. Stock: `Geom_Plane`, `Geom_ConicalSurface`, `Geom_BSplineSurface`, `AdvApprox`, `BOPAlgo_CheckerSI`. Ours: the builder. UI: Extrude, Taper angle.

#### Extrude and booleans: why a result was refused, said plainly

**Problem:** A failed extrude said only "Nothing was made", or "couldn't be built ... left as it was", with no reason. And a cut whose walls lay a hair off a converted model's own walls came back valid and closed, but with 106 edges shared by more than two faces and next to nothing cut away. It was kept.

**Resolution:**
- `GeometryEngine::whyNot()`: the reason for the last extrude or combine that gave nothing. The extrude's note carries it: "The cut couldn't be built on Body3: the result would touch itself along a line, leaving a face with no area there. It was left as it was." The same goes for areas that couldn't be extruded at all (a taper too steep, an outline that doesn't close).
- A boolean's result is kept only if:
  - no edge or corner had to be loosened past 0.00001 in (0.000254 mm), or past the loosest of the shapes it started from, whichever is larger (`loosest`, `kAccuracy`). Reason: "the shapes don't quite meet where they should: the result would be looser than 0.00001 in there".
  - no face has zero area (`hasEmptyFace`). Reason: "the result would touch itself along a line, leaving a face with no area there".
  - it is a valid closed solid (`soundSolid`). Reason: "the result wouldn't be a sound solid".
- The merged result is tried first, then the unmerged one. If both fail, the more telling reason is given: loosened, then empty face, then unsound.
- Material that touches itself along a line or at a point is kept, as drawn (the owner's choice): a star whose tips land exactly on the block's walls, a bow-tie whose triangles meet at a point. Edges shared by more than two faces are allowed.

Checked: the star on the walls and the bow-tie are kept, loosest edge 0.0000001 mm, exact volumes. A self-crossing spline figure-eight is refused (a face of no area). The cut on a converted speaker mount is refused (edges loosened to 0.3 mm). Stock: `BRepCheck_Analyzer`, `BRepGProp`, `BRep_Tool::Tolerance`. Ours: the rule and the reasons. UI: the Extrude note.

#### Face move: checked by self-intersection, not volume

**Problem:** A moved face (`pushFace`) was checked by comparing the volume change against the face's area times the distance, with 1% slack. Volumes are unreliable on spline faces (see above), and the slack could hide a small collision.

**Resolution:** The moved body must pass `BOPAlgo_CheckerSI` to level 4 (no edge passing through a face). The block's top face moved up and down 5 mm: exact, checked in 0.00 to 0.05 s. Stock: `BOPAlgo_CheckerSI`. UI: Extrude of a body's own face, no taper.

#### Booleans: the model holds 0.00001 in

**Problem:** The model was to hold 0.0001 in. That is now 0.00001 in (0.000254 mm). And faces of one surface merged after a join, cut or intersect within 0.000001 rad: over a 20 ft face, two faces that far apart in angle lean 0.006 mm (0.00024 in) apart at the far end, and the merge keeps one of them.

**Resolution:**
- `kAccuracy` is 0.000254 mm: a boolean whose result had to be loosened past that (or past the shapes that went in) is refused with "the result would be looser than 0.00001 in there".
- `kMergeAngular` is 0.00000001 rad: 0.00006 mm over 20 ft. `kMergeLinear` stays 0.0001 mm (0.0000039 in).

Stock: `ShapeUpgrade_UnifySameDomain`. Ours: the limits. UI: none; the Extrude note gives the reason.

#### Extrude: a line crossing a curve meets it exactly, at any size

**Problem:** A straight line drawn across a 10 ft spline flower came out as a wall turned 0.0002 rad (0.012 degrees) in plan: 0.4 mm off at its ends. The profile's corner where the line crosses the curve is found on the curve's straight sample steps. The engine moves it onto the true crossing (`Geom2dAPI_InterCurveCurve`), but took that crossing only within 0.05 mm of the drawn corner. On a big curve the steps bow further than that off it (40 mm steps on the 10 ft flower), so the corner stayed on the curve at a point part way between two samples, off the line.

**Resolution:** The crossing is taken up to one sample step's length either side of the drawn corner (at least 0.05 mm). The 10 ft flower's four trimmed flats are now within 0.0000000000002 mm of the lines drawn.

Stock: `Geom2dAPI_InterCurveCurve`. Ours: the reach. UI: none.

#### Extrude: a curve closing on itself at a corner is two edges

**Problem:** A figure-eight drawn as one spline was refused at 100 mm ("a face with no area"), yet built at 10 ft. Its left lobe runs from the crossing round to the crossing: one edge, starting and ending at the same corner. Swept up, the lobe's wall became one face that wraps round to itself over a seam. A seam is how a smoothly closed surface (a cylinder) is held, not a sharp corner. The cut kept that wall with no area. At 10 ft the test's trim lines happened to split the lobe, which hid it.

**Resolution:** A curve run that comes back to its own start with a sharp corner there (tangents more than 0.0000001 rad apart) is made as two edges, split at the middle of its range, as a whole circle already is. The crossing is then a real corner edge between two faces. A curve that closes smoothly (a closed spline flower) stays one edge.

Checked: the 100 mm figure-eight builds valid, its opening at the top and its floor 1761.670661387 mm² both (walls straight), volume exact. Its lobes touch along one line at the crossing, kept as for the bow-tie.

Stock: `BRepBuilderAPI_MakeEdge`. Ours: the split. UI: none.

#### Extrude: an arc crossing a line meets it exactly, on its own circle

**Problem:** A gear with a bore and bolt holes cut through a 10 ft plate left its arc walls up to 0.005 mm off the arcs drawn (0.00017 mm at 100 mm). Where an arc meets a line, the profile's corner is found on the arc's straight sample steps, on a chord inside the circle. The step from the last sample to that corner was not marked as on the arc, so it was built flat. The corner was left where it was found, because corners were only moved onto the true crossing next to a spline, ellipse or conic. And the arc was made through three of its points, which gives a circle of their own, off the drawn one by their error.

**Resolution:**
- The sketch marks a step as on an arc when both its ends lie on the arc's drawn path, even if one end is a crossing inside the circle (`Document::arcCentres`).
- An arc's radius is the largest distance of its samples from its centre, since a crossing lies inside the circle.
- An arc meeting a line or another arc gets its corner moved onto the true crossing of the circle and the line (`Geom2dAPI_InterCurveCurve`), within one sample step. Two lines still meet where they were drawn.
- The arc edge is made on the drawn circle itself (its centre and radius), from corner to corner, round the way its samples go.

Checked: every wall of the bore-and-bolt-hole gear within 0.00000000003 mm of the circles drawn, at 100 mm and 10 ft. Circles that meet nothing were already exact.

Stock: `Geom2dAPI_InterCurveCurve`, `GC_MakeArcOfCircle` (circle and two points). Ours: the marking, the radius, the corner rule. UI: none.

#### Booleans: a flat face through an extruded curve's wall meets it exactly

**Problem:** A 32-tooth spline gear cut through a plate was refused at both sizes ("the result would be looser than 0.00001 in there"), and so was a spline gear body's face cut through one. Where the plate's bottom face crosses a tooth's wall, OpenCascade works the edge out as it does any free-form crossing: by walking along it and fitting a curve (`IntPatch_Intersection::GeomParamPerfom`, `IntPatch_ImpPrmIntersection`; read in the 7.9.3 source). It has no exact case for a plane and a wall swept from a curve. The fitted edges were 0.0013 mm loose at 100 mm and 0.036 mm loose at 10 ft. A cut that starts on a face, or ends exactly on one, never showed this, because there the faces lie in one plane and the tool's own exact edges are used.

**Resolution:** After the boolean, before the merge (`exactSections`), the engine looks at each new edge between:
- a wall swept straight from a curve that lies square to the sweep (`Geom_SurfaceOfLinearExtrusion`), and
- a flat face square to that sweep.

That edge is exactly the wall's curve, moved to the face's height. It gets:
- that curve as its curve;
- the wall's own line at that height as its place on the wall;
- the curve laid flat as its place on the face.

Where it starts and ends along the curve is read off its own place on the wall (the wall's first parameter is the curve's). It is not found by projecting its corners, because on a curve that comes near itself (a figure-eight's crossing) a projection lands on the other branch: that gave a 6096 mm loose edge before it was changed. Its new corners are put on it, and its tolerance becomes what it really needs, measured (`ShapeAnalysis_Edge::CheckSameParameter`, then `BRepLib::UpdateTolerances`). The result is used only if it comes out valid and no looser than the boolean's own. Otherwise the boolean's own is kept, as it was in a few full-suite cases already within the limit. The work is done on a copy. Edges and corners of the bodies that went in are never moved, so undo keeps them as they were. This works both ways round: a tool's spline walls through a body's faces, and a body's spline walls through a tool's floor.

Checked: the spline gear cut through, and the spline gear body's face cut through, at 100 mm and 10 ft. 64 to 88 edges made exact each time. Loosest edge 0.0000051 mm, no edge on more than two faces, every wall within 0.00000000003 mm of the splines drawn, valid.

Stock: `BRepBuilderAPI_Copy`, `GeomAPI::To2d`, `ShapeAnalysis_Edge`, `BRepLib::UpdateTolerances`. Ours: the exact crossing. UI: none.

#### Booleans: exact crossings hold at 100 ft too

**Problem:** At 100 ft (a 100 ft plate, 40 ft thick) two cuts were still refused:
- A 300-point wavy spline cut through. OpenCascade's fitted edges across the plate's bottom strayed 3.3 mm off mid-span. That was past the fixed 0.1 mm the exact-crossing step allowed when confirming it had the right edge, so it skipped them.
- A spline figure-eight cut through. Four exact edges met at the crossing on the plate's bottom, but two ended 0.0012 mm away. Their ends had been read off OpenCascade's fitted places on the wall.

**Resolution:**
- An edge is confirmed as lying along the wall's curve within twice what the boolean itself says it may be off (its tolerance), not a fixed distance.
- An end at an upright edge (a straight edge along the sweep) is put exactly where that edge stands on the face's plane. It is found from the fitted end by Newton's method along the curve: a local step, so it can't jump to another branch of a curve that comes near itself.
- Every corner of a profile where a curve meets a line, an arc or itself is polished to the crossing by Newton's method on the two curves, after `Geom2dAPI_InterCurveCurve` finds it. Two areas sharing one crossing (a figure-eight's lobes) then put their corners on the very same point.
- A refusal for looseness now writes its loosest edges and corners to the window's log, with what each lies between and where. Any refusal can be traced afterwards.

Checked: the hammer suite (`history/tools/probes/suitehammer`), 19 cases at 100 mm, 10 ft and 100 ft:
- splines and gears through, exactly to the bottom, 3x deeper, partway;
- another body's face as the profile;
- slots, rounded rectangles, D shapes, keyholes, overlapping circles, a plate with 12 holes and a slot;
- bow-tie, heptagram, figure-eight, star over circle.

Stock: `Geom2dAPI_InterCurveCurve`. Ours: the polish, the upright ends, the bound, the log. UI: none; the log is the window's.

#### Display: walls swept from a curve meshed without interior checks

**Problem:** A 32-tooth spline gear cut through a 40 ft plate took 8 s, and 6.4 s of that was making the triangles to draw. Delabella leaves walls swept from a curve bare, so they go to the standard mesher (Watson). Watson also checked how far the inside of each wall strays from its triangles. That filled the 44 tall walls with 524,812 triangles.

**Resolution:** A wall swept straight from a curve is straight from end to end. Triangles between the points of its edges are as close to it as those points are to the curve, so for those faces (`GeomAbs_SurfaceOfExtrusion`) only, the inside isn't checked (`ControlSurfaceDeflection` off). The same 44 walls now take 0.05 s and 2,480 triangles. Every other face is meshed as before.

Stock: `BRepMesh_IncrementalMesh`, `IMeshTools_Parameters`. Ours: the per-face setting in `HeavyEngine::mesh`. UI: none.

#### Booleans: a body OpenCascade calls invalid is put right before a join, cut or intersect

**Problem:** A model converted from a mesh by an older OmaMake came in with 47 flat strips about 0.01 mm wide, left between its faces. Its edges are up to 0.05 mm loose. In one strip, three edges don't meet on the face. OpenCascade's check therefore calls the whole body invalid, and every cut of it was refused as "not a sound solid". Before that check was added, the same cuts went through.

**Resolution:** Before a join, cut or intersect, each body is checked once (a body never changes once made, so the answer is kept per body). A body that fails the check, or has an edge with a face on one side only, is put right on a copy with OpenCascade's standard Shape Healing:
- `ShapeFix_Shape` first: wires, their order and direction, edges and their curves on the faces.
- Then `ShapeFix_FixSmallFace`: faces narrower than the body's own loosest edge are folded into the faces beside them.

Both work within the body's own loosest edge or corner, never past it. The fix is kept only if:
- the body comes out sound;
- it is no looser than before;
- every corner of the fixed body lies within that same distance of a corner of the old one.

The faces keep their own surfaces, so that is the same shape. The original body stays untouched, for undo. Results the Boolean has already checked in full are marked sound and not checked again. The window's log says when a body was put right, how, and how long it took.

On that model the 47 strips are folded away in 0.3 s (the first check takes 0.1 s). The body loses 0.0066 mm² of surface and keeps its box exactly.

**Not covered:** that is not enough for that model's cuts. Cut upward with its own outline (5 mm, 20 mm, or up to its shoulder), the Boolean still can't tell inside from outside. The part to be cut away is kept as a second solid, and the thread's faces are left inside the material. Its curved faces really don't meet its flat ones (231 edges lie over 0.001 mm off their faces in 3D, the worst 0.05 mm), and the tool's walls lie on the body's walls within that. No repair short of refitting the surfaces tightens that. Those cuts are now refused, with a log (next two entries). A first version of this entry said the 20 mm cut went through: it was accepted, but broken in the same way, which the next entry's checks now catch.

Stock: `ShapeFix_Shape`, `ShapeFix_FixSmallFace`, `BRepCheck_Analyzer`, `ShapeAnalysis_Shell`. Ours: when it runs, the limits, the corner check, the per-body memory. UI: none; the log is the window's.

#### Booleans: a result with faces left inside, or a shell that isn't closed, is refused

**Problem:** A cut of a converted model came back from OpenCascade's Boolean "valid" by its own check (`BRepCheck_Analyzer`), with no edge that has a face on one side only, but broken:
- two solids, the part meant to be cut away kept as the second;
- 4 shells that weren't closed;
- 17 faces marked internal, left inside the material, which showed through the model like a wireframe;
- nothing removed (35,331 mm³ before and after).

It was accepted and saved.

**Resolution:** A sound solid now also has every shell closed (each edge of a shell used by two of its faces, `BRep_Tool::IsClosed`), and no face marked internal or external. These come before OpenCascade's own check, which passes both. That cut is now refused and the body stays as it was.

Stock: `BRep_Tool::IsClosed`, `TopAbs_INTERNAL`. Ours: `soundSolid`. UI: none.

#### Extrude: checked whole when done; on failure all put back, with a log file

**Problem:** An extrude that went wrong left only a short note on screen and a line or two in the window's log. That wasn't enough to work out why afterwards.

**Resolution:** When an extrude has done everything it was asked, every body it changed or made is checked in full (`GeometryEngine::inspect`: the sound-solid check above, plus no face of no area). If any isn't sound, or any body's join, cut or intersect couldn't be built, or no area could be extruded, all of it is put back as it was, and one log file is written per failure. Dev builds write it to the source folder's `logs/` (`OMAMAKE_SOURCE_DIR`, gitignored); the on-screen note gives its path. The log holds:
- when it happened, the file, and the engine;
- what the person was told;
- the extrude's settings as asked;
- every step top to bottom with times: the areas (placement, loops, arcs, exact curves), each side (operation, spans, bodies reached), the tools made, each body's operation and its outcome;
- each engine line above the step it belongs to (`GeometryEngine::takeTrace`), including any repair before use and any refused result described in full;
- each body and tool as it came out and as it went in, described in full (`describe`): solids, each shell (faces, closed or open, where), faces marked internal (area, where), OpenCascade's findings by kind of piece with where they are, edges with a face on one side only, loosest edge and corner, volume and box.

Stock: `BRepCheck`, `ShapeAnalysis_Shell`, `BRepBndLib`. Ours: the check, the put-back, the log and its contents. UI: the failure note names the log file.

#### Extrude: pieces left apart become bodies of their own

**Problem:** A cut that left pieces standing apart kept them all in the one body as several solids. Cutting only the area between an outline and the circles inside it, for example, leaves a pin where each circle was. Fusion puts each separate piece in as a body of its own.

**Resolution:** After the whole-extrude check passes, each body the extrude changed is looked at for separate solids (`GeometryEngine::pieces`). Where there are several, the biggest by volume stays that body and each other one becomes a new body in the active component's Bodies. A new body made of several solids is split the same way. The geometry isn't touched: each piece is the solid exactly as the Boolean left it, and a body already checked sound passes that on to its pieces. Undo removes the new bodies and puts the original back whole. The log notes each split.

Stock: `TopExp_Explorer` (solids), `BRepGProp`. Ours: `pieces`, which piece keeps the body, where the rest go. UI: the new bodies show in the Browser under the active component's Bodies.

#### Extrude: a new body's faces in one surface made one

**Problem:** A new body extruded from a sketch circle came out with its round side in two half faces and a line down it. A cut or join didn't have this, since a Boolean's result already had its faces merged. A plate with three holes as a new body had 12 faces where it should have 9.

**Resolution:** Each new body not made by a join now gets the same merge as a Boolean's result (`GeometryEngine::merged`): FreeCAD's way (`refine::sameSurfaces`), then OpenCascade's `ShapeUpgrade_UnifySameDomain`, with the same 0.0001 mm and 0.00000001 rad limits. The merged body is kept only if:
- it has fewer faces;
- it's a sound solid, with no face of no area;
- it's no looser than before (or 0.00001 in);
- its volume is the same to a millionth of a percent.

Otherwise the body stays exactly as it was built, and the log says why.

Stock: `ShapeUpgrade_UnifySameDomain`, `BRepGProp`. Ours: `merged`, the conditions, which new bodies get it. UI: none (one face where there were two).

#### Extrude: Fuzzy profile, tried automatically when the exact one fails

**Problem:** A model whose own faces don't quite meet can't always be cut exactly. A body converted from a mesh is the usual case: neighbouring faces there can be up to 0.05 mm apart. Where an extrude's tool crosses two faces that should share an edge, the two crossings miss each other, nothing closes, and the Boolean fails. Every cut on the converted speaker mount failed this way. A "Fuzzy profile" tick box (2026-11-06) fixed those cuts, but only when someone knew to tick it.

**Resolution:** It is automatic now, and the tick box is gone (2026-11-07). An extrude is tried in this order:
1. The whole extrude is worked out exactly, as always (`Document::computeExtrudeTrying` calls `computeExtrude` with `job.fuzzy` false). Each Boolean uses `kTouching`, 0.00001 mm, which every extrude has always used.
2. Only if that failed because a body's join, cut or intersect couldn't be built (`ExtrudeResult::booleanFailed`, set where the step fails in `computeExtrude`) is the whole extrude worked out again from the start, with `job.fuzzy` true. Any other failure (no area could be extruded, a bad setting) is reported as it is, with no second try. Exact models never reach this step.
3. In the second try each Boolean is still tried exactly first (`HeavyEngine::combine` → `combineAt(…, kTouching)`). Only a Boolean that fails is taken again with `SetFuzzyValue(kFuzzyProfile)`: points up to 0.001 mm (0.00004 in) apart are taken as one place. So only the Booleans that need it are fuzzy.
4. A fuzzy result passes the same checks as any other Boolean before it's kept: a sound solid (every shell closed, nothing internal or external, OpenCascade's check), no face of no area, and no looser than the loosest input or `kAccuracy` (0.000254 mm, 0.00001 in), whichever is larger. A fuzzy Boolean that fails any of these is refused, the same way an exact one is.
5. Then the whole-extrude check, the put-back on failure, and the split into separate bodies run as usual (see "Extrude: checked whole when done…").

What the person is told:
- **Built exactly:** nothing new.
- **Built on the second try:** after the extrude finishes, a note stays up long enough to read (`ExtrudeTool.qml`, `tool.readingMs`): "This extrude needed Fuzzy profile: the model's faces don't quite meet, so it couldn't be built exactly. Where the profile meets the model it may be off by up to 0.001 mm (0.00004 in)." If the model is looser than that, the note goes on: "The model itself is only exact to N mm (N in) there." N is the loosest edge or corner of the bodies the extrude reached, measured before the extrude (`GeometryEngine::looseness`, Heavy's `loosest`; Lite returns 0). On the converted speaker mount it is 0.052 mm (0.0021 in).
- **Failed both times:** everything is put back as usual. The note says why, adds "It was tried again with Fuzzy profile (points up to 0.001 mm apart taken as one), and that failed too.", and names one log file holding both tries in order ("first try, exact:", then "second try, automatic, with Fuzzy profile").

The estimate, and why it's honest: fuzzy only joins points within 0.001 mm of each other, so a face it moves moves no further than that. Everywhere else the result is the exact geometry of the profile and the model. A fuzzy result can't come out looser than the model already was (step 4), so the model's own looseness bounds the error where that is the larger figure.

Tolerances in play:
- `kTouching` 0.00001 mm: every Boolean, every time (Parasolid's session precision).
- `kFuzzyProfile` 0.001 mm (0.00004 in): the second try only, and only the Booleans that failed exactly (`document.cpp`).
- `kAccuracy` 0.000254 mm (0.00001 in): the floor for how loose any result may be.

Measured on the converted speaker mount (`history/tools/probes/suitespeaker`, three box cuts through it): all three fail exactly, and each is built on the second try with the note. Each volume is within 0.004 % to 0.021 % of the same cut worked out separately. Each takes about 7 to 10 s in all.

Stock: `BOPAlgo_Options::SetFuzzyValue`, `BRepCheck`. Ours: the two tries and when the second runs, exact first per Boolean, the checks a fuzzy result must pass, the accuracy note, `looseness`, the log of both tries. UI: no tick box; a note after an extrude that needed it.
