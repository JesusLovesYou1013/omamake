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
    // Optional, per loop and per point: the centre of the circle the step from that point to the next
    // lies on, NaN when it's straight. Lets an engine build true arcs instead of many short flats.
    std::vector<std::vector<Point2>> centres;
    // Optional: the sketch's other curves exactly (splines, ellipses, conics), and per loop and per
    // step which of them it lies on (-1: none) and where along it its two ends are. Lets an engine
    // build each curve as one smooth edge instead of the many short flats it was drawn with.
    struct Curve {
        // A (rational) B-spline, or an ellipse: centre, the end of its long axis, its short radius.
        bool ellipse = false;
        int degree = 3;
        std::vector<Point2> poles;
        std::vector<double> weights;   // empty: all 1
        std::vector<double> knots;
        std::vector<int> mults;
        Point2 centre, major;
        double minor = 0;
        // A step's "along" is its place among the sketch's samples of the curve (sample k = k, a
        // point part way between two samples a fraction); the curve's parameter is shift + scale * along.
        double shift = 0, scale = 1;
    };
    std::vector<Curve> curves;
    std::vector<std::vector<int>> onCurve;
    std::vector<std::vector<std::pair<double, double>>> along;
};

enum class Combine { Join, Cut, Intersect };

// One of a body's edges seen straight down on a sketch, at full precision: a line, or a circle or arc
// lying parallel to the sketch. Project draws these instead of what it traced off the drawing mesh.
struct FlatCurve {
    bool round = false;     // a circle or an arc of one
    bool whole = false;     // a whole circle (a == b)
    Point2 a, b;            // its ends
    Point2 c;               // round: the centre
    double r = 0;           // round: the radius
};

// Project, exactly (owner, 2026-11-06: "the resulting profile that is projected precisely matches every
// single geometric detail"): one piece of what the model throws on a sketch, taken from the model's own
// edges and surfaces, never from the drawing mesh.
struct FlatPiece {
    enum Kind { Point, Line, Arc, Circle, Ellipse, Spline } kind = Line;
    Point2 a, b;                // Point: a. Line: its ends. Arc: from a round to b counter-clockwise.
                                // Circle: a on its rim. Spline: its ends (its first and last pole).
    Point2 c;                   // Arc, Circle, Ellipse: the centre
    Point2 major, minor;        // Ellipse: the end of its long axis, the end of its short axis
    std::vector<Point2> poles;  // Spline: a clamped cubic B-spline on uniform knots (a sketch's
                                // Control Point Spline), within kProjectFit of the true curve
};
// How close a projected curve that is no line, circle or whole ellipse is drawn to the true one, in mm.
constexpr double kProjectFit = 1e-5;

// A flat sheet in space: a profile's (u, v) points sit at origin + u*x + v*y; the normal is x cross y.
struct Placement {
    double origin[3] = {0, 0, 0};
    double x[3] = {1, 0, 0};
    double y[3] = {0, 1, 0};
};

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

// Model Chamfer and Model Fillet (one pipeline, GB-051): one edge to chamfer or round, as its selection set gives it.
struct ChamferEdge {
    int edge = -1;          // numbered as in the body's mesh
    int set = 0;            // its selection set: d1 is measured on the face most of the set's edges share
    int kind = 0;           // chamfer: 0 Equal Distance, 1 Two Distances, 2 Distance and Angle; fillet: 3 radius d1, 4 chord length d1
    double d1 = 1, d2 = 1;  // mm
    double angle = 45;      // degrees, between the bevel and d1's face
    bool flip = false;      // d1 (and the angle) on the other face
    double at = 0.5;        // chamferSections only: where along the edge, 0 its start to 1 its end
    int point = -1;         // GB-053: a corner (point) instead of an edge, numbered as in the body's mesh points;
                            // chamfer: a flat across the point's equal-angle axis d1 in, fillet: a round of radius
                            // d1 (chord: d1 the diameter of the circle it touches the faces in)
};
enum class ChamferCorner { Chamfer, Miter, Blend };
struct ChamferReport {
    std::vector<int> ranThrough;    // too big for the faces beside them: carried on through what lies there
    std::vector<int> skipped;       // left sharp
    std::string note;
};

class GeometryEngine {
public:
    virtual ~GeometryEngine() = default;

    virtual std::string name() const = 0;

    // A box with one corner at the origin.
    virtual BodyId box(double x, double y, double z) = 0;
    // A flat profile on the XY plane pushed up along +Z.
    virtual BodyId extrude(const Profile &profile, double height) = 0;
    // The profile on `place`, pushed from `from` to `to` along its normal (either may be negative;
    // from < to or the other way). `taper` in degrees, Fusion's way round: positive widens the shape
    // as it goes, negative narrows it. 0 if it can't be made (the Lite engine makes no taper).
    virtual BodyId extrudeOn(const Profile &profile, const Placement &place, double from, double to, double taper) = 0;
    // A flat face of a body (its number in the body's mesh, as faceProfile) pushed exactly as it is: its
    // own edges and curves, nothing sampled, so the extrude meets the body edge for edge. `place` is the
    // one faceProfile gives; `from`, `to` and `taper` as extrudeOn. 0 if it can't be made (the Lite
    // engine has no faces; the caller then pushes the face's outline).
    virtual BodyId extrudeFace(BodyId body, int face, const Placement &place, double from, double to, double taper) = 0;
    // A body's own flat face (numbered as faceProfile) moved `distance` along `place`'s normal, its
    // neighbouring faces carried on along their own surfaces to meet it: the body itself pulled out or
    // pushed in, with no new walls and so no line where it was. 0 and why when it can't be done that
    // way (the caller then builds and combines as before); the Lite engine never can.
    virtual BodyId pushFace(BodyId body, int face, const Placement &place, double distance, std::string &why) = 0;
    // `a` joined with, cut by or intersected with all of `tools` in one go (for Intersect, OpenCascade's
    // Common: what lies inside every tool). 0 if the result isn't a valid solid of a size that fits.
    // `fuzzy` (Fuzzy profile, 0: off; Extrude passes it only on its automatic second try): if the exact Boolean
    // can't be built, it's tried once more with points that close (mm) taken as one, for a model whose own
    // faces don't quite meet.
    virtual BodyId combine(BodyId a, const std::vector<BodyId> &tools, Combine op, double fuzzy = 0) = 0;
    BodyId combine(BodyId a, BodyId b, Combine op, double fuzzy = 0) { return combine(a, std::vector<BodyId>{b}, op, fuzzy); }
    // Why the last extrudeOn, extrudeFace or combine that gave 0 couldn't be done, in words for the
    // person ("the result would touch itself along a line or at a point, ..."); empty when the engine
    // can't say.
    virtual std::string whyNot() const { return {}; }
    // Everything someone troubleshooting needs about one body, a line each: its solids, their shells
    // and whether each is closed, faces marked internal, the engine's own check and what it finds,
    // edges with a face on one side only, the loosest edge, volume, box. True if it's a sound solid.
    virtual bool inspect(BodyId body, std::string &report) const { (void)body; report.clear(); return true; }
    // What the engine did since the last call, a step a line: for a failure's log.
    virtual std::string takeTrace() { return {}; }
    // The separate solids a body is made of, each as a body of its own, biggest first (volume), when there's
    // more than one; empty when it's a single solid. The geometry is the body's own, unchanged.
    virtual std::vector<BodyId> pieces(BodyId body) { (void)body; return {}; }
    // The body with faces lying in one surface made one (as a Boolean's result already is): a new body from
    // a sketch circle otherwise has its side in two halves with a line down it. A new body, or 0 if there was
    // nothing to merge or the merged one wouldn't be exactly as sound, as tight and the same size.
    virtual BodyId merged(BodyId body) { (void)body; return 0; }
    // How loose a body is (mm): its loosest edge or corner, how far apart the pieces it joins may lie (an
    // exactly made model: about 1e-7). 0 when the engine can't tell (the mesh engine).
    virtual double looseness(BodyId body) const { (void)body; return 0; }
    // Volume of a body (mm³), and the gap between two (0 when they touch or overlap; capped at `search`).
    virtual double volume(BodyId body) const = 0;
    virtual double gap(BodyId a, BodyId b, double search) const = 0;
    // A flat face of a body (by its number in the body's mesh) as the sheet it lies on, its normal
    // pointing out of the body, and its outline in that sheet (the first loop the outside, the rest holes).
    // False if the face is curved or there is no such face (the Lite engine has no faces to give).
    virtual bool faceProfile(BodyId body, int face, Placement &place, Profile &profile) const = 0;
    // A face's exact plane (a point on it and its outward normal), when it is a true plane. False
    // otherwise, or when the engine only has triangles.
    virtual bool planeOfFace(BodyId body, int face, double origin[3], double normal[3]) const = 0;
    // How far along `way` (a unit vector) from `start` the body's face, edge or corner (kind 'f', 'e',
    // 'p'; numbered as in its mesh) lies, on the exact shape. False when it can't say (the Lite engine).
    virtual bool reach(BodyId body, char kind, int index, const double start[3], const double way[3], double &mm) const = 0;
    // A body's edges (all of them, or one face's: face = its own index, -1 for all) and corners, seen
    // straight down on the plane, in the plane's (u, v). Edges that come out as anything but a line or
    // a circle parallel to the plane are left out. False if the engine has no exact edges.
    virtual bool flatCurves(BodyId body, int face, const Placement &plane, std::vector<FlatCurve> &curves,
                            std::vector<Point2> &corners) const = 0;
    // Project, exactly: the area these bodies (face -1), or one face of the first (its own index), cover
    // seen straight down on the plane, as its outlines: the outside first, then any holes, each piece
    // running round with the area on its left. Built from the exact edges and the curved faces' exact
    // outlines, never the drawing mesh. A face seen edge on covers no area: its edges come back as
    // loose pieces. False and why when it can't (the Lite engine has no exact edges).
    virtual bool shadow(const std::vector<BodyId> &bodies, int face, const Placement &plane,
                        std::vector<std::vector<FlatPiece>> &loops, std::string &why) const {
        (void)bodies; (void)face; (void)plane; (void)loops;
        why = "this engine has no exact edges";
        return false;
    }
    // Project, exactly: one edge ('e') or corner ('p') of a body, numbered as in its mesh, straight down
    // onto the plane: a point, a line, an arc or circle, a whole ellipse, or a spline within kProjectFit.
    virtual bool flatPart(BodyId body, char kind, int index, const Placement &plane, std::vector<FlatPiece> &pieces,
                          std::string &why) const {
        (void)body; (void)kind; (void)index; (void)plane; (void)pieces;
        why = "this engine has no exact edges";
        return false;
    }
    // Model Chamfer / Fillet: a new body, `body` with these edges bevelled (or rounded, kinds 3 and 4: all of one
    // call are one or the other); corners where they meet as `corner` says (chamfers).
    // An edge too big for its faces runs through (see ENGINE-HANDOFF "Chamfer"); `report` names those and any
    // left sharp. 0 and whyNot() when nothing could be done (the Lite engine has no edges to chamfer).
    virtual BodyId chamfer(BodyId body, const std::vector<ChamferEdge> &edges, ChamferCorner corner, ChamferReport &report) {
        (void)body; (void)edges; (void)corner;
        report.note = "Chamfer and Fillet need the exact engine";
        return 0;
    }
    // One face's edges (numbered as in the mesh); every edge joined on smoothly to these, on and on (Tangent
    // Chain); and a feature's edges: those of `now` on faces lying where `after` has a face `before` hadn't
    // (before 0: all of after's).
    virtual std::vector<int> faceEdges(BodyId body, int face) const { (void)body; (void)face; return {}; }
    virtual std::vector<int> tangentChain(BodyId body, const std::vector<int> &edges) const { (void)body; return edges; }
    virtual std::vector<int> featureEdges(BodyId now, BodyId before, BodyId after) const { (void)now; (void)before; (void)after; return {}; }
    // Where each edge's chamfer lies halfway along it: 9 numbers (the edge's point, the bevel's end on d1's face,
    // on the other face), empty when it can't say.
    virtual std::vector<std::vector<double>> chamferSections(BodyId body, const std::vector<ChamferEdge> &edges) const {
        (void)body;
        return std::vector<std::vector<double>>(edges.size());
    }
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
