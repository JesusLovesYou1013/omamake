#pragma once

// Project (Fusion's name, owner 2026-10-31): the shadow a set of triangles throws straight down
// onto a sketch, read back as lines, arcs and circles. It works from triangles, so both engines
// give the same result.

#include <array>
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
// `tol` of the triangles' points.
std::vector<std::vector<Piece>> outline(const std::vector<std::array<Flat, 3>> &triangles, double tol);

} // namespace projection
