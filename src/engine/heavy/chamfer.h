#pragma once

// Model Chamfer on the exact engine (owner, 2026-11-08: "match all the features that Fusion 360 [has], and
// ... more reliable than Fusion's version where it will do its best to apply the chamfer and blend it into
// the surrounding geometry"). OpenCascade's own chamfer first (BRepFilletAPI_MakeChamfer), for every edge
// at once. An edge it can't do (most often a chamfer too big for the faces beside it, which Fusion refuses)
// RUNS THROUGH instead (owner's choice, 2026-11-08): the cross-section the chamfer would have (corner, a point
// on each face) is swept along the edge and cut away (added, on an inside edge), so the slope keeps its size
// and carries on through whatever lies beside the edge, meeting the surrounding faces wherever it hits them.
// Only an edge that even that can't build is left sharp, and named.
//
// Model Fillet goes the same way (owner, 2026-11-09, GB-051: "add that with the same concepts"): an Item of
// kind Fillet (d1 the radius) or Chord (d1 the chord between where the round meets the faces) is OpenCascade's
// BRepFilletAPI_MakeFillet first, and runs through the same way, its cross-section the corner less the round
// (two straight sides and the arc between the points where it touches the faces).
//
// Corners where chamfered edges meet (Fusion's Corner Type):
//  - Chamfer: OpenCascade's own corner, a flat patch joining the bevels.
//  - Miter:   every edge as its swept cross-section, so the bevels run on until they meet in a point.
//  - Blend:   OpenCascade's corner patch replaced by a surface filled in tangent to the bevels round it.

#include <TopoDS_Edge.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Shape.hxx>

#include <functional>
#include <string>
#include <vector>

namespace chamfer {

enum class Kind { Equal, Two, Angle, Fillet, Chord };   // the last two round the edge (d1: radius / chord length)
inline bool rounds(Kind k) { return k == Kind::Fillet || k == Kind::Chord; }
enum class Corner { Chamfer, Miter, Blend };

struct Item {
    TopoDS_Edge edge;
    TopoDS_Face ref;        // the face d1 is measured on (the angle too); the other face takes d2
    Kind kind = Kind::Equal;
    double d1 = 1, d2 = 1;  // mm
    double angle = 0.7853981633974483;   // radians, between the bevel and `ref`
    int id = -1;            // the edge's number, for the report
    bool sweep = false;     // GB-055: always run through (exact turned/swept cut), not OpenCascade's own chamfer
};

struct Report {
    std::vector<int> ranThrough;   // built as a swept cut (or fill) instead of OpenCascade's chamfer
    std::vector<int> skipped;      // left sharp
    std::vector<std::string> notes;
    bool consumed = false;         // the bevels, meeting, would take the whole body
};

using Check = std::function<bool(const TopoDS_Shape &)>;
using Log = std::function<void(const std::string &)>;
// The engine's own checked join (cut false) or cut of `tools` from `shape`: null when it can't be built soundly.
using Boolean = std::function<TopoDS_Shape(const TopoDS_Shape &shape, const std::vector<TopoDS_Shape> &tools, bool cut)>;

// `body` with the items chamfered. Null when nothing at all could be done (report.skipped holds every edge).
TopoDS_Shape make(const TopoDS_Shape &body, const std::vector<Item> &items, Corner corner, const Check &sound,
                  const Boolean &boolean, const Log &log, Report &report);

// The other face beside `edge` in `body` (null when the edge hasn't two different faces: a seam, a free edge).
TopoDS_Face otherFace(const TopoDS_Shape &body, const TopoDS_Edge &edge, const TopoDS_Face &face);

// Where a chamfer of `item` lies, `at` along its edge (0 its start, 1 its end; halfway by default): the edge's
// point, and the bevel's two ends (on `ref` and on the other face; a fillet's: where its round touches them).
// False when the edge's faces can't say.
bool section(const TopoDS_Shape &body, const Item &item, double out[9], double at = 0.5);

// A point's chamfer or fillet (owner, 2026-11-09, GB-053: "flatten that point off based on its surrounding
// angles so that the angle applied to it would essentially be equal compared to the other angles around it").
// The point's axis `n` is the way out of it that makes the same angle with every face round it (n . n_i
// equal for each face's outward normal n_i: exact for three faces, least squares for more; a cone's tip: its
// axis). Chamfer: a flat across n, `size` from the point. Fillet: the ball of radius `size` (Chord: the
// circle it touches the faces in has that diameter) set in on n until it touches the faces, and what lies
// past where it touches them taken off round it: a cone's tip comes out a smooth round tip.
struct Point {
    double at[3] = {0, 0, 0};   // where it is (found again by place on the body as it is by then)
    Kind kind = Kind::Equal;    // Equal (Two / Angle take d1): a flat; Fillet / Chord: a round
    double size = 1;            // mm
    int id = -1;                // for the report
};
// The points' cuts on `body`, all at once (one at a time if that fails, each that won't go left as it was).
// `around` (GB-055): the body the points are on and their cuts are worked out from, when `body` is that body
// with the set's edges already done: each cut is what the point takes from the original, so cutting it from
// what the edges left gives the same as both at once, whichever goes first, even where they overlap or the
// edges' rounds took the point itself.
TopoDS_Shape points(const TopoDS_Shape &body, const std::vector<Point> &pts, const Check &sound, const Boolean &boolean,
                    const Log &log, Report &report, const TopoDS_Shape &around = TopoDS_Shape());
// Where a point's flat (or round's tip) lies: the point, then that spot twice (9 numbers, as section()).
bool pointSection(const TopoDS_Shape &body, const Point &pt, double out[9]);
// A tip: a corner where a surface closes to a point that is sharp there (a cone's apex), not smooth (a
// sphere's pole). Shown as a corner so it can be picked.
bool isTip(const TopoDS_Shape &body, const TopoDS_Shape &vertex);

} // namespace chamfer
