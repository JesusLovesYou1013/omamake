// Heavy engine: OpenCascade (exact surfaces: planes, cylinders, splines). Slower booleans than
// Lite, but a circle stays a circle; triangles are only made for drawing and picking.

#include "engine/engine.h"
#include "engine/heavy/meshsolid.h"

#include <BinTools.hxx>
#include <BRepAlgoAPI_Common.hxx>
#include <BRepAlgoAPI_Cut.hxx>
#include <BRepAlgoAPI_Fuse.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_MakePolygon.hxx>
#include <BRepBuilderAPI_Transform.hxx>
#include <BRepLib_ToolTriangulatedShape.hxx>
#include <BRepBndLib.hxx>
#include <BRepMesh_IncrementalMesh.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <BRepPrimAPI_MakePrism.hxx>
#include <BRep_Tool.hxx>
#include <Bnd_Box.hxx>
#include <IMeshTools_Parameters.hxx>
#include <Poly_PolygonOnTriangulation.hxx>
#include <Poly_Triangulation.hxx>
#include <STEPControl_Reader.hxx>
#include <STEPControl_Writer.hxx>
#include <ShapeCustom.hxx>
#include <Interface_Static.hxx>
#include <ShapeUpgrade_UnifySameDomain.hxx>
#include <Standard_Failure.hxx>
#include <TopExp.hxx>
#include <TopTools_IndexedDataMapOfShapeListOfShape.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Shape.hxx>
#include <gp_Trsf.hxx>

#include <algorithm>
#include <cmath>
#include <sstream>
#include <unordered_map>

namespace omamake {
namespace {

// Drawing triangles stray from the true surface by at most the model's size over kDeflectionShare
// (owner, 2026-10-30: a big table needn't be drawn as finely as a small part), kept between these
// (mm), and a curve turns at most this much (radians) between two of their points, so a small
// hole on a big model stays round. The solid itself stays exact whatever these are.
constexpr double kDeflectionShare = 50000;
constexpr double kDeflectionMin = 0.01;
constexpr double kDeflectionMax = 0.5;
constexpr double kAngle = 0.2;
constexpr double kInteriorShare = 5;

class HeavyEngine final : public GeometryEngine {
public:
    std::string name() const override { return "Heavy (OpenCascade)"; }

    BodyId box(double x, double y, double z) override {
        return keep(BRepPrimAPI_MakeBox(x, y, z).Shape());
    }

    BodyId extrude(const Profile &profile, double height) override {
        try {
            TopoDS_Face face;
            for (std::size_t i = 0; i < profile.loops.size(); ++i) {
                BRepBuilderAPI_MakePolygon polygon;
                for (const auto &p : profile.loops[i])
                    polygon.Add(gp_Pnt(p.x, p.y, 0));
                polygon.Close();
                TopoDS_Wire wire = polygon.Wire();
                if (i == 0) {
                    face = BRepBuilderAPI_MakeFace(wire, true).Face();
                } else {
                    wire.Reverse();
                    face = BRepBuilderAPI_MakeFace(face, wire).Face();
                }
            }
            return keep(BRepPrimAPI_MakePrism(face, gp_Vec(0, 0, height)).Shape());
        } catch (const Standard_Failure &) {
            return 0;
        }
    }

    BodyId combine(BodyId a, BodyId b, Combine op) override {
        const TopoDS_Shape &first = m_bodies.at(a);
        const TopoDS_Shape &second = m_bodies.at(b);
        try {
            switch (op) {
            case Combine::Join: return keep(BRepAlgoAPI_Fuse(first, second).Shape());
            case Combine::Cut: return keep(BRepAlgoAPI_Cut(first, second).Shape());
            case Combine::Intersect: return keep(BRepAlgoAPI_Common(first, second).Shape());
            }
        } catch (const Standard_Failure &) {
        }
        return 0;
    }

    BodyId translate(BodyId body, double x, double y, double z) override {
        gp_Trsf move;
        move.SetTranslation(gp_Vec(x, y, z));
        return keep(BRepBuilderAPI_Transform(m_bodies.at(body), move, true).Shape());
    }

    BodyId importStep(const std::string &path, std::string &why, const std::function<void(const char *)> &stage) override {
        try {
            stage("Reading the file");
            STEPControl_Reader reader;
            if (reader.ReadFile(path.c_str()) != IFSelect_RetDone) {
                why = "That isn't a STEP file OmaMake can read.";
                return 0;
            }
            stage("Building the solid");
            reader.TransferRoots();
            const TopoDS_Shape shape = reader.OneShape();
            if (shape.IsNull()) {
                why = "That STEP file holds no shapes.";
                return 0;
            }
            // Many programs write a hole as two half cylinders: one face and one rim is what a
            // person means by it. The surfaces themselves don't change.
            ShapeUpgrade_UnifySameDomain unify(shape, true, true, false);
            unify.Build();
            return keep(unify.Shape());
        } catch (const Standard_Failure &failure) {
            why = failure.GetMessageString();
            return 0;
        }
    }

    BodyId importMesh(const std::string &path, std::string &why, const std::function<void(const char *)> &stage) override {
        try {
            stage("Reading the file");
            const meshsolid::Mesh triangles = meshsolid::read(path, why);
            if (triangles.triangles.empty())
                return 0;
            const TopoDS_Shape shape = meshsolid::solid(triangles, stage);
            if (shape.IsNull()) {
                why = "That mesh couldn't be turned into a solid.";
                return 0;
            }
            const BodyId id = keep(shape);
            m_imported.emplace(id, triangles);
            return id;
        } catch (const Standard_Failure &failure) {
            why = failure.GetMessageString();
            return 0;
        }
    }

    BodyId fitSurfaces(BodyId body, double share, FitResult &result, std::string &why, const std::function<void(const char *)> &stage) override {
        try {
            stage("Reading the facets");
            // The file's own triangles if this body is as it came in: the solid's flats have been
            // joined, and meshing them again loses the corners that show where curves are.
            const auto imported = m_imported.find(body);
            const meshsolid::Mesh triangles = imported != m_imported.end() ? imported->second : meshsolid::fromShape(m_bodies.at(body));
            meshsolid::FitReport report;
            const TopoDS_Shape shape = meshsolid::fit(triangles, share, stage, report);
            if (shape.IsNull()) {
                why = "The curved surfaces couldn't be joined into a valid solid; nothing was changed.";
                return 0;
            }
            if (!report.cylinders && !report.cones && !report.spheres) {
                why = "No facets here follow a cylinder, cone or sphere closely enough.";
                return 0;
            }
            result = {report.cylinders, report.cones, report.spheres, report.facetsLeft, report.worstMm};
            return keep(shape);
        } catch (const Standard_Failure &failure) {
            why = failure.GetMessageString();
            return 0;
        }
    }

    bool exportStep(BodyId body, const std::string &path, std::string &why) const override {
        try {
            Interface_Static::SetCVal("write.step.unit", "MM");
            // A tight fillet is a torus whose tube is wider than its ring. Written as a revolved
            // circle, Fusion takes the wrong branch of it and dents the surface in. As an exact
            // spline there's only the patch itself to read.
            const TopoDS_Shape shape = ShapeCustom::ConvertToBSpline(m_bodies.at(body), false, true, false, false);
            STEPControl_Writer writer;
            if (writer.Transfer(shape, STEPControl_AsIs) != IFSelect_RetDone) {
                why = "That body couldn't be put in a STEP file.";
                return false;
            }
            if (writer.Write(path.c_str()) != IFSelect_RetDone) {
                why = "Couldn't write that file.";
                return false;
            }
            return true;
        } catch (const Standard_Failure &failure) {
            why = failure.GetMessageString();
            return false;
        }
    }

    std::string saveBody(BodyId body) const override {
        std::ostringstream out(std::ios::binary);
        BinTools::Write(m_bodies.at(body), out, false, false, BinTools_FormatVersion_CURRENT);
        return out.str();
    }

    BodyId loadBody(const std::string &bytes) override {
        try {
            std::istringstream in(bytes, std::ios::binary);
            TopoDS_Shape shape;
            BinTools::Read(shape, in);
            return shape.IsNull() ? 0 : keep(shape);
        } catch (const Standard_Failure &) {
            return 0;
        }
    }

    Mesh mesh(BodyId body) const override;

    void release(BodyId body) override {
        m_bodies.erase(body);
        m_imported.erase(body);
    }

private:
    BodyId keep(const TopoDS_Shape &shape) {
        const BodyId id = ++m_lastId;
        m_bodies.emplace(id, shape);
        return id;
    }

    std::unordered_map<BodyId, TopoDS_Shape> m_bodies;
    std::unordered_map<BodyId, meshsolid::Mesh> m_imported;    // the triangles an imported mesh came in as
    BodyId m_lastId = 0;
};

// Each true face is cut into triangles (smooth normals from its own surface, so a cylinder
// reads round); edges and corner points are the shape's real ones, drawn along the same
// triangle points so outlines sit exactly on the faces.
Mesh HeavyEngine::mesh(BodyId body) const {
    Mesh out;
    const TopoDS_Shape &shape = m_bodies.at(body);
    Bnd_Box box;
    BRepBndLib::Add(shape, box);
    IMeshTools_Parameters mesher;
    mesher.Deflection = std::clamp(std::sqrt(box.SquareExtent()) / kDeflectionShare, kDeflectionMin, kDeflectionMax);
    mesher.Angle = kAngle;
    // Inside a face and between points: left alone, a long thread face gets millions of slivers.
    mesher.DeflectionInterior = mesher.Deflection * kInteriorShare;
    mesher.MinSize = mesher.Deflection * kInteriorShare;
    mesher.InParallel = true;
    mesher.MeshAlgo = IMeshTools_MeshAlgoType_Delabella;   // ten times quicker on a face full of holes
    BRepMesh_IncrementalMesh(shape, mesher);

    for (int k = 0; k < 3; ++k) {
        out.min[k] = INFINITY;
        out.max[k] = -INFINITY;
    }

    TopTools_IndexedMapOfShape faces;
    TopExp::MapShapes(shape, TopAbs_FACE, faces);
    out.faceCount = std::uint32_t(faces.Extent());
    for (int f = 1; f <= faces.Extent(); ++f) {
        const TopoDS_Face &face = TopoDS::Face(faces(f));
        TopLoc_Location loc;
        const Handle(Poly_Triangulation) tri = BRep_Tool::Triangulation(face, loc);
        if (tri.IsNull())
            continue;
        if (!tri->HasNormals())
            BRepLib_ToolTriangulatedShape::ComputeNormals(face, tri);
        const gp_Trsf place = loc.Transformation();
        const bool reversed = face.Orientation() == TopAbs_REVERSED;
        const std::uint32_t base = std::uint32_t(out.positions.size() / 3);
        for (int i = 1; i <= tri->NbNodes(); ++i) {
            const gp_Pnt p = tri->Node(i).Transformed(place);
            gp_Dir n = tri->Normal(i).Transformed(place);
            if (reversed)
                n.Reverse();
            const double xyz[3] = {p.X(), p.Y(), p.Z()};
            for (int c = 0; c < 3; ++c) {
                out.positions.push_back(float(xyz[c]));
                out.min[c] = std::min(out.min[c], float(xyz[c]));
                out.max[c] = std::max(out.max[c], float(xyz[c]));
            }
            out.normals.insert(out.normals.end(), {float(n.X()), float(n.Y()), float(n.Z())});
        }
        for (int t = 1; t <= tri->NbTriangles(); ++t) {
            int a, b, c;
            tri->Triangle(t).Get(a, b, c);
            if (reversed)
                std::swap(b, c);
            out.indices.insert(out.indices.end(), {base + a - 1, base + b - 1, base + c - 1});
            out.triangleFace.push_back(std::uint32_t(f - 1));
        }
    }
    if (out.positions.empty())
        for (int k = 0; k < 3; ++k)
            out.min[k] = out.max[k] = 0;

    // Edges: each real edge once, along its points on the first face that holds it. A seam (where
    // a cylinder's surface wraps round onto itself) is no edge to the user: left out, as in Fusion.
    TopTools_IndexedDataMapOfShapeListOfShape edgeFaces;
    TopExp::MapShapesAndAncestors(shape, TopAbs_EDGE, TopAbs_FACE, edgeFaces);
    TopTools_IndexedMapOfShape seams;
    for (int e = 1; e <= edgeFaces.Extent(); ++e) {
        const TopoDS_Edge &edge = TopoDS::Edge(edgeFaces.FindKey(e));
        if (BRep_Tool::Degenerated(edge) || edgeFaces(e).IsEmpty())
            continue;
        const TopoDS_Face &face = TopoDS::Face(edgeFaces(e).First());
        if (BRep_Tool::IsClosed(edge, face)) {
            seams.Add(edge);
            continue;
        }
        TopLoc_Location loc;
        const Handle(Poly_Triangulation) tri = BRep_Tool::Triangulation(face, loc);
        if (tri.IsNull())
            continue;
        const Handle(Poly_PolygonOnTriangulation) poly = BRep_Tool::PolygonOnTriangulation(edge, tri, loc);
        if (poly.IsNull())
            continue;
        const gp_Trsf place = loc.Transformation();
        std::vector<float> line;
        for (int i = 1; i <= poly->NbNodes(); ++i) {
            const gp_Pnt p = tri->Node(poly->Node(i)).Transformed(place);
            line.insert(line.end(), {float(p.X()), float(p.Y()), float(p.Z())});
        }
        for (std::size_t i = 3; i < line.size(); i += 3)
            out.edgeLines.insert(out.edgeLines.end(), line.begin() + long(i) - 3, line.begin() + long(i) + 3);
        out.edges.push_back(std::move(line));
    }

    // Points: the shape's corners, where two or more edges meet (a lone circle's start isn't one).
    TopTools_IndexedDataMapOfShapeListOfShape vertexEdges;
    TopExp::MapShapesAndAncestors(shape, TopAbs_VERTEX, TopAbs_EDGE, vertexEdges);
    for (int v = 1; v <= vertexEdges.Extent(); ++v) {
        TopTools_IndexedMapOfShape distinct;
        for (const TopoDS_Shape &edge : vertexEdges(v))
            if (!BRep_Tool::Degenerated(TopoDS::Edge(edge)) && !seams.Contains(edge))
                distinct.Add(edge);
        if (distinct.Extent() < 2)
            continue;
        const gp_Pnt p = BRep_Tool::Pnt(TopoDS::Vertex(vertexEdges.FindKey(v)));
        out.points.insert(out.points.end(), {float(p.X()), float(p.Y()), float(p.Z())});
    }
    return out;
}

} // namespace

std::unique_ptr<GeometryEngine> makeHeavyEngine() {
    return std::make_unique<HeavyEngine>();
}

} // namespace omamake
