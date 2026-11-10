#include "engine/heavy/chamfer.h"

#include <BRepAdaptor_Curve.hxx>
#include <BRepBndLib.hxx>
#include <BRepGProp.hxx>
#include <GProp_GProps.hxx>
#include <Bnd_Box.hxx>
#include <BRepAdaptor_Surface.hxx>
#include <BRepAlgoAPI_Common.hxx>
#include <BRepAlgoAPI_Cut.hxx>
#include <BRepAlgoAPI_Fuse.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_MakePolygon.hxx>
#include <BRepBuilderAPI_MakeSolid.hxx>
#include <BRepBuilderAPI_Sewing.hxx>
#include <BRepClass3d_SolidClassifier.hxx>
#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepBuilderAPI_MakeWire.hxx>
#include <BRepBuilderAPI_Transform.hxx>
#include <BRepFilletAPI_MakeChamfer.hxx>
#include <BRepFilletAPI_MakeFillet.hxx>
#include <TopTools_MapOfShape.hxx>
#include <GC_MakeArcOfCircle.hxx>
#include <BRepOffsetAPI_MakeFilling.hxx>
#include <BRepOffsetAPI_ThruSections.hxx>
#include <BRepPrimAPI_MakeCone.hxx>
#include <BRepPrimAPI_MakeCylinder.hxx>
#include <BRepPrimAPI_MakeHalfSpace.hxx>
#include <BRepPrimAPI_MakePrism.hxx>
#include <BRepPrimAPI_MakeRevol.hxx>
#include <BRepPrimAPI_MakeSphere.hxx>
#include <BRepTools.hxx>
#include <GCPnts_AbscissaPoint.hxx>
#include <TopoDS_Vertex.hxx>
#include <BRepTopAdaptor_FClass2d.hxx>
#include <BRep_Tool.hxx>
#include <GeomAPI_ProjectPointOnSurf.hxx>
#include <Geom_Surface.hxx>
#include <ShapeFix_Solid.hxx>
#include <ShapeUpgrade_UnifySameDomain.hxx>
#include <Standard_Failure.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopTools_IndexedDataMapOfShapeListOfShape.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Shell.hxx>
#include <gp_Lin.hxx>
#include <gp_Pln.hxx>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace chamfer {

TopoDS_Face otherFace(const TopoDS_Shape &body, const TopoDS_Edge &edge, const TopoDS_Face &face) {
    TopTools_IndexedDataMapOfShapeListOfShape edgeFaces;
    TopExp::MapShapesAndAncestors(body, TopAbs_EDGE, TopAbs_FACE, edgeFaces);
    if (!edgeFaces.Contains(edge))
        return {};
    TopoDS_Face other;
    int distinct = 0;
    for (const TopoDS_Shape &f : edgeFaces.FindFromKey(edge)) {
        if (f.IsSame(face))
            continue;
        if (other.IsNull() || !other.IsSame(f)) {
            if (!other.IsNull())
                return {};                       // three or more faces: not a solid's edge
            other = TopoDS::Face(f);
            ++distinct;
        }
    }
    return distinct == 1 ? other : TopoDS_Face();
}

namespace {

// The face's outward normal at the edge's parameter s, and the way into the face square to the edge
// (along the surface's tangent plane). False when the face can't say there.
bool intoFace(const TopoDS_Edge &edge, const TopoDS_Face &face, double s, const gp_Vec &t, double probe,
              gp_Vec &normal, gp_Vec &into) {
    Standard_Real first, last;
    const Handle(Geom2d_Curve) pcurve = BRep_Tool::CurveOnSurface(edge, face, first, last);
    if (pcurve.IsNull())
        return false;
    const gp_Pnt2d uv = pcurve->Value(s);
    BRepAdaptor_Surface surface(face);
    gp_Pnt p;
    gp_Vec du, dv;
    surface.D1(uv.X(), uv.Y(), p, du, dv);
    gp_Vec n = du.Crossed(dv);
    if (n.Magnitude() < 1e-12)
        return false;
    n.Normalize();
    if (face.Orientation() == TopAbs_REVERSED)
        n.Reverse();
    gp_Vec u = t.Crossed(n);
    if (u.Magnitude() < 1e-9)
        return false;
    u.Normalize();
    // Which way is into the face: a point a little way along each side, found on the face's own surface and
    // tested against its outline.
    const Handle(Geom_Surface) geom = BRep_Tool::Surface(face);
    BRepTopAdaptor_FClass2d inside(face, 1e-7);
    for (const double sign : {1.0, -1.0}) {
        GeomAPI_ProjectPointOnSurf onto(p.Translated(u * (probe * sign)), geom);
        if (!onto.IsDone() || onto.NbPoints() == 0)
            continue;
        double a, b;
        onto.LowerDistanceParameters(a, b);
        if (inside.Perform(gp_Pnt2d(a, b)) == TopAbs_IN) {
            normal = n;
            into = u * sign;
            return true;
        }
    }
    return false;
}

// How far from `p` along `u` (a unit vector, p on the face's edge) the face itself goes, up to `upto`: the
// points on the way must lie on its surface and inside its outline (GB-052, for Ring::reach).
double faceReach(const TopoDS_Face &face, const gp_Pnt &p, const gp_Vec &u, double upto) {
    if (upto <= 0)
        return 0;
    try {
        const Handle(Geom_Surface) geom = BRep_Tool::Surface(face);
        BRepTopAdaptor_FClass2d inside(face, 1e-7);
        const double tol = std::max(1e-7, upto * 1e-7);
        const auto on = [&](double at) {
            GeomAPI_ProjectPointOnSurf onto(p.Translated(u * at), geom);
            if (!onto.IsDone() || onto.NbPoints() == 0 || onto.LowerDistance() > tol)
                return false;
            double a, b;
            onto.LowerDistanceParameters(a, b);
            return inside.Perform(gp_Pnt2d(a, b)) != TopAbs_OUT;
        };
        const int n = 32;
        double in = 0;
        for (int k = 1; k <= n; ++k) {
            const double at = upto * k / n;
            if (!on(at)) {
                double out = at;
                for (int i = 0; i < 30; ++i) {
                    const double mid = (in + out) / 2;
                    (on(mid) ? in : out) = mid;
                }
                return in;
            }
            in = at;
        }
        return upto;
    } catch (Standard_Failure &) {
        return 0;
    }
}

struct Section {
    gp_Pnt p, a, b;      // the edge's point, the bevel's end on the ref face, on the other face
    gp_Vec t;            // the edge's direction there
    bool round = false;  // a fillet's: a and b are where the round touches the faces, m the arc's middle
    gp_Pnt m;
    double r = 0;        // and its radius
    Section moved(const gp_Vec &by) const {
        Section s = *this;
        s.p.Translate(by);
        s.a.Translate(by);
        s.b.Translate(by);
        s.m.Translate(by);
        return s;
    }
};

bool sectionAt(const Item &it, const TopoDS_Face &other, double s, Section &out) {
    BRepAdaptor_Curve curve(it.edge);
    gp_Pnt p;
    gp_Vec t;
    curve.D1(s, p, t);
    if (t.Magnitude() < 1e-12)
        return false;
    t.Normalize();
    const double probe = std::clamp(std::min(it.d1, it.d2) * 0.01, 1e-4, 0.01);
    gp_Vec n1, u1, n2, u2;
    if (!intoFace(it.edge, it.ref, s, t, probe, n1, u1) || !intoFace(it.edge, other, s, t, probe, n2, u2))
        return false;
    if (rounds(it.kind)) {
        // The round: a circle touching both faces, the corner less it the cross-section. The faces open at phi:
        // it touches them r / tan(phi/2) from the edge; a chord's radius from its length, chord = 2 r cos(phi/2).
        const double phi = u1.Angle(u2);
        if (phi < 1e-3 || phi > M_PI - 1e-3)
            return false;
        const double r = it.kind == Kind::Chord ? it.d1 / (2 * std::cos(phi / 2)) : it.d1;
        const double reach = r / std::tan(phi / 2);
        gp_Vec bisector = u1.Normalized() + u2.Normalized();
        if (bisector.Magnitude() < 1e-9)
            return false;
        bisector.Normalize();
        const gp_Pnt centre = p.Translated(bisector * (r / std::sin(phi / 2)));
        out.p = p;
        out.a = p.Translated(u1 * reach);
        out.b = p.Translated(u2 * reach);
        out.m = centre.Translated(gp_Vec(centre, p).Normalized() * r);
        out.round = true;
        out.r = r;
        out.t = t;
        return true;
    }
    double second = it.d1;
    if (it.kind == Kind::Two) {
        second = it.d2;
    } else if (it.kind == Kind::Angle) {
        // The bevel leaves the ref face at `angle`; the faces open at `phi` between them (law of sines).
        const double phi = u1.Angle(u2);
        const double across = std::sin(phi + it.angle);
        if (across < 1e-6)
            return false;
        second = it.d1 * std::sin(it.angle) / across;
    }
    out.p = p;
    out.a = p.Translated(u1 * it.d1);
    out.b = p.Translated(u2 * second);
    out.t = t;
    return true;
}

// A triangle given in (r, z) about an axis (r across from it, z along it) turned `sweep` radians about it
// (0: right round), built from OpenCascade's own cylinders and cones: each side of the triangle is a cone, a
// cylinder or a flat ring about the axis, so the tool's faces are the same kinds as a turned body's own. A side
// that crosses the axis is cut back there, so the bevel comes to a point in the middle (a cone).
// Sizes typed against a sketch's solved sizes are equal only to within the solver's noise (owner, pt156u, GB-054:
// a 10 in circle dimensioned in the sketch is 127.000000000251 mm across-half, so a 5 in chamfer stopped 2.5e-10 mm
// short of the axis and OpenCascade's cone refused a radius that small; just past it a fillet lost the whole body).
// A section's radii within `tol` of the axis go onto it, and values within `tol` of each other become one, so
// a bevel meant to reach the axis (or meet another) does so exactly, as when the numbers were typed exactly.
void snapNear(double &v, std::vector<double> &seen, double tol, bool axis) {
    if (axis && std::abs(v) <= tol)
        v = 0;
    for (const double s : seen)
        if (std::abs(v - s) <= tol) {
            v = s;
            return;
        }
    seen.push_back(v);
}

TopoDS_Shape ringWedge(const gp_Ax2 &frame, const double rzIn[3][2], double sweep) {
    double rz[3][2];
    {
        double scale = 1;
        for (int i = 0; i < 3; ++i)
            scale = std::max({scale, std::abs(rzIn[i][0]), std::abs(rzIn[i][1])});
        const double tol = 1e-9 * scale;
        std::vector<double> rs{0.0}, zs;
        for (int i = 0; i < 3; ++i) {
            rz[i][0] = rzIn[i][0];
            rz[i][1] = rzIn[i][1];
            snapNear(rz[i][0], rs, tol, true);
            snapNear(rz[i][1], zs, tol, false);
        }
    }
    double zlo = rz[0][1], zhi = zlo, rhi = rz[0][0];
    for (int i = 1; i < 3; ++i) {
        zlo = std::min(zlo, rz[i][1]);
        zhi = std::max(zhi, rz[i][1]);
        rhi = std::max(rhi, rz[i][0]);
    }
    const double eps = 1e-9 * std::max(1.0, rhi + zhi - zlo);
    if (rhi <= eps || zhi - zlo <= eps)
        return {};
    const auto at = [&](double z) { return frame.Translated(gp_Vec(frame.Direction()) * z); };
    const auto cylinder = [&](double r) {
        return sweep > 0 ? BRepPrimAPI_MakeCylinder(at(zlo), r, zhi - zlo, sweep).Shape() : BRepPrimAPI_MakeCylinder(at(zlo), r, zhi - zlo).Shape();
    };
    TopoDS_Shape solid = cylinder(rhi);
    for (int i = 0; i < 3; ++i) {
        const double *q = rz[i], *n = rz[(i + 1) % 3], *o = rz[(i + 2) % 3];
        const double dr = n[0] - q[0], dz = n[1] - q[1];
        if (std::abs(dz) <= eps)
            continue;                    // flat: a triangle's flat side is its top or bottom, the cylinder's own
        TopoDS_Shape side;
        bool inside;                     // the triangle lies inside this side (nearer the axis)
        if (std::abs(dr) <= eps) {
            if (q[0] >= rhi - eps || q[0] <= eps)
                continue;                // the cylinder's own wall, or the axis
            side = cylinder(q[0]);
            inside = o[0] < q[0];
        } else {
            const auto r = [&](double z) { return q[0] + dr * (z - q[1]) / dz; };
            inside = o[0] < r(o[1]);
            double za = zlo, zb = zhi, ra = r(zlo), rb = r(zhi);
            if (ra < 0 && rb < 0) {
                if (inside)
                    return {};
                continue;
            }
            if (ra < 0) { za = q[1] - q[0] * dz / dr; ra = 0; }
            if (rb < 0) { zb = q[1] - q[0] * dz / dr; rb = 0; }
            if (zb - za <= eps)
                continue;
            side = sweep > 0 ? BRepPrimAPI_MakeCone(at(za), ra, rb, zb - za, sweep).Shape() : BRepPrimAPI_MakeCone(at(za), ra, rb, zb - za).Shape();
        }
        if (inside) {
            BRepAlgoAPI_Common keep(solid, side);
            if (!keep.IsDone())
                return {};
            solid = keep.Shape();
        } else {
            BRepAlgoAPI_Cut keep(solid, side);
            if (!keep.IsDone())
                return {};
            solid = keep.Shape();
        }
    }
    return solid;
}

// Whether a shape still holds any material (bevels that meet can take a body away altogether).
bool hasVolume(const TopoDS_Shape &s) {
    if (s.IsNull())
        return false;
    GProp_GProps g;
    BRepGProp::VolumeProperties(s, g);
    return g.Mass() > 1e-9;
}

// The cross-section's outline: a chamfer's triangle; a fillet's corner, its two sides and the round's arc.
TopoDS_Wire outline(const Section &s) {
    if (!s.round) {
        BRepBuilderAPI_MakePolygon poly(s.p, s.a, s.b, true);
        return poly.IsDone() ? poly.Wire() : TopoDS_Wire();
    }
    try {
        const GC_MakeArcOfCircle arc(s.a, s.m, s.b);
        if (!arc.IsDone())
            return {};
        BRepBuilderAPI_MakeWire w(BRepBuilderAPI_MakeEdge(s.p, s.a).Edge(), BRepBuilderAPI_MakeEdge(arc.Value()).Edge(),
                                  BRepBuilderAPI_MakeEdge(s.b, s.p).Edge());
        return w.IsDone() ? w.Wire() : TopoDS_Wire();
    } catch (Standard_Failure &) {
        return {};
    }
}

// The faces at a corner of the edge, other than its own two, that end the swept bevel there: each one's
// plane at the corner, unless it carries on smoothly along the edge (it then holds the edge's direction).
// And a stop square across the edge where those faces themselves end (`last`: the curve's far end, so out is
// along t): an end face's plane carried on past the face would let a bevel running through reach on past the
// body's end (owner's test, 2026-11-08: a boss's chamfer running down into the plate went on along the plate,
// because the boss's end was an earlier chamfer's slanted face, and its plane reaches out further down).
std::vector<gp_Pln> endsAt(const TopoDS_Shape &body, const TopoDS_Vertex &corner, const Item &it, const TopoDS_Face &other,
                           const gp_Vec &t, bool last) {
    std::vector<gp_Pln> out;
    const gp_Vec outward = last ? t : -t;
    double reach = 0;
    bool square = false;            // an end face already square across the edge
    TopTools_IndexedDataMapOfShapeListOfShape vertexFaces;
    TopExp::MapShapesAndAncestors(body, TopAbs_VERTEX, TopAbs_FACE, vertexFaces);
    if (!vertexFaces.Contains(corner))
        return out;
    const gp_Pnt at = BRep_Tool::Pnt(corner);
    TopTools_IndexedMapOfShape seen;
    for (const TopoDS_Shape &f : vertexFaces.FindFromKey(corner)) {
        if (f.IsSame(it.ref) || f.IsSame(other) || seen.Contains(f))
            continue;
        seen.Add(f);
        const TopoDS_Face &face = TopoDS::Face(f);
        const Handle(Geom_Surface) geom = BRep_Tool::Surface(face);
        GeomAPI_ProjectPointOnSurf onto(at, geom);
        if (!onto.IsDone() || onto.NbPoints() == 0)
            continue;
        double u, v;
        onto.LowerDistanceParameters(u, v);
        gp_Pnt p;
        gp_Vec du, dv;
        geom->D1(u, v, p, du, dv);
        gp_Vec n = du.Crossed(dv);
        if (n.Magnitude() < 1e-12)
            continue;
        n.Normalize();
        if (std::abs(n.Dot(t)) < 0.05)
            continue;                            // runs on along the edge: not an end
        out.emplace_back(at, gp_Dir(n));
        square = square || std::abs(n.Dot(t)) > 0.9999;
        Bnd_Box box;
        BRepBndLib::Add(face, box);
        if (!box.IsVoid()) {
            double x0, y0, z0, x1, y1, z1;
            box.Get(x0, y0, z0, x1, y1, z1);
            for (const double x : {x0, x1})
                for (const double y : {y0, y1})
                    for (const double z : {z0, z1})
                        reach = std::max(reach, gp_Vec(at, gp_Pnt(x, y, z)).Dot(outward));
        }
    }
    if (!out.empty() && !(square && reach < 1e-6) && outward.Magnitude() > 1e-12)
        out.emplace_back(at.Translated(outward * reach), gp_Dir(outward));
    return out;
}

// A whole rim's bevel as it was swept: its axis, the way across from it the section was taken, and the
// section's three corners (and a fillet's arc middle), so bevels round one axis can be put together in one
// section (Ring::ok).
struct Ring {
    bool ok = false;
    gp_Ax1 axis;
    gp_Dir radial;
    gp_Pnt pts[3];
    bool round = false;
    gp_Pnt m;
    // How far each face beside the edge goes along the section from it (to a, to b): the tool reaches out past
    // the body over that much of each (see ringsTool), so none of its faces lies on one of the body's.
    double reach[2] = {0, 0};
};

// The swept bevel of one edge, as a solid; `cut` says whether it comes away (an outside edge) or is added
// (an inside one). Null when it can't be built.
TopoDS_Shape ringsTool(const std::vector<const Ring *> &rings);

// Outside or inside edge: the middle of the cross-section, close in by the edge, lies in the body or out of
// it (close in: a bevel bigger than the body reaches out past it, owner 2026-11-09, and its middle there said
// "inside edge", so the bevel was added on instead of cut away). IN: it's cut away; ON: it can't be told.
TopAbs_State sideOf(const TopoDS_Shape &body, const Section &m) {
    const gp_Pnt c = m.p.Translated((gp_Vec(m.p, m.a) + gp_Vec(m.p, m.b)) * (0.01 / 3));
    BRepClass3d_SolidClassifier where(body, c, 1e-7);
    return where.State();
}

TopoDS_Shape wedge(const TopoDS_Shape &body, const Item &it, bool &cut, std::string &why, bool wholeCircle = false,
                   Ring *ring = nullptr) {
    const TopoDS_Face other = otherFace(body, it.edge, it.ref);
    if (other.IsNull()) {
        why = "the edge hasn't two faces";
        return {};
    }
    BRepAdaptor_Curve curve(it.edge);
    const double s0 = curve.FirstParameter(), s1 = curve.LastParameter(), mid = (s0 + s1) / 2;
    Section m;
    if (!sectionAt(it, other, mid, m)) {
        why = "the faces beside the edge can't say where the bevel lies";
        return {};
    }
    const TopAbs_State side = sideOf(body, m);
    if (side == TopAbs_ON) {
        why = "the bevel lies flat on the faces";
        return {};
    }
    cut = side == TopAbs_IN;
    const double size = std::max({m.p.Distance(m.a), m.p.Distance(m.b), 1e-3});
    TopoDS_Vertex v0, v1;
    TopExp::Vertices(it.edge, v0, v1, true);
    const bool closed = wholeCircle || BRep_Tool::IsClosed(it.edge) || (!v0.IsNull() && v0.IsSame(v1));
    std::vector<gp_Pln> ends0, ends1;
    if (!closed && !v0.IsNull() && !v1.IsNull()) {
        // The ends in the curve's own direction (TopExp::Vertices with the edge's orientation can swap them).
        const bool forward = BRep_Tool::Pnt(v0).Distance(curve.Value(s0)) <= BRep_Tool::Pnt(v1).Distance(curve.Value(s0));
        const TopoDS_Vertex &first = forward ? v0 : v1, &last = forward ? v1 : v0;
        gp_Pnt p;
        gp_Vec t0, t1;
        curve.D1(s0, p, t0);
        curve.D1(s1, p, t1);
        ends0 = endsAt(body, first, it, other, t0.Normalized(), false);
        ends1 = endsAt(body, last, it, other, t1.Normalized(), true);
    }
    // Past an end that another face closes, the bevel runs on a little and is trimmed back by that face's
    // plane, so it ends exactly where the body's corner does.
    const double run = 3 * size;
    TopoDS_Shape solid;
    try {
        if (curve.GetType() == GeomAbs_Line) {
            const double back = ends0.empty() ? 0 : run, on = ends1.empty() ? 0 : run;
            const gp_Vec t = m.t;
            gp_Pnt start;
            curve.D0(s0, start);
            const gp_Vec shift = gp_Vec(m.p, start) - t * back;
            const Section s = m.moved(shift);
            const TopoDS_Wire w = outline(s);
            if (w.IsNull())
                return {};
            const TopoDS_Face f = BRepBuilderAPI_MakeFace(w, true).Face();
            solid = BRepPrimAPI_MakePrism(f, t * ((s1 - s0) + back + on)).Shape();   // a line's parameter is its length
        } else if (curve.GetType() == GeomAbs_Circle) {
            const gp_Circ circle = curve.Circle();
            const double r = std::max(circle.Radius(), 1e-9);
            const double back = closed || ends0.empty() ? 0 : std::min(run / r, 0.5), on = closed || ends1.empty() ? 0 : std::min(run / r, 0.5);
            const double from = s0 - back, sweep = closed ? 2 * M_PI : (s1 - s0) + back + on;
            // The section from the middle of the arc (its ends can sit on a line between faces, where which way
            // is into a face can't be told), in r (across from the axis, in the section's plane) and z (along it).
            const Section &s = m;
            const gp_Ax1 axis = circle.Axis();
            const gp_Vec along(axis.Direction());
            gp_Vec radial(axis.Location(), s.p);
            radial -= along * radial.Dot(along);
            if (radial.Magnitude() < 1e-9)
                return {};
            radial.Normalize();
            double rz[3][2];
            const gp_Pnt tri[3] = {s.p, s.a, s.b};
            for (int i = 0; i < 3; ++i) {
                const gp_Vec v(axis.Location(), tri[i]);
                rz[i][0] = v.Dot(radial);
                rz[i][1] = v.Dot(along);
            }
            // Swept from where the arc starts (the frame's x), round the circle's own way. A fillet's section,
            // turned about the axis (its arc makes a torus).
            if (s.round && closed) {
                // A whole rim's round: by ringsTool, which keeps its section to the near side of the axis (a
                // round bigger than the rim's radius reaches across it, and a turned face crossing its own axis
                // makes no solid, GB-051).
                Ring one;
                one.ok = true;
                one.axis = axis;
                one.radial = gp_Dir(radial);
                for (int i = 0; i < 3; ++i)
                    one.pts[i] = tri[i];
                one.round = true;
                one.m = s.m;
                one.reach[0] = faceReach(it.ref, s.p, gp_Vec(s.p, s.a).Normalized(), s.p.Distance(s.a));
                one.reach[1] = faceReach(other, s.p, gp_Vec(s.p, s.b).Normalized(), s.p.Distance(s.b));
                solid = ringsTool({&one});
            } else if (s.round) {
                const TopoDS_Wire w = outline(s);
                if (w.IsNull())
                    return {};
                gp_Trsf back;
                back.SetRotation(axis, from - mid);
                const TopoDS_Shape f = BRepBuilderAPI_Transform(BRepBuilderAPI_MakeFace(w, true).Face(), back, true).Shape();
                solid = BRepPrimAPI_MakeRevol(f, axis, sweep, true).Shape();
            } else {
                const gp_Vec start = radial.Rotated(axis, from - mid);
                solid = ringWedge(gp_Ax2(axis.Location(), axis.Direction(), gp_Dir(start)), rz, closed ? 0 : sweep);
            }
            if (solid.IsNull())
                return {};
            if (ring && closed) {
                ring->ok = true;
                ring->axis = axis;
                ring->radial = gp_Dir(radial);
                for (int i = 0; i < 3; ++i)
                    ring->pts[i] = tri[i];
                ring->round = s.round;
                ring->m = s.m;
                ring->reach[0] = faceReach(it.ref, s.p, gp_Vec(s.p, s.a).Normalized(), s.p.Distance(s.a));
                ring->reach[1] = faceReach(other, s.p, gp_Vec(s.p, s.b).Normalized(), s.p.Distance(s.b));
            }
        } else {
            // Any other curve: the cross-section at many points along it, lofted.
            BRepOffsetAPI_ThruSections loft(true, false, 1e-6);
            const int n = 32;
            for (int i = 0; i <= n; ++i) {
                Section s;
                if (!sectionAt(it, other, s0 + (s1 - s0) * i / n, s)) {
                    why = "the faces beside the edge can't say where the bevel lies all along it";
                    return {};
                }
                const TopoDS_Wire w = outline(s);
                if (w.IsNull())
                    return {};
                loft.AddWire(w);
            }
            loft.CheckCompatibility(false);
            loft.Build();
            if (!loft.IsDone())
                return {};
            solid = loft.Shape();
        }
        // Trimmed by each closing face's plane, keeping the side the edge lies on.
        const gp_Pnt inner = curve.Value(mid);
        for (const auto *list : {&ends0, &ends1})
            for (const gp_Pln &plane : *list) {
                if (plane.Distance(inner) < 1e-7)
                    continue;
                const TopoDS_Face sheet = BRepBuilderAPI_MakeFace(plane).Face();
                const TopoDS_Shape half = BRepPrimAPI_MakeHalfSpace(sheet, inner).Solid();
                BRepAlgoAPI_Common keep(solid, half);
                if (!keep.IsDone())
                    return {};
                solid = keep.Shape();
            }
    } catch (Standard_Failure &e) {
        why = e.GetMessageString() ? e.GetMessageString() : "OpenCascade failed";
        return {};
    }
    return solid;
}

// GB-063 (owner pt169u: "a neighboring fillet or chamfer is not melding very well with an existing fillet or
// chamfer ... we want this geometry to meld together in a natural way no matter what order they are applied in"):
// whether the band the bevel or round takes off a face beside the edge runs off that face somewhere along the
// edge, into a hole or a neighbouring fillet's or chamfer's face. OpenCascade's own chamfer and fillet know only
// the two faces beside the edge and can't trim the faces the band runs into: they come back "sound" and wrong
// (a 12 mm round of a slab's edge across a 6 mm hole whose rim was bevelled 3 mm left 116.7 mm3 that should have
// gone, one across a plain hole 21.6 mm3), so such an edge runs through: its swept cross-section cut away by
// the Boolean, which meets whatever it hits exactly. A point counts only where it lies on the face's own surface
// (a band across a curved face leaves the surface at once and says nothing) and outside its outline. Not at the
// edge's ends, where the faces round its corners take over.
bool leavesFaces(const TopoDS_Shape &body, const Item &it) {
    try {
        const TopoDS_Face other = otherFace(body, it.edge, it.ref);
        if (other.IsNull())
            return false;
        BRepAdaptor_Curve curve(it.edge);
        const double s0 = curve.FirstParameter(), s1 = curve.LastParameter();
        const TopoDS_Face faces[2] = {it.ref, other};
        const Handle(Geom_Surface) geom[2] = {BRep_Tool::Surface(it.ref), BRep_Tool::Surface(other)};
        BRepTopAdaptor_FClass2d inside0(it.ref, 1e-7), inside1(other, 1e-7);
        BRepTopAdaptor_FClass2d *inside[2] = {&inside0, &inside1};
        const int n = 16, m = 8;
        for (int k = 1; k < n; ++k) {
            Section sec;
            if (!sectionAt(it, other, s0 + (s1 - s0) * k / n, sec))
                continue;
            const gp_Pnt ends[2] = {sec.a, sec.b};
            for (int f = 0; f < 2; ++f) {
                const double width = sec.p.Distance(ends[f]), tol = std::max(1e-6, width * 1e-7);
                for (int j = 1; j <= m; ++j) {
                    const gp_Pnt q = sec.p.Translated(gp_Vec(sec.p, ends[f]) * (double(j) / m));
                    GeomAPI_ProjectPointOnSurf onto(q, geom[f]);
                    if (!onto.IsDone() || onto.NbPoints() == 0 || onto.LowerDistance() > tol)
                        break;                  // off the surface (a curved face): nothing more to tell here
                    double u, v;
                    onto.LowerDistanceParameters(u, v);
                    if (inside[f]->Perform(gp_Pnt2d(u, v)) == TopAbs_OUT)
                        return true;
                }
            }
        }
    } catch (Standard_Failure &) {
    }
    return false;
}

// A fillet item's radius (a chord's, from the faces' opening halfway along); 0 when it can't be told.
double radiusOf(const TopoDS_Shape &body, const Item &it) {
    if (it.kind == Kind::Fillet)
        return it.d1;
    const TopoDS_Face other = otherFace(body, it.edge, it.ref);
    BRepAdaptor_Curve curve(it.edge);
    Section s;
    return !other.IsNull() && sectionAt(it, other, (curve.FirstParameter() + curve.LastParameter()) / 2, s) ? s.r : 0;
}

// OpenCascade's own chamfer (or fillet, for rounding items) of these items, built or null.
TopoDS_Shape native(const TopoDS_Shape &body, const std::vector<const Item *> &items, std::unique_ptr<BRepFilletAPI_LocalOperation> *keep = nullptr) {
    if (items.empty())
        return {};
    try {
        std::unique_ptr<BRepFilletAPI_LocalOperation> mk;
        if (rounds(items.front()->kind)) {
            auto fillet = std::make_unique<BRepFilletAPI_MakeFillet>(body);
            for (const Item *it : items) {
                const double r = radiusOf(body, *it);
                if (!(r > 0))
                    return {};
                fillet->Add(r, it->edge);
            }
            mk = std::move(fillet);
        } else {
            auto bevel = std::make_unique<BRepFilletAPI_MakeChamfer>(body);
            for (const Item *it : items) {
                switch (it->kind) {
                case Kind::Equal: bevel->Add(it->d1, it->edge); break;
                case Kind::Two: bevel->Add(it->d1, it->d2, it->edge, it->ref); break;
                case Kind::Angle: bevel->AddDA(it->d1, it->angle, it->edge, it->ref); break;
                default: break;
                }
            }
            mk = std::move(bevel);
        }
        mk->Build();
        if (!mk->IsDone())
            return {};
        // OpenCascade carries a chamfer or fillet on along every edge tangent to the picked ones (a vertical
        // edge into a filleted one's rounded rim, say). The set's edges are all it may do (a chain is picked
        // as one by then), so when it went further its result isn't this set's (GB-058: a Two Distances
        // chamfer beside a fillet took the fillet's whole rim at small sizes, then not at its full size).
        TopTools_MapOfShape asked;
        for (const Item *it : items)
            asked.Add(it->edge);
        for (int c = 1; c <= mk->NbContours(); ++c)
            for (int j = 1; j <= mk->NbEdges(c); ++j)
                if (!asked.Contains(mk->Edge(c, j)))
                    return {};
        TopoDS_Shape out = mk->Shape();
        if (keep)
            *keep = std::move(mk);
        return out;
    } catch (Standard_Failure &) {
        return {};
    }
}

// Whether two axes are the same line.
bool sameLine(const gp_Ax1 &a, const gp_Ax1 &b) {
    return a.Direction().IsParallel(b.Direction(), 1e-9) && gp_Lin(a).Distance(b.Location()) < 1e-7;
}

// Whole rims' bevels round one axis as one tool: their sections joined flat, in one plane through the axis,
// kept to the near side of it, and turned right round. Two bevels that meet then meet in the section as a
// point (the ridge), instead of OpenCascade having to cut one cone by another round the same axis, which
// it gets wrong (owner, 2026-11-09: a 2.5 x 1 in disc's two rims chamfered past half its thickness lost
// one bevel, or the whole body, or kept everything). Null when it can't be built.
TopoDS_Shape ringsTool(const std::vector<const Ring *> &rings) {
    const gp_Ax1 &axis = rings.front()->axis;
    const gp_Pnt o = axis.Location();
    const gp_Dir up = axis.Direction(), x = rings.front()->radial;
    try {
        TopoDS_Shape flat;
        double rmax = 0, zlo = 1e300, zhi = -1e300;
        // Near the axis or another rim's section within the sketch solver's noise: exactly on it (GB-054, snapNear).
        double scale = 1;
        for (const Ring *r : rings)
            for (const gp_Pnt &p : r->pts)
                scale = std::max(scale, o.Distance(p));
        const double tol = 1e-9 * scale;
        std::vector<double> rs{0.0}, zs;
        for (const Ring *r : rings) {
            gp_Pnt q[4];
            const gp_Pnt from[4] = {r->pts[0], r->pts[1], r->pts[2], r->m};
            double rz[4][2];
            for (int i = 0; i < 4; ++i) {
                const gp_Vec v(o, from[i]);
                rz[i][0] = v.Dot(gp_Vec(r->radial));
                rz[i][1] = v.Dot(gp_Vec(up));
            }
            // A round's arc: its centre (through a, the middle, b) snapped too, and the middle put back on the
            // circle round it, so two rims' rounds meant to be one sphere are exactly one (GB-054).
            double cr = 0, cz = 0;
            bool centred = false;
            if (r->round) {
                const double ax = rz[1][0], ay = rz[1][1], bx = rz[3][0], by = rz[3][1], qx = rz[2][0], qy = rz[2][1];
                const double d = 2 * (ax * (by - qy) + bx * (qy - ay) + qx * (ay - by));
                if (std::abs(d) > 1e-12 * scale * scale) {
                    const double a2 = ax * ax + ay * ay, b2 = bx * bx + by * by, q2 = qx * qx + qy * qy;
                    cr = (a2 * (by - qy) + b2 * (qy - ay) + q2 * (ay - by)) / d;
                    cz = (a2 * (qx - bx) + b2 * (ax - qx) + q2 * (bx - ax)) / d;
                    centred = true;
                }
            }
            for (int i = 0; i < 3; ++i) {
                snapNear(rz[i][0], rs, tol, true);
                snapNear(rz[i][1], zs, tol, false);
            }
            if (centred) {
                snapNear(cr, rs, tol, true);
                snapNear(cz, zs, tol, false);
                const double rho = (std::hypot(rz[1][0] - cr, rz[1][1] - cz) + std::hypot(rz[2][0] - cr, rz[2][1] - cz)) / 2;
                const double mx = rz[3][0] - cr, my = rz[3][1] - cz, ml = std::hypot(mx, my);
                if (ml > 1e-12) {
                    rz[3][0] = cr + mx / ml * rho;
                    rz[3][1] = cz + my / ml * rho;
                }
            }
            for (int i = 0; i < 4; ++i) {
                q[i] = o.Translated(gp_Vec(x) * rz[i][0] + gp_Vec(up) * rz[i][1]);
                if (i == 3)
                    break;
                rmax = std::max(rmax, rz[i][0]);
                zlo = std::min(zlo, rz[i][1]);
                zhi = std::max(zhi, rz[i][1]);
            }
            Section flatOne;
            flatOne.p = q[0];
            flatOne.a = q[1];
            flatOne.b = q[2];
            flatOne.m = q[3];
            flatOne.round = r->round;
            // Reaching out past the body (GB-052): over each face beside the edge, as far as that face goes, the
            // section goes a little way out of the body (and round the corner between), in one outline. Left
            // lying on the body's own faces, the tool's sides made OpenCascade's cut go wrong just past where
            // a round reaches the axis or two rounds meet (a 10 x 10 in cylinder's rims at r 127.01 mm kept a
            // quarter of the cylinder; at 9.9 in tall and r 5 in, half of it).
            TopoDS_Wire w;
            const gp_Vec u1(q[0], q[1]), u2(q[0], q[2]);
            const double l1 = u1.Magnitude(), l2 = u2.Magnitude();
            if (l1 > 1e-9 && l2 > 1e-9 && (r->reach[0] > 0 || r->reach[1] > 0)) {
                const gp_Vec v1 = u1 / l1, v2 = u2 / l2;
                const double pad = std::clamp(1e-3 * std::max(l1, l2), 1e-4, 0.1);
                const double e1 = std::min(r->reach[0], l1), e2 = std::min(r->reach[1], l2);
                std::vector<gp_Pnt> path{q[1]};
                const auto to = [&](const gp_Pnt &x) {
                    if (x.Distance(path.back()) > 1e-9)
                        path.push_back(x);
                };
                to(q[0].Translated(v1 * e1));
                to(q[0].Translated(v1 * e1 - v2 * pad));
                to(q[0].Translated(-(v1 + v2) * pad));
                to(q[0].Translated(v2 * e2 - v1 * pad));
                to(q[0].Translated(v2 * e2));
                to(q[2]);
                BRepBuilderAPI_MakeWire mk;
                for (std::size_t k = 1; k < path.size(); ++k)
                    mk.Add(BRepBuilderAPI_MakeEdge(path[k - 1], path[k]).Edge());
                if (r->round)
                    mk.Add(BRepBuilderAPI_MakeEdge(GC_MakeArcOfCircle(q[2], q[3], q[1]).Value()).Edge());
                else
                    mk.Add(BRepBuilderAPI_MakeEdge(q[2], q[1]).Edge());
                if (mk.IsDone())
                    w = mk.Wire();
            }
            if (w.IsNull())
                w = outline(flatOne);
            if (w.IsNull())
                return {};
            const TopoDS_Shape f = BRepBuilderAPI_MakeFace(w, true).Face();
            if (flat.IsNull()) {
                flat = f;
            } else {
                BRepAlgoAPI_Fuse join(flat, f);
                if (!join.IsDone())
                    return {};
                flat = join.Shape();
            }
        }
        if (rmax <= 1e-9 || zhi - zlo <= 1e-9)
            return {};
        // The near side of the axis only (a bevel bigger than the rim's radius reaches across it).
        const double pad = zhi - zlo + rmax;
        const gp_Pnt c0 = o.Translated(gp_Vec(up) * (zlo - pad)), c1 = c0.Translated(gp_Vec(x) * (rmax + pad)),
                     c2 = c1.Translated(gp_Vec(up) * (zhi - zlo + 2 * pad)), c3 = c0.Translated(gp_Vec(up) * (zhi - zlo + 2 * pad));
        const TopoDS_Shape side = BRepBuilderAPI_MakeFace(BRepBuilderAPI_MakePolygon(c0, c1, c2, c3, true).Wire(), true).Face();
        BRepAlgoAPI_Common keep(flat, side);
        if (!keep.IsDone())
            return {};
        ShapeUpgrade_UnifySameDomain one(keep.Shape(), true, true, true);
        one.Build();
        const TopoDS_Shape section = one.Shape();
        if (!TopExp_Explorer(section, TopAbs_FACE).More())
            return {};
        BRepPrimAPI_MakeRevol turn(section, axis, 2 * M_PI, true);
        if (!turn.IsDone())
            return {};
        return turn.Shape();
    } catch (Standard_Failure &) {
        return {};
    }
}

// Every wedge of `items` cut from (or added to) `shape`, all at once; failing that, one at a time, each
// that won't go left sharp.
TopoDS_Shape applyWedges(const TopoDS_Shape &original, TopoDS_Shape shape, const std::vector<const Item *> &items,
                         const Check &sound, const Boolean &boolean, const Log &log, Report &report, bool ranThrough) {
    struct Built { const Item *it; TopoDS_Shape solid; bool cut; };
    // Arcs of one circle, with the same bevel, that go all the way round between them (a rim in two halves):
    // one sweep right round, so no two sweeps meet end to end.
    std::vector<char> covered(items.size(), 0), whole(items.size(), 0);
    for (std::size_t i = 0; i < items.size(); ++i) {
        BRepAdaptor_Curve ci(items[i]->edge);
        if (covered[i] || ci.GetType() != GeomAbs_Circle)
            continue;
        const gp_Circ a = ci.Circle();
        double turn = ci.LastParameter() - ci.FirstParameter();
        std::vector<std::size_t> same;
        for (std::size_t j = i + 1; j < items.size(); ++j) {
            BRepAdaptor_Curve cj(items[j]->edge);
            if (covered[j] || cj.GetType() != GeomAbs_Circle)
                continue;
            const gp_Circ b = cj.Circle();
            const Item &x = *items[i], &y = *items[j];
            if (std::abs(a.Radius() - b.Radius()) < 1e-7 && a.Location().Distance(b.Location()) < 1e-7
                && a.Axis().Direction().IsParallel(b.Axis().Direction(), 1e-9) && x.kind == y.kind
                && std::abs(x.d1 - y.d1) < 1e-9 && std::abs(x.d2 - y.d2) < 1e-9 && std::abs(x.angle - y.angle) < 1e-9) {
                same.push_back(j);
                turn += cj.LastParameter() - cj.FirstParameter();
            }
        }
        if (!same.empty() && turn > 2 * M_PI - 1e-6) {
            whole[i] = 1;
            for (const std::size_t j : same)
                covered[j] = 1;
        }
    }
    std::vector<Built> built;
    std::vector<Ring> rings;
    rings.reserve(items.size());
    std::vector<const Item *> alongWith;     // swept with another arc of their circle, or another rim's bevel
    for (std::size_t i = 0; i < items.size(); ++i) {
        const Item *it = items[i];
        if (covered[i]) {
            alongWith.push_back(it);
            continue;
        }
        bool cut = true;
        std::string why;
        Ring ring;
        const TopoDS_Shape w = wedge(original, *it, cut, why, whole[i], &ring);
        if (w.IsNull()) {
            log("chamfer: edge " + std::to_string(it->id) + ": its bevel couldn't be swept: " + why);
            report.skipped.push_back(it->id);
            continue;
        }
        built.push_back({it, w, cut});
        rings.push_back(cut ? ring : Ring());
    }
    // Whole rims cut round one axis go as one tool (see ringsTool); the first carries it, the rest go along.
    {
        std::vector<char> merged(built.size(), 0);
        std::vector<Built> kept;
        for (std::size_t i = 0; i < built.size(); ++i) {
            if (merged[i])
                continue;
            std::vector<const Ring *> group;
            std::vector<std::size_t> members;
            if (rings[i].ok) {
                group.push_back(&rings[i]);
                for (std::size_t j = i + 1; j < built.size(); ++j)
                    if (!merged[j] && rings[j].ok && sameLine(rings[i].axis, rings[j].axis)) {
                        group.push_back(&rings[j]);
                        members.push_back(j);
                    }
            }
            const TopoDS_Shape tool = group.size() > 1 ? ringsTool(group) : TopoDS_Shape();
            if (tool.IsNull()) {
                kept.push_back(built[i]);
                continue;
            }
            kept.push_back({built[i].it, tool, true});
            for (const std::size_t j : members) {
                merged[j] = 1;
                alongWith.push_back(built[j].it);
            }
        }
        built = std::move(kept);
    }
    const auto apply = [&](const TopoDS_Shape &from, const std::vector<const Built *> &list) -> TopoDS_Shape {
        TopoDS_Shape now = from;
        for (const bool cut : {true, false}) {
            std::vector<TopoDS_Shape> tools;
            for (const Built *b : list)
                if (b->cut == cut)
                    tools.push_back(b->solid);
            if (tools.empty())
                continue;
            now = boolean(now, tools, cut);
            if (now.IsNull())
                return {};
        }
        return sound(now) ? now : TopoDS_Shape();
    };
    std::vector<const Built *> all;
    for (const Built &b : built)
        all.push_back(&b);
    if (all.empty())
        return shape;
    const auto volume = [](const TopoDS_Shape &s) {
        GProp_GProps g;
        if (!s.IsNull())
            BRepGProp::VolumeProperties(s, g);
        return g.Mass();
    };
    bool cutsOnly = true;
    for (const Built *b : all)
        cutsOnly = cutsOnly && b->cut;
    const double before = volume(shape);
    // Cuts that leave more than there was have gone wrong (OpenCascade, given a tool that swallows the whole
    // body, can hand back the tool's own outside as if it were what's left).
    const auto grew = [&](const TopoDS_Shape &s) { return cutsOnly && volume(s) > before * (1 + 1e-6) + 1e-9; };
    // A bevel that by itself swallows the whole body.
    const auto swallowed = [&] {
        for (const Built *b : all) {
            if (!b->cut)
                continue;
            BRepAlgoAPI_Common in(shape, b->solid);
            if (in.IsDone() && volume(in.Shape()) >= before * (1 - 1e-6))
                return true;
        }
        return false;
    };
    TopoDS_Shape done = apply(shape, all);
    if (!done.IsNull() && grew(done))
        done = {};
    if (done.IsNull() && cutsOnly && swallowed()) {
        log("chamfer: the bevels together take the whole body");
        report.consumed = true;
        return {};
    }
    if (done.IsNull() || !hasVolume(done)) {
        // Bevels that meet take the material between them evenly (owner, 2026-11-09: "allow both chamfers to
        // progress evenly together ... chamfer this cylinder into oblivion"): when together they take it all,
        // that's the answer, not one of them on its own.
        TopTools_ListOfShape from, cuts;
        from.Append(shape);
        for (const Built *b : all)
            if (b->cut)
                cuts.Append(b->solid);
        BRepAlgoAPI_Cut raw;
        raw.SetArguments(from);
        raw.SetTools(cuts);
        raw.Build();
        if (!cuts.IsEmpty() && raw.IsDone() && (!hasVolume(raw.Shape()) || (cutsOnly && grew(raw.Shape()) && swallowed()))) {
            log("chamfer: the bevels together take the whole body");
            report.consumed = true;
            return {};
        }
        done = {};
    }
    if (!done.IsNull()) {
        if (ranThrough) {
            for (const Built *b : all)
                report.ranThrough.push_back(b->it->id);
            for (const Item *it : alongWith)
                report.ranThrough.push_back(it->id);
        }
        return done;
    }
    for (const Item *it : alongWith)      // they went with a sweep that's tried on its own below
        (ranThrough ? report.ranThrough : report.skipped).push_back(it->id);
    for (const Built *b : all) {
        TopoDS_Shape one = apply(shape, {b});
        if (!one.IsNull() && b->cut && volume(one) > volume(shape) * (1 + 1e-6) + 1e-9)
            one = {};
        if (one.IsNull()) {
            log("chamfer: edge " + std::to_string(b->it->id) + ": its swept bevel wouldn't cut cleanly");
            report.skipped.push_back(b->it->id);
            continue;
        }
        shape = one;
        if (ranThrough)
            report.ranThrough.push_back(b->it->id);
    }
    return shape;
}

// OpenCascade's corner patches (faces made neither from a chamfered edge nor from a face of the body)
// filled in again, tangent to the faces round each. The shape back unchanged when that can't be done.
TopoDS_Shape blendCorners(const TopoDS_Shape &body, const TopoDS_Shape &chamfered, BRepBuilderAPI_MakeShape &mk,
                          const std::vector<const Item *> &items, const Check &sound, Report &report) {
    TopTools_IndexedMapOfShape known;
    for (const Item *it : items)
        for (const TopoDS_Shape &f : mk.Generated(it->edge))
            known.Add(f);
    for (TopExp_Explorer e(body, TopAbs_FACE); e.More(); e.Next()) {
        if (mk.IsDeleted(e.Current()))
            continue;
        const TopTools_ListOfShape &now = mk.Modified(e.Current());
        if (now.IsEmpty())
            known.Add(e.Current());
        for (const TopoDS_Shape &f : now)
            known.Add(f);
    }
    std::vector<TopoDS_Face> corners;
    TopTools_IndexedMapOfShape faces;
    TopExp::MapShapes(chamfered, TopAbs_FACE, faces);
    for (int i = 1; i <= faces.Extent(); ++i)
        if (!known.Contains(faces(i)))
            corners.push_back(TopoDS::Face(faces(i)));
    if (corners.empty())
        return chamfered;
    TopTools_IndexedDataMapOfShapeListOfShape edgeFaces;
    TopExp::MapShapesAndAncestors(chamfered, TopAbs_EDGE, TopAbs_FACE, edgeFaces);
    try {
        BRepBuilderAPI_Sewing sew(1e-4);
        TopTools_IndexedMapOfShape replaced;
        int filled = 0;
        std::vector<TopoDS_Face> fills;
        for (const TopoDS_Face &corner : corners) {
            BRepOffsetAPI_MakeFilling fill;
            int edges = 0;
            for (TopExp_Explorer e(corner, TopAbs_EDGE); e.More(); e.Next(), ++edges) {
                TopoDS_Face beside;
                for (const TopoDS_Shape &f : edgeFaces.FindFromKey(e.Current()))
                    if (!f.IsSame(corner))
                        beside = TopoDS::Face(f);
                if (beside.IsNull())
                    fill.Add(TopoDS::Edge(e.Current()), GeomAbs_C0);
                else
                    fill.Add(TopoDS::Edge(e.Current()), beside, GeomAbs_G1);
            }
            if (edges < 3)
                continue;
            fill.Build();
            if (!fill.IsDone())
                continue;
            fills.push_back(TopoDS::Face(fill.Shape()));
            replaced.Add(corner);
            ++filled;
        }
        if (!filled)
            return chamfered;
        for (int i = 1; i <= faces.Extent(); ++i)
            if (!replaced.Contains(faces(i)))
                sew.Add(faces(i));
        for (const TopoDS_Face &f : fills)
            sew.Add(f);
        sew.Perform();
        const TopoDS_Shape sewn = sew.SewedShape();
        TopoDS_Shell shell;
        int shells = 0;
        for (TopExp_Explorer e(sewn, TopAbs_SHELL); e.More(); e.Next(), ++shells)
            shell = TopoDS::Shell(e.Current());
        if (shells != 1)
            return chamfered;
        BRepBuilderAPI_MakeSolid make(shell);
        if (!make.IsDone())
            return chamfered;
        ShapeFix_Solid fix(make.Solid());
        fix.Perform();
        const TopoDS_Shape out = fix.Solid();
        if (!sound(out)) {
            report.notes.push_back("The Blend corners couldn't be built cleanly, so they are plain Chamfer corners.");
            return chamfered;
        }
        if (filled < int(corners.size()))
            report.notes.push_back(std::to_string(int(corners.size()) - filled) + " corner(s) couldn't be blended and stay plain Chamfer corners.");
        return out;
    } catch (Standard_Failure &) {
        report.notes.push_back("The Blend corners couldn't be built, so they are plain Chamfer corners.");
        return chamfered;
    }
}

} // namespace

bool section(const TopoDS_Shape &body, const Item &item, double out[9], double at) {
    const TopoDS_Face other = otherFace(body, item.edge, item.ref);
    if (other.IsNull())
        return false;
    BRepAdaptor_Curve curve(item.edge);
    Section s;
    at = std::clamp(at, 0.0, 1.0);
    if (!sectionAt(item, other, curve.FirstParameter() + (curve.LastParameter() - curve.FirstParameter()) * at, s))
        return false;
    const gp_Pnt pts[3] = {s.p, s.a, s.b};
    for (int i = 0; i < 3; ++i) {
        out[3 * i] = pts[i].X();
        out[3 * i + 1] = pts[i].Y();
        out[3 * i + 2] = pts[i].Z();
    }
    return true;
}

TopoDS_Shape makeExact(const TopoDS_Shape &body, const std::vector<Item> &items, Corner corner, const Check &sound,
                       const Boolean &boolean, const Log &log, Report &report, bool exactRims, bool *refused);

// A size meant to reach a rim's axis exactly (a 5 in chamfer or round on a 10 in circle) is taken as exactly
// the rim's radius when it is within the sketch solver's noise of it (GB-054, owner pt156u: the circle's solved
// radius was 127.000000000251 mm, so 5 in stopped 2.5e-10 mm short of the axis; OpenCascade can't build a cone
// that small and the chamfer failed, preview and Done alike; a hair past it a round lost the whole body).
TopoDS_Shape make(const TopoDS_Shape &body, const std::vector<Item> &items, Corner corner, const Check &sound,
                  const Boolean &boolean, const Log &log, Report &report) {
    std::vector<Item> snapped = items;
    for (Item &it : snapped) {
        BRepAdaptor_Curve curve(it.edge);
        if (curve.GetType() != GeomAbs_Circle || it.kind == Kind::Chord)
            continue;
        const double r = curve.Circle().Radius(), tol = std::max(1e-7, 1e-9 * r);
        for (double *d : {&it.d1, &it.d2})
            if (*d != r && std::abs(*d - r) <= tol) {
                log("chamfer: edge " + std::to_string(it.id) + ": size within " + std::to_string(std::abs(*d - r)) + " mm of its rim's radius, taken as it");
                *d = r;
            }
    }
    // Whole rims exactly first (GB-055): OpenCascade's own chamfer or round of a rim between two cones (a double
    // cone's equator) comes back as one B-spline face, not quite round (its centroid 0.17 mm off the axis on a
    // 10 in double cone) and too loose for a point's cut beside it; the turned run-through section is exact.
    // When that leaves more edges sharp than OpenCascade's would, OpenCascade's is used after all.
    bool refused = false;
    Report exact = report;
    TopoDS_Shape out = makeExact(body, snapped, corner, sound, boolean, log, exact, true, &refused);
    if (!refused || (!out.IsNull() && exact.skipped.empty())) {
        report = exact;
        return out;
    }
    Report loose = report;
    const TopoDS_Shape other = makeExact(body, snapped, corner, sound, boolean, log, loose, false, nullptr);
    if (!other.IsNull() && (out.IsNull() || loose.skipped.size() < exact.skipped.size())) {
        log("chamfer: the exact rims left edges sharp; OpenCascade's own taken");
        report = loose;
        return other;
    }
    report = exact;
    return out;
}

// B-spline (or other free-form) faces in a shape.
int freeFormFaces(const TopoDS_Shape &s) {
    int n = 0;
    for (TopExp_Explorer e(s, TopAbs_FACE); e.More(); e.Next()) {
        const GeomAbs_SurfaceType t = BRepAdaptor_Surface(TopoDS::Face(e.Current())).GetType();
        n += t == GeomAbs_BSplineSurface || t == GeomAbs_BezierSurface || t == GeomAbs_OffsetSurface || t == GeomAbs_OtherSurface;
    }
    return n;
}

TopoDS_Shape makeExact(const TopoDS_Shape &body, const std::vector<Item> &items, Corner corner, const Check &sound,
                       const Boolean &boolean, const Log &log, Report &report, bool exactRims, bool *refused) {
    const int freeBefore = exactRims ? freeFormFaces(body) : 0;
    std::vector<const Item *> all;
    for (const Item &it : items)
        all.push_back(&it);
    // OpenCascade's own result, taken only when sound and no bigger than the body when every edge is an outside
    // one (no smaller when every one is inside): two rounds that overlap on the face between them can come back
    // "sound" and half again as big as the body (GB-052: a 10 in cylinder 9.9 in tall, both rims at r 5 in,
    // gave 21.2 litres from 12.7; then they run through and meet, as they should).
    const auto volumeOf = [](const TopoDS_Shape &s) {
        GProp_GProps g;
        BRepGProp::VolumeProperties(s, g);
        return g.Mass();
    };
    const double before = volumeOf(body);
    std::vector<int> sides(items.size(), 0);   // +1 outside edge (cut), -1 inside (added), 0 unknown
    for (std::size_t i = 0; i < items.size(); ++i) {
        const TopoDS_Face other = otherFace(body, items[i].edge, items[i].ref);
        BRepAdaptor_Curve c(items[i].edge);
        Section m;
        if (!other.IsNull() && sectionAt(items[i], other, (c.FirstParameter() + c.LastParameter()) / 2, m)) {
            const TopAbs_State s = sideOf(body, m);
            sides[i] = s == TopAbs_IN ? 1 : s == TopAbs_OUT ? -1 : 0;
        }
    }
    const auto good = [&](const TopoDS_Shape &s, const std::vector<const Item *> &list) {
        if (s.IsNull() || !sound(s))
            return false;
        if (exactRims && freeFormFaces(s) > freeBefore
            && std::all_of(list.begin(), list.end(), [](const Item *it) {
                   BRepAdaptor_Curve c(it->edge);
                   return c.GetType() == GeomAbs_Circle;      // a rim, whole or in arcs (a seam splits a cone's)
               })) {
            if (refused)
                *refused = true;
            return false;                // a rim made free-form: run through instead (exact, see make)
        }
        int cuts = 0, adds = 0;
        for (const Item *it : list) {
            const int side = sides[std::size_t(it - items.data())];
            cuts += side > 0;
            adds += side < 0;
        }
        const double now = volumeOf(s), slack = 1e-7 * std::max(before, 1.0);
        if (cuts == int(list.size()) && now > before + slack)
            return false;
        if (adds == int(list.size()) && now < before - slack)
            return false;
        return true;
    };
    if (corner == Corner::Miter) {
        // Every bevel swept on until it meets the others: the corners come out as points.
        const TopoDS_Shape out = applyWedges(body, body, all, sound, boolean, log, report, false);
        return report.skipped.size() == items.size() ? TopoDS_Shape() : out;
    }
    // Swept, every one (GB-055): a point's cut then goes on exact faces (OpenCascade's own chamfer of two cones
    // comes back as loose B-spline faces a point's flat can't be cut cleanly through).
    if (std::any_of(items.begin(), items.end(), [](const Item &it) { return it.sweep; })) {
        log("chamfer: every edge run through (exact faces, for the points with them)");
        const TopoDS_Shape out = applyWedges(body, body, all, sound, boolean, log, report, true);
        return report.skipped.size() == items.size() ? TopoDS_Shape() : out;
    }
    // GB-063: an edge whose band runs off its faces runs through, never by OpenCascade's own (see leavesFaces).
    std::vector<const Item *> offFaces;
    for (const Item *it : all)
        if (leavesFaces(body, *it)) {
            offFaces.push_back(it);
            log("chamfer: edge " + std::to_string(it->id) + ": its band runs off the faces beside it (into a hole or a neighbouring face), so it runs through");
        }
    if (!offFaces.empty())
        all.erase(std::remove_if(all.begin(), all.end(), [&](const Item *it) { return std::find(offFaces.begin(), offFaces.end(), it) != offFaces.end(); }), all.end());
    std::unique_ptr<BRepFilletAPI_LocalOperation> mk;
    TopoDS_Shape done = all.empty() ? TopoDS_Shape() : native(body, all, &mk);
    if (offFaces.empty() && good(done, all)) {
        log("chamfer: all " + std::to_string(items.size()) + " edge(s) in one go");
        return corner == Corner::Blend ? blendCorners(body, done, *mk, all, sound, report) : done;
    }
    // Which edges OpenCascade can do: each alone first, then together, one more at a time.
    std::vector<const Item *> fits, runs = offFaces;
    std::vector<const Item *> together;
    TopoDS_Shape shape = body;
    if (!offFaces.empty() && !all.empty() && good(done, all)) {
        together = all;         // the rest went in one go
        shape = done;
    } else {
        for (const Item *it : all) {
            const TopoDS_Shape one = native(body, {it});
            (good(one, {it}) ? fits : runs).push_back(it);
        }
    }
    if (!fits.empty()) {
        done = native(body, fits, &mk);
        if (good(done, fits)) {
            together = fits;
            shape = done;
        } else {
            for (const Item *it : fits) {
                std::vector<const Item *> more = together;
                more.push_back(it);
                std::unique_ptr<BRepFilletAPI_LocalOperation> tried;
                const TopoDS_Shape s = native(body, more, &tried);
                if (good(s, more)) {
                    together = std::move(more);
                    shape = s;
                    mk = std::move(tried);
                } else {
                    runs.push_back(it);
                }
            }
        }
    }
    // A whole rim OpenCascade can do, round the same axis as one that must run through, runs through with it:
    // their bevels can meet, and OpenCascade's own cone cut again by a swept one round the same axis goes
    // wrong (owner, 2026-11-09: the disc's top bevel vanished once the two met).
    const auto rimAxis = [](const Item *it, gp_Ax1 &axis) {
        BRepAdaptor_Curve c(it->edge);
        if (c.GetType() != GeomAbs_Circle || std::abs(c.LastParameter() - c.FirstParameter() - 2 * M_PI) > 1e-6)
            return false;
        axis = c.Circle().Axis();
        return true;
    };
    bool moved = false;
    for (std::size_t i = 0; i < together.size();) {
        gp_Ax1 a, b;
        bool with = false;
        if (rimAxis(together[i], a))
            for (const Item *r : runs)
                with = with || (rimAxis(r, b) && sameLine(a, b));
        if (with) {
            runs.push_back(together[i]);
            together.erase(together.begin() + i);
            moved = true;
        } else {
            ++i;
        }
    }
    if (moved) {
        shape = body;
        mk.reset();
        if (!together.empty()) {
            const TopoDS_Shape s = native(body, together, &mk);
            if (good(s, together)) {
                shape = s;
            } else {
                runs.insert(runs.end(), together.begin(), together.end());
                together.clear();
                mk.reset();
            }
        }
    }
    log("chamfer: " + std::to_string(together.size()) + " edge(s) by OpenCascade's chamfer, " + std::to_string(runs.size()) +
        " run through");
    if (corner == Corner::Blend && !together.empty() && mk)
        shape = blendCorners(body, shape, *mk, together, sound, report);
    shape = applyWedges(body, shape, runs, sound, boolean, log, report, true);
    if (report.skipped.size() == items.size())
        return {};
    return shape;
}

// ---- Points (GB-053) ----

namespace {

struct Around {
    gp_Pnt p;
    gp_Vec n;                       // the point's axis, out of the body
    std::vector<gp_Vec> normals;    // the faces' outward normals there (a cone's: round its tip)
    std::vector<gp_Vec> ways;       // the edges from it, as unit vectors
    std::vector<double> lengths;    // and how long each is
    double c = 0;                   // the least n . n_i
    std::string why;
};

// A face's outward normal at `uv` (null when the surface closes to a point there).
bool normalAt(const BRepAdaptor_Surface &s, const TopoDS_Face &face, double u, double v, gp_Vec &n, gp_Vec *du = nullptr, gp_Vec *dv = nullptr) {
    gp_Pnt p;
    gp_Vec a, b;
    s.D1(u, v, p, a, b);
    if (du)
        *du = a;
    if (dv)
        *dv = b;
    n = a.Crossed(b);
    if (n.Magnitude() <= 1e-9 * (a.Magnitude() * b.Magnitude() + 1e-300) || n.Magnitude() < 1e-14)
        return false;
    n.Normalize();
    if (face.Orientation() == TopAbs_REVERSED)
        n.Reverse();
    return true;
}

// The faces' normals at the vertex: one per face; where the face closes to a point there, many, round it.
bool aroundOf(const TopoDS_Shape &body, const TopoDS_Vertex &v, Around &out) {
    out.p = BRep_Tool::Pnt(v);
    TopTools_IndexedDataMapOfShapeListOfShape vf, ve;
    TopExp::MapShapesAndAncestors(body, TopAbs_VERTEX, TopAbs_FACE, vf);
    TopExp::MapShapesAndAncestors(body, TopAbs_VERTEX, TopAbs_EDGE, ve);
    if (!vf.Contains(v)) {
        out.why = "the point isn't on a face";
        return false;
    }
    TopTools_IndexedMapOfShape faces;
    for (const TopoDS_Shape &f : vf.FindFromKey(v))
        faces.Add(f);
    for (int i = 1; i <= faces.Extent(); ++i) {
        const TopoDS_Face face = TopoDS::Face(faces(i));
        const gp_Pnt2d uv = BRep_Tool::Parameters(v, face);
        BRepAdaptor_Surface s(face);
        gp_Vec n, du, dv;
        if (normalAt(s, face, uv.X(), uv.Y(), n, &du, &dv)) {
            out.normals.push_back(n);
            continue;
        }
        // Closes to a point here (a cone's tip): normals a little way in, all round.
        double u1, u2, v1, v2;
        BRepTools::UVBounds(face, u1, u2, v1, v2);
        BRepTopAdaptor_FClass2d inside(face, 1e-9);
        const bool alongU = du.Magnitude() < dv.Magnitude();     // u runs round the point
        bool got = false;
        for (const double sign : {1.0, -1.0}) {
            const double step = 1e-3 * (alongU ? v2 - v1 : u2 - u1) * sign;
            const gp_Pnt2d mid = alongU ? gp_Pnt2d((u1 + u2) / 2, uv.Y() + step) : gp_Pnt2d(uv.X() + step, (v1 + v2) / 2);
            if (inside.Perform(mid) != TopAbs_IN)
                continue;
            for (int k = 0; k < 24; ++k) {
                const double w = (k + 0.5) / 24;
                gp_Vec m;
                if (alongU ? normalAt(s, face, u1 + (u2 - u1) * w, uv.Y() + step, m) : normalAt(s, face, uv.X() + step, v1 + (v2 - v1) * w, m))
                    out.normals.push_back(m);
            }
            got = true;
            break;
        }
        if (!got) {
            out.why = "a face round the point can't say which way it faces";
            return false;
        }
    }
    if (ve.Contains(v))
        for (const TopoDS_Shape &e : ve.FindFromKey(v)) {
            const TopoDS_Edge edge = TopoDS::Edge(e);
            if (BRep_Tool::Degenerated(edge))
                continue;
            BRepAdaptor_Curve c(edge);
            const double t = BRep_Tool::Parameter(v, edge);
            gp_Pnt q;
            gp_Vec d;
            c.D1(t, q, d);
            if (d.Magnitude() < 1e-12)
                continue;
            d.Normalize();
            if (std::abs(t - c.LastParameter()) < std::abs(t - c.FirstParameter()))
                d.Reverse();
            out.ways.push_back(d);
            out.lengths.push_back(GCPnts_AbscissaPoint::Length(c));
        }
    if (out.normals.empty()) {
        out.why = "no face round the point";
        return false;
    }
    // n . n_i the same for every face: least squares of N n = 1 (exact for three faces), then of unit length.
    double a[3][3] = {{0, 0, 0}, {0, 0, 0}, {0, 0, 0}}, b[3] = {0, 0, 0};
    for (const gp_Vec &m : out.normals) {
        const double x[3] = {m.X(), m.Y(), m.Z()};
        for (int r = 0; r < 3; ++r) {
            b[r] += x[r];
            for (int k = 0; k < 3; ++k)
                a[r][k] += x[r] * x[k];
        }
    }
    const double trace = a[0][0] + a[1][1] + a[2][2];
    for (int r = 0; r < 3; ++r)
        a[r][r] += 1e-12 * trace;
    const auto det = [](const double m[3][3]) {
        return m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) - m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0])
             + m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
    };
    const double d = det(a);
    if (std::abs(d) < 1e-300) {
        out.why = "the faces round the point lie flat";
        return false;
    }
    double x[3];
    for (int k = 0; k < 3; ++k) {
        double m[3][3];
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 3; ++c)
                m[r][c] = c == k ? b[r] : a[r][c];
        x[k] = det(m) / d;
    }
    out.n = gp_Vec(x[0], x[1], x[2]);
    if (out.n.Magnitude() < 1e-12) {
        out.why = "the faces round the point lie flat";
        return false;
    }
    out.n.Normalize();
    out.c = 1;
    for (const gp_Vec &m : out.normals)
        out.c = std::min(out.c, out.n.Dot(m));
    if (out.c <= 1e-6) {
        out.why = "it's an inside corner (the faces round it don't close in on it)";
        return false;
    }
    if (out.c >= 1 - 1e-9) {
        out.why = "the point isn't sharp (the faces round it are flat there)";
        return false;
    }
    return true;
}

TopoDS_Vertex vertexNear(const TopoDS_Shape &body, const gp_Pnt &at) {
    TopTools_IndexedMapOfShape all;
    TopExp::MapShapes(body, TopAbs_VERTEX, all);
    Bnd_Box box;
    BRepBndLib::Add(body, box);
    const double tol = 1e-6 * std::sqrt(box.SquareExtent()) + 1e-6;
    TopoDS_Vertex best;
    double gap = tol;
    for (int i = 1; i <= all.Extent(); ++i) {
        const TopoDS_Vertex v = TopoDS::Vertex(all(i));
        const double g = BRep_Tool::Pnt(v).Distance(at);
        if (g <= gap) {
            gap = g;
            best = v;
        }
    }
    return best;
}

// The point's cut: the flat (or the round's touching circle) `depth` in from the point along n; how far out it
// reaches round n, from where it crosses each edge. `over`: past the end of an edge (it runs on through).
struct Cut {
    double depth = 0, reach = 0, r = 0;
    gp_Pnt centre;
    bool round = false, over = false;
};
Cut cutOf(const Around &a, const Point &pt) {
    Cut c;
    c.round = rounds(pt.kind);
    if (c.round) {
        // The ball touches the faces where n . n_i is least; Chord: the circle it touches them in, its diameter.
        c.r = pt.kind == Kind::Chord ? pt.size / (2 * std::sqrt(std::max(1e-12, 1 - a.c * a.c))) : pt.size;
        const double s = c.r / a.c;
        c.centre = a.p.Translated(-a.n * s);
        c.depth = s - c.r * a.c;
    } else {
        c.depth = pt.size;
    }
    for (std::size_t i = 0; i < a.ways.size(); ++i) {
        const double down = -a.ways[i].Dot(a.n);
        if (down <= 1e-9)
            continue;
        const double t = c.depth / down;
        c.reach = std::max(c.reach, t * std::sqrt(std::max(0.0, 1 - down * down)));
        c.over = c.over || t > a.lengths[i] * (1 + 1e-9);
    }
    if (c.reach <= 0)
        c.reach = c.depth * std::sqrt(std::max(0.0, 1 - a.c * a.c)) / std::max(a.c, 1e-6);
    return c;
}

TopoDS_Shape toolOf(const Around &a, const Cut &c) {
    const double radius = 1.25 * c.reach + 1e-3, height = 2 * c.depth + c.reach + 1e-3;
    const gp_Pnt base = a.p.Translated(-a.n * c.depth);
    const TopoDS_Shape slab = BRepPrimAPI_MakeCylinder(gp_Ax2(base, gp_Dir(a.n)), radius, height).Shape();
    if (!c.round)
        return slab;
    BRepAlgoAPI_Cut less(slab, BRepPrimAPI_MakeSphere(c.centre, c.r).Shape());
    return less.IsDone() ? less.Shape() : TopoDS_Shape();
}

double volumeOf(const TopoDS_Shape &s) {
    GProp_GProps g;
    if (!s.IsNull())
        BRepGProp::VolumeProperties(s, g);
    return g.Mass();
}

} // namespace

bool isTip(const TopoDS_Shape &body, const TopoDS_Shape &vertex) {
    try {
        const TopoDS_Vertex v = TopoDS::Vertex(vertex);
        TopTools_IndexedDataMapOfShapeListOfShape ve;
        TopExp::MapShapesAndAncestors(body, TopAbs_VERTEX, TopAbs_EDGE, ve);
        if (!ve.Contains(v))
            return false;
        bool closes = false;
        for (const TopoDS_Shape &e : ve.FindFromKey(v))
            closes = closes || BRep_Tool::Degenerated(TopoDS::Edge(e));
        if (!closes)
            return false;
        Around a;
        if (!aroundOf(body, v, a))
            return false;
        // Sharp: the normals round it don't all agree (a sphere's pole: they do).
        for (const gp_Vec &m : a.normals)
            if (m.Angle(a.normals.front()) > 1e-3)
                return true;
        return false;
    } catch (Standard_Failure &) {
        return false;
    }
}

bool pointSection(const TopoDS_Shape &body, const Point &pt, double out[9]) {
    try {
        const TopoDS_Vertex v = vertexNear(body, gp_Pnt(pt.at[0], pt.at[1], pt.at[2]));
        Around a;
        if (v.IsNull() || !aroundOf(body, v, a))
            return false;
        const Cut c = cutOf(a, pt);
        // The round's tip (a fillet) or the flat's middle (a chamfer), on n.
        const gp_Pnt spot = c.round ? c.centre.Translated(a.n * c.r) : a.p.Translated(-a.n * c.depth);
        const gp_Pnt pts[3] = {a.p, spot, spot};
        for (int i = 0; i < 3; ++i) {
            out[3 * i] = pts[i].X();
            out[3 * i + 1] = pts[i].Y();
            out[3 * i + 2] = pts[i].Z();
        }
        return true;
    } catch (Standard_Failure &) {
        return false;
    }
}

TopoDS_Shape points(const TopoDS_Shape &body, const std::vector<Point> &pts, const Check &sound, const Boolean &boolean,
                    const Log &log, Report &report, const TopoDS_Shape &around) {
    struct Built { const Point *pt; TopoDS_Shape tool; };
    std::vector<Built> built;
    const TopoDS_Shape &on = around.IsNull() ? body : around;
    for (const Point &pt : pts) {
        try {
            const TopoDS_Vertex v = vertexNear(on, gp_Pnt(pt.at[0], pt.at[1], pt.at[2]));
            Around a;
            if (v.IsNull()) {
                log("chamfer: point " + std::to_string(pt.id) + ": not on the body");
                report.skipped.push_back(pt.id);
                continue;
            }
            if (!aroundOf(on, v, a)) {
                log("chamfer: point " + std::to_string(pt.id) + ": " + a.why);
                report.skipped.push_back(pt.id);
                report.notes.push_back("A point couldn't be done: " + a.why + ".");
                continue;
            }
            const Cut c = cutOf(a, pt);
            const TopoDS_Shape tool = toolOf(a, c);
            if (tool.IsNull()) {
                report.skipped.push_back(pt.id);
                continue;
            }
            if (c.over)
                report.ranThrough.push_back(pt.id);
            built.push_back({&pt, tool});
        } catch (Standard_Failure &) {
            report.skipped.push_back(pt.id);
        }
    }
    if (built.empty())
        return body;
    const double before = volumeOf(body);
    const auto apply = [&](const TopoDS_Shape &from, const std::vector<const Built *> &list) -> TopoDS_Shape {
        std::vector<TopoDS_Shape> tools;
        for (const Built *b : list)
            tools.push_back(b->tool);
        const TopoDS_Shape now = boolean(from, tools, true);
        if (now.IsNull() || !sound(now))
            return {};
        const double v = volumeOf(now);
        if (v > volumeOf(from) * (1 + 1e-7) + 1e-9)
            return {};
        return now;
    };
    std::vector<const Built *> all;
    for (const Built &b : built)
        all.push_back(&b);
    TopoDS_Shape out = apply(body, all);
    if (out.IsNull()) {
        out = body;
        for (const Built *b : all) {
            const TopoDS_Shape s = apply(out, {b});
            if (s.IsNull()) {
                log("chamfer: point " + std::to_string(b->pt->id) + ": its cut wouldn't go cleanly");
                report.skipped.push_back(b->pt->id);
            } else {
                out = s;
            }
        }
    }
    if (volumeOf(out) < before * 1e-9) {
        report.consumed = true;
        return {};
    }
    return out;
}

} // namespace chamfer

