#include "engine/heavy/projectexact.h"

#include "document/projection.h"

#include <BRepAdaptor_Curve.hxx>
#include <TopLoc_Location.hxx>
#include <GeomAdaptor_Surface.hxx>
#include <ElSLib.hxx>
#include <GeomAPI_IntCS.hxx>
#include <BRepAdaptor_Surface.hxx>
#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepBuilderAPI_MakeVertex.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_Transform.hxx>
#include <BRepLProp_SLProps.hxx>
#include <BRepLib.hxx>
#include <BRepMesh_IncrementalMesh.hxx>
#include <BRepTools.hxx>
#include <OSD_Parallel.hxx>
#include <Bnd_Box2d.hxx>
#include <BndLib_Add2dCurve.hxx>
#include <Geom2d_BSplineCurve.hxx>
#include <Geom2dAdaptor_Curve.hxx>
#include <TColStd_ListOfInteger.hxx>
#include <Bnd_HArray1OfBox.hxx>
#include <Bnd_BoundSortBox.hxx>
#include <TColStd_Array1OfReal.hxx>
#include <BRep_Builder.hxx>
#include <BRep_Tool.hxx>
#include <Bnd_Box.hxx>
#include <BRepBndLib.hxx>
#include <BRepGProp.hxx>
#include <GProp_GProps.hxx>
#include <BRepExtrema_DistShapeShape.hxx>
#include <GCPnts_TangentialDeflection.hxx>
#include <Geom2dAPI_InterCurveCurve.hxx>
#include <Geom2d_TrimmedCurve.hxx>
#include <GeomAPI.hxx>
#include <IntRes2d_IntersectionSegment.hxx>
#include <ElCLib.hxx>
#include <GeomAPI_ProjectPointOnCurve.hxx>
#include <GeomProjLib.hxx>
#include <Geom_BSplineCurve.hxx>
#include <Geom_Circle.hxx>
#include <Geom_Ellipse.hxx>
#include <Geom_Line.hxx>
#include <Geom_Plane.hxx>
#include <Geom_TrimmedCurve.hxx>
#include <GeomAdaptor_Curve.hxx>
#include <HLRAlgo_Projector.hxx>
#include <HLRBRep_Algo.hxx>
#include <HLRBRep_HLRToShape.hxx>
#include <Message_Report.hxx>
#include <TopoDS_AlertWithShape.hxx>
#include <Poly_Triangulation.hxx>
#include <Precision.hxx>
#include <Standard_Failure.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopTools_IndexedDataMapOfShapeListOfShape.hxx>
#include <ShapeAnalysis_Edge.hxx>
#include <TopoDS_Wire.hxx>
#include <BRepTools_WireExplorer.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopTools_ListOfShape.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Iterator.hxx>
#include <TopoDS_Compound.hxx>
#include <TopoDS_Face.hxx>
#include <gp_Ax3.hxx>
#include <gp_Circ.hxx>
#include <gp_Elips.hxx>
#include <gp_Lin.hxx>
#include <gp_Pln.hxx>

#include <algorithm>
#include <cmath>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <map>
#include <sstream>
#include <memory>
#include <set>

namespace projectexact {

using omamake::FlatPiece;
using omamake::Point2;

namespace {

constexpr double kSame = 1e-9;         // two flat points closer than this (mm) are one
constexpr double kOutlineReach = 1e-4; // a curved face's outline end is joined to an edge this close (mm)
constexpr double kExactJoin = 1e-6;    // outline pieces whose ends are this close (mm) meet exactly
// How far to either side of a piece (mm) to see whether the model is there: past OpenCascade's own
// face classifier, which answers "on the boundary" up to about 1e-5 mm either side of a spline edge
// whatever the edge's tolerance; still under the 0.00001 inch (0.000254 mm) asked for.
constexpr double kSideStep = 1e-4;
constexpr double kCrumb = 1e-6;     // a piece shorter than this (mm) is no piece: a hundredth of the accuracy asked for

// OMAMAKE_PROJECT_TIMES set: each step's time on stderr (probes).
void stage(const char *what) {
    static auto last = std::chrono::steady_clock::now();
    const auto now = std::chrono::steady_clock::now();
    if (getenv("OMAMAKE_PROJECT_TIMES"))
        fprintf(stderr, "project: %-28s %.3f s\n", what, std::chrono::duration<double>(now - last).count());
    last = now;
}

// The plane's own frame: a point's x and y there are its (u, v) on the sketch, z its height off it.
gp_Trsf toPlane(const omamake::Placement &plane) {
    const gp_Dir x(plane.x[0], plane.x[1], plane.x[2]), y(plane.y[0], plane.y[1], plane.y[2]);
    gp_Trsf t;
    t.SetTransformation(gp_Ax3(gp_Pnt(plane.origin[0], plane.origin[1], plane.origin[2]), x.Crossed(y), x));
    return t;
}
TopoDS_Shape onPlane(const TopoDS_Shape &shape, const gp_Trsf &t) {
    return BRepBuilderAPI_Transform(shape, t, false).Shape();
}
gp_Pnt down(const gp_Pnt &p) { return gp_Pnt(p.X(), p.Y(), 0); }
Point2 two(const gp_Pnt &p) { return {p.X(), p.Y()}; }
double apart(const Point2 &a, const Point2 &b) { return std::hypot(a.x - b.x, a.y - b.y); }

// An edge's curve laid flat (z = 0 in the plane's frame), over the edge's own parameters.
struct Laid {
    enum Kind { Gone, Point, Segment, Curve } kind = Gone;
    gp_Pnt p, q;                    // Point: p. Segment: from p to q (seen edge on: its farthest points).
    Handle(Geom_Curve) curve;       // Curve: flat, same parameters as the edge
    double first = 0, last = 0;
};

// The farthest a flat curve reaches each way along `way`, worked out on the curve itself.
void extremes(const Handle(Geom_Curve) &c, double f, double l, const gp_Dir &way, gp_Pnt &low, gp_Pnt &high) {
    const auto along = [&](double t) { return gp_Vec(down(c->Value(t)).XYZ()).Dot(gp_Vec(way)); };
    const auto refine = [&](double t, double sign) {
        double step = (l - f) / 512;
        for (int i = 0; i < 200 && step > (l - f) * 1e-15; ++i) {
            const double here = sign * along(t);
            if (t + step <= l && sign * along(t + step) > here) t += step;
            else if (t - step >= f && sign * along(t - step) > here) t -= step;
            else step /= 2;
        }
        return t;
    };
    double tl = f, th = f;
    for (int i = 0; i <= 512; ++i) {
        const double t = f + (l - f) * i / 512;
        if (along(t) < along(tl)) tl = t;
        if (along(t) > along(th)) th = t;
    }
    low = down(c->Value(refine(tl, -1)));
    high = down(c->Value(refine(th, 1)));
}

Laid lay(const TopoDS_Edge &edge) {
    Laid out;
    if (BRep_Tool::Degenerated(edge))
        return out;
    double f = 0, l = 0;
    const Handle(Geom_Curve) c3 = BRep_Tool::Curve(edge, f, l);
    if (c3.IsNull())
        return out;
    const BRepAdaptor_Curve a(edge);
    out.first = f;
    out.last = l;
    if (a.GetType() == GeomAbs_Line) {
        out.p = down(a.Value(f));
        out.q = down(a.Value(l));
        out.kind = out.p.Distance(out.q) < kSame ? Laid::Point : Laid::Segment;
        return out;
    }
    if (a.GetType() == GeomAbs_Circle && std::abs(a.Circle().Axis().Direction().Z()) > 1 - 1e-12) {
        const gp_Circ k = a.Circle();
        gp_Ax2 axes(down(k.Location()), gp_Dir(0, 0, k.Axis().Direction().Z() > 0 ? 1 : -1), gp_Dir(k.XAxis().Direction().X(), k.XAxis().Direction().Y(), 0));
        out.curve = new Geom_Circle(axes, k.Radius());
        out.kind = Laid::Curve;
        return out;
    }
    Handle(Geom_Curve) flat;
    try {
        flat = GeomProjLib::ProjectOnPlane(new Geom_TrimmedCurve(c3, f, l), new Geom_Plane(gp::XOY()), gp::DZ(), true);
    } catch (const Standard_Failure &) {
    }
    // Seen edge on, or end on: no flat curve, or one with no width. Its farthest points then.
    gp_Pnt lo = down(c3->Value(f)), hi = lo;
    for (int i = 1; i <= 64; ++i) {
        const gp_Pnt p = down(c3->Value(f + (l - f) * i / 64));
        if (p.Distance(lo) > hi.Distance(lo)) hi = p;
    }
    gp_Pnt far = hi;
    for (int i = 0; i <= 64; ++i) {
        const gp_Pnt p = down(c3->Value(f + (l - f) * i / 64));
        if (p.Distance(hi) > far.Distance(hi)) far = p;
    }
    if (far.Distance(hi) < kSame) {
        out.p = hi;
        out.kind = Laid::Point;
        return out;
    }
    const gp_Lin across(far, gp_Dir(gp_Vec(far, hi)));
    bool straight = true;
    for (int i = 0; i <= 256 && straight; ++i)
        straight = across.Distance(down(c3->Value(f + (l - f) * i / 256))) < kSame;
    if (straight || flat.IsNull()) {
        if (!straight)
            return out;     // can't be laid flat: left out
        extremes(c3, f, l, across.Direction(), out.p, out.q);
        out.kind = Laid::Segment;
        return out;
    }
    out.curve = flat;
    out.first = flat->FirstParameter();
    out.last = flat->LastParameter();
    out.kind = Laid::Curve;
    return out;
}

// A clamped cubic B-spline on uniform knots over [0, 1] (the sketch's Control Point Spline) through
// `start` and `end`, within omamake::kProjectFit of the curve c from t0 to t1 (either way round).
// Least squares with more poles until it is close enough.
bool fit(const Handle(Geom_Curve) &c, double t0, double t1, const Point2 &startAt, const Point2 &endAt, std::vector<Point2> &poles,
         double &missed) {
    // Worked out from the curve's own start: far from the model's origin (a part placed 25 m out) the
    // least squares would otherwise lose the last digits it needs to the size of the coordinates.
    const Point2 origin = startAt, start{0, 0}, end{endAt.x - startAt.x, endAt.y - startAt.y};
    missed = 0;
    // Arc length decides where along the spline each point of the curve goes, from a table fine enough
    // (steps of 0.01 mm at most) that reading between its entries slides no point along the curve by
    // anything near kProjectFit: the spline is held to the curve point for point, in order, so it can
    // never cut across a bend.
    //
    // The table's steps are laid out stretch by stretch, a stretch running between the curve's own
    // breaks in smoothness (a spline's knots where it is less than C2): a spline whose speed jumps a
    // hundredfold at a knot (a tiny first span holding 0.004 mm of the curve) would otherwise have that
    // whole span inside one step, and reading between the step's ends would put points in the wrong place.
    const double lo = std::min(t0, t1), hi = std::max(t0, t1);
    std::vector<double> breaks{lo};
    {
        GeomAdaptor_Curve along(c, lo, hi);
        const int nb = along.NbIntervals(GeomAbs_C2);
        if (nb > 1) {
            TColStd_Array1OfReal at(1, nb + 1);
            along.Intervals(at, GeomAbs_C2);
            for (int i = 2; i <= nb; ++i)
                if (at(i) > breaks.back() && at(i) < hi) breaks.push_back(at(i));
        }
        breaks.push_back(hi);
    }
    if (t0 > t1) std::reverse(breaks.begin(), breaks.end());
    std::vector<double> rough(breaks.size() - 1, 0);
    double roughAll = 0;
    for (std::size_t k = 0; k + 1 < breaks.size(); ++k) {
        gp_Pnt last = c->Value(breaks[k]);
        for (int i = 1; i <= 64; ++i) {
            const gp_Pnt p = c->Value(breaks[k] + (breaks[k + 1] - breaks[k]) * i / 64);
            rough[k] += p.Distance(last);
            last = p;
        }
        roughAll += rough[k];
    }
    const int kTable = std::clamp(int(std::ceil(roughAll / 0.01)), 4096, 1 << 20);
    std::vector<double> ts{t0};
    for (std::size_t k = 0; k + 1 < breaks.size(); ++k) {
        const int steps = std::max(16, int(std::ceil(kTable * (roughAll > 0 ? rough[k] / roughAll : 1.0))));
        for (int i = 1; i <= steps; ++i)
            ts.push_back(i == steps ? breaks[k + 1] : breaks[k] + (breaks[k + 1] - breaks[k]) * i / steps);
    }
    const std::size_t kLast = ts.size() - 1;
    std::vector<double> len(ts.size(), 0);
    for (std::size_t i = 1; i <= kLast; ++i)
        len[i] = len[i - 1] + c->Value(ts[i - 1]).Distance(c->Value(ts[i]));
    if (len.back() < kSame) {
        missed = -1;            // no curve left at all: the caller draws a straight line between the ends
        return false;
    }
    // A model whose faces meet loosely can have a corner a little off the curve's own end: the spline
    // goes to the corner, the difference taken up evenly along it (on an exact model there is none).
    const gp_Vec shift0(c->Value(t0), gp_Pnt(startAt.x, startAt.y, 0)), shift1(c->Value(t1), gp_Pnt(endAt.x, endAt.y, 0));
    const auto target = [&](double s, double t) {
        const gp_Pnt p = c->Value(t).Translated(gp_Vec(-origin.x, -origin.y, 0));
        return gp_Pnt(p.X() + (1 - s) * shift0.X() + s * shift1.X(), p.Y() + (1 - s) * shift0.Y() + s * shift1.Y(), 0);
    };
    const auto paramAt = [&](double s) {    // the curve's parameter at share s of its length
        const double want = s * len.back();
        const auto it = std::lower_bound(len.begin(), len.end(), want);
        const std::size_t i = std::min<std::size_t>(std::size_t(it - len.begin()), kLast);
        if (i == 0) return ts[0];
        const double span = len[i] - len[i - 1];
        return ts[i - 1] + (ts[i] - ts[i - 1]) * (span > 0 ? (want - len[i - 1]) / span : 0);
    };
    for (int n = 4; n <= 4096; n *= 2) {
        // Basis on clamped uniform knots: knots 0,0,0,0, 1/(n-3), ..., 1,1,1,1.
        std::vector<double> knot;
        for (int i = 0; i < 4; ++i) knot.push_back(0);
        for (int i = 1; i < n - 3; ++i) knot.push_back(double(i) / (n - 3));
        for (int i = 0; i < 4; ++i) knot.push_back(1);
        const auto basis = [&](double s, int &span, double N[4]) {
            span = std::min(n - 1, std::max(3, int(std::upper_bound(knot.begin(), knot.end(), s) - knot.begin()) - 1));
            double left[4], right[4];
            N[0] = 1;
            for (int j = 1; j <= 3; ++j) {
                left[j] = s - knot[std::size_t(span + 1 - j)];
                right[j] = knot[std::size_t(span + j)] - s;
                double saved = 0;
                for (int r = 0; r < j; ++r) {
                    const double d = right[r + 1] + left[j - r];
                    const double temp = d != 0 ? N[r] / d : 0;
                    N[r] = saved + right[r + 1] * temp;
                    saved = left[j - r] * temp;
                }
                N[j] = saved;
            }
        };
        const int m = 16 * n, unknown = n - 2;
        // Banded normal equations (half width 3) for poles 1..n-2, poles 0 and n-1 held at the ends.
        std::vector<double> A(std::size_t(unknown) * 4, 0), bx(std::size_t(unknown), 0), by(std::size_t(unknown), 0);
        std::vector<gp_Pnt> fitTo(std::size_t(m + 1));
        std::vector<double> sAt(std::size_t(m + 1));
        for (int k = 0; k <= m; ++k) {
            const double s = double(k) / m;
            sAt[std::size_t(k)] = s;
            fitTo[std::size_t(k)] = target(s, paramAt(s));
            int span;
            double N[4];
            basis(s, span, N);
            double rx = fitTo[std::size_t(k)].X(), ry = fitTo[std::size_t(k)].Y();
            for (int j = 0; j < 4; ++j) {
                const int pole = span - 3 + j;
                if (pole == 0) { rx -= N[j] * start.x; ry -= N[j] * start.y; }
                if (pole == n - 1) { rx -= N[j] * end.x; ry -= N[j] * end.y; }
            }
            for (int j = 0; j < 4; ++j) {
                const int pj = span - 3 + j - 1;
                if (pj < 0 || pj >= unknown) continue;
                bx[std::size_t(pj)] += N[j] * rx;
                by[std::size_t(pj)] += N[j] * ry;
                for (int i = j; i < 4; ++i) {
                    const int pi = span - 3 + i - 1;
                    if (pi < 0 || pi >= unknown) continue;
                    A[std::size_t(pj) * 4 + std::size_t(pi - pj)] += N[j] * N[i];
                }
            }
        }
        // Banded Cholesky: A = L Lt, L stored by row with the diagonal and three below it.
        std::vector<double> L(std::size_t(unknown) * 4, 0);
        bool ok = true;
        for (int i = 0; i < unknown && ok; ++i)
            for (int j = std::max(0, i - 3); j <= i; ++j) {
                double sum = A[std::size_t(j) * 4 + std::size_t(i - j)];
                for (int k = std::max(0, i - 3); k < j; ++k)
                    sum -= L[std::size_t(i) * 4 + std::size_t(i - k)] * L[std::size_t(j) * 4 + std::size_t(j - k)];
                if (i == j) {
                    if (sum <= 0) { ok = false; break; }
                    L[std::size_t(i) * 4] = std::sqrt(sum);
                } else {
                    L[std::size_t(i) * 4 + std::size_t(i - j)] = sum / L[std::size_t(j) * 4];
                }
            }
        if (!ok)
            continue;
        const auto solve = [&](std::vector<double> b) {
            for (int i = 0; i < unknown; ++i) {
                for (int k = std::max(0, i - 3); k < i; ++k) b[std::size_t(i)] -= L[std::size_t(i) * 4 + std::size_t(i - k)] * b[std::size_t(k)];
                b[std::size_t(i)] /= L[std::size_t(i) * 4];
            }
            for (int i = unknown - 1; i >= 0; --i) {
                for (int k = i + 1; k <= std::min(unknown - 1, i + 3); ++k) b[std::size_t(i)] -= L[std::size_t(k) * 4 + std::size_t(k - i)] * b[std::size_t(k)];
                b[std::size_t(i)] /= L[std::size_t(i) * 4];
            }
            return b;
        };
        const std::vector<double> x = solve(bx), y = solve(by);
        poles.assign(std::size_t(n), {});
        poles.front() = start;
        poles.back() = end;
        for (int i = 0; i < unknown; ++i)
            poles[std::size_t(i + 1)] = {x[std::size_t(i)], y[std::size_t(i)]};
        // Checked between the fitted points too: the worst of every point, and half way between each.
        double worst = 0;
        for (int k = 0; k <= 2 * m && worst <= omamake::kProjectFit; ++k) {
            const double s = double(k) / (2 * m);
            const gp_Pnt want = target(s, paramAt(s));
            int span;
            double N[4];
            basis(s, span, N);
            double px = 0, py = 0;
            for (int j = 0; j < 4; ++j) {
                const Point2 &p = poles[std::size_t(span - 3 + j)];
                px += N[j] * p.x;
                py += N[j] * p.y;
            }
            const double off = std::hypot(px - want.X(), py - want.Y());
            worst = std::max(worst, off);
        }
        missed = worst;
        if (worst <= omamake::kProjectFit) {
            for (Point2 &p : poles) {
                p.x += origin.x;
                p.y += origin.y;
            }
            poles.front() = startAt;
            poles.back() = endAt;
            return true;
        }
    }
    return false;
}

// A piece of an outline while it's being put together, in the way it is travelled.
struct Run {
    FlatPiece::Kind kind = FlatPiece::Line;
    Point2 from, to;
    Point2 centre;
    double radius = 0;
    bool ccw = true;                // round: travelled counter-clockwise
    gp_Elips ellipse;               // a whole ellipse
    Handle(Geom_Curve) curve;       // anything else: the flat curve, travelled from t0 to t1
    double t0 = 0, t1 = 0;
};

Run runOf(const TopoDS_Edge &e) {
    Run r;
    const BRepAdaptor_Curve a(e);
    const bool forward = e.Orientation() != TopAbs_REVERSED;
    r.from = two(BRep_Tool::Pnt(TopExp::FirstVertex(e, true)));
    r.to = two(BRep_Tool::Pnt(TopExp::LastVertex(e, true)));
    const bool closed = TopExp::FirstVertex(e, true).IsSame(TopExp::LastVertex(e, true));
    if (a.GetType() == GeomAbs_Line) {
        r.kind = FlatPiece::Line;
    } else if (a.GetType() == GeomAbs_Circle) {
        const gp_Circ k = a.Circle();
        r.kind = closed ? FlatPiece::Circle : FlatPiece::Arc;
        r.centre = two(k.Location());
        r.radius = k.Radius();
        r.ccw = (k.Axis().Direction().Z() > 0) == forward;
    } else if (a.GetType() == GeomAbs_Ellipse && closed) {
        r.kind = FlatPiece::Ellipse;
        r.ellipse = a.Ellipse();
    } else {
        r.kind = FlatPiece::Spline;
        double f, l;
        r.curve = BRep_Tool::Curve(e, f, l);
        r.t0 = forward ? f : l;
        r.t1 = forward ? l : f;
    }
    return r;
}

bool sameLine(const Run &a, const Run &b) {
    if (a.kind != FlatPiece::Line || b.kind != FlatPiece::Line)
        return false;
    const double dx = a.to.x - a.from.x, dy = a.to.y - a.from.y, l = std::hypot(dx, dy);
    const double ex = b.to.x - b.from.x, ey = b.to.y - b.from.y, m = std::hypot(ex, ey);
    if (l < kSame || m < kSame || (dx * ex + dy * ey) <= 0)
        return false;
    return std::abs((b.to.x - a.from.x) * dy - (b.to.y - a.from.y) * dx) / l < kSame
        && std::abs(dx / l * ey / m - dy / l * ex / m) < 1e-12;
}
bool sameCircle(const Run &a, const Run &b) {
    return a.kind == FlatPiece::Arc && b.kind == FlatPiece::Arc && a.ccw == b.ccw && apart(a.centre, b.centre) < kSame
        && std::abs(a.radius - b.radius) < kSame;
}
bool sameCurve(const Run &a, const Run &b) {
    return a.kind == FlatPiece::Spline && b.kind == FlatPiece::Spline && a.curve == b.curve && std::abs(a.t1 - b.t0) < 1e-12;
}
Point2 ontoCircle(const Run &r, const Point2 &p) {
    const double dx = p.x - r.centre.x, dy = p.y - r.centre.y, l = std::hypot(dx, dy);
    return l < kSame ? p : Point2{r.centre.x + dx / l * r.radius, r.centre.y + dy / l * r.radius};
}

// The point at share j/n along a run, the way it is travelled (a whole ellipse: round its own axis).
gp_Pnt2d pointOn(const Run &r, int j, int n) {
    if (r.kind == FlatPiece::Spline) {
        const gp_Pnt q = r.curve->Value(r.t0 + (r.t1 - r.t0) * j / n);
        return gp_Pnt2d(q.X(), q.Y());
    }
    if (r.kind == FlatPiece::Arc || r.kind == FlatPiece::Circle) {
        double a0 = std::atan2(r.from.y - r.centre.y, r.from.x - r.centre.x), a1 = std::atan2(r.to.y - r.centre.y, r.to.x - r.centre.x);
        if (r.ccw) { while (a1 <= a0 + 1e-12) a1 += 2 * M_PI; } else { while (a1 >= a0 - 1e-12) a1 -= 2 * M_PI; }
        const double a = a0 + (a1 - a0) * j / n;
        return gp_Pnt2d(r.centre.x + r.radius * std::cos(a), r.centre.y + r.radius * std::sin(a));
    }
    if (r.kind == FlatPiece::Ellipse) {
        const gp_Pnt q = ElCLib::Value(2 * M_PI * j / n, r.ellipse);
        return gp_Pnt2d(q.X(), q.Y());
    }
    return gp_Pnt2d(r.from.x + (r.to.x - r.from.x) * j / n, r.from.y + (r.to.y - r.from.y) * j / n);
}

// Twice the area a run sweeps round the origin (the shoelace sum; enough to tell which way a loop goes).
double sweep(const Run &r) {
    double area = 0;
    for (int k = 0; k < 16; ++k) {
        const gp_Pnt2d p = pointOn(r, k, 16), q = pointOn(r, k + 1, 16);
        area += p.X() * q.Y() - q.X() * p.Y();
    }
    return area;
}

// The same run travelled the other way.
Run reversed(Run r) {
    std::swap(r.from, r.to);
    std::swap(r.t0, r.t1);
    r.ccw = !r.ccw;
    if (r.kind == FlatPiece::Ellipse)
        r.ellipse.SetPosition(gp_Ax2(r.ellipse.Location(), r.ellipse.Axis().Direction().Reversed(), r.ellipse.XAxis().Direction()));
    return r;
}

// Runs joined where one curve was cut in two, corners put exactly on the curves they join, and the
// pieces made: what the sketch gets for one loop.
bool finish(std::vector<Run> runs, std::vector<FlatPiece> &out, std::string &why) {
    // One loop that is one closed curve: nothing to join.
    if (runs.size() > 1) {
        // Start where a new curve starts, so nothing that wraps round the start is left in two.
        std::size_t start = 0;
        for (; start < runs.size(); ++start) {
            const Run &before = runs[(start + runs.size() - 1) % runs.size()], &here = runs[start];
            if (!sameLine(before, here) && !sameCircle(before, here) && !sameCurve(before, here))
                break;
        }
        if (start < runs.size()) {
            std::rotate(runs.begin(), runs.begin() + std::ptrdiff_t(start), runs.end());
            std::vector<Run> joined;
            for (const Run &r : runs) {
                if (!joined.empty() && (sameLine(joined.back(), r) || sameCircle(joined.back(), r) || sameCurve(joined.back(), r))) {
                    joined.back().to = r.to;
                    joined.back().t1 = r.t1;
                    continue;
                }
                joined.push_back(r);
            }
            runs = std::move(joined);
        }
        for (Run &r : runs)
            if (r.kind == FlatPiece::Arc && runs.size() == 1)
                r.kind = FlatPiece::Circle;
    }
    // Crumbs far below any accuracy asked for (where several edges almost meet): gone, their two
    // neighbours meeting where they were.
    if (runs.size() > 1) {
        std::vector<Run> kept;
        for (const Run &r : runs) {
            bool crumb = apart(r.from, r.to) < kCrumb && r.kind != FlatPiece::Circle && r.kind != FlatPiece::Ellipse;
            if (crumb && r.kind == FlatPiece::Spline) {
                double length = 0;
                gp_Pnt last = r.curve->Value(r.t0);
                for (int k = 1; k <= 16; ++k) {
                    const gp_Pnt p = r.curve->Value(r.t0 + (r.t1 - r.t0) * k / 16);
                    length += p.Distance(last);
                    last = p;
                }
                crumb = length < kCrumb;
            }
            if (!crumb)
                kept.push_back(r);
        }
        if (!kept.empty())
            runs = std::move(kept);
    }
    // Each corner on both curves it joins (a round one decides; a line just takes it).
    const std::size_t n = runs.size();
    const auto turn = [](const Run &r) {        // how far an arc turns, from its start to its end
        double a0 = std::atan2(r.from.y - r.centre.y, r.from.x - r.centre.x), a1 = std::atan2(r.to.y - r.centre.y, r.to.x - r.centre.x);
        double t = r.ccw ? a1 - a0 : a0 - a1;
        while (t < 0) t += 2 * M_PI;
        return t;
    };
    std::vector<double> turnBefore(n, 0);
    for (std::size_t i = 0; i < n; ++i)
        if (runs[i].kind == FlatPiece::Arc)
            turnBefore[i] = turn(runs[i]);
    if (n > 1)
        for (std::size_t i = 0; i < n; ++i) {
            Run &a = runs[i], &b = runs[(i + 1) % n];
            Point2 p = a.to;
            const bool ra = a.kind == FlatPiece::Arc, rb = b.kind == FlatPiece::Arc;
            if (ra && rb && apart(a.centre, b.centre) > kSame) {
                // Where the two circles cross, nearest the corner; else on the first.
                const double dx = b.centre.x - a.centre.x, dy = b.centre.y - a.centre.y, d = std::hypot(dx, dy);
                const double s = (a.radius * a.radius - b.radius * b.radius + d * d) / (2 * d), h2 = a.radius * a.radius - s * s;
                p = ontoCircle(a, p);
                if (h2 >= 0) {
                    const double h = std::sqrt(h2), mx = a.centre.x + dx * s / d, my = a.centre.y + dy * s / d;
                    const Point2 one{mx - dy * h / d, my + dx * h / d}, other{mx + dy * h / d, my - dx * h / d};
                    const Point2 &best = apart(one, a.to) < apart(other, a.to) ? one : other;
                    if (apart(best, a.to) < 1e-6)
                        p = best;
                }
            } else if (ra) {
                p = ontoCircle(a, p);
            } else if (rb) {
                p = ontoCircle(b, p);
            } else if ((a.kind == FlatPiece::Spline) != (b.kind == FlatPiece::Spline)) {
                // A curve meeting a line: the corner on the curve, nearest where it was; the line takes it.
                // Only as far as the gap it closes: a foot farther off is some other stretch of the curve,
                // and the corner then stays at the curve's own end.
                Run &k = a.kind == FlatPiece::Spline ? a : b;
                const double t = &k == &a ? k.t1 : k.t0;
                const double gap = apart(a.to, b.from) + kExactJoin;
                p = two(k.curve->Value(t));
                const Point2 lineEnd = &k == &a ? b.from : a.to;
                GeomAPI_ProjectPointOnCurve onto(gp_Pnt(lineEnd.x, lineEnd.y, 0), k.curve, std::min(k.t0, k.t1), std::max(k.t0, k.t1));
                double best = 1e300;
                for (int j = 1; j <= onto.NbPoints(); ++j)
                    if (onto.Distance(j) <= gap && apart(two(onto.Point(j)), p) <= gap && std::abs(onto.Parameter(j) - t) < best) {
                        best = std::abs(onto.Parameter(j) - t);
                        p = two(onto.Point(j));
                        (&k == &a ? k.t1 : k.t0) = onto.Parameter(j);
                    }
            }
            a.to = p;
            b.from = p;
        }
    // A short arc whose neighbours' corners closed in on it from both sides (where their circles cross)
    // is gone: its ends met or passed each other, which would read as the whole circle the other way.
    // Its neighbours already meet there.
    std::vector<bool> gone(n, false);
    for (std::size_t i = 0; i < n; ++i)
        if (n > 1 && runs[i].kind == FlatPiece::Arc && std::abs(turn(runs[i]) - turnBefore[i]) > M_PI)
            gone[i] = true;
    for (std::size_t i = 0; i < n; ++i) {
        if (gone[i])
            continue;
        const Run &r = runs[i];
        FlatPiece f;
        f.kind = r.kind;
        switch (r.kind) {
        case FlatPiece::Line:
            if (apart(r.from, r.to) < kSame)
                continue;
            f.a = r.from;
            f.b = r.to;
            break;
        case FlatPiece::Arc:
            f.c = r.centre;
            f.a = r.ccw ? r.from : r.to;
            f.b = r.ccw ? r.to : r.from;
            if (apart(f.a, f.b) < kSame)
                continue;
            break;
        case FlatPiece::Circle:
            f.c = r.centre;
            f.a = {r.centre.x + r.radius, r.centre.y};
            f.b = f.a;
            break;
        case FlatPiece::Ellipse: {
            const gp_Pnt c = r.ellipse.Location();
            f.c = two(c);
            f.major = two(c.Translated(gp_Vec(r.ellipse.XAxis().Direction()) * r.ellipse.MajorRadius()));
            f.minor = two(c.Translated(gp_Vec(r.ellipse.YAxis().Direction()) * r.ellipse.MinorRadius()));
            break;
        }
        case FlatPiece::Spline: {
            // The sketch's spline is smooth to its curvature (a cubic on even knots) and can't follow a
            // curve that isn't: wherever the curve's bend jumps (a B-spline's knot repeated degree - 1 times
            // or more; degree times is a sharp corner) one spline ends and the next starts. Each stretch then
            // fits with a handful of poles, where one spline over the jumps would need thousands.
            std::vector<double> at{r.t0};
            Handle(Geom_Curve) basis = r.curve;
            if (const Handle(Geom_TrimmedCurve) trimmed = Handle(Geom_TrimmedCurve)::DownCast(basis); !trimmed.IsNull())
                basis = trimmed->BasisCurve();
            if (const Handle(Geom_BSplineCurve) b = Handle(Geom_BSplineCurve)::DownCast(basis); !b.IsNull()) {
                const double lo = std::min(r.t0, r.t1), hi = std::max(r.t0, r.t1), gap = (hi - lo) * 1e-9;
                std::vector<double> breaks;
                for (int k = 1; k <= b->NbKnots(); ++k)
                    if (b->Multiplicity(k) >= b->Degree() - 1 && b->Knot(k) > lo + gap && b->Knot(k) < hi - gap)
                        breaks.push_back(b->Knot(k));
                if (r.t1 < r.t0)
                    std::reverse(breaks.begin(), breaks.end());
                at.insert(at.end(), breaks.begin(), breaks.end());
            }
            at.push_back(r.t1);
            for (std::size_t k = 0; k + 1 < at.size(); ++k) {
                FlatPiece one;
                one.kind = FlatPiece::Spline;
                one.a = k == 0 ? r.from : two(r.curve->Value(at[k]));
                one.b = k + 2 == at.size() ? r.to : two(r.curve->Value(at[k + 1]));
                double missed = 0;
                const bool fitted = fit(r.curve, at[k], at[k + 1], one.a, one.b, one.poles, missed);
                if (!fitted && missed < 0) {
                    // A curve of no length between two corners a little apart (a loose model): the line between.
                    if (apart(one.a, one.b) >= kSame) {
                        one.kind = FlatPiece::Line;
                        one.poles.clear();
                        out.push_back(one);
                    }
                    continue;
                }
                if (!fitted) {
                    char said[300];
                    const Handle(Geom_BSplineCurve) b = Handle(Geom_BSplineCurve)::DownCast(basis);
                    snprintf(said, sizeof said, "a %s (%d poles, degree %d, %d knots) from (%.4f, %.4f) to (%.4f, %.4f) couldn't be drawn "
                             "as a spline within %g mm (best %.2g mm)", basis->DynamicType()->Name(), b.IsNull() ? 0 : b->NbPoles(),
                             b.IsNull() ? 0 : b->Degree(), b.IsNull() ? 0 : b->NbKnots(), one.a.x, one.a.y, one.b.x, one.b.y,
                             omamake::kProjectFit, missed);
                    why = said;
                    return false;
                }
                out.push_back(one);
            }
            continue;
        }
        case FlatPiece::Point:
            continue;
        }
        out.push_back(f);
    }
    return true;
}

// Whether a point (u, v) of a face's surface is inside the face, judged on the face's own boundary: each
// edge's curve on the face as chords within kInsideFit of it (in mm on the model), and the points where a
// line from (u, v) crosses them counted. OpenCascade's face classifiers judge on a coarser boundary: along
// a long spline edge they can be a few thousandths of a mm off, far more than the accuracy asked for.
constexpr double kInsideFit = 1e-7;
class FaceInside {
public:
    explicit FaceInside(const TopoDS_Face &face) {
        const BRepAdaptor_Surface s(face, false);
        double u0, u1, v0, v1;
        BRepTools::UVBounds(face, u0, u1, v0, v1);
        uLow = u0;
        vLow = v0;
        uPeriod = s.IsUPeriodic() ? s.UPeriod() : 0;
        vPeriod = s.IsVPeriodic() ? s.VPeriod() : 0;
        // A chord within kInsideFit on the model is within this in (u, v): the surface's fastest stretch.
        gp_Pnt p;
        gp_Vec du, dv;
        double stretch = 1e-12;
        for (int i = 0; i <= 4; ++i)
            for (int j = 0; j <= 4; ++j) {
                s.D1(u0 + (u1 - u0) * i / 4, v0 + (v1 - v0) * j / 4, p, du, dv);
                stretch = std::max({stretch, du.Magnitude(), dv.Magnitude()});
            }
        // (No finer than OpenCascade's own smallest length, which its chord maker refuses below.)
        const double fit = std::max(kInsideFit / stretch, 2 * Precision::Confusion());
        tolOn = fit;
        double vlo = std::numeric_limits<double>::max(), vhi = -vlo;
        const auto add = [&](const gp_Pnt2d &a, const gp_Pnt2d &b) {
            if (a.SquareDistance(b) == 0)
                return;
            segs.push_back({a.X(), a.Y(), b.X(), b.Y()});
            vlo = std::min({vlo, a.Y(), b.Y()});
            vhi = std::max({vhi, a.Y(), b.Y()});
        };
        // One edge's boundary curve in chords, in the order the wire travels it.
        const auto chordsOf = [&](const TopoDS_Edge &edge) {
            std::vector<gp_Pnt2d> at;
            double f, l;
            const Handle(Geom2d_Curve) c = BRep_Tool::CurveOnSurface(edge, face, f, l);
            if (c.IsNull())
                return at;
            const Geom2dAdaptor_Curve a(c, f, l);
            if (a.GetType() == GeomAbs_Line) {
                at = {a.Value(f), a.Value(l)};
            } else {
                GCPnts_TangentialDeflection chords(a, f, l, 0.05, fit, 2, Precision::PConfusion(), 0);
                for (int k = 1; k <= chords.NbPoints(); ++k)
                    at.push_back(a.Value(chords.Parameter(k)));
            }
            if (edge.Orientation() == TopAbs_REVERSED)
                std::reverse(at.begin(), at.end());
            return at;
        };
        // Each boundary loop edge after edge, as the wire runs. On a loosely made model one edge's curve
        // can stop short of where the next one starts (by up to the corner's looseness): the gap is closed
        // straight across, so the loop is always closed and a line across it always crosses it an even
        // number of times. (OpenCascade's own face classifiers close these gaps the same way.) A wire that
        // can't be walked in order (a broken one) goes in edge by edge, unjoined.
        for (TopExp_Explorer w(face, TopAbs_WIRE); w.More(); w.Next()) {
            const TopoDS_Wire &wire = TopoDS::Wire(w.Current());
            int inWire = 0, walked = 0;
            for (TopExp_Explorer e(wire, TopAbs_EDGE); e.More(); e.Next())
                ++inWire;
            std::vector<std::vector<gp_Pnt2d>> loop;
            for (BRepTools_WireExplorer x(wire, face); x.More(); x.Next()) {
                ++walked;
                std::vector<gp_Pnt2d> at = chordsOf(x.Current());
                if (!at.empty())
                    loop.push_back(std::move(at));
            }
            if (walked == inWire) {
                for (std::size_t k = 0; k < loop.size(); ++k) {
                    for (std::size_t i = 0; i + 1 < loop[k].size(); ++i)
                        add(loop[k][i], loop[k][i + 1]);
                    add(loop[k].back(), loop[(k + 1) % loop.size()].front());
                }
            } else {
                for (TopExp_Explorer e(wire, TopAbs_EDGE); e.More(); e.Next()) {
                    const std::vector<gp_Pnt2d> at = chordsOf(TopoDS::Edge(e.Current()));
                    for (std::size_t i = 0; i + 1 < at.size(); ++i)
                        add(at[i], at[i + 1]);
                }
            }
        }
        if (segs.empty())
            return;
        low = vlo;
        bins = int(std::clamp<std::size_t>(segs.size() / 4, 1, 4096));
        width = std::max(vhi - vlo, 1e-300) / bins;
        inBin.assign(std::size_t(bins), {});
        for (std::size_t k = 0; k < segs.size(); ++k) {
            const int a = binOf(std::min(segs[k].v0, segs[k].v1)), b = binOf(std::max(segs[k].v0, segs[k].v1));
            for (int i = a; i <= b; ++i)
                inBin[std::size_t(i)].push_back(int(k));
        }
    }
    TopAbs_State operator()(double u, double v) const {
        if (segs.empty())
            return TopAbs_OUT;
        if (uPeriod > 0)
            u = uLow + std::fmod(std::fmod(u - uLow, uPeriod) + uPeriod, uPeriod);
        if (vPeriod > 0)
            v = vLow + std::fmod(std::fmod(v - vLow, vPeriod) + vPeriod, vPeriod);
        if (v < low || v > low + width * bins)
            return TopAbs_OUT;
        for (const int k : inBin[std::size_t(binOf(v))]) {
            // Right on the boundary: can't tell.
            const Seg &g = segs[std::size_t(k)];
            const double du = g.u1 - g.u0, dv = g.v1 - g.v0, len = du * du + dv * dv;
            const double t = len > 0 ? std::clamp(((u - g.u0) * du + (v - g.v0) * dv) / len, 0.0, 1.0) : 0;
            if (std::hypot(g.u0 + du * t - u, g.v0 + dv * t - v) <= tolOn)
                return TopAbs_ON;
        }
        // The crossings of a line along u through the point, either side of it. The point is farther than
        // tolOn from every chord, so moving it less than that along v can't change the answer; it is moved
        // so the line passes no chord's end: a curve's two ends (a circle's seam) differ by a rounding hair,
        // and a line right through them counts that curve once too often or not at all. (Two such miscounts
        // on one line even out, so the total's evenness can't catch them.) Should every place be crowded,
        // an odd total still gives "can't tell".
        constexpr double kShifts[] = {0, 0.5, -0.5, 0.25, -0.25, 0.375, -0.375, 0.125, -0.125, 0.4375, -0.4375, 0.3125, -0.3125, 0.1875, -0.1875, 0.0625, -0.0625};
        const double clear = tolOn * 1e-3;
        for (const double shift : kShifts) {
            const double at = v + shift * tolOn;
            const std::vector<int> &near = inBin[std::size_t(binOf(at))];
            const bool throughEnd = std::any_of(near.begin(), near.end(), [&](int k) {
                const Seg &g = segs[std::size_t(k)];
                return std::abs(g.v0 - at) <= clear || std::abs(g.v1 - at) <= clear;
            });
            if (throughEnd)
                continue;
            int after = 0, before = 0;
            for (const int k : near) {
                const Seg &g = segs[std::size_t(k)];
                if ((g.v0 > at) != (g.v1 > at))
                    ++(u < g.u0 + (at - g.v0) * (g.u1 - g.u0) / (g.v1 - g.v0) ? after : before);
            }
            if ((after + before) % 2 == 0)
                return after % 2 ? TopAbs_IN : TopAbs_OUT;
        }
        return TopAbs_ON;
    }

private:
    struct Seg {
        double u0, v0, u1, v1;
    };
    int binOf(double v) const { return std::clamp(int((v - low) / width), 0, bins - 1); }
    std::vector<Seg> segs;
    std::vector<std::vector<int>> inBin;
    double low = 0, width = 1, uLow = 0, vLow = 0, uPeriod = 0, vPeriod = 0, tolOn = 0;
    int bins = 1;
};

// Does a curved face fold over as seen straight down (its normal turning from up to down across it)?
// Only then does it have an outline of its own, beside its edges.
bool folds(const TopoDS_Face &face) {
    const BRepAdaptor_Surface s(face);
    switch (s.GetType()) {
    case GeomAbs_Plane:
        return false;
    case GeomAbs_Cylinder:
        if (std::abs(s.Cylinder().Axis().Direction().Z()) > 1 - 1e-12) return false;   // its walls stand straight up
        break;
    case GeomAbs_Cone:
        if (std::abs(s.Cone().Axis().Direction().Z()) > 1 - 1e-12) return false;       // its slope never turns over
        break;
    default:
        break;
    }
    double u0, u1, v0, v1;
    BRepTools::UVBounds(face, u0, u1, v0, v1);
    double low = 2, high = -2;
    constexpr int kSteps = 24;
    BRepLProp_SLProps props(s, 1, 1e-9);
    for (int i = 0; i <= kSteps; ++i)
        for (int j = 0; j <= kSteps; ++j) {
            props.SetParameters(u0 + (u1 - u0) * i / kSteps, v0 + (v1 - v0) * j / kSteps);
            if (!props.IsNormalDefined())
                continue;
            const double z = props.Normal().Z();
            low = std::min(low, z);
            high = std::max(high, z);
        }
    // Turning through level anywhere (or too little known to say): give it its outline.
    return low > high || (low < 1e-7 && high > -1e-7 && !(std::abs(low) < 1e-7 && std::abs(high) < 1e-7));
}

} // namespace

FlatPiece corner(const TopoDS_Vertex &vertex, const omamake::Placement &plane) {
    FlatPiece p;
    p.kind = FlatPiece::Point;
    p.a = two(BRep_Tool::Pnt(vertex).Transformed(toPlane(plane)));
    return p;
}

bool edge(const TopoDS_Edge &edge, const omamake::Placement &plane, std::vector<FlatPiece> &pieces, std::string &why) {
    try {
        const TopoDS_Edge e = TopoDS::Edge(onPlane(edge, toPlane(plane)));
        const Laid l = lay(e);
        if (l.kind == Laid::Gone) {
            why = "that edge can't be laid flat on this sketch";
            return false;
        }
        TopoDS_Vertex v0, v1;
        TopExp::Vertices(e, v0, v1);
        if (l.kind == Laid::Point) {
            FlatPiece p;
            p.kind = FlatPiece::Point;
            p.a = two(l.p);
            pieces.push_back(p);
            return true;
        }
        if (l.kind == Laid::Segment) {
            FlatPiece p;
            p.a = two(l.p);
            p.b = two(l.q);
            pieces.push_back(p);
            return true;
        }
        // Its own ends: the edge's corners laid flat.
        BRepBuilderAPI_MakeEdge flat(l.curve, l.first, l.last);
        if (!flat.IsDone()) {
            why = "that edge can't be laid flat on this sketch";
            return false;
        }
        Run r = runOf(flat.Edge());
        if (!v0.IsNull() && !v1.IsNull() && r.kind != FlatPiece::Circle && r.kind != FlatPiece::Ellipse) {
            r.from = two(down(BRep_Tool::Pnt(v0)));
            r.to = two(down(BRep_Tool::Pnt(v1)));
            if (r.kind == FlatPiece::Arc) {
                r.from = ontoCircle(r, r.from);
                r.to = ontoCircle(r, r.to);
            }
        }
        return finish({r}, pieces, why);
    } catch (const Standard_Failure &failure) {
        why = std::string("that edge can't be laid flat (") + failure.GetMessageString() + ")";
        return false;
    }
}

bool shadow(const std::vector<TopoDS_Shape> &shapes, const omamake::Placement &plane,
            std::vector<std::vector<FlatPiece>> &loops, std::string &why) {
    try {
        stage("start");
        const gp_Trsf t = toPlane(plane);
        TopoDS_Compound all;
        BRep_Builder build;
        build.MakeCompound(all);
        for (const TopoDS_Shape &s : shapes)
            build.Add(all, onPlane(s, t));

        // 1. Every edge laid flat. Edges that met at a corner meet at that corner laid flat: on a model
        //    whose faces meet loosely the curve's own end can be a little off it, and the corner's
        //    tolerance takes that in, just where it is (the model's own tolerance says it may).
        TopTools_IndexedMapOfShape edges, corners;
        TopExp::MapShapes(all, TopAbs_EDGE, edges);
        TopExp::MapShapes(all, TopAbs_VERTEX, corners);
        TopTools_IndexedDataMapOfShapeListOfShape facesOf;
        TopExp::MapShapesAndAncestors(all, TopAbs_EDGE, TopAbs_FACE, facesOf);
        std::vector<Laid> laid(std::size_t(edges.Extent()));
        std::vector<double> reach(std::size_t(corners.Extent()), Precision::Confusion());
        for (int i = 1; i <= edges.Extent(); ++i) {
            const TopoDS_Edge &e = TopoDS::Edge(edges(i));
            Laid &l = laid[std::size_t(i - 1)];
            l = lay(e);
            if (l.kind != Laid::Curve)
                continue;
            TopoDS_Vertex v0, v1;
            TopExp::Vertices(e, v0, v1);
            if (v0.IsNull() || v1.IsNull())
                continue;
            const auto stretch = [&](const TopoDS_Vertex &v, double at) {
                double &r = reach[std::size_t(corners.FindIndex(v) - 1)];
                r = std::max(r, down(BRep_Tool::Pnt(v)).Distance(l.curve->Value(at)) * 1.001 + 1e-9);
            };
            stretch(v0, l.first);
            stretch(v1, l.last);
        }
        std::vector<TopoDS_Vertex> flatCorner(std::size_t(corners.Extent()));
        for (int i = 1; i <= corners.Extent(); ++i)
            build.MakeVertex(flatCorner[std::size_t(i - 1)], down(BRep_Tool::Pnt(TopoDS::Vertex(corners(i)))), reach[std::size_t(i - 1)]);
        TopTools_ListOfShape tools;
        Bnd_Box box;
        int left = 0;
        // How loose the model is where each flat edge came from: its edge's and corners' tolerances, or
        // how far the edge's curves on its faces really stray from it, if more (a model can claim less
        // than it has; the faces end where those curves say, so the true boundary lies anywhere that
        // close to the edge).
        std::map<const void *, double> looseOf;
        double looseNow = Precision::Confusion();
        const auto add = [&](const TopoDS_Edge &flat) {
            looseOf[flat.TShape().get()] = looseNow;
            tools.Append(flat);
            BRepBndLib::Add(flat, box, Standard_False);
        };
        std::vector<TopoDS_Edge> flatOf(std::size_t(edges.Extent()));    // each model edge laid flat (null: none)
        for (int i = 1; i <= edges.Extent(); ++i) {
            const TopoDS_Edge &e = TopoDS::Edge(edges(i));
            const Laid &l = laid[std::size_t(i - 1)];
            if (l.kind == Laid::Gone && !BRep_Tool::Degenerated(e)) {
                ++left;
                continue;
            }
            if (l.kind == Laid::Gone || l.kind == Laid::Point)
                continue;
            looseNow = BRep_Tool::Tolerance(e);
            for (TopoDS_Iterator v(e); v.More(); v.Next())
                if (v.Value().ShapeType() == TopAbs_VERTEX)
                    looseNow = std::max(looseNow, BRep_Tool::Tolerance(TopoDS::Vertex(v.Value())));
            if (const TopTools_ListOfShape *faces = facesOf.Seek(e)) {
                ShapeAnalysis_Edge check;
                for (const TopoDS_Shape &f : *faces) {
                    double strays = 0;
                    check.CheckSameParameter(e, TopoDS::Face(f), strays);
                    looseNow = std::max(looseNow, strays);
                }
            }
            TopoDS_Vertex v0, v1;
            TopExp::Vertices(e, v0, v1);
            const auto mine = [&](const TopoDS_Vertex &v, const gp_Pnt &at) {
                if (!v.IsNull() && down(BRep_Tool::Pnt(v)).Distance(at) <= reach[std::size_t(corners.FindIndex(v) - 1)])
                    return flatCorner[std::size_t(corners.FindIndex(v) - 1)];
                TopoDS_Vertex own;
                build.MakeVertex(own, at, Precision::Confusion());
                return own;
            };
            if (l.kind == Laid::Segment) {
                const TopoDS_Vertex a = mine(v0, l.p), b = mine(v1, l.q);
                const gp_Pnt pa = BRep_Tool::Pnt(a), pb = BRep_Tool::Pnt(b);
                if (pa.Distance(pb) < kSame)
                    continue;
                BRepBuilderAPI_MakeEdge make(a, b);
                if (!make.IsDone()) { ++left; continue; }
                add(make.Edge());
                flatOf[std::size_t(i - 1)] = make.Edge();
            } else {
                const TopoDS_Vertex a = mine(v0, l.curve->Value(l.first));
                const TopoDS_Vertex b = v0.IsSame(v1) ? a : mine(v1, l.curve->Value(l.last));
                BRepBuilderAPI_MakeEdge make(l.curve, a, b, l.first, l.last);
                if (!make.IsDone()) { ++left; continue; }
                add(make.Edge());
                flatOf[std::size_t(i - 1)] = make.Edge();
            }
        }
        if (left) {
            why = std::to_string(left) + " of the model's edges couldn't be laid flat";
            return false;
        }

        stage("edges laid flat");
        // 2. The outline of every curved face that folds over (a cylinder lying down, a ball), exactly.
        TopoDS_Compound folding;
        build.MakeCompound(folding);
        int foldCount = 0;
        for (TopExp_Explorer f(all, TopAbs_FACE); f.More(); f.Next())
            if (folds(TopoDS::Face(f.Current()))) {
                build.Add(folding, f.Current());
                ++foldCount;
            }
        // One face that doesn't fold over as seen from the sketch covers its own area once: its shadow is
        // its own boundary, each wire laid flat in the wire's order, the outer one going round
        // anticlockwise and the holes the other way. Exact whatever the model's looseness, and nothing
        // to work out. (Seen edge on it covers nothing: that is left to the general way below.)
        if (!foldCount && shapes.size() == 1 && shapes[0].ShapeType() == TopAbs_FACE) {
            const TopoDS_Face face = TopoDS::Face(TopExp_Explorer(all, TopAbs_FACE).Current());
            const TopoDS_Wire outer = BRepTools::OuterWire(face);
            std::vector<std::vector<FlatPiece>> own;
            double outerArea = 0;
            bool whole = true;
            for (TopExp_Explorer w(face, TopAbs_WIRE); w.More() && whole; w.Next()) {
                std::vector<Run> runs;
                double area = 0;
                for (BRepTools_WireExplorer x(TopoDS::Wire(w.Current()), face); x.More(); x.Next()) {
                    const TopoDS_Edge &e = x.Current();
                    if (BRep_Tool::Degenerated(e) || BRep_Tool::IsClosed(e, face))
                        continue;           // a seam: crossed there and back, it bounds nothing
                    const int i = edges.FindIndex(e);
                    if (i == 0 || flatOf[std::size_t(i - 1)].IsNull())
                        continue;           // seen end on: a point
                    runs.push_back(runOf(TopoDS::Edge(flatOf[std::size_t(i - 1)].Oriented(e.Orientation()))));
                    area += sweep(runs.back());
                }
                if (runs.empty())
                    continue;
                const bool isOuter = w.Current().IsSame(outer);
                if ((area > 0) != isOuter) {
                    std::reverse(runs.begin(), runs.end());
                    for (Run &r : runs)
                        r = reversed(r);
                }
                if (isOuter)
                    outerArea = std::abs(area) / 2;
                std::vector<FlatPiece> pieces;
                whole = finish(runs, pieces, why);
                if (whole && !pieces.empty())
                    own.insert(isOuter ? own.begin() : own.end(), std::move(pieces));
            }
            if (whole && !own.empty() && outerArea > kExactJoin) {
                loops = std::move(own);
                stage("one face: its own wires");
                return true;
            }
            why.clear();
        }
        if (foldCount) {
            std::vector<TopoDS_Edge> outlineEdges;
            Handle(HLRBRep_Algo) hlr = new HLRBRep_Algo();
            hlr->Add(folding);
            hlr->Projector(HLRAlgo_Projector(gp_Ax2(gp::Origin(), gp::DZ(), gp::DX())));
            hlr->Update();
            stage("outlines found");
            // Every outline, hidden or not, is wanted (the side test decides what is covered), so nothing is
            // hidden: working out what hides what across hundreds of faces is most of HLR's time.
            hlr->ShowAll();
            HLRBRep_HLRToShape outlines(hlr);
            for (const TopoDS_Shape &part : {outlines.OutLineVCompound(), outlines.OutLineHCompound()})
                if (!part.IsNull() && BRepLib::BuildCurves3d(part, 1e-7, GeomAbs_C2, 14, 0))  // they come with flat curves only
                    for (TopExp_Explorer x(part, TopAbs_EDGE); x.More(); x.Next())
                        outlineEdges.push_back(TopoDS::Edge(x.Current()));
            stage("outlines given 3D curves");
            // An outline that only runs along edges already laid flat (a round meeting a wall just at
            // level, or a wall that leans a hair either way) adds nothing: within the model's own
            // tolerance there it is that edge. Kept, it would lie a hair beside it and cut slivers.
            // The flat edges' boxes, worked out once and sorted, so each point below only looks at the edges
            // that could be within reach of it (the farthest any reach goes: kOutlineReach or the loosest edge).
            std::vector<TopoDS_Edge> flatEdges;
            for (const TopoDS_Shape &t : tools)
                flatEdges.push_back(TopoDS::Edge(t));
            Bnd_BoundSortBox sorted;
            {
                double widest = kOutlineReach;
                for (const auto &[shape, loose] : looseOf)
                    widest = std::max(widest, loose);
                Handle(Bnd_HArray1OfBox) boxes = new Bnd_HArray1OfBox(1, std::max<int>(1, int(flatEdges.size())));
                Bnd_Box all;
                for (std::size_t k = 0; k < flatEdges.size(); ++k) {
                    Bnd_Box b;
                    BRepBndLib::Add(flatEdges[k], b, Standard_False);
                    b.Enlarge(widest);
                    boxes->SetValue(int(k) + 1, b);
                    all.Add(b);
                }
                if (!flatEdges.empty())
                    sorted.Initialize(all, boxes);
            }
            const auto nearby = [&](const gp_Pnt &p) -> std::vector<const TopoDS_Edge *> {
                std::vector<const TopoDS_Edge *> got;
                if (flatEdges.empty())
                    return got;
                Bnd_Box at;
                at.Add(p);
                for (TColStd_ListOfInteger::Iterator i(sorted.Compare(at)); i.More(); i.Next())
                    got.push_back(&flatEdges[std::size_t(i.Value() - 1)]);
                return got;
            };
            const auto distanceToEdges = [&](const gp_Pnt &p, double reachAt) {
                double best = std::numeric_limits<double>::max();
                for (const TopoDS_Edge *near : nearby(p)) {
                    const TopoDS_Edge &e = *near;
                    const auto loose = looseOf.find(e.TShape().get());
                    const double reach = std::max(reachAt, loose == looseOf.end() ? 0.0 : loose->second);
                    Bnd_Box b;
                    BRepBndLib::Add(e, b, Standard_False);
                    b.Enlarge(reach);
                    if (b.IsOut(p))
                        continue;
                    double f, l;
                    const Handle(Geom_Curve) c = BRep_Tool::Curve(e, f, l);
                    if (c.IsNull())
                        continue;
                    const gp_Trsf place = e.Location().Transformation();
                    GeomAPI_ProjectPointOnCurve on(p.Transformed(place.Inverted()), c, f, l);
                    double d = std::min(c->Value(f).Transformed(place).Distance(p), c->Value(l).Transformed(place).Distance(p));
                    if (on.NbPoints() > 0)
                        d = std::min(d, on.LowerDistance());
                    if (d <= reach)
                        best = std::min(best, d);
                }
                return best;
            };
            {
                std::vector<TopoDS_Edge> real;
                for (const TopoDS_Edge &o : outlineEdges) {
                    const BRepAdaptor_Curve c(o);
                    bool along = true;
                    for (int k = 0; k <= 32 && along; ++k)
                        along = distanceToEdges(c.Value(c.FirstParameter() + (c.LastParameter() - c.FirstParameter()) * k / 32), kOutlineReach)
                                < std::numeric_limits<double>::max();
                    if (!along)
                        real.push_back(o);
                }
                outlineEdges = real;
            }
            stage("outlines along edges dropped");
            // An outline ends where its face does, worked out on the face's surface; the edge there can be off
            // that by a hair (a few 1e-7), or on a loose model by as much as the model says it is loose
            // (its edge's tolerance). Its end is bridged to the nearest point of the flat edges within
            // that, so the shadow's rim has no gap there.
            for (const TopoDS_Edge &o : outlineEdges) {
                TopoDS_Vertex ends[2];
                TopExp::Vertices(o, ends[0], ends[1]);
                for (const TopoDS_Vertex &v : ends) {
                    if (v.IsNull())
                        continue;
                    const gp_Pnt p = BRep_Tool::Pnt(v);
                    double best = std::numeric_limits<double>::max();
                    gp_Pnt to;
                    bool found = false;
                    const auto look = [&](const TopoDS_Edge &e) {
                        if (e.IsSame(o))
                            return;
                        const auto loose = looseOf.find(e.TShape().get());
                        const double reach = std::max(kOutlineReach, loose == looseOf.end() ? 0.0 : loose->second);
                        Bnd_Box b;
                        BRepBndLib::Add(e, b, Standard_False);
                        b.Enlarge(std::min(best, reach));
                        if (b.IsOut(p))
                            return;
                        double f, l;
                        const Handle(Geom_Curve) c = BRep_Tool::Curve(e, f, l);
                        if (c.IsNull())
                            return;
                        gp_Pnt local = p.Transformed(e.Location().Transformation().Inverted());
                        GeomAPI_ProjectPointOnCurve on(local, c, f, l);
                        const auto consider = [&](const gp_Pnt &q) {
                            const gp_Pnt g = q.Transformed(e.Location().Transformation());
                            if (g.Distance(p) < best && g.Distance(p) <= reach) { best = g.Distance(p); to = g; found = true; }
                        };
                        if (on.NbPoints() > 0)
                            consider(on.NearestPoint());
                        consider(c->Value(f));
                        consider(c->Value(l));
                    };
                    for (const TopoDS_Edge *e : nearby(p))
                        look(*e);
                    for (const TopoDS_Edge &e : outlineEdges)
                        look(e);
                    // Joined the way corners are: the end's tolerance takes the gap in.
                    if (found && best > BRep_Tool::Tolerance(v))
                        build.UpdateVertex(v, best * 1.001 + 1e-9);
                }
            }
            for (const TopoDS_Edge &o : outlineEdges)
                add(o);

        }
        stage("curved faces' outlines");
        if (tools.IsEmpty() || box.IsVoid()) {
            why = "the model throws nothing on this sketch";
            return false;
        }

        double x0, y0, z0, x1, y1, z1;
        box.Get(x0, y0, z0, x1, y1, z1);

        // Only edges that can be on the outline go on: those that come within reach of the shadow the
        // drawn triangles throw, which is never farther from the true one than the triangles stray from
        // the true surface. The rest lie well inside it and only make the cutting slow (a thread's
        // turns all on one circle). What is kept is still each exact edge, whole.
        std::vector<std::vector<projection::Flat>> rough;
        double reachOut = 0;
        std::map<std::pair<long long, long long>, std::vector<std::pair<projection::Flat, projection::Flat>>> grid;
        const auto key = [&](double x, double y) { return std::pair{(long long)std::floor(x / reachOut), (long long)std::floor(y / reachOut)}; };
        const auto near = [&](const gp_Pnt &p) {
            const auto [gx, gy] = key(p.X(), p.Y());
            for (long long dx = -1; dx <= 1; ++dx)
                for (long long dy = -1; dy <= 1; ++dy) {
                    const auto it = grid.find({gx + dx, gy + dy});
                    if (it == grid.end())
                        continue;
                    for (const auto &[a, b] : it->second) {
                        const double ux = b.u - a.u, uy = b.v - a.v, L = ux * ux + uy * uy;
                        const double s = L > 0 ? std::clamp(((p.X() - a.u) * ux + (p.Y() - a.v) * uy) / L, 0.0, 1.0) : 0;
                        if (std::hypot(a.u + ux * s - p.X(), a.v + uy * s - p.Y()) <= reachOut)
                            return true;
                    }
                }
            return false;
        };
        // Inside the drawn shadow (even-odd over its outlines)?
        const auto insideRough = [&](const gp_Pnt &p) {
            bool in = false;
            for (const auto &loop : rough)
                for (std::size_t i = 0, j = loop.size() - 1; i < loop.size(); j = i++)
                    if ((loop[i].v > p.Y()) != (loop[j].v > p.Y())
                        && p.X() < loop[i].u + (p.Y() - loop[i].v) * (loop[j].u - loop[i].u) / (loop[j].v - loop[i].v))
                        in = !in;
            return in;
        };
        {
            std::vector<std::array<projection::Flat, 3>> drawn;
            double stray = std::clamp(std::hypot(std::hypot(x1 - x0, y1 - y0), z1 - z0) / 50000, 0.01, 0.5);
            bool meshed = false;
            for (TopExp_Explorer f(all, TopAbs_FACE); f.More(); f.Next()) {
                TopLoc_Location loc;
                Handle(Poly_Triangulation) tri = BRep_Tool::Triangulation(TopoDS::Face(f.Current()), loc);
                if (tri.IsNull() && !meshed) {
                    BRepMesh_IncrementalMesh(all, stray, false, 0.5, true);
                    meshed = true;
                    tri = BRep_Tool::Triangulation(TopoDS::Face(f.Current()), loc);
                }
                if (tri.IsNull())
                    continue;
                stray = std::max(stray, tri->Deflection());
                const gp_Trsf place = loc.Transformation();
                for (int k = 1; k <= tri->NbTriangles(); ++k) {
                    int a, b, c;
                    tri->Triangle(k).Get(a, b, c);
                    const gp_Pnt pa = tri->Node(a).Transformed(place), pb = tri->Node(b).Transformed(place), pc = tri->Node(c).Transformed(place);
                    drawn.push_back({projection::Flat{pa.X(), pa.Y()}, projection::Flat{pb.X(), pb.Y()}, projection::Flat{pc.X(), pc.Y()}});
                }
            }
            rough = projection::covering(drawn, 1e-6);
            reachOut = 3 * stray + 1e-4;
            if (!rough.empty()) {
                for (const auto &loop : rough)
                    for (std::size_t i = 0; i < loop.size(); ++i) {
                        const projection::Flat &a = loop[i], &b = loop[(i + 1) % loop.size()];
                        const double l = std::hypot(b.u - a.u, b.v - a.v);
                        const int steps = std::max(1, int(std::ceil(l / reachOut)));
                        std::set<std::pair<long long, long long>> cellsOn;
                        for (int k = 0; k <= steps; ++k)
                            cellsOn.insert(key(a.u + (b.u - a.u) * k / steps, a.v + (b.v - a.v) * k / steps));
                        for (const auto &c : cellsOn)
                            grid[c].push_back({a, b});
                    }
                TopTools_ListOfShape onOutline;
                for (const TopoDS_Shape &e : tools) {
                    const BRepAdaptor_Curve c(TopoDS::Edge(e));
                    const double f = c.FirstParameter(), l = c.LastParameter();
                    double length = 0;
                    gp_Pnt last = c.Value(f);
                    for (int k = 1; k <= 64; ++k) {
                        const gp_Pnt p = c.Value(f + (l - f) * k / 64);
                        length += p.Distance(last);
                        last = p;
                    }
                    const int steps = std::clamp(int(std::ceil(length * 1.5 / (reachOut / 2))) + 1, 2, 400000);
                    bool keep = false;
                    for (int k = 0; k <= steps && !keep; ++k)
                        keep = near(c.Value(f + (l - f) * k / steps));
                    if (keep)
                        onOutline.Append(e);
                }
                tools = onOutline;
            }
        }
        stage("edges near the outline");

        // 3. Each flat edge cut where another crosses it, touches it, or ends on it (within how loose
        //    the model is there), into pieces that have the same shadow along their whole length.
        struct Flat {
            TopoDS_Edge edge;
            Handle(Geom_Curve) curve;      // located, in the sketch plane (z 0)
            Handle(Geom2d_Curve) flat;     // the same, as a 2D curve
            double f = 0, l = 0;
            double loose = Precision::Confusion();
            Bnd_Box box;
            std::vector<double> cuts;
        };
        std::vector<Flat> flats;
        for (const TopoDS_Shape &t : tools) {
            Flat x;
            x.edge = TopoDS::Edge(t);
            TopLoc_Location loc;
            x.curve = BRep_Tool::Curve(x.edge, loc, x.f, x.l);
            if (x.curve.IsNull() || x.l - x.f <= 0)
                continue;
            if (!loc.IsIdentity())
                x.curve = Handle(Geom_Curve)::DownCast(x.curve->Transformed(loc.Transformation()));
            x.flat = GeomAPI::To2d(x.curve, gp_Pln(gp::XOY()));
            if (x.flat.IsNull())
                continue;
            const auto loose = looseOf.find(x.edge.TShape().get());
            x.loose = loose == looseOf.end() ? Precision::Confusion() : std::max(loose->second, Precision::Confusion());
            BRepBndLib::Add(x.edge, x.box, Standard_False);
            x.box.Enlarge(x.loose);
            x.cuts = {x.f, x.l};
            // A curve seen nearly end on can fold back on itself, laid flat (out, and back over itself):
            // the shadow changes sides there, so a fold cuts it too. Found where the flat tangent turns
            // back between two neighbouring samples, then exactly where its speed is least between them.
            {
                const Geom2dAdaptor_Curve along(x.flat, x.f, x.l);
                const GeomAbs_CurveType type = along.GetType();
                if (type != GeomAbs_Line && type != GeomAbs_Circle && type != GeomAbs_Ellipse) {
                    std::vector<double> stretch{x.f};
                    const int nb = along.NbIntervals(GeomAbs_C2);
                    if (nb > 1) {
                        TColStd_Array1OfReal at(1, nb + 1);
                        along.Intervals(at, GeomAbs_C2);
                        for (int i = 2; i <= nb; ++i)
                            if (at(i) > stretch.back() && at(i) < x.l) stretch.push_back(at(i));
                    }
                    stretch.push_back(x.l);
                    const auto speed2 = [&](double t) { gp_Pnt2d p; gp_Vec2d d; x.flat->D1(t, p, d); return d.SquareMagnitude(); };
                    for (std::size_t k = 0; k + 1 < stretch.size(); ++k) {
                        constexpr int kSteps = 32;
                        gp_Vec2d last;
                        double lastT = stretch[k];
                        for (int i = 0; i <= kSteps; ++i) {
                            const double t = stretch[k] + (stretch[k + 1] - stretch[k]) * i / kSteps;
                            gp_Pnt2d p;
                            gp_Vec2d d;
                            x.flat->D1(t, p, d);
                            if (i > 0 && last.Dot(d) < 0) {
                                double a = lastT, b = t;            // least speed between: golden section
                                const double g = (std::sqrt(5.0) - 1) / 2;
                                for (int it = 0; it < 100 && b - a > 1e-15 * std::max(1.0, std::abs(a)); ++it) {
                                    const double c = b - g * (b - a), e = a + g * (b - a);
                                    if (speed2(c) < speed2(e)) b = e; else a = c;
                                }
                                x.cuts.push_back((a + b) / 2);
                            }
                            last = d;
                            lastT = t;
                        }
                    }
                }
            }
            flats.push_back(std::move(x));
        }
        long crossings = 0;
        std::set<std::pair<std::size_t, std::size_t>> along;   // edges that lie along each other somewhere
        {
            // Every pair whose boxes meet, worked out side by side (each pair on its own; a thread's flanks
            // laid flat cross hundreds of times), then taken in, in order.
            // A long spline (a thread's flank, thousands of poles) goes in stretches of 16 knot spans, each
            // with its own box, so only stretches that come near each other are worked on.
            struct Stretch {
                std::size_t edge = 0;
                double t0 = 0, t1 = 0;
                Bnd_Box2d box;
            };
            std::vector<std::vector<Stretch>> stretches(flats.size());
            for (std::size_t i = 0; i < flats.size(); ++i) {
                const Flat &F = flats[i];
                std::vector<double> at{F.f};
                if (const Handle(Geom2d_BSplineCurve) b = Handle(Geom2d_BSplineCurve)::DownCast(F.flat); !b.IsNull() && b->NbKnots() > 32)
                    for (int k = 1; k <= b->NbKnots(); k += 16)
                        if (b->Knot(k) > at.back() && b->Knot(k) < F.l)
                            at.push_back(b->Knot(k));
                at.push_back(F.l);
                for (std::size_t k = 0; k + 1 < at.size(); ++k) {
                    Stretch st;
                    st.edge = i;
                    st.t0 = at[k];
                    st.t1 = at[k + 1];
                    BndLib_Add2dCurve::Add(Geom2dAdaptor_Curve(F.flat, st.t0, st.t1), 0.0, st.box);
                    st.box.Enlarge(2 * Precision::Confusion());
                    stretches[i].push_back(st);
                }
            }
            struct Meeting {
                std::size_t i = 0, j = 0;
                double ti0 = 0, ti1 = 0, tj0 = 0, tj1 = 0;
                std::vector<std::pair<double, double>> points;  // parameters on i and on j
                bool alongside = false;
            };
            std::vector<Meeting> pairs;
            for (std::size_t i = 0; i < flats.size(); ++i)
                for (std::size_t j = i + 1; j < flats.size(); ++j) {
                    if (flats[i].box.IsOut(flats[j].box))
                        continue;
                    for (const Stretch &si : stretches[i])
                        for (const Stretch &sj : stretches[j])
                            if (!si.box.IsOut(sj.box))
                                pairs.push_back({i, j, si.t0, si.t1, sj.t0, sj.t1, {}, false});
                }
            OSD_Parallel::For(0, int(pairs.size()), [&](int k) {
                Meeting &w = pairs[std::size_t(k)];
                const Flat &A = flats[w.i], &B = flats[w.j];
                const Handle(Geom2d_TrimmedCurve) ta = new Geom2d_TrimmedCurve(A.flat, w.ti0, w.ti1);
                const Handle(Geom2d_TrimmedCurve) tb = new Geom2d_TrimmedCurve(B.flat, w.tj0, w.tj1);
                Geom2dAPI_InterCurveCurve meet(ta, tb, Precision::Confusion());
                const Geom2dInt_GInter &found = meet.Intersector();
                if (!found.IsDone())
                    return;
                for (int n = 1; n <= found.NbPoints(); ++n)
                    w.points.push_back({found.Point(n).ParamOnFirst(), found.Point(n).ParamOnSecond()});
                w.alongside = found.NbSegments() > 0;
                for (int n = 1; n <= found.NbSegments(); ++n) {
                    const IntRes2d_IntersectionSegment &seg = found.Segment(n);
                    for (const IntRes2d_IntersectionPoint *pt : {seg.HasFirstPoint() ? &seg.FirstPoint() : nullptr, seg.HasLastPoint() ? &seg.LastPoint() : nullptr})
                        if (pt)
                            w.points.push_back({pt->ParamOnFirst(), pt->ParamOnSecond()});
                }
            });
            for (const Meeting &w : pairs) {
                for (const auto &[ti, tj] : w.points) {
                    flats[w.i].cuts.push_back(ti);
                    flats[w.j].cuts.push_back(tj);
                }
                crossings += long(w.points.size());
                if (w.alongside) {
                    along.insert({w.i, w.j});
                    along.insert({w.j, w.i});
                }
            }
        }
        stage("  crossings");
        // An end of one edge on (or, on a loose model, within its looseness of) another: cut there too.
        for (std::size_t i = 0; i < flats.size(); ++i)
            for (const double t : {flats[i].f, flats[i].l}) {
                const gp_Pnt p = flats[i].curve->Value(t);
                for (std::size_t j = 0; j < flats.size(); ++j) {
                    Flat &B = flats[j];
                    if (j == i || B.box.IsOut(p))
                        continue;
                    GeomAPI_ProjectPointOnCurve on(p, B.curve, B.f, B.l);
                    if (on.NbPoints() > 0 && on.LowerDistance() <= std::max(flats[i].loose, B.loose)) {
                        B.cuts.push_back(on.LowerDistanceParameter());
                        along.insert({i, j});
                        along.insert({j, i});
                    }
                }
            }
        stage("  ends on edges");
        // Edges lying on each other (a row of circles seen edge on, all on one line) must be cut at the same
        // places, or their pieces overlap without matching and none can stand for the other: every cut of
        // one edge (where something crossed or ended on it) also cuts each other edge it lies along, within
        // how loose the model is there. (Edges that only cross already share that cut.)
        {
            std::vector<std::vector<double>> was(flats.size());
            for (std::size_t i = 0; i < flats.size(); ++i)
                was[i] = flats[i].cuts;
            for (const auto &[i, j] : along)
                for (const double t : was[i]) {
                    const gp_Pnt p = flats[i].curve->Value(t);
                    {
                        Flat &B = flats[j];
                        if (B.box.IsOut(p))
                            continue;
                        GeomAPI_ProjectPointOnCurve on(p, B.curve, B.f, B.l);
                        if (on.NbPoints() > 0 && on.LowerDistance() <= std::max(flats[i].loose, B.loose))
                            B.cuts.push_back(on.LowerDistanceParameter());
                    }
                }
        }
        stage("edges cut where they meet");

        // 4. Which pieces are outline: a line straight through a point just to each side of the piece's
        //    middle meets the model on one side only. The point is clear of the model's looseness there,
        //    so the side is the side, and the model is asked with no tolerance at its faces' boundaries.
        struct FaceTest {
            TopoDS_Face face;
            Bnd_Box box;
            Handle(Geom_Surface) surface;   // untrimmed, in the face's place
            bool plane = false;
            bool upright = false;           // stands straight up: seen edge on, covers nothing
            gp_Pln pln;
            std::shared_ptr<FaceInside> inside;   // made when first asked
        };
        std::vector<FaceTest> faceTests;
        for (TopExp_Explorer f(all, TopAbs_FACE); f.More(); f.Next()) {
            FaceTest t;
            t.face = TopoDS::Face(f.Current());
            // From the exact surfaces: a box from the drawn triangles is smaller wherever their chords cut
            // inside a curved edge, and would hide the face from points just inside that edge.
            BRepBndLib::Add(t.face, t.box, Standard_False);
            t.box.Enlarge(Precision::Confusion());
            {
                TopLoc_Location at;
                const Handle(Geom_Surface) own = BRep_Tool::Surface(t.face, at);
                t.surface = Handle(Geom_Surface)::DownCast(own->Transformed(at.Transformation()));
                const GeomAdaptor_Surface kind(t.surface);
                if (kind.GetType() == GeomAbs_Plane) {
                    t.plane = true;
                    t.pln = kind.Plane();
                }
                // A wall swept straight up (a round one, or any profile drawn up) is only a curve from
                // above, like a flat face seen edge on. A line straight down runs along it rather than
                // through it, which also makes meeting it slow to work out.
                const auto up = [](const gp_Dir &d) { return std::abs(d.Z()) > 1 - 1e-12; };
                if (kind.GetType() == GeomAbs_Cylinder)
                    t.upright = up(kind.Cylinder().Axis().Direction());
                else if (kind.GetType() == GeomAbs_SurfaceOfExtrusion)
                    t.upright = up(kind.Direction());
            }
            faceTests.push_back(std::move(t));
        }
        // Any face the line is inside answers, so the quick ones are asked first: flat faces, then the
        // simple round ones, the free-form ones (a thread's flanks) last.
        {
            const auto cost = [](const TopoDS_Face &f) {
                switch (BRepAdaptor_Surface(f, Standard_False).GetType()) {
                case GeomAbs_Plane: return 0;
                case GeomAbs_Cylinder: case GeomAbs_Cone: case GeomAbs_Sphere: case GeomAbs_Torus: return 1;
                default: return 2;
                }
            };
            std::stable_sort(faceTests.begin(), faceTests.end(), [&](const FaceTest &a, const FaceTest &b) { return cost(a.face) < cost(b.face); });
        }
        // 1: the model is there (inside a face); 0: not; -1: only on a face's boundary, can't tell.
        const auto meetsModel = [&](const gp_Pnt &m) {
            int on = 0;
            const gp_Lin line(m, gp::DZ());
            for (FaceTest &t : faceTests) {
                if (t.upright || t.box.IsOut(gp_Pnt(m.X(), m.Y(), t.box.CornerMin().Z())))
                    continue;
                // Where the line meets the face's whole surface, then whether that is inside the face,
                // judged on the face's own boundary curves (FaceInside).
                // Inside, not merely on its boundary: a side point a hair outside can read as on it.
                if (!t.inside)
                    t.inside = std::make_shared<FaceInside>(t.face);
                const auto judge = [&](double u, double v) { return (*t.inside)(u, v); };
                if (t.plane) {
                    const gp_Dir n = t.pln.Axis().Direction();
                    if (std::abs(n.Z()) < 1e-12)
                        continue;   // seen edge on: covers nothing
                    const double z = t.pln.Location().Z() - ((m.X() - t.pln.Location().X()) * n.X() + (m.Y() - t.pln.Location().Y()) * n.Y()) / n.Z();
                    double u, v;
                    ElSLib::Parameters(t.pln, gp_Pnt(m.X(), m.Y(), z), u, v);
                    const TopAbs_State st = judge(u, v);
                    if (st == TopAbs_IN)
                        return 1;
                    if (st == TopAbs_ON)
                        on = -1;
                    continue;
                }
                GeomAPI_IntCS hits(new Geom_Line(line), t.surface);
                if (!hits.IsDone())
                    continue;
                for (int k = 1; k <= hits.NbPoints(); ++k) {
                    double u, v, w;
                    hits.Parameters(k, u, v, w);
                    const TopAbs_State st = judge(u, v);
                    if (st == TopAbs_IN)
                        return 1;
                    if (st == TopAbs_ON)
                        on = -1;
                }
            }
            return on;
        };
        const auto covered = [&](const gp_Pnt &m) { return !rough.empty() && !near(m) ? int(insideRough(m)) : meetsModel(m); };
        struct Bit {
            Run run;
            gp_Pnt2d from, to, middle;
            double loose = 0;
            Handle(Geom_Curve) curve;    // its flat curve, from ta to tb, travelled forward or back
            double ta = 0, tb = 0, tm = 0;  // tm: its middle along its length
            bool forward = true;
            double length = 0;           // about (through its middle)
        };
        std::vector<Bit> bits;
        int pieceCount = 0;
        for (Flat &x : flats) {
            std::sort(x.cuts.begin(), x.cuts.end());
            std::vector<double> at;
            for (const double t : x.cuts) {
                const double c = std::clamp(t, x.f, x.l);
                // Two cuts closer than the edge's own looseness are one place where it meets something
                // (on a loose model a crossing comes twice, a hair apart): a piece between them would be
                // a crumb with no shape of its own.
                // (Judged by the curve between them too: a closed edge starts and ends at one point.)
                const double tol = std::max(kSame, x.loose);
                if (at.empty() || x.curve->Value(c).Distance(x.curve->Value(at.back())) > tol ||
                    x.curve->Value((c + at.back()) / 2).Distance(x.curve->Value(c)) > tol)
                    at.push_back(c);
            }
            if (at.size() < 2)
                continue;
            at.back() = x.l;
            const GeomAdaptor_Curve kind(x.curve);
            const bool whole = at.size() == 2 && x.curve->Value(x.f).Distance(x.curve->Value(x.l)) <= kExactJoin;
            const double side = std::max(10 * x.loose, kSideStep);
            for (std::size_t k = 0; k + 1 < at.size(); ++k) {
                const double ta = at[k], tb = at[k + 1];
                // Its middle along its length, not its parameter: a curve seen nearly end on crawls through
                // most of its parameter in a hair of length (and may scribble there, within the model's
                // looseness), so the parameter's middle can sit at the piece's very end, facing back.
                double tm = (ta + tb) / 2;
                if (kind.GetType() != GeomAbs_Line && kind.GetType() != GeomAbs_Circle) {
                    // (Only a point to look from: 64 chords find it well enough.)
                    constexpr int kChords = 64;
                    double run[kChords + 1] = {0};
                    gp_Pnt last = x.curve->Value(ta);
                    for (int i = 1; i <= kChords; ++i) {
                        const gp_Pnt q = x.curve->Value(ta + (tb - ta) * i / kChords);
                        run[i] = run[i - 1] + q.Distance(last);
                        last = q;
                    }
                    const double half = run[kChords] / 2;
                    for (int i = 1; i <= kChords && run[kChords] > 0; ++i)
                        if (run[i] >= half) {
                            const double f = run[i] > run[i - 1] ? (half - run[i - 1]) / (run[i] - run[i - 1]) : 0;
                            tm = ta + (tb - ta) * (i - 1 + f) / kChords;
                            break;
                        }
                }
                ++pieceCount;
                gp_Pnt m;
                gp_Vec d;
                x.curve->D1(tm, m, d);
                const gp_Vec2d along(d.X(), d.Y());
                if (along.Magnitude() < 1e-15)
                    continue;
                // Looked at from 10 times the model's looseness there in to kSideStep (kSideStep to 10 times
                // it on an exact model), but no farther out than half the piece is long, or twice the
                // looseness if that is more: the piece is cut where anything meets it, so the nearest corner
                // is at its end, and a step longer than that can reach round the corner into the shadow
                // beyond. Shadow on both sides at any size no finer than the looseness puts it inside
                // (finer, a face reaching a hair past the rim can cover both; a crack between loosely
                // meeting faces can leave one bare); else the first size with shadow on one side only
                // decides. A side point only on some face's boundary can't tell: that size is skipped.
                const gp_Vec2d across = gp_Vec2d(-along.Y(), along.X()).Normalized();
                double reachOver = 0;
                {
                    const gp_Pnt a0 = x.curve->Value(ta), a1 = x.curve->Value(tb);
                    reachOver = std::max(std::min(m.Distance(a0), m.Distance(a1)), 2 * x.loose);
                }
                std::vector<double> steps;                  // largest first
                for (double step = side; step >= kSideStep * 0.999; step /= 2)
                    if (step <= reachOver || step <= kSideStep * 1.001)
                        steps.push_back(step);
                if (side <= kSideStep * 1.001)
                    for (const double more : {3 * kSideStep, 10 * kSideStep})
                        if (more <= reachOver)
                            steps.push_back(more);
                int l = 0, rr = 0;
                bool decided = false, inside = false;
                // A side point in a crack narrower than twice the looseness, measured along the same line
                // (shadow both nearer the piece and farther out, within the looseness), is in the shadow:
                // the model is only that exact there. Both points stay on the same side of the piece.
                const auto sideAt = [&](double step, double sign) {
                    const auto at = [&](double r) {
                        return covered(gp_Pnt(m.X() + across.X() * r * sign, m.Y() + across.Y() * r * sign, 0));
                    };
                    const int c = at(step);
                    const double h = std::min(x.loose, 0.9 * step);
                    if (c != 0 || h < kSideStep)
                        return c;
                    return at(step - h) == 1 && at(step + h) == 1 ? 1 : 0;
                };
                for (const double step : steps) {
                    const int sl = sideAt(step, 1);
                    const int sr = sideAt(step, -1);
                    if (sl < 0 || sr < 0)
                        continue;
                    if (sl == 1 && sr == 1) {
                        if (step >= x.loose) {
                            inside = true;
                            break;
                        }
                        continue;
                    }
                    if (sl != sr && !decided) {
                        decided = true;
                        l = sl;
                        rr = sr;
                    }
                }
                if (!decided || inside)
                    l = rr = 0;
                const bool onLeft = l == 1, onRight = rr == 1;
                if (onLeft == onRight)
                    continue;
                // Travelled with the shadow on its left.
                const bool forward = onLeft;
                Bit bit;
                bit.loose = x.loose;
                bit.curve = x.curve;
                bit.ta = ta;
                bit.tb = tb;
                bit.tm = tm;
                bit.forward = forward;
                const double t0 = forward ? ta : tb, t1 = forward ? tb : ta;
                const gp_Pnt p0 = x.curve->Value(t0), p1 = x.curve->Value(t1);
                bit.from = gp_Pnt2d(p0.X(), p0.Y());
                bit.to = gp_Pnt2d(p1.X(), p1.Y());
                bit.middle = gp_Pnt2d(m.X(), m.Y());
                bit.length = bit.from.Distance(bit.middle) + bit.middle.Distance(bit.to);
                Run &r = bit.run;
                r.from = Point2{p0.X(), p0.Y()};
                r.to = Point2{p1.X(), p1.Y()};
                if (kind.GetType() == GeomAbs_Line) {
                    r.kind = FlatPiece::Line;
                } else if (kind.GetType() == GeomAbs_Circle) {
                    const gp_Circ c = kind.Circle();
                    r.kind = whole ? FlatPiece::Circle : FlatPiece::Arc;
                    r.centre = two(c.Location());
                    r.radius = c.Radius();
                    r.ccw = (c.Axis().Direction().Z() > 0) == forward;
                } else if (kind.GetType() == GeomAbs_Ellipse && whole) {
                    r.kind = FlatPiece::Ellipse;
                    r.ellipse = kind.Ellipse();
                } else {
                    r.kind = FlatPiece::Spline;
                    r.curve = x.curve;
                    r.t0 = t0;
                    r.t1 = t1;
                }
                bits.push_back(std::move(bit));
            }
        }
        // On a loose model one boundary can come as two curves a hair apart (the rims of two faces that
        // meet loosely), and both pass: the side points have to step past the looseness. The outer one
        // is the outline; one lying within that looseness on the shadow side of another, going the same
        // way, with the model between them, is its copy.
        {
            std::vector<Bnd_Box> boxes(bits.size());
            for (std::size_t i = 0; i < bits.size(); ++i) {
                boxes[i].Add(gp_Pnt(bits[i].from.X(), bits[i].from.Y(), 0));
                boxes[i].Add(gp_Pnt(bits[i].to.X(), bits[i].to.Y(), 0));
                boxes[i].Add(gp_Pnt(bits[i].middle.X(), bits[i].middle.Y(), 0));
                BRepBndLib::Add(BRepBuilderAPI_MakeEdge(bits[i].curve, bits[i].ta, bits[i].tb).Edge(), boxes[i], Standard_False);
            }
            std::vector<bool> copy(bits.size(), false);
            std::vector<std::size_t> copyOf(bits.size(), bits.size());
            for (std::size_t i = 0; i < bits.size(); ++i) {
                const gp_Pnt m(bits[i].middle.X(), bits[i].middle.Y(), 0);
                for (std::size_t j = 0; j < bits.size() && !copy[i]; ++j) {
                    if (j == i || copy[j])
                        continue;
                    const double reach = std::max(10 * std::max(bits[i].loose, bits[j].loose), kSideStep);
                    Bnd_Box b = boxes[j];
                    b.Enlarge(reach);
                    if (b.IsOut(m))
                        continue;
                    GeomAPI_ProjectPointOnCurve on(m, bits[j].curve, bits[j].ta, bits[j].tb);
                    if (on.NbPoints() == 0 || on.LowerDistance() > reach)
                        continue;
                    {
                        // Alongside j itself, not past its end (a line's foot can come back from beyond it),
                        // and so are both of i's ends: i lies along j's length, whole. (Then dropping copies
                        // never loses ground, whatever the order: a copy of a copy is still within j.)
                        const double lo = std::min(bits[j].ta, bits[j].tb), hi = std::max(bits[j].ta, bits[j].tb);
                        const double u = on.LowerDistanceParameter();
                        if (u <= lo || u >= hi)
                            continue;
                        const auto along = [&](const gp_Pnt2d &q) {
                            const gp_Pnt p(q.X(), q.Y(), 0);
                            if (p.Distance(gp_Pnt(bits[j].from.X(), bits[j].from.Y(), 0)) <= reach ||
                                p.Distance(gp_Pnt(bits[j].to.X(), bits[j].to.Y(), 0)) <= reach)
                                return true;
                            GeomAPI_ProjectPointOnCurve foot(p, bits[j].curve, lo, hi);
                            return foot.NbPoints() > 0 && foot.LowerDistance() <= reach && foot.LowerDistanceParameter() > lo &&
                                   foot.LowerDistanceParameter() < hi;
                        };
                        if (!along(bits[i].from) || !along(bits[i].to))
                            continue;
                    }
                    gp_Pnt foot;
                    gp_Vec d;
                    bits[j].curve->D1(on.LowerDistanceParameter(), foot, d);
                    if (!bits[j].forward)
                        d.Reverse();
                    const double side = d.X() * (m.Y() - foot.Y()) - d.Y() * (m.X() - foot.X());
                    // On j's left, going the same way, and the model there between them: inside the shadow
                    // j already bounds. (Going the other way, the two bound a strip from either side;
                    // nothing between them is a real gap, however narrow: both bound it.)
                    gp_Pnt at;
                    gp_Vec mine;
                    bits[i].curve->D1(bits[i].tm, at, mine);
                    if (!bits[i].forward)
                        mine.Reverse();
                    // A copy runs alongside: the two within 30 degrees of each other (a piece meeting j at a
                    // corner, square to it, is never its copy however short and close it is).
                    const bool alongside = mine.Magnitude() > 0 && d.Magnitude() > 0 && mine.Dot(d) > 0.866 * mine.Magnitude() * d.Magnitude();
                    // (Closer than the looseness, a bare hair between them is no gap: the model is only that
                    // exact there, as with a crack the side points step over.)
                    // Within the looseness of both together (each is within its own of where the model really is;
                    // or on top of each other) the two are one boundary,
                    // whichever side of j the hair puts i: i, lying along j's length, is its copy.
                    const bool same = on.LowerDistance() <= std::max(bits[i].loose + bits[j].loose, kSame);
                    const bool between = same || covered(gp_Pnt((m.X() + foot.X()) / 2, (m.Y() + foot.Y()) / 2, 0)) != 0;
                    if ((side > 0 || same) && alongside && between) {
                        copy[i] = true;
                        copyOf[i] = j;
                    }
                }
            }
            // The piece kept stands for its copies, so it is as loose as they are: they agree with it only
            // that closely, and what meets a copy's ends must still meet it.
            for (std::size_t i = 0; i < bits.size(); ++i) {
                std::size_t r = i;
                for (std::size_t hops = 0; copy[r] && copyOf[r] < bits.size() && hops < bits.size(); ++hops)
                    r = copyOf[r];
                if (r != i && !copy[r])
                    bits[r].loose = std::max(bits[r].loose, bits[i].loose);
            }
            std::vector<Bit> outer;
            for (std::size_t i = 0; i < bits.size(); ++i)
                if (!copy[i])
                    outer.push_back(std::move(bits[i]));
            bits = std::move(outer);
        }
        // The same piece twice (two edges lying on each other): once.
        {
            std::vector<Bit> once;
            for (Bit &b : bits) {
                const double tol = std::max(b.loose, Precision::Confusion());
                // The one kept stands for both, so it is as loose as the looser: they agree only that
                // closely, and what meets the other's ends must still meet it.
                const auto twin = std::find_if(once.begin(), once.end(), [&](const Bit &o) {
                    const double t = std::max(tol, o.loose);
                    return o.from.Distance(b.from) <= t && o.to.Distance(b.to) <= t && o.middle.Distance(b.middle) <= 2 * t;
                });
                if (twin == once.end())
                    once.push_back(std::move(b));
                else
                    twin->loose = std::max(twin->loose, b.loose);
            }
            bits = std::move(once);
        }
        if (getenv("OMAMAKE_PROJECT_TIMES"))
            fprintf(stderr, "project: %zu flat edges, %ld crossings, %d pieces, %zu outline\n", flats.size(), crossings, pieceCount, bits.size());
        stage("pieces on the outline");

        // Nothing covered on one side only: a face seen edge on. Its edges, laid flat, each on its own.
        if (bits.empty()) {
            for (const Flat &x : flats) {
                std::vector<FlatPiece> one;
                if (!finish({runOf(TopoDS::Edge(x.edge.Oriented(TopAbs_FORWARD)))}, one, why))
                    return false;
                if (!one.empty())
                    loops.push_back(one);
            }
            if (loops.empty()) {
                why = "that throws no outline on this sketch";
                return false;
            }
            return true;
        }

        // 5. The pieces chained end to start into closed outlines, the shadow on their left; where a
        //    loose model's curves stop short of each other, the nearest start within its looseness.
        std::vector<bool> used(bits.size(), false), taken(bits.size(), false);   // taken: by an earlier chain
        std::vector<bool> inLoop(bits.size(), false);
        std::vector<std::pair<std::vector<std::size_t>, gp_Pnt2d>> leftover;     // chains that didn't close, and where
        std::vector<std::vector<FlatPiece>> outside, holes;
        for (std::size_t s = 0; s < bits.size(); ++s) {
            if (used[s])
                continue;
            std::vector<Run> runs;
            double area = 0;
            std::size_t at = s;
            bool closed = false;
            for (;;) {
                used[at] = true;
                const Bit &b = bits[at];
                runs.push_back(b.run);
                area += sweep(b.run);     // outside or hole, by the way it goes round
                // Where it goes on: a start exactly at its end (pieces that meet at a crossing share the
                // point) before anything merely within the model's looseness.
                const auto nearest = [&](double within, bool loose) {
                    std::size_t next = bits.size();
                    double best = std::numeric_limits<double>::max();
                    for (std::size_t k = 0; k < bits.size(); ++k) {
                        if (used[k])
                            continue;
                        const double d = bits[k].from.Distance(b.to);
                        if (d < best && d <= (loose ? std::max(within, 2 * bits[k].loose) : within)) {
                            best = d;
                            next = k;
                        }
                    }
                    return next;
                };
                if (b.run.kind == FlatPiece::Circle || b.run.kind == FlatPiece::Ellipse) {
                    closed = true;
                    break;
                }
                const double home = b.to.Distance(bits[s].from);
                if (runs.size() > 1 && home <= kExactJoin) {
                    closed = true;
                    break;
                }
                std::size_t next = nearest(kExactJoin, false);
                if (next == bits.size()) {
                    const double reach = std::max(2 * b.loose, kExactJoin);
                    if (runs.size() > 1 && home <= std::max(reach, 2 * bits[s].loose)) {
                        closed = true;
                        break;
                    }
                    next = nearest(reach, true);
                }
                if (next == bits.size())
                    break;
                at = next;
            }
            if (!closed) {
                // Left over: judged once every outline is found (below).
                std::vector<std::size_t> mine;
                for (std::size_t k = 0; k < bits.size(); ++k)
                    if (used[k] && !taken[k]) {
                        mine.push_back(k);
                        taken[k] = true;
                    }
                leftover.push_back({std::move(mine), bits[at].to});
                continue;
            }
            for (std::size_t k = 0; k < bits.size(); ++k)
                if (used[k] && !taken[k])
                    taken[k] = inLoop[k] = true;
            std::vector<FlatPiece> pieces;
            if (!finish(runs, pieces, why))
                return false;
            if (!pieces.empty())
                (area >= 0 ? outside : holes).push_back(std::move(pieces));
        }
        // What didn't close. On a loose model the shadow's edge is only known to within its looseness:
        // a stub where faces overlap a hair past the outline, or a copy the copy check couldn't see (past
        // the end of the curve it copies). It is no shape if all of it lies within the looseness of an
        // outline found; anything farther is a real gap, and fails.
        for (const auto &[chain, where] : leftover) {
            for (const std::size_t k : chain)
                for (const gp_Pnt2d &q : {bits[k].from, bits[k].middle, bits[k].to}) {
                    bool near = false;
                    for (std::size_t j = 0; j < bits.size() && !near; ++j) {
                        if (!inLoop[j])
                            continue;
                        const double reach = std::max(10 * std::max(bits[j].loose, bits[k].loose), kSideStep);
                        if (q.Distance(bits[j].from) <= reach || q.Distance(bits[j].to) <= reach) {
                            near = true;
                            break;
                        }
                        GeomAPI_ProjectPointOnCurve on(gp_Pnt(q.X(), q.Y(), 0), bits[j].curve, bits[j].ta, bits[j].tb);
                        near = on.NbPoints() > 0 && on.LowerDistance() <= reach;
                    }
                    if (!near) {
                        char text[160];
                        snprintf(text, sizeof text, "the outline doesn't close near (%.4f, %.4f)", where.X(), where.Y());
                        why = text;
                        return false;
                    }
                }
        }
        loops = std::move(outside);
        loops.insert(loops.end(), holes.begin(), holes.end());
        return !loops.empty() || (why = "that throws no outline on this sketch", false);
    } catch (const Standard_Failure &failure) {
        why = std::string("the outline couldn't be worked out (") + failure.GetMessageString() + ")";
        return false;
    }
}

} // namespace projectexact
