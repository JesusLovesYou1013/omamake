#pragma once

// Faces lying in one surface made one face, the way FreeCAD's Refine does it (src/Mod/Part/App/
// modelRefine.cpp), our own way. OpenCascade's ShapeUpgrade_UnifySameDomain gave up on a whole shell
// when one group failed, and couldn't join a wall built turning one way to one turning the other
// (owner, 2026-11-04: a face cut out and put back left its line). Heavy engine only.

#include <TopoDS_Shape.hxx>

namespace refine {

// The shape with every group of neighbouring faces on exactly one plane, cylinder or cone made one
// face, and straight edges left cut in two by a corner that's gone made one edge. Exactly means
// within 0.0000001 mm and 0.0000000001 rad (Precision::Confusion and Angular, as FreeCAD): no face
// moves. Null if the result doesn't check out; `united` counts the groups joined.
TopoDS_Shape sameSurfaces(const TopoDS_Shape &shape, int *united = nullptr);

} // namespace refine
