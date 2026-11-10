# Known issues

Problems seen while building OmaMake that come from the system around it, kept here so they stay on record in every copy of the project. When one is solved, its resolution is added under it, whether an update fixed it or we did. Solved issues stay listed.

## Notification toast blanks the whole screen

Status: worked around on this machine with the setting below, waiting on an Omarchy fix, first seen 2026-10-30.

Problem: while an Omarchy notification toast shows, the whole screen goes black except the toast: every window, the wallpaper and the top bar. It comes back when the toast goes.

Not OmaMake: it happens with OmaMake closed. Turning Hyprland's blur off and making the toast solid both leave it black.

Suspected, not proven: a system update. It started in the first session on these versions:

| Package | Before | After |
|---|---|---|
| hyprland | 0.56.2-4 | 0.57.0-1 |
| hyprutils | 0.14.2-1 | 0.14.3-1 |
| mesa, vulkan drivers | 26.2.5-1 | 26.2.6-1 |
| qt6-declarative | 6.12.0-1 | 6.12.0-3 |

Reproduce: `notify-send -t 3000 "Blank test" "one"`.

Cause, found 2026-10-31: Hyprland 0.57 turned `quirks:skip_non_kms_dmabuf_formats` on by default, and on this VM's graphics that leaves apps with a single buffer format, XR24, which has no alpha. The toast is a full-screen transparent window on the overlay layer, so without alpha it paints black over everything. Reported at [omacom/omarchy#17061](https://github.com/omacom/omarchy/issues/17061).

Seen here: `quirks:skip_non_kms_dmabuf_formats` reads true, OmaMake's graphics format table has one entry, and all 5 of its buffers are XR24.

Resolution: tested and working 2026-10-31 with `hl.config({ quirks = { skip_non_kms_dmabuf_formats = false } })`, then `hyprctl reload` and `omarchy restart shell`. `hyprctl eval` alone doesn't work, because the format list is only rebuilt on a reload. Apps started before the change stay without alpha until restarted.

Side effect seen: the wallpaper was missing after the shell restart. Picking a new one in OmaSkins brought it back, after several tries. Why it went missing is not known.

## Window background is black instead of see-through

Status: worked around on this machine with the setting below, waiting on an Omarchy fix, first seen 2026-10-30.

Problem: OmaMake's model space shows black where the wallpaper should show through. It worked at 07:38 on 2026-10-30, before the session restarted onto Hyprland 0.57.

Not OmaMake or its engine: the app still draws the same see-through pixels on both engines (143 of 255 unfocused), but they reach Hyprland in a buffer with no alpha.

Cause: the same as the toast above, and the same resolution cleared both.

## OpenCascade booleans hand back empty or broken solids

Status: worked around in OmaMake (each result checked and redone), one case still open, first seen 2026-11-03.

Problem: on a body converted from a mesh, whose edges carry a loose tolerance (0.063 mm), OpenCascade's boolean can report success and return a solid that is empty or broken. Joining an extrude whose sides lay exactly on the body's notched outer wall came back with no volume, and the body vanished from the view while staying in the browser. The same join with the same settings could also come back whole on another run.

Not OmaMake: OpenCascade 7.9 `BRepAlgoAPI_Fuse` with parallel running, oriented boxes and non-destructive mode returns 0 mm³ for a join of a 35,511 mm³ body and a 15,586 mm³ extrude. A plain fuse of the same two shapes gives 37,981 mm³. A fuzzy value (0.01 to 0.5 mm) made it worse.

Resolution (OmaMake side): every join, cut and intersect is checked. The result must be a valid solid, and its volume must fit the operation: a join holds at least the bigger shape and at most both, a cut removes at most the tool, an intersect is no bigger than either. A failed result is redone plainly on copies of the shapes, with threads, then on one thread. If nothing passes, the body is left as it was and Extrude says so. Every attempt is written to the window's log as an `engine:` line. The checks add about 0.3 s per operation.

Second form: a cut whose sides lie right on the body's own walls (an extrude of the body's projected outline) reported success and removed 18 mm³ of 13,000. A volume check can't catch that, since a cut that removes little still fits. Extrude now works out the overlap from the body's mesh first, checks every straight join, cut and intersect against it, and when one is far off redoes it with the outline moved 0.01 mm off the walls, and says so. The moved cut removes 13,115 mm³, as it should. Since then only the edges that lie on a wall are moved, toward the empty side (into the body for a join), so the shape is unchanged; such an outline goes straight to the moved try, as drawn it fails slowly. Exactly coplanar faces (a sketch placed exactly on the base) made the plain boolean return a broken solid too.

Third form: a join whose tool stood on the body's face (the face itself pushed back down) came back the right volume, but OpenCascade's check called it broken: 7 new edges at the seam lay up to 0.0101 mm off their faces against 0.0091 allowed, and neither input had a loose edge. Such a result is now repaired with OpenCascade's own `BOPTools_AlgoTools::CorrectCurveOnSurface` (tolerance widened to fit, never past the loosest tolerance of the inputs) and used. The outline is never moved as a whole any more, since that changes the size; if nothing works, the extrude fails and says so. Still failing: a cut from a converted body's own face straight through the body, where the walls flare by less than the body's tolerance.

Still open: a narrowing taper (−10°) cut through the speaker mount's thread is broken in all three ways, and is refused. A widening taper through it works.


## A STEP file saved by OmaMake reads back with one wall inside out

Status: open, first seen 2026-11-04.

Problem: the speaker mount saved as STEP and opened again won't take a cut up to its top face, at 0.1 in or at 1 mm. Extrude says the cut couldn't be built. The same cut on the body before it was saved works.

Cause: after the STEP round trip one spline wall's boundary curves run past the edge of its surface (to 1.0100 of a 0–1 range, where the original sits at exactly 1.0000). OpenCascade then measures that wall at −113.95 mm² instead of 400.86, and the whole body at 33,636 mm³ instead of 35,510. The mesh, and Fusion's import of the same file, still read 35,328. The engine's cut is actually right (23,595 mm³, the same as an exact overlap), but Extrude's check compares it with an estimate from the mesh, sees it 1,800 mm³ off, and refuses it.

Not the inch change, the merge change or the touching tolerance: the cut fails with all three turned off, and the current build makes the same cut (whole model projected onto the base, up to the top face at −0.1 in and −1 mm) on the mesh-converted body in 0.8–1.0 s, 23,593 and 22,395 mm³ left. OpenCascade's own repairs (`ShapeFix_Shape`, `ShapeFix_Face`, rebuilding the boundary curves, every `read.surfacecurve.mode`) leave the wall as it is.

Resolution: none yet. Options: repair such walls on import, write STEP that reads back cleanly, or check a cut against an exact overlap when the body's mesh and exact volume disagree.

## OpenCascade's own chamfer and fillet hand back shapes that pass its check but are wrong

Status: worked around in OmaMake, first seen 2026-11-08, some cases still open.

Problem: OpenCascade 7.9's `BRepFilletAPI_MakeChamfer` / `MakeFillet` can report success and return a solid its own check calls valid but that is the wrong shape. Seen: two rounds that overlap on the face between them came back half again as big as the body; an edge whose bevel runs into a hole or a neighbouring fillet kept material that should have gone (21.6 and 116.7 mm³); a chamfer or round between two cones came back as one approximated spline face, slightly off round; a chamfer carried on along edges tangent to the picked one. Cutting a cone with another cone round the same axis can also lose one of them.

Resolution (OmaMake side): every result's volume is checked against what the operation can do (a cut no bigger than the body). Edges OpenCascade gets wrong, whole rims, edges whose band runs off the faces beside it, and sizes too big for the space run through OmaMake's own exact cut instead: the cross-section swept along the edge, and rims round one axis joined into one turned section, so cuts that meet form a ridge or a sphere. A size within the sketch solver's rounding (2.5e-10 mm) of a rim's radius is taken as exactly that radius.

Still open: on curved faces where a bevel leaves the surface at once, OmaMake can't yet tell that it runs off the face, so OpenCascade's own result is used there.

## Merging faces on a torus turned fillets inside out (fixed in 0.52)

Status: fixed in OmaMake, first seen 2026-11-09.

Problem: after a chamfer cut through the fillets round a row of holes, the fillets showed inside out and the chamfer seemed to pass through them, while OpenCascade's check still called the body valid. OmaMake's own clean-up, which joins faces lying on one surface, rebuilt a cut torus face from its outline, and on a torus that outline encloses the rest of the ring.

Resolution: that clean-up now joins faces only on planes, cylinders and cones, and a joined face must have the same area as its pieces. Spheres, tori and free-form faces are left to OpenCascade's `ShapeUpgrade_UnifySameDomain`. Files saved before the fix keep their saved body until that step is done again.

## Not done yet, or not tested much (0.52)

- Variable-radius fillets; a chamfer and a fillet in one step; Chamfer and Fillet on the Lite engine.
- A corner (point) fillet on a corner of a box leaves small flats rather than a smooth corner blend; a corner where more than three faces meet gets nearly, not exactly, equal angles.
- The drag arrow doesn't move to a better side when the view is turned.
- A large file opens with the window blank for several seconds and a short pause after it appears.
- The light look hasn't been checked with the new Chamfer and Fillet symbols or the Timeline's progress fill.

