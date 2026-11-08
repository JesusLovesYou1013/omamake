#pragma once

// A flat face swept straight with its sides leaning, the way a commercial kernel drafts them: every
// edge of the outline gets its own drafted side (a straight edge a leaning plane, an arc a cone, a
// spline or an ellipse the surface its ruling lines sweep), and each corner runs up where the two
// sides beside it truly meet, so the top outline is the bottom one grown or shrunk with sharp
// corners. OpenCascade's own draft tools only lean flat and round sides (Draft_Modification_1.cxx
// refuses any other curve; LocOpe_DPrism comes out invalid on splines). Heavy engine only.

#include <TopoDS_Face.hxx>
#include <TopoDS_Shape.hxx>
#include <gp_Vec.hxx>

#include <string>

namespace taper {

// `face` swept along `travel` (square to it), its outline growing by `grow` mm for every mm it
// travels (negative shrinks). Null, and why, when the sides can't be made: an edge that would shrink
// to nothing before the top, sides that would run into each other, a corner the sides never meet.
TopoDS_Shape prism(const TopoDS_Face &face, const gp_Vec &travel, double grow, std::string &why);

} // namespace taper
