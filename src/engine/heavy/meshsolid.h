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

// What the person said the mesh is, asked on Import Mesh (owner, 2026-11-10, round 4: "faceted holes
// and corners are almost never intended, so ask"). Detailed: as before. LowPoly: the facets are the
// look; neighbours within kLowPolyDegrees of one another are joined into one flat and flattened onto
// it, and nothing is fitted to curves. LowRes: a coarse export of a smooth part; Fit takes a curve
// from as few as kLowResFewest triangles (2 facets), so a pocket corner cut into 2-3 facets is one.
enum class Style { Detailed, LowPoly, LowRes };
constexpr double kLowPolyDegrees = 3.0;     // Detailed joins under 1 degree
constexpr int kLowResFewest = 4;            // Detailed needs 6 to start a curve and 8 to keep it
constexpr int kLowResFewestAround = 10;     // Low Res: a whole round (hole, boss) with fewer facets round it stays facets
constexpr double kLowResWidth = 1.6;        // Low Res: a round's facets are within this of one another's width
// GB-092 (owner, 2026-11-17): a piece of an STL sharing no triangle side with the rest and smaller than this
// across (its box's diagonal) is floating debris and is dropped on reading. Nothing joined to the model is touched.
constexpr double kDetachedSpeck = 0.02;     // mm

// A solid (a shell if the mesh has open edges), each flat area one face.
TopoDS_Shape solid(const Mesh &mesh, const Stage &stage, Style style = Style::Detailed);

struct FitReport {
    int cylinders = 0, cones = 0, spheres = 0;
    int bowls = 0;              // smooth caps of second degree (a paraboloid dome), one face each
    int facetsLeft = 0;         // triangles no round surface fitted
    double worstMm = 0;         // the furthest a point of the mesh ended up from its surface
};

// Fit Curved Surfaces: facets that follow a cylinder, cone or sphere to within `share` of its
// radius become that surface; everything else stays as it is. Null if nothing could be built.
TopoDS_Shape fit(const Mesh &mesh, double share, const Stage &stage, FitReport &report, Style style = Style::Detailed);

} // namespace meshsolid
