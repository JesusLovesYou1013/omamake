#pragma once

// Project (Fusion's name, owner 2026-10-31): the shadow a set of triangles throws straight down
// onto a sketch, read back as lines, arcs and circles. It works from triangles, so both engines
// give the same result.

#include <array>
#include <cstddef>
#include <vector>

namespace projection {

struct Flat {
    double u = 0, v = 0;
};
struct Piece {
    enum Kind { Line, Arc, Circle } kind = Line;
    Flat a, b;      // Line: its ends. Arc: from a round to b counter-clockwise. Circle: a on its rim.
    Flat c;         // Arc, Circle: the centre
};

// The outlines of the area these triangles cover, the outside and any holes through it. Within
// `tol` of the triangles' points. outsideOnly (GB-095, Project of the whole body): only the outside
// of each separate region, no holes.
std::vector<std::vector<Piece>> outline(const std::vector<std::array<Flat, 3>> &triangles, double tol, bool outsideOnly = false);

// GB-095 (owner, 2026-11-17: projecting the whole body gives "no interior lines whatsoever at all, only
// the exterior"): of these loops (each a closed polygon, the last point joining the first), the ones to
// keep: each loop covering more than minArea, and on average wider than minWidth (twice its area over
// its length round), that lies inside no larger such loop. A hole, an island in a hole, and anything
// open, of no area or thinner than minWidth (a stray line there and back) go. When no loop covers any
// area (a face seen edge on), all of them are kept.
std::vector<std::size_t> outermost(const std::vector<std::vector<Flat>> &loops, double minArea, double minWidth = 0);

// The same area as plain closed polygons, the outside and any holes: where the true outline lies, give
// or take how far the triangles stray, plus tol. (Exact Project uses it only to tell which of the
// model's exact edges can be on the outline at all.)
std::vector<std::vector<Flat>> covering(const std::vector<std::array<Flat, 3>> &triangles, double tol);

} // namespace projection
