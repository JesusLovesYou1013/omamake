#include "engine/heavy/taper.h"

#include <AdvApprox_ApproxAFunction.hxx>
#include <AdvApprox_DichoCutting.hxx>
#include <AdvApprox_EvaluatorFunction.hxx>
#include <AdvApprox_PrefAndRec.hxx>
#include <BOPAlgo_CheckerSI.hxx>
#include <TopTools_ListOfShape.hxx>
#include <BRepAdaptor_Surface.hxx>
#include <BRepBuilderAPI_Transform.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <BRepLib.hxx>
#include <BRepTools_WireExplorer.hxx>
#include <BRep_Builder.hxx>
#include <BRep_Tool.hxx>
#include <GeomAPI.hxx>
#include <GeomAPI_Interpolate.hxx>
#include <Geom2dAPI_Interpolate.hxx>
#include <TColgp_HArray1OfPnt.hxx>
#include <TColgp_HArray1OfPnt2d.hxx>
#include <Geom2d_BSplineCurve.hxx>
#include <Geom2d_Circle.hxx>
#include <Geom2d_Line.hxx>
#include <Geom2d_TrimmedCurve.hxx>
#include <Geom_BSplineCurve.hxx>
#include <Geom_BSplineSurface.hxx>
#include <Geom_Circle.hxx>
#include <Geom_ConicalSurface.hxx>
#include <Geom_Line.hxx>
#include <Geom_Plane.hxx>
#include <Geom_TrimmedCurve.hxx>
#include <ShapeFix_Shell.hxx>
#include <Standard_Failure.hxx>
#include <TColgp_Array1OfPnt.hxx>
#include <TColgp_Array1OfPnt2d.hxx>
#include <TColgp_Array2OfPnt.hxx>
#include <TColStd_Array1OfInteger.hxx>
#include <TColStd_Array1OfReal.hxx>
#include <TColStd_HArray1OfInteger.hxx>
#include <TColStd_HArray1OfReal.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Shell.hxx>
#include <TopoDS_Solid.hxx>
#include <TopoDS_Wire.hxx>
#include <gp_Ax3.hxx>
#include <gp_Circ2d.hxx>
#include <gp_Lin2d.hxx>
#include <gp_Pln.hxx>
#include <gp_Trsf.hxx>

#include <algorithm>
#include <memory>
#include <cmath>
#include <vector>

namespace taper {

namespace {

constexpr double kTol = 1e-7;    // how far an approximated surface or corner may stray, mm
constexpr double kSmooth = 1e-7; // sides turning by less than this at a corner (radians) carry on into each other

enum class Kind { Line, Circle, Other };

// One edge of the outline, in the face's own frame (the face at z = 0, travelling up +z).
struct Side {
    TopoDS_Edge edge;           // as the wire runs
    Handle(Geom2d_Curve) curve; // flat; its parameter is the edge's own
    Kind kind = Kind::Other;
    double out = 1;             // the outline grows along the curve's right-hand normal (+1) or its left (-1)
    double run = 1;             // the wire runs along the parameter (+1) or against it (-1)
    double first = 0, last = 0; // the edge's range
    double start = 0, end = 0;  // parameter where the run starts and ends
    double vScale = 1;          // the side surface's v per mm of height (its u is the curve's parameter)
    Handle(Geom_Surface) surface;
    TopoDS_Edge top;
    Handle(Geom2d_Curve) topFlat; // the top edge on the top face
};

// A corner running up from where one side ends and the next starts.
struct Corner {
    TopoDS_Edge edge;                       // bottom corner to top corner, its parameter the height
    Handle(Geom2d_Curve) onBefore, onAfter; // along it on each side's surface: (curve parameter, v)
    TopoDS_Vertex top;
    gp_Pnt2d topAt;
    double before = 0, after = 0;           // each side's curve parameter at the top
};

// Point and derivative at `u` of `curve` moved `delta` along its right-hand normal (C' turned a quarter
// clockwise, the way Geom2d_OffsetCurve moves).
bool grown(const Handle(Geom2d_Curve) &curve, double u, double delta, gp_Pnt2d &p, gp_Vec2d &d1) {
    gp_Vec2d v1, v2;
    curve->D2(u, p, v1, v2);
    const gp_Vec2d w(v1.Y(), -v1.X()), dw(v2.Y(), -v2.X());
    const double len = w.Magnitude();
    if (len < 1e-14)
        return false;
    const gp_Vec2d n = w / len;
    p.Translate(n * delta);
    d1 = v1 + (dw - n * n.Dot(dw)) * (delta / len);
    return true;
}

gp_Vec2d rightNormal(const Handle(Geom2d_Curve) &curve, double u) {
    gp_Pnt2d p;
    gp_Vec2d v1;
    curve->D1(u, p, v1);
    return gp_Vec2d(v1.Y(), -v1.X()).Normalized();
}

// A spline or ellipse side's bottom outline and its grown top one, as one function of the curve's
// parameter, so the ruled surface between them pairs each bottom point with its own top point.
class SideFunction : public AdvApprox_EvaluatorFunction {
public:
    SideFunction(const Handle(Geom2d_Curve) &curve, double delta, double height)
        : m_curve(curve), m_delta(delta), m_height(height) {}
    void Evaluate(Standard_Integer *dimension, Standard_Real[2], Standard_Real *u, Standard_Integer *derivative,
                  Standard_Real *result, Standard_Integer *error) override {
        *error = 0;
        if (*dimension != 6 || *derivative > 1) {
            *error = 1;
            return;
        }
        gp_Pnt2d p, q;
        gp_Vec2d dp, dq;
        m_curve->D1(*u, p, dp);
        if (!grown(m_curve, *u, m_delta, q, dq)) {
            *error = 1;
            return;
        }
        const double values[6] = {p.X(), p.Y(), 0, q.X(), q.Y(), m_height};
        const double slopes[6] = {dp.X(), dp.Y(), 0, dq.X(), dq.Y(), 0};
        std::copy_n(*derivative == 0 ? values : slopes, 6, result);
    }

private:
    Handle(Geom2d_Curve) m_curve;
    double m_delta, m_height;
};

// Where `a` (near its parameter s) and `b` (near r) meet once both are grown for height t. Newton on
// the two grown curves; s and r carry the answer.
bool meet(const Side &a, const Side &b, double grow, double t, double &s, double &r, gp_Pnt2d &at) {
    const double da = a.out * grow * t, db = b.out * grow * t;
    for (int i = 0; i < 50; ++i) {
        gp_Pnt2d p, q;
        gp_Vec2d dp, dq;
        if (!grown(a.curve, s, da, p, dp) || !grown(b.curve, r, db, q, dq))
            return false;
        const double fx = p.X() - q.X(), fy = p.Y() - q.Y();
        if (std::hypot(fx, fy) < 1e-12 * (1 + std::abs(p.X()) + std::abs(p.Y()))) {
            at = p;
            return true;
        }
        // dp ds - dq dr = -f
        const double det = -dp.X() * dq.Y() + dq.X() * dp.Y();
        if (std::abs(det) < 1e-300)
            return false;
        s += (fx * dq.Y() - dq.X() * fy) / det;
        r += (dp.X() * -fy + fx * dp.Y()) / det;
    }
    return false;
}

// From the answer at `t0` to the one at `t1`, in steps small enough for Newton to keep to the same
// meeting point.
bool track(const Side &a, const Side &b, double grow, double t0, double t1, double &s, double &r, gp_Pnt2d &at) {
    double t = t0, step = t1 - t0;
    while (std::abs(t1 - t) > 1e-15 * (1 + std::abs(t1))) {
        if (std::abs(step) > std::abs(t1 - t))
            step = t1 - t;
        double ts = s, tr = r;
        gp_Pnt2d tat;
        if (meet(a, b, grow, t + step, ts, tr, tat)) {
            t += step;
            s = ts;
            r = tr;
            at = tat;
            step *= 2;
        } else {
            step /= 2;
            if (std::abs(step) < 1e-9 * std::abs(t1 - t0))
                return false;
        }
    }
    return true;
}

Handle(Geom_BSplineCurve) line3d(const gp_Pnt &p, const gp_Pnt &q, double t1) {
    TColgp_Array1OfPnt poles(1, 2);
    poles(1) = p;
    poles(2) = q;
    TColStd_Array1OfReal knots(1, 2);
    knots(1) = 0;
    knots(2) = t1;
    TColStd_Array1OfInteger mults(1, 2);
    mults.Init(2);
    return new Geom_BSplineCurve(poles, knots, mults, 1);
}

Handle(Geom2d_BSplineCurve) line2d(const gp_Pnt2d &p, const gp_Pnt2d &q, double t1) {
    TColgp_Array1OfPnt2d poles(1, 2);
    poles(1) = p;
    poles(2) = q;
    TColStd_Array1OfReal knots(1, 2);
    knots(1) = 0;
    knots(2) = t1;
    TColStd_Array1OfInteger mults(1, 2);
    mults.Init(2);
    return new Geom2d_BSplineCurve(poles, knots, mults, 1);
}

// The corner from where `a` ends to where `b` starts, all the way up: sampled where the two grown
// outlines meet, then a curve through the samples, with more samples until the curve and its trace on
// each side surface stay within kTol everywhere between them.
bool makeCorner(const Side &a, const Side &b, double grow, double height, Corner &corner, Handle(Geom_Curve) &up,
                std::string &why) {
    const gp_Vec2d na = rightNormal(a.curve, a.end) * a.out, nb = rightNormal(b.curve, b.start) * b.out;
    const double turn = std::atan2(na.Crossed(nb), na.Dot(nb));
    gp_Pnt2d bottom;
    gp_Vec2d unused;
    a.curve->D1(a.end, bottom, unused);
    if (std::abs(turn) < kSmooth) {
        // The sides carry on into each other: the corner is the one straight line both share.
        const gp_Vec2d n = (na + nb).Normalized();
        corner.topAt = bottom.Translated(n * grow * height);
        corner.before = a.end;
        corner.after = b.start;
        up = line3d(gp_Pnt(bottom.X(), bottom.Y(), 0), gp_Pnt(corner.topAt.X(), corner.topAt.Y(), height), height);
        corner.onBefore = line2d(gp_Pnt2d(a.end, 0), gp_Pnt2d(a.end, a.vScale * height), height);
        corner.onAfter = line2d(gp_Pnt2d(b.start, 0), gp_Pnt2d(b.start, b.vScale * height), height);
        return true;
    }
    if (std::abs(std::abs(turn) - M_PI) < 1e-6) {
        why = "the outline doubles back on itself at a corner";
        return false;
    }
    for (int n = 1; n <= 512; n *= 2) {
        std::vector<double> t(n + 1), s(n + 1), r(n + 1);
        std::vector<gp_Pnt2d> at(n + 1);
        s[0] = a.end;
        r[0] = b.start;
        at[0] = bottom;
        bool ok = true;
        for (int k = 1; k <= n && ok; ++k) {
            t[k] = height * k / n;
            s[k] = s[k - 1];
            r[k] = r[k - 1];
            ok = track(a, b, grow, t[k - 1], t[k], s[k], r[k], at[k]);
        }
        if (!ok) {
            why = "two sides of the outline never meet at a corner";
            return false;
        }
        Handle(Geom_BSplineCurve) curve;
        Handle(Geom2d_BSplineCurve) onA, onB;
        if (n == 1) {
            curve = line3d(gp_Pnt(at[0].X(), at[0].Y(), 0), gp_Pnt(at[1].X(), at[1].Y(), height), height);
            onA = line2d(gp_Pnt2d(s[0], 0), gp_Pnt2d(s[1], a.vScale * height), height);
            onB = line2d(gp_Pnt2d(r[0], 0), gp_Pnt2d(r[1], b.vScale * height), height);
        } else {
            Handle(TColgp_HArray1OfPnt) points = new TColgp_HArray1OfPnt(1, n + 1);
            Handle(TColgp_HArray1OfPnt2d) pa = new TColgp_HArray1OfPnt2d(1, n + 1), pb = new TColgp_HArray1OfPnt2d(1, n + 1);
            Handle(TColStd_HArray1OfReal) params = new TColStd_HArray1OfReal(1, n + 1);
            for (int k = 0; k <= n; ++k) {
                points->SetValue(k + 1, gp_Pnt(at[k].X(), at[k].Y(), t[k]));
                pa->SetValue(k + 1, gp_Pnt2d(s[k], a.vScale * t[k]));
                pb->SetValue(k + 1, gp_Pnt2d(r[k], b.vScale * t[k]));
                params->SetValue(k + 1, t[k]);
            }
            GeomAPI_Interpolate i3(points, params, false, 1e-12);
            Geom2dAPI_Interpolate ia(pa, params, false, 1e-12), ib(pb, params, false, 1e-12);
            i3.Perform();
            ia.Perform();
            ib.Perform();
            if (!i3.IsDone() || !ia.IsDone() || !ib.IsDone())
                continue;
            curve = i3.Curve();
            onA = ia.Curve();
            onB = ib.Curve();
        }
        // Between every pair of samples: the true meeting point against the curve, and the side
        // parameters against the traces (as a distance along each side).
        double worst = 0;
        for (int k = 0; k < n && ok; ++k) {
            const double tm = (t[k] + t[k + 1]) / 2;
            double sm = (s[k] + s[k + 1]) / 2, rm = (r[k] + r[k + 1]) / 2;
            gp_Pnt2d atm;
            ok = meet(a, b, grow, tm, sm, rm, atm);
            if (!ok)
                break;
            const gp_Pnt c = curve->Value(tm);
            gp_Pnt2d p;
            gp_Vec2d da, db;
            a.curve->D1(sm, p, da);
            b.curve->D1(rm, p, db);
            worst = std::max({worst, c.Distance(gp_Pnt(atm.X(), atm.Y(), tm)),
                              std::abs(onA->Value(tm).X() - sm) * da.Magnitude(),
                              std::abs(onB->Value(tm).X() - rm) * db.Magnitude()});
        }
        if (!ok || worst > kTol)
            continue;
        up = curve;
        corner.onBefore = onA;
        corner.onAfter = onB;
        corner.topAt = at[n];
        corner.before = s[n];
        corner.after = r[n];
        return true;
    }
    why = "a corner's sides couldn't be followed up to the top";
    return false;
}

// The side's surface, its u the curve's parameter over [lo, hi] and its v rising with the height.
Handle(Geom_Surface) sideSurface(Side &side, double grow, double height, double lo, double hi, std::string &why) {
    const double delta = side.out * grow * height; // along the right-hand normal at the top
    if (side.kind == Kind::Line) {
        const gp_Lin2d line = Handle(Geom2d_Line)::DownCast(side.curve)->Lin2d();
        const gp_Vec2d n = rightNormal(side.curve, 0) * side.out;
        const gp_Vec rise(n.X() * grow, n.Y() * grow, 1); // per mm of height
        side.vScale = rise.Magnitude();
        const gp_Dir x(line.Direction().X(), line.Direction().Y(), 0), y(rise);
        return new Geom_Plane(gp_Ax3(gp_Pnt(line.Location().X(), line.Location().Y(), 0), x.Crossed(y), x));
    }
    if (side.kind == Kind::Circle) {
        const gp_Circ2d circle = Handle(Geom2d_Circle)::DownCast(side.curve)->Circ2d();
        // Away from the centre the outline grows by +grow, towards it by -grow.
        gp_Pnt2d p;
        gp_Vec2d unused;
        side.curve->D1((side.first + side.last) / 2, p, unused);
        const double away = gp_Vec2d(circle.Location(), p).Dot(rightNormal(side.curve, (side.first + side.last) / 2) * side.out) > 0 ? 1 : -1;
        const double radius = circle.Radius(), topRadius = radius + away * grow * height;
        if (topRadius < 1e-6) {
            why = "the taper is too steep: an arc would shrink to nothing before the top";
            return {};
        }
        const double a = std::atan(away * grow);
        side.vScale = 1 / std::cos(a);
        const gp_Dir x(circle.XAxis().Direction().X(), circle.XAxis().Direction().Y(), 0),
            y(circle.YAxis().Direction().X(), circle.YAxis().Direction().Y(), 0);
        gp_Ax3 frame(gp_Pnt(circle.Location().X(), circle.Location().Y(), 0), gp::DZ(), x);
        if (frame.YDirection().Dot(y) < 0)
            frame.YReverse();
        return new Geom_ConicalSurface(frame, a, radius);
    }
    side.vScale = 1 / height;
    Handle(TColStd_HArray1OfReal) none, tolerance = new TColStd_HArray1OfReal(1, 2);
    tolerance->Init(kTol / 2);
    SideFunction function(side.curve, delta, height);
    const GeomAbs_Shape smooth = side.curve->Continuity() >= GeomAbs_C2 ? GeomAbs_C1 : GeomAbs_C0;
    std::vector<double> knots;
    if (auto spline = Handle(Geom2d_BSplineCurve)::DownCast(side.curve))
        for (int i = 1; i <= spline->NbKnots(); ++i)
            if (spline->Knot(i) > lo && spline->Knot(i) < hi)
                knots.push_back(spline->Knot(i));
    std::unique_ptr<AdvApprox_ApproxAFunction> approx;
    if (knots.empty()) {
        AdvApprox_DichoCutting halves;
        approx = std::make_unique<AdvApprox_ApproxAFunction>(0, 0, 2, none, none, tolerance, lo, hi, smooth, 14, 10000, function, halves);
    } else {
        TColStd_Array1OfReal cuts(1, int(knots.size()));
        for (size_t i = 0; i < knots.size(); ++i)
            cuts(int(i) + 1) = knots[i];
        AdvApprox_PrefAndRec atKnots(cuts, cuts);
        approx = std::make_unique<AdvApprox_ApproxAFunction>(0, 0, 2, none, none, tolerance, lo, hi, smooth, 14, 10000, function, atKnots);
    }
    if (!approx->HasResult() || approx->MaxError(3, 1) > kTol || approx->MaxError(3, 2) > kTol) {
        why = "a curved side's leaning surface couldn't be made";
        return {};
    }
    const int count = approx->NbPoles();
    TColgp_Array1OfPnt bottom(1, count), top(1, count);
    approx->Poles(1, bottom);
    approx->Poles(2, top);
    TColgp_Array2OfPnt poles(1, count, 1, 2);
    for (int i = 1; i <= count; ++i) {
        poles(i, 1) = bottom(i);
        poles(i, 2) = top(i);
    }
    TColStd_Array1OfReal vKnots(1, 2);
    vKnots(1) = 0;
    vKnots(2) = 1;
    TColStd_Array1OfInteger vMults(1, 2);
    vMults.Init(2);
    return new Geom_BSplineSurface(poles, approx->Knots()->Array1(), vKnots, approx->Multiplicities()->Array1(), vMults,
                                   approx->Degree(), 1);
}

} // namespace

TopoDS_Shape prism(const TopoDS_Face &given, const gp_Vec &travel, double grow, std::string &why) {
    const double height = travel.Magnitude();
    if (height < 1e-9 || std::abs(grow) < 1e-12) {
        why = "nothing to taper";
        return {};
    }
    BRepAdaptor_Surface plane(given);
    if (plane.GetType() != GeomAbs_Plane) {
        why = "only a flat face can be tapered";
        return {};
    }
    const gp_Dir up(travel);
    const gp_Pln pln = plane.Plane();
    if (std::abs(pln.Axis().Direction().Dot(up)) < 1 - 1e-9) {
        why = "a taper must travel square to the face";
        return {};
    }
    // Everything is made in the face's own frame and put back at the end.
    const gp_Ax3 frame(pln.Location(), up, pln.XAxis().Direction());
    gp_Trsf toFrame;
    toFrame.SetTransformation(frame);
    const TopoDS_Face face = TopoDS::Face(BRepBuilderAPI_Transform(given, toFrame, true).Shape());
    // The face's outward normal up or down: each wire keeps the face on its left seen from it.
    const double facing = (BRepAdaptor_Surface(face).Plane().Axis().Direction().Z() > 0) == (face.Orientation() != TopAbs_REVERSED) ? 1 : -1;

    std::vector<std::vector<Side>> wires;
    for (TopExp_Explorer w(face, TopAbs_WIRE); w.More(); w.Next()) {
        std::vector<Side> sides;
        for (BRepTools_WireExplorer e(TopoDS::Wire(w.Current()), face); e.More(); e.Next()) {
            Side side;
            side.edge = e.Current();
            if (BRep_Tool::Degenerated(side.edge))
                continue;
            double f, l;
            Handle(Geom_Curve) c3 = BRep_Tool::Curve(side.edge, f, l);
            if (c3.IsNull()) {
                why = "an edge of the face has no curve";
                return {};
            }
            while (auto trimmed = Handle(Geom_TrimmedCurve)::DownCast(c3))
                c3 = trimmed->BasisCurve();
            side.curve = GeomAPI::To2d(c3, gp_Pln(gp::XOY()));
            while (auto trimmed = Handle(Geom2d_TrimmedCurve)::DownCast(side.curve))
                side.curve = trimmed->BasisCurve();
            for (double k : {0.0, 0.5, 1.0}) {
                const double u = f + (l - f) * k;
                const gp_Pnt2d p = side.curve->Value(u);
                if (gp_Pnt(p.X(), p.Y(), 0).Distance(c3->Value(u)) > 1e-9) {
                    why = "an edge of the face isn't flat in it";
                    return {};
                }
            }
            side.kind = side.curve->IsKind(STANDARD_TYPE(Geom2d_Line)) ? Kind::Line
                        : side.curve->IsKind(STANDARD_TYPE(Geom2d_Circle)) ? Kind::Circle
                                                                           : Kind::Other;
            side.first = f;
            side.last = l;
            side.run = side.edge.Orientation() == TopAbs_REVERSED ? -1 : 1;
            side.start = side.run > 0 ? f : l;
            side.end = side.run > 0 ? l : f;
            side.out = facing * side.run;
            if (side.kind == Kind::Other)
                side.vScale = 1 / height;
            else if (side.kind == Kind::Line)
                side.vScale = std::hypot(grow, 1);
            else
                side.vScale = std::hypot(grow, 1); // 1 / cos(atan(grow))
            sides.push_back(side);
        }
        if (sides.empty()) {
            why = "a wire of the face has no edges";
            return {};
        }
        wires.push_back(sides);
    }

    BRep_Builder b;
    std::vector<TopoDS_Face> faces{face};
    TopoDS_Face topFace;
    b.MakeFace(topFace, new Geom_Plane(gp_Ax3(gp_Pnt(0, 0, height), gp::DZ(), gp::DX())), kTol);
    for (auto &sides : wires) {
        const int m = int(sides.size());
        // Corner i runs up from where side i ends and side i + 1 starts.
        std::vector<Corner> corners(m);
        for (int i = 0; i < m; ++i) {
            Side &a = sides[i], &next = sides[(i + 1) % m];
            Handle(Geom_Curve) curve;
            if (!makeCorner(a, next, grow, height, corners[i], curve, why))
                return {};
            const TopoDS_Vertex bottom = TopExp::LastVertex(a.edge, true);
            b.MakeVertex(corners[i].top, gp_Pnt(corners[i].topAt.X(), corners[i].topAt.Y(), height), kTol);
            b.MakeEdge(corners[i].edge, curve, kTol);
            b.Add(corners[i].edge, bottom.Oriented(TopAbs_FORWARD));
            b.Add(corners[i].edge, corners[i].top.Oriented(TopAbs_REVERSED));
            b.Range(corners[i].edge, 0, height);
            b.UpdateVertex(bottom, 0, corners[i].edge, kTol);
            b.UpdateVertex(corners[i].top, height, corners[i].edge, kTol);
        }
        for (int i = 0; i < m; ++i) {
            Side &side = sides[i];
            const Corner &startCorner = corners[(i + m - 1) % m], &endCorner = corners[i];
            // No side may shrink to nothing on the way up.
            for (int k = 0; k <= 32; ++k) {
                const double t = height * k / 32;
                if ((endCorner.onBefore->Value(t).X() - startCorner.onAfter->Value(t).X()) * side.run <= 1e-9) {
                    why = "the taper is too steep: an edge of the outline would shrink to nothing before the top";
                    return {};
                }
            }
            // The span the side's surface must cover: the edge and every point of its corners.
            double lo = std::min(side.first, side.last), hi = std::max(side.first, side.last);
            for (const Handle(Geom2d_Curve) &trace : {endCorner.onBefore, startCorner.onAfter}) {
                const auto spline = Handle(Geom2d_BSplineCurve)::DownCast(trace);
                for (int k = 1; k <= spline->NbPoles(); ++k) {
                    lo = std::min(lo, spline->Pole(k).X());
                    hi = std::max(hi, spline->Pole(k).X());
                }
            }
            side.surface = sideSurface(side, grow, height, lo, hi, why);
            if (side.surface.IsNull())
                return {};

            // The top edge, its parameter the curve's own.
            const double delta = side.out * grow * height;
            Handle(Geom_Curve) top;
            if (side.kind == Kind::Line) {
                const gp_Lin2d line = Handle(Geom2d_Line)::DownCast(side.curve)->Lin2d();
                const gp_Pnt2d at = line.Location().Translated(rightNormal(side.curve, 0) * delta);
                top = new Geom_Line(gp_Pnt(at.X(), at.Y(), height), gp_Dir(line.Direction().X(), line.Direction().Y(), 0));
                side.topFlat = new Geom2d_Line(at, line.Direction());
            } else if (side.kind == Kind::Circle) {
                const gp_Circ2d circle = Handle(Geom2d_Circle)::DownCast(side.curve)->Circ2d();
                const double radius = std::hypot(corners[i].topAt.X() - circle.Location().X(), corners[i].topAt.Y() - circle.Location().Y());
                const gp_Dir x(circle.XAxis().Direction().X(), circle.XAxis().Direction().Y(), 0),
                    y(circle.YAxis().Direction().X(), circle.YAxis().Direction().Y(), 0);
                top = new Geom_Circle(gp_Ax2(gp_Pnt(circle.Location().X(), circle.Location().Y(), height), x.Crossed(y), x), radius);
                side.topFlat = new Geom2d_Circle(gp_Circ2d(circle.Axis(), radius));
            } else {
                const auto spline = Handle(Geom_BSplineCurve)::DownCast(side.surface->VIso(1));
                TColgp_Array1OfPnt2d flat(1, spline->NbPoles());
                for (int k = 1; k <= spline->NbPoles(); ++k)
                    flat(k) = gp_Pnt2d(spline->Pole(k).X(), spline->Pole(k).Y());
                top = spline;
                side.topFlat = new Geom2d_BSplineCurve(flat, spline->Knots(), spline->Multiplicities(), spline->Degree());
            }
            const double from = startCorner.after, to = endCorner.before;
            b.MakeEdge(side.top, top, kTol);
            b.Add(side.top, (from < to ? startCorner.top : endCorner.top).Oriented(TopAbs_FORWARD));
            b.Add(side.top, (from < to ? endCorner.top : startCorner.top).Oriented(TopAbs_REVERSED));
            b.Range(side.top, std::min(from, to), std::max(from, to));
            if (m > 1) {
                b.UpdateVertex(startCorner.top, from, side.top, kTol);
                b.UpdateVertex(endCorner.top, to, side.top, kTol);
            }
            b.UpdateEdge(side.top, side.topFlat, topFace, kTol);

            // The side face: in its (u, v) the bottom edge runs +u, the corner at the far end up, the
            // top edge back and the near corner down, so its wire goes anticlockwise.
            TopoDS_Face sideFace;
            b.MakeFace(sideFace, side.surface, kTol);
            const Corner &far = side.run > 0 ? endCorner : startCorner, &near = side.run > 0 ? startCorner : endCorner;
            const Handle(Geom2d_Curve) farTrace = side.run > 0 ? endCorner.onBefore : startCorner.onAfter;
            const Handle(Geom2d_Curve) nearTrace = side.run > 0 ? startCorner.onAfter : endCorner.onBefore;
            b.UpdateEdge(TopoDS::Edge(side.edge.Oriented(TopAbs_FORWARD)), new Geom2d_Line(gp_Pnt2d(0, 0), gp::DX2d()), sideFace, kTol);
            b.UpdateEdge(side.top, new Geom2d_Line(gp_Pnt2d(0, side.vScale * height), gp::DX2d()), sideFace, kTol);
            if (m == 1) // the one corner is the seam of a closed side
                b.UpdateEdge(far.edge, farTrace, nearTrace, sideFace, kTol);
            else {
                b.UpdateEdge(far.edge, farTrace, sideFace, kTol);
                b.UpdateEdge(near.edge, nearTrace, sideFace, kTol);
            }
            TopoDS_Wire wire;
            b.MakeWire(wire);
            b.Add(wire, side.edge.Oriented(TopAbs_FORWARD));
            b.Add(wire, far.edge.Oriented(TopAbs_FORWARD));
            b.Add(wire, side.top.Oriented(TopAbs_REVERSED));
            b.Add(wire, near.edge.Oriented(TopAbs_REVERSED));
            b.Add(sideFace, wire);
            faces.push_back(sideFace);
        }
        // The top wire runs the way the bottom one does; anticlockwise seen from above for the outside.
        TopoDS_Wire wire;
        b.MakeWire(wire);
        for (const Side &side : sides)
            b.Add(wire, side.top.Oriented(side.run > 0 ? TopAbs_FORWARD : TopAbs_REVERSED));
        if (facing < 0)
            wire.Reverse();
        b.Add(topFace, wire);
    }
    faces.push_back(topFace);

    TopoDS_Shell shell;
    b.MakeShell(shell);
    for (const TopoDS_Face &f : faces)
        b.Add(shell, f);
    // Let OpenCascade fix the edge tolerances and same-parameter flags for the traces just made.
    for (TopExp_Explorer e(shell, TopAbs_EDGE); e.More(); e.Next())
        b.SameParameter(TopoDS::Edge(e.Current()), false);
    BRepLib::SameParameter(shell, kTol);
    ShapeFix_Shell orient(shell);
    orient.FixFaceOrientation(shell);
    TopoDS_Solid solid;
    b.MakeSolid(solid);
    b.Add(solid, orient.Shell());
    BRepLib::OrientClosedSolid(solid);
    if (!BRepCheck_Analyzer(solid).IsValid()) {
        why = "the tapered shape didn't check out";
        return {};
    }
    // Sides that run into each other: an edge of one passing through another (BOPAlgo_CheckerSI up to
    // edge/face, level 4). Level 5 would also intersect every curved face with itself over its whole
    // parameter box, and a closed spline side with a sharp start runs a little past its own seam, so
    // that always finds the seam (BOPAlgo_CheckerSI.cxx, CheckFaceSelfIntersection). A side folding over
    // itself shows as the top outline crossing itself, which the check above already refuses.
    BOPAlgo_CheckerSI clash;
    TopTools_ListOfShape argument;
    argument.Append(solid);
    clash.SetArguments(argument);
    clash.SetLevelOfCheck(4);
    clash.Perform();
    if (clash.HasErrors()) {
        why = "the taper is too steep: the sides would run into each other";
        return {};
    }
    return BRepBuilderAPI_Transform(solid, toFrame.Inverted(), false).Shape();
}


} // namespace taper
