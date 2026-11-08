#pragma once

// Project, exactly (owner, 2026-11-06: "the resulting profile that is projected precisely matches every
// single geometric detail that we are trying to project ... every single option"). Everything is taken
// from the model's own edges and surfaces, never from the triangles it is drawn with:
//
// - an edge is its own curve laid flat: a line stays a line, a circle parallel to the sketch stays that
//   circle, one seen at a slant becomes its exact ellipse, one seen edge on the line between its two
//   farthest points; anything else is a spline within kProjectFit of the true flat curve;
// - an area (the whole model, or one face) is its true shadow, the way Fusion and the other commercial
//   modellers make it: every edge laid flat, plus the outline of each curved face that folds over as
//   seen from the sketch (OpenCascade's hidden-line outlines, HLRBRep), cut wherever they meet; a piece
//   is outline when a line straight down just to one side of it meets the model and just to the other
//   side doesn't (meeting judged on each face's own boundary curves, FaceInside); pieces lying along
//   one another within the model's looseness count once; each kept piece is the exact curve it came
//   from. One flat face seen square on is simply its own wires laid flat.
//
// Heavy engine only.

#include "engine/engine.h"

#include <TopoDS_Edge.hxx>
#include <TopoDS_Shape.hxx>
#include <TopoDS_Vertex.hxx>

#include <string>
#include <vector>

namespace projectexact {

// The shadow these shapes (solids, or one face) throw straight down on the plane: closed outlines with
// the shadow on their left, the outside ones first. Nothing covered (a face seen edge on): its edges
// laid flat, each a loop of its own. False and why when it can't be worked out.
bool shadow(const std::vector<TopoDS_Shape> &shapes, const omamake::Placement &plane,
            std::vector<std::vector<omamake::FlatPiece>> &loops, std::string &why);

// One edge laid flat on the plane (a Point when it is seen end on).
bool edge(const TopoDS_Edge &edge, const omamake::Placement &plane, std::vector<omamake::FlatPiece> &pieces, std::string &why);

// One corner laid flat on the plane.
omamake::FlatPiece corner(const TopoDS_Vertex &vertex, const omamake::Placement &plane);

} // namespace projectexact
