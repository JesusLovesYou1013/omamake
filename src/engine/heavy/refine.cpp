#include "engine/heavy/refine.h"

#include <BRepAdaptor_Curve.hxx>
#include <BRepAdaptor_Surface.hxx>
#include <BRepBndLib.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <BRepGProp.hxx>
#include <BRepLProp_SLProps.hxx>
#include <BRepLib_FuseEdges.hxx>
#include <BRepTools.hxx>
#include <BRepTopAdaptor_FClass2d.hxx>
#include <BRep_Builder.hxx>
#include <BRep_Tool.hxx>
#include <Bnd_Box.hxx>
#include <GProp_GProps.hxx>
#include <Geom_Conic.hxx>
#include <Geom_ConicalSurface.hxx>
#include <Geom_CylindricalSurface.hxx>
#include <Geom_Plane.hxx>
#include <Precision.hxx>
#include <ShapeBuild_ReShape.hxx>
#include <ShapeExtend.hxx>
#include <ShapeFix_Face.hxx>
#include <Standard_Failure.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopTools_DataMapOfShapeShape.hxx>
#include <TopTools_IndexedDataMapOfShapeListOfShape.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Edge.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Wire.hxx>

#include <algorithm>
#include <cmath>
#include <list>
#include <map>
#include <numeric>
#include <vector>

namespace refine {
namespace {

// Exactly one surface: FreeCAD's FaceTypedPlane/Cylinder::isEqual (modelRefine.cpp:354, 457), and a
// cone the same way (one apex, one axis line, one half-angle, either way along it).
// Only these three kinds: the faces are rebuilt from their outline below, which is right for them alone.
// GB-063 (owner, pt169u: a chamfer run into the hole fillets "inverted them to be internal fillets instead of
// external", the chamfer "seem[ing] to glitch through them"): two pieces of one hole's round (a torus, the
// very same surface object, so the old `sa == sb` shortcut took them for one) were rebuilt as a single face,
// and on a torus, closed both ways round, the outline bounded the rest of the ring instead: 1211.79 mm2 of
// it in place of the 209.44 left, the body's volume 12791 mm3 off, and OpenCascade's check still passed.
// Spheres, tori and free-form faces are left to OpenCascade's merge (ShapeUpgrade_UnifySameDomain), after.
bool sameSurface(const TopoDS_Face &a, const TopoDS_Face &b) {
    const Handle(Geom_Surface) sa = BRep_Tool::Surface(a), sb = BRep_Tool::Surface(b);
    const auto kind = [](const Handle(Geom_Surface) &s) {
        return !Handle(Geom_Plane)::DownCast(s).IsNull() || !Handle(Geom_CylindricalSurface)::DownCast(s).IsNull()
               || !Handle(Geom_ConicalSurface)::DownCast(s).IsNull();
    };
    if (!kind(sa) || !kind(sb))
        return false;
    if (sa == sb)
        return true;
    const auto pa = Handle(Geom_Plane)::DownCast(sa), pb = Handle(Geom_Plane)::DownCast(sb);
    if (pa && pb)
        return pa->Pln().Axis().Direction().IsParallel(pb->Pln().Axis().Direction(), Precision::Angular())
               && pa->Pln().Distance(pb->Pln().Location()) < Precision::Confusion();
    const auto ca = Handle(Geom_CylindricalSurface)::DownCast(sa), cb = Handle(Geom_CylindricalSurface)::DownCast(sb);
    if (ca && cb) {
        const gp_Cylinder x = ca->Cylinder(), y = cb->Cylinder();
        return std::abs(x.Radius() - y.Radius()) < Precision::Confusion()
               && (x.Axis().IsCoaxial(y.Axis(), Precision::Angular(), Precision::Confusion())
                   || x.Axis().IsCoaxial(y.Axis().Reversed(), Precision::Angular(), Precision::Confusion()));
    }
    const auto ka = Handle(Geom_ConicalSurface)::DownCast(sa), kb = Handle(Geom_ConicalSurface)::DownCast(sb);
    if (ka && kb) {
        const gp_Cone x = ka->Cone(), y = kb->Cone();
        const bool along = x.Axis().Direction().IsParallel(y.Axis().Direction(), Precision::Angular());
        const bool against = x.Axis().Direction().IsOpposite(y.Axis().Direction(), Precision::Angular());
        return x.Apex().Distance(y.Apex()) < Precision::Confusion()
               && ((along && std::abs(x.SemiAngle() - y.SemiAngle()) < Precision::Angular())
                   || (against && std::abs(x.SemiAngle() + y.SemiAngle()) < Precision::Angular()));
    }
    return false;
}

// The outward normal somewhere inside a face.
bool outward(const TopoDS_Face &f, gp_Dir &n) {
    double u0, u1, v0, v1;
    BRepTools::UVBounds(f, u0, u1, v0, v1);
    BRepTopAdaptor_FClass2d inside(f, 1e-9);
    BRepAdaptor_Surface s(f);
    for (int i = 1; i < 12; ++i)
        for (int j = 1; j < 12; ++j) {
            const gp_Pnt2d uv(u0 + (u1 - u0) * i / 12, v0 + (v1 - v0) * j / 12);
            if (inside.Perform(uv) != TopAbs_IN)
                continue;
            BRepLProp_SLProps p(s, uv.X(), uv.Y(), 1, 1e-9);
            if (!p.IsNormalDefined())
                continue;
            n = p.Normal();
            if (f.Orientation() == TopAbs_REVERSED)
                n.Reverse();
            return true;
        }
    return false;
}

// Does a wire go once round the axis (the rim of a whole ring)? FreeCAD's wireEncirclesAxis
// (modelRefine.cpp:515), by the angle swept round the axis instead of arc lengths.
bool goesRound(const TopoDS_Wire &w, const gp_Ax1 &axis) {
    const gp_Ax3 frame(axis.Location(), axis.Direction());
    double swept = 0, last = 0;
    bool first = true;
    for (TopExp_Explorer e(w, TopAbs_EDGE); e.More(); e.Next()) {
        const TopoDS_Edge edge = TopoDS::Edge(e.Current());
        BRepAdaptor_Curve c(edge);
        const bool reversed = edge.Orientation() == TopAbs_REVERSED;
        constexpr int kSteps = 16;
        for (int k = 0; k <= kSteps; ++k) {
            const double span = c.LastParameter() - c.FirstParameter();
            const double t = reversed ? c.LastParameter() - span * k / kSteps : c.FirstParameter() + span * k / kSteps;
            const gp_Vec d(frame.Location(), c.Value(t));
            const double a = std::atan2(d.Dot(gp_Vec(frame.YDirection())), d.Dot(gp_Vec(frame.XDirection())));
            if (!first) {
                double step = a - last;
                while (step > M_PI)
                    step -= 2 * M_PI;
                while (step < -M_PI)
                    step += 2 * M_PI;
                swept += step;
            }
            first = false;
            last = a;
        }
    }
    return std::abs(swept) > M_PI;
}

double areaOf(const TopoDS_Shape &s) {
    GProp_GProps p;
    BRepGProp::SurfaceProperties(s, p);
    return p.Mass();
}

// One face for a group on `ref`'s surface, or null. `context` gathers every edge the fix swaps, so
// the neighbours get the very same new edge (FreeCAD sews afterwards for the same reason).
TopoDS_Face unite(const std::vector<TopoDS_Face> &group, const TopoDS_Face &ref, const Handle(ShapeBuild_ReShape) &context) {
    // The group's outline: every edge one of its faces has once (FreeCAD's boundaryEdges,
    // modelRefine.cpp:88); an edge between two of them, and a seam inside one, cancel out.
    std::list<TopoDS_Edge> edges;
    for (const TopoDS_Face &f : group)
        for (TopExp_Explorer e(f, TopAbs_EDGE); e.More(); e.Next()) {
            const TopoDS_Edge edge = TopoDS::Edge(e.Current());
            const auto it = std::find_if(edges.begin(), edges.end(), [&](const TopoDS_Edge &x) { return x.IsSame(edge); });
            if (it != edges.end())
                edges.erase(it);
            else
                edges.push_back(edge);
        }
    // Chained into closed wires, end to start (FreeCAD's boundarySplit, modelRefine.cpp:265). Put
    // together as they are: BRepLib_MakeWire copies an edge whose ends it thinks merely close, and
    // the copy is no longer the one the neighbour has (the speaker mount's 0.02 mm edges).
    std::vector<TopoDS_Wire> wires;
    BRep_Builder build;
    while (!edges.empty()) {
        TopoDS_Wire wire;
        build.MakeWire(wire);
        const TopoDS_Edge head = edges.front();
        edges.pop_front();
        build.Add(wire, head);
        const TopoDS_Vertex start = TopExp::FirstVertex(head, true);
        TopoDS_Vertex end = TopExp::LastVertex(head, true);
        while (!end.IsSame(start)) {
            const auto it = std::find_if(edges.begin(), edges.end(), [&](const TopoDS_Edge &x) { return TopExp::FirstVertex(x, true).IsSame(end); });
            if (it == edges.end())
                return {};
            build.Add(wire, *it);
            end = TopExp::LastVertex(*it, true);
            edges.erase(it);
        }
        wire.Closed(true);
        wires.push_back(wire);
    }
    if (wires.empty())
        return {};
    // Biggest first: the outer boundary (FreeCAD's WireSort).
    std::sort(wires.begin(), wires.end(), [](const TopoDS_Wire &a, const TopoDS_Wire &b) {
        Bnd_Box x, y;
        BRepBndLib::Add(a, x);
        BRepBndLib::Add(b, y);
        return y.SquareExtent() < x.SquareExtent();
    });
    const Handle(Geom_Surface) surface = BRep_Tool::Surface(ref);
    const auto cylinder = Handle(Geom_CylindricalSurface)::DownCast(surface);
    const auto cone = Handle(Geom_ConicalSurface)::DownCast(surface);
    std::vector<TopoDS_Wire> round, inner;
    for (const TopoDS_Wire &w : wires)
        ((cylinder || cone) && goesRound(w, cylinder ? cylinder->Axis() : cone->Axis()) ? round : inner).push_back(w);
    // A whole ring has its two rims and no outer boundary: holes go on first, as those across the
    // seam fail after the rims (FreeCAD's buildFace, modelRefine.cpp:649).
    if (!round.empty() && round.size() != 2)
        return {};
    std::vector<TopoDS_Wire> order = round.empty() ? wires : inner;
    order.insert(order.end(), round.begin(), round.end());
    BRepBuilderAPI_MakeFace make(surface, order[0], round.empty());
    for (std::size_t i = 1; make.IsDone() && i < order.size(); ++i)
        make.Add(order[i]);
    if (!make.IsDone())
        return {};
    // FreeCAD's fixFace: the missing seam, then the wires' orientation.
    ShapeFix_Face fix(make.Face());
    fix.SetContext(context);
    fix.Perform();
    if (fix.Status(ShapeExtend_FAIL))
        return {};
    fix.FixMissingSeam();
    fix.Perform();
    if (fix.Status(ShapeExtend_FAIL))
        return {};
    fix.FixOrientation();
    fix.Perform();
    if (fix.Status(ShapeExtend_FAIL))
        return {};
    TopoDS_Face face = fix.Face();
    // Facing out the way the faces it replaces did.
    gp_Dir want, got;
    if (!outward(ref, want) || !outward(face, got))
        return {};
    if (want.Dot(got) < 0)
        face.Reverse();
    return face;
}

} // namespace

TopoDS_Shape sameSurfaces(const TopoDS_Shape &shape, int *united) {
    if (united)
        *united = 0;
    try {
        TopTools_IndexedMapOfShape faces;
        TopExp::MapShapes(shape, TopAbs_FACE, faces);
        TopTools_IndexedDataMapOfShapeListOfShape edgeFaces;
        TopExp::MapShapesAndAncestors(shape, TopAbs_EDGE, TopAbs_FACE, edgeFaces);
        // Groups: faces joined across an edge they share with a face of the same surface.
        std::vector<int> parent(std::size_t(faces.Extent()) + 1);
        std::iota(parent.begin(), parent.end(), 0);
        const auto root = [&](int i) {
            while (parent[std::size_t(i)] != i)
                i = parent[std::size_t(i)] = parent[std::size_t(parent[std::size_t(i)])];
            return i;
        };
        for (int i = 1; i <= edgeFaces.Extent(); ++i) {
            const TopTools_ListOfShape &list = edgeFaces(i);
            if (list.Extent() != 2)
                continue;
            const TopoDS_Face a = TopoDS::Face(list.First()), b = TopoDS::Face(list.Last());
            if (!a.IsSame(b) && sameSurface(a, b))
                parent[std::size_t(root(faces.FindIndex(a)))] = root(faces.FindIndex(b));
        }
        std::map<int, std::vector<TopoDS_Face>> groups;
        for (int i = 1; i <= faces.Extent(); ++i)
            groups[root(i)].push_back(TopoDS::Face(faces(i)));
        const Handle(ShapeBuild_ReShape) reshape = new ShapeBuild_ReShape();
        int joined = 0;
        for (const auto &[key, group] : groups) {
            if (group.size() < 2)
                continue;
            // The biggest face's surface: the one the shape was built around, rather than a short
            // piece added beside it.
            const TopoDS_Face ref = *std::max_element(group.begin(), group.end(), [](const TopoDS_Face &a, const TopoDS_Face &b) { return areaOf(a) < areaOf(b); });
            const TopoDS_Face one = unite(group, ref, reshape);
            if (one.IsNull() || !BRepCheck_Analyzer(one).IsValid())
                continue;
            // GB-063: the face made must cover what the faces it replaces did, no more and no less (an
            // outline taken the wrong way round bounds the rest of the surface: valid, and wrong).
            double was = 0;
            for (const TopoDS_Face &f : group)
                was += areaOf(f);
            if (std::abs(areaOf(one) - was) > 1e-6 * std::max(was, 1.0))
                continue;
            reshape->Replace(group[0], one);
            for (std::size_t k = 1; k < group.size(); ++k)
                reshape->Remove(group[k]);
            ++joined;
        }
        if (joined == 0)
            return shape;
        TopoDS_Shape result = reshape->Apply(shape);
        if (!BRepCheck_Analyzer(result).IsValid())
            return {};
        // Straight edges cut in two at a corner that's gone (FreeCAD's process, modelRefine.cpp:1326:
        // BRepLib_FuseEdges, circles and seams left alone). Asking for the faces it touches first
        // builds its chains; Perform() alone found none.
        TopTools_IndexedMapOfShape avoid;
        for (TopExp_Explorer f(result, TopAbs_FACE); f.More(); f.Next())
            for (TopExp_Explorer e(f.Current(), TopAbs_EDGE); e.More(); e.Next()) {
                const TopoDS_Edge edge = TopoDS::Edge(e.Current());
                double a, b;
                const Handle(Geom_Curve) c = BRep_Tool::Curve(edge, a, b);
                if (BRep_Tool::IsClosed(edge, TopoDS::Face(f.Current())) || (!c.IsNull() && c->IsKind(STANDARD_TYPE(Geom_Conic))))
                    avoid.Add(edge);
            }
        BRepLib_FuseEdges fuse(result, Standard_True);
        fuse.AvoidEdges(avoid);
        TopTools_DataMapOfShapeShape touched;
        fuse.Faces(touched);
        if (!touched.IsEmpty()) {
            const TopoDS_Shape fused = fuse.Shape();
            if (BRepCheck_Analyzer(fused).IsValid())
                result = fused;
        }
        if (united)
            *united = joined;
        return result;
    } catch (const Standard_Failure &) {
        return {};
    }
}

} // namespace refine
