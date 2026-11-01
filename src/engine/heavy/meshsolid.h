#pragma once

// Mesh to solid (owner, 2026-10-30): triangles become a real solid, true flats one face each, and
// on request facets standing in for a round surface become that surface. Heavy engine only.

#include <TopoDS_Shape.hxx>

#include <array>
#include <functional>
#include <string>
#include <vector>

namespace meshsolid {

struct Mesh {
    std::vector<std::array<double, 3>> points;
    std::vector<std::array<int, 3>> triangles;    // each anticlockwise from outside
};

using Stage = std::function<void(const char *)>;

// STL (binary or text) or OBJ, points joined. Empty, with why, if it can't be read.
Mesh read(const std::string &path, std::string &why);

// Triangles of a shape's drawing mesh, points joined by position.
Mesh fromShape(const TopoDS_Shape &shape);

// A solid (a shell if the mesh has open edges), each flat area one face.
TopoDS_Shape solid(const Mesh &mesh, const Stage &stage);

struct FitReport {
    int cylinders = 0, cones = 0, spheres = 0;
    int facetsLeft = 0;         // triangles no round surface fitted
    double worstMm = 0;         // the furthest a point of the mesh ended up from its surface
};

// Fit Curved Surfaces: facets that follow a cylinder, cone or sphere to within `share` of its
// radius become that surface; everything else stays as it is. Null if nothing could be built.
TopoDS_Shape fit(const Mesh &mesh, double share, const Stage &stage, FitReport &report);

} // namespace meshsolid
