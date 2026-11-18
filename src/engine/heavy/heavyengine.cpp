// Heavy engine: OpenCascade (exact surfaces: planes, cylinders, splines). Slower booleans than
// Lite, but a circle stays a circle; triangles are only made for drawing and picking.

#include "engine/engine.h"
#include "engine/heavy/meshsolid.h"

#include <BinTools.hxx>
#include <BRepAdaptor_Curve.hxx>
#include <BRepAdaptor_Surface.hxx>
#include <BRepExtrema_DistShapeShape.hxx>
#include <IntCurvesFace_ShapeIntersector.hxx>
#include <gp_Lin.hxx>
#include <Precision.hxx>
#include <TopoDS_Vertex.hxx>
#include <BRepGProp.hxx>
#include <BRepTools.hxx>
#include <BRepTopAdaptor_FClass2d.hxx>
#include <GeomAPI_ProjectPointOnSurf.hxx>
#include <array>
#include <BRepTools_WireExplorer.hxx>
#include <GCPnts_TangentialDeflection.hxx>
#include <GProp_GProps.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS_Edge.hxx>
#include <TopoDS_Wire.hxx>
#include <gp_Ax3.hxx>
#include <gp_Pln.hxx>
#include <gp_Circ.hxx>
#include <BRepAlgoAPI_Common.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <BRepBuilderAPI_MakeWire.hxx>
#include <BRepBuilderAPI_MakeEdge.hxx>
#include <GC_MakeArcOfCircle.hxx>
#include <TopTools_ListOfShape.hxx>
#include <BRepAlgoAPI_Cut.hxx>
#include <BRepAlgoAPI_Fuse.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_MakePolygon.hxx>
#include <BRepBuilderAPI_Transform.hxx>
#include <BRepLib_ToolTriangulatedShape.hxx>
#include <BRepBndLib.hxx>
#include <BRepMesh_Context.hxx>
#include <BRepMesh_DelaunayBaseMeshAlgo.hxx>
#include <BRepMesh_FaceDiscret.hxx>
#include <BRepMesh_IncrementalMesh.hxx>
#include <BRepMesh_MeshAlgoFactory.hxx>
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
#include "engine/heavy/refine.h"
#include "engine/heavy/projectexact.h"
#include "engine/heavy/pushface.h"
#include "engine/heavy/chamfer.h"
#include <Standard_Failure.hxx>
#include "engine/heavy/taper.h"
#include <BOPAlgo_CheckerSI.hxx>
#include <TopExp.hxx>
#include <TopTools_IndexedDataMapOfShapeListOfShape.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Shape.hxx>
#include <ShapeAnalysis_Shell.hxx>
#include <BRepClass3d_SolidClassifier.hxx>
#include <BRepClass_FaceClassifier.hxx>
#include <GeomLProp_SLProps.hxx>
#include <TopTools_ListIteratorOfListOfShape.hxx>
#include <ElSLib.hxx>
#include <ElCLib.hxx>
#include <GeomAPI.hxx>
#include <Geom2d_BSplineCurve.hxx>
#include <Geom2d_Circle.hxx>
#include <Geom2d_Ellipse.hxx>
#include <Geom2d_Line.hxx>
#include <Geom2d_TrimmedCurve.hxx>
#include <Geom2dAPI_InterCurveCurve.hxx>
#include <Geom2dInt_GInter.hxx>
#include <IntRes2d_IntersectionPoint.hxx>
#include <Geom2dAPI_ProjectPointOnCurve.hxx>
#include <BRepBuilderAPI_MakeVertex.hxx>
#include <TColgp_Array1OfPnt2d.hxx>
#include <TColStd_Array1OfInteger.hxx>
#include <TColStd_Array1OfReal.hxx>
#include <map>
#include <gp_Trsf.hxx>
#include <BRepBuilderAPI_Copy.hxx>
#include <ShapeFix_Shape.hxx>
#include <ShapeFix_FixSmallFace.hxx>
#include <BRepLib.hxx>
#include <BRep_Builder.hxx>
#include <BRep_TEdge.hxx>
#include <BRep_TVertex.hxx>
#include <GeomAPI_ProjectPointOnCurve.hxx>
#include <Geom_Plane.hxx>
#include <Geom_RectangularTrimmedSurface.hxx>
#include <Geom_SurfaceOfLinearExtrusion.hxx>
#include <ShapeAnalysis_Edge.hxx>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <chrono>
#include <cstdio>
#include <string>
#include <cmath>
#include <sstream>
#include <cstdarg>
#include <mutex>
#include <BRepCheck.hxx>
#include <BRepCheck_Result.hxx>
#include <BRepCheck_ListOfStatus.hxx>
#include <map>
#include <unordered_map>
#include <unordered_set>
#include <memory>
#include <mutex>
#include <cstring>
#include "engine/stop.h"
#include <Message_ProgressIndicator.hxx>
#include <Message_ProgressRange.hxx>

namespace {
// GB-089: OpenCascade's Boolean asks this, through its progress range, whether to stop. The memory guard's stop
// (every job) or the starting thread's StopScope deadline (copied here: the Boolean may ask from its own threads).
class BreakCheck : public Message_ProgressIndicator {
public:
    BreakCheck() : m_deadline(omamake::stopDeadline()) {}
    Standard_Boolean UserBreak() override {
        return omamake::memoryStopRequested() || std::chrono::steady_clock::now() > m_deadline;
    }
    void Show(const Message_ProgressScope &, const Standard_Boolean) override {}
private:
    std::chrono::steady_clock::time_point m_deadline;
};
} // namespace

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
// Closer than this (mm), two faces are the same place in a join, cut or intersect.
constexpr double kTouching = 1e-5;
// Faces of one surface within these (mm, radians) merge into one after a join, cut or intersect. The
// angle holds 0.00001 in over a 20 ft face (0.00006 mm); 0.000001 rad leaned it 0.006 mm there.
constexpr double kMergeLinear = 1e-4;
constexpr double kMergeAngular = 1e-8;
// The model holds 0.00001 in (mm): a join, cut or intersect may leave edges no looser than this, or
// than the shapes that went in.
constexpr double kAccuracy = 0.000254;

// The engine's own log: each line to the window's log (stderr), and kept until takeTrace() collects
// it, so a failed operation's log file holds every step the engine took (owner, 2026-11-06).
static std::mutex g_traceLock;
static std::string g_trace;
static void engineLog(const char *format, ...) {
    va_list args, again;
    va_start(args, format);
    va_copy(again, args);
    std::string line(std::size_t(std::max(0, std::vsnprintf(nullptr, 0, format, args))), '\0');
    va_end(args);
    std::vsnprintf(line.data(), line.size() + 1, format, again);
    va_end(again);
    std::fputs(line.c_str(), stderr);
    const std::lock_guard<std::mutex> hold(g_traceLock);
    g_trace += line;
}

// The edges a person sees, in the order the mesh numbers them: each real edge once, drawn along its
// points on the first face that holds it. A seam (where a cylinder's surface wraps round onto itself)
// is no edge to the user: left out, as in Fusion. Needs the shape meshed (edges without a drawn line
// are left out too). Shared by mesh() and reach(), so a picked edge is always the same edge.
struct ShownEdge { TopoDS_Edge edge; TopoDS_Face face; };
std::vector<ShownEdge> shownEdges(const TopoDS_Shape &shape, TopTools_IndexedMapOfShape &seams) {
    std::vector<ShownEdge> out;
    TopTools_IndexedDataMapOfShapeListOfShape edgeFaces;
    TopExp::MapShapesAndAncestors(shape, TopAbs_EDGE, TopAbs_FACE, edgeFaces);
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
        if (tri.IsNull() || BRep_Tool::PolygonOnTriangulation(edge, tri, loc).IsNull())
            continue;
        out.push_back({edge, face});
    }
    return out;
}

// The shape's corners in the mesh's order: where two or more edges meet (a lone circle's start isn't one).
std::vector<TopoDS_Vertex> shownCorners(const TopoDS_Shape &shape, const TopTools_IndexedMapOfShape &seams) {
    std::vector<TopoDS_Vertex> out;
    TopTools_IndexedDataMapOfShapeListOfShape vertexEdges;
    TopExp::MapShapesAndAncestors(shape, TopAbs_VERTEX, TopAbs_EDGE, vertexEdges);
    for (int v = 1; v <= vertexEdges.Extent(); ++v) {
        TopTools_IndexedMapOfShape distinct;
        bool closes = false;
        for (const TopoDS_Shape &edge : vertexEdges(v)) {
            closes = closes || BRep_Tool::Degenerated(TopoDS::Edge(edge));
            if (!BRep_Tool::Degenerated(TopoDS::Edge(edge)) && !seams.Contains(edge))
                distinct.Add(edge);
        }
        // A cone's tip is a corner too (GB-053: to chamfer or round it), a sphere's pole isn't.
        if (distinct.Extent() >= 2 || (closes && chamfer::isTip(shape, vertexEdges.FindKey(v))))
            out.push_back(TopoDS::Vertex(vertexEdges.FindKey(v)));
    }
    return out;
}

// Every edge has its curve on each face it bounds. OpenCascade's face merge (ShapeUpgrade_UnifySameDomain)
// reads those curves without checking and crashes the whole program on a missing one (chamfer run-through:
// two half-turn sweeps meeting end to end, 2026-11-08); a shape without them is left unmerged.
// With `noPoints` (a chamfer's cut), also none with a point where a surface closes to nothing (a cone's tip): the
// merge crashed on two half cones meeting at their tip, every curve there, 2026-11-08.
static bool mergeable(const TopoDS_Shape &shape, bool noPoints = false) {
    if (noPoints)
        for (TopExp_Explorer e(shape, TopAbs_EDGE); e.More(); e.Next())
            if (BRep_Tool::Degenerated(TopoDS::Edge(e.Current())))
                return false;
    for (TopExp_Explorer f(shape, TopAbs_FACE); f.More(); f.Next()) {
        const TopoDS_Face &face = TopoDS::Face(f.Current());
        for (TopExp_Explorer e(face, TopAbs_EDGE); e.More(); e.Next()) {
            const TopoDS_Edge &edge = TopoDS::Edge(e.Current());
            Standard_Real a, b;
            if (BRep_Tool::CurveOnSurface(edge, face, a, b).IsNull())
                return false;
        }
    }
    return true;
}

// Valid by OpenCascade's own check, and closed: no edge with a face on one side only (edges drawn
// inside a face, where a round wall just touches a flat one, aren't such edges).
// Also every shell closed (each edge shared by two of its faces), and no face marked internal or external
// (one OpenCascade leaves inside the material when it can't tell inside from outside). OpenCascade's own
// check passes both: a cut of a converted model came back "valid" as two solids, the part to be cut away
// kept as a second one, with 4 open shells and 17 internal faces, and nothing removed (owner, 2026-11-06).
static bool soundSolid(const TopoDS_Shape &shape) {
    if (shape.IsNull())
        return false;
    for (TopExp_Explorer f(shape, TopAbs_FACE); f.More(); f.Next())
        if (f.Current().Orientation() == TopAbs_INTERNAL || f.Current().Orientation() == TopAbs_EXTERNAL)
            return false;
    for (TopExp_Explorer s(shape, TopAbs_SHELL); s.More(); s.Next())
        if (!BRep_Tool::IsClosed(s.Current()))
            return false;
    if (!BRepCheck_Analyzer(shape, true, true).IsValid())   // every piece checked on all cores (its standard parallel mode)
        return false;
    ShapeAnalysis_Shell shells;
    shells.LoadShells(shape);
    shells.CheckOrientedShells(shape, true);
    return !shells.HasFreeEdges();
}

// A face of no area: OpenCascade's leftover where two parts of one smooth surface meet along a line
// (a figure eight's crossing), not real geometry. Parts that only touch at an edge or a corner, as the
// person drew them (a bow-tie, a star tip on a wall), leave no such face and are kept, as in Fusion.
static bool hasEmptyFace(const TopoDS_Shape &shape) {
    for (TopExp_Explorer f(shape, TopAbs_FACE); f.More(); f.Next()) {
        GProp_GProps props;
        BRepGProp::SurfaceProperties(f.Current(), props);
        if (std::abs(props.Mass()) < 1e-8)
            return true;
    }
    return false;
}

// GB-094 (owner, 2026-11-17): faces under 0.000001 mm2 a cut left (where a corner of the body lies a hair, under
// 0.001 mm, off the tool's wall, the cut can't help but leave a sliver triangle there, b205d's hole cut from below:
// 6e-9 to 7e-7 mm2) folded into the faces beside them by OpenCascade's Shape Healing (ShapeFix_FixSmallFace, as
// `mended` uses it), with the model's accuracy (0.00001 in) as the most any corner may move and `allowed` as the
// loosest any edge may get. Null if there were none, or if it did more than that: removed more faces than there
// were such slivers, or changed the volume by more than a millionth.
static TopoDS_Shape foldTinyFaces(const TopoDS_Shape &shape, double allowed) {
    constexpr double kTiny = 1e-6;   // mm2
    try {
        int tiny = 0, before = 0;
        for (TopExp_Explorer f(shape, TopAbs_FACE); f.More(); f.Next(), ++before) {
            GProp_GProps props;
            BRepGProp::SurfaceProperties(f.Current(), props);
            tiny += std::abs(props.Mass()) < kTiny;
        }
        if (!tiny)
            return {};
        ShapeFix_FixSmallFace small;
        small.Init(shape);
        small.SetPrecision(kAccuracy);
        small.SetMaxTolerance(allowed);
        small.Perform();
        const TopoDS_Shape out = small.FixShape();
        if (out.IsNull())
            return {};
        int after = 0;
        for (TopExp_Explorer f(out, TopAbs_FACE); f.More(); f.Next())
            ++after;
        GProp_GProps was, now;
        BRepGProp::VolumeProperties(shape, was);
        BRepGProp::VolumeProperties(out, now);
        const bool narrow = before - after <= tiny && std::abs(now.Mass() - was.Mass()) <= 1e-6 * std::max(1.0, std::abs(was.Mass()));
        engineLog("engine: sliver faces: %d under 0.000001 mm2; folded: %d face(s) fewer, volume %+.6f mm3, %s\n", tiny, before - after,
                  now.Mass() - was.Mass(), narrow ? "kept for the checks" : "more than the slivers changed, not used");
        return narrow ? out : TopoDS_Shape();
    } catch (const Standard_Failure &) {
        return {};
    }
}

// The loosest edge or corner (mm): how far apart the pieces it joins may lie.
static double loosest(const TopoDS_Shape &shape) {
    double worst = 0;
    for (TopExp_Explorer e(shape, TopAbs_EDGE); e.More(); e.Next())
        worst = std::max(worst, BRep_Tool::Tolerance(TopoDS::Edge(e.Current())));
    for (TopExp_Explorer v(shape, TopAbs_VERTEX); v.More(); v.Next())
        worst = std::max(worst, BRep_Tool::Tolerance(TopoDS::Vertex(v.Current())));
    return worst;
}

// A body described for someone troubleshooting it, a line each (inspect, and a refused result in the
// log): faces, solids, each shell (faces, closed or not, where), faces marked internal or external (where),
// OpenCascade's check and every finding it has by kind of piece, edges with a face on one side only, the
// loosest edge and corner, volume, box. The finer lists stop at 8 entries. True if it is sound.
static bool describe(const TopoDS_Shape &shape, std::string &report) {
    report.clear();
    if (shape.IsNull()) {
        report = "no shape\n";
        return false;
    }
    char line[512];
    auto add = [&](const char *format, auto... values) {
        std::snprintf(line, sizeof line, format, values...);
        report += line;
        report += '\n';
    };
    auto where = [](const TopoDS_Shape &part) {
        Bnd_Box box;
        BRepBndLib::Add(part, box);
        if (box.IsVoid())
            return std::string("(nowhere)");
        double a[6];
        box.Get(a[0], a[1], a[2], a[3], a[4], a[5]);
        char at[200];
        std::snprintf(at, sizeof at, "box (%.4f %.4f %.4f) to (%.4f %.4f %.4f)", a[0], a[1], a[2], a[3], a[4], a[5]);
        return std::string(at);
    };
    int faces = 0, solids = 0, shells = 0, open = 0, inner = 0;
    for (TopExp_Explorer f(shape, TopAbs_FACE); f.More(); f.Next())
        ++faces;
    for (TopExp_Explorer so(shape, TopAbs_SOLID); so.More(); so.Next())
        ++solids;
    GProp_GProps props;
    BRepGProp::VolumeProperties(shape, props);
    add("faces %d, solids %d, volume %.6f mm3 (OpenCascade's integration), %s", faces, solids, props.Mass(), where(shape).c_str());
    int solid = 0;
    for (TopExp_Explorer so(shape, TopAbs_SOLID); so.More(); so.Next(), ++solid) {
        int shell = 0;
        for (TopExp_Explorer sh(so.Current(), TopAbs_SHELL); sh.More(); sh.Next(), ++shell) {
            ++shells;
            int n = 0;
            for (TopExp_Explorer f(sh.Current(), TopAbs_FACE); f.More(); f.Next())
                ++n;
            const bool closed = BRep_Tool::IsClosed(sh.Current());
            open += !closed;
            if (shells <= 8 || !closed)
                add("  solid %d shell %d: %d faces, %s, %s", solid, shell, n, closed ? "closed" : "OPEN", where(sh.Current()).c_str());
        }
    }
    for (TopExp_Explorer sh(shape, TopAbs_SHELL, TopAbs_SOLID); sh.More(); sh.Next())
        add("  a shell outside any solid: %s", where(sh.Current()).c_str());
    for (TopExp_Explorer f(shape, TopAbs_FACE); f.More(); f.Next())
        if (f.Current().Orientation() == TopAbs_INTERNAL || f.Current().Orientation() == TopAbs_EXTERNAL) {
            if (++inner <= 8) {
                GProp_GProps area;
                BRepGProp::SurfaceProperties(f.Current(), area);
                add("  face marked %s: area %.6f mm2, %s", f.Current().Orientation() == TopAbs_INTERNAL ? "internal" : "external", area.Mass(), where(f.Current()).c_str());
            }
        }
    add("shells %d, open shells %d, faces marked internal or external %d", shells, open, inner);
    BRepCheck_Analyzer check(shape, true, true);
    add("OpenCascade's check: %s", check.IsValid() ? "valid" : "INVALID");
    if (!check.IsValid()) {
        std::map<std::string, int> tally;
        int shown = 0;
        static const char *const kinds[] = {"compound", "compsolid", "solid", "shell", "face", "wire", "edge", "vertex", "shape"};
        for (TopAbs_ShapeEnum kind : {TopAbs_SOLID, TopAbs_SHELL, TopAbs_FACE, TopAbs_WIRE, TopAbs_EDGE, TopAbs_VERTEX}) {
            TopTools_IndexedMapOfShape parts;
            TopExp::MapShapes(shape, kind, parts);
            for (int i = 1; i <= parts.Extent(); ++i) {
                const Handle(BRepCheck_Result) &r = check.Result(parts(i));
                if (r.IsNull())
                    continue;
                for (BRepCheck_ListIteratorOfListOfStatus it(r->Status()); it.More(); it.Next()) {
                    if (it.Value() == BRepCheck_NoError)
                        continue;
                    std::ostringstream name;
                    BRepCheck::Print(it.Value(), name);
                    std::string said = name.str();
                    while (!said.empty() && (said.back() == '\n' || said.back() == ' '))
                        said.pop_back();
                    ++tally[std::string(kinds[kind]) + ": " + said];
                    if (++shown <= 8)
                        add("  %s %d: %s, %s", kinds[kind], i, said.c_str(), where(parts(i)).c_str());
                }
            }
        }
        for (const auto &[what, count] : tally)
            add("  %d x %s", count, what.c_str());
    }
    ShapeAnalysis_Shell shellCheck;
    shellCheck.LoadShells(shape);
    shellCheck.CheckOrientedShells(shape, true);
    int freeEdges = 0;
    if (shellCheck.HasFreeEdges())
        for (TopExp_Explorer e(shellCheck.FreeEdges(), TopAbs_EDGE); e.More(); e.Next())
            if (++freeEdges <= 8)
                add("  edge with a face on one side only: %s", where(e.Current()).c_str());
    add("edges with a face on one side only: %d", freeEdges);
    double edgeWorst = 0, cornerWorst = 0;
    for (TopExp_Explorer e(shape, TopAbs_EDGE); e.More(); e.Next())
        edgeWorst = std::max(edgeWorst, BRep_Tool::Tolerance(TopoDS::Edge(e.Current())));
    for (TopExp_Explorer v(shape, TopAbs_VERTEX); v.More(); v.Next())
        cornerWorst = std::max(cornerWorst, BRep_Tool::Tolerance(TopoDS::Vertex(v.Current())));
    add("loosest edge %.6g mm, loosest corner %.6g mm", edgeWorst, cornerWorst);
    const bool empty = hasEmptyFace(shape);
    if (empty)
        add("a face of no area");
    const bool sound = soundSolid(shape) && !empty && solids > 0;
    add("sound: %s", sound ? "yes" : "NO");
    return sound;
}

// A body OpenCascade's own check finds fault with, or with a crack (an edge with a face on one side only),
// put right before a join, cut or intersect uses it, the standard way (OpenCascade's Shape Healing, what
// FreeCAD's and Open CASCADE's own "fix shape" run): ShapeFix_Shape first (wires, their order and way
// round, edges and their curves on the faces), then ShapeFix_FixSmallFace for faces narrower than the
// body's own tolerance, which it folds into the faces beside them. An older mesh-to-solid conversion left
// such slivers: a speaker mount came in with 47 flat strips 0.01 mm wide (its edges are 0.05 mm loose),
// one of whose three edges don't meet on the face, so OpenCascade calls the whole body invalid and every
// cut of it was refused as unsound (owner, 2026-11-06). Never past what was there: the body's own loosest
// edge or corner is the most either may move or widen anything, and the fix is kept only if the body comes
// out sound, no looser, and with every corner where one was (within that). On a copy: the body stays as it was, for
// undo. Null if it was sound already, or couldn't be put right this way.
static TopoDS_Shape mended(const TopoDS_Shape &shape, std::string &how) {
    if (shape.IsNull() || soundSolid(shape))
        return {};
    const double own = loosest(shape);
    std::vector<gp_Pnt> corners;
    for (TopExp_Explorer v(shape, TopAbs_VERTEX); v.More(); v.Next())
        corners.push_back(BRep_Tool::Pnt(TopoDS::Vertex(v.Current())));
    // Sound, no looser, and every corner where one was, within the body's own tolerance (the faces keep
    // their own surfaces, so that is the shape kept). Not by the exact box: 4 s on that speaker mount.
    auto same = [&](const TopoDS_Shape &out) {
        if (out.IsNull() || loosest(out) > own || !soundSolid(out))
            return false;
        for (TopExp_Explorer v(out, TopAbs_VERTEX); v.More(); v.Next()) {
            const gp_Pnt at = BRep_Tool::Pnt(TopoDS::Vertex(v.Current()));
            if (std::none_of(corners.begin(), corners.end(), [&](const gp_Pnt &was) { return was.Distance(at) <= own; }))
                return false;
        }
        return true;
    };
    try {
        ShapeFix_Shape fix(BRepBuilderAPI_Copy(shape, true, false).Shape());
        fix.SetPrecision(own);
        fix.SetMaxTolerance(own);
        fix.Perform();
        TopoDS_Shape out = fix.Shape();
        if (same(out)) {
            how = "its faces' outlines put right";
            return out;
        }
        ShapeFix_FixSmallFace small;
        small.Init(out);
        small.SetPrecision(own);
        small.SetMaxTolerance(own);
        small.Perform();
        out = small.FixShape();
        if (same(out)) {
            int before = 0, after = 0;
            for (TopExp_Explorer f(shape, TopAbs_FACE); f.More(); f.Next())
                ++before;
            for (TopExp_Explorer f(out, TopAbs_FACE); f.More(); f.Next())
                ++after;
            char said[160];
            std::snprintf(said, sizeof said, "%d sliver face(s) narrower than its own %.3g mm folded into the faces beside them", before - after, own);
            how = said;
            return out;
        }
    } catch (const Standard_Failure &) {
    }
    return {};
}

// Where a flat face lying square to an extruded wall's direction crosses that wall, the edge is the wall's
// own curve moved onto the face, exactly. OpenCascade works it out like any free-form crossing, by walking
// along it and fitting a curve (IntPatch_Intersection::GeomParamPerfom, IntPatch_ImpPrmIntersection), so a
// cut ending inside a body, or going through one, left a spline wall's edge 0.0013 mm off on a 100 mm gear
// and 0.036 mm off at 10 ft. Each such new edge is given the exact curve, the exact line it is on the wall
// (the wall's own curve at that height) and its exact copy on the face, and only the tolerance it really
// needs; its new corners are put on it. On a copy: the boolean's result can share pieces with the bodies
// that went in, which stay as they were. Null if there was nothing to put right.
static TopoDS_Shape exactSections(const TopoDS_Shape &result, const TopoDS_Shape &first, const TopTools_ListOfShape &tools) {
    TopTools_IndexedMapOfShape old;
    TopExp::MapShapes(first, old);
    for (TopTools_ListIteratorOfListOfShape it(tools); it.More(); it.Next())
        TopExp::MapShapes(it.Value(), old);
    auto unwrap = [](const TopoDS_Face &face) {
        Handle(Geom_Surface) s = BRep_Tool::Surface(face);   // where the face really is
        while (auto t = Handle(Geom_RectangularTrimmedSurface)::DownCast(s))
            s = t->BasisSurface();
        return s;
    };
    // The new edges between such a wall and such a face.
    struct Section { TopoDS_Edge edge; TopoDS_Face wall, flat; };
    std::vector<Section> found;
    TopTools_IndexedDataMapOfShapeListOfShape edgeFaces;
    TopExp::MapShapesAndAncestors(result, TopAbs_EDGE, TopAbs_FACE, edgeFaces);
    for (int i = 1; i <= edgeFaces.Extent(); ++i) {
        const TopoDS_Edge edge = TopoDS::Edge(edgeFaces.FindKey(i));
        if (old.Contains(edge) || BRep_Tool::Degenerated(edge) || edgeFaces(i).Extent() != 2)
            continue;
        TopoDS_Face wall, flat;
        for (TopTools_ListIteratorOfListOfShape f(edgeFaces(i)); f.More(); f.Next()) {
            const TopoDS_Face face = TopoDS::Face(f.Value());
            const Handle(Geom_Surface) s = unwrap(face);
            if (!Handle(Geom_SurfaceOfLinearExtrusion)::DownCast(s).IsNull())
                wall = face;
            else if (!Handle(Geom_Plane)::DownCast(s).IsNull())
                flat = face;
        }
        if (wall.IsNull() || flat.IsNull())
            continue;
        const auto ext = Handle(Geom_SurfaceOfLinearExtrusion)::DownCast(unwrap(wall));
        const gp_Pln pln = Handle(Geom_Plane)::DownCast(unwrap(flat))->Pln();
        if (!pln.Axis().Direction().IsParallel(ext->Direction(), 1e-12))
            continue;
        found.push_back({edge, wall, flat});
    }
    if (found.empty())
        return {};
    BRepBuilderAPI_Copy copy(result, false);   // the same curves and surfaces, new edges and corners to change
    const TopoDS_Shape out = copy.Shape();
    TopTools_IndexedMapOfShape oldInOut;        // what the copy holds of the bodies that went in: corners not moved
    {
        TopTools_IndexedMapOfShape inResult;
        TopExp::MapShapes(result, inResult);
        for (int i = 1; i <= inResult.Extent(); ++i)
            if (old.Contains(inResult(i)))
                oldInOut.Add(copy.ModifiedShape(inResult(i)));
    }
    TopTools_IndexedDataMapOfShapeListOfShape cornerEdges;
    TopExp::MapShapesAndUniqueAncestors(out, TopAbs_VERTEX, TopAbs_EDGE, cornerEdges);
    BRep_Builder builder;
    int fixed = 0;
    std::vector<TopoDS_Edge> done;
    for (const Section &found1 : found) {
        const TopoDS_Edge edge = TopoDS::Edge(copy.ModifiedShape(found1.edge));
        const TopoDS_Face wall = TopoDS::Face(copy.ModifiedShape(found1.wall)), flat = TopoDS::Face(copy.ModifiedShape(found1.flat));
        const auto ext = Handle(Geom_SurfaceOfLinearExtrusion)::DownCast(unwrap(wall));
        const gp_Pln pln = Handle(Geom_Plane)::DownCast(unwrap(flat))->Pln();
        const gp_Dir d = ext->Direction();
        const Handle(Geom_Curve) basis = ext->BasisCurve();
        // The wall's curve must lie square to its direction (a sketch's, pushed straight): then at the
        // face's height it is the curve moved there.
        const double f0 = basis->FirstParameter(), f1 = basis->LastParameter();
        auto height = [&](double u) { return basis->Value(u).XYZ().Dot(d.XYZ()); };
        if (Precision::IsInfinite(f0) || Precision::IsInfinite(f1) || std::abs(height(f0) - height(f1)) > 1e-9 || std::abs(height(f0) - height((f0 + f1) / 2)) > 1e-9)
            continue;
        const double t = (pln.Location().XYZ() - basis->Value(f0).XYZ()).Dot(d.XYZ());
        Handle(Geom_Curve) exact = Handle(Geom_Curve)::DownCast(basis->Translated(gp_Vec(d) * t));
        // Its ends: where the edge already is along the wall's curve, read off its place on the wall (the
        // wall's u is the curve's own parameter), not found by projecting its corners onto the curve: on a
        // curve that comes near itself (a figure eight's crossing) a projection lands on the other branch.
        TopoDS_Vertex v0, v1;
        TopExp::Vertices(edge, v0, v1);
        if (v0.IsNull() || v1.IsNull() || v0.IsSame(v1))
            continue;
        double e0, e1;
        const Handle(Geom2d_Curve) wasOnWall = BRep_Tool::CurveOnSurface(edge, wall, e0, e1);
        const Handle(Geom_Curve) was = BRep_Tool::Curve(edge, e0, e1);
        if (wasOnWall.IsNull() || was.IsNull())
            continue;
        double u0 = wasOnWall->Value(e0).X(), u1 = wasOnWall->Value(e1).X();
        // A corner on an upright edge (a straight edge along the wall's sweep, from the tool or the body as
        // drawn): the end is exactly where that edge stands, found from the fitted end by Newton along the
        // curve (a local step: it can't jump to another branch). Four fitted ends meeting at a figure
        // eight's crossing 100 ft out were 0.0024 mm apart; its upright edges were exact.
        auto standing = [&](const TopoDS_Vertex &v, double &u) {
            const int at = cornerEdges.FindIndex(v);
            if (!at)
                return;
            for (TopTools_ListIteratorOfListOfShape e(cornerEdges(at)); e.More(); e.Next()) {
                BRepAdaptor_Curve up(TopoDS::Edge(e.Value()));
                if (up.GetType() != GeomAbs_Line || !up.Line().Direction().IsParallel(d, 1e-12))
                    continue;
                const gp_Lin line = up.Line();
                // where it stands on the face's plane
                const double s = (pln.Location().XYZ() - line.Location().XYZ()).Dot(pln.Axis().Direction().XYZ())
                                 / line.Direction().Dot(pln.Axis().Direction());
                const gp_Pnt target = ElCLib::Value(s, line);
                double w = u;
                for (int k = 0; k < 30; ++k) {
                    gp_Pnt c;
                    gp_Vec d1, d2;
                    exact->D2(w, c, d1, d2);
                    const gp_Vec off(target, c);
                    const double f = off.Dot(d1), fp = d1.SquareMagnitude() + off.Dot(d2);
                    if (fp <= 0)
                        break;
                    const double step = f / fp;
                    w -= step;
                    if (std::abs(step) < 1e-15 * (1 + std::abs(w)))
                        break;
                }
                if (exact->Value(w).Distance(target) < exact->Value(u).Distance(target) + 1e-12)
                    u = w;
                return;
            }
        };
        standing(v0, u0);
        standing(v1, u1);
        const double um = wasOnWall->Value((e0 + e1) / 2).X();
        if (!((u0 < um && um < u1) || (u1 < um && um < u0)))
            continue;
        // And it must really lie along it: its ends and middle near the curve at those parameters, within
        // what the boolean itself says it may be off (its tolerance: 3.3 mm on a 300-point spline at 100 ft).
        const double near = 2 * std::max({BRep_Tool::Tolerance(edge), BRep_Tool::Tolerance(v0), BRep_Tool::Tolerance(v1)}) + 1e-6;
        if (exact->Value(u0).Distance(BRep_Tool::Pnt(v0)) > near || exact->Value(u1).Distance(BRep_Tool::Pnt(v1)) > near
            || exact->Value(um).Distance(was->Value((e0 + e1) / 2)) > near)
            continue;
        // The wall's line for it: (u, t) along the wall's curve, or the other way round if the edge runs back.
        Handle(Geom2d_Line) onWall;
        if (u1 < u0) {
            exact = exact->Reversed();
            u0 = exact->ReversedParameter(u0);
            u1 = exact->ReversedParameter(u1);
            const double a = exact->ReversedParameter(0.0), b = exact->ReversedParameter(1.0);
            onWall = new Geom2d_Line(gp_Pnt2d(a, t), gp_Dir2d(b - a, 0));
        } else {
            onWall = new Geom2d_Line(gp_Pnt2d(0, t), gp_Dir2d(1, 0));
        }
        const Handle(Geom2d_Curve) onFlat = GeomAPI::To2d(exact, pln);
        Handle(Geom_Curve) stored = exact;
        if (!edge.Location().IsIdentity())
            stored = Handle(Geom_Curve)::DownCast(exact->Transformed(edge.Location().Transformation().Inverted()));
        const double tiny = Precision::Confusion();
        Handle(BRep_TEdge)::DownCast(edge.TShape())->Tolerance(tiny);
        builder.UpdateEdge(edge, stored, tiny);
        builder.UpdateEdge(edge, onWall, wall, tiny);
        builder.UpdateEdge(edge, onFlat, flat, tiny);
        builder.Range(edge, u0, u1);
        builder.SameRange(edge, true);
        builder.SameParameter(edge, true);
        for (const auto &[v, u] : {std::pair{v0, u0}, std::pair{v1, u1}})
            if (!oldInOut.Contains(v)) {
                Handle(BRep_TVertex)::DownCast(v.TShape())->Tolerance(tiny);
                builder.UpdateVertex(v, exact->Value(u).Transformed(v.Location().Transformation().Inverted()), tiny);
            }
        done.push_back(edge);
        ++fixed;
    }
    if (!fixed)
        return {};
    // What each really needs now: the edge its pieces' spread, its corners the ends of everything at them.
    for (const TopoDS_Edge &edge : done) {
        double spread = 0;
        ShapeAnalysis_Edge().CheckSameParameter(edge, spread);
        Handle(BRep_TEdge)::DownCast(edge.TShape())->Tolerance(std::max(Precision::Confusion(), spread));
    }
    BRepLib::UpdateTolerances(out, false);
    // Only ever tighter than the boolean's own: anything else means one wasn't what it seemed.
    if (loosest(out) > loosest(result) || !BRepCheck_Analyzer(out).IsValid()) {
        engineLog("engine: %d crossing edge(s) made exact came out no tighter; the boolean's own kept\n", fixed);
        return {};
    }
    engineLog("engine: %d crossing edge(s) of flat faces through extruded walls made exact\n", fixed);
    return out;
}

// For the window's log when a result is refused as loose: its loosest edges (up to five), what kind of
// curve each is, the two kinds of face it lies between, and where it runs. So a refusal can be traced.
static void logLoose(const char *which, const TopoDS_Shape &shape, double allowed) {
    TopTools_IndexedDataMapOfShapeListOfShape edgeFaces;
    TopExp::MapShapesAndAncestors(shape, TopAbs_EDGE, TopAbs_FACE, edgeFaces);
    std::vector<std::pair<double, int>> loose;
    for (int i = 1; i <= edgeFaces.Extent(); ++i)
        if (const double t = BRep_Tool::Tolerance(TopoDS::Edge(edgeFaces.FindKey(i))); t > allowed)
            loose.push_back({t, i});
    std::sort(loose.rbegin(), loose.rend());
    static const char *const kinds[] = {"plane", "cylinder", "cone", "sphere", "torus", "bezier", "bspline", "revolution", "extrusion", "offset", "other"};
    for (std::size_t k = 0; k < loose.size() && k < 5; ++k) {
        const TopoDS_Edge edge = TopoDS::Edge(edgeFaces.FindKey(loose[k].second));
        std::string between;
        for (TopTools_ListIteratorOfListOfShape f(edgeFaces(loose[k].second)); f.More(); f.Next())
            between += std::string(between.empty() ? "" : "/") + kinds[std::min(10, int(BRepAdaptor_Surface(TopoDS::Face(f.Value()), false).GetType()))];
        TopoDS_Vertex a, b;
        TopExp::Vertices(edge, a, b);
        const gp_Pnt p = a.IsNull() ? gp_Pnt() : BRep_Tool::Pnt(a), q = b.IsNull() ? gp_Pnt() : BRep_Tool::Pnt(b);
        engineLog("engine:   %s: loose edge %.4g mm, %s, between %s, (%.3f %.3f %.3f) to (%.3f %.3f %.3f)\n", which, loose[k].first,
                     BRepAdaptor_Curve(edge).GetType() == GeomAbs_Line ? "line" : BRepAdaptor_Curve(edge).GetType() == GeomAbs_Circle ? "circle" : "free-form",
                     between.c_str(), p.X(), p.Y(), p.Z(), q.X(), q.Y(), q.Z());
    }
    if (loose.size() > 5)
        engineLog("engine:   and %zu more\n", loose.size() - 5);
    TopTools_IndexedDataMapOfShapeListOfShape cornerEdges;
    TopExp::MapShapesAndAncestors(shape, TopAbs_VERTEX, TopAbs_EDGE, cornerEdges);
    int shown = 0;
    for (int i = 1; i <= cornerEdges.Extent() && shown < 5; ++i) {
        const TopoDS_Vertex v = TopoDS::Vertex(cornerEdges.FindKey(i));
        if (BRep_Tool::Tolerance(v) <= allowed)
            continue;
        std::string kinds2;
        for (TopTools_ListIteratorOfListOfShape e(cornerEdges(i)); e.More(); e.Next()) {
            const GeomAbs_CurveType t = BRepAdaptor_Curve(TopoDS::Edge(e.Value())).GetType();
            kinds2 += std::string(kinds2.empty() ? "" : "/") + (t == GeomAbs_Line ? "line" : t == GeomAbs_Circle ? "circle" : "free-form");
        }
        const gp_Pnt p = BRep_Tool::Pnt(v);
        engineLog("engine:   %s: loose corner %.4g mm, where %s meet, at (%.3f %.3f %.3f)\n", which, BRep_Tool::Tolerance(v), kinds2.c_str(), p.X(), p.Y(), p.Z());
        ++shown;
    }
}

// A point inside a solid, a hair in from one of its faces (where the face really is, not in a hole
// of it), checked against the solid itself. False if none was found.
static bool pointInside(const TopoDS_Shape &solid, gp_Pnt &out) {
    Bnd_Box box;
    BRepBndLib::Add(solid, box);
    if (box.IsVoid())
        return false;
    const double in = std::max(1e-4, 1e-6 * std::sqrt(box.SquareExtent()));
    for (TopExp_Explorer f(solid, TopAbs_FACE); f.More(); f.Next()) {
        const TopoDS_Face face = TopoDS::Face(f.Current());
        double u0, u1, v0, v1;
        BRepTools::UVBounds(face, u0, u1, v0, v1);
        const Handle(Geom_Surface) surface = BRep_Tool::Surface(face);
        for (const double fu : {0.5, 0.25, 0.75})
            for (const double fv : {0.5, 0.25, 0.75}) {
                const double u = u0 + (u1 - u0) * fu, v = v0 + (v1 - v0) * fv;
                if (BRepClass_FaceClassifier(face, gp_Pnt2d(u, v), Precision::Confusion()).State() != TopAbs_IN)
                    continue;
                GeomLProp_SLProps props(surface, u, v, 1, Precision::Confusion());
                if (!props.IsNormalDefined())
                    continue;
                gp_Dir n = props.Normal();
                if (face.Orientation() == TopAbs_REVERSED)
                    n.Reverse();
                const gp_Pnt p = props.Value().Translated(-gp_Vec(n) * in);   // outward normal: step back in
                if (BRepClass3d_SolidClassifier(solid, p, Precision::Confusion()).State() == TopAbs_IN) {
                    out = p;
                    return true;
                }
            }
    }
    return false;
}

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

    BodyId extrudeOn(const Profile &profile, const Placement &place, double from, double to, double taper) override {
        m_whyNot.clear();
        if (std::abs(to - from) < 1e-9)
            return 0;
        std::string where;                              // the piece being made, for the failure log
        try {
            const gp_Pnt o(place.origin[0], place.origin[1], place.origin[2]);
            const gp_Vec x(place.x[0], place.x[1], place.x[2]), y(place.y[0], place.y[1], place.y[2]);
            const gp_Vec n = x.Crossed(y).Normalized();
            auto area = [](const std::vector<Point2> &loop) {
                double a = 0;
                for (std::size_t i = 0; i < loop.size(); ++i) {
                    const Point2 &p = loop[i], &q = loop[(i + 1) % loop.size()];
                    a += p.x * q.y - q.x * p.y;
                }
                return a / 2;
            };
            TopoDS_Face face;
            for (std::size_t i = 0; i < profile.loops.size(); ++i) {
                const std::vector<Point2> &loop = profile.loops[i];
                if (loop.size() < 3)
                    continue;
                const std::size_t count = loop.size();
                // What step k lies on: a circle (one true arc for a run of steps: a circle's quarter as a few
                // hundred flats made every cut through it slow), one of the sketch's other curves (one
                // smooth edge for a run), or nothing (a straight line).
                const bool curved = i < profile.centres.size() && profile.centres[i].size() == count;
                auto centreOf = [&](std::size_t k) -> const Point2 * {
                    if (!curved)
                        return nullptr;
                    const Point2 &c = profile.centres[i][k % count];
                    return std::isnan(c.x) ? nullptr : &c;
                };
                const bool tagged = i < profile.onCurve.size() && profile.onCurve[i].size() == count && profile.along[i].size() == count;
                auto curveOf = [&](std::size_t k) { return tagged ? profile.onCurve[i][k % count] : -1; };
                auto alongOf = [&](std::size_t k) { return profile.along[i][k % count]; };
                auto same = [](const Point2 *a, const Point2 *b) {
                    return a && b && std::abs(a->x - b->x) < 1e-9 && std::abs(a->y - b->y) < 1e-9;
                };
                // Step k carries on what step k - 1 was on.
                auto continues = [&](std::size_t k) {
                    const std::size_t prev = k + count - 1;
                    if (curveOf(k) >= 0)
                        return curveOf(k) == curveOf(prev) && std::abs(alongOf(prev).second - alongOf(k).first) < 1e-9;
                    return curveOf(prev) < 0 && same(centreOf(k), centreOf(prev));
                };
                // Start where one run ends, so no arc or curve is cut in two at the loop's seam.
                std::size_t start = 0;
                for (std::size_t k = 0; k < count; ++k)
                    if (!continues(k)) {
                        start = k;
                        break;
                    }
                struct Run { std::size_t first, steps; int curve; const Point2 *centre; double a, b; };
                std::vector<Run> runs;
                for (std::size_t k = 0; k < count;) {
                    Run r{start + k, 1, curveOf(start + k), centreOf(start + k), 0, 0};
                    while ((r.curve >= 0 || r.centre) && k + r.steps < count && continues(start + k + r.steps))
                        ++r.steps;
                    if (r.curve >= 0) {
                        const Profile::Curve &c = profile.curves[std::size_t(r.curve)];
                        r.a = c.shift + c.scale * alongOf(r.first).first;
                        r.b = c.shift + c.scale * alongOf(r.first + r.steps - 1).second;
                    }
                    runs.push_back(r);
                    k += r.steps;
                }
                // The sketch's curves, flat (built once per loop as they come up).
                std::map<int, Handle(Geom2d_Curve)> flatCurves;
                auto flatCurve = [&](int index) -> Handle(Geom2d_Curve) {
                    auto &slot = flatCurves[index];
                    if (!slot.IsNull())
                        return slot;
                    const Profile::Curve &c = profile.curves[std::size_t(index)];
                    if (c.ellipse) {
                        const gp_Dir2d along(c.major.x - c.centre.x, c.major.y - c.centre.y);
                        const gp_Ax22d axes(gp_Pnt2d(c.centre.x, c.centre.y), along, gp_Dir2d(-along.Y(), along.X()));
                        slot = new Geom2d_Ellipse(axes, std::hypot(c.major.x - c.centre.x, c.major.y - c.centre.y), c.minor);
                        return slot;
                    }
                    TColgp_Array1OfPnt2d poles(1, int(c.poles.size()));
                    for (std::size_t p = 0; p < c.poles.size(); ++p)
                        poles(int(p) + 1) = gp_Pnt2d(c.poles[p].x, c.poles[p].y);
                    TColStd_Array1OfReal knots(1, int(c.knots.size()));
                    TColStd_Array1OfInteger mults(1, int(c.mults.size()));
                    for (std::size_t q = 0; q < c.knots.size(); ++q) {
                        knots(int(q) + 1) = c.knots[q];
                        mults(int(q) + 1) = c.mults[q];
                    }
                    if (c.weights.empty()) {
                        slot = new Geom2d_BSplineCurve(poles, knots, mults, c.degree);
                    } else {
                        TColStd_Array1OfReal weights(1, int(c.weights.size()));
                        for (std::size_t w = 0; w < c.weights.size(); ++w)
                            weights(int(w) + 1) = c.weights[w];
                        slot = new Geom2d_BSplineCurve(poles, weights, knots, mults, c.degree);
                    }
                    return slot;
                };
                // Where each run starts, flat: the drawn point, or for a curve its own point. Two pieces
                // meeting where the outline crossed itself meet at their true crossing, not the drawn one.
                const std::size_t runCount = runs.size();
                std::vector<gp_Pnt2d> corner(runCount);
                for (std::size_t r = 0; r < runCount; ++r) {
                    const Point2 &p = loop[runs[r].first % count];
                    corner[r] = gp_Pnt2d(p.x, p.y);
                }
                // An arc's radius, from its own samples: its ends can be where it crossed a line, which
                // lies on a sample step (a chord, inside the circle), not on the circle.
                auto radiusOf = [&](const Run &r) {
                    double rad = 0;
                    for (std::size_t k = 0; k <= r.steps; ++k) {
                        const Point2 &p = loop[(r.first + k) % count];
                        rad = std::max(rad, std::hypot(p.x - r.centre->x, p.y - r.centre->y));
                    }
                    return rad;
                };
                // How far a point is off a run that is an arc or a line (a curve's runs: -1, unknown).
                auto offRun = [&](const Run &r, const gp_Pnt2d &p) {
                    if (r.curve >= 0)
                        return -1.0;
                    if (r.centre)
                        return std::abs(std::hypot(p.X() - r.centre->x, p.Y() - r.centre->y) - radiusOf(r));
                    const Point2 &a = loop[r.first % count], &b = loop[(r.first + 1) % count];
                    const double l = std::hypot(b.x - a.x, b.y - a.y);
                    return l < 1e-12 ? 0.0 : std::abs((b.x - a.x) * (p.Y() - a.y) - (b.y - a.y) * (p.X() - a.x)) / l;
                };
                auto pieceNear = [&](const Run &r, bool atEnd) -> Handle(Geom2d_Curve) {
                    if (r.curve >= 0) {
                        // the curve near that end: a sample either side, so a crossing of itself is one point
                        const Profile::Curve &c = profile.curves[std::size_t(r.curve)];
                        const double u = atEnd ? r.b : r.a, reach = 2 * std::abs(c.scale);
                        const Handle(Geom2d_Curve) whole = flatCurve(r.curve);
                        const double lo = std::max(whole->FirstParameter(), u - reach), hi = std::min(whole->LastParameter(), u + reach);
                        return hi > lo ? Handle(Geom2d_Curve)(new Geom2d_TrimmedCurve(whole, lo, hi)) : Handle(Geom2d_Curve)();
                    }
                    if (r.centre)
                        return new Geom2d_Circle(gp_Ax2d(gp_Pnt2d(r.centre->x, r.centre->y), gp_Dir2d(1, 0)), radiusOf(r));
                    const Point2 &p = loop[r.first % count], &q = loop[(r.first + 1) % count];
                    if (std::hypot(q.x - p.x, q.y - p.y) < 1e-12)
                        return {};
                    return new Geom2d_Line(gp_Pnt2d(p.x, p.y), gp_Dir2d(q.x - p.x, q.y - p.y));
                };
                for (std::size_t r = 0; r < runCount; ++r) {
                    const Run &before = runs[(r + runCount - 1) % runCount], &after = runs[r];
                    // Two lines meet where they were drawn; an arc meeting anything meets it on its circle
                    // (its end can be a crossing found on a sample step, off the circle by that step's bow).
                    if (before.curve < 0 && !before.centre && after.curve < 0 && !after.centre)
                        continue;
                    if (before.curve < 0 && after.curve < 0 && offRun(before, corner[r]) < 1e-10 && offRun(after, corner[r]) < 1e-10)
                        continue;
                    gp_Pnt2d mine = corner[r];
                    if (after.curve >= 0)
                        mine = flatCurve(after.curve)->Value(after.a);
                    else if (before.curve >= 0)
                        mine = flatCurve(before.curve)->Value(before.b);
                    const Handle(Geom2d_Curve) g1 = pieceNear(before, true), g2 = pieceNear(after, false);
                    if (!g1.IsNull() && !g2.IsNull() && (mine.Distance(corner[r]) > 1e-9 || (before.curve < 0 && after.curve < 0))) {
                        Geom2dAPI_InterCurveCurve meet(g1, g2, 1e-10);
                        // The drawn corner lies on a curve's straight sample steps, so it can be as far from
                        // the true crossing as a step bows off the curve: taken up to a step's length either
                        // side. A fixed 0.05 mm missed it on a 10 ft spline (steps 40 mm long there), and the
                        // corner stayed on the curve off the line, turning the line's face 0.0002 rad.
                        double closest = 0.05;
                        for (const Run *run : {&before, &after})
                            if (run->curve >= 0) {
                                const Profile::Curve &c = profile.curves[std::size_t(run->curve)];
                                const Handle(Geom2d_Curve) whole = flatCurve(run->curve);
                                const double u = run == &before ? run->b : run->a;
                                const double lo = std::max(whole->FirstParameter(), u - std::abs(c.scale));
                                const double hi = std::min(whole->LastParameter(), u + std::abs(c.scale));
                                closest = std::max(closest, whole->Value(lo).Distance(whole->Value(hi)));
                            } else if (run->centre) {   // an arc's step next to the corner
                                const std::size_t k = run == &before ? run->first + run->steps - 1 : run->first;
                                const Point2 &p = loop[k % count], &q = loop[(k + 1) % count];
                                closest = std::max(closest, std::hypot(q.x - p.x, q.y - p.y));
                            }
                        int chosen = 0;
                        for (int m = 1; m <= meet.NbPoints(); ++m)
                            if (meet.Point(m).Distance(corner[r]) < closest) {
                                closest = meet.Point(m).Distance(corner[r]);
                                mine = meet.Point(m);
                                chosen = m;
                            }
                        // Polished to the crossing itself (Newton on the two curves): the intersector's answer
                        // depends on the pieces it was handed, so two areas meeting at one crossing (a figure
                        // eight's lobes) put their corners 0.0012 mm apart 2.5 km out. Newton from either
                        // side lands on the same point, to the last digit.
                        if (chosen) {
                            const IntRes2d_IntersectionPoint &at = meet.Intersector().Point(chosen);
                            double u = at.ParamOnFirst(), v = at.ParamOnSecond();
                            for (int k = 0; k < 30; ++k) {
                                gp_Pnt2d p1, p2;
                                gp_Vec2d d1, d2;
                                g1->D1(u, p1, d1);
                                g2->D1(v, p2, d2);
                                const gp_Vec2d gap(p2, p1);   // p1 - p2 = 0 wanted: d1 du - d2 dv = -gap
                                const double det = -d1.X() * d2.Y() + d2.X() * d1.Y();
                                if (std::abs(det) < 1e-14 * (d1.SquareMagnitude() + d2.SquareMagnitude()))
                                    break;                 // touching, not crossing: as found
                                const double du = (-gap.X() * -d2.Y() - -d2.X() * -gap.Y()) / det;
                                const double dv = (d1.X() * -gap.Y() - -gap.X() * d1.Y()) / det;
                                u += du;
                                v += dv;
                                if (std::abs(du) + std::abs(dv) < 1e-15 * (1 + std::abs(u) + std::abs(v)))
                                    break;
                            }
                            const gp_Pnt2d q1 = g1->Value(u), q2 = g2->Value(v);
                            if (q1.Distance(q2) < mine.Distance(corner[r]) + 1e-9 && q1.Distance(mine) < 1e-3 * (1 + closest))
                                mine = gp_Pnt2d((q1.X() + q2.X()) / 2, (q1.Y() + q2.Y()) / 2);
                        }
                    }
                    corner[r] = mine;
                }
                // Each curve's ends where its corners are now.
                for (std::size_t r = 0; r < runCount; ++r) {
                    Run &run = runs[r];
                    if (run.curve < 0)
                        continue;
                    const Handle(Geom2d_Curve) whole = flatCurve(run.curve);
                    const double reach = 2 * std::abs(profile.curves[std::size_t(run.curve)].scale);
                    auto onto = [&](const gp_Pnt2d &p, double u) {
                        Geom2dAPI_ProjectPointOnCurve to(p, whole, std::max(whole->FirstParameter(), u - reach), std::min(whole->LastParameter(), u + reach));
                        return to.NbPoints() ? to.LowerDistanceParameter() : u;
                    };
                    run.a = onto(corner[r], run.a);
                    run.b = onto(corner[(r + 1) % runCount], run.b);
                }
                const gp_Ax3 sheet(o.Translated(n * from), gp_Dir(n), gp_Dir(x));
                const gp_Pln pln(sheet);
                std::vector<TopoDS_Vertex> vertex(runCount);
                for (std::size_t r = 0; r < runCount; ++r)
                    vertex[r] = BRepBuilderAPI_MakeVertex(ElSLib::Value(corner[r].X(), corner[r].Y(), pln)).Vertex();
                auto at3 = [&](double u, double v) { return ElSLib::Value(u, v, pln); };
                // A corner where two pieces meet without an exact crossing sits on one of them, a hair
                // off the other: its vertex covers that miss, up to what the profile finder counts as
                // joined, so the edge it ends can still be made.
                auto cover = [](const TopoDS_Vertex &v, const gp_Pnt &on) {
                    const double miss = BRep_Tool::Pnt(v).Distance(on);
                    if (miss > BRep_Tool::Tolerance(v) && miss <= 1e-4)
                        BRep_Builder().UpdateVertex(v, 1.5 * miss);
                };
                BRepBuilderAPI_MakeWire wire;
                for (std::size_t r = 0; r < runCount; ++r) {
                    const Run &run = runs[r];
                    const TopoDS_Vertex &va = vertex[r], &vb = vertex[(r + 1) % runCount];
                    const gp_Pnt a = BRep_Tool::Pnt(va), b = BRep_Tool::Pnt(vb);
                    where = "loop " + std::to_string(i) + ", piece " + std::to_string(r) + " of " + std::to_string(runCount) + " ("
                        + (run.curve >= 0 ? "curve" : !run.centre ? "line" : run.steps == count ? "circle" : "arc") + ", "
                        + std::to_string(run.steps) + " step(s), corner to corner " + std::to_string(a.Distance(b)) + " mm)";
                    if (run.curve >= 0) {
                        const Handle(Geom_Curve) c3 = GeomAPI::To3d(flatCurve(run.curve), pln);
                        const bool forward = run.b > run.a;
                        // A curve that comes back to where it started with a sharp corner there (a figure
                        // eight's lobe, from its crossing round to it): two halves, as a whole circle is.
                        // One edge closing on itself makes its side wall a face that wraps round to itself
                        // over a seam, which is how a smoothly closed surface is held, not a corner: a cut
                        // through it lost that whole wall (no area left).
                        if (runCount == 1) {
                            gp_Pnt p0, p1;
                            gp_Vec d0, d1;
                            c3->D1(run.a, p0, d0);
                            c3->D1(run.b, p1, d1);
                            if (d0.Magnitude() > 0 && d1.Magnitude() > 0 && d0.Angle(d1) > 1e-7) {
                                const double mid = (run.a + run.b) / 2;
                                const TopoDS_Vertex vm = BRepBuilderAPI_MakeVertex(c3->Value(mid)).Vertex();
                                for (const auto &[from, to, v0, v1] : {std::tuple{run.a, mid, va, vm}, std::tuple{mid, run.b, vm, vb}}) {
                                    const bool ahead = to > from;
                                    cover(v0, c3->Value(from));
                                    cover(v1, c3->Value(to));
                                    BRepBuilderAPI_MakeEdge half(c3, ahead ? v0 : v1, ahead ? v1 : v0, std::min(from, to), std::max(from, to));
                                    if (!half.IsDone()) {
                                        m_whyNot = "a curve of the outline couldn't be made (" + where + ", error " + std::to_string(int(half.Error())) + ")";
                                        return 0;
                                    }
                                    TopoDS_Edge e = half.Edge();
                                    if (!ahead)
                                        e.Reverse();
                                    wire.Add(e);
                                }
                                continue;
                            }
                        }
                        cover(va, c3->Value(run.a));
                        cover(vb, c3->Value(run.b));
                        BRepBuilderAPI_MakeEdge make(c3, forward ? va : vb, forward ? vb : va, std::min(run.a, run.b), std::max(run.a, run.b));
                        if (!make.IsDone()) {
                            m_whyNot = "a curve of the outline couldn't be made (" + where + ", error " + std::to_string(int(make.Error())) + ")";
                            return 0;
                        }
                        TopoDS_Edge e = make.Edge();
                        if (!forward)
                            e.Reverse();
                        wire.Add(e);
                    } else if (!run.centre) {
                        if (a.Distance(b) > 1e-9)
                            wire.Add(BRepBuilderAPI_MakeEdge(va, vb).Edge());
                    } else if (run.steps == count) {           // a whole circle: two halves
                        const std::size_t half = count / 2;
                        auto drawn = [&](std::size_t k) { const Point2 &p = loop[(run.first + k) % count]; return at3(p.x, p.y); };
                        const TopoDS_Vertex vh = BRepBuilderAPI_MakeVertex(drawn(half)).Vertex();
                        wire.Add(BRepBuilderAPI_MakeEdge(GC_MakeArcOfCircle(a, drawn(half / 2), drawn(half)).Value(), va, vh).Edge());
                        wire.Add(BRepBuilderAPI_MakeEdge(GC_MakeArcOfCircle(drawn(half), drawn(half + (count - half) / 2), a).Value(), vh, va).Edge());
                    } else {
                        // The drawn circle itself (its centre and radius), from corner to corner, round the way
                        // the samples go: three points would make a circle of their own, off it by their error.
                        const Point2 *c = run.centre;
                        const double rad = radiusOf(run);
                        const Point2 &p = loop[run.first % count], &q = loop[(run.first + 1) % count];
                        if (std::hypot(q.x - p.x, q.y - p.y) < 1e-12 || rad < 1e-12) {
                            wire.Add(BRepBuilderAPI_MakeEdge(va, vb).Edge());
                            continue;
                        }
                        // anticlockwise if the first step heads that way round the centre
                        const bool ccw = (p.x - c->x) * (q.y - c->y) - (p.y - c->y) * (q.x - c->x) > 0;
                        const gp_Circ circ(gp_Ax2(at3(c->x, c->y), sheet.Direction(), sheet.XDirection()), rad);
                        {
                            char said[160];
                            std::snprintf(said, sizeof said, ", radius %.6f, ends off it by %.2g and %.2g mm", rad,
                                          std::abs(a.Distance(circ.Location()) - rad), std::abs(b.Distance(circ.Location()) - rad));
                            where += said;
                        }
                        for (const TopoDS_Vertex &v : {va, vb})
                            cover(v, ElCLib::Value(ElCLib::Parameter(circ, BRep_Tool::Pnt(v)), circ));
                        wire.Add(BRepBuilderAPI_MakeEdge(GC_MakeArcOfCircle(circ, a, b, ccw).Value(), va, vb).Edge());
                    }
                }
                if (!wire.IsDone()) {
                    m_whyNot = "the profile's outline doesn't close";
                    return 0;
                }
                where = "loop " + std::to_string(i) + ": its face";
                TopoDS_Wire w = wire.Wire();
                if ((i == 0) != (area(loop) > 0))       // outside anticlockwise, holes clockwise
                    w.Reverse();
                if (i == 0) {
                    face = BRepBuilderAPI_MakeFace(w, true).Face();
                } else {
                    face = BRepBuilderAPI_MakeFace(face, w).Face();
                }
            }
            if (face.IsNull()) {
                m_whyNot = "the profile couldn't be made into a face";
                return 0;
            }
            where = "pushing the face";
            return pushed(face, n, from, to, taper);
        } catch (const Standard_Failure &failure) {
            engineLog("engine: extrude not made: %s (at %s)\n", failure.GetMessageString(), where.c_str());
            m_whyNot = "the profile couldn't be made into a solid";
            return 0;
        }
    }

    // A face already at `from`, pushed along `n` to `to`, straight or with a taper (taper.h: each side
    // drafted on its own, positive widening the shape as it goes, Fusion's way round).
    BodyId pushed(const TopoDS_Face &face, const gp_Vec &n, double from, double to, double taper) {
        if (std::abs(taper) < 1e-9)
            return keep(BRepPrimAPI_MakePrism(face, n * (to - from)).Shape());
        const double a = taper * M_PI / 180;
        if (std::abs(a) >= M_PI / 2 - 1e-3) {
            m_whyNot = "the taper angle must be less than 90 degrees";
            return 0;
        }
        std::string why;
        const TopoDS_Shape made = taper::prism(face, n * (to - from), std::tan(a), why);
        if (made.IsNull()) {
            engineLog("engine: taper of %.3f degrees not made: %s\n", taper, why.c_str());
            m_whyNot = why;
            return 0;
        }
        return keep(made);
    }

    BodyId pushFace(BodyId body, int faceNumber, const Placement &place, double distance, std::string &why) override {
        try {
            TopTools_IndexedMapOfShape faces;
            TopExp::MapShapes(m_bodies.at(body), TopAbs_FACE, faces);
            if (faceNumber < 0 || faceNumber >= faces.Extent()) {
                why = "no such face";
                return 0;
            }
            const TopoDS_Face face = TopoDS::Face(faces(faceNumber + 1));
            const gp_Vec x(place.x[0], place.x[1], place.x[2]), y(place.y[0], place.y[1], place.y[2]);
            const gp_Vec n = x.Crossed(y).Normalized();
            const TopoDS_Shape before = m_bodies.at(body);
            const TopoDS_Shape moved = pushface::move(before, face, n * distance, why);
            if (moved.IsNull())
                return 0;
            // The moved face mustn't run into another part of the body: OpenCascade's self-intersection
            // check, up to edges through faces (level 4; level 5 also intersects every curved face with
            // itself over its whole parameter box, which a closed spline side always fails at its seam).
            const auto checking = std::chrono::steady_clock::now();
            BOPAlgo_CheckerSI clash;
            TopTools_ListOfShape argument;
            argument.Append(moved);
            clash.SetArguments(argument);
            clash.SetLevelOfCheck(4);
            clash.SetRunParallel(true);
            clash.Perform();
            const double took = std::chrono::duration<double>(std::chrono::steady_clock::now() - checking).count();
            if (clash.HasErrors()) {
                engineLog("engine: face move runs into the body (self-intersection check, %.2f s): not used\n", took);
                why = "the moved face would run into another part of the body";
                return 0;
            }
            engineLog("engine: face %d moved %.4f mm, its neighbours carried on along their own surfaces (checked in %.2f s)\n",
                         faceNumber, distance, took);
            return keep(moved);
        } catch (const Standard_Failure &failure) {
            why = failure.GetMessageString();
            return 0;
        }
    }

    BodyId extrudeFace(BodyId body, int faceNumber, const Placement &place, double from, double to, double taper) override {
        m_whyNot.clear();
        if (std::abs(to - from) < 1e-9)
            return 0;
        try {
            TopTools_IndexedMapOfShape faces;
            TopExp::MapShapes(m_bodies.at(body), TopAbs_FACE, faces);
            if (faceNumber < 0 || faceNumber >= faces.Extent())
                return 0;
            TopoDS_Face face = TopoDS::Face(faces(faceNumber + 1));
            if (BRepAdaptor_Surface(face).GetType() != GeomAbs_Plane)
                return 0;
            // Facing the way it is pushed: as a body's outer face it may face the other way. A copy, so
            // the body keeps its own edges.
            const gp_Vec x(place.x[0], place.x[1], place.x[2]), y(place.y[0], place.y[1], place.y[2]);
            const gp_Vec n = x.Crossed(y).Normalized();
            face.Orientation(TopAbs_FORWARD);
            BRepAdaptor_Surface surface(face);
            if (gp_Vec(surface.Plane().Axis().Direction()).Dot(n) < 0)
                face.Reverse();
            gp_Trsf shift;
            shift.SetTranslation(n * from);
            const TopoDS_Face start = TopoDS::Face(BRepBuilderAPI_Transform(face, shift, true).Shape());
            return pushed(start, n, from, to, taper);
        } catch (const Standard_Failure &) {
            return 0;
        }
    }

    double looseness(BodyId body) const override {
        const auto it = m_bodies.find(body);
        return it == m_bodies.end() ? 0 : loosest(it->second);
    }

    double volume(BodyId body) const override {
        GProp_GProps props;
        BRepGProp::VolumeProperties(m_bodies.at(body), props);
        return std::abs(props.Mass());
    }

    BodyId merged(BodyId body) override {
        try {
            const TopoDS_Shape &shape = m_bodies.at(body);
            const auto start = std::chrono::steady_clock::now();
            // The same merge as a Boolean's result gets (combine): FreeCAD's way, then OpenCascade's.
            TopoDS_Shape refined = refine::sameSurfaces(shape);
            if (refined.IsNull())
                refined = shape;
            if (!mergeable(refined))
                return 0;                // OpenCascade's merge would crash on it: left as it is
            // A hole's two halves are left apart if joined they couldn't be drawn (refine.h).
            const TopoDS_Shape out = refine::unifyDrawable(refined, kMergeLinear, kMergeAngular);
            if (out.IsNull())
                return 0;
            auto count = [](const TopoDS_Shape &s) { int n = 0; for (TopExp_Explorer e(s, TopAbs_FACE); e.More(); e.Next()) ++n; return n; };
            const int before = count(shape), after = count(out);
            if (after >= before)
                return 0;   // nothing in one surface: left exactly as it was
            // Kept only if it's the same solid: sound, no face of no area, no looser than it was (or
            // 0.00001 in), and the same volume to a millionth of a percent (merging moves no surface).
            const auto vol = [](const TopoDS_Shape &s) { GProp_GProps p; BRepGProp::VolumeProperties(s, p); return std::abs(p.Mass()); };
            const double was = vol(shape), now = vol(out);
            const double allowed = std::max(kAccuracy, loosest(shape));
            const char *refused = loosest(out) > allowed ? "looser than it was"
                                  : hasEmptyFace(out) ? "a face of no area"
                                  : std::abs(now - was) > 1e-8 * was ? "a different volume"
                                  : !soundSolid(out) ? "not a sound solid" : nullptr;
            if (refused) {
                engineLog("engine: body %llu faces in one surface not merged: merged would be %s\n", (unsigned long long)body, refused);
                return 0;
            }
            engineLog("engine: body %llu faces in one surface merged, %d faces to %d, checked in %.2f s\n", (unsigned long long)body,
                      before, after, std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count());
            return keepSound(out);
        } catch (const Standard_Failure &failure) {
            engineLog("engine: body %llu faces in one surface not merged: %s\n", (unsigned long long)body, failure.GetMessageString());
            return 0;
        }
    }

    std::vector<BodyId> pieces(BodyId body) override {
        std::vector<std::pair<double, TopoDS_Shape>> found;
        for (TopExp_Explorer e(m_bodies.at(body), TopAbs_SOLID); e.More(); e.Next()) {
            GProp_GProps props;
            BRepGProp::VolumeProperties(e.Current(), props);
            found.push_back({std::abs(props.Mass()), e.Current()});
        }
        if (found.size() < 2)
            return {};
        std::stable_sort(found.begin(), found.end(), [](const auto &a, const auto &b) { return a.first > b.first; });
        const bool sound = m_sound.count(body) > 0;   // each solid of a body checked sound in full is sound too
        std::vector<BodyId> out;
        for (const auto &[volume, solid] : found)
            out.push_back(sound ? keepSound(solid) : keep(solid));
        engineLog("engine: body %llu is %zu separate solids, each now a body of its own\n", (unsigned long long)body, out.size());
        return out;
    }

    double gap(BodyId a, BodyId b, double search) const override {
        try {
            BRepExtrema_DistShapeShape distance(m_bodies.at(a), m_bodies.at(b));
            if (!distance.IsDone())
                return search;
            const double d = distance.Value();
            if (d > 1e-7)
                return std::min(d, search);
            // Zero distance: they touch or overlap; either is "no gap".
            return 0;
        } catch (const Standard_Failure &) {
            return search;
        }
    }

    bool planeOfFace(BodyId body, int faceNumber, double origin[3], double normal[3]) const override {
        try {
            TopTools_IndexedMapOfShape faces;
            TopExp::MapShapes(m_bodies.at(body), TopAbs_FACE, faces);
            if (faceNumber < 0 || faceNumber >= faces.Extent())
                return false;
            const TopoDS_Face face = TopoDS::Face(faces(faceNumber + 1));
            BRepAdaptor_Surface surface(face);
            if (surface.GetType() != GeomAbs_Plane)
                return false;
            const gp_Pln plane = surface.Plane();
            gp_Dir n = plane.Axis().Direction();
            if (face.Orientation() == TopAbs_REVERSED)
                n.Reverse();
            const gp_Pnt o = plane.Location();
            origin[0] = o.X(), origin[1] = o.Y(), origin[2] = o.Z();
            normal[0] = n.X(), normal[1] = n.Y(), normal[2] = n.Z();
            return true;
        } catch (const Standard_Failure &) {
            return false;
        }
    }

    // A body's edges and corners seen straight down on a plane (FreeCAD's processEdge does the same
    // for its sketches' external geometry: SketchObjectExternal.cpp). Lines from their vertices, and
    // circles lying parallel to the plane as a circle round their centre, so a projected wall's arc is
    // the wall's own circle and the extrude from it the same surface as the wall.
    bool flatCurves(BodyId body, int face, const Placement &plane, std::vector<FlatCurve> &curves,
                    std::vector<Point2> &corners) const override {
        try {
            const TopoDS_Shape &shape = m_bodies.at(body);
            TopoDS_Shape from = shape;
            if (face >= 0) {
                TopTools_IndexedMapOfShape faces;
                TopExp::MapShapes(shape, TopAbs_FACE, faces);
                if (face >= faces.Extent())
                    return false;
                from = faces(face + 1);
            }
            const gp_Pnt o(plane.origin[0], plane.origin[1], plane.origin[2]);
            const gp_Vec x(plane.x[0], plane.x[1], plane.x[2]), y(plane.y[0], plane.y[1], plane.y[2]);
            const gp_Dir n(x.Crossed(y));
            const auto flat = [&](const gp_Pnt &p) { const gp_Vec d(o, p); return Point2{d.Dot(x), d.Dot(y)}; };
            TopTools_IndexedMapOfShape edges, vertices;
            TopExp::MapShapes(from, TopAbs_EDGE, edges);
            TopExp::MapShapes(from, TopAbs_VERTEX, vertices);
            for (int i = 1; i <= vertices.Extent(); ++i)
                corners.push_back(flat(BRep_Tool::Pnt(TopoDS::Vertex(vertices(i)))));
            for (int i = 1; i <= edges.Extent(); ++i) {
                const TopoDS_Edge &edge = TopoDS::Edge(edges(i));
                if (BRep_Tool::Degenerated(edge))
                    continue;
                TopoDS_Vertex first, last;
                TopExp::Vertices(edge, first, last, true);
                if (first.IsNull() || last.IsNull())
                    continue;
                const BRepAdaptor_Curve curve(edge);
                FlatCurve c;
                c.a = flat(BRep_Tool::Pnt(first));
                c.b = flat(BRep_Tool::Pnt(last));
                if (curve.GetType() == GeomAbs_Line) {
                    if (std::hypot(c.a.x - c.b.x, c.a.y - c.b.y) < 1e-9)
                        continue;          // straight down at the sketch: a point
                } else if (curve.GetType() == GeomAbs_Circle && curve.Circle().Axis().Direction().IsParallel(n, 1e-12)) {
                    const gp_Circ circle = curve.Circle();
                    c.round = true;
                    c.c = flat(circle.Location());
                    c.r = circle.Radius();
                    c.whole = first.IsSame(last) && curve.LastParameter() - curve.FirstParameter() > M_PI;
                } else {
                    continue;
                }
                curves.push_back(c);
            }
            return true;
        } catch (const Standard_Failure &) {
            return false;
        }
    }

    bool shadow(const std::vector<BodyId> &bodies, int face, const Placement &plane,
                std::vector<std::vector<FlatPiece>> &loops, std::string &why) const override {
        std::vector<TopoDS_Shape> shapes;
        try {
            for (const BodyId b : bodies)
                shapes.push_back(m_bodies.at(b));
        } catch (const std::out_of_range &) {
            why = "that body is gone";
            return false;
        }
        if (face >= 0) {
            TopTools_IndexedMapOfShape faces;
            if (!shapes.empty())
                TopExp::MapShapes(shapes.front(), TopAbs_FACE, faces);
            if (face >= faces.Extent()) {
                why = "that face is gone";
                return false;
            }
            shapes = {faces(face + 1)};
        }
        const auto t0 = std::chrono::steady_clock::now();
        const bool ok = projectexact::shadow(shapes, plane, loops, why);
        std::size_t pieces = 0;
        for (const auto &l : loops)
            pieces += l.size();
        engineLog("engine: Project, exact: %s, %zu outlines, %zu pieces, %.3f s%s%s\n", face < 0 ? "the whole model" : "one face",
                  loops.size(), pieces, std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(),
                  ok ? "" : ", failed: ", ok ? "" : why.c_str());
        return ok;
    }

    bool flatPart(BodyId body, char kind, int index, const Placement &plane, std::vector<FlatPiece> &pieces,
                  std::string &why) const override {
        const auto found = m_bodies.find(body);
        if (found == m_bodies.end()) {
            why = "that body is gone";
            return false;
        }
        TopTools_IndexedMapOfShape seams;
        const std::vector<ShownEdge> edges = shownEdges(found->second, seams);
        if (kind == 'p') {
            const std::vector<TopoDS_Vertex> corners = shownCorners(found->second, seams);
            if (index < 0 || std::size_t(index) >= corners.size()) {
                why = "that corner is gone";
                return false;
            }
            pieces.push_back(projectexact::corner(corners[std::size_t(index)], plane));
            return true;
        }
        if (index < 0 || std::size_t(index) >= edges.size()) {
            why = "that edge is gone";
            return false;
        }
        return projectexact::edge(edges[std::size_t(index)].edge, plane, pieces, why);
    }

    // How far along `way` (a unit vector) from `start` a face, edge or corner of a body lies, measured on
    // the exact shape (owner, 2026-11-04: the mesh is single precision and up to 0.01 mm off a curve).
    // A face: where a line from start along the way first meets it, nearest either side. An edge: its
    // point farthest from start along the way, either side. A corner: its point.
    bool reach(BodyId body, char kind, int index, const double start[3], const double way[3], double &mm) const override {
        try {
            const TopoDS_Shape &shape = m_bodies.at(body);
            const gp_Pnt from(start[0], start[1], start[2]);
            const gp_Dir dir(way[0], way[1], way[2]);
            auto along = [&](const gp_Pnt &p) { return gp_Vec(from, p).Dot(gp_Vec(dir)); };
            if (kind == 'f') {
                TopTools_IndexedMapOfShape faces;
                TopExp::MapShapes(shape, TopAbs_FACE, faces);
                if (index < 0 || index >= faces.Extent())
                    return false;
                IntCurvesFace_ShapeIntersector cross;
                cross.Load(faces(index + 1), Precision::Confusion());
                cross.Perform(gp_Lin(from, dir), -Precision::Infinite(), Precision::Infinite());
                if (!cross.IsDone() || cross.NbPnt() == 0)
                    return false;
                double best = 0;
                bool any = false;
                for (int i = 1; i <= cross.NbPnt(); ++i)
                    if (!any || std::abs(cross.WParameter(i)) < std::abs(best)) {
                        best = cross.WParameter(i);
                        any = true;
                    }
                mm = best;
                return true;
            }
            TopTools_IndexedMapOfShape seams;
            const std::vector<ShownEdge> edges = shownEdges(shape, seams);
            if (kind == 'p') {
                const std::vector<TopoDS_Vertex> corners = shownCorners(shape, seams);
                if (index < 0 || std::size_t(index) >= corners.size())
                    return false;
                mm = along(BRep_Tool::Pnt(corners[std::size_t(index)]));
                return true;
            }
            if (kind != 'e' || index < 0 || std::size_t(index) >= edges.size())
                return false;
            // The edge's nearest point to a plane across the way, far out ahead and then far back:
            // its farthest point each way, exactly (BRepExtrema solves it on the curve itself).
            Bnd_Box box;
            BRepBndLib::Add(shape, box);
            const double far = std::sqrt(box.SquareExtent()) + from.Distance(box.IsVoid() ? from : gp_Pnt((box.CornerMin().XYZ() + box.CornerMax().XYZ()) / 2)) + 1;
            double best = 0;
            bool any = false;
            for (int side : {1, -1}) {
                const gp_Pnt at = from.Translated(gp_Vec(dir) * (side * far));
                const TopoDS_Face sheet = BRepBuilderAPI_MakeFace(gp_Pln(at, dir), -2 * far, 2 * far, -2 * far, 2 * far).Face();
                BRepExtrema_DistShapeShape near(edges[std::size_t(index)].edge, sheet);
                if (!near.IsDone() || near.NbSolution() == 0)
                    continue;
                const double a = along(near.PointOnShape1(1));
                if (!any || std::abs(a) > std::abs(best)) {
                    best = a;
                    any = true;
                }
            }
            if (any)
                mm = best;
            return any;
        } catch (const Standard_Failure &) {
            return false;
        }
    }

    bool faceProfile(BodyId body, int faceNumber, Placement &place, Profile &profile) const override {
        try {
            TopTools_IndexedMapOfShape faces;
            TopExp::MapShapes(m_bodies.at(body), TopAbs_FACE, faces);
            if (faceNumber < 0 || faceNumber >= faces.Extent())
                return false;
            const TopoDS_Face face = TopoDS::Face(faces(faceNumber + 1));
            BRepAdaptor_Surface surface(face);
            if (surface.GetType() != GeomAbs_Plane)
                return false;
            const gp_Pln plane = surface.Plane();
            gp_Dir normal = plane.Axis().Direction();
            if (face.Orientation() == TopAbs_REVERSED)
                normal.Reverse();
            const gp_Pnt origin = plane.Location();
            const gp_Dir xDir = plane.XAxis().Direction();
            const gp_Dir yDir = normal.Crossed(xDir);
            const double o[3] = {origin.X(), origin.Y(), origin.Z()};
            const double xs[3] = {xDir.X(), xDir.Y(), xDir.Z()}, ys[3] = {yDir.X(), yDir.Y(), yDir.Z()};
            for (int k = 0; k < 3; ++k) {
                place.origin[k] = o[k];
                place.x[k] = xs[k];
                place.y[k] = ys[k];
            }
            profile.loops.clear();
            const TopoDS_Wire outer = BRepTools::OuterWire(face);
            auto loopOf = [&](const TopoDS_Wire &wire) {
                std::vector<Point2> loop;
                for (BRepTools_WireExplorer walk(wire, face); walk.More(); walk.Next()) {
                    const TopoDS_Edge edge = walk.Current();
                    BRepAdaptor_Curve curve(edge);
                    std::vector<gp_Pnt> points;
                    // 0.001 mm off the curve at most (0.00004 in): the outline is only for the meshes'
                    // estimates and the edges-moved try; the extrude itself pushes the face (extrudeFace).
                    GCPnts_TangentialDeflection sample(curve, 0.2, 0.001);
                    for (int i = 1; i <= sample.NbPoints(); ++i)
                        points.push_back(sample.Value(i));
                    if (edge.Orientation() == TopAbs_REVERSED)
                        std::reverse(points.begin(), points.end());
                    for (std::size_t i = loop.empty() ? 0 : 1; i < points.size(); ++i) {
                        const gp_Vec d(origin, points[i]);
                        loop.push_back({d.Dot(gp_Vec(xDir)), d.Dot(gp_Vec(yDir))});
                    }
                }
                if (loop.size() > 1 && std::abs(loop.front().x - loop.back().x) < 1e-9 && std::abs(loop.front().y - loop.back().y) < 1e-9)
                    loop.pop_back();
                return loop;
            };
            profile.loops.push_back(loopOf(outer));
            for (TopExp_Explorer w(face, TopAbs_WIRE); w.More(); w.Next()) {
                const TopoDS_Wire wire = TopoDS::Wire(w.Current());
                if (!wire.IsSame(outer))
                    profile.loops.push_back(loopOf(wire));
            }
            return profile.loops.front().size() >= 3;
        } catch (const Standard_Failure &) {
            return false;
        }
    }

    // Fuzzy profile (owner, 2026-11-06; automatic since 2026-11-07): only when asked for (Extrude's second try,
    // after the whole extrude failed exactly), and only after this exact Boolean has failed too.
    BodyId combine(BodyId a, const std::vector<BodyId> &with, Combine op, double fuzzy) override {
        const BodyId exact = combineAt(a, with, op, kTouching);
        if (exact || fuzzy <= kTouching || omamake::stopAsked())   // stopped (GB-089): no second Boolean
            return exact;
        const std::string exactWhy = m_whyNot;
        engineLog("engine: the exact one couldn't be built (%s); Fuzzy profile is on: trying again with points %.4g mm apart taken as one\n",
                  exactWhy.c_str(), fuzzy);
        const BodyId fuzzed = combineAt(a, with, op, fuzzy);
        if (fuzzed)
            engineLog("engine: built with Fuzzy profile (%.4g mm)\n", fuzzy);
        else
            m_whyNot = exactWhy + ", not even with Fuzzy profile";
        return fuzzed;
    }

    BodyId combineAt(BodyId a, const std::vector<BodyId> &with, Combine op, double fuzz) {
        m_whyNot.clear();
        if (with.empty())
            return 0;
        // Each body as a Boolean can use it: put right first if it needs it (mended, above).
        const TopoDS_Shape first = usable(a);
        // One job for every tool, as OpenCascade's Boolean takes them (a list of tools), so a profile
        // that makes several areas is one pass and one check, not one per area.
        TopTools_ListOfShape arguments, tools;
        arguments.Append(first);
        for (const BodyId b : with)
            tools.Append(usable(b));
        static const char *const names[] = {"join", "cut", "intersect"};
        const auto started = std::chrono::steady_clock::now();
        auto say = [&](const char *what) {
            // In the window's log, so a bad result can be traced afterwards (owner, 2026-11-03).
            engineLog("engine: %s with %zu tool(s): %s, %.2f s\n", names[int(op)], with.size(), what,
                         std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count());
        };
        try {
            std::unique_ptr<BRepAlgoAPI_BooleanOperation> job;
            switch (op) {
            case Combine::Join: job = std::make_unique<BRepAlgoAPI_Fuse>(); break;
            case Combine::Cut: job = std::make_unique<BRepAlgoAPI_Cut>(); break;
            case Combine::Intersect: job = std::make_unique<BRepAlgoAPI_Common>(); break;
            }
            job->SetArguments(arguments);
            job->SetTools(tools);
            job->SetRunParallel(true);
            job->SetUseOBB(true);              // boxes that turn with each face: far fewer faces to compare
            job->SetNonDestructive(true);      // the body as it was stays untouched, for undo
            // Faces within 0.00001 mm count as touching (Parasolid's session precision): a join
            // ending 0.000003 mm short of a face left sliver walls and a line round where it met
            // (owner, 2026-11-03). Far under any tolerance that shows or exports.
            job->SetFuzzyValue(fuzz);
            // GB-089: built with a progress range whose break check stops it (time limit or the memory guard).
            Handle(BreakCheck) breakCheck = new BreakCheck;
            job->Build(breakCheck->Start());
            if (omamake::stopAsked()) {
                say("stopped (time limit or low memory)");
                m_whyNot = "it was stopped before it finished";
                return 0;
            }
            if (!job->IsDone() || job->HasErrors() || job->Shape().IsNull()) {
                say("failed");
                m_whyNot = "the shapes couldn't be worked out where they meet";
                return 0;
            }
            // Did it do anything? From the boolean's own history, not from volumes (OpenCascade's
            // volume of a long spline edge's face can be out by a percent: a fixed number of sample
            // points per edge). A tool face in the result, or a face of the body changed or gone,
            // means the tools reached the body.
            bool reached = false;
            for (TopTools_ListIteratorOfListOfShape it(tools); it.More() && !reached; it.Next())
                for (TopExp_Explorer f(it.Value(), TopAbs_FACE); f.More() && !reached; f.Next())
                    reached = !job->IsDeleted(f.Current());
            for (TopExp_Explorer f(first, TopAbs_FACE); f.More() && !reached; f.Next())
                reached = job->IsDeleted(f.Current()) || !job->Modified(f.Current()).IsEmpty();
            if (!reached) {
                // The body as it was: right only when the tools really miss it (cut), lie inside it
                // (join), or it lies inside them (intersect). Asked of a point inside each, exactly.
                bool right = true;
                for (TopTools_ListIteratorOfListOfShape it(tools); it.More() && right; it.Next()) {
                    gp_Pnt p;
                    const bool fromBody = op == Combine::Intersect;
                    if (!pointInside(fromBody ? first : it.Value(), p)) {
                        right = false;
                        break;
                    }
                    BRepClass3d_SolidClassifier where(fromBody ? it.Value() : first, p, Precision::Confusion());
                    right = op == Combine::Cut ? where.State() == TopAbs_OUT : where.State() == TopAbs_IN;
                }
                if (!right) {
                    say("the body came back as it was, though the tools reach into it; not used");
                    m_whyNot = "the shapes couldn't be worked out where they meet";
                    return 0;
                }
                say("nothing to change");
                return keep(first);
            }
            TopoDS_Shape result = job->Shape();
            if (const TopoDS_Shape exact = exactSections(result, first, tools); !exact.IsNull())
                result = exact;
            if (omamake::stopAsked()) {     // GB-089: between the Boolean's own steps too
                say("stopped (time limit or low memory)");
                m_whyNot = "it was stopped before it finished";
                return 0;
            }
            // Faces left lying in one surface become one, as in Fusion: a filled hole otherwise leaves
            // its rim drawn as a circle on the face (owner, 2026-11-03). FreeCAD's way first
            // (refine.h): every group of faces on exactly one plane, cylinder or cone made one face;
            // OpenCascade's merge after it, for spheres, rings and free-form faces. Within 0.0001 mm
            // and 0.00000001 rad, under 0.00001 in even across 20 ft. The full check runs once, on the merged result.
            const auto merging = std::chrono::steady_clock::now();
            TopoDS_Shape merged = result;
            if (const TopoDS_Shape refined = refine::sameSurfaces(result); !refined.IsNull())
                merged = refined;
            TopoDS_Shape unified = merged;
            if (mergeable(merged, m_chamfering)) {
                // A hole's two halves are left apart if joined they couldn't be drawn (refine.h).
                if (const TopoDS_Shape drawable = refine::unifyDrawable(merged, kMergeLinear, kMergeAngular); !drawable.IsNull())
                    unified = drawable;
            } else {
                engineLog("engine: %s: an edge lacks its curve on a face, so faces weren't merged (OpenCascade's merge would crash)\n", names[int(op)]);
            }
            // Kept only if sound, with no face of no area, and no looser than what went in or 0.00001 in:
            // a boolean that has to widen an edge past that glued together things that don't really
            // meet. A cut of a converted model whose walls lay a hair off the tool's came back valid and
            // closed that way, with edges loosened to 0.31 mm (the model's own were 0.06) and next to
            // nothing cut away (2026-11-05).
            double allowed = std::max(kAccuracy, loosest(first));
            for (TopTools_ListIteratorOfListOfShape it(tools); it.More(); it.Next())
                allowed = std::max(allowed, loosest(it.Value()));
            enum class Flaw { None, Unsound, Empty, Loose, Gone };
            double worst = 0;
            // Nothing at all left is right only for a cut whose tools cover the whole body: the
            // empty result otherwise passed every check here (no faces, so nothing unsound), and a
            // 2 mm cut of a converted hinge said "done" with the body gone (round 5). Asked once, of
            // the volume the body and the tools share against the body's own.
            int gone = -1;
            auto takenAway = [&]() {
                if (gone < 0) {
                    gone = 0;
                    if (op == Combine::Cut) {
                        BRepAlgoAPI_Common shared;
                        shared.SetArguments(arguments);
                        shared.SetTools(tools);
                        shared.SetRunParallel(true);
                        shared.SetFuzzyValue(fuzz);
                        shared.Build();
                        GProp_GProps body, common;
                        BRepGProp::VolumeProperties(first, body);
                        if (shared.IsDone() && !shared.HasErrors()) {
                            BRepGProp::VolumeProperties(shared.Shape(), common);
                            gone = std::abs(common.Mass() - body.Mass()) <= 1e-6 * std::max(1.0, std::abs(body.Mass()));
                        }
                    }
                }
                return gone == 1;
            };
            // Worst first, as the reason given: nothing left where something should be, loosened (the
            // cause: a sliver face or a crack can come of it), then a face of no area, then anything
            // else unsound.
            auto flaw = [&](const TopoDS_Shape &shape) {
                if (!TopExp_Explorer(shape, TopAbs_SOLID).More() && !takenAway())
                    return Flaw::Gone;
                worst = loosest(shape);
                if (worst > allowed)
                    return Flaw::Loose;
                if (hasEmptyFace(shape))
                    return Flaw::Empty;
                return soundSolid(shape) ? Flaw::None : Flaw::Unsound;
            };
            if (omamake::stopAsked()) {     // GB-089
                say("stopped (time limit or low memory)");
                m_whyNot = "it was stopped before it finished";
                return 0;
            }
            const Flaw mergedFlaw = flaw(unified);
            if (mergedFlaw == Flaw::None) {
                say("done, faces in one surface merged");
                engineLog("engine: %s, merged and checked in %.2f s, loosest edge %.2g mm (allowed %.2g)\n", names[int(op)],
                             std::chrono::duration<double>(std::chrono::steady_clock::now() - merging).count(), worst, allowed);
                return keepSound(unified);
            }
            const double mergedWorst = worst;
            Flaw unmerged = flaw(result);
            if (unmerged == Flaw::None) {
                say("done, faces not merged (the merge spoiled it)");
                return keepSound(result);
            }
            // GB-094: refused only for a face of no area: the cut's sliver faces folded in (foldTinyFaces), then
            // merged and checked in full again, as above.
            if ((mergedFlaw == Flaw::Empty || unmerged == Flaw::Empty) && !omamake::stopAsked()) {
                if (const TopoDS_Shape folded = foldTinyFaces(result, allowed); !folded.IsNull()) {
                    TopoDS_Shape foldedMerged = folded;
                    if (const TopoDS_Shape refined = refine::sameSurfaces(folded); !refined.IsNull())
                        foldedMerged = refined;
                    if (mergeable(foldedMerged, m_chamfering))
                        if (const TopoDS_Shape drawable = refine::unifyDrawable(foldedMerged, kMergeLinear, kMergeAngular); !drawable.IsNull())
                            foldedMerged = drawable;
                    if (flaw(foldedMerged) == Flaw::None) {
                        say("done, sliver faces folded in and faces in one surface merged");
                        return keepSound(foldedMerged);
                    }
                    if (flaw(folded) == Flaw::None) {
                        say("done, sliver faces folded in, faces not merged");
                        return keepSound(folded);
                    }
                }
            }
            {   // The refused result, described for the log (both forms).
                std::string merged, unmergedReport;
                describe(unified, merged);
                describe(result, unmergedReport);
                engineLog("engine: the refused result, faces merged:\n%s", merged.c_str());
                engineLog("engine: the refused result, faces not merged:\n%s", unmergedReport.c_str());
            }
            // The reason given is the telling one of the two.
            auto rank = [](Flaw f) { return f == Flaw::Gone ? 4 : f == Flaw::Loose ? 3 : f == Flaw::Empty ? 2 : 1; };
            if (rank(mergedFlaw) > rank(unmerged)) {
                unmerged = mergedFlaw;
                worst = mergedWorst;
            }
            // Said plainly, as Fusion does.
            if (unmerged == Flaw::Gone) {
                say("nothing came back, though the tools don't cover the whole body; not used");
                m_whyNot = "the result came back empty, though the cut doesn't take the whole body away";
            } else if (unmerged == Flaw::Empty) {
                say("result has a face of no area (parts of one surface meeting along a line), not used");
                m_whyNot = "the result would touch itself along a line, leaving a face with no area there";
            } else if (unmerged == Flaw::Loose) {
                engineLog("engine: loosest edge %.4f mm, allowed %.4f\n", worst, allowed);
                logLoose("merged", unified, allowed);
                logLoose("unmerged", result, allowed);
                say("result had to be loosened past what went in to fit together, not used");
                m_whyNot = "the shapes don't quite meet where they should: the result would be looser than 0.00001 in there";
            } else {
                say("result isn't a sound solid (invalid, or a crack with a face on one side only), not used");
                m_whyNot = "the result wouldn't be a sound solid";
            }
        } catch (const Standard_Failure &failure) {
            say(failure.GetMessageString());
            m_whyNot = "the shapes couldn't be worked out where they meet";
        }
        return 0;
    }

    std::string whyNot() const override { return m_whyNot; }

    // GB-093 (owner, 2026-11-17): see engine.h. A cut whose profile was projected from the bodies it cuts
    // has walls lying on the bodies' own walls; OpenCascade's Boolean then has to settle faces a hair apart
    // and can leave a sliver solid and a loosened corner (b205d, 2026-11-17: 0.377 mm). Each such step is
    // moved off its wall into the empty side, so the tool's wall and the body's never coincide. The bodies are
    // read from their triangles (every shown body has them): a step is on a wall where triangles on one side
    // of it have an edge along it (a wall straight along the push, or a slanting face whose edge is the
    // outline, as at a hole's narrowest ring), and it moves only if no triangle at all lies in the strip it
    // would cross, over the whole push.
    static bool triangleInBox(const double centre[3], const double half[3], const double (&corner)[3][3]) {
        // Akenine-Moeller's separating axis test: the box's 3 axes, the triangle's normal, and the 9 crossings.
        double v[3][3], e[3][3];
        for (int i = 0; i < 3; ++i)
            for (int k = 0; k < 3; ++k)
                v[i][k] = corner[i][k] - centre[k];
        for (int i = 0; i < 3; ++i)
            for (int k = 0; k < 3; ++k)
                e[i][k] = v[(i + 1) % 3][k] - v[i][k];
        for (int k = 0; k < 3; ++k) {
            const double lo = std::min({v[0][k], v[1][k], v[2][k]}), hi = std::max({v[0][k], v[1][k], v[2][k]});
            if (lo > half[k] || hi < -half[k])
                return false;
        }
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j) {
                double axis[3] = {0, 0, 0};   // box axis i crossed with edge j
                const int a = (i + 1) % 3, b = (i + 2) % 3;
                axis[a] = -e[j][b];
                axis[b] = e[j][a];
                double lo = INFINITY, hi = -INFINITY;
                for (int t = 0; t < 3; ++t) {
                    const double d = axis[0] * v[t][0] + axis[1] * v[t][1] + axis[2] * v[t][2];
                    lo = std::min(lo, d);
                    hi = std::max(hi, d);
                }
                const double r = half[0] * std::abs(axis[0]) + half[1] * std::abs(axis[1]) + half[2] * std::abs(axis[2]);
                if (lo > r || hi < -r)
                    return false;
            }
        const double n[3] = {e[0][1] * e[1][2] - e[0][2] * e[1][1], e[0][2] * e[1][0] - e[0][0] * e[1][2], e[0][0] * e[1][1] - e[0][1] * e[1][0]};
        const double d = n[0] * v[0][0] + n[1] * v[0][1] + n[2] * v[0][2];
        return std::abs(d) <= half[0] * std::abs(n[0]) + half[1] * std::abs(n[1]) + half[2] * std::abs(n[2]);
    }

    std::string clearWalls(Profile &profile, const Placement &place, double from, double to,
                           const std::vector<BodyId> &bodies, double gap) override {
        const auto started = std::chrono::steady_clock::now();
        constexpr double kOn = 0.001;      // a triangle corner this close to a step's line is on it (mm)
        constexpr double kAcross = 0.05;   // a triangle facing less across the step than this touches it along no wall
        constexpr double kCell = 1.0;      // the triangles' grid on the sketch (mm)
        if (gap <= 0 || bodies.empty() || std::abs(to - from) < 1e-9)
            return {};
        try {
            const gp_Pnt o(place.origin[0], place.origin[1], place.origin[2]);
            const gp_Vec X = gp_Vec(place.x[0], place.x[1], place.x[2]).Normalized();
            const gp_Vec Y = gp_Vec(place.y[0], place.y[1], place.y[2]).Normalized();
            const gp_Vec N = X.Crossed(Y).Normalized();
            const double hlo = std::min(from, to), hhi = std::max(from, to);
            // Every triangle of the bodies within the push, in the sketch's terms (u, v along it, h along the
            // push), with its normal out of the material. A face with no triangles is kept as its box: it stops a
            // move across it and is on no wall. A curved face's triangles stand off it by up to their deflection.
            struct Tri {
                double p[3][3] = {};
                double a = 0, b = 0;           // the outward normal's (u, v) part
                double pad = 0;
                bool flat = true, box = false;
                double lo[3] = {INFINITY, INFINITY, INFINITY}, hi[3] = {-INFINITY, -INFINITY, -INFINITY};
            };
            std::vector<Tri> tris;
            auto into = [&](const gp_Pnt &q, double (&at)[3]) {
                const gp_Vec r(o, q);
                at[0] = r.Dot(X);
                at[1] = r.Dot(Y);
                at[2] = r.Dot(N);
            };
            auto keep = [&](Tri &t, int corners) {
                for (int i = 0; i < corners; ++i)
                    for (int k = 0; k < 3; ++k) {
                        t.lo[k] = std::min(t.lo[k], t.p[i][k]);
                        t.hi[k] = std::max(t.hi[k], t.p[i][k]);
                    }
                if (t.hi[2] < hlo - kOn - t.pad || t.lo[2] > hhi + kOn + t.pad)
                    return;                     // wholly above or below the push
                tris.push_back(t);
            };
            int bare = 0;
            for (const BodyId id : bodies) {
                const auto found = m_bodies.find(id);
                if (found == m_bodies.end())
                    continue;
                TopTools_IndexedMapOfShape all;
                TopExp::MapShapes(found->second, TopAbs_FACE, all);
                for (int f = 1; f <= all.Extent(); ++f) {
                    const TopoDS_Face face = TopoDS::Face(all(f));
                    TopLoc_Location loc;
                    const Handle(Poly_Triangulation) mesh = BRep_Tool::Triangulation(face, loc);
                    if (mesh.IsNull() || mesh->NbTriangles() == 0) {
                        Bnd_Box box;
                        BRepBndLib::Add(face, box, false);
                        if (box.IsVoid())
                            continue;
                        ++bare;
                        double c[6];
                        box.Get(c[0], c[1], c[2], c[3], c[4], c[5]);
                        Tri t;
                        t.box = true;
                        t.flat = false;
                        for (int k = 0; k < 8; ++k) {
                            double at[3];
                            into(gp_Pnt(c[(k & 1) ? 3 : 0], c[(k & 2) ? 4 : 1], c[(k & 4) ? 5 : 2]), at);
                            for (int j = 0; j < 3; ++j) {
                                t.lo[j] = std::min(t.lo[j], at[j]);
                                t.hi[j] = std::max(t.hi[j], at[j]);
                            }
                        }
                        if (!(t.hi[2] < hlo - kOn || t.lo[2] > hhi + kOn))
                            tris.push_back(t);
                        continue;
                    }
                    const bool planar = BRepAdaptor_Surface(face, false).GetType() == GeomAbs_Plane;
                    const gp_Trsf where = loc.Transformation();
                    const bool reversed = face.Orientation() == TopAbs_REVERSED;
                    for (int k = 1; k <= mesh->NbTriangles(); ++k) {
                        int n1, n2, n3;
                        mesh->Triangle(k).Get(n1, n2, n3);
                        if (reversed)
                            std::swap(n2, n3);
                        const gp_Pnt P1 = mesh->Node(n1).Transformed(where), P2 = mesh->Node(n2).Transformed(where),
                                     P3 = mesh->Node(n3).Transformed(where);
                        Tri t;
                        t.flat = planar;
                        t.pad = planar ? 0 : mesh->Deflection();
                        into(P1, t.p[0]);
                        into(P2, t.p[1]);
                        into(P3, t.p[2]);
                        const gp_Vec n = gp_Vec(P1, P2).Crossed(gp_Vec(P1, P3));
                        if (n.Magnitude() > 1e-18) {
                            const gp_Vec u = n.Normalized();
                            t.a = u.Dot(X);
                            t.b = u.Dot(Y);
                        }
                        keep(t, 3);
                    }
                }
            }
            if (tris.empty())
                return {};
            double gu = INFINITY, gv = INFINITY;
            for (const Tri &t : tris) {
                gu = std::min(gu, t.lo[0]);
                gv = std::min(gv, t.lo[1]);
            }
            auto cellKey = [](long long iu, long long iv) { return (iu << 32) ^ (iv & 0xffffffffLL); };
            std::unordered_map<long long, std::vector<int>> grid;
            for (int k = 0; k < int(tris.size()); ++k) {
                const Tri &t = tris[k];
                const long long u0 = (long long)std::floor((t.lo[0] - t.pad - gu) / kCell), u1 = (long long)std::floor((t.hi[0] + t.pad - gu) / kCell);
                const long long v0 = (long long)std::floor((t.lo[1] - t.pad - gv) / kCell), v1 = (long long)std::floor((t.hi[1] + t.pad - gv) / kCell);
                for (long long iu = u0; iu <= u1; ++iu)
                    for (long long iv = v0; iv <= v1; ++iv)
                        grid[cellKey(iu, iv)].push_back(k);
            }
            std::vector<int> seen(tris.size(), -1);
            int query = 0;
            std::vector<int> near;
            auto gather = [&](double ulo, double uhi, double vlo, double vhi) {
                near.clear();
                ++query;
                for (long long iu = (long long)std::floor((ulo - gu) / kCell); iu <= (long long)std::floor((uhi - gu) / kCell); ++iu)
                    for (long long iv = (long long)std::floor((vlo - gv) / kCell); iv <= (long long)std::floor((vhi - gv) / kCell); ++iv) {
                        const auto cell = grid.find(cellKey(iu, iv));
                        if (cell == grid.end())
                            continue;
                        for (const int k : cell->second)
                            if (seen[k] != query) {
                                seen[k] = query;
                                near.push_back(k);
                            }
                    }
            };
            // Whether the strip a step from p to q would move across, toward `side` of it (+1 its left), holds no
            // triangle of any body: short of the step's ends, and of the push's own ends, by 0.001 mm.
            auto stripClear = [&](const Point2 &p, const Point2 &q, double side) {
                const double len = std::hypot(q.x - p.x, q.y - p.y);
                const double tx = (q.x - p.x) / len, ty = (q.y - p.y) / len, nx = -ty, ny = tx;
                const double reach = gap + 2 * kOn;
                gather(std::min(p.x, q.x) - reach, std::max(p.x, q.x) + reach, std::min(p.y, q.y) - reach, std::max(p.y, q.y) + reach);
                const double half[3] = {std::max(0.0, len / 2 - kOn), gap / 2, std::max(0.0, (hhi - hlo) / 2 - kOn)};
                const double centre[3] = {len / 2, kOn + gap / 2, (hlo + hhi) / 2};
                for (const int n : near) {
                    const Tri &t = tris[n];
                    if (t.box) {
                        // its box against the strip's, in the sketch's terms: conservative
                        const double su[2] = {std::min(p.x, q.x) + std::min(0.0, side * gap * nx), std::max(p.x, q.x) + std::max(0.0, side * gap * nx)};
                        const double sv[2] = {std::min(p.y, q.y) + std::min(0.0, side * gap * ny), std::max(p.y, q.y) + std::max(0.0, side * gap * ny)};
                        if (t.lo[0] < su[1] && t.hi[0] > su[0] && t.lo[1] < sv[1] && t.hi[1] > sv[0] && t.lo[2] < hhi - kOn && t.hi[2] > hlo + kOn) {
                            return false;
                        }
                        continue;
                    }
                    double c[3][3];
                    for (int j = 0; j < 3; ++j) {
                        const double du = t.p[j][0] - p.x, dv = t.p[j][1] - p.y;
                        c[j][0] = du * tx + dv * ty;
                        c[j][1] = side * (du * nx + dv * ny);
                        c[j][2] = t.p[j][2];
                    }
                    const double grown[3] = {half[0] + t.pad, half[1] + t.pad, half[2] + t.pad};
                    if (triangleInBox(centre, grown, c)) {
                        return false;
                    }
                }
                return true;
            };
            int steps = 0, onWall = 0, moved = 0, blocked = 0, twoWay = 0, curved = 0, reverted = 0, followed = 0;
            std::string perLoop;        // "loop:moved/on a wall" for each loop with a step on a wall
            for (std::size_t i = 0; i < profile.loops.size(); ++i) {
                std::vector<Point2> &loop = profile.loops[i];
                const std::size_t count = loop.size();
                if (count < 3)
                    continue;
                const bool arcs = i < profile.centres.size() && profile.centres[i].size() == count;
                const bool tagged = i < profile.onCurve.size() && profile.onCurve[i].size() == count && i < profile.along.size()
                                    && profile.along[i].size() == count;
                std::vector<char> fixed(count, 0);
                const int movedBefore = moved, onWallBefore = onWall;
                std::vector<Point2> shift(count, Point2{0, 0});
                bool any = false;
                for (std::size_t k = 0; k < count; ++k) {
                    if ((arcs && !std::isnan(profile.centres[i][k].x)) || (tagged && profile.onCurve[i][k] >= 0)) {
                        fixed[k] = 1;           // an arc or a curve: built exactly from its ends, left as it is
                        ++curved;
                        continue;
                    }
                    const Point2 p = loop[k], q = loop[(k + 1) % count];
                    const double len = std::hypot(q.x - p.x, q.y - p.y);
                    if (len < 1e-12)
                        continue;
                    ++steps;
                    const double tx = (q.x - p.x) / len, ty = (q.y - p.y) / len, nx = -ty, ny = tx;
                    const double reach = gap + 2 * kOn;
                    gather(std::min(p.x, q.x) - reach, std::max(p.x, q.x) + reach, std::min(p.y, q.y) - reach, std::max(p.y, q.y) + reach);
                    // How much of the step has triangles along it facing left (material on its right: cover[0])
                    // or right (cover[1]).
                    double cover[2] = {0, 0};
                    for (const int n : near) {
                        const Tri &t = tris[n];
                        if (t.box || !t.flat)
                            continue;
                        const double across = t.a * nx + t.b * ny;
                        if (std::abs(across) < kAcross)
                            continue;
                        const double sign = across > 0 ? 1.0 : -1.0;
                        int on = 0;
                        double smin = INFINITY, smax = -INFINITY, hmin = INFINITY, hmax = -INFINITY;
                        bool behind = true;     // the whole triangle on its material's side of the line
                        for (int c = 0; c < 3; ++c) {
                            const double du = t.p[c][0] - p.x, dv = t.p[c][1] - p.y;
                            const double off = du * nx + dv * ny;
                            behind = behind && off * sign <= kOn;
                            if (std::abs(off) < kOn) {
                                ++on;
                                const double sAt = du * tx + dv * ty;
                                smin = std::min(smin, sAt);
                                smax = std::max(smax, sAt);
                                hmin = std::min(hmin, t.p[c][2]);
                                hmax = std::max(hmax, t.p[c][2]);
                            }
                        }
                        if (on < 2 || !behind || hmax < hlo - kOn || hmin > hhi + kOn)
                            continue;
                        if (on == 3 && std::min(hmax, hhi) - std::max(hmin, hlo) < 1e-6)
                            continue;           // a wall only at the push's end
                        const double overlap = std::min(len, smax) - std::max(0.0, smin);
                        if (overlap > 0)
                            cover[sign > 0 ? 1 : 0] += overlap;
                    }
                    const double need = std::min(0.5 * len, 0.02);
                    if (cover[0] < need && cover[1] < need)
                        continue;
                    ++onWall;
                    if (cover[0] >= need && cover[1] >= need) {
                        ++twoWay;               // walls facing both ways: two bodies meet along it
                        continue;
                    }
                    const double side = cover[1] >= need ? 1.0 : -1.0;   // toward the empty side
                    const bool clear = stripClear(p, q, side);
                    if (!clear) {
                        ++blocked;
                        continue;
                    }
                    shift[k] = {side * gap * nx, side * gap * ny};
                    any = true;
                    ++moved;
                }
                // A step under 0.005 mm (half the clearance) long, as a corner's two ends projected a hair apart give
                // where a wall leans a few ten-thousandths of a mm, has too little length to show a wall. Next to a
                // moved step it moves the same way, if its own strip is empty too; else the tool would come back onto
                // the body's corner there (A, 2026-11-17: 12 sliver solids of 0 mm3 beside the cut body).
                for (int pass = 0; pass < 2; ++pass)
                    for (std::size_t n = 0; n < count; ++n) {
                        const std::size_t k = pass == 0 ? n : count - 1 - n;
                        if (fixed[k] || shift[k].x != 0 || shift[k].y != 0)
                            continue;
                        const Point2 p = loop[k], q = loop[(k + 1) % count];
                        const double len = std::hypot(q.x - p.x, q.y - p.y);
                        if (len < 1e-12 || len >= gap / 2)
                            continue;
                        const std::size_t beside = pass == 0 ? (k + count - 1) % count : (k + 1) % count;
                        if (fixed[beside] || (shift[beside].x == 0 && shift[beside].y == 0))
                            continue;
                        const double nx = -(q.y - p.y) / len, ny = (q.x - p.x) / len;
                        const double along = shift[beside].x * nx + shift[beside].y * ny;
                        if (std::abs(along) < 1e-12)
                            continue;
                        const double side = along > 0 ? 1.0 : -1.0;
                        if (!stripClear(p, q, side))
                            continue;
                        shift[k] = {side * gap * nx, side * gap * ny};
                        ++followed;
                        any = true;
                    }
                if (onWall > onWallBefore)
                    perLoop += (perLoop.empty() ? "" : " ") + std::to_string(i) + ":" + std::to_string(moved - movedBefore) + "/"
                               + std::to_string(onWall - onWallBefore);
                if (!any)
                    continue;
                // The loop again: each corner where the steps either side meet once moved; where they can't meet
                // near it (parallel, a long spike, or an arc's end that must stay), a short step joins them.
                std::vector<Point2> pts, centres;
                std::vector<int> onCurve;
                std::vector<std::pair<double, double>> along;
                const Point2 straightCentre{std::nan(""), std::nan("")};
                auto emit = [&](const Point2 &at, bool own, std::size_t k) {
                    if (!pts.empty() && std::abs(pts.back().x - at.x) < 1e-12 && std::abs(pts.back().y - at.y) < 1e-12) {
                        if (own) {              // the same point: it takes the step's own kind
                            if (arcs) centres.back() = profile.centres[i][k];
                            if (tagged) { onCurve.back() = profile.onCurve[i][k]; along.back() = profile.along[i][k]; }
                        }
                        return;
                    }
                    pts.push_back(at);
                    if (arcs) centres.push_back(own ? profile.centres[i][k] : straightCentre);
                    if (tagged) {
                        onCurve.push_back(own ? profile.onCurve[i][k] : -1);
                        along.push_back(own ? profile.along[i][k] : std::make_pair(0.0, 0.0));
                    }
                };
                for (std::size_t j = 0; j < count; ++j) {
                    const std::size_t before = (j + count - 1) % count, next = (j + 1) % count;
                    const Point2 P = loop[j], wp = shift[before], wn = shift[j];
                    const bool still = wp.x == 0 && wp.y == 0 && wn.x == 0 && wn.y == 0;
                    if (still) {
                        emit(P, true, j);
                        continue;
                    }
                    if (!fixed[before] && !fixed[j]) {
                        const double ax = P.x - loop[before].x, ay = P.y - loop[before].y;
                        const double bx = loop[next].x - P.x, by = loop[next].y - P.y;
                        const double cross = ax * by - ay * bx, la = std::hypot(ax, ay), lb = std::hypot(bx, by);
                        if (std::abs(cross) > 1e-9 * la * lb) {
                            // (P + wp) + t a = (P + wn) + s b
                            const double rx = wn.x - wp.x, ry = wn.y - wp.y;
                            const double t = (rx * by - ry * bx) / cross;
                            const Point2 meet{P.x + wp.x + t * ax, P.y + wp.y + t * ay};
                            if (std::hypot(meet.x - P.x, meet.y - P.y) <= 5 * gap) {
                                emit(meet, true, j);
                                continue;
                            }
                        } else if (std::abs(wp.x - wn.x) < 1e-12 && std::abs(wp.y - wn.y) < 1e-12) {
                            emit({P.x + wp.x, P.y + wp.y}, true, j);
                            continue;
                        }
                    }
                    emit({P.x + wp.x, P.y + wp.y}, false, j);
                    emit({P.x + wn.x, P.y + wn.y}, true, j);
                }
                if (pts.size() > 1 && std::abs(pts.front().x - pts.back().x) < 1e-12 && std::abs(pts.front().y - pts.back().y) < 1e-12) {
                    pts.pop_back();
                    if (arcs) centres.pop_back();
                    if (tagged) { onCurve.pop_back(); along.pop_back(); }
                }
                // Kept only if the loop still runs the same way round and its area changed by no more than the
                // strips themselves could (a loop that crossed itself would show here); else left exact.
                auto area = [](const std::vector<Point2> &l) {
                    double a = 0;
                    for (std::size_t k = 0; k < l.size(); ++k)
                        a += l[k].x * l[(k + 1) % l.size()].y - l[(k + 1) % l.size()].x * l[k].y;
                    return a / 2;
                };
                double perimeter = 0;
                for (std::size_t k = 0; k < count; ++k)
                    perimeter += std::hypot(loop[(k + 1) % count].x - loop[k].x, loop[(k + 1) % count].y - loop[k].y);
                const double was = area(loop), now = area(pts);
                if (pts.size() < 3 || (was > 0) != (now > 0) || std::abs(now - was) > 1.5 * gap * perimeter + 1e-9) {
                    ++reverted;
                    continue;
                }
                loop = std::move(pts);
                if (arcs) profile.centres[i] = std::move(centres);
                if (tagged) { profile.onCurve[i] = std::move(onCurve); profile.along[i] = std::move(along); }
            }
            const double took = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
            if (!onWall)
                return {};
            char line[700];
            std::snprintf(line, sizeof line,
                          "%d of %d straight step(s) on a wall of the %zu body(ies) cut (%zu triangles, %d bare face(s)); %d moved %.3g mm "
                          "into empty space, %d kept (something within reach beyond it), %d kept (walls facing both ways), %d short step(s) "
                          "(under 0.005 mm) moved with the step beside them, %d loop(s) left exact (would cross itself); %d arc/curve "
                          "step(s) not looked at; by loop (0 outer) moved/on a wall: %.200s; %.2f s",
                          onWall, steps, bodies.size(), tris.size(), bare, moved, gap, blocked, twoWay, followed, reverted, curved,
                          perLoop.c_str(), took);
            engineLog("engine: wall clearance: %s\n", line);
            return line;
        } catch (const Standard_Failure &) {
            return {};
        }
    }

    bool inspect(BodyId body, std::string &report) const override {
        const auto found = m_bodies.find(body);
        if (found == m_bodies.end()) {
            report = "no such body\n";
            return false;
        }
        try {
            // Checked in full already, by the same test, when the Boolean made it: said so, not done twice
            // (doing it again doubled the time of every extrude in the test suites).
            if (m_sound.count(body)) {
                report = "sound: yes (checked in full when it was made)\n";
                return true;
            }
            // Otherwise the test itself, and the full description (slow on a big model) only if it fails.
            if (soundSolid(found->second) && !hasEmptyFace(found->second)) {
                report = "sound: yes\n";
                return true;
            }
            return describe(found->second, report);
        } catch (const Standard_Failure &failure) {
            report += std::string("the check itself failed: ") + failure.GetMessageString() + "\n";
            return false;
        }
    }

    std::string takeTrace() override {
        const std::lock_guard<std::mutex> hold(g_traceLock);
        std::string out;
        out.swap(g_trace);
        return out;
    }

    // ---- Model Chamfer (chamfer.cpp does the work) ----

    // The body's edges as its mesh numbers them (meshed first if it hasn't been, as the numbers need).
    std::vector<ShownEdge> edgesOf(BodyId body) const {
        const TopoDS_Shape &shape = m_bodies.at(body);
        for (TopExp_Explorer e(shape, TopAbs_FACE); e.More(); e.Next()) {
            TopLoc_Location loc;
            if (BRep_Tool::Triangulation(TopoDS::Face(e.Current()), loc).IsNull()) {
                (void)mesh(body);
                break;
            }
        }
        TopTools_IndexedMapOfShape seams;
        return shownEdges(shape, seams);
    }

    // The items for chamfer.cpp: each edge with the face d1 is measured on (per set, the face most of the set's
    // edges share; the other one when flipped). Edges that aren't there are left out (and named in `missing`).
    std::vector<chamfer::Item> chamferItems(BodyId body, const std::vector<ChamferEdge> &edges, std::vector<int> *missing) const {
        const TopoDS_Shape &shape = m_bodies.at(body);
        const std::vector<ShownEdge> shown = edgesOf(body);
        TopTools_IndexedDataMapOfShapeListOfShape edgeFaces;
        TopExp::MapShapesAndAncestors(shape, TopAbs_EDGE, TopAbs_FACE, edgeFaces);
        std::map<int, std::vector<std::pair<TopoDS_Face, int>>> counts;    // per set: faces and how many edges touch each
        const auto facesOf = [&](const TopoDS_Edge &e) {
            std::vector<TopoDS_Face> out;
            if (!edgeFaces.Contains(e))
                return out;
            for (const TopoDS_Shape &f : edgeFaces.FindFromKey(e))
                if (std::none_of(out.begin(), out.end(), [&](const TopoDS_Face &g) { return g.IsSame(f); }))
                    out.push_back(TopoDS::Face(f));
            return out;
        };
        for (const ChamferEdge &c : edges) {
            if (c.edge < 0 || std::size_t(c.edge) >= shown.size())
                continue;
            for (const TopoDS_Face &f : facesOf(shown[std::size_t(c.edge)].edge)) {
                auto &list = counts[c.set];
                auto found = std::find_if(list.begin(), list.end(), [&](const auto &p) { return p.first.IsSame(f); });
                if (found == list.end())
                    list.emplace_back(f, 1);
                else
                    ++found->second;
            }
        }
        std::vector<chamfer::Item> out;
        for (const ChamferEdge &c : edges) {
            if (c.edge < 0 || std::size_t(c.edge) >= shown.size()) {
                if (missing)
                    missing->push_back(c.edge);
                continue;
            }
            const TopoDS_Edge &e = shown[std::size_t(c.edge)].edge;
            const std::vector<TopoDS_Face> faces = facesOf(e);
            if (faces.size() != 2) {
                if (missing)
                    missing->push_back(c.edge);
                continue;
            }
            int best = -1, ref = 0;
            for (int k = 0; k < 2; ++k)
                for (const auto &[f, n] : counts[c.set])
                    if (f.IsSame(faces[std::size_t(k)]) && n > best) {
                        best = n;
                        ref = k;
                    }
            if (c.flip)
                ref = 1 - ref;
            chamfer::Item it;
            it.edge = e;
            it.ref = faces[std::size_t(ref)];
            it.kind = c.kind == 1 ? chamfer::Kind::Two : c.kind == 2 ? chamfer::Kind::Angle : c.kind == 3 ? chamfer::Kind::Fillet
                    : c.kind == 4 ? chamfer::Kind::Chord : chamfer::Kind::Equal;
            it.d1 = c.d1;
            it.d2 = c.kind == 1 ? c.d2 : c.d1;
            it.angle = c.angle * M_PI / 180;
            it.id = c.edge;
            it.sweep = m_sweepEdges;
            out.push_back(it);
        }
        return out;
    }

    // A body's corners as the mesh numbers them (Mesh::points), for points picked to chamfer or round.
    std::vector<TopoDS_Vertex> cornersOf(BodyId body) const {
        const TopoDS_Shape &shape = m_bodies.at(body);
        TopTools_IndexedMapOfShape seams;
        shownEdges(shape, seams);
        return shownCorners(shape, seams);
    }
    std::vector<chamfer::Point> chamferPoints(BodyId body, const std::vector<ChamferEdge> &edges, std::vector<int> *missing) const {
        std::vector<chamfer::Point> out;
        const std::vector<TopoDS_Vertex> corners = cornersOf(body);
        for (const ChamferEdge &c : edges) {
            if (c.point < 0)
                continue;
            if (std::size_t(c.point) >= corners.size()) {
                if (missing)
                    missing->push_back(-1 - c.point);
                continue;
            }
            const gp_Pnt p = BRep_Tool::Pnt(corners[std::size_t(c.point)]);
            chamfer::Point pt;
            pt.at[0] = p.X();
            pt.at[1] = p.Y();
            pt.at[2] = p.Z();
            pt.kind = c.kind == 3 ? chamfer::Kind::Fillet : c.kind == 4 ? chamfer::Kind::Chord : chamfer::Kind::Equal;
            pt.size = c.d1;
            pt.id = -1 - c.point;
            out.push_back(pt);
        }
        return out;
    }

    // Edges and points together (GB-055): when a point's cut won't go cleanly on what OpenCascade's own
    // chamfer of the edges left (loose B-spline faces), the edges are all run through instead (exact turned and
    // swept faces) and the points cut from that; the one with fewer left sharp is kept.
    BodyId chamfer(BodyId body, const std::vector<ChamferEdge> &all, ChamferCorner corner, ChamferReport &report) override {
        const BodyId first = chamferOnce(body, all, corner, report);
        const bool mixed = std::any_of(all.begin(), all.end(), [](const ChamferEdge &c) { return c.point >= 0; })
                        && std::any_of(all.begin(), all.end(), [](const ChamferEdge &c) { return c.point < 0; });
        const bool pointsLeft = std::any_of(report.skipped.begin(), report.skipped.end(), [&](int id) {
            return std::any_of(all.begin(), all.end(), [&](const ChamferEdge &c) { return c.point >= 0 && -1 - c.point == id; });
        });
        if (!mixed || m_sweepEdges || (first && !pointsLeft) || corner == ChamferCorner::Miter)
            return first;
        const std::string whyFirst = m_whyNot;
        ChamferReport again;
        m_sweepEdges = true;
        const BodyId second = chamferOnce(body, all, corner, again);
        m_sweepEdges = false;
        engineLog("engine: edges and points again with every edge run through: %s (%zu left sharp, before %zu)\n",
                  second ? "made" : "nothing made", again.skipped.size(), report.skipped.size());
        if (second && (!first || again.skipped.size() < report.skipped.size())) {
            if (first)
                release(first);
            report = again;
            return second;
        }
        if (second)
            release(second);
        m_whyNot = whyFirst;
        return first;
    }
    BodyId chamferOnce(BodyId body, const std::vector<ChamferEdge> &all, ChamferCorner corner, ChamferReport &report) {
        m_whyNot.clear();
        report = {};
        // Points (GB-053): their cuts worked out on the body as it is, then cut from what the set's edges leave
        // (GB-055: found again on the edges' result, a point the rounds had moved or taken was left sharp and
        // the rest went on quietly; past where an edge's round and a point's meet, they now overlap and take
        // the body down together, as two edges' rounds do).
        std::vector<ChamferEdge> edges, points;
        for (const ChamferEdge &c : all)
            (c.point >= 0 ? points : edges).push_back(c);
        if (!points.empty()) {
            try {
                const bool round = points.front().kind == 3 || points.front().kind == 4;
                std::vector<int> missing;
                const std::vector<chamfer::Point> pts = chamferPoints(body, points, &missing);
                for (const chamfer::Point &p : pts)
                    if (!(p.size > 0)) {
                        m_whyNot = "the size must be above 0";
                        return 0;
                    }
                BodyId from = body;
                ChamferReport edgeReport;
                if (!edges.empty()) {
                    from = chamfer(body, edges, corner, edgeReport);
                    if (!from)
                        return 0;
                }
                chamfer::Report r;
                const auto started = std::chrono::steady_clock::now();
                const TopoDS_Shape made = chamfer::points(usable(from), pts,
                    [](const TopoDS_Shape &s) { return !s.IsNull() && soundSolid(s) && !hasEmptyFace(s); },
                    [this](const TopoDS_Shape &shape, const std::vector<TopoDS_Shape> &tools, bool cut) {
                        const BodyId a = keep(shape);
                        std::vector<BodyId> with;
                        for (const TopoDS_Shape &t : tools)
                            with.push_back(keep(t));
                        m_chamfering = true;
                        const BodyId got = combineAt(a, with, cut ? Combine::Cut : Combine::Join, kTouching);
                        m_chamfering = false;
                        const TopoDS_Shape out = got ? m_bodies.at(got) : TopoDS_Shape();
                        release(a);
                        for (const BodyId b : with)
                            release(b);
                        if (got)
                            release(got);
                        return out;
                    },
                    [](const std::string &line) { engineLog("engine: %s\n", line.c_str()); }, r, usable(body));
                engineLog("engine: %s of %zu point(s): %s, %.2f s\n", round ? "fillet" : "chamfer", pts.size(), made.IsNull() ? "nothing made" : "made",
                          std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count());
                if (from != body)
                    release(from);
                report = edgeReport;
                report.skipped.insert(report.skipped.end(), missing.begin(), missing.end());
                report.skipped.insert(report.skipped.end(), r.skipped.begin(), r.skipped.end());
                report.ranThrough.insert(report.ranThrough.end(), r.ranThrough.begin(), r.ranThrough.end());
                for (const std::string &n : r.notes)
                    report.note += (report.note.empty() ? "" : " ") + n;
                if (made.IsNull()) {
                    m_whyNot = r.consumed ? "that would take the whole body away" : "nothing could be done to the points";
                    return 0;
                }
                if (edges.empty() && r.skipped.size() + missing.size() >= points.size()) {     // nothing done
                    m_whyNot = report.note.empty() ? std::string("no point could be done") : report.note;
                    return 0;
                }
                return keepSound(made);
            } catch (Standard_Failure &e) {
                m_whyNot = e.GetMessageString() ? e.GetMessageString() : "OpenCascade failed";
                return 0;
            }
        }
        try {
            std::vector<int> missing;
            const std::vector<chamfer::Item> items = chamferItems(body, edges, &missing);
            report.skipped = missing;
            const bool round = !edges.empty() && (edges.front().kind == 3 || edges.front().kind == 4);
            if (items.empty()) {
                m_whyNot = round ? "none of those edges is there to fillet" : "none of those edges is there to chamfer";
                return 0;
            }
            for (const chamfer::Item &it : items)
                if (it.d1 <= 0 || it.d2 <= 0 || (it.kind == chamfer::Kind::Angle && (it.angle <= 0 || it.angle >= M_PI / 2))) {
                    m_whyNot = it.kind == chamfer::Kind::Angle ? "the distance must be above 0 and the angle between 0 and 90 degrees"
                                                               : "the distances must be above 0";
                    return 0;
                }
            chamfer::Report r;
            const auto started = std::chrono::steady_clock::now();
            const TopoDS_Shape made = chamfer::make(
                usable(body), items,
                corner == ChamferCorner::Miter ? chamfer::Corner::Miter : corner == ChamferCorner::Blend ? chamfer::Corner::Blend : chamfer::Corner::Chamfer,
                [](const TopoDS_Shape &s) { return !s.IsNull() && soundSolid(s) && !hasEmptyFace(s); },
                [this](const TopoDS_Shape &shape, const std::vector<TopoDS_Shape> &tools, bool cut) {
                    // Through the engine's own checked join or cut (mended, merged, checked), on bodies kept for it.
                    const BodyId a = keep(shape);
                    std::vector<BodyId> with;
                    for (const TopoDS_Shape &t : tools)
                        with.push_back(keep(t));
                    m_chamfering = true;
                    const BodyId made = combineAt(a, with, cut ? Combine::Cut : Combine::Join, kTouching);
                    m_chamfering = false;
                    const TopoDS_Shape out = made ? m_bodies.at(made) : TopoDS_Shape();
                    release(a);
                    for (const BodyId b : with)
                        release(b);
                    if (made)
                        release(made);
                    return out;
                },
                [](const std::string &line) { engineLog("engine: %s\n", line.c_str()); }, r);
            engineLog("engine: chamfer of %zu edge(s): %s, %.2f s\n", items.size(), made.IsNull() ? "nothing made" : "made",
                      std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count());
            report.ranThrough = r.ranThrough;
            report.skipped.insert(report.skipped.end(), r.skipped.begin(), r.skipped.end());
            for (const std::string &n : r.notes)
                report.note += (report.note.empty() ? "" : " ") + n;
            if (made.IsNull()) {
                m_whyNot = r.consumed ? (round ? "the fillets meet and would take the whole body away" : "the chamfers meet and would take the whole body away")
                                      : round ? "no edge could be filleted, not even by running the round through"
                                              : "no edge could be chamfered, not even by running the bevel through";
                return 0;
            }
            return keepSound(made);
        } catch (Standard_Failure &e) {
            m_whyNot = e.GetMessageString() ? e.GetMessageString() : "OpenCascade failed";
            return 0;
        }
    }

    std::vector<int> faceEdges(BodyId body, int faceNumber) const override {
        std::vector<int> out;
        TopTools_IndexedMapOfShape faces;
        TopExp::MapShapes(m_bodies.at(body), TopAbs_FACE, faces);
        if (faceNumber < 0 || faceNumber >= faces.Extent())
            return out;
        TopTools_IndexedMapOfShape own;
        TopExp::MapShapes(faces(faceNumber + 1), TopAbs_EDGE, own);
        const std::vector<ShownEdge> shown = edgesOf(body);
        for (std::size_t i = 0; i < shown.size(); ++i)
            if (own.Contains(shown[i].edge))
                out.push_back(int(i));
        return out;
    }

    std::vector<int> tangentChain(BodyId body, const std::vector<int> &edges) const override {
        const std::vector<ShownEdge> shown = edgesOf(body);
        // Each edge's way out of each of its ends.
        struct End { TopoDS_Vertex v; gp_Vec out; };
        std::vector<std::array<End, 2>> ends(shown.size());
        for (std::size_t i = 0; i < shown.size(); ++i) {
            BRepAdaptor_Curve c(shown[i].edge);
            gp_Pnt p;
            gp_Vec t0, t1;
            c.D1(c.FirstParameter(), p, t0);
            c.D1(c.LastParameter(), p, t1);
            TopoDS_Vertex a, b;
            TopExp::Vertices(shown[i].edge, a, b);
            const bool forward = !a.IsNull() && BRep_Tool::Pnt(a).Distance(c.Value(c.FirstParameter())) < 1e-6;
            const TopoDS_Vertex first = forward ? a : b, last = forward ? b : a;
            ends[i] = {End{first, t0.Magnitude() > 1e-12 ? -t0.Normalized() : t0}, End{last, t1.Magnitude() > 1e-12 ? t1.Normalized() : t1}};
        }
        std::vector<char> in(shown.size(), 0);
        std::vector<int> todo;
        for (const int e : edges)
            if (e >= 0 && std::size_t(e) < shown.size() && !in[std::size_t(e)]) {
                in[std::size_t(e)] = 1;
                todo.push_back(e);
            }
        std::vector<int> out = todo;
        while (!todo.empty()) {
            const int e = todo.back();
            todo.pop_back();
            for (const End &mine : ends[std::size_t(e)]) {
                if (mine.v.IsNull())
                    continue;
                for (std::size_t g = 0; g < shown.size(); ++g) {
                    if (in[g])
                        continue;
                    for (const End &theirs : ends[g])
                        // Smoothly on: leaving the shared corner, the next edge goes on the way this one came in.
                        if (!theirs.v.IsNull() && theirs.v.IsSame(mine.v) && mine.out.Magnitude() > 0 && theirs.out.Magnitude() > 0
                            && mine.out.Dot(theirs.out) < -std::cos(M_PI / 180)) {
                            in[g] = 1;
                            out.push_back(int(g));
                            todo.push_back(int(g));
                            break;
                        }
                }
            }
        }
        std::sort(out.begin(), out.end());
        return out;
    }

    // A point inside a face and its outward normal there.
    static bool faceSample(const TopoDS_Face &face, gp_Pnt &at, gp_Vec &normal) {
        double u0, u1, v0, v1;
        BRepTools::UVBounds(face, u0, u1, v0, v1);
        BRepTopAdaptor_FClass2d inside(face, 1e-7);
        BRepAdaptor_Surface surface(face);
        for (const int n : {1, 3, 7, 15})
            for (int i = 1; i <= n; ++i)
                for (int j = 1; j <= n; ++j) {
                    const double u = u0 + (u1 - u0) * i / (n + 1), v = v0 + (v1 - v0) * j / (n + 1);
                    if (inside.Perform(gp_Pnt2d(u, v)) != TopAbs_IN)
                        continue;
                    gp_Vec du, dv;
                    surface.D1(u, v, at, du, dv);
                    normal = du.Crossed(dv);
                    if (normal.Magnitude() < 1e-12)
                        continue;
                    normal.Normalize();
                    if (face.Orientation() == TopAbs_REVERSED)
                        normal.Reverse();
                    return true;
                }
        return false;
    }

    // Whether a point (with its normal) lies on one of these faces, facing the same way.
    static bool liesOn(const gp_Pnt &p, const gp_Vec &n, const std::vector<TopoDS_Face> &faces) {
        const TopoDS_Vertex v = BRepBuilderAPI_MakeVertex(p);
        for (const TopoDS_Face &f : faces) {
            BRepExtrema_DistShapeShape d(v, f);
            if (!d.IsDone() || d.Value() > 1e-4)
                continue;
            gp_Pnt q;
            gp_Vec m;
            const Handle(Geom_Surface) s = BRep_Tool::Surface(f);
            GeomAPI_ProjectPointOnSurf onto(p, s);
            if (!onto.IsDone() || onto.NbPoints() == 0)
                continue;
            double u, w;
            onto.LowerDistanceParameters(u, w);
            gp_Vec du, dv;
            s->D1(u, w, q, du, dv);
            m = du.Crossed(dv);
            if (m.Magnitude() < 1e-12)
                continue;
            m.Normalize();
            if (f.Orientation() == TopAbs_REVERSED)
                m.Reverse();
            if (m.Dot(n) > 0.9999)
                return true;
        }
        return false;
    }

    std::vector<int> featureEdges(BodyId now, BodyId before, BodyId after) const override {
        std::vector<int> out;
        if (!m_bodies.count(now) || !m_bodies.count(after))
            return out;
        const auto facesOf = [](const TopoDS_Shape &s) {
            std::vector<TopoDS_Face> list;
            for (TopExp_Explorer e(s, TopAbs_FACE); e.More(); e.Next())
                list.push_back(TopoDS::Face(e.Current()));
            return list;
        };
        const std::vector<TopoDS_Face> old = before && m_bodies.count(before) ? facesOf(m_bodies.at(before)) : std::vector<TopoDS_Face>{};
        std::vector<TopoDS_Face> made;
        for (const TopoDS_Face &f : facesOf(m_bodies.at(after))) {
            gp_Pnt p;
            gp_Vec n;
            if (faceSample(f, p, n) && !liesOn(p, n, old))
                made.push_back(f);
        }
        TopTools_IndexedMapOfShape theirs;
        for (const TopoDS_Face &f : facesOf(m_bodies.at(now))) {
            gp_Pnt p;
            gp_Vec n;
            if (faceSample(f, p, n) && liesOn(p, n, made))
                TopExp::MapShapes(f, TopAbs_EDGE, theirs);
        }
        const std::vector<ShownEdge> shown = edgesOf(now);
        for (std::size_t i = 0; i < shown.size(); ++i)
            if (theirs.Contains(shown[i].edge))
                out.push_back(int(i));
        return out;
    }

    std::vector<std::vector<double>> chamferSections(BodyId body, const std::vector<ChamferEdge> &edges) const override {
        std::vector<std::vector<double>> out(edges.size());
        try {
            const TopoDS_Shape &shape = m_bodies.at(body);
            const std::vector<chamfer::Item> items = chamferItems(body, edges, nullptr);
            const std::vector<chamfer::Point> pts = chamferPoints(body, edges, nullptr);
            for (std::size_t i = 0; i < edges.size(); ++i) {
                if (edges[i].point >= 0) {
                    for (const chamfer::Point &p : pts)
                        if (p.id == -1 - edges[i].point) {
                            double s[9];
                            if (chamfer::pointSection(shape, p, s))
                                out[i].assign(s, s + 9);
                            break;
                        }
                    continue;
                }
                for (const chamfer::Item &it : items)
                    if (it.id == edges[i].edge) {
                        double s[9];
                        if (chamfer::section(shape, it, s, edges[i].at))
                            out[i].assign(s, s + 9);
                        break;
                    }
            }
        } catch (Standard_Failure &) {
        }
        return out;
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
            const TopoDS_Shape refined = refine::sameSurfaces(shape);
            if (!mergeable(refined.IsNull() ? shape : refined))
                return keep(refined.IsNull() ? shape : refined);     // OpenCascade's merge would crash on it
            const TopoDS_Shape unified = refine::unifyDrawable(refined.IsNull() ? shape : refined, Precision::Confusion(), Precision::Angular());
            return keep(unified.IsNull() ? (refined.IsNull() ? shape : refined) : unified);
        } catch (const Standard_Failure &failure) {
            why = failure.GetMessageString();
            return 0;
        }
    }

    static meshsolid::Style styleOf(MeshStyle style) {
        return style == MeshStyle::LowPoly ? meshsolid::Style::LowPoly
               : style == MeshStyle::LowRes ? meshsolid::Style::LowRes
                                            : meshsolid::Style::Detailed;
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
            m_imported.emplace(id, std::make_shared<const meshsolid::Mesh>(triangles));
            return id;
        } catch (const Standard_Failure &failure) {
            why = failure.GetMessageString();
            return 0;
        }
    }

    BodyId fitSurfaces(BodyId body, double share, MeshStyle style, FitResult &result, std::string &why, const std::function<void(const char *)> &stage) override {
        try {
            stage("Reading the facets");
            // The file's own triangles if this body is as it came in: the solid's flats have been
            // joined, and meshing them again loses the corners that show where curves are.
            const auto imported = m_imported.find(body);
            const meshsolid::Mesh triangles = imported != m_imported.end() ? *imported->second : meshsolid::fromShape(m_bodies.at(body));
            if (style == MeshStyle::LowPoly) {
                const TopoDS_Shape joined = meshsolid::solid(triangles, stage, meshsolid::Style::LowPoly);
                if (joined.IsNull())
                    why = "Its facets couldn't be joined into a valid solid; nothing was changed.";
                return joined.IsNull() ? 0 : keep(joined);
            }
            meshsolid::FitReport report;
            const TopoDS_Shape shape = meshsolid::fit(triangles, share, stage, report, styleOf(style));
            if (shape.IsNull()) {
                why = "The curved surfaces couldn't be joined into a valid solid; nothing was changed.";
                return 0;
            }
            if (!report.cylinders && !report.cones && !report.spheres && !report.bowls) {
                why = "No facets here follow a cylinder, cone, sphere or smooth cap closely enough.";
                return 0;
            }
            result = {report.cylinders, report.cones, report.spheres, report.bowls, report.facetsLeft, report.worstMm};
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
        std::string bytes = out.str();
        if (const auto imported = m_imported.find(body); imported != m_imported.end())
            Sources::tie(bytes, imported->second);
        return bytes;
    }

    std::function<std::string()> saveBodyLater(BodyId body) const override {
        const auto found = m_bodies.find(body);
        if (found == m_bodies.end())
            return {};
        const TopoDS_Shape shape = found->second;
        std::shared_ptr<const meshsolid::Mesh> imported;
        if (const auto it = m_imported.find(body); it != m_imported.end())
            imported = it->second;
        return [shape, imported] {
            std::ostringstream out(std::ios::binary);
            BinTools::Write(shape, out, false, false, BinTools_FormatVersion_CURRENT);
            std::string bytes = out.str();
            if (imported)
                Sources::tie(bytes, imported);
            return bytes;
        };
    }

    BodyId loadBody(const std::string &bytes) override {
        try {
            std::istringstream in(bytes, std::ios::binary);
            TopoDS_Shape shape;
            BinTools::Read(shape, in);
            if (shape.IsNull())
                return 0;
            const BodyId id = keep(shape);
            if (auto source = Sources::of(bytes))
                m_imported.emplace(id, std::move(source));
            return id;
        } catch (const Standard_Failure &) {
            return 0;
        }
    }

    std::string sourceOf(const std::string &bodyBytes) const override {
        const auto source = Sources::of(bodyBytes);
        return source ? Sources::name(*source) : std::string();
    }

    std::string sourceBytes(const std::string &source) const override {
        const auto mesh = Sources::named(source);
        return mesh ? Sources::write(*mesh) : std::string();
    }

    void addSource(const std::string &bodyBytes, const std::string &source, const std::string &meshBytes) override {
        auto mesh = Sources::named(source);
        if (!mesh)
            mesh = Sources::read(meshBytes);
        if (mesh)
            Sources::tie(bodyBytes, mesh);
    }

    Mesh mesh(BodyId body) const override;

    void release(BodyId body) override {
        m_bodies.erase(body);
        m_usable.erase(body);
        m_sound.erase(body);
        m_imported.erase(body);
    }

private:
    // The body, or its mended copy: checked once, as a body never changes once made.
    const TopoDS_Shape &usable(BodyId body) {
        if (const auto known = m_usable.find(body); known != m_usable.end())
            return known->second;
        const TopoDS_Shape &shape = m_bodies.at(body);
        const auto started = std::chrono::steady_clock::now();
        if (soundSolid(shape)) {
            m_sound.insert(body);
            const double took = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
            if (took > 0.05)
                engineLog("engine: body %llu checked sound before use, %.2f s\n", (unsigned long long)body, took);
            return m_usable.emplace(body, shape).first->second;
        }
        std::string how;
        const TopoDS_Shape fixed = mended(shape, how);
        const double took = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
        if (!fixed.IsNull())
            engineLog("engine: body %llu put right before use: %s, %.2f s\n", (unsigned long long)body, how.c_str(), took);
        else if (took > 0.05)
            engineLog("engine: body %llu checked before use, %.2f s\n", (unsigned long long)body, took);
        return m_usable.emplace(body, fixed.IsNull() ? shape : fixed).first->second;
    }

    // A result already checked sound in full (combine): no need to check it again when it's next used.
    BodyId keepSound(const TopoDS_Shape &shape) {
        const BodyId id = keep(shape);
        m_usable.emplace(id, shape);
        m_sound.insert(id);
        return id;
    }

    BodyId keep(const TopoDS_Shape &shape) {
        const BodyId id = ++m_lastId;
        m_bodies.emplace(id, shape);
        return id;
    }

    std::unordered_map<BodyId, TopoDS_Shape> m_bodies;
    std::unordered_map<BodyId, TopoDS_Shape> m_usable;    // each body as Booleans use it (usable)
    std::unordered_set<BodyId> m_sound;                    // bodies already checked sound in full (soundSolid, no face of no area)
    bool m_chamfering = false;   // combineAt is cutting for a chamfer (see mergeable)
    bool m_sweepEdges = false;   // GB-055: the set's edges all run through (see chamfer)
    std::string m_whyNot; // why the last extrude or combine that gave 0 couldn't be done, for the person
    std::unordered_map<BodyId, std::shared_ptr<const meshsolid::Mesh>> m_imported;    // the triangles an imported mesh came in as

    // Saved bytes -> the mesh their body came in as, for every engine in the program (the history's
    // background rebuild has one of its own). One copy of each mesh, however many saved bytes
    // stand for its body; a key is the bytes' length and hash, a name the mesh's own.
    struct Sources {
        static std::mutex &lock() { static std::mutex m; return m; }
        static std::unordered_map<std::string, std::shared_ptr<const meshsolid::Mesh>> &byBytes() {
            static std::unordered_map<std::string, std::shared_ptr<const meshsolid::Mesh>> m;
            return m;
        }
        static std::unordered_map<std::string, std::weak_ptr<const meshsolid::Mesh>> &byName() {
            static std::unordered_map<std::string, std::weak_ptr<const meshsolid::Mesh>> m;
            return m;
        }
        static std::string key(const std::string &bytes) {
            return std::to_string(bytes.size()) + ":" + std::to_string(std::hash<std::string>()(bytes));
        }
        static std::string name(const meshsolid::Mesh &mesh) {
            const std::string_view points(reinterpret_cast<const char *>(mesh.points.data()), mesh.points.size() * sizeof mesh.points[0]);
            const std::string_view triangles(reinterpret_cast<const char *>(mesh.triangles.data()), mesh.triangles.size() * sizeof mesh.triangles[0]);
            return std::to_string(mesh.points.size()) + "-" + std::to_string(mesh.triangles.size()) + "-"
                   + std::to_string(std::hash<std::string_view>()(points) ^ (std::hash<std::string_view>()(triangles) * 31));
        }
        static void tie(const std::string &bytes, const std::shared_ptr<const meshsolid::Mesh> &mesh) {
            const std::lock_guard<std::mutex> hold(lock());
            byBytes()[key(bytes)] = mesh;
            byName()[name(*mesh)] = mesh;
        }
        static std::shared_ptr<const meshsolid::Mesh> of(const std::string &bytes) {
            const std::lock_guard<std::mutex> hold(lock());
            const auto found = byBytes().find(key(bytes));
            return found == byBytes().end() ? nullptr : found->second;
        }
        static std::shared_ptr<const meshsolid::Mesh> named(const std::string &source) {
            const std::lock_guard<std::mutex> hold(lock());
            const auto found = byName().find(source);
            return found == byName().end() ? nullptr : found->second.lock();
        }
        // "OMAMESH1", the counts, then points (float if every one is exactly a float, as an STL's
        // are, else double) and triangles.
        static std::string write(const meshsolid::Mesh &mesh) {
            bool floats = true;
            for (const auto &p : mesh.points)
                for (const double c : p)
                    floats = floats && double(float(c)) == c;
            std::string out = "OMAMESH1";
            const auto put = [&](const void *data, std::size_t size) { out.append(static_cast<const char *>(data), size); };
            const std::uint64_t counts[3] = {mesh.points.size(), mesh.triangles.size(), floats ? 4u : 8u};
            put(counts, sizeof counts);
            for (const auto &p : mesh.points)
                for (const double c : p) {
                    if (floats) {
                        const float f = float(c);
                        put(&f, sizeof f);
                    } else {
                        put(&c, sizeof c);
                    }
                }
            for (const auto &t : mesh.triangles)
                for (const int i : t) {
                    const std::int32_t v = i;
                    put(&v, sizeof v);
                }
            return out;
        }
        static std::shared_ptr<const meshsolid::Mesh> read(const std::string &bytes) {
            std::uint64_t counts[3];
            if (bytes.size() < 8 + sizeof counts || bytes.compare(0, 8, "OMAMESH1") != 0)
                return nullptr;
            std::memcpy(counts, bytes.data() + 8, sizeof counts);
            const std::uint64_t width = counts[2];
            if ((width != 4 && width != 8) || counts[0] > bytes.size() || counts[1] > bytes.size()
                || bytes.size() != 8 + sizeof counts + counts[0] * 3 * width + counts[1] * 3 * 4)
                return nullptr;
            auto mesh = std::make_shared<meshsolid::Mesh>();
            mesh->points.resize(counts[0]);
            mesh->triangles.resize(counts[1]);
            const char *at = bytes.data() + 8 + sizeof counts;
            for (auto &p : mesh->points)
                for (double &c : p) {
                    if (width == 4) {
                        float f;
                        std::memcpy(&f, at, 4);
                        c = f;
                    } else {
                        std::memcpy(&c, at, 8);
                    }
                    at += width;
                }
            for (auto &t : mesh->triangles)
                for (int &i : t) {
                    std::int32_t v;
                    std::memcpy(&v, at, 4);
                    at += 4;
                    if (v < 0 || std::uint64_t(v) >= counts[0])
                        return nullptr;
                    i = v;
                }
            return mesh;
        }
    };
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
    // Delabella quietly leaves some faces bare (walls swept from an ellipse arc, as a pushed face of
    // a converted body has): those get the standard mesher, or they'd be missing from the screen.
    // A wall pushed straight from a curve (every spline or ellipse side of an extrude) is one of them:
    // it is straight from end to end, so triangles between its edges' points are already as close to it
    // as those points are to the curve. Checking inside it as well filled a 40 ft spline gear's 44
    // walls with half a million triangles and took 7 s (0.05 s and 2,480 triangles without). So such a
    // wall is meshed from its edges' points alone, as OpenCascade meshes a plane: even unchecked, its
    // own mesher first estimated the surface at every knot of the curve, 9.5 s for one wall of a
    // 4,096-pole projected curve (2026-11-12; 0.00 s this way, every triangle within 0.002 mm of it).
    struct Walls : IMeshTools_MeshAlgoFactory {
        Handle(IMeshTools_MeshAlgoFactory) usual = new BRepMesh_MeshAlgoFactory;
        Handle(IMeshTools_MeshAlgo) GetAlgo(const GeomAbs_SurfaceType type, const IMeshTools_Parameters &p) const override {
            return type == GeomAbs_SurfaceOfExtrusion ? Handle(IMeshTools_MeshAlgo)(new BRepMesh_DelaunayBaseMeshAlgo) : usual->GetAlgo(type, p);
        }
    };
    mesher.MeshAlgo = IMeshTools_MeshAlgoType_Watson;
    for (TopExp_Explorer e(shape, TopAbs_FACE); e.More(); e.Next()) {
        const TopoDS_Face &face = TopoDS::Face(e.Current());
        TopLoc_Location loc;
        if (!BRep_Tool::Triangulation(face, loc).IsNull())
            continue;
        IMeshTools_Parameters own = mesher;
        own.ControlSurfaceDeflection = BRepAdaptor_Surface(face).GetType() != GeomAbs_SurfaceOfExtrusion;
        Handle(BRepMesh_Context) context = new BRepMesh_Context(own.MeshAlgo);
        context->SetFaceDiscret(new BRepMesh_FaceDiscret(new Walls));
        BRepMesh_IncrementalMesh one;
        one.SetShape(face);
        one.ChangeParameters() = own;
        one.Perform(context);
    }

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

    TopTools_IndexedMapOfShape seams;
    for (const ShownEdge &shown : shownEdges(shape, seams)) {
        TopLoc_Location loc;
        const Handle(Poly_Triangulation) tri = BRep_Tool::Triangulation(shown.face, loc);
        const Handle(Poly_PolygonOnTriangulation) poly = BRep_Tool::PolygonOnTriangulation(shown.edge, tri, loc);
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
    for (const TopoDS_Vertex &corner : shownCorners(shape, seams)) {
        const gp_Pnt p = BRep_Tool::Pnt(corner);
        out.points.insert(out.points.end(), {float(p.X()), float(p.Y()), float(p.Z())});
    }
    return out;
}

} // namespace

std::unique_ptr<GeometryEngine> makeHeavyEngine() {
    return std::make_unique<HeavyEngine>();
}

} // namespace omamake
