# OmaMake

A parametric 3D CAD program made for [Omarchy](https://omarchy.org), and for any Linux system.

**Status: early development.** It runs, but it can't make anything yet: this is the groundwork the modelling tools will stand on.

## What works so far

- **The view.** Orbit, pan and zoom, a view cube, a floor grid, studio-style lighting, dark and light looks (or the current Omarchy theme), and see-through backgrounds that follow OmaSkins.
- **Search for every command** with Alt+Space.
- **The Browser**, the design's tree on the left: nested components, bodies, sketches and each component's origin, with an eye on every row to show or hide it.
- **The origin**: centre point, axes and planes you can hover and select. Planes come in two styles, Cross (full size, through the centre) or Cube Corner (smaller, plus side only), for all of them or each one.
- **Sketches.** Triple-click an origin plane, a flat face or empty space to start one; Esc or Finish Sketch to leave, and the view flies back to where it was. No drawing tools yet.
- **One history.** Every action is kept in order and Ctrl+Z steps back through all of them, in and out of sketches. The Timeline along the bottom shows it: a sketch is one point there, and inside a sketch you see only that sketch's own steps.

## Building

Needs Qt 6 (base, declarative, quick3d, shadertools), [Manifold](https://github.com/elalish/manifold), ccache and mold.

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
