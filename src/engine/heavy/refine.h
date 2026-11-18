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

// True if every face that goes all the way round (a cylinder or cone with a wrap-around line) can be
// cut into triangles to be drawn. OpenCascade's merge can join a hole's two halves into one face that
// checks out valid yet can't be drawn (hinge_female's countersinks, round 6): a merge that makes such
// a face isn't used. Each such face is tried alone, as a copy, at a coarse size.
bool roundsDraw(const TopoDS_Shape &shape);

// OpenCascade's merge (ShapeUpgrade_UnifySameDomain) at the given tolerances, every edge kept that
// lies between two faces of one cylinder or cone, when merging those would leave a round face that
// can't be drawn (roundsDraw); everything else merged as usual. Null if the merge failed.
TopoDS_Shape unifyDrawable(const TopoDS_Shape &shape, double linear, double angular);

} // namespace refine
