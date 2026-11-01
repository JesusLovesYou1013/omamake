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
#include <functional>
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

// Ready-to-draw triangles plus the outline edges a CAD view shows, plus the
// topology a user hovers and selects: faces, edges and corner points.
struct Mesh {
    std::vector<float> positions;        // x,y,z per vertex
    std::vector<float> normals;          // x,y,z per vertex
    std::vector<std::uint32_t> indices;  // 3 per triangle
    std::vector<float> edgeLines;        // x,y,z,x,y,z per outline edge
    float min[3] = {0, 0, 0};
    float max[3] = {0, 0, 0};

    std::uint32_t faceCount = 0;
    std::vector<std::uint32_t> triangleFace;   // which face each triangle belongs to
    std::vector<std::vector<float>> edges;     // each edge as a polyline: x,y,z,x,y,z,...
    std::vector<float> points;                 // x,y,z per corner point (where edges meet)
};

// What Fit Curved Surfaces did.
struct FitResult {
    int cylinders = 0, cones = 0, spheres = 0;
    int trianglesLeft = 0;      // of the source triangles, those still flat facets
    double worstMm = 0;         // the furthest a source point ended up from its surface
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
    // Read a STEP file, in its own position and sized in mm whatever its units. 0 and why if it
    // can't be. stage is told what's happening, in a few words. May run off the main thread:
    // nothing else may call the engine until it returns.
    virtual BodyId importStep(const std::string &path, std::string &why,
                              const std::function<void(const char *)> &stage) = 0;

    // Read an STL or OBJ into a solid, its flat areas one face each. Same rules as importStep.
    virtual BodyId importMesh(const std::string &path, std::string &why,
                              const std::function<void(const char *)> &stage) = 0;
    // A new body with facets standing in for a round surface turned into that surface, kept
    // within `share` of its radius; `body` stays as it was. 0 and why if nothing could be fitted.
    virtual BodyId fitSurfaces(BodyId body, double share, FitResult &result, std::string &why,
                               const std::function<void(const char *)> &stage) = 0;
    // Write a body as a STEP file, in mm. False and why if it can't be.
    virtual bool exportStep(BodyId body, const std::string &path, std::string &why) const = 0;
    // A body's exact solid as bytes and back, for the design file. Empty / 0 if not possible.
    virtual std::string saveBody(BodyId body) const = 0;
    virtual BodyId loadBody(const std::string &bytes) = 0;

    virtual Mesh mesh(BodyId body) const = 0;
    virtual void release(BodyId body) = 0;
};

std::unique_ptr<GeometryEngine> makeLiteEngine();
std::unique_ptr<GeometryEngine> makeHeavyEngine();

} // namespace omamake
