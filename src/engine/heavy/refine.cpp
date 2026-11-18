#include "engine/heavy/refine.h"

#include <BRepAdaptor_Curve.hxx>
#include <BRepAdaptor_Surface.hxx>
#include <BRepBndLib.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <TopTools_MapOfShape.hxx>
#include <ShapeUpgrade_UnifySameDomain.hxx>
#include <TopTools_HSequenceOfShape.hxx>
#include <ShapeAnalysis_FreeBounds.hxx>
#include <Poly_Triangulation.hxx>
#include <BRepMesh_IncrementalMesh.hxx>
#include <BRepBuilderAPI_Copy.hxx>
#include <cstdio>
#include <cstdlib>
#include <Geom_Line.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <BRepGProp.hxx>
#include <BRepLProp_SLProps.hxx>
#include <BRepLib.hxx>
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
#include <ShapeFix_Shell.hxx>
#include <Standard_Failure.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopTools_DataMapOfShapeShape.hxx>
#include <TopTools_IndexedDataMapOfShapeListOfShape.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Edge.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Shell.hxx>
#include <TopoDS_Solid.hxx>
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

// A joined face can come out facing the wrong way for its shell (BRepCheck's
// BadOrientationOfSubshape, though it faces out as the faces it replaced did): turned round
// together with its neighbours, as the converter does after its own merge (meshsolid.cpp).
TopoDS_Shape turnedRight(const TopoDS_Shape &shape) {
    if (shape.ShapeType() != TopAbs_SOLID)
        return shape;
    TopExp_Explorer one(shape, TopAbs_SHELL);
    if (!one.More())
        return shape;
    one.Next();
    if (one.More())
        return shape; // More than one shell: leave it.
    ShapeFix_Shell faces(TopoDS::Shell(TopExp_Explorer(shape, TopAbs_SHELL).Current()));
    faces.Perform();
    TopoDS_Solid turned;
    BRep_Builder().MakeSolid(turned);
    BRep_Builder().Add(turned, faces.Shell());
    if (!BRepLib::OrientClosedSolid(turned))
        return shape;
    return turned;
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
    // A copy of the surface: an edge's curve on a face is kept per surface, so the fix below working
    // on the very surface the old faces stand on rewrote their rims' curves in place, and a ring it
    // then gave up on was left broken (hinge_female's countersinks, round 6).
    const Handle(Geom_Surface) surface = Handle(Geom_Surface)::DownCast(BRep_Tool::Surface(ref)->Copy());
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
    // A whole ring's new wrap-around line must be the straight line along the surface. The fix can
    // trace it as a spline instead, and split a rim beside it into a piece of no length; OpenCascade's
    // Boolean then can't split the face, and a cut through hinge_female's countersinks came back
    // empty (round 6). Such a ring is left for OpenCascade's own merge, which keeps the line it had.
    if (round.size() == 2)
        for (TopExp_Explorer e(face, TopAbs_EDGE); e.More(); e.Next()) {
            const TopoDS_Edge edge = TopoDS::Edge(e.Current());
            if (!BRep_Tool::IsClosed(edge, face))
                continue;
            double a, b;
            const Handle(Geom_Curve) curve = BRep_Tool::Curve(edge, a, b);
            if (curve.IsNull() || !curve->IsKind(STANDARD_TYPE(Geom_Line)))
                return {};
        }
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
        if (!BRepCheck_Analyzer(result).IsValid()) {
            // Turned round only if it still holds what it held: a whole shell can come out inside
            // out and valid (the shelf bracket: -200% volume).
            const TopoDS_Shape turned = turnedRight(result);
            GProp_GProps was, now;
            BRepGProp::VolumeProperties(shape, was);
            BRepGProp::VolumeProperties(turned, now);
            if (std::abs(now.Mass() - was.Mass()) <= 1e-5 * std::max(std::abs(was.Mass()), 1.0))
                result = turned;
        }
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
        // Joined faces must hold what the faces they replace held. A shell joined valid but inside
        // out (the converted shelf bracket, -200% volume after the merge) is turned back; anything
        // else that changed what it holds isn't the same solid.
        if (shape.ShapeType() == TopAbs_SOLID) {
            GProp_GProps was, now;
            BRepGProp::VolumeProperties(shape, was);
            BRepGProp::VolumeProperties(result, now);
            const double close = 1e-5 * std::max(std::abs(was.Mass()), 1.0);
            if (std::abs(now.Mass() + was.Mass()) <= close && std::abs(was.Mass()) > close) {
                // Its faces turned round as a whole solid would be: a solid merely flagged reversed
                // isn't valid, and OpenCascade's merge after this one then refused it (hinge_female's
                // countersinks stayed in halves, round 6).
                const TopoDS_Shape turned = turnedRight(result);
                GProp_GProps back;
                BRepGProp::VolumeProperties(turned, back);
                if (std::abs(back.Mass() - was.Mass()) > close || !BRepCheck_Analyzer(turned).IsValid())
                    return {};
                result = turned;
            } else if (std::abs(now.Mass() - was.Mass()) > close)
                return {};
        }
        if (united)
            *united = joined;
        return result;
    } catch (const Standard_Failure &) {
        return {};
    }
}

bool roundsDraw(const TopoDS_Shape &shape) {
    try {
        for (TopExp_Explorer e(shape, TopAbs_FACE); e.More(); e.Next()) {
            const TopoDS_Face face = TopoDS::Face(e.Current());
            const BRepAdaptor_Surface surface(face, false);
            if (surface.GetType() != GeomAbs_Cylinder && surface.GetType() != GeomAbs_Cone)
                continue;
            bool wraps = false;
            for (TopExp_Explorer x(face, TopAbs_EDGE); x.More() && !wraps; x.Next())
                wraps = BRep_Tool::IsClosed(TopoDS::Edge(x.Current()), face);
            if (!wraps)
                continue;
            const TopoDS_Face alone = TopoDS::Face(BRepBuilderAPI_Copy(face, true, false).Shape());
            Bnd_Box box;
            BRepBndLib::Add(alone, box);
            BRepMesh_IncrementalMesh(alone, std::max(1e-3, std::sqrt(box.SquareExtent()) * 0.01), false, 0.5, false);
            TopLoc_Location where;
            const Handle(Poly_Triangulation) drawn = BRep_Tool::Triangulation(alone, where);
            if (drawn.IsNull() || drawn->NbTriangles() == 0)
                return false;
        }
        return true;
    } catch (const Standard_Failure &) {
        return false;
    }
}

// One wrapped round face drawn on its own; the triangles, or -1.
static int drawnTriangles(const TopoDS_Face &face) {
    const TopoDS_Face alone = TopoDS::Face(BRepBuilderAPI_Copy(face, true, false).Shape());
    Bnd_Box box;
    BRepBndLib::Add(alone, box);
    BRepMesh_IncrementalMesh(alone, std::max(1e-3, std::sqrt(box.SquareExtent()) * 0.01), false, 0.5, false);
    TopLoc_Location where;
    const Handle(Poly_Triangulation) drawn = BRep_Tool::Triangulation(alone, where);
    return drawn.IsNull() ? -1 : drawn->NbTriangles();
}

// Every round face that can't be drawn, built again from its rims (its edges other than the
// wrap-around one) on the same surface, the wrap-around line added by ShapeFix_Face as a made
// cylinder or cone has it. The edges it re-makes are swapped in its neighbours too. Null unless
// every such face then draws, keeps its area, and the body stays valid with its volume.
static TopoDS_Shape reseamed(const TopoDS_Shape &shape) {
    Handle(ShapeBuild_ReShape) context = new ShapeBuild_ReShape;
    BRep_Builder builder;
    bool any = false;
    for (TopExp_Explorer e(shape, TopAbs_FACE); e.More(); e.Next()) {
        const TopoDS_Face face = TopoDS::Face(e.Current());
        const GeomAbs_SurfaceType kind = BRepAdaptor_Surface(face, false).GetType();
        if ((kind != GeomAbs_Cylinder && kind != GeomAbs_Cone) || drawnTriangles(face) > 0)
            continue;
        Handle(TopTools_HSequenceOfShape) rims = new TopTools_HSequenceOfShape, wires;
        for (TopExp_Explorer x(face, TopAbs_EDGE); x.More(); x.Next())
            if (!BRep_Tool::IsClosed(TopoDS::Edge(x.Current()), face))
                rims->Append(x.Current());
        ShapeAnalysis_FreeBounds::ConnectEdgesToWires(rims, Precision::Confusion(), false, wires);
        TopLoc_Location where;
        const Handle(Geom_Surface) surface = BRep_Tool::Surface(face, where);
        TopoDS_Face bare = BRepBuilderAPI_MakeFace(surface, Precision::Confusion()).Face();
        bare.Location(where);
        TopoDS_Face rebuilt = TopoDS::Face(bare.EmptyCopied());
        for (int k = 1; k <= wires->Length(); ++k)
            builder.Add(rebuilt, wires->Value(k));
        ShapeFix_Face fix(rebuilt);
        fix.SetContext(context);
        fix.FixMissingSeamMode() = 1;
        fix.FixOrientationMode() = 1;
        fix.Perform();
        TopoDS_Face made = fix.Face();
        made.Orientation(face.Orientation());
        GProp_GProps was, now;
        BRepGProp::SurfaceProperties(face, was);
        BRepGProp::SurfaceProperties(made, now);
        if (drawnTriangles(made) <= 0 || std::abs(now.Mass() - was.Mass()) > 1e-6 * std::max(1.0, was.Mass()))
            return {};
        context->Replace(face, made);
        any = true;
    }
    if (!any)
        return {};
    const TopoDS_Shape result = context->Apply(shape);
    GProp_GProps was, now;
    BRepGProp::VolumeProperties(shape, was);
    BRepGProp::VolumeProperties(result, now);
    if (!BRepCheck_Analyzer(result).IsValid() || std::abs(now.Mass() - was.Mass()) > 1e-6 * std::max(1.0, std::abs(was.Mass()))
        || !roundsDraw(result))
        return {};
    return result;
}

TopoDS_Shape unifyDrawable(const TopoDS_Shape &shape, double linear, double angular) {
    try {
        ShapeUpgrade_UnifySameDomain unify(shape, true, true, false);
        unify.SetLinearTolerance(linear);
        unify.SetAngularTolerance(angular);
        unify.Build();
        if (roundsDraw(unify.Shape()))
            return unify.Shape();
        // The merge can leave a round's merged face with no wrap-around line it can be drawn
        // along (hinge_female's countersinks, round 6): built again on its own surface from its
        // rims, with OpenCascade's healing adding the wrap-around line, as a made cone has one.
        if (const TopoDS_Shape again = reseamed(unify.Shape()); !again.IsNull())
            return again;
        // Again, the halves of every round left as they are.
        TopTools_IndexedDataMapOfShapeListOfShape edgeFaces;
        TopExp::MapShapesAndAncestors(shape, TopAbs_EDGE, TopAbs_FACE, edgeFaces);
        TopTools_MapOfShape keep;
        for (int i = 1; i <= edgeFaces.Extent(); ++i) {
            const TopTools_ListOfShape &list = edgeFaces(i);
            if (list.Extent() != 2)
                continue;
            const TopoDS_Face a = TopoDS::Face(list.First()), b = TopoDS::Face(list.Last());
            const GeomAbs_SurfaceType kind = BRepAdaptor_Surface(a, false).GetType();
            if ((kind == GeomAbs_Cylinder || kind == GeomAbs_Cone) && !a.IsSame(b) && sameSurface(a, b))
                keep.Add(edgeFaces.FindKey(i));
        }
        ShapeUpgrade_UnifySameDomain again(shape, true, true, false);
        again.SetLinearTolerance(linear);
        again.SetAngularTolerance(angular);
        again.KeepShapes(keep);
        again.Build();
        return roundsDraw(again.Shape()) ? again.Shape() : TopoDS_Shape();
    } catch (const Standard_Failure &) {
        return {};
    }
}

} // namespace refine
