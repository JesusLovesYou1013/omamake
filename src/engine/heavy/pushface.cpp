#include "engine/heavy/pushface.h"

#include <BRepCheck_Analyzer.hxx>
#include <BRepLib.hxx>
#include <BRepTools_ReShape.hxx>
#include <BRep_Builder.hxx>
#include <BRep_Tool.hxx>
#include <BRepAdaptor_Curve.hxx>
#include <BRepAdaptor_Surface.hxx>
#include <Geom2d_Curve.hxx>
#include <GeomAPI_IntSS.hxx>
#include <GeomAPI_ProjectPointOnCurve.hxx>
#include <GeomProjLib.hxx>
#include <Geom_Line.hxx>
#include <Geom_Plane.hxx>
#include <Geom_Surface.hxx>
#include <Geom_TrimmedCurve.hxx>
#include <Standard_Failure.hxx>
#include <TopAbs.hxx>
#include <TopExp.hxx>
#include <TopTools_DataMapOfShapeShape.hxx>
#include <TopTools_IndexedDataMapOfShapeListOfShape.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Iterator.hxx>
#include <gp_Pln.hxx>

#include <algorithm>
#include <cmath>

namespace pushface {

namespace {

// A curve on a round wall can come out a whole turn round from the one it must join: shifted by whole
// turns so its point at `at` lands nearest `ref`.
void alignTurns(Handle(Geom2d_Curve) &curve, const Handle(Geom_Surface) &surface, double at, const gp_Pnt2d &ref) {
    if (curve.IsNull() || !surface->IsUPeriodic())
        return;
    const double turn = surface->UPeriod();
    const double k = std::round((ref.X() - curve->Value(at).X()) / turn);
    if (k != 0) {
        curve = Handle(Geom2d_Curve)::DownCast(curve->Copy());
        curve->Translate(gp_Vec2d(k * turn, 0));
    }
}

// Rebuilt with each wire's edges swapped for their new ones, the face's own surface kept.
TopoDS_Face rebuilt(const TopoDS_Face &face, const Handle(Geom_Surface) &surface, const TopLoc_Location &where,
                    const TopTools_DataMapOfShapeShape &newEdges, const TopTools_IndexedMapOfShape &turned) {
    BRep_Builder b;
    TopoDS_Face out;
    b.MakeFace(out, surface, where, BRep_Tool::Tolerance(face));
    for (TopoDS_Iterator w(face.Oriented(TopAbs_FORWARD)); w.More(); w.Next()) {
        TopoDS_Wire wire;
        b.MakeWire(wire);
        for (TopoDS_Iterator e(w.Value()); e.More(); e.Next()) {
            TopoDS_Shape edge = newEdges.IsBound(e.Value()) ? newEdges(e.Value()) : e.Value();
            edge.Orientation(turned.Contains(edge) ? TopAbs::Reverse(e.Value().Orientation()) : e.Value().Orientation());
            b.Add(wire, edge);
        }
        wire.Orientation(w.Value().Orientation());
        b.Add(out, wire);
    }
    out.Orientation(face.Orientation());
    return out;
}

} // namespace

TopoDS_Shape move(const TopoDS_Shape &body, const TopoDS_Face &face, const gp_Vec &by, std::string &why) {
    auto fail = [&](const std::string &reason) {
        why = reason;
        return TopoDS_Shape();
    };
    try {
        BRep_Builder b;
        BRepAdaptor_Surface flat(face);
        if (flat.GetType() != GeomAbs_Plane)
            return fail("the face isn't flat");
        gp_Pln plane = flat.Plane();
        if (gp_Vec(plane.Axis().Direction()).Crossed(by).Magnitude() > 1e-9 * std::max(1.0, by.Magnitude()))
            return fail("the move isn't square to the face");
        plane.Translate(by);
        const Handle(Geom_Plane) moved = new Geom_Plane(plane);

        TopTools_IndexedDataMapOfShapeListOfShape facesOf, edgesAt;
        TopExp::MapShapesAndAncestors(body, TopAbs_EDGE, TopAbs_FACE, facesOf);
        TopExp::MapShapesAndAncestors(body, TopAbs_VERTEX, TopAbs_EDGE, edgesAt);
        TopTools_IndexedMapOfShape own, corners;
        TopExp::MapShapes(face, TopAbs_EDGE, own);
        TopExp::MapShapes(face, TopAbs_VERTEX, corners);
        TopTools_DataMapOfShapeShape newCorner, newEdge;   // as it was -> as it is now
        TopTools_IndexedMapOfShape turned;                   // new edges that run the other way round
        TopTools_IndexedMapOfShape sides;

        // 1. Each corner slides along the one straight edge running up from it, that edge's own line,
        //    to the new plane. The edge keeps its line, its far corner and its curves on its faces.
        for (int i = 1; i <= corners.Extent(); ++i) {
            const TopoDS_Vertex &corner = TopoDS::Vertex(corners(i));
            TopoDS_Edge side;
            int count = 0;
            for (const TopoDS_Shape &e : edgesAt.FindFromKey(corner))
                if (!own.Contains(e) && !side.IsSame(e)) {
                    side = TopoDS::Edge(e);
                    ++count;
                }
            if (count != 1)
                return fail("a corner of the face has " + std::to_string(count) + " edges running up from it");
            if (BRep_Tool::Degenerated(side))
                return fail("an edge running up from the face is a point");
            TopLoc_Location at;
            double first, last;
            const Handle(Geom_Curve) curve = BRep_Tool::Curve(side, at, first, last);
            const Handle(Geom_Line) line = Handle(Geom_Line)::DownCast(curve);
            if (line.IsNull() || !at.IsIdentity())
                return fail("an edge running up from the face isn't a plain straight line");
            const gp_Lin lin = line->Lin();
            const double along = gp_Vec(lin.Direction()).Dot(gp_Vec(plane.Axis().Direction()));
            if (std::abs(along) < 1e-9)
                return fail("an edge running up from the face lies along it");
            const double u = gp_Vec(lin.Location(), plane.Location()).Dot(gp_Vec(plane.Axis().Direction())) / along;
            TopoDS_Vertex slid;
            b.MakeVertex(slid, line->Value(u), BRep_Tool::Tolerance(corner));
            newCorner.Bind(corner, slid);

            const bool nearFirst = TopExp::FirstVertex(side).IsSame(corner);
            const double from = nearFirst ? u : first, to = nearFirst ? last : u;
            if (from >= to - 1e-9)
                return fail("the face would be moved past the far end of an edge running up from it");
            TopoDS_Edge longer;
            b.MakeEdge(longer, curve, BRep_Tool::Tolerance(side));
            TopoDS_Vertex a = nearFirst ? slid : TopExp::FirstVertex(side), z = nearFirst ? TopExp::LastVertex(side) : slid;
            a.Orientation(TopAbs_FORWARD);
            z.Orientation(TopAbs_REVERSED);
            b.Add(longer, a);
            b.Add(longer, z);
            b.Range(longer, from, to);
            newEdge.Bind(side, longer);
            sides.Add(side);
        }

        // 2. Each edge of the face again: where the new plane meets the neighbour's own surface.
        for (int i = 1; i <= own.Extent(); ++i) {
            const TopoDS_Edge &edge = TopoDS::Edge(own(i));
            TopoDS_Face next;
            for (const TopoDS_Shape &f : facesOf.FindFromKey(edge))
                if (!f.IsSame(face))
                    next = TopoDS::Face(f);
            if (next.IsNull())
                return fail("an edge of the face has nothing on its other side");
            TopLoc_Location at;
            const Handle(Geom_Surface) wall = BRep_Tool::Surface(next, at);
            if (!at.IsIdentity())
                return fail("a neighbouring face is placed by a move (not handled yet)");
            TopoDS_Vertex a = TopoDS::Vertex(newCorner(TopExp::FirstVertex(edge))),
                          z = TopoDS::Vertex(newCorner(TopExp::LastVertex(edge)));
            const gp_Pnt pa = BRep_Tool::Pnt(a), pz = BRep_Tool::Pnt(z);
            GeomAPI_IntSS meet(moved, wall, 1e-9);
            if (!meet.IsDone() || meet.NbLines() == 0)
                return fail("the new plane doesn't meet a neighbouring face's surface");
            Handle(Geom_Curve) curve;
            double ua = 0, uz = 0, off = 1e9;
            for (int k = 1; k <= meet.NbLines(); ++k) {
                Handle(Geom_Curve) c = meet.Line(k);
                if (const Handle(Geom_TrimmedCurve) t = Handle(Geom_TrimmedCurve)::DownCast(c))
                    c = t->BasisCurve();       // a whole ellipse, not a stretch of it
                GeomAPI_ProjectPointOnCurve ka(pa, c), kz(pz, c);
                if (!ka.NbPoints() || !kz.NbPoints())
                    continue;
                const double worst = std::max(ka.LowerDistance(), kz.LowerDistance());
                if (worst < off) {
                    off = worst;
                    curve = c;
                    ua = ka.LowerDistanceParameter();
                    uz = kz.LowerDistanceParameter();
                }
            }
            if (curve.IsNull())
                return fail("no curve where the new plane meets a neighbouring face");
            // The new corners sit on the edges running up, which the input's own tolerance allows to lie
            // a hair off a wall; never more than the input already allowed.
            const double allowed = std::max({BRep_Tool::Tolerance(edge), BRep_Tool::Tolerance(TopExp::FirstVertex(edge)),
                                             BRep_Tool::Tolerance(TopExp::LastVertex(edge))});
            if (off > std::max(allowed, 1e-7) * 1.5)
                return fail("a new corner lies off a neighbouring face by more than the model's own tolerance");
            bool turn = false;
            if (curve->IsPeriodic()) {
                const double period = curve->Period();
                while (uz <= ua)
                    uz += period;
                // The old edge's middle, moved, must lie on the stretch chosen; else it's the other way round.
                BRepAdaptor_Curve old(edge);
                const gp_Pnt middle = old.Value((old.FirstParameter() + old.LastParameter()) / 2).Translated(by);
                GeomAPI_ProjectPointOnCurve km(middle, curve);
                double um = km.LowerDistanceParameter();
                while (um < ua)
                    um += period;
                if (um > uz) {
                    std::swap(ua, uz);
                    std::swap(a, z);
                    while (uz <= ua)
                        uz += period;
                    turn = true;
                }
            } else if (uz < ua) {
                std::swap(ua, uz);
                std::swap(a, z);
                turn = true;
            }
            const double tolerance = std::max({BRep_Tool::Tolerance(edge), off * 1.01, 1e-7});
            TopoDS_Edge again;
            b.MakeEdge(again, curve, tolerance);
            TopoDS_Vertex fa = a, fz = z;
            fa.Orientation(TopAbs_FORWARD);
            fz.Orientation(TopAbs_REVERSED);
            b.Add(again, fa);
            b.Add(again, fz);
            b.UpdateVertex(a, std::max(BRep_Tool::Tolerance(a), off * 1.01));
            b.UpdateVertex(z, std::max(BRep_Tool::Tolerance(z), off * 1.01));
            b.Range(again, ua, uz);
            newEdge.Bind(edge, again);
            if (turn)
                turned.Add(again);
        }

        // 3. The face on its new plane, and each neighbour on its own surface with its new edges.
        BRepTools_ReShape swap;
        const TopoDS_Face movedFace = rebuilt(face, moved, TopLoc_Location(), newEdge, turned);
        for (int i = 1; i <= own.Extent(); ++i)
            BRepLib::BuildPCurveForEdgeOnPlane(TopoDS::Edge(newEdge(own(i))), movedFace);
        swap.Replace(face.Oriented(TopAbs_FORWARD), movedFace.Oriented(TopAbs_FORWARD));
        TopTools_DataMapOfShapeShape newFace;
        for (TopTools_DataMapOfShapeShape::Iterator it(newEdge); it.More(); it.Next())
            for (const TopoDS_Shape &f : facesOf.FindFromKey(it.Key())) {
                if (f.IsSame(face) || newFace.IsBound(f))
                    continue;
                TopLoc_Location at;
                const Handle(Geom_Surface) surface = BRep_Tool::Surface(TopoDS::Face(f), at);
                const TopoDS_Face again = rebuilt(TopoDS::Face(f), surface, at, newEdge, turned);
                newFace.Bind(f, again);
                swap.Replace(f.Oriented(TopAbs_FORWARD), again.Oriented(TopAbs_FORWARD));
            }

        // 4. The new edges' curves on the neighbours. An edge running up keeps its own where it still
        //    reaches; one that doesn't (a short spline on some imported walls) is worked out again on
        //    the wall's own surface. The face's new edges get theirs on the wall's surface.
        for (int i = 1; i <= sides.Extent(); ++i) {
            const TopoDS_Edge &side = TopoDS::Edge(sides(i));
            const TopoDS_Edge &longer = TopoDS::Edge(newEdge(side));
            double from, to;
            const Handle(Geom_Curve) line = BRep_Tool::Curve(longer, from, to);
            for (const TopoDS_Shape &f : facesOf.FindFromKey(side)) {
                if (BRep_Tool::IsClosed(side, TopoDS::Face(f)))
                    return fail("an edge running up from the face is a seam");
                double pf, pl;
                Handle(Geom2d_Curve) on = BRep_Tool::CurveOnSurface(side, TopoDS::Face(f), pf, pl);
                if (on.IsNull())
                    return fail("an edge running up from the face has no curve on its face");
                if (on->FirstParameter() > from + 1e-12 || on->LastParameter() < to - 1e-12) {
                    const Handle(Geom2d_Curve) old = on;
                    double sf, sl;
                    BRep_Tool::Range(side, sf, sl);
                    const double still = std::abs(sf - from) < 1e-9 ? from : to;   // the end that didn't move
                    TopLoc_Location at;
                    const Handle(Geom_Surface) surface = BRep_Tool::Surface(TopoDS::Face(f), at);
                    on = GeomProjLib::Curve2d(line, from, to, surface);
                    if (on.IsNull())
                        return fail("no curve on a wall for an edge running up from the face");
                    alignTurns(on, surface, still, old->Value(still));
                }
                b.UpdateEdge(longer, on, TopoDS::Face(newFace(f)), BRep_Tool::Tolerance(longer));
            }
            b.Range(longer, from, to);
            BRepLib::SameParameter(longer, BRep_Tool::Tolerance(longer));
        }
        for (int i = 1; i <= own.Extent(); ++i) {
            const TopoDS_Edge &edge = TopoDS::Edge(own(i));
            const TopoDS_Edge &again = TopoDS::Edge(newEdge(edge));
            double from, to;
            const Handle(Geom_Curve) curve = BRep_Tool::Curve(again, from, to);
            for (const TopoDS_Shape &f : facesOf.FindFromKey(edge)) {
                if (f.IsSame(face))
                    continue;
                TopLoc_Location at;
                const Handle(Geom_Surface) surface = BRep_Tool::Surface(TopoDS::Face(f), at);
                Handle(Geom2d_Curve) on = GeomProjLib::Curve2d(curve, from, to, surface);
                if (on.IsNull())
                    return fail("no curve on a wall for a moved edge");
                double of, ol;
                if (const Handle(Geom2d_Curve) old = BRep_Tool::CurveOnSurface(edge, TopoDS::Face(f), of, ol); !old.IsNull())
                    alignTurns(on, surface, (from + to) / 2, old->Value((of + ol) / 2));
                b.UpdateEdge(again, on, TopoDS::Face(newFace(f)), BRep_Tool::Tolerance(again));
            }
            b.Range(again, from, to);
            BRepLib::SameParameter(again, BRep_Tool::Tolerance(again));
        }

        const TopoDS_Shape out = swap.Apply(body);
        if (out.IsNull() || !BRepCheck_Analyzer(out).IsValid())
            return fail("the moved face didn't come out a sound solid");
        return out;
    } catch (const Standard_Failure &failure) {
        return fail(std::string("OpenCascade: ") + failure.GetMessageString());
    }
}

} // namespace pushface
