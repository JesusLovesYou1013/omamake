# OmaMake

A parametric 3D CAD program made for [Omarchy](https://omarchy.org), and for any Linux system.

**Status: version 0.52.** Sketch mode is about 95% done. Extrude, Chamfer and Fillet are the modelling tools so far. The Timeline scrubs back and forth over every step, also in files opened from disk, and shows a chamfer, fillet or extrude growing as you drag. The other modelling tools are next.

## What works so far

- **The view.** Orbit, pan and zoom, a view cube, a floor grid, studio-style lighting, dark and light looks (or the current Omarchy theme), and see-through backgrounds that follow OmaSkins.
- **Search for every command** with Alt+Space. It offers only what fits the mode you're in, shows nothing until you type, and puts the likeliest first, so one letter is usually enough (in a sketch, L then Enter draws a Line, R a rectangle).
- **The Browser**, the design's tree on the left: nested components, bodies, sketches and each component's origin, with an eye on every row to show or hide it.
- **The origin**: centre point, axes and planes you can hover and select. Planes come in two styles, Cross (full size, through the centre) or Cube Corner (smaller, plus side only), for all of them or each one.
- **Sketches.** Triple-click an origin plane, a flat face or empty space to start one; Esc or Finish Sketch to leave, and the view flies back to where it was. Double-click a sketch in the Timeline or the Browser to go back into it. Inside: every Fusion 360 sketch drawing tool, each under Fusion's own name: Line, Midpoint Line, 2-Point, 3-Point and Center Rectangle, Center Diameter, 2-Point, 3-Point, 2-Tangent and 3-Tangent Circle, 3-Point, Center Point and Tangent Arc, Inscribed, Circumscribed and Edge Polygon, the five Slots, Fit Point and Control Point Spline, Conic Curve, Ellipse, Point, Text, Mirror (across any line or axis, held by Symmetry), Circular and Rectangular Pattern, Fillet, the three Chamfers (click corner after corner, or select a shape and do every corner at once), Offset (drag its arrow out or in, or type the distance), Trim, Extend, Break, Sketch Scale, Blend Curve and Move/Copy. Each makes the constraints that keep its shape, and options like a fillet's radius or a polygon's sides sit in a small box under the sketch bar. Drag a circle's edge to resize it. Snapping onto points, the origin, the middles of lines (a small triangle shows) and anywhere along lines, arcs and axes (and lines within 2° of level or upright), with the matching constraints made for you; drag points, lines or whole selected shapes (double-click a line to select its shape) and the rest follows as little as it must, snapping onto points, corners, middles and lines just as drawing does and staying held where you drop them; constraints Coincident, Collinear, Midpoint, Fix/UnFix, Parallel, Perpendicular, Horizontal/Vertical, Equal, Symmetry, Tangent and Concentric (pick before or after choosing one; the origin point and axes count too, which is how a sketch gets locked in place); Sketch Dimension for a line's length, an arc's radius, a circle's diameter, the distance between two points or a point and a line, the gap between two parallel lines or the angle between two lines, in inches unless you type another unit (`2.5`, `1 1/2`, `3in`, `60 mm`, `1' 6"`). Fully constrained lines turn grey, and selecting a point or line shows the constraints on it as small badges (hover one for its name; click badges to pick them and press Delete to take those constraints off, leaving the lines; Ctrl+Z brings them back).
- **Drawing and constraining.** Fusion 360's sketch tools and constraints by their Fusion names, sketch dimensions, a solver, closed profiles shaded as you draw, and construction lines.
- **Project.** Throw the whole model's outline, or any face's, flat or curved, straight onto a sketch as lines, arcs and circles.
- **Importing.** STEP, STL and OBJ files come in as solids. Fit Curved Surfaces turns a mesh's facets into true cylinders, cones, spheres, fillets and threads. It's been proven on one threaded part so far.
- **Extrude.** Pull a closed sketch profile or a flat face into a new body, or join, cut or intersect with the bodies already there.
- **Chamfer and Fillet** (Shift+C, Shift+F). Bevel or round edges, every edge of a face, or a single corner or cone tip, with a live preview you drag or type into. Equal, Two Distances and Distance and Angle chamfers; Constant Radius and Chord Length fillets. Sizes too big for the space keep cutting evenly until the cuts meet (a cylinder's two rims rounded to its radius make a sphere), and chamfers and fillets that run into each other blend in either order.
- **Exporting** a body as a STEP file.
- **Saving.** Save and Save As (Ctrl+S, Ctrl+Shift+S, or search) write the whole design, its sketches, constraints and history to a readable `.omamake` file, in `~/Documents/OmaMake` unless you pick another folder. Opening comes next.
- **One history.** Every action is kept in order and Ctrl+Z steps back through all of them, in and out of sketches. The Timeline along the bottom shows it: a sketch is one point there, and inside a sketch you see only that sketch's own steps. Drag the marker back and forth to see the design as it was at any step; in an opened file the steps are rebuilt in the background while you keep working, and a faint fill shows how far they're ready.

## Building

Needs Qt 6 (base, declarative, quick3d, shadertools), [Manifold](https://github.com/elalish/manifold), [OpenCascade](https://dev.opencascade.org), ccache and mold.

```sh
mkdir build && cd build
qmake6 .. && make -j2
./omamake
```

A new design starts empty. `OMAMAKE_TEST_CUBE=1 ./omamake` places a 12 inch cube to try faces on.

## Goals

- **Simple to look at, powerful underneath.** A near-empty screen like SolveSpace, with every feature you'd expect from Fusion 360 one search away.
- **Parametric history.** Every step is kept and can be edited later. No code to write.
- **Assemblies.** Nested components, joints between parts, and reference images, decals and notes kept right in the component tree.
- **Opens almost anything.** STEP, IGES, STL, 3MF, OBJ and more, with Fusion 360 `.f3d` files as a top priority.
- **Made for making.** 3D printing first, basic CAM later. Threads are real modelled geometry by default.
- **Looks good, runs light.** A pretty viewport with a simple-look switch for older hardware.
- **Extensible.** Features are plugins, and the geometry engine can be swapped behind one interface.

## License

GPL-3.0-or-later. See [LICENSE](LICENSE).
