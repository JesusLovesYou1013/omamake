#pragma once

// A body's own flat face moved along its normal, the way a local face move works in a commercial
// kernel: the face is put on its new plane, each neighbour keeps its very own surface and only its
// edge on the moved face changes, and each edge running up from a corner of the face is carried on
// along its own line. Every other face, edge and corner stays exactly as it was. So pulling a face
// out never makes a wall of new faces beside the old ones, and there's no line to merge away
// (owner, 2026-11-05: "this literally should be just taking what is already there and pulling it
// out"). Heavy engine only.

#include <TopoDS_Face.hxx>
#include <TopoDS_Shape.hxx>
#include <gp_Vec.hxx>

#include <string>

namespace pushface {

// `body` with `face` moved by `move` (which must be along the face's normal). Null, and why, when it
// can't be done this way: a face that isn't flat, a corner without exactly one straight edge running
// up from it, an edge that would turn round (the face moved past its far end), a neighbour the new
// plane doesn't meet, or a result that doesn't check out.
TopoDS_Shape move(const TopoDS_Shape &body, const TopoDS_Face &face, const gp_Vec &move, std::string &why);

} // namespace pushface
