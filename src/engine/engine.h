#pragma once

// The one door between OmaMake and a geometry engine.
//
// The UI, the document and every plugin talk only to GeometryEngine. An engine
// (Lite = Manifold3D triangles, Heavy = OpenCascade exact surfaces, or any later
// one) is a drop-in replacement: implement this class, nothing else changes.
// Every function here gets an entry in ENGINE-HANDOFF.md.
//
// Units: millimetres everywhere inside the engine. Inches are a display matter.

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace omamake {

using BodyId = std::uint64_t;   // 0 = no body

struct Point2 {
    double x = 0;
    double y = 0;
};

// A closed outline; the first loop is the outside, any further loops are holes.
struct Profile {
    std::vector<std::vector<Point2>> loops;
};

enum class Combine { Join, Cut, Intersect };

// Ready-to-draw triangles plus the outline edges a CAD view shows.
struct Mesh {
    std::vector<float> positions;        // x,y,z per vertex
    std::vector<float> normals;          // x,y,z per vertex
    std::vector<std::uint32_t> indices;  // 3 per triangle
    std::vector<float> edgeLines;        // x,y,z,x,y,z per outline edge
    float min[3] = {0, 0, 0};
    float max[3] = {0, 0, 0};
};

class GeometryEngine {
public:
    virtual ~GeometryEngine() = default;

    virtual std::string name() const = 0;

    // A box with one corner at the origin.
    virtual BodyId box(double x, double y, double z) = 0;
    // A flat profile on the XY plane pushed up along +Z.
    virtual BodyId extrude(const Profile &profile, double height) = 0;
    virtual BodyId combine(BodyId a, BodyId b, Combine op) = 0;
    virtual BodyId translate(BodyId body, double x, double y, double z) = 0;

    virtual Mesh mesh(BodyId body) const = 0;
    virtual void release(BodyId body) = 0;
};

std::unique_ptr<GeometryEngine> makeLiteEngine();

} // namespace omamake
