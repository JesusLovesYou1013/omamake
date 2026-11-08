#include "engine/heavy/meshsolid.h"
#include "engine/heavy/refine.h"

#include <BRepBndLib.hxx>
#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepBuilderAPI_MakeVertex.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <BRepLib.hxx>
#include <BRep_Builder.hxx>
#include <BRep_Tool.hxx>
#include <Bnd_Box.hxx>
#include <ElCLib.hxx>
#include <GC_MakeCircle.hxx>
#include <GeomAPI_PointsToBSpline.hxx>
#include <GeomAPI_PointsToBSplineSurface.hxx>
#include <GeomAPI_ProjectPointOnSurf.hxx>
#include <Geom_BSplineCurve.hxx>
#include <Geom_BSplineSurface.hxx>
#include <Geom_Circle.hxx>
#include <Geom_Line.hxx>
#include <Geom_ConicalSurface.hxx>
#include <Geom_CylindricalSurface.hxx>
#include <Geom_Plane.hxx>
#include <Geom_SphericalSurface.hxx>
#include <Geom_SurfaceOfRevolution.hxx>
#include <Poly_Triangulation.hxx>
#include <RWStl.hxx>
#include <ShapeFix_Edge.hxx>
#include <ShapeFix_Shell.hxx>
#include <ShapeFix_Wire.hxx>
#include <ShapeUpgrade_UnifySameDomain.hxx>
#include <Standard_Failure.hxx>
#include <TColStd_Array1OfInteger.hxx>
#include <TColStd_Array1OfReal.hxx>
#include <TColgp_Array1OfPnt.hxx>
#include <TColgp_Array2OfPnt.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopTools_IndexedDataMapOfShapeListOfShape.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Shell.hxx>
#include <TopoDS_Solid.hxx>
#include <TopoDS_Wire.hxx>
#include <gp_Ax3.hxx>
#include <gp_Circ.hxx>
#include <Approx_ParametrizationType.hxx>
#include <Geom2dAPI_PointsToBSpline.hxx>
#include <Geom2d_BSplineCurve.hxx>
#include <TColStd_Array1OfInteger.hxx>
#include <TColStd_Array1OfReal.hxx>
#include <TColgp_Array1OfPnt2d.hxx>
#include <math_Gauss.hxx>
#include <math_Jacobi.hxx>
#include <math_Matrix.hxx>
#include <math_Vector.hxx>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>
#include <map>
#include <numeric>
#include <set>
#include <sstream>
#include <unordered_map>

namespace meshsolid {
namespace {

using V = gp_XYZ;

using Key = std::pair<int, int>;
Key sideKey(int a, int b) { return std::minmax(a, b); }

V pointOf(const Mesh &m, int i) {
    const auto &p = m.points[std::size_t(i)];
    return V(p[0], p[1], p[2]);
}

// Join points closer than cell; drop triangles that collapse.
Mesh weld(const Mesh &in, double cell) {
    Mesh out;
    std::map<std::array<std::int64_t, 3>, int> seen;
    std::vector<int> to(in.points.size());
    for (std::size_t i = 0; i < in.points.size(); ++i) {
        const auto &p = in.points[i];
        const std::array<std::int64_t, 3> key{std::llround(p[0] / cell), std::llround(p[1] / cell), std::llround(p[2] / cell)};
        const auto [it, added] = seen.emplace(key, int(out.points.size()));
        if (added)
            out.points.push_back(p);
        to[i] = it->second;
    }
    for (const auto &t : in.triangles) {
        const std::array<int, 3> n{to[std::size_t(t[0])], to[std::size_t(t[1])], to[std::size_t(t[2])]};
        if (n[0] != n[1] && n[1] != n[2] && n[0] != n[2])
            out.triangles.push_back(n);
    }
    return out;
}

double reach(const Mesh &m) {
    V lo(INFINITY, INFINITY, INFINITY), hi(-INFINITY, -INFINITY, -INFINITY);
    for (const auto &p : m.points)
        for (int c = 1; c <= 3; ++c) {
            lo.SetCoord(c, std::min(lo.Coord(c), p[std::size_t(c - 1)]));
            hi.SetCoord(c, std::max(hi.Coord(c), p[std::size_t(c - 1)]));
        }
    return m.points.empty() ? 0 : (hi - lo).Modulus();
}

// Exporters do write triangles facing the wrong way: turn each to agree with its neighbours,
// then the whole mesh outward if its volume came out negative.
void orient(Mesh &m) {
    std::map<Key, std::vector<int>> sides;
    for (std::size_t t = 0; t < m.triangles.size(); ++t)
        for (int k = 0; k < 3; ++k)
            sides[sideKey(m.triangles[t][std::size_t(k)], m.triangles[t][std::size_t((k + 1) % 3)])].push_back(int(t));
    auto runs = [](const std::array<int, 3> &t, int a, int b) {
        return (t[0] == a && t[1] == b) || (t[1] == a && t[2] == b) || (t[2] == a && t[0] == b);
    };
    std::vector<char> seen(m.triangles.size(), 0);
    std::vector<int> stack;
    for (std::size_t first = 0; first < m.triangles.size(); ++first) {
        if (seen[first])
            continue;
        seen[first] = 1;
        stack = {int(first)};
        std::vector<int> part;
        while (!stack.empty()) {
            const int t = stack.back();
            stack.pop_back();
            part.push_back(t);
            for (int k = 0; k < 3; ++k) {
                const int a = m.triangles[std::size_t(t)][std::size_t(k)], b = m.triangles[std::size_t(t)][std::size_t((k + 1) % 3)];
                const auto &on = sides[sideKey(a, b)];
                if (on.size() != 2)
                    continue;
                const int other = on[0] == t ? on[1] : on[0];
                if (seen[std::size_t(other)])
                    continue;
                seen[std::size_t(other)] = 1;
                if (runs(m.triangles[std::size_t(other)], a, b))
                    std::swap(m.triangles[std::size_t(other)][1], m.triangles[std::size_t(other)][2]);
                stack.push_back(other);
            }
        }
        double volume = 0;
        for (const int t : part) {
            const auto &tri = m.triangles[std::size_t(t)];
            volume += pointOf(m, tri[0]).Dot(pointOf(m, tri[1]).Crossed(pointOf(m, tri[2])));
        }
        if (volume < 0)
            for (const int t : part)
                std::swap(m.triangles[std::size_t(t)][1], m.triangles[std::size_t(t)][2]);
    }
}

// What's known about the triangles: normals, areas, and who shares which side.
struct Facets {
    const Mesh &mesh;
    std::vector<V> normal;
    std::vector<double> area;
    std::vector<std::array<int, 3>> across;    // the triangle over each side, -1 for none or several

    explicit Facets(const Mesh &m) : mesh(m) {
        const std::size_t n = m.triangles.size();
        normal.resize(n);
        area.resize(n);
        across.assign(n, {-1, -1, -1});
        std::map<std::pair<int, int>, std::vector<std::pair<int, int>>> sides;
        for (std::size_t t = 0; t < n; ++t) {
            const auto &tri = m.triangles[t];
            const V a = pointOf(m, tri[0]), b = pointOf(m, tri[1]), c = pointOf(m, tri[2]);
            V cross = (b - a).Crossed(c - a);
            area[t] = cross.Modulus() / 2;
            if (area[t] > 0)
                cross /= cross.Modulus();
            normal[t] = cross;
            for (int k = 0; k < 3; ++k)
                sides[std::minmax(tri[std::size_t(k)], tri[std::size_t((k + 1) % 3)])].push_back({int(t), k});
        }
        for (const auto &[edge, on] : sides)
            if (on.size() == 2) {
                across[std::size_t(on[0].first)][std::size_t(on[0].second)] = on[1].first;
                across[std::size_t(on[1].first)][std::size_t(on[1].second)] = on[0].first;
            }
    }
};

// A surface a group of triangles is turned into.
struct Surface {
    enum Kind { Flat, Cylinder, Cone, Sphere, Screw, Torus } kind = Flat;
    V origin, axis;         // Flat: a point and the normal; round: a point on the axis (sphere: centre) and the axis
    double radius = 0;      // at origin
    double slope = 0;       // Cone: radius gained per mm along axis
    double tube = 0;        // Torus: the fillet's own radius; radius is its centre line's
    double facing = 0;      // Torus: which way round the tube the fillet faces, 0 straight out from the axis
    bool inward = false;    // the solid is on the surface's outer side: a hole
    V seam;                 // round: where its wrap-around line goes, if set
    // Screw: one face of a thread's outline swept up a helix. The outline is (w, rho) points across
    // the thread, w always rising; pitch is mm per turn, negative for left-handed; low..high the height.
    double pitch = 0, low = 0, high = 0;
    std::vector<std::array<double, 2>> outline;
};

// The turns a screw's spline covers, counted from the origin.
void screwTurns(const Surface &s, double &first, double &last) {
    const double turn = std::abs(s.pitch);
    first = (s.low - s.outline.back()[0] - turn) / turn;
    last = (s.high - s.outline.front()[0] + turn) / turn;
}

// Where p sits on a screw: the outline's coordinates, wrapped to the turn nearest that piece.
void onScrew(const Surface &s, const V &p, double &w, double &rho) {
    const gp_Ax3 frame(gp_Pnt(s.origin), gp_Dir(s.axis));
    const V d = p - s.origin;
    const double h = d.Dot(s.axis), x = d.Dot(frame.XDirection().XYZ()), y = d.Dot(frame.YDirection().XYZ());
    rho = std::hypot(x, y);
    w = h - s.pitch * std::atan2(y, x) / (2 * M_PI);
    const double turn = std::abs(s.pitch);
    w -= std::round((w - (s.outline.front()[0] + s.outline.back()[0]) / 2) / turn) * turn;
}

// The nearest place on a screw's outline to (w, rho): as a position counted in outline pieces,
// and how far off it is.
double nearOutline(const Surface &s, double w, double rho, double &position, std::size_t &piece) {
    double best = INFINITY;
    for (std::size_t i = 0; i + 1 < s.outline.size(); ++i) {
        const double dw = s.outline[i + 1][0] - s.outline[i][0], dr = s.outline[i + 1][1] - s.outline[i][1];
        const double along = std::clamp(((w - s.outline[i][0]) * dw + (rho - s.outline[i][1]) * dr) / std::max(dw * dw + dr * dr, 1e-12), 0.0, 1.0);
        const double off = std::hypot(w - s.outline[i][0] - dw * along, rho - s.outline[i][1] - dr * along);
        if (off < best) {
            best = off;
            position = double(i) + along;
            piece = i;
        }
    }
    return best;
}

// How far along a screw's outline a position is, 0 to 1 by length: the surface's v.
double outlineShare(const Surface &s, double position) {
    double total = 0, reached = 0;
    for (std::size_t i = 0; i + 1 < s.outline.size(); ++i) {
        const double length = std::hypot(s.outline[i + 1][0] - s.outline[i][0], s.outline[i + 1][1] - s.outline[i][1]);
        reached += length * std::clamp(position - double(i), 0.0, 1.0);
        total += length;
    }
    return reached / std::max(total, 1e-12);
}

// The outline's point that share of its length along.
std::array<double, 2> outlineAt(const Surface &s, double share) {
    double total = 0;
    for (std::size_t i = 0; i + 1 < s.outline.size(); ++i)
        total += std::hypot(s.outline[i + 1][0] - s.outline[i][0], s.outline[i + 1][1] - s.outline[i][1]);
    double left = share * total;
    for (std::size_t i = 0; i + 1 < s.outline.size(); ++i) {
        const auto &a = s.outline[i], &b = s.outline[i + 1];
        const double length = std::hypot(b[0] - a[0], b[1] - a[1]);
        if (left <= length || i + 2 == s.outline.size()) {
            const double along = std::clamp(left / std::max(length, 1e-12), 0.0, 1.0);
            return {a[0] + (b[0] - a[0]) * along, a[1] + (b[1] - a[1]) * along};
        }
        left -= length;
    }
    return s.outline.back();
}

double distanceTo(const Surface &s, const V &p) {
    const V d = p - s.origin;
    switch (s.kind) {
    case Surface::Flat: return std::abs(d.Dot(s.axis));
    case Surface::Sphere: return std::abs(d.Modulus() - s.radius);
    case Surface::Torus: {
        const double h = d.Dot(s.axis);
        return std::abs(std::hypot((d - s.axis * h).Modulus() - s.radius, h) - s.tube);
    }
    case Surface::Cylinder: return std::abs((d - s.axis * d.Dot(s.axis)).Modulus() - s.radius);
    case Surface::Cone: {
        const double h = d.Dot(s.axis), rho = (d - s.axis * h).Modulus();
        return std::abs(rho - (s.radius + s.slope * h)) / std::sqrt(1 + s.slope * s.slope);
    }
    case Surface::Screw: {
        double w, rho;
        onScrew(s, p, w, rho);
        double position;
        std::size_t piece;
        return nearOutline(s, w, rho, position, piece);
    }
    }
    return 0;
}

// How far p is off the surface, signed, and which way that grows; false for a screw (no formula).
bool offBy(const Surface &s, const V &p, double &off, V &way) {
    const V d = p - s.origin;
    const double h = d.Dot(s.axis);
    V radial = d - s.axis * h;
    const double rho = radial.Modulus();
    radial = rho > 1e-12 ? radial / rho : V(0, 0, 0);
    switch (s.kind) {
    case Surface::Flat: off = h; way = s.axis; return true;
    case Surface::Cylinder: off = rho - s.radius; way = radial; return rho > 1e-12;
    case Surface::Cone: {
        const double scale = std::sqrt(1 + s.slope * s.slope);
        off = (rho - s.radius - s.slope * h) / scale;
        way = (radial - s.axis * s.slope) / scale;
        return rho > 1e-12;
    }
    case Surface::Sphere: off = d.Modulus() - s.radius; way = d / std::max(d.Modulus(), 1e-12); return d.Modulus() > 1e-12;
    case Surface::Torus: {
        const V tube = radial * (rho - s.radius) + s.axis * h;
        off = tube.Modulus() - s.tube;
        way = tube / std::max(tube.Modulus(), 1e-12);
        return tube.Modulus() > 1e-12 && rho > 1e-12;
    }
    case Surface::Screw: return false;
    }
    return false;
}

// The point nearest p that lies on every one of the surfaces: where they truly meet, as a CAD
// program's corner is (FreeCAD and every B-rep modeller keep corners on all their faces; ours sat on
// mesh points up to 0.003 mm off, so anything cut through them came out tilted). Gauss-Newton on the
// signed distances, the step held to the smallest that does it. False if they don't meet near p.
bool meetingPoint(const std::vector<const Surface *> &given, const V &p, double reach, V &at) {
    // A round wall touching a flat meets it along a line, where distances only grow as the square
    // of the miss and the solve would stop short: there it stands for the plane through its axis
    // square to the flat, which holds that line exactly.
    std::vector<Surface> stand;
    stand.reserve(given.size());
    std::vector<const Surface *> on;
    for (const Surface *s : given) {
        const Surface *use = s;
        if (s->kind == Surface::Cylinder)
            for (const Surface *f : given)
                if (f->kind == Surface::Flat && std::abs(f->axis.Dot(s->axis)) < 1e-12
                    && std::abs(std::abs((s->origin - f->origin).Dot(f->axis)) - s->radius) < 1e-9) {
                    Surface across;
                    across.kind = Surface::Flat;
                    across.origin = s->origin;
                    across.axis = f->axis.Crossed(s->axis);
                    across.axis /= across.axis.Modulus();
                    stand.push_back(across);
                    use = &stand.back();
                    break;
                }
        on.push_back(use);
    }
    at = p;
    for (int pass = 0; pass < 100; ++pass) {
        math_Matrix a(1, 3, 1, 3, 0.0);
        math_Vector b(1, 3, 0.0);
        double worst = 0;
        for (const Surface *s : on) {
            double off;
            V way;
            if (!offBy(*s, at, off, way))
                return false;
            worst = std::max(worst, std::abs(off));
            const double g[3] = {way.X(), way.Y(), way.Z()};
            for (int i = 0; i < 3; ++i) {
                b(i + 1) -= g[i] * off;
                for (int k = 0; k < 3; ++k)
                    a(i + 1, k + 1) += g[i] * g[k];
            }
        }
        if (worst < 1e-12)
            return (at - p).Modulus() <= reach;
        // Fewer than three ways to go (two planes meet in a line): the shortest step along it.
        for (int i = 1; i <= 3; ++i)
            a(i, i) += 1e-12;
        math_Gauss solve(a);
        if (!solve.IsDone())
            return false;
        math_Vector step(1, 3);
        solve.Solve(b, step);
        at += V(step(1), step(2), step(3));
        if ((at - p).Modulus() > reach)
            return false;
    }
    for (const Surface *s : given)
        if (distanceTo(*s, at) > 1e-9)
            return false;
    return (at - p).Modulus() <= reach;
}

// The surface's outward normal at p.
V normalAt(const Surface &s, const V &p) {
    const V d = p - s.origin;
    if (s.kind == Surface::Flat)
        return s.axis;
    if (s.kind == Surface::Sphere)
        return d / std::max(d.Modulus(), 1e-12);
    V out = d - s.axis * d.Dot(s.axis);
    out /= std::max(out.Modulus(), 1e-12);
    if (s.kind == Surface::Torus) {
        const V n = d - out * s.radius;
        return n / std::max(n.Modulus(), 1e-12);
    }
    if (s.kind == Surface::Screw) {
        double w, rho, position;
        std::size_t piece = 0;
        onScrew(s, p, w, rho);
        nearOutline(s, w, rho, position, piece);
        const double dw = s.outline[piece + 1][0] - s.outline[piece][0], dr = s.outline[piece + 1][1] - s.outline[piece][1], len = std::max(std::hypot(dw, dr), 1e-12);
        return out * (dw / len) - s.axis * (dr / len);
    }
    if (s.kind == Surface::Cone) {
        V n = out - s.axis * s.slope;
        return n / n.Modulus();
    }
    return out;
}

// Flat areas: joined triangles lying in one plane. labels[t] = which; surfaces[label] = its plane.
void flatAreas(const Facets &f, double reachMm, std::vector<int> &labels, std::vector<Surface> &surfaces) {
    // Owner, 2026-10-30: under a degree apart is meant to be flat. Measured against the area's
    // first triangle, never just the neighbour, or a fine cylinder would creep all the way round;
    // and no point may sit further than this from it, so a big gentle curve stays a curve.
    const double offPlane = std::max(0.02, reachMm * 1e-5);
    const double cosLimit = std::cos(1.0 * M_PI / 180);
    const std::size_t n = f.area.size();
    labels.assign(n, -1);
    std::vector<int> order(n);
    std::iota(order.begin(), order.end(), 0);
    std::sort(order.begin(), order.end(), [&](int a, int b) { return f.area[std::size_t(a)] > f.area[std::size_t(b)]; });
    std::vector<int> stack;
    for (const int seed : order) {
        if (labels[std::size_t(seed)] >= 0)
            continue;
        const int label = int(surfaces.size());
        Surface s;
        s.axis = f.normal[std::size_t(seed)];
        s.origin = pointOf(f.mesh, f.mesh.triangles[std::size_t(seed)][0]);
        surfaces.push_back(s);
        labels[std::size_t(seed)] = label;
        stack = {seed};
        while (!stack.empty()) {
            const int t = stack.back();
            stack.pop_back();
            for (const int other : f.across[std::size_t(t)]) {
                if (other < 0 || labels[std::size_t(other)] >= 0 || f.normal[std::size_t(other)].Dot(s.axis) < cosLimit)
                    continue;
                bool on = true;
                for (const int p : f.mesh.triangles[std::size_t(other)])
                    on = on && distanceTo(s, pointOf(f.mesh, p)) < offPlane;
                if (on) {
                    labels[std::size_t(other)] = label;
                    stack.push_back(other);
                }
            }
        }
    }

    // Each area's true plane: the average of its triangles, through their middle.
    std::vector<V> facing(surfaces.size(), V(0, 0, 0)), middle(surfaces.size(), V(0, 0, 0));
    std::vector<double> total(surfaces.size(), 0);
    for (std::size_t t = 0; t < n; ++t) {
        const std::size_t label = std::size_t(labels[t]);
        const auto &tri = f.mesh.triangles[t];
        facing[label] += f.normal[t] * f.area[t];
        middle[label] += (pointOf(f.mesh, tri[0]) + pointOf(f.mesh, tri[1]) + pointOf(f.mesh, tri[2])) / 3 * f.area[t];
        total[label] += f.area[t];
    }
    for (std::size_t i = 0; i < surfaces.size(); ++i)
        if (total[i] > 0 && facing[i].Modulus() > 0) {
            surfaces[i].axis = facing[i] / facing[i].Modulus();
            surfaces[i].origin = middle[i] / total[i];
        }
}

// One piece of edge between two labels' areas (or the same one, on an open mesh's rim).
struct Chain {
    std::vector<int> points;
    int a = -1, b = -1;
};

TopoDS_Edge splineThrough(const Mesh &m, const std::vector<int> &pts, const TopoDS_Vertex &vs, const TopoDS_Vertex &ve, double tol) {
    // Point to point: always true to them, if not smooth.
    const auto direct = [&]() {
        TColgp_Array1OfPnt poles(1, int(pts.size()));
        TColStd_Array1OfReal knots(1, int(pts.size()));
        TColStd_Array1OfInteger times(1, int(pts.size()));
        double along = 0;
        for (std::size_t i = 0; i < pts.size(); ++i) {
            if (i)
                along += (pointOf(m, pts[i]) - pointOf(m, pts[i - 1])).Modulus();
            poles.SetValue(int(i) + 1, gp_Pnt(pointOf(m, pts[i])));
            knots.SetValue(int(i) + 1, along);
            times.SetValue(int(i) + 1, i == 0 || i + 1 == pts.size() ? 2 : 1);
        }
        BRepBuilderAPI_MakeEdge make(new Geom_BSplineCurve(poles, knots, times, 1), vs, ve);
        return make.IsDone() ? make.Edge() : BRepBuilderAPI_MakeEdge(vs, ve).Edge();
    };
    // A smooth curve can't turn a corner.
    for (std::size_t i = 1; i + 1 < pts.size(); ++i) {
        const V in = pointOf(m, pts[i]) - pointOf(m, pts[i - 1]), out = pointOf(m, pts[i + 1]) - pointOf(m, pts[i]);
        if (in.Dot(out) < std::cos(45 * M_PI / 180) * in.Modulus() * out.Modulus())
            return direct();
    }
    // The fit's time grows far faster than its points, and a long helix doesn't need them all.
    constexpr std::size_t kMost = 320;
    const std::size_t count = std::min(pts.size(), kMost);
    TColgp_Array1OfPnt through(1, int(count));
    for (std::size_t i = 0; i < count; ++i)
        through.SetValue(int(i) + 1, gp_Pnt(pointOf(m, pts[i * (pts.size() - 1) / (count - 1)])));
    GeomAPI_PointsToBSpline spline(through, 1, 3, GeomAbs_C1, tol * 0.5);
    if (!spline.IsDone())
        return direct();
    // A smooth curve can swing far off between its points, past the surfaces it's meant to join.
    const Handle(Geom_BSplineCurve) &curve = spline.Curve();
    constexpr int kChecks = 64;
    for (int i = 0; pts.size() < kMost && i <= kChecks; ++i) {
        const V at = curve->Value(curve->FirstParameter() + (curve->LastParameter() - curve->FirstParameter()) * i / kChecks).XYZ();
        double nearest = INFINITY;
        for (std::size_t k = 1; k < pts.size(); ++k) {
            const V a = pointOf(m, pts[k - 1]), d = pointOf(m, pts[k]) - a;
            const double along = std::clamp((at - a).Dot(d) / std::max(d.SquareModulus(), 1e-18), 0.0, 1.0);
            nearest = std::min(nearest, (a + d * along - at).Modulus());
        }
        if (nearest > 0.02)
            return direct();
    }
    BRepBuilderAPI_MakeEdge make(curve, vs, ve);
    return make.IsDone() ? make.Edge() : direct();
}

// Where two surfaces turned about one axis line meet (a plane square to it counts): the very circle
// they share, worked out from the surfaces alone, as a CAD program's edge is the meeting of its two
// faces. False if they aren't on one axis, don't meet in a circle, or the chain's points (near is how
// far they may lie) aren't on it. Traced through mesh points instead, the two halves of one rim came
// out with centres 0.0000002 mm apart and could never merge again (the speaker mount's 9 mm hole).
bool sharedCircle(const Surface &a, const Surface &b, const Mesh &m, const std::vector<int> &pts, double near, gp_Circ &circle) {
    const auto turned = [](const Surface &s) { return s.kind == Surface::Cylinder || s.kind == Surface::Cone || s.kind == Surface::Torus; };
    if (!turned(a) && !turned(b))
        return false;
    const Surface &r = turned(a) ? a : b, &o = turned(a) ? b : a;
    if (o.kind != Surface::Flat && !turned(o))
        return false;
    if (std::abs(std::abs(o.axis.Dot(r.axis)) - 1) > 1e-12)
        return false;
    // The chain's middle, across and along the axis.
    double hm = 0, rm = 0;
    for (const int p : pts) {
        const V d = pointOf(m, p) - r.origin;
        const double h = d.Dot(r.axis);
        hm += h;
        rm += (d - r.axis * h).Modulus();
    }
    hm /= double(pts.size());
    rm /= double(pts.size());
    // Each surface as the rings it holds: radius at a height, or height at a radius.
    const auto radiusAt = [&](const Surface &s, double h, double &rho) {
        if (s.kind == Surface::Cylinder)
            rho = s.radius;
        else if (s.kind == Surface::Cone)
            rho = s.radius + s.slope * h;
        else {
            const double across = s.tube * s.tube - h * h;
            if (across < 0)
                return false;
            const double one = s.radius + std::sqrt(across), two = s.radius - std::sqrt(across);
            rho = std::abs(one - rm) < std::abs(two - rm) ? one : two;
        }
        return rho > 0;
    };
    double h = 0, rho = 0;
    if (o.kind == Surface::Flat) {
        h = (o.origin - r.origin).Dot(r.axis);
        if (!radiusAt(r, h, rho))
            return false;
    } else {
        const V d = o.origin - r.origin;
        if ((d - r.axis * d.Dot(r.axis)).Modulus() > 1e-9)
            return false;
        // Heights measured from r's origin along r's axis; o's own run from its origin, its way.
        const double shift = d.Dot(r.axis), sense = o.axis.Dot(r.axis) > 0 ? 1 : -1;
        const Surface *fixed = r.kind == Surface::Cylinder ? &r : o.kind == Surface::Cylinder ? &o : nullptr;
        const Surface *other = fixed == &r ? &o : &r;
        if (fixed) {
            if (other->kind == Surface::Cylinder)
                return false;
            rho = fixed->radius;
            const double off = other == &o ? shift : 0, way = other == &o ? sense : 1;
            double local;
            if (other->kind == Surface::Cone) {
                if (std::abs(other->slope) < 1e-12)
                    return false;
                local = (rho - other->radius) / other->slope;
            } else {
                const double along = other->tube * other->tube - (rho - other->radius) * (rho - other->radius);
                if (along < -1e-12)
                    return false;
                const double s = std::sqrt(std::max(along, 0.0)), mine = (hm - off) * way;
                local = std::abs(s - mine) < std::abs(-s - mine) ? s : -s;
            }
            h = off + local * way;
        } else if (r.kind == Surface::Cone && o.kind == Surface::Cone) {
            // r.radius + r.slope h = o.radius + o.slope (h - shift) sense
            const double k = r.slope - o.slope * sense;
            if (std::abs(k) < 1e-12)
                return false;
            h = (o.radius - o.slope * shift * sense - r.radius) / k;
            rho = r.radius + r.slope * h;
        } else {
            return false;
        }
    }
    if (rho <= 0)
        return false;
    circle = gp_Circ(gp_Ax2(gp_Pnt(r.origin + r.axis * h), gp_Dir(r.axis)), rho);
    for (const int p : pts) {
        const V d = pointOf(m, p) - r.origin;
        const double along = d.Dot(r.axis);
        if (std::abs(along - h) > near || std::abs((d - r.axis * along).Modulus() - rho) > near)
            return false;
    }
    return true;
}

// The curve for a chain beside a round surface: the circle its two surfaces share if known, a circle
// where its points make one, else a smooth curve through them.
TopoDS_Edge curved(const Mesh &m, const std::vector<int> &pts, const TopoDS_Vertex &vs, const TopoDS_Vertex &ve, double tol,
                   const gp_Circ *known = nullptr) {
    const V first = pointOf(m, pts.front()), last = pointOf(m, pts.back()), middle = pointOf(m, pts[pts.size() / 2]);
    if (known) {
        // The short way or the long way round: whichever passes the chain's middle.
        const Handle(Geom_Circle) circle = new Geom_Circle(*known);
        double u0 = ElCLib::Parameter(*known, BRep_Tool::Pnt(vs)), u1 = ElCLib::Parameter(*known, BRep_Tool::Pnt(ve));
        // Two points: the chord's middle, on the short arc's side.
        double mid = ElCLib::Parameter(*known, gp_Pnt(pts.size() == 2 ? (first + last) / 2 : middle));
        if (pts.front() == pts.back() || vs.IsSame(ve))
            u1 = u0 + 2 * M_PI;
        while (u1 <= u0 + 1e-12)
            u1 += 2 * M_PI;
        while (mid < u0)
            mid += 2 * M_PI;
        if (mid <= u1) {
            BRepBuilderAPI_MakeEdge make(circle, vs, ve, u0, u1);
            if (make.IsDone())
                return make.Edge();
        } else {
            // the other way round: the circle turned over
            gp_Circ flipped = *known;
            flipped.SetAxis(known->Axis().Reversed());
            const Handle(Geom_Circle) other = new Geom_Circle(flipped);
            double w0 = ElCLib::Parameter(flipped, BRep_Tool::Pnt(vs)), w1 = ElCLib::Parameter(flipped, BRep_Tool::Pnt(ve));
            while (w1 <= w0 + 1e-12)
                w1 += 2 * M_PI;
            BRepBuilderAPI_MakeEdge make(other, vs, ve, w0, w1);
            if (make.IsDone())
                return make.Edge();
        }
    }
    if (pts.size() == 2)
        return BRepBuilderAPI_MakeEdge(vs, ve).Edge();
    double sag = 0;
    const V along = (last - first) / std::max((last - first).Modulus(), 1e-12);
    for (const int i : pts) {
        const V d = pointOf(m, i) - first;
        sag = std::max(sag, (d - along * d.Dot(along)).Modulus());
    }
    if (sag < tol * 0.1)
        return BRepBuilderAPI_MakeEdge(vs, ve).Edge();
    // Three points in a zigzag sit on a circle too.
    bool cornered = false;
    for (std::size_t i = 1; i + 1 < pts.size(); ++i) {
        const V in = pointOf(m, pts[i]) - pointOf(m, pts[i - 1]), out = pointOf(m, pts[i + 1]) - pointOf(m, pts[i]);
        cornered = cornered || in.Dot(out) < std::cos(45 * M_PI / 180) * in.Modulus() * out.Modulus();
    }
    GC_MakeCircle circle((gp_Pnt(first)), gp_Pnt(middle), gp_Pnt(last));
    if (!cornered && circle.IsDone()) {
        const gp_Circ c = circle.Value()->Circ();
        bool on = true;
        for (const int i : pts)
            on = on && std::abs(gp_Pnt(pointOf(m, i)).Distance(c.Location()) - c.Radius()) < tol
                 && std::abs(gp_Vec(c.Location(), gp_Pnt(pointOf(m, i))).Dot(gp_Vec(c.Axis().Direction()))) < tol;
        if (on) {
            BRepBuilderAPI_MakeEdge make(circle.Value(), vs, ve);
            if (make.IsDone()) {
                // the short way or the long way round: it must pass the middle point
                TopoDS_Edge e = make.Edge();
                Standard_Real u0, u1;
                Handle(Geom_Curve) curve = BRep_Tool::Curve(e, u0, u1);
                Standard_Real mid = ElCLib::Parameter(c, gp_Pnt(middle));
                while (mid < u0)
                    mid += 2 * M_PI;
                // Three points in a zigzag sit on a circle too, the long way round: not this edge.
                double run = 0;
                for (std::size_t i = 1; i < pts.size(); ++i)
                    run += (pointOf(m, pts[i]) - pointOf(m, pts[i - 1])).Modulus();
                if (mid <= u1 + 1e-9 && (u1 - u0) * c.Radius() < run * 1.5)
                    return e;
                if (mid <= u1 + 1e-9)
                    return splineThrough(m, pts, vs, ve, tol);
                gp_Circ flipped = c;
                flipped.SetAxis(c.Axis().Reversed());
                BRepBuilderAPI_MakeEdge other(flipped, vs, ve);
                if (other.IsDone()) {
                    Standard_Real o0, o1;
                    BRep_Tool::Curve(other.Edge(), o0, o1);
                    if ((o1 - o0) * c.Radius() < run * 1.5)
                        return other.Edge();
                }
            }
        }
    }
    return splineThrough(m, pts, vs, ve, tol);
}

gp_Ax3 frameOf(const Surface &s) {
    const V along = s.seam - s.axis * s.seam.Dot(s.axis);
    if (along.Modulus() < 1e-9)
        return gp_Ax3(gp_Pnt(s.origin), gp_Dir(s.axis));
    return gp_Ax3(gp_Pnt(s.origin), gp_Dir(s.axis), gp_Dir(along));
}

Handle(Geom_Surface) surfaceOf(const Surface &s) {
    switch (s.kind) {
    case Surface::Flat: return new Geom_Plane(gp_Pnt(s.origin), gp_Dir(s.axis));
    case Surface::Cylinder: return new Geom_CylindricalSurface(frameOf(s), s.radius);
    case Surface::Sphere: return new Geom_SphericalSurface(frameOf(s), s.radius);
    case Surface::Torus: {
        // A torus's seam is on its outer equator, where a top fillet ends. Turned from an arc,
        // the seam goes round the far side of the tube.
        const gp_Ax3 frame = frameOf(s);
        const V out = frame.XDirection().XYZ(), up = frame.Direction().XYZ();
        const V start = out * std::cos(s.facing + M_PI) + up * std::sin(s.facing + M_PI);
        const Handle(Geom_Circle) arc = new Geom_Circle(gp_Ax2(gp_Pnt(s.origin + out * s.radius), gp_Dir(out.Crossed(up)), gp_Dir(start)), s.tube);
        return new Geom_SurfaceOfRevolution(arc, gp_Ax1(gp_Pnt(s.origin), gp_Dir(up)));
    }
    case Surface::Cone: return new Geom_ConicalSurface(frameOf(s), std::atan(s.slope), s.radius);
    case Surface::Screw: {
        // Not a surface OpenCascade has: a spline laid over it, 24 steps a turn.
        const gp_Ax3 frame(gp_Pnt(s.origin), gp_Dir(s.axis));
        const V x = frame.XDirection().XYZ(), y = frame.YDirection().XYZ();
        double first, last;
        screwTurns(s, first, last);
        const int steps = std::max(8, int((last - first) * 24));
        // Rows evenly along the outline: unevenly spaced ones make the spline overshoot and fold.
        const int rows = std::max(4, 2 * int(s.outline.size()));
        TColgp_Array2OfPnt grid(1, steps + 1, 1, rows);
        for (int i = 0; i <= steps; ++i)
            for (int j = 0; j < rows; ++j) {
                const auto at = outlineAt(s, double(j) / (rows - 1));
                const double spin = (first + (last - first) * i / steps) * (s.pitch > 0 ? 1 : -1) * 2 * M_PI;
                grid.SetValue(i + 1, j + 1, gp_Pnt(s.origin + x * (at[1] * std::cos(spin)) + y * (at[1] * std::sin(spin)) + s.axis * (at[0] + s.pitch * spin / (2 * M_PI))));
            }
        GeomAPI_PointsToBSplineSurface make;
        make.Interpolate(grid, Approx_IsoParametric);
        if (!make.IsDone())
            return nullptr;
        return make.Surface();
    }
    }
    return nullptr;
}

// A face's outline can't touch itself. Grown patches do: spikes one triangle wide, and corners
// where two parts of a patch meet at a point. Give such triangles back to the facets (or, across
// the cut of a full round, to the other half) until it's clean.
void tidy(const Facets &f, const std::vector<Surface> &fitted, std::vector<int> &owner, std::vector<int> &side) {
    const Mesh &m = f.mesh;
    std::vector<std::vector<int>> around(m.points.size());
    for (std::size_t t = 0; t < m.triangles.size(); ++t)
        if (owner[t] >= 0)
            for (const int p : m.triangles[t])
                around[std::size_t(p)].push_back(int(t));
    const auto same = [&](int a, int b) { return owner[std::size_t(a)] == owner[std::size_t(b)] && side[std::size_t(a)] == side[std::size_t(b)]; };
    std::vector<int> fan;
    for (int pass = 0; pass < 8; ++pass) {
        bool changed = false;
        for (std::size_t t = 0; t < owner.size(); ++t) {
            if (owner[t] < 0)
                continue;
            int joined = 0, sibling = -1, shared = -1;
            for (int k = 0; k < 3; ++k) {
                const int other = f.across[t][std::size_t(k)];
                if (other >= 0 && owner[std::size_t(other)] == owner[t]) {
                    if (same(int(t), other)) {
                        ++joined;
                        shared = k;
                    } else {
                        sibling = other;
                    }
                }
            }
            if (joined > 1)
                continue;
            // A spike comes to a sharp tip. A strip's square end doesn't, however narrow the strip:
            // the stretch of a round between two grooves can be a row of four triangles.
            if (joined == 1) {
                const auto &tri = m.triangles[t];
                const V tip = pointOf(m, tri[std::size_t((shared + 2) % 3)]);
                const V a = pointOf(m, tri[std::size_t(shared)]) - tip, b = pointOf(m, tri[std::size_t((shared + 1) % 3)]) - tip;
                if (a.Dot(b) <= a.Modulus() * b.Modulus() * 0.5)
                    continue;
                // Where two fillets meet in a corner of a flat, each really does end in a point.
                const Surface &on = fitted[std::size_t(owner[t])];
                const bool round = on.kind == Surface::Torus || on.kind == Surface::Cylinder;
                bool exact = round || on.kind == Surface::Cone;
                for (const int p : tri)
                    exact = exact && distanceTo(on, pointOf(m, p)) <= (round ? 5e-3 : 0.035);
                if (exact)
                    continue;
            }
            if (sibling >= 0)
                side[t] = side[std::size_t(sibling)];
            else
                owner[t] = -1;
            changed = true;
        }
        for (std::size_t p = 0; p < around.size(); ++p) {
            auto &list = around[p];
            list.erase(std::remove_if(list.begin(), list.end(), [&](int t) { return owner[std::size_t(t)] < 0; }), list.end());
            if (list.size() < 2)
                continue;
            // Fans of one label round this point: walk across the sides that hold the point.
            std::vector<int> fanOf(list.size(), -1);
            std::vector<int> sizes;
            for (std::size_t i = 0; i < list.size(); ++i) {
                if (fanOf[i] >= 0)
                    continue;
                const int id = int(sizes.size());
                sizes.push_back(0);
                fan = {int(i)};
                fanOf[i] = id;
                while (!fan.empty()) {
                    const int at = list[std::size_t(fan.back())];
                    fan.pop_back();
                    ++sizes.back();
                    const auto &tri = m.triangles[std::size_t(at)];
                    for (int k = 0; k < 3; ++k) {
                        if (tri[std::size_t(k)] != int(p) && tri[std::size_t((k + 1) % 3)] != int(p))
                            continue;
                        const int other = f.across[std::size_t(at)][std::size_t(k)];
                        if (other < 0 || !same(at, other))
                            continue;
                        const auto it = std::find(list.begin(), list.end(), other);
                        if (it != list.end() && fanOf[std::size_t(it - list.begin())] < 0) {
                            fanOf[std::size_t(it - list.begin())] = id;
                            fan.push_back(int(it - list.begin()));
                        }
                    }
                }
            }
            // Fans of the same label beyond the biggest go.
            for (std::size_t i = 0; i < list.size(); ++i) {
                bool beaten = false;
                for (std::size_t j = 0; j < list.size(); ++j)
                    beaten = beaten || (same(list[i], list[j]) && fanOf[i] != fanOf[j]
                                        && (sizes[std::size_t(fanOf[j])] > sizes[std::size_t(fanOf[i])]
                                            || (sizes[std::size_t(fanOf[j])] == sizes[std::size_t(fanOf[i])] && fanOf[j] < fanOf[i])));
                if (beaten) {
                    owner[std::size_t(list[i])] = -1;
                    changed = true;
                }
            }
        }
        if (!changed)
            break;
    }
}

// Build faces from labelled triangles. Returns the shape, or null; `bad` gets the labels that
// pinch at a point (a face can't), to be split into single triangles and tried again.
TopoDS_Shape build(const Mesh &m, const std::vector<int> &labels, const std::vector<Surface> &surfaces,
                   double tol, std::set<int> &bad, const Stage &stage, std::vector<std::pair<TopoDS_Face, int>> *made) {
    // Every side where the label changes, directed as its triangle runs it.
    struct Side { int from, to, label; };
    std::map<Key, std::vector<Side>> sides;
    for (std::size_t t = 0; t < m.triangles.size(); ++t)
        for (int k = 0; k < 3; ++k) {
            const int a = m.triangles[t][std::size_t(k)], b = m.triangles[t][std::size_t((k + 1) % 3)];
            sides[sideKey(a, b)].push_back({a, b, labels[t]});
        }
    std::vector<Side> boundary;
    std::unordered_map<int, std::vector<int>> atPoint;    // point -> boundary sides touching it (unique geometry)
    std::map<Key, Key> pairOf;                            // geometry side -> its pair of labels
    std::vector<Key> edgeSides;
    for (const auto &[key, on] : sides) {
        std::set<int> kinds;
        for (const Side &s : on)
            kinds.insert(s.label);
        if (on.size() == 2 && kinds.size() == 1)
            continue;
        pairOf[key] = {*kinds.begin(), *kinds.rbegin()};
        const int id = int(edgeSides.size());
        edgeSides.push_back(key);
        atPoint[key.first].push_back(id);
        atPoint[key.second].push_back(id);
    }

    // A point where the outline can't run straight on: chains end there.
    auto junction = [&](int p) {
        const auto &list = atPoint.at(p);
        return list.size() != 2 || pairOf.at(edgeSides[std::size_t(list[0])]) != pairOf.at(edgeSides[std::size_t(list[1])]);
    };

    // Chains: walk from each junction along sides of one label pair; loops with none get two.
    std::vector<Chain> chains;
    std::vector<char> walked(edgeSides.size(), 0);
    auto walk = [&](int start, int firstSide) {
        Chain c;
        const Key pair = pairOf.at(edgeSides[std::size_t(firstSide)]);
        c.a = pair.first;
        c.b = pair.second;
        c.points = {start};
        int side = firstSide, at = start;
        while (true) {
            walked[std::size_t(side)] = 1;
            const Key k = edgeSides[std::size_t(side)];
            at = k.first == at ? k.second : k.first;
            c.points.push_back(at);
            if (at == start || junction(at))
                break;
            const auto &list = atPoint.at(at);
            const int next = list[0] == side ? list[1] : list[0];
            if (walked[std::size_t(next)])
                break;
            side = next;
        }
        chains.push_back(std::move(c));
    };
    for (const auto &[p, list] : atPoint)
        if (junction(p))
            for (const int s : list)
                if (!walked[std::size_t(s)])
                    walk(p, s);
    for (std::size_t s = 0; s < edgeSides.size(); ++s)
        if (!walked[s])
            walk(edgeSides[s].first, int(s));

    // Straight runs between flats give one edge per corner; a chain beside a round surface is
    // one curve. A closed loop is cut in two so every edge has two ends.
    std::vector<Chain> pieces;
    auto roundBeside = [&](const Chain &c) {
        return surfaces[std::size_t(c.a)].kind != Surface::Flat || surfaces[std::size_t(c.b)].kind != Surface::Flat;
    };
    auto cutAtCorners = [&](const Chain &c) {
        Chain run = c;
        run.points = {c.points.front()};
        for (std::size_t i = 1; i + 1 < c.points.size(); ++i) {
            run.points.push_back(c.points[i]);
            const V u = pointOf(m, c.points[i]) - pointOf(m, c.points[i - 1]);
            const V w = pointOf(m, c.points[i + 1]) - pointOf(m, c.points[i]);
            if (u.Crossed(w).Modulus() > 1e-6 * u.Modulus() * w.Modulus()) {
                pieces.push_back(run);
                run.points = {c.points[i]};
            }
        }
        run.points.push_back(c.points.back());
        pieces.push_back(run);
    };
    auto place = [&](const Chain &c) {
        if (roundBeside(c))
            pieces.push_back(c);
        else
            cutAtCorners(c);
    };
    for (const Chain &c : chains) {
        if (c.points.front() != c.points.back() || c.points.size() <= 2) {
            place(c);
            continue;
        }
        const std::size_t half = c.points.size() / 2;
        Chain one = c, two = c;
        one.points.assign(c.points.begin(), c.points.begin() + long(half) + 1);
        two.points.assign(c.points.begin() + long(half), c.points.end());
        place(one);
        place(two);
    }

    // Geometry: one vertex per end, one edge per piece, shared by the faces on either side.
    stage("Building faces");
    // A mesh's points sit near their surfaces, not on them (STL rounds them to floats, and a fit
    // is a fit): each corner and edge is told how far, or the checks say it doesn't join up.
    BRep_Builder builder;
    auto gap = [&](int p, const Chain &c) {
        return std::max(distanceTo(surfaces[std::size_t(c.a)], pointOf(m, p)), distanceTo(surfaces[std::size_t(c.b)], pointOf(m, p)));
    };
    std::unordered_map<int, double> gapAt;
    for (const Chain &c : pieces)
        for (const int p : c.points) {
            double &worst = gapAt[p];
            worst = std::max(worst, gap(p, c));
        }
    for (const auto &[p, list] : atPoint) {
        double &worst = gapAt[p];
        for (const int side : list) {
            const Key pair = pairOf.at(edgeSides[std::size_t(side)]);
            worst = std::max({worst, distanceTo(surfaces[std::size_t(pair.first)], pointOf(m, p)),
                              distanceTo(surfaces[std::size_t(pair.second)], pointOf(m, p))});
        }
    }
    constexpr double kMargin = 1.5, kFloor = 1e-6, kExact = 1e-7;
    // Each corner where its surfaces truly meet, when they do near it.
    std::unordered_map<int, V> meets;
    std::unordered_map<int, double> moved;
    std::unordered_map<int, std::set<int>> honoured;   // the surfaces a corner is truly on
    std::vector<double> areaOf(surfaces.size(), 0);
    for (std::size_t t = 0; t < m.triangles.size(); ++t)
        if (labels[t] >= 0) {
            const auto &tri = m.triangles[t];
            areaOf[std::size_t(labels[t])] += (pointOf(m, tri[1]) - pointOf(m, tri[0])).Crossed(pointOf(m, tri[2]) - pointOf(m, tri[0])).Modulus() / 2;
        }
    auto surfacesAt = [&](int p) {
        std::set<int> at;
        for (const int side : atPoint.at(p)) {
            const Key pair = pairOf.at(edgeSides[std::size_t(side)]);
            at.insert(pair.first);
            at.insert(pair.second);
        }
        return std::vector<int>(at.begin(), at.end());
    };
    for (const Chain &c : pieces)
        for (const int p : {c.points.front(), c.points.back()}) {
            if (moved.count(p))
                continue;
            moved[p] = -1;
            // The biggest surfaces first: where small facets crowd a corner and don't all meet, the
            // corner stays on the walls they edge, the facets taking up the difference.
            std::vector<int> on = surfacesAt(p);
            std::sort(on.begin(), on.end(), [&](int a, int b) { return areaOf[std::size_t(a)] > areaOf[std::size_t(b)]; });
            for (; on.size() >= 2; on.pop_back()) {
                std::vector<const Surface *> these;
                double widest = 0;
                for (const int l : on) {
                    const Surface &s = surfaces[std::size_t(l)];
                    these.push_back(&s);
                    if (s.kind != Surface::Flat && s.kind != Surface::Screw)
                        widest = std::max(widest, s.kind == Surface::Torus ? s.tube : s.radius);
                }
                // A round meeting a flat it touches does so along a line: a gap off it slides the
                // corner sqrt(2 r gap) along, not gap.
                const double reach = std::max({10 * gapAt[p], 3 * std::sqrt(2 * widest * gapAt[p]), 1e-6});
                V at;
                if (!meetingPoint(these, pointOf(m, p), std::min(0.1, reach), at))
                    continue;
                meets[p] = at;
                honoured[p] = {on.begin(), on.end()};
                double off = (at - pointOf(m, p)).Modulus();
                for (const int l : surfacesAt(p))
                    if (!honoured[p].count(l))
                        off = std::max(off, gapAt[p] + distanceTo(surfaces[std::size_t(l)], at));
                moved[p] = off;
                break;
            }
        }
    const auto onBoth = [&](int p, const Chain &c) {
        const auto it = honoured.find(p);
        return it != honoured.end() && it->second.count(c.a) && it->second.count(c.b);
    };
    // An edge is exact when it is where its two surfaces meet, end to end: a straight one between two
    // flats, or the circle two turned surfaces share.
    std::vector<char> exactPiece(pieces.size(), 0);
    std::vector<gp_Circ> circles(pieces.size());
    std::vector<double> worstOf(pieces.size(), 0);
    for (std::size_t i = 0; i < pieces.size(); ++i) {
        const Chain &c = pieces[i];
        for (const int p : c.points)
            worstOf[i] = std::max(worstOf[i], gapAt[p]);
        const bool ends = onBoth(c.points.front(), c) && onBoth(c.points.back(), c);
        if (!roundBeside(c))
            exactPiece[i] = ends;
        else if (sharedCircle(surfaces[std::size_t(c.a)], surfaces[std::size_t(c.b)], m, c.points, std::max(worstOf[i] * kMargin, kFloor), circles[i]))
            exactPiece[i] = ends ? 1 : 2;
        else if (ends) {
            // A flat along a round wall, or two side by side, meet in a straight line: exact when
            // all of it lies on both and the chain follows it.
            const V from = meets.at(c.points.front()), to = meets.at(c.points.back());
            bool on = (to - from).Modulus() > 1e-9;
            for (int k = 1; on && k < 8; ++k)
                on = distanceTo(surfaces[std::size_t(c.a)], from + (to - from) * (k / 8.0)) < 1e-9
                     && distanceTo(surfaces[std::size_t(c.b)], from + (to - from) * (k / 8.0)) < 1e-9;
            const V along = (to - from) / std::max((to - from).Modulus(), 1e-12);
            for (const int p : c.points) {
                const V d = pointOf(m, p) - from;
                on = on && (d - along * d.Dot(along)).Modulus() <= std::max(worstOf[i] * kMargin, kFloor);
            }
            if (on)
                exactPiece[i] = 3;
        }
    }
    // A corner all of whose edges are exact is exact; one with a traced curve keeps room for it.
    std::unordered_map<int, char> allExact;
    for (std::size_t i = 0; i < pieces.size(); ++i)
        for (const int p : {pieces[i].points.front(), pieces[i].points.back()}) {
            auto it = allExact.emplace(p, 1).first;
            it->second = it->second && (exactPiece[i] == 1 || exactPiece[i] == 3);
        }
    std::unordered_map<int, TopoDS_Vertex> vertices;
    auto vertexOf = [&](int p) -> const TopoDS_Vertex & {
        auto it = vertices.find(p);
        if (it == vertices.end()) {
            const bool exact = meets.count(p) > 0;
            it = vertices.emplace(p, BRepBuilderAPI_MakeVertex(gp_Pnt(exact ? meets.at(p) : pointOf(m, p))).Vertex()).first;
            builder.UpdateVertex(it->second, exact && allExact[p] ? kExact
                                             : std::max((gapAt[p] + std::max(0.0, moved[p])) * kMargin, kFloor));
        }
        return it->second;
    };
    std::vector<TopoDS_Edge> edges(pieces.size());
    // label -> ends of its pieces, directed its way round
    struct Run { int from, to, piece; bool forward; };
    std::map<int, std::vector<Run>> ofLabel;
    std::map<Key, std::vector<Side>> directed;
    for (const auto &[key, on] : sides)
        if (pairOf.count(key))
            directed[key] = on;
    for (std::size_t i = 0; i < pieces.size(); ++i) {
        const Chain &c = pieces[i];
        const bool round = roundBeside(c);
        const TopoDS_Vertex &vs = vertexOf(c.points.front()), &ve = vertexOf(c.points.back());
        // Tight, however loose the surfaces are: a thin face's sides would wobble across each other.
        const double worst = worstOf[i];
        edges[i] = round && exactPiece[i] != 3 ? curved(m, c.points, vs, ve, std::min(tol, 0.003), exactPiece[i] ? &circles[i] : nullptr)
                                               : BRepBuilderAPI_MakeEdge(vs, ve).Edge();
        builder.UpdateEdge(edges[i], exactPiece[i] == 1 || exactPiece[i] == 3 ? kExact : std::max(worst * kMargin, kFloor));
        const auto &on = directed.at(sideKey(c.points[0], c.points[1]));
        std::set<std::pair<int, bool>> added;
        for (const Side &s : on) {
            const bool forward = s.from == c.points[0];
            if (added.insert({s.label, forward}).second)
                ofLabel[s.label].push_back({forward ? c.points.front() : c.points.back(),
                                            forward ? c.points.back() : c.points.front(), int(i), forward});
        }
    }

    TopoDS_Shell shell;
    builder.MakeShell(shell);
    int done = 0;
    for (auto &[label, runs] : ofLabel) {
        if (++done % 512 == 0)
            stage("Building faces");
        std::unordered_map<int, std::vector<int>> from;
        for (std::size_t r = 0; r < runs.size(); ++r)
            from[runs[r].from].push_back(int(r));
        for (const auto &[p, list] : from)
            if (list.size() > 1 && surfaces[std::size_t(label)].kind == Surface::Flat)
                bad.insert(label);
        if (bad.count(label))
            continue;
        std::vector<char> used(runs.size(), 0);
        const Surface &surface = surfaces[std::size_t(label)];
        Handle(Geom_Surface) geometry = surfaceOf(surface);
        TopoDS_Face face;
        builder.MakeFace(face, geometry, Precision::Confusion());
        // Loops run anticlockwise seen from outside. A hole's surface faces the other way, so
        // there they're laid down reversed and the whole face turned over.
        bool complete = true;
        for (std::size_t r = 0; r < runs.size(); ++r) {
            if (used[r])
                continue;
            std::vector<TopoDS_Edge> loop;
            int at = int(r);
            const int start = runs[r].from;
            while (true) {
                used[std::size_t(at)] = 1;
                const Run &run = runs[std::size_t(at)];
                TopoDS_Edge e = edges[std::size_t(run.piece)];
                loop.push_back(TopoDS::Edge(e.Oriented(run.forward != surface.inward ? TopAbs_FORWARD : TopAbs_REVERSED)));
                if (run.to == start)
                    break;
                const auto hit = from.find(run.to);
                int next = -1;
                if (hit != from.end())
                    for (const int candidate : hit->second)
                        if (!used[std::size_t(candidate)])
                            next = candidate;
                if (next < 0) {
                    complete = false;
                    break;
                }
                at = next;
            }
            if (surface.inward)
                std::reverse(loop.begin(), loop.end());
            TopoDS_Wire wire;
            builder.MakeWire(wire);
            for (const TopoDS_Edge &e : loop)
                builder.Add(wire, e);
            builder.Add(face, wire);
        }
        if (!complete) {
            bad.insert(label);
            continue;
        }
        builder.Add(shell, surface.inward ? TopoDS::Face(face.Reversed()) : face);
        if (made)
            made->push_back({face, label});
    }
    if (!bad.empty())
        return {};
    return shell;
}

// A helix edge's place on a screw's spline is known outright, and projecting a many-turn edge
// takes minutes. Surface u is turns, v is along the outline, both straight lines by construction.
// A straight edge across a fillet stays near its surface, and where the two are nearly tangent the
// automatic projection wanders far off along it. The ends are known, so draw it straight between
// them. Fine only for short edges: on a long one a straight line in u, v isn't the edge.
bool filletEdge(const TopoDS_Edge &edge, const TopoDS_Face &face, const Handle(Geom_Surface) &geometry, double tol) {
    Standard_Real c0, c1;
    const Handle(Geom_Curve) curve = BRep_Tool::Curve(edge, c0, c1);
    if (curve.IsNull() || !curve->IsKind(STANDARD_TYPE(Geom_Line)))
        return false;
    GeomAPI_ProjectPointOnSurf from(curve->Value(c0), geometry), to(curve->Value(c1), geometry);
    if (from.NbPoints() == 0 || to.NbPoints() == 0)
        return false;
    Standard_Real u0, v0, u1, v1;
    from.LowerDistanceParameters(u0, v0);
    to.LowerDistanceParameters(u1, v1);
    if (std::abs(u1 - u0) > M_PI)
        u1 += u1 < u0 ? 2 * M_PI : -2 * M_PI;
    if (std::abs(v1 - v0) > M_PI && geometry->IsVPeriodic())
        v1 += v1 < v0 ? 2 * M_PI : -2 * M_PI;
    TColgp_Array1OfPnt2d poles(1, 2);
    poles.SetValue(1, gp_Pnt2d(u0, v0));
    poles.SetValue(2, gp_Pnt2d(u1, v1));
    TColStd_Array1OfReal knots(1, 2);
    knots.SetValue(1, c0);
    knots.SetValue(2, c1);
    TColStd_Array1OfInteger times(1, 2);
    times.Init(2);
    BRep_Builder().UpdateEdge(edge, new Geom2d_BSplineCurve(poles, knots, times, 1), face, tol);
    return true;
}

bool screwEdge(const TopoDS_Edge &edge, const TopoDS_Face &face, const Surface &s, double tol) {
    Standard_Real c0, c1;
    const Handle(Geom_Curve) curve = BRep_Tool::Curve(edge, c0, c1);
    if (curve.IsNull())
        return false;
    constexpr int kSamples = 400;
    double first, last;
    screwTurns(s, first, last);
    const double turn = std::abs(s.pitch);
    std::vector<gp_Pnt2d> uv;
    std::vector<double> at;
    for (int i = 0; i <= kSamples; ++i) {
        const double param = c0 + (c1 - c0) * i / kSamples;
        const V p = curve->Value(param).XYZ();
        double w, rho;
        onScrew(s, p, w, rho);
        double position;
        std::size_t piece;
        nearOutline(s, w, rho, position, piece);
        const double along = outlineShare(s, position);
        const double turns = ((p - s.origin).Dot(s.axis) - w) / turn;
        uv.emplace_back((turns - first) / (last - first), along);
        at.push_back(param);
    }
    // Straight between the points that matter: a helix is one line on its surface, and a smooth
    // fit overshoots where a chord runs off the outline's end.
    std::vector<char> kept(uv.size(), 0);
    kept.front() = kept.back() = 1;
    std::vector<std::pair<std::size_t, std::size_t>> stack{{0, uv.size() - 1}};
    while (!stack.empty()) {
        const auto [a, b] = stack.back();
        stack.pop_back();
        const gp_XY along = uv[b].XY() - uv[a].XY();
        const double share = std::max(along.Modulus(), 1e-12);
        double worst = 1e-5;
        std::size_t far = 0;
        for (std::size_t i = a + 1; i < b; ++i) {
            const double off = std::abs(along.Crossed(uv[i].XY() - uv[a].XY())) / share;
            if (off > worst) {
                worst = off;
                far = i;
            }
        }
        if (far) {
            kept[far] = 1;
            stack.push_back({a, far});
            stack.push_back({far, b});
        }
    }
    const int count = std::accumulate(kept.begin(), kept.end(), 0);
    TColgp_Array1OfPnt2d poles(1, count);
    TColStd_Array1OfReal knots(1, count);
    TColStd_Array1OfInteger times(1, count);
    times.Init(1);
    times.SetValue(1, 2);
    times.SetValue(count, 2);
    for (std::size_t i = 0, n = 1; i < uv.size(); ++i)
        if (kept[i]) {
            poles.SetValue(int(n), uv[i]);
            knots.SetValue(int(n++), at[i]);
        }
    BRep_Builder().UpdateEdge(edge, new Geom2d_BSplineCurve(poles, knots, times, 1), face, tol);
    return true;
}

// Everything in one pass, re-splitting pinched flats until it builds.
TopoDS_Shape assemble(const Mesh &m, std::vector<int> labels, std::vector<Surface> surfaces, double tol, const Stage &stage,
                      std::vector<std::pair<TopoDS_Face, int>> *made = nullptr) {
    const Facets f(m);
    TopoDS_Shape shell;
    for (int round = 0; round < 6; ++round) {
        std::set<int> bad;
        if (made)
            made->clear();
        shell = build(m, labels, surfaces, tol, bad, stage, made);
        if (bad.empty())
            break;
        // Split pinched areas: each of their triangles its own flat.
        for (std::size_t t = 0; t < labels.size(); ++t)
            if (bad.count(labels[t])) {
                Surface s;
                s.axis = f.normal[t];
                s.origin = pointOf(m, m.triangles[t][0]);
                labels[t] = int(surfaces.size());
                surfaces.push_back(s);
            }
    }
    if (shell.IsNull())
        return {};
    BRep_Builder builder;
    // Closed, every edge on two faces: a solid.
    bool closed = true;
    {
        TopTools_IndexedDataMapOfShapeListOfShape edgeFaces;
        TopExp::MapShapesAndAncestors(shell, TopAbs_EDGE, TopAbs_FACE, edgeFaces);
        for (int e = 1; e <= edgeFaces.Extent(); ++e)
            closed = closed && edgeFaces(e).Extent() == 2;
    }
    stage("Closing the solid");
    if (!closed)
        return shell;
    TopoDS_Shell sealed = TopoDS::Shell(shell);
    sealed.Closed(true);
    TopoDS_Solid solid;
    builder.MakeSolid(solid);
    builder.Add(solid, sealed);
    return solid;
}

// Smallest-spread direction of a set of unit vectors.
V leastSpread(const std::vector<V> &dirs, const std::vector<double> &weight, double *mean = nullptr) {
    V centre(0, 0, 0);
    double total = 0;
    for (std::size_t i = 0; i < dirs.size(); ++i) {
        centre += dirs[i] * weight[i];
        total += weight[i];
    }
    centre /= std::max(total, 1e-12);
    math_Matrix cov(1, 3, 1, 3, 0.0);
    for (std::size_t i = 0; i < dirs.size(); ++i) {
        const V d = dirs[i] - centre;
        for (int r = 1; r <= 3; ++r)
            for (int c = 1; c <= 3; ++c)
                cov(r, c) += weight[i] * d.Coord(r) * d.Coord(c);
    }
    math_Jacobi jacobi(cov);
    int smallest = 1;
    for (int i = 2; i <= 3; ++i)
        if (jacobi.Value(i) < jacobi.Value(smallest))
            smallest = i;
    math_Vector v(1, 3);
    jacobi.Vector(smallest, v);
    V axis(v(1), v(2), v(3));
    axis /= std::max(axis.Modulus(), 1e-12);
    if (mean)
        *mean = centre.Dot(axis);
    return axis;
}

// A round surface through a group of triangles, from where their normals point: the simplest
// that holds, cylinder (normals square to one axis), cone (at a set angle to it), then sphere
// (normals meeting at one point; points on two circles alone fit one too, so it comes last and
// needs normals that really spread in every direction). false if none is any good.
bool fitRound(const Facets &f, const std::vector<int> &group, double share, double capMm, Surface &best, double &bestOff) {
    std::vector<V> dirs, mids;
    std::vector<double> weight;
    std::set<int> pts;
    for (const int t : group) {
        dirs.push_back(f.normal[std::size_t(t)]);
        weight.push_back(f.area[std::size_t(t)]);
        const auto &tri = f.mesh.triangles[std::size_t(t)];
        mids.push_back((pointOf(f.mesh, tri[0]) + pointOf(f.mesh, tri[1]) + pointOf(f.mesh, tri[2])) / 3);
        pts.insert(tri.begin(), tri.end());
    }
    const auto solveLines = [&](const V &around, V &out) {
        // Point nearest every normal line; along an axis it's free, so a small pull keeps it near.
        math_Matrix A(1, 3, 1, 3, 0.0);
        math_Vector b(1, 3, 0.0);
        for (std::size_t i = 0; i < dirs.size(); ++i)
            for (int r = 1; r <= 3; ++r)
                for (int c = 1; c <= 3; ++c) {
                    const double e = (r == c ? 1.0 : 0.0) - dirs[i].Coord(r) * dirs[i].Coord(c);
                    A(r, c) += weight[i] * e;
                    b(r) += weight[i] * e * mids[i].Coord(c);
                }
        double total = std::accumulate(weight.begin(), weight.end(), 0.0);
        for (int r = 1; r <= 3; ++r) {
            A(r, r) += total * 1e-6;
            b(r) += total * 1e-6 * around.Coord(r);
        }
        const double a = A(1, 1), bb = A(1, 2), c = A(1, 3), d = A(2, 2), e = A(2, 3), g = A(3, 3);
        const double det = a * (d * g - e * e) - bb * (bb * g - e * c) + c * (bb * e - d * c);
        if (std::abs(det) < 1e-18)
            return false;
        out.SetX(((d * g - e * e) * b(1) + (c * e - bb * g) * b(2) + (bb * e - c * d) * b(3)) / det);
        out.SetY(((c * e - bb * g) * b(1) + (a * g - c * c) * b(2) + (bb * c - a * e) * b(3)) / det);
        out.SetZ(((bb * e - c * d) * b(1) + (bb * c - a * e) * b(2) + (a * d - bb * bb) * b(3)) / det);
        return true;
    };

    V centroid(0, 0, 0);
    for (const V &m : mids)
        centroid += m;
    centroid /= double(mids.size());

    double meanDot = 0;
    const V axis = leastSpread(dirs, weight, &meanDot);
    V onAxis;
    if (!solveLines(centroid, onAxis))
        return false;

    bool found = false;
    auto consider = [&](const Surface &s) {
        double worst = 0;
        for (const int p : pts)
            worst = std::max(worst, distanceTo(s, pointOf(f.mesh, p)));
        const double allowed = std::min(capMm, std::max(s.radius, 1e-6) * share);
        if (worst > allowed || found)
            return;
        found = true;
        best = s;
        bestOff = worst;
    };

    // Which way out is the surface? The normals point away from the axis (a boss) or toward it
    // (a hole). `distanceTo` doesn't care; the orientation is settled on the face later.
    Surface cyl;
    cyl.kind = Surface::Cylinder;
    cyl.axis = axis;
    cyl.origin = onAxis;
    double sum = 0;
    for (const int p : pts) {
        const V d = pointOf(f.mesh, p) - onAxis;
        sum += (d - axis * d.Dot(axis)).Modulus();
    }
    cyl.radius = sum / double(pts.size());
    double spreadAlong = 0;
    for (std::size_t i = 0; i < dirs.size(); ++i)
        spreadAlong = std::max(spreadAlong, std::abs(dirs[i].Dot(axis) - meanDot));
    const bool ring = spreadAlong < 0.05;    // every normal at one angle to the axis
    if (ring)
        consider(cyl);

    // radius against height along the axis
    double sh = 0, sr = 0, shh = 0, shr = 0;
    for (const int p : pts) {
        const V d = pointOf(f.mesh, p) - onAxis;
        const double h = d.Dot(axis), r = (d - axis * h).Modulus();
        sh += h; sr += r; shh += h * h; shr += h * r;
    }
    const double n = double(pts.size()), denom = n * shh - sh * sh;
    if (ring && std::abs(denom) > 1e-9) {
        Surface cone;
        cone.kind = Surface::Cone;
        cone.axis = axis;
        cone.origin = onAxis;
        cone.slope = (n * shr - sh * sr) / denom;
        cone.radius = (sr - cone.slope * sh) / n;
        if (std::abs(cone.slope) > 0.01 && std::abs(cone.slope) < 3 && cone.radius > 0)
            consider(cone);
    }

    // A sphere's axis lies across the patch, so its poles (where a surface pinches) stay off it.
    V facing(0, 0, 0);
    for (std::size_t i = 0; i < dirs.size(); ++i)
        facing += dirs[i] * weight[i];
    V across = facing.Crossed(std::abs(facing.X()) < std::abs(facing.Y()) ? V(1, 0, 0) : V(0, 1, 0));
    Surface ball;
    ball.kind = Surface::Sphere;
    ball.axis = across / std::max(across.Modulus(), 1e-12);
    ball.origin = onAxis;
    double around = 0;
    for (const int p : pts)
        around += (pointOf(f.mesh, p) - onAxis).Modulus();
    ball.radius = around / n;
    if (!ring)
        consider(ball);
    return found;
}

// Sections (owner, 2026-10-31): slice along the part's length, find the arcs in each slice, and an
// arc that carries on from slice to slice is one surface up the height. Its centre and radius are
// read over the whole height at once, so a gentle draft a small patch can't see still comes out as
// a cone. `seeds` are the triangles under its arcs.
struct Arc {
    double h, cu, cv, r;
    std::vector<std::array<double, 3>> points;    // h, u, v
    std::vector<int> triangles;
};

bool circleOf(const std::vector<std::array<double, 2>> &p, std::size_t from, std::size_t to, double &cu, double &cv, double &r, double &worst) {
    double a = 0, b = 0, c = 0, d = 0, e = 0, x = 0, y = 0, z = 0;
    const double n = double(to - from + 1);
    for (std::size_t i = from; i <= to; ++i) {
        const double u = p[i][0], v = p[i][1], w = u * u + v * v;
        a += u * u; b += u * v; c += u; d += v * v; e += v;
        x += u * w; y += v * w; z += w;
    }
    math_Matrix M(1, 3, 1, 3);
    M(1, 1) = 2 * a; M(1, 2) = 2 * b; M(1, 3) = c;
    M(2, 1) = 2 * b; M(2, 2) = 2 * d; M(2, 3) = e;
    M(3, 1) = 2 * c; M(3, 2) = 2 * e; M(3, 3) = n;
    math_Vector rhs(1, 3);
    rhs(1) = x; rhs(2) = y; rhs(3) = z;
    math_Gauss solve(M);
    if (!solve.IsDone())
        return false;
    math_Vector out(1, 3);
    solve.Solve(rhs, out);
    cu = out(1); cv = out(2);
    r = std::sqrt(std::max(out(3) + cu * cu + cv * cv, 0.0));
    worst = 0;
    for (std::size_t i = from; i <= to; ++i)
        worst = std::max(worst, std::abs(std::hypot(p[i][0] - cu, p[i][1] - cv) - r));
    return true;
}

void findArcs(const std::vector<std::array<double, 2>> &ring, const std::vector<int> &tris, double h, double tol, double most, std::vector<Arc> &arcs) {
    const std::size_t n = ring.size();
    if (n < 6)
        return;
    // Start at the sharpest corner, so no arc is cut in two by the start of the loop.
    std::size_t start = 0;
    double sharpest = -1;
    for (std::size_t i = 0; i < n; ++i) {
        const auto &a = ring[(i + n - 1) % n], &b = ring[i], &c = ring[(i + 1) % n];
        const double ux = b[0] - a[0], uy = b[1] - a[1], wx = c[0] - b[0], wy = c[1] - b[1];
        const double turn = std::abs(std::atan2(ux * wy - uy * wx, ux * wx + uy * wy));
        if (turn > sharpest) {
            sharpest = turn;
            start = i;
        }
    }
    std::vector<std::array<double, 2>> p;
    std::vector<int> t;
    for (std::size_t i = 0; i <= n; ++i) {
        p.push_back(ring[(start + i) % n]);
        t.push_back(tris[(start + i) % n]);
    }
    const auto offLine = [&](std::size_t a, std::size_t b) {
        const double dx = p[b][0] - p[a][0], dy = p[b][1] - p[a][1], len = std::max(std::hypot(dx, dy), 1e-12);
        double worst = 0;
        for (std::size_t i = a; i <= b; ++i)
            worst = std::max(worst, std::abs((p[i][0] - p[a][0]) * dy - (p[i][1] - p[a][1]) * dx) / len);
        return worst;
    };
    std::size_t i = 0;
    while (i < n) {
        std::size_t bestLine = i + 1, bestArc = 0;
        double cu = 0, cv = 0, r = 0;
        int misses = 0;
        for (std::size_t k = i + 2; k <= n; ++k) {
            bool held = false;
            const double off = offLine(i, k);
            if (off <= tol) {
                bestLine = k;
                held = true;
            }
            double u, v, radius, worst;
            if (k - i >= 3 && off > tol * 2 && circleOf(p, i, k, u, v, radius, worst) && worst <= tol && radius < most) {
                bestArc = k;
                cu = u; cv = v; r = radius;
                held = true;
            }
            if (held)
                misses = 0;
            else if (++misses > 6)
                break;
        }
        if (bestArc > bestLine) {
            Arc arc{h, cu, cv, r, {}, {}};
            for (std::size_t k = i; k <= bestArc; ++k) {
                arc.points.push_back({h, p[k][0], p[k][1]});
                arc.triangles.push_back(t[k]);
            }
            arcs.push_back(std::move(arc));
            i = bestArc;
        } else {
            i = bestLine;
        }
    }
}

// Least-squares circle-per-height: centre and radius both straight lines in h. false if it won't settle.
bool fitAlong(const std::vector<std::array<double, 3>> &pts, double (&q)[6]) {
    for (int step = 0; step < 12; ++step) {
        math_Matrix M(1, 6, 1, 6, 0.0);
        math_Vector rhs(1, 6, 0.0);
        for (const auto &p : pts) {
            const double h = p[0], du = p[1] - q[0] - q[1] * h, dv = p[2] - q[2] - q[3] * h;
            const double rho = std::max(std::hypot(du, dv), 1e-9), res = rho - q[4] - q[5] * h;
            const double g[6] = {-du / rho, -du / rho * h, -dv / rho, -dv / rho * h, -1, -h};
            for (int a = 0; a < 6; ++a) {
                rhs(a + 1) -= g[a] * res;
                for (int b = 0; b < 6; ++b)
                    M(a + 1, b + 1) += g[a] * g[b];
            }
        }
        for (int a = 1; a <= 6; ++a)
            M(a, a) += 1e-9;
        math_Gauss solve(M);
        if (!solve.IsDone())
            return false;
        math_Vector dq(1, 6);
        solve.Solve(rhs, dq);
        double moved = 0;
        for (int a = 0; a < 6; ++a) {
            q[a] += dq(a + 1);
            moved += std::abs(dq(a + 1));
        }
        if (moved < 1e-9)
            break;
    }
    return q[4] > 0;
}

// Threads (owner's speaker adapter, 5/8-27). A thread's points fall on one outline once each is
// slid along the helix to the same turn; the pitch is the one that makes that happen. Each straight
// piece of that outline, swept up the helix, is a surface. `rings` are slices that aren't round.
struct Ring { double h, cu, cv, low, high; };

void screwSurfaces(const Mesh &m, const V &axis, const V &uDir, const V &vDir, const std::vector<Ring> &rings, double hintU, double hintV, double tol, std::vector<Surface> &out) {
    std::vector<Ring> left = rings;
    for (int tried = 0; tried < 6 && !left.empty(); ++tried) {
        const Ring first = left.front();
        double cu = 0, cv = 0, low = INFINITY, high = -INFINITY, lo = INFINITY, hi = -INFINITY;
        std::vector<Ring> others, same;
        for (const Ring &r : left) {
            if (std::hypot(r.cu - first.cu, r.cv - first.cv) < 0.3 && std::abs(r.high - first.high) < 1 && std::abs(r.low - first.low) < 1)
                same.push_back(r);
            else
                others.push_back(r);
        }
        left = std::move(others);
        if (same.size() < 4)
            continue;
        for (const Ring &r : same) {
            cu += r.cu / double(same.size());
            cv += r.cv / double(same.size());
            lo = std::min(lo, r.h);
            hi = std::max(hi, r.h);
            low = std::min(low, r.low);
            high = std::max(high, r.high);
        }
        std::vector<std::array<double, 3>> raw, pts;    // height, u, v; height, angle, rho
        for (std::size_t i = 0; i < m.points.size(); ++i) {
            const V p = pointOf(m, int(i));
            const double h = p.Dot(axis), x = p.Dot(uDir), y = p.Dot(vDir), rho = std::hypot(x - cu, y - cv);
            if (h >= lo && h <= hi && rho >= low - 0.5 && rho <= high + 0.5)
                raw.push_back({h, x, y});
        }
        const auto centre = [&](double u, double v) {
            pts.clear();
            for (const auto &p : raw) {
                const double x = p[1] - u, y = p[2] - v;
                pts.push_back({p[0], std::atan2(y, x), std::hypot(x, y)});
            }
        };
        // A thread's slice sits off its axis, turning as it climbs; all its turns together don't.
        {
            std::vector<std::array<double, 2>> flat;
            for (const auto &p : raw)
                flat.push_back({p[1], p[2]});
            double r = 0, worst = 0;
            if (raw.size() > 3)
                circleOf(flat, 0, flat.size() - 1, cu, cv, r, worst);
        }
        centre(cu, cv);
        if (pts.size() < 40 || pts.size() > 60000)
            continue;
        constexpr int kBins = 40;
        const auto spread = [&](double pitch) {
            std::array<double, kBins> sum{}, square{};
            std::array<int, kBins> count{};
            const double turn = std::abs(pitch);
            for (const auto &p : pts) {
                double w = std::fmod(p[0] - pitch * p[1] / (2 * M_PI), turn);
                w += w < 0 ? turn : 0;
                const int b = std::min(kBins - 1, int(w / turn * kBins));
                sum[std::size_t(b)] += p[2];
                square[std::size_t(b)] += p[2] * p[2];
                ++count[std::size_t(b)];
            }
            double total = 0;
            for (int b = 0; b < kBins; ++b)
                if (count[std::size_t(b)] > 0)
                    total += square[std::size_t(b)] - sum[std::size_t(b)] * sum[std::size_t(b)] / count[std::size_t(b)];
            return total / double(pts.size());
        };
        double bestPitch = 0, best = INFINITY, mean = 0, whole = 0;
        for (const auto &p : pts)
            mean += p[2] / double(pts.size());
        for (const auto &p : pts)
            whole += (p[2] - mean) * (p[2] - mean) / double(pts.size());
        for (const double hand : {1.0, -1.0})
            for (double pitch = 0.3; pitch < std::min(10.0, (hi - lo) / 2); pitch += 0.004) {
                const double off = spread(pitch * hand);
                if (off < best) {
                    best = off;
                    bestPitch = pitch * hand;
                }
            }
        for (double step = 0.0004, around = bestPitch; step > 5e-5; step /= 4, around = bestPitch)
            for (int k = -5; k <= 5; ++k) {
                const double off = spread(around + k * step);
                if (off < best) {
                    best = off;
                    bestPitch = around + k * step;
                }
            }
        // A thread's slices sit off its axis and its mesh points bunch, so nothing above finds the
        // axis exactly; but only the true axis and pitch together put every point on one outline.
        {
            const double u0 = cu, v0 = cv, p0 = bestPitch;
            for (const auto &[startU, startV] : {std::pair{u0, v0}, {hintU, hintV}})
            for (int a = -12; a <= 12; ++a)
                for (int b = -12; b <= 12; ++b) {
                    centre(startU + a * 0.06, startV + b * 0.06);
                    for (int k = -4; k <= 4; ++k) {
                        const double off = spread(p0 + k * 0.003);
                        if (off < best) {
                            best = off;
                            cu = startU + a * 0.06;
                            cv = startV + b * 0.06;
                            bestPitch = p0 + k * 0.003;
                        }
                    }
                }
            for (double step = 0.03; step > 0.001; step /= 2) {
                bool moved = true;
                while (moved) {
                    moved = false;
                    for (const auto [du, dv] : {std::pair{step, 0.0}, {-step, 0.0}, {0.0, step}, {0.0, -step}}) {
                        centre(cu + du, cv + dv);
                        const double off = spread(bestPitch);
                        if (off < best) {
                            best = off;
                            cu += du;
                            cv += dv;
                            moved = true;
                        }
                    }
                }
            }
        }
        centre(cu, cv);
        for (double step = 0.002, around = bestPitch; step > 5e-5; step /= 4, around = bestPitch)
            for (int k = -5; k <= 5; ++k) {
                const double off = spread(around + k * step);
                if (off < best) {
                    best = off;
                    bestPitch = around + k * step;
                }
            }
        if (bestPitch == 0 || best > whole * 0.02)
            continue;

        // The outline: points in order across one turn, then only its corners.
        const double turn = std::abs(bestPitch);
        std::vector<std::array<double, 2>> sorted;
        for (const auto &p : pts) {
            double w = std::fmod(p[0] - bestPitch * p[1] / (2 * M_PI), turn);
            w += w < 0 ? turn : 0;
            sorted.push_back({w, p[2]});
        }
        std::sort(sorted.begin(), sorted.end());
        std::vector<std::array<double, 2>> line;
        for (std::size_t i = 0; i < sorted.size();) {
            double w = 0, rho = 0;
            std::size_t j = i;
            while (j < sorted.size() && sorted[j][0] - sorted[i][0] < turn / 200) {
                w += sorted[j][0];
                rho += sorted[j][1];
                ++j;
            }
            line.push_back({w / double(j - i), rho / double(j - i)});
            i = j;
        }
        if (line.size() < 3)
            continue;
        line.push_back({line.front()[0] + turn, line.front()[1]});
        std::vector<char> corner(line.size(), 0);
        corner.front() = corner.back() = 1;
        std::vector<std::pair<std::size_t, std::size_t>> stack{{0, line.size() - 1}};
        while (!stack.empty()) {
            const auto [a, b] = stack.back();
            stack.pop_back();
            const double dw = line[b][0] - line[a][0], dr = line[b][1] - line[a][1], len = std::max(std::hypot(dw, dr), 1e-12);
            double worst = tol * 0.2;
            std::size_t at = 0;
            for (std::size_t i = a + 1; i < b; ++i) {
                const double off = std::abs((line[i][0] - line[a][0]) * dr - (line[i][1] - line[a][1]) * dw) / len;
                if (off > worst) {
                    worst = off;
                    at = i;
                }
            }
            if (at) {
                corner[at] = 1;
                stack.push_back({a, at});
                stack.push_back({at, b});
            }
        }
        // The long straights are faces of their own (flanks, a flat crest); the short pieces
        // between them are one rounded face, a root or a crest.
        std::vector<std::size_t> at;
        for (std::size_t i = 0; i < line.size(); ++i)
            if (corner[i])
                at.push_back(i);
        double longest = 0;
        for (std::size_t i = 0; i + 1 < at.size(); ++i)
            longest = std::max(longest, std::hypot(line[at[i + 1]][0] - line[at[i]][0], line[at[i + 1]][1] - line[at[i]][1]));
        const auto straight = [&](std::size_t i) {
            return std::hypot(line[at[i + 1]][0] - line[at[i]][0], line[at[i + 1]][1] - line[at[i]][1]) >= longest * 0.25;
        };
        // Go round from the start of a long straight, so the join of the turn is inside no face.
        const std::size_t pieces = at.size() - 1;
        std::size_t begin = 0;
        while (begin < pieces && !(straight(begin) && !straight((begin + pieces - 1) % pieces)))
            ++begin;
        begin = begin == pieces ? 0 : begin;
        std::vector<std::array<double, 2>> outline;
        const auto finish = [&] {
            if (outline.size() < 2)
                return;
            // Straight to within the mesh's noise is straight: a kink of a thousandth of a mm
            // shows as blotches when the light runs along a crest.
            const auto &from = outline.front(), &to = outline.back();
            const double dw = to[0] - from[0], dr = to[1] - from[1], chord = std::max(std::hypot(dw, dr), 1e-12);
            double bow = 0;
            for (const auto &q : outline)
                bow = std::max(bow, std::abs((q[0] - from[0]) * dr - (q[1] - from[1]) * dw) / chord);
            if (bow <= tol * 0.25)
                outline = {outline.front(), outline.back()};
            Surface s;
            s.kind = Surface::Screw;
            s.origin = uDir * cu + vDir * cv;
            s.axis = axis;
            s.pitch = bestPitch;
            s.low = lo - turn;
            s.high = hi + turn;
            s.outline = outline;
            s.radius = (outline.front()[1] + outline.back()[1]) / 2;
            out.push_back(s);
        };
        for (std::size_t k = 0; k < pieces; ++k) {
            const std::size_t i = (begin + k) % pieces;
            const double shift = i < begin ? turn : 0;
            const std::array<double, 2> from{line[at[i]][0] + shift, line[at[i]][1]}, to{line[at[i + 1]][0] + shift, line[at[i + 1]][1]};
            if (straight(i) || (!outline.empty() && straight((i + pieces - 1) % pieces))) {
                finish();
                outline.clear();
            }
            if (outline.empty())
                outline.push_back(from);
            outline.push_back(to);
        }
        finish();
    }
}

// A round broken up by grooves or notches: each stretch alone is two or three flats, too little to
// read, but all their corners sit on one circle. Take the corners of a slice (points along a flat
// are only where a triangle's diagonal crossed it), try the circle through each three in a row and
// keep the one most corners agree with. Corners a polygon's width apart aren't a round: a hexagon
// has six on a circle too, so most must be close neighbours.
void findBrokenCircles(const std::vector<std::array<double, 2>> &ring, const std::vector<int> &tris, double h, double tol, double most, std::vector<Arc> &arcs) {
    const std::size_t n = ring.size();
    if (n < 6)
        return;
    std::vector<std::size_t> corners;
    for (std::size_t i = 0; i < n; ++i) {
        const auto &a = ring[(i + n - 1) % n], &b = ring[i], &c = ring[(i + 1) % n];
        const double ux = b[0] - a[0], uy = b[1] - a[1], vx = c[0] - b[0], vy = c[1] - b[1];
        if (std::abs(std::atan2(ux * vy - uy * vx, ux * vx + uy * vy)) > 0.3 * M_PI / 180)
            corners.push_back(i);
    }
    const std::size_t count = corners.size();
    if (count < 6)
        return;
    constexpr double kNeighbours = 27 * M_PI / 180;
    std::vector<char> used(count, 0);
    for (int found = 0; found < 8; ++found) {
        double bestU = 0, bestV = 0, bestR = 0;
        std::size_t bestCount = 0;
        const std::size_t step = std::max<std::size_t>(1, count / 96);
        for (std::size_t i = 0; i < count; i += step) {
            const auto &a = ring[corners[i]], &b = ring[corners[(i + 1) % count]], &c = ring[corners[(i + 2) % count]];
            const double d = 2 * (a[0] * (b[1] - c[1]) + b[0] * (c[1] - a[1]) + c[0] * (a[1] - b[1]));
            if (std::abs(d) < 1e-9)
                continue;
            const double aa = a[0] * a[0] + a[1] * a[1], bb = b[0] * b[0] + b[1] * b[1], cc = c[0] * c[0] + c[1] * c[1];
            const double u = (aa * (b[1] - c[1]) + bb * (c[1] - a[1]) + cc * (a[1] - b[1])) / d;
            const double v = (aa * (c[0] - b[0]) + bb * (a[0] - c[0]) + cc * (b[0] - a[0])) / d;
            const double r = std::hypot(a[0] - u, a[1] - v);
            if (r > most)
                continue;
            std::size_t on = 0;
            for (std::size_t k = 0; k < count; ++k)
                on += !used[k] && std::abs(std::hypot(ring[corners[k]][0] - u, ring[corners[k]][1] - v) - r) <= tol * 0.5;
            if (on > bestCount) {
                bestCount = on;
                bestU = u;
                bestV = v;
                bestR = r;
            }
        }
        if (bestCount < 6)
            return;
        std::vector<std::size_t> on;
        for (std::size_t k = 0; k < count; ++k)
            if (!used[k] && std::abs(std::hypot(ring[corners[k]][0] - bestU, ring[corners[k]][1] - bestV) - bestR) <= tol * 0.5)
                on.push_back(k);
        std::size_t close = 0;
        for (std::size_t k = 0; k < on.size(); ++k) {
            const auto &a = ring[corners[on[k]]], &b = ring[corners[on[(k + 1) % on.size()]]];
            close += std::abs(std::remainder(std::atan2(b[1] - bestV, b[0] - bestU) - std::atan2(a[1] - bestV, a[0] - bestU), 2 * M_PI)) <= kNeighbours;
        }
        if (close * 2 < on.size())
            return;
        Arc arc{h, bestU, bestV, bestR, {}, {}};
        for (const std::size_t k : on) {
            used[k] = 1;
            const std::size_t i = corners[k];
            arc.points.push_back({h, ring[i][0], ring[i][1]});
            arc.triangles.push_back(tris[i]);
            arc.triangles.push_back(tris[(i + n - 1) % n]);
        }
        arcs.push_back(std::move(arc));
    }
}

// A circle through (x, y) points, by least squares.
bool circleThrough(const std::vector<std::array<double, 2>> &p, double &cx, double &cy, double &r) {
    if (p.size() < 3)
        return false;
    math_Matrix a(1, 3, 1, 3, 0.0);
    math_Vector b(1, 3, 0.0);
    for (const auto &q : p) {
        const double row[3] = {q[0], q[1], 1}, value = q[0] * q[0] + q[1] * q[1];
        for (int i = 0; i < 3; ++i) {
            b(i + 1) += row[i] * value;
            for (int j = 0; j < 3; ++j)
                a(i + 1, j + 1) += row[i] * row[j];
        }
    }
    math_Gauss solve(a);
    if (!solve.IsDone())
        return false;
    math_Vector x(1, 3);
    solve.Solve(b, x);
    cx = x(1) / 2;
    cy = x(2) / 2;
    r = std::sqrt(std::max(0.0, x(3) + cx * cx + cy * cy));
    return r > 0;
}

// One stretch of a flat's outline: a line (a point and direction) or a circle (centre and radius),
// with how far along it runs.
struct RimPiece {
    bool straight = true;
    std::array<double, 4> line{};
    std::array<double, 3> circle{};
    double from = 0, sweep = 0;
    std::vector<std::array<double, 2>> points;

    bool holds(double u, double v, double margin) const {
        if (straight) {
            const double along = (u - line[0]) * line[2] + (v - line[1]) * line[3];
            return along >= -margin && along <= sweep + margin;
        }
        const double spare = margin / std::max(circle[2], margin) + 0.1, sign = sweep < 0 ? -1 : 1;
        const double turned = (std::atan2(v - circle[1], u - circle[0]) - from) * sign;
        const double centred = std::remainder(turned - std::abs(sweep) / 2, 2 * M_PI);
        return std::abs(centred) <= std::abs(sweep) / 2 + spare;
    }
};

// The edge of a flat is exact where the mesh's other points only sit near a fillet: walk it, and
// take lines and circles off it, longest first from each corner.
std::vector<RimPiece> rimPieces(const Facets &f, const std::vector<int> &part, const std::vector<int> &labels, int label, const V &x, const V &y, double size) {
    const Mesh &m = f.mesh;
    std::map<int, std::vector<int>> linked;
    for (const int t : part) {
        const auto &tri = m.triangles[std::size_t(t)];
        for (int k = 0; k < 3; ++k) {
            const int other = f.across[std::size_t(t)][std::size_t(k)];
            if (other < 0 || labels[std::size_t(other)] != label)
                continue;
            const int a = tri[std::size_t(k)], b = tri[std::size_t((k + 1) % 3)];
            linked[a].push_back(b);
            linked[b].push_back(a);
        }
    }
    const auto at = [&](int p) { return std::array<double, 2>{pointOf(m, p).Dot(x), pointOf(m, p).Dot(y)}; };
    const auto turnAt = [&](const std::array<double, 2> &a, const std::array<double, 2> &b, const std::array<double, 2> &c) {
        return std::abs(std::remainder(std::atan2(c[1] - b[1], c[0] - b[0]) - std::atan2(b[1] - a[1], b[0] - a[0]), 2 * M_PI));
    };
    std::vector<std::vector<int>> chains;
    std::set<int> walked;
    const auto walk = [&](int start) {
        std::vector<int> chain{start};
        walked.insert(start);
        for (int here = start;;) {
            int next = -1;
            for (const int other : linked[here])
                if (!walked.count(other)) {
                    next = other;
                    break;
                }
            if (next < 0)
                break;
            walked.insert(next);
            chain.push_back(next);
            if (linked[next].size() != 2)
                break;
            here = next;
        }
        if (chain.size() >= 2)
            chains.push_back(std::move(chain));
    };
    for (const auto &[p, list] : linked)
        if (list.size() != 2 && !walked.count(p))
            walk(p);
    for (const auto &[p, list] : linked)
        if (!walked.count(p))
            walk(p);

    constexpr double kExact = 0.004;
    std::vector<RimPiece> pieces;
    for (std::vector<int> chain : chains) {
        // A closed loop starts at its sharpest corner, so no arc is cut in two.
        const auto &ends = linked[chain.back()];
        if (chain.size() > 3 && std::find(ends.begin(), ends.end(), chain.front()) != ends.end()) {
            std::size_t sharpest = 0;
            double most = -1;
            for (std::size_t i = 0; i < chain.size(); ++i) {
                const double turn = turnAt(at(chain[(i + chain.size() - 1) % chain.size()]), at(chain[i]), at(chain[(i + 1) % chain.size()]));
                if (turn > most) {
                    most = turn;
                    sharpest = i;
                }
            }
            std::rotate(chain.begin(), chain.begin() + long(sharpest), chain.end());
            chain.push_back(chain.front());
        }
        std::vector<std::array<double, 2>> rim;
        for (const int p : chain)
            rim.push_back(at(p));
        for (std::size_t i = 0; i + 1 < rim.size();) {
            std::size_t lineEnd = i + 1, circleEnd = i;
            for (std::size_t j = i + 2; j < rim.size(); ++j) {
                const double dx = rim[j][0] - rim[i][0], dy = rim[j][1] - rim[i][1], length = std::max(std::hypot(dx, dy), 1e-9);
                bool on = true;
                for (std::size_t k = i + 1; k < j; ++k)
                    on = on && std::abs((rim[k][0] - rim[i][0]) * dy - (rim[k][1] - rim[i][1]) * dx) / length <= kExact;
                if (!on)
                    break;
                lineEnd = j;
            }
            double cx = 0, cy = 0, r = 0;
            for (std::size_t j = i + 3; j < rim.size(); ++j) {
                std::vector<std::array<double, 2>> some(rim.begin() + long(i), rim.begin() + long(j) + 1);
                double u, v, radius;
                if (!circleThrough(some, u, v, radius) || radius > size * 0.6)
                    break;
                bool on = true;
                for (const auto &q : some)
                    on = on && std::abs(std::hypot(q[0] - u, q[1] - v) - radius) <= kExact;
                if (!on)
                    break;
                circleEnd = j;
                cx = u;
                cy = v;
                r = radius;
            }
            RimPiece piece;
            std::size_t end = lineEnd;
            if (circleEnd > lineEnd) {
                end = circleEnd;
                piece.straight = false;
                piece.circle = {cx, cy, r};
                piece.from = std::atan2(rim[i][1] - cy, rim[i][0] - cx);
                for (std::size_t k = i; k < end; ++k)
                    piece.sweep += std::remainder(std::atan2(rim[k + 1][1] - cy, rim[k + 1][0] - cx) - std::atan2(rim[k][1] - cy, rim[k][0] - cx), 2 * M_PI);
            } else {
                const double dx = rim[end][0] - rim[i][0], dy = rim[end][1] - rim[i][1], length = std::max(std::hypot(dx, dy), 1e-9);
                piece.line = {rim[i][0], rim[i][1], dx / length, dy / length};
                piece.sweep = length;
            }
            piece.points.assign(rim.begin() + long(i), rim.begin() + long(end) + 1);
            pieces.push_back(std::move(piece));
            i = end;
        }
    }
    // An outline can break mid-arc where another surface's corner lands on it, and a stub of the
    // arc reads as a line. A piece that joins a circle and lies on it is that circle.
    const auto same = [](const std::array<double, 2> &a, const std::array<double, 2> &b) { return std::hypot(a[0] - b[0], a[1] - b[1]) < 1e-6; };
    for (bool joined = true; joined;) {
        joined = false;
        for (std::size_t a = 0; a < pieces.size() && !joined; ++a) {
            if (pieces[a].straight)
                continue;
            for (std::size_t b = 0; b < pieces.size() && !joined; ++b) {
                if (a == b)
                    continue;
                std::vector<std::array<double, 2>> &here = pieces[a].points;
                std::vector<std::array<double, 2>> there = pieces[b].points;
                if (same(here.front(), there.front()) || same(here.back(), there.back()))
                    std::reverse(there.begin(), there.end());
                const bool after = same(here.back(), there.front()), before = same(here.front(), there.back());
                if (!after && !before)
                    continue;
                const auto &c = pieces[a].circle;
                bool on = true;
                for (const auto &q : there)
                    on = on && std::abs(std::hypot(q[0] - c[0], q[1] - c[1]) - c[2]) <= kExact * 2;
                if (!on)
                    continue;
                if (after)
                    here.insert(here.end(), there.begin() + 1, there.end());
                else
                    here.insert(here.begin(), there.begin(), there.end() - 1);
                double cx, cy, r;
                if (circleThrough(here, cx, cy, r))
                    pieces[a].circle = {cx, cy, r};
                const auto &n = pieces[a].circle;
                pieces[a].from = std::atan2(here[0][1] - n[1], here[0][0] - n[0]);
                pieces[a].sweep = 0;
                for (std::size_t k = 0; k + 1 < here.size(); ++k)
                    pieces[a].sweep += std::remainder(std::atan2(here[k + 1][1] - n[1], here[k + 1][0] - n[0]) - std::atan2(here[k][1] - n[1], here[k][0] - n[0]), 2 * M_PI);
                pieces.erase(pieces.begin() + long(b));
                joined = true;
            }
        }
    }
    std::sort(pieces.begin(), pieces.end(), [](const RimPiece &a, const RimPiece &b) { return a.points.size() > b.points.size(); });
    return pieces;
}

// A rounded edge beside a flat is a ball rolled along it: every point of it is one radius from a
// line, so height against how far the surface has turned from the flat is straight, and its slope
// is that radius. Move each triangle in by it and the line itself shows: straight gives a
// cylinder, a circle gives a torus. A rounded outline is a chain of both, so they're taken off it
// piece by piece, the piece holding the most triangles first.
void filletSurfaces(const Facets &f, const std::vector<int> &labels, const std::vector<Surface> &flats, const std::vector<int> &owner,
                    double size, double tol, std::vector<Surface> &out, std::vector<std::vector<int>> &seeds, std::vector<V> &beside,
                    const std::vector<char> &soft, std::vector<char> &onBigFlat) {
    const Mesh &m = f.mesh;
    std::vector<double> flatArea(flats.size(), 0);
    for (std::size_t t = 0; t < labels.size(); ++t)
        if (labels[t] >= 0)
            flatArea[std::size_t(labels[t])] += f.area[t];
    const double bigFlat = size * size * 2e-3, crease = std::cos(40 * M_PI / 180);
    onBigFlat.assign(labels.size(), 0);
    for (std::size_t t = 0; t < labels.size(); ++t)
        onBigFlat[t] = labels[t] >= 0 && flatArea[std::size_t(labels[t])] >= bigFlat;
    const auto loose = [&](int t) { return (owner[std::size_t(t)] < 0 || soft[std::size_t(t)]) && !onBigFlat[std::size_t(t)]; };
    const auto middleOf = [&](int t) {
        const auto &tri = m.triangles[std::size_t(t)];
        return (pointOf(m, tri[0]) + pointOf(m, tri[1]) + pointOf(m, tri[2])) / 3;
    };

    std::vector<int> stamp(labels.size(), -1);
    for (std::size_t label = 0; label < flats.size(); ++label) {
        if (flatArea[label] < bigFlat)
            continue;
        V facing(0, 0, 0);
        for (std::size_t t = 0; t < labels.size(); ++t)
            if (labels[t] == int(label))
                facing += f.normal[t] * f.area[t];
        const V a = facing / std::max(facing.Modulus(), 1e-12);
        const double level = flats[label].origin.Dot(a);
        const gp_Ax3 frame(gp_Pnt(0, 0, 0), gp_Dir(a));
        const V x = frame.XDirection().XYZ(), y = frame.YDirection().XYZ();
        for (std::size_t edge = 0; edge < labels.size(); ++edge) {
            if (labels[edge] != int(label))
                continue;
            for (const int start : f.across[edge]) {
                if (start < 0 || !loose(start) || stamp[std::size_t(start)] == int(label))
                    continue;
                std::vector<int> part{start};
                stamp[std::size_t(start)] = int(label);
                for (std::size_t i = 0; i < part.size(); ++i)
                    for (const int other : f.across[std::size_t(part[i])])
                        if (other >= 0 && loose(other) && stamp[std::size_t(other)] != int(label) && f.normal[std::size_t(other)].Dot(a) > -0.05
                            && f.normal[std::size_t(other)].Dot(f.normal[std::size_t(part[i])]) > crease) {
                            stamp[std::size_t(other)] = int(label);
                            part.push_back(other);
                        }
                if (part.size() < 6)
                    continue;
                double sw = 0, sx = 0, sy = 0, sxx = 0, sxy = 0;
                for (const int t : part) {
                    const double w = f.area[std::size_t(t)], turned = f.normal[std::size_t(t)].Dot(a), h = middleOf(t).Dot(a);
                    sw += w;
                    sx += w * turned;
                    sy += w * h;
                    sxx += w * turned * turned;
                    sxy += w * turned * h;
                }
                const double spread = sw * sxx - sx * sx;
                if (spread <= sw * sw * 1e-3)
                    continue;
                const double round = (sw * sxy - sx * sy) / spread;
                if (std::abs(round) < 0.05 || std::abs(round) > size * 0.2)
                    continue;
                std::vector<std::array<double, 2>> spine;
                for (const int t : part) {
                    const V c = middleOf(t) - f.normal[std::size_t(t)] * round;
                    spine.push_back({c.Dot(x), c.Dot(y)});
                }
                const double close = std::max(0.06, std::abs(round) * 0.05);
                std::vector<char> used(part.size(), 0);
                const std::vector<RimPiece> planned = rimPieces(f, part, labels, int(label), x, y, size);
                for (int piece = 0; piece < int(planned.size()) + 32; ++piece) {
                    const bool fromRim = piece < int(planned.size());
                    std::vector<std::size_t> open;
                    for (std::size_t i = 0; i < part.size(); ++i)
                        if (!used[i])
                            open.push_back(i);
                    if (open.size() < 4)
                        break;
                    const std::size_t step = std::max<std::size_t>(1, open.size() / 24);
                    std::size_t bestLine = 0, bestRound = 0;
                    std::array<double, 4> line{};
                    std::array<double, 3> circle{};
                    for (std::size_t i = 0; !fromRim && i < open.size(); i += step)
                        for (std::size_t j = i + step; j < open.size(); j += step) {
                            const auto &p = spine[open[i]], &q = spine[open[j]];
                            const double length = std::hypot(q[0] - p[0], q[1] - p[1]);
                            if (length < close * 2)
                                continue;
                            const double dx = (q[0] - p[0]) / length, dy = (q[1] - p[1]) / length;
                            std::size_t on = 0;
                            double low = INFINITY, high = -INFINITY;
                            for (const std::size_t k : open) {
                                const double u = spine[k][0] - p[0], v = spine[k][1] - p[1];
                                if (std::abs(u * dy - v * dx) <= close) {
                                    ++on;
                                    low = std::min(low, u * dx + v * dy);
                                    high = std::max(high, u * dx + v * dy);
                                }
                            }
                            if (on > bestLine && high - low >= std::abs(round) * 0.4) {
                                bestLine = on;
                                line = {p[0], p[1], dx, dy};
                            }
                            for (std::size_t l = j + step; l < open.size(); l += step) {
                                const auto &c = spine[open[l]];
                                const double d = 2 * (p[0] * (q[1] - c[1]) + q[0] * (c[1] - p[1]) + c[0] * (p[1] - q[1]));
                                if (std::abs(d) < 1e-9)
                                    continue;
                                const double aa = p[0] * p[0] + p[1] * p[1], bb = q[0] * q[0] + q[1] * q[1], cc = c[0] * c[0] + c[1] * c[1];
                                const double u = (aa * (q[1] - c[1]) + bb * (c[1] - p[1]) + cc * (p[1] - q[1])) / d;
                                const double v = (aa * (c[0] - q[0]) + bb * (p[0] - c[0]) + cc * (q[0] - p[0])) / d;
                                const double r = std::hypot(p[0] - u, p[1] - v);
                                if (r > size * 0.6)
                                    continue;
                                std::size_t around = 0;
                                for (const std::size_t k : open)
                                    around += std::abs(std::hypot(spine[k][0] - u, spine[k][1] - v) - r) <= close;
                                if (around > bestRound) {
                                    bestRound = around;
                                    circle = {u, v, r};
                                }
                            }
                        }
                    // A big circle passes for a line: the circle must hold clearly more.
                    bool straight = bestLine >= 4 && double(bestLine) * 1.15 >= double(bestRound);
                    if (fromRim) {
                        straight = planned[std::size_t(piece)].straight;
                        line = planned[std::size_t(piece)].line;
                        circle = planned[std::size_t(piece)].circle;
                    } else if (!straight && bestRound < 5)
                        break;
                    std::vector<int> on;
                    std::set<int> points;
                    for (const std::size_t k : open) {
                        const double u = spine[k][0], v = spine[k][1];
                        const bool hit = straight ? std::abs((u - line[0]) * line[3] - (v - line[1]) * line[2]) <= close
                                                  : std::abs(std::hypot(u - circle[0], v - circle[1]) - circle[2]) <= close;
                        if (!hit || (fromRim && !planned[std::size_t(piece)].holds(u, v, close)))
                            continue;
                        used[k] = 1;
                        on.push_back(part[k]);
                        for (const int p : m.triangles[std::size_t(part[k])])
                            points.insert(p);
                    }
                    // The picked points only got it close: settle the line or circle on all it took.
                    std::vector<std::array<double, 2>> taken;
                    for (const int t : on) {
                        const V c = middleOf(t) - f.normal[std::size_t(t)] * round;
                        taken.push_back({c.Dot(x), c.Dot(y)});
                    }
                    // Better, where the fillet's edge shows: those points are exact, and straight
                    // over the centre line.
                    std::vector<std::array<double, 2>> rim;
                    for (const int t : on)
                        for (const int other : f.across[std::size_t(t)])
                            if (other >= 0 && labels[std::size_t(other)] == int(label))
                                for (const int p : m.triangles[std::size_t(t)])
                                    if (std::abs(pointOf(m, p).Dot(a) - level) < 1e-3 && std::find_if(rim.begin(), rim.end(), [&](const auto &q) {
                                            return std::hypot(q[0] - pointOf(m, p).Dot(x), q[1] - pointOf(m, p).Dot(y)) < 1e-6;
                                        }) == rim.end())
                                        rim.push_back({pointOf(m, p).Dot(x), pointOf(m, p).Dot(y)});
                    if (fromRim)
                        rim = planned[std::size_t(piece)].points;
                    const bool edgeShows = rim.size() >= (straight ? 2u : 4u);
                    if (edgeShows)
                        taken = rim;
                    if (straight) {
                        double mx = 0, my = 0, xx = 0, xy = 0, yy = 0;
                        for (const auto &q : taken) {
                            mx += q[0] / double(taken.size());
                            my += q[1] / double(taken.size());
                        }
                        for (const auto &q : taken) {
                            xx += (q[0] - mx) * (q[0] - mx);
                            xy += (q[0] - mx) * (q[1] - my);
                            yy += (q[1] - my) * (q[1] - my);
                        }
                        const double angle = std::atan2(2 * xy, xx - yy) / 2;
                        line = {mx, my, std::cos(angle), std::sin(angle)};
                    } else {
                        double cx, cy, cr;
                        if (circleThrough(taken, cx, cy, cr) && cr <= size * 0.6)
                            circle = {cx, cy, cr};
                    }
                    const V dir = x * line[2] + y * line[3], side = a.Crossed(dir);
                    std::vector<std::array<double, 2>> across;
                    for (const int p : points) {
                        const V at = pointOf(m, p);
                        across.push_back({straight ? at.Dot(side) : std::hypot(at.Dot(x) - circle[0], at.Dot(y) - circle[1]), at.Dot(a)});
                    }
                    // A fillet leaves its flat smoothly, so its centre sits one radius off that flat,
                    // leaving only where it lies across and its radius. Points of the walls beside
                    // it come along; they're off the round, so they're dropped as it settles.
                    const double sign = round > 0 ? 1 : -1;
                    double height = level, along = straight ? (x * line[0] + y * line[1]).Dot(side) : circle[2], r = std::abs(round);
                    const std::size_t all = across.size();
                    if (edgeShows) {
                        std::vector<double> radii;
                        for (const auto &q : across) {
                            const double ds = q[0] - along, dh = q[1] - height;
                            if (std::abs(dh) > std::abs(round) * 0.05)
                                radii.push_back(-(ds * ds + dh * dh) / (2 * sign * dh));
                        }
                        if (radii.size() < 3)
                            continue;
                        std::nth_element(radii.begin(), radii.begin() + long(radii.size() / 2), radii.end());
                        r = radii[radii.size() / 2];
                        std::vector<std::array<double, 2>> kept;
                        for (const auto &q : across)
                            if (std::abs(std::hypot(q[0] - along, q[1] - height + sign * r) - r) <= tol * 1.5)
                                kept.push_back(q);
                        across = std::move(kept);
                    }
                    for (int pass = 0; !edgeShows && pass < 12 && across.size() >= 3; ++pass) {
                        double aa = 0, ab = 0, bb = 0, ae = 0, be = 0;
                        for (const auto &q : across) {
                            const double ds = q[0] - along, dh = q[1] - height + sign * r, length = std::max(std::hypot(ds, dh), 1e-9);
                            const double e = length - r, da = -ds / length, db = sign * dh / length - 1;
                            aa += da * da;
                            ab += da * db;
                            bb += db * db;
                            ae += da * e;
                            be += db * e;
                        }
                        const double det = aa * bb - ab * ab;
                        if (std::abs(det) < 1e-12)
                            break;
                        along -= (bb * ae - ab * be) / det;
                        r -= (aa * be - ab * ae) / det;
                        const double allowed = std::max(tol * 1.5, 0.4 * std::abs(round) * std::pow(0.7, pass));
                        std::vector<std::array<double, 2>> kept;
                        for (const auto &q : across)
                            if (std::abs(std::hypot(q[0] - along, q[1] - height + sign * r) - r) <= allowed)
                                kept.push_back(q);
                        across = std::move(kept);
                    }
                    if (across.size() < 3 || across.size() * 3 < all || std::abs(r - std::abs(round)) > std::abs(round) * 0.25)
                        continue;
                    height -= sign * r;
                    Surface s;
                    if (straight) {
                        s.kind = Surface::Cylinder;
                        s.origin = side * along + a * height;
                        s.axis = dir;
                        s.radius = r;
                    } else {
                        s.kind = Surface::Torus;
                        s.origin = x * circle[0] + y * circle[1] + a * height;
                        s.axis = a;
                        s.radius = along;
                        s.tube = r;
                        double sideways = 0, upwards = 0;
                        for (const auto &q : across) {
                            sideways += q[0] - along;
                            upwards += q[1] - height;
                        }
                        s.facing = std::atan2(upwards, sideways);
                    }
                    double worst = 0;
                    for (const auto &q : across)
                        worst = std::max(worst, std::abs(std::hypot(q[0] - along, q[1] - height) - r));
                    if (worst > tol * 3)
                        continue;
                    out.push_back(s);
                    seeds.push_back(std::move(on));
                    beside.push_back(a);
                }
            }
        }
    }
}

// A printed thread's triangles run from flank to flank right across its root, so none of them
// belongs to one face of the thread. Cut them along the helices where the faces meet: unrolled
// around the axis those are straight parallel lines, one edge at a time, shared by both neighbours.
Mesh cutAlongThread(const Mesh &in, const std::vector<Surface> &faces, double tol) {
    const Surface &s = faces.front();
    const gp_Ax3 frame(gp_Pnt(s.origin), gp_Dir(s.axis));
    const V x = frame.XDirection().XYZ(), y = frame.YDirection().XYZ();
    const double turn = std::abs(s.pitch), onLine = 2e-3;
    std::vector<std::array<double, 2>> lines;
    for (const Surface &face : faces)
        lines.push_back(face.outline.front());

    const auto spinOf = [&](const V &p) {
        const V d = p - s.origin;
        return std::atan2(d.Dot(y), d.Dot(x));
    };
    // How far b is across the thread from a, going the short way round.
    const auto apart = [&](const V &a, const V &b) {
        const double spin = std::remainder(spinOf(b) - spinOf(a), 2 * M_PI);
        return (b - a).Dot(s.axis) - s.pitch * spin / (2 * M_PI);
    };
    const auto lineAt = [&](double w) {
        for (std::size_t k = 0; k < lines.size(); ++k)
            if (std::abs(std::remainder(w - lines[k][0], turn)) < onLine)
                return int(k);
        return -1;
    };

    Mesh out = in;
    std::vector<char> on(in.points.size());
    // A screw's surface runs on forever: well past the thread, a point can lie on it by chance.
    for (std::size_t p = 0; p < on.size(); ++p)
        for (const Surface &face : faces) {
            const double height = (pointOf(in, int(p)) - face.origin).Dot(face.axis);
            on[p] = on[p] || (height >= face.low - turn && height <= face.high + turn && distanceTo(face, pointOf(in, int(p))) <= tol);
        }

    std::map<Key, std::vector<std::pair<double, int>>> cuts;
    const auto cutsOf = [&](int a, int b) -> const std::vector<std::pair<double, int>> & {
        const Key key = sideKey(a, b);
        const auto found = cuts.find(key);
        if (found != cuts.end())
            return found->second;
        auto &list = cuts[key];
        if (!on[std::size_t(key.first)] || !on[std::size_t(key.second)])
            return list;
        const V from = pointOf(in, key.first), to = pointOf(in, key.second);
        double w, rho;
        onScrew(s, from, w, rho);
        const double span = apart(from, to), spin = std::remainder(spinOf(to) - spinOf(from), 2 * M_PI);
        if (std::abs(span) <= onLine)
            return list;
        for (const auto &line : lines)
            for (double target = w + std::remainder(line[0] - w, turn) - turn; target <= w + std::abs(span) + turn; target += turn) {
                const double along = (target - w) / span;
                if (along * std::abs(span) <= onLine || (1 - along) * std::abs(span) <= onLine)
                    continue;
                const double angle = spinOf(from) + spin * along;
                const V d = s.axis * ((from - s.origin).Dot(s.axis) + (to - from).Dot(s.axis) * along) + x * (line[1] * std::cos(angle)) + y * (line[1] * std::sin(angle));
                const V at = s.origin + d;
                out.points.push_back({at.X(), at.Y(), at.Z()});
                list.push_back({along, int(out.points.size()) - 1});
            }
        std::sort(list.begin(), list.end());
        return list;
    };

    const auto cornerOf = [&](int p) {
        double w, rho;
        onScrew(s, pointOf(out, p), w, rho);
        return lineAt(w);
    };
    std::vector<std::array<int, 3>> triangles;
    std::vector<std::vector<int>> pending;
    for (const auto &tri : in.triangles) {
        std::vector<int> ring;
        for (int k = 0; k < 3; ++k) {
            const int a = tri[std::size_t(k)], b = tri[std::size_t((k + 1) % 3)];
            ring.push_back(a);
            auto list = cutsOf(a, b);
            if (a > b)
                std::reverse(list.begin(), list.end());
            for (const auto &cut : list)
                ring.push_back(cut.second);
        }
        if (ring.size() == 3) {
            triangles.push_back(tri);
            continue;
        }
        // Part it along each line that goes in one side and out another.
        pending = {ring};
        while (!pending.empty()) {
            const std::vector<int> part = pending.back();
            pending.pop_back();
            const std::size_t n = part.size();
            bool split = false;
            for (std::size_t i = 0; i < n && !split; ++i) {
                const int line = cornerOf(part[i]);
                if (line < 0)
                    continue;
                for (std::size_t j = i + 2; j < n && !split; ++j) {
                    if ((j + 1) % n == i || cornerOf(part[j]) != line || std::abs(apart(pointOf(out, part[i]), pointOf(out, part[j]))) > onLine * 2)
                        continue;
                    pending.emplace_back(part.begin() + std::ptrdiff_t(i), part.begin() + std::ptrdiff_t(j) + 1);
                    std::vector<int> rest(part.begin() + std::ptrdiff_t(j), part.end());
                    rest.insert(rest.end(), part.begin(), part.begin() + std::ptrdiff_t(i) + 1);
                    pending.push_back(std::move(rest));
                    split = true;
                }
            }
            if (split)
                continue;
            // What's left has no line through it: fan it from the corner that makes no slivers.
            std::size_t apex = 0;
            double best = -1;
            for (std::size_t i = 0; i < n; ++i) {
                double smallest = INFINITY;
                for (std::size_t k = 1; k + 1 < n; ++k)
                    smallest = std::min(smallest, (pointOf(out, part[(i + k) % n]) - pointOf(out, part[i])).Crossed(pointOf(out, part[(i + k + 1) % n]) - pointOf(out, part[i])).Modulus());
                if (smallest > best) {
                    best = smallest;
                    apex = i;
                }
            }
            for (std::size_t k = 1; k + 1 < n; ++k)
                triangles.push_back({part[apex], part[(apex + k) % n], part[(apex + k + 1) % n]});
        }
    }
    out.triangles = std::move(triangles);
    return out;
}

void sectionSurfaces(const Facets &f, double size, double tol, std::vector<Surface> &out, std::vector<std::vector<int>> &seeds) {
    const Mesh &m = f.mesh;
    V middle(0, 0, 0);
    for (std::size_t i = 0; i < m.points.size(); ++i)
        middle += pointOf(m, int(i));
    middle /= double(m.points.size());
    math_Matrix cov(1, 3, 1, 3, 0.0);
    for (std::size_t i = 0; i < m.points.size(); ++i) {
        const V d = pointOf(m, int(i)) - middle;
        for (int r = 1; r <= 3; ++r)
            for (int c = 1; c <= 3; ++c)
                cov(r, c) += d.Coord(r) * d.Coord(c);
    }
    math_Jacobi jacobi(cov);
    int largest = 1;
    for (int i = 2; i <= 3; ++i)
        if (jacobi.Value(i) > jacobi.Value(largest))
            largest = i;
    math_Vector v(1, 3);
    jacobi.Vector(largest, v);
    const gp_Ax3 frame(gp_Pnt(0, 0, 0), gp_Dir(v(1), v(2), v(3)));
    const V axis = frame.Direction().XYZ(), uDir = frame.XDirection().XYZ(), vDir = frame.YDirection().XYZ();

    std::vector<double> along(m.points.size());
    double lo = INFINITY, hi = -INFINITY;
    for (std::size_t i = 0; i < m.points.size(); ++i) {
        along[i] = pointOf(m, int(i)).Dot(axis);
        lo = std::min(lo, along[i]);
        hi = std::max(hi, along[i]);
    }
    constexpr int kStations = 60;
    const double gap = (hi - lo) / kStations;
    std::vector<Arc> arcs;
    std::vector<Ring> rings;
    std::vector<char> seen(m.triangles.size());
    std::vector<std::array<double, 2>> ring;
    std::vector<int> tris;
    for (int station = 0; station < kStations; ++station) {
        const double h = lo + (station + 0.5) * gap + 1.37e-7;
        std::fill(seen.begin(), seen.end(), 0);
        const auto crossing = [&](std::size_t t, int k) {
            return (along[std::size_t(m.triangles[t][std::size_t(k)])] > h) != (along[std::size_t(m.triangles[t][std::size_t((k + 1) % 3)])] > h);
        };
        for (std::size_t first = 0; first < m.triangles.size(); ++first) {
            if (seen[first] || !(crossing(first, 0) || crossing(first, 1) || crossing(first, 2)))
                continue;
            ring.clear();
            tris.clear();
            std::size_t t = first;
            int from = -1;
            while (!seen[t]) {
                seen[t] = 1;
                int side = -1;
                for (int k = 0; k < 3; ++k)
                    if (crossing(t, k) && k != from)
                        side = k;
                if (side < 0)
                    break;
                const int a = m.triangles[t][std::size_t(side)], b = m.triangles[t][std::size_t((side + 1) % 3)];
                const double share = (h - along[std::size_t(a)]) / (along[std::size_t(b)] - along[std::size_t(a)]);
                const V at = pointOf(m, a) + (pointOf(m, b) - pointOf(m, a)) * share;
                ring.push_back({at.Dot(uDir), at.Dot(vDir)});
                tris.push_back(int(t));
                const int next = f.across[t][std::size_t(side)];
                if (next < 0)
                    break;
                from = -1;
                for (int k = 0; k < 3; ++k)
                    if (f.across[std::size_t(next)][std::size_t(k)] == int(t))
                        from = k;
                t = std::size_t(next);
            }
            findArcs(ring, tris, h, tol, size, arcs);
            findBrokenCircles(ring, tris, h, tol, size, arcs);
            double cu, cv, r, worst;
            if (ring.size() >= 12 && circleOf(ring, 0, ring.size() - 1, cu, cv, r, worst) && worst > tol && worst < r * 0.15) {
                // The middle by area: points bunch where the mesh is fine, and pull a mean away.
                double area = 0, mu = 0, mv = 0;
                for (std::size_t i = 0; i < ring.size(); ++i) {
                    const auto &p = ring[i], &q = ring[(i + 1) % ring.size()];
                    const double cross = p[0] * q[1] - q[0] * p[1];
                    area += cross;
                    mu += (p[0] + q[0]) * cross;
                    mv += (p[1] + q[1]) * cross;
                }
                if (std::abs(area) > 1e-9) {
                    cu = mu / (3 * area);
                    cv = mv / (3 * area);
                }
                double low = INFINITY, high = 0;
                for (const auto &p : ring) {
                    low = std::min(low, std::hypot(p[0] - cu, p[1] - cv));
                    high = std::max(high, std::hypot(p[0] - cu, p[1] - cv));
                }
                rings.push_back({h, cu, cv, low, high});
            }
        }
    }

    // Arcs with the same circle, slice after slice, are one surface.
    struct Run { std::vector<int> arcs; double h, cu, cv, r; };
    std::vector<Run> runs;
    for (std::size_t i = 0; i < arcs.size(); ++i) {
        const Arc &a = arcs[i];
        int best = -1;
        double closest = INFINITY;
        for (std::size_t j = 0; j < runs.size(); ++j) {
            const Run &run = runs[j];
            const double off = std::hypot(run.cu - a.cu, run.cv - a.cv), grew = std::abs(run.r - a.r);
            if (a.h - run.h > 3.5 * gap || off > std::max(0.3, a.r * 0.1) || grew > std::max(0.2, a.r * 0.1))
                continue;
            if (off + grew < closest) {
                closest = off + grew;
                best = int(j);
            }
        }
        if (best < 0) {
            runs.push_back({{int(i)}, a.h, a.cu, a.cv, a.r});
        } else {
            Run &run = runs[std::size_t(best)];
            run.arcs.push_back(int(i));
            run.h = a.h; run.cu = a.cu; run.cv = a.cv; run.r = a.r;
        }
    }
    std::sort(runs.begin(), runs.end(), [](const Run &a, const Run &b) { return a.arcs.size() > b.arcs.size(); });

    for (const Run &run : runs) {
        std::set<double> heights;
        std::vector<std::array<double, 3>> pts;
        std::vector<int> under;
        for (const int i : run.arcs) {
            const Arc &a = arcs[std::size_t(i)];
            heights.insert(a.h);
            pts.insert(pts.end(), a.points.begin(), a.points.end());
            under.insert(under.end(), a.triangles.begin(), a.triangles.end());
        }
        if (heights.size() < 3 || pts.size() < 12)
            continue;
        const double mid = (*heights.begin() + *heights.rbegin()) / 2;
        for (auto &p : pts)
            p[0] -= mid;
        const Arc &a = arcs[std::size_t(run.arcs.front())];
        double q[6] = {a.cu, 0, a.cv, 0, a.r, 0};
        if (!fitAlong(pts, q))
            continue;
        // Drop what isn't this surface and read it again.
        std::vector<std::array<double, 3>> kept;
        for (int pass = 0; pass < 3; ++pass) {
            kept.clear();
            for (const auto &p : pts)
                if (std::abs(std::hypot(p[1] - q[0] - q[1] * p[0], p[2] - q[2] - q[3] * p[0]) - q[4] - q[5] * p[0]) <= tol)
                    kept.push_back(p);
            if (kept.size() < 12 || !fitAlong(kept, q))
                break;
        }
        if (kept.size() < 12 || double(kept.size()) < 0.6 * double(pts.size()))
            continue;
        V direction = axis + uDir * q[1] + vDir * q[3];
        const double stretch = direction.Modulus();
        direction /= stretch;
        Surface s;
        s.axis = direction;
        s.origin = axis * mid + uDir * q[0] + vDir * q[2];
        s.radius = q[4];
        s.slope = q[5] / stretch;
        s.kind = std::abs(s.slope) * (hi - lo) < tol ? Surface::Cylinder : Surface::Cone;
        const auto holds = [&](const Surface &surface) {
            for (const auto &p : kept) {
                const V at = axis * (p[0] + mid) + uDir * p[1] + vDir * p[2];
                if (distanceTo(surface, at) > tol * 1.5)
                    return false;
            }
            return true;
        };
        bool merged = false;
        for (std::size_t i = 0; i < out.size() && !merged; ++i)
            if (holds(out[i])) {
                seeds[i].insert(seeds[i].end(), under.begin(), under.end());
                merged = true;
            }
        if (merged)
            continue;
        out.push_back(s);
        seeds.push_back(std::move(under));
    }

    // A screw has no arcs to seed it: anything on it will do.
    // The slices' direction is only near the part's axis; the longest surface found has it exactly.
    const gp_Ax3 exact(gp_Pnt(0, 0, 0), gp_Dir(out.empty() ? axis : out.front().axis));
    const V eu = exact.XDirection().XYZ(), ev = exact.YDirection().XYZ();
    const V at = out.empty() ? V(0, 0, 0) : out.front().origin;
    for (Ring &ring : rings) {
        const V middle2 = uDir * ring.cu + vDir * ring.cv + axis * ring.h;
        ring.cu = middle2.Dot(eu);
        ring.cv = middle2.Dot(ev);
    }
    screwSurfaces(m, exact.Direction().XYZ(), eu, ev, rings, at.Dot(eu), at.Dot(ev), tol, out);
    std::vector<int> every(m.triangles.size());
    std::iota(every.begin(), every.end(), 0);
    seeds.resize(out.size(), every);
}

} // namespace

Mesh read(const std::string &path, std::string &why) {
    Mesh out;
    std::string lower = path;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    if (lower.size() > 4 && lower.compare(lower.size() - 4, 4, ".obj") == 0) {
        std::ifstream in(path);
        if (!in) {
            why = "Couldn't read that file.";
            return out;
        }
        std::string line;
        while (std::getline(in, line)) {
            std::istringstream words(line);
            std::string tag;
            words >> tag;
            if (tag == "v") {
                std::array<double, 3> p{};
                words >> p[0] >> p[1] >> p[2];
                out.points.push_back(p);
            } else if (tag == "f") {
                std::vector<int> corners;
                std::string corner;
                while (words >> corner) {
                    const int i = std::atoi(corner.c_str());
                    corners.push_back(i < 0 ? int(out.points.size()) + i : i - 1);
                }
                for (std::size_t k = 1; k + 1 < corners.size(); ++k)
                    out.triangles.push_back({corners[0], corners[k], corners[k + 1]});
            }
        }
    } else {
        const Handle(Poly_Triangulation) tri = RWStl::ReadFile(path.c_str(), M_PI / 2.0);
        if (tri.IsNull()) {
            why = "That isn't a mesh file OmaMake can read.";
            return out;
        }
        out.points.reserve(std::size_t(tri->NbNodes()));
        for (int i = 1; i <= tri->NbNodes(); ++i) {
            const gp_Pnt p = tri->Node(i);
            out.points.push_back({p.X(), p.Y(), p.Z()});
        }
        for (int t = 1; t <= tri->NbTriangles(); ++t) {
            int a, b, c;
            tri->Triangle(t).Get(a, b, c);
            out.triangles.push_back({a - 1, b - 1, c - 1});
        }
    }
    out = weld(out, std::max(1e-5, reach(out) * 1e-8));
    orient(out);
    if (out.triangles.empty())
        why = "That file holds no triangles.";
    return out;
}

Mesh fromShape(const TopoDS_Shape &shape) {
    Mesh out;
    for (TopExp_Explorer it(shape, TopAbs_FACE); it.More(); it.Next()) {
        const TopoDS_Face face = TopoDS::Face(it.Current());
        TopLoc_Location loc;
        const Handle(Poly_Triangulation) tri = BRep_Tool::Triangulation(face, loc);
        if (tri.IsNull())
            continue;
        const int base = int(out.points.size());
        const gp_Trsf place = loc.Transformation();
        for (int i = 1; i <= tri->NbNodes(); ++i) {
            const gp_Pnt p = tri->Node(i).Transformed(place);
            out.points.push_back({p.X(), p.Y(), p.Z()});
        }
        const bool reversed = face.Orientation() == TopAbs_REVERSED;
        for (int t = 1; t <= tri->NbTriangles(); ++t) {
            int a, b, c;
            tri->Triangle(t).Get(a, b, c);
            if (reversed)
                std::swap(b, c);
            out.triangles.push_back({base + a - 1, base + b - 1, base + c - 1});
        }
    }
    return weld(out, std::max(1e-5, reach(out) * 1e-7));
}

TopoDS_Shape solid(const Mesh &mesh, const Stage &stage) {
    stage("Finding the flat areas");
    const Facets f(mesh);
    std::vector<int> labels;
    std::vector<Surface> surfaces;
    flatAreas(f, reach(mesh), labels, surfaces);
    return assemble(mesh, labels, surfaces, 1e-3, stage);
}

// One surface and the mesh points it was fitted to, for regularize().
struct Fitted { Surface *surface; std::vector<int> points; double area; };

// The surface read again with its direction held at `way`: a plane through the points' middle, a
// cylinder or cone by Gauss-Newton on its axis line, radius and (cone) slope. False if it won't settle.
bool refit(const Mesh &mesh, const std::vector<int> &points, const V &way, bool slope, Surface &s) {
    if (s.kind == Surface::Flat) {
        double sum = 0;
        for (const int p : points)
            sum += (pointOf(mesh, p) - s.origin).Dot(way);
        s.origin += way * (sum / double(points.size()));
        s.axis = way;
        return true;
    }
    if (s.kind != Surface::Cylinder && s.kind != Surface::Cone)
        return false;
    const gp_Ax3 frame(gp_Pnt(s.origin), gp_Dir(way));
    const V e1 = frame.XDirection().XYZ(), e2 = frame.YDirection().XYZ();
    double q[4] = {0, 0, s.radius, slope ? s.slope : 0};      // centre across, radius at origin, slope
    const int unknowns = slope ? 4 : 3;
    for (int pass = 0; pass < 20; ++pass) {
        math_Matrix a(1, unknowns, 1, unknowns, 0.0);
        math_Vector b(1, unknowns, 0.0);
        for (const int p : points) {
            const V d = pointOf(mesh, p) - s.origin;
            const double x = d.Dot(e1) - q[0], y = d.Dot(e2) - q[1], h = d.Dot(way), rho = std::max(std::hypot(x, y), 1e-12);
            const double r = rho - q[2] - q[3] * h;
            const double j[4] = {-x / rho, -y / rho, -1, -h};
            for (int i = 0; i < unknowns; ++i) {
                b(i + 1) -= j[i] * r;
                for (int k = 0; k < unknowns; ++k)
                    a(i + 1, k + 1) += j[i] * j[k];
            }
        }
        math_Gauss solve(a);
        if (!solve.IsDone())
            return false;
        math_Vector step(1, unknowns);
        solve.Solve(b, step);
        double moved = 0;
        for (int i = 0; i < unknowns; ++i) {
            q[i] += step(i + 1);
            moved = std::max(moved, std::abs(step(i + 1)));
        }
        if (moved < 1e-13)
            break;
    }
    s.origin += e1 * q[0] + e2 * q[1];
    s.axis = way;
    s.radius = q[2];
    s.slope = slope ? q[3] : 0;
    s.kind = slope ? Surface::Cone : Surface::Cylinder;
    return s.radius > 0;
}

// Fitted surfaces put back on the directions the part was drawn along, as CGAL's regularize_planes
// (Shape_regularization) does for planes after shape detection. A fit takes the mesh's float noise
// up as a tilt or a slope: the speaker mount's base walls, straight up in its STL (lean 0.000000°),
// came out as cylinders and cones leaning up to 0.06°, so nothing extruded beside them could ever be
// the same surface. Biggest first, each plane, cylinder or cone tries the world axes and the
// directions already settled, within a degree (a fillet ring is only turned upright), or else square
// to one of them, as CGAL also snaps orthogonality; a cone tries being a cylinder; and a round wall
// tries the axis line of a bigger one on the same direction. A try is kept only if every point of the
// surface still lies within `allowed` of it, as when it was first fitted.
// Returns how far any point ended up from a wall straightened by the short-wall rule (0 if none), so
// the build allows for it.
double regularize(const Mesh &mesh, std::vector<Fitted> &all, double allowed) {
    const auto holds = [&](const Surface &s, const std::vector<int> &points) {
        for (const int p : points)
            if (distanceTo(s, pointOf(mesh, p)) > allowed)
                return false;
        return true;
    };
    // A short wall under half a degree off is taken as meant upright (or square, or a plain
    // cylinder), even if its triangles lean: on a part that size the lean is the mesh, not the
    // design (owner, 2026-11-05: under 25 mm and 0.5 degrees, "make them solid and coplanar").
    // Taller walls keep a lean their points show, such as the speaker mount's 64 mm plug, drawn
    // with a 0.4 degree draft for a push fit.
    const double shortWall = 25, smallLean = std::sin(0.5 * M_PI / 180);
    const auto tall = [&](const std::vector<int> &points, const V &way) {
        double lo = 1e300, hi = -1e300;
        for (const int p : points) {
            const double h = pointOf(mesh, p).Dot(way);
            lo = std::min(lo, h);
            hi = std::max(hi, h);
        }
        return hi - lo;
    };
    // Along the wall: its axis for a round one, the steepest way across a flat one's points.
    const auto meant = [&](const Surface &s, const std::vector<int> &points, const V &way) {
        if (s.kind == Surface::Flat) {
            const V up = way - s.axis * way.Dot(s.axis);     // the way the flat tilts off `way`
            return tall(points, up.Modulus() > 1e-9 ? up / up.Modulus() : way) < shortWall;
        }
        return tall(points, way) < shortWall;
    };
    double forcedOff = 0;
    const auto worst = [&](const Surface &s, const std::vector<int> &points) {
        for (const int p : points)
            forcedOff = std::max(forcedOff, distanceTo(s, pointOf(mesh, p)));
        return true;
    };
    std::sort(all.begin(), all.end(), [](const Fitted &a, const Fitted &b) { return a.area > b.area; });
    // A surface that meets a thread stays as fitted: the mesh was cut along the thread against it,
    // so moved even a hair it no longer meets the thread's edge (the speaker mount's 45° lead-in cone
    // at the bore, turned upright, left the solid open with a 15 mm gap at its seam).
    std::set<int> threaded;
    for (const Fitted &one : all)
        if (one.surface->kind == Surface::Screw)
            threaded.insert(one.points.begin(), one.points.end());
    std::set<const Surface *> keep;
    for (const Fitted &one : all) {
        int shared = 0;
        for (const int p : one.points)
            shared += threaded.count(p) ? 1 : 0;
        if (one.surface->kind != Surface::Screw && shared >= 2)
            keep.insert(one.surface);
    }
    std::vector<V> ways = {V(1, 0, 0), V(0, 1, 0), V(0, 0, 1)};
    const double near = std::cos(1.0 * M_PI / 180);
    std::vector<const Surface *> rounds;
    for (Fitted &one : all) {
        Surface &s = *one.surface;
        const bool round = s.kind == Surface::Cylinder || s.kind == Surface::Cone;
        if ((s.kind != Surface::Flat && s.kind != Surface::Torus && !round) || one.points.size() < 3 || keep.count(&s))
            continue;
        bool settled = false;
        for (const V &w : ways) {
            const double c = s.axis.Dot(w);
            if (std::abs(c) < near)
                continue;
            const V way = c < 0 ? w * -1 : w;
            if (std::abs(c) >= 1 - 1e-15) {
                s.axis = way;
                settled = true;
                break;
            }
            Surface tried = s;
            if (s.kind == Surface::Torus)       // a fillet ring: only turned upright, about its centre
                tried.axis = way;
            const bool forced = std::sqrt(std::max(0.0, 1 - c * c)) < smallLean && s.kind != Surface::Torus && meant(s, one.points, way);
            if ((s.kind == Surface::Torus || refit(mesh, one.points, way, s.kind == Surface::Cone && !(forced && std::abs(s.slope) < smallLean), tried))
                && (holds(tried, one.points) || (forced && worst(tried, one.points)))) {
                s = tried;
                settled = true;
                break;
            }
        }
        // Square to a settled direction instead (a groove's side wall standing on the base).
        const double square = std::sin(1.0 * M_PI / 180);
        for (std::size_t k = 0; k < ways.size() && !settled && s.kind != Surface::Torus; ++k) {
            const double c = s.axis.Dot(ways[k]);
            if (std::abs(c) > square)
                continue;
            if (c == 0) {
                settled = true;
                break;
            }
            V way = s.axis - ways[k] * c;
            way /= way.Modulus();
            Surface tried = s;
            const bool forced = std::abs(c) < smallLean && meant(s, one.points, way);
            if (refit(mesh, one.points, way, s.kind == Surface::Cone && !(forced && std::abs(s.slope) < smallLean), tried) && (holds(tried, one.points) || (forced && worst(tried, one.points)))) {
                s = tried;
                settled = true;
            }
        }
        if (!settled || std::none_of(ways.begin(), ways.end(), [&](const V &w) { return std::abs(std::abs(w.Dot(s.axis)) - 1) < 1e-15; }))
            ways.push_back(s.axis);
        if (s.kind == Surface::Cone) {
            Surface tried = s;
            const bool forced = std::abs(s.slope) < smallLean && tall(one.points, s.axis) < shortWall;
            if (refit(mesh, one.points, s.axis, false, tried) && (holds(tried, one.points) || (forced && worst(tried, one.points))))
                s = tried;
        }
        if (!round)
            continue;
        // On a bigger round wall's axis line, if it fits there.
        for (const Surface *big : rounds) {
            if (std::abs(big->axis.Dot(s.axis)) < 1 - 1e-15)
                continue;
            const V d = s.origin - big->origin;
            if ((d - big->axis * d.Dot(big->axis)).Modulus() > allowed || (d - big->axis * d.Dot(big->axis)).Modulus() == 0)
                continue;
            Surface tried = s;
            tried.origin = big->origin + big->axis * d.Dot(big->axis);
            tried.axis = big->axis;
            // Radius (and slope) again with the centre held: a straight line through (h, rho).
            double n = 0, sh = 0, sr = 0, shh = 0, shr = 0;
            for (const int p : one.points) {
                const V e = pointOf(mesh, p) - tried.origin;
                const double h = e.Dot(tried.axis), rho = (e - tried.axis * h).Modulus();
                n += 1; sh += h; sr += rho; shh += h * h; shr += h * rho;
            }
            if (tried.kind == Surface::Cone && n * shh - sh * sh > 1e-12) {
                tried.slope = (n * shr - sh * sr) / (n * shh - sh * sh);
                tried.radius = (sr - tried.slope * sh) / n;
            } else {
                tried.radius = sr / n;
            }
            if (holds(tried, one.points)) {
                s = tried;
                break;
            }
        }
        rounds.push_back(&s);
    }
    // Then two surfaces that are one: a plane level with a bigger one, or a round wall on a bigger
    // one's axis with its radius, becomes that very surface, as CGAL's regularize_planes also snaps
    // coplanarity. Fitted apart they lie a hair off each other (the speaker mount's base had two side
    // faces 0.00007 mm apart), so the line between them stays and merging them breaks the solid.
    for (std::size_t i = 1; i < all.size(); ++i) {
        Surface &s = *all[i].surface;
        if ((s.kind != Surface::Flat && s.kind != Surface::Cylinder && s.kind != Surface::Cone) || all[i].points.size() < 3 || keep.count(&s))
            continue;
        for (std::size_t j = 0; j < i; ++j) {
            const Surface &big = *all[j].surface;
            const double c = big.axis.Dot(s.axis);
            if (big.kind != s.kind || std::abs(c) < 1 - 1e-15)
                continue;
            const V d = s.origin - big.origin;
            if (s.kind == Surface::Flat ? std::abs(d.Dot(big.axis)) > allowed
                                        : (d - big.axis * d.Dot(big.axis)).Modulus() > allowed)
                continue;
            Surface tried = s;
            tried.origin = big.origin;
            tried.axis = s.kind == Surface::Flat && c < 0 ? big.axis * -1 : big.axis;
            tried.radius = big.radius;
            tried.slope = big.slope;
            if (s.kind != Surface::Flat && std::abs(s.radius + s.slope * d.Dot(big.axis) - big.radius - big.slope * d.Dot(big.axis)) > allowed)
                continue;
            if (holds(tried, all[i].points)) {
                s = tried;
                break;
            }
        }
    }
    // And a round wall that blends into a flat one touches it exactly, as a CAD fillet does by being
    // one: the cylinder's axis moved, square to itself, until it sits its radius off each flat it runs
    // along (one or two). Fitted, a 0.3 mm round on the speaker mount's base stood a hair off its wall,
    // so the corner where it, the wall and the bottom met was on no single point of all three, and
    // everything cut through that corner came out tilted.
    for (Fitted &one : all) {
        Surface &s = *one.surface;
        if (s.kind != Surface::Cylinder || one.points.size() < 3 || keep.count(&s))
            continue;
        std::vector<const Fitted *> flats;
        for (const Fitted &other : all) {
            const Surface &f = *other.surface;
            if (f.kind != Surface::Flat || std::abs(f.axis.Dot(s.axis)) > 1e-12 || flats.size() == 2)
                continue;
            const double off = std::abs((s.origin - f.origin).Dot(f.axis));
            if (std::abs(off - s.radius) > allowed || off == s.radius
                || (flats.size() == 1 && std::abs(flats[0]->surface->axis.Dot(f.axis)) > 1 - 1e-9))
                continue;
            // It must blend into this very flat: they share mesh points (both lists are sorted), and
            // at one of them the round faces the flat's way, within a facet's turn.
            int blends = 0, shared = 0;
            for (const int p : other.points) {
                if (!std::binary_search(one.points.begin(), one.points.end(), p))
                    continue;
                ++shared;
                V e = pointOf(mesh, p) - s.origin;
                e -= s.axis * e.Dot(s.axis);
                if (e.Modulus() > 0 && std::abs(e.Dot(f.axis)) / e.Modulus() > std::cos(30 * M_PI / 180))
                    ++blends;
            }
            if (shared >= 2 && blends > 0)
                flats.push_back(&other);
        }
        if (flats.empty())
            continue;
        // Each flat: n.(origin + shift) - level = side * radius; the shift square to the axis.
        V shift(0, 0, 0);
        const auto miss = [&](const Surface &f) {
            const double off = (s.origin - f.origin).Dot(f.axis);
            return (off < 0 ? -s.radius : s.radius) - off;
        };
        const Surface &f0 = *flats[0]->surface;
        if (flats.size() == 1) {
            shift = f0.axis * miss(f0);
        } else {
            const Surface &f1 = *flats[1]->surface;
            const double c = f0.axis.Dot(f1.axis), m0 = miss(f0), m1 = miss(f1), det = 1 - c * c;
            shift = f0.axis * ((m0 - c * m1) / det) + f1.axis * ((m1 - c * m0) / det);
        }
        Surface tried = s;
        tried.origin = s.origin + shift;
        if (shift.Modulus() <= allowed && holds(tried, one.points))
            s = tried;
    }
    return forcedOff;
}

static TopoDS_Shape fitMesh(const Mesh &mesh, double share, const Stage &stage, FitReport &report, const std::vector<Surface> *threads) {
    stage("Finding the flat areas");
    const Facets f(mesh);
    const double size = reach(mesh);
    std::vector<int> labels;
    std::vector<Surface> surfaces;
    flatAreas(f, size, labels, surfaces);

    const double crease = std::cos(40 * M_PI / 180), flatEnough = std::cos(0.3 * M_PI / 180);
    // Tight: every edge inherits its surfaces' error, and a loose one folds a thin neighbour over.
    const double capMm = std::max(0.03, size * 2e-4);

    std::vector<int> owner(labels.size(), -1);     // which fitted surface took each triangle
    std::vector<Surface> fitted;
    std::vector<int> order(labels.size());
    std::iota(order.begin(), order.end(), 0);
    std::sort(order.begin(), order.end(), [&](int a, int b) { return f.area[std::size_t(a)] < f.area[std::size_t(b)]; });

    std::vector<double> offBy;
    double forcedOff = 0;          // how far a wall straightened by the short-wall rule sits off its points
    const auto keep = [&](Surface surface, const std::vector<int> &group, double off) {
        const int id = int(fitted.size());
        double facing = 0;
        for (const int t : group) {
            const auto &tri = mesh.triangles[std::size_t(t)];
            const V mid = (pointOf(mesh, tri[0]) + pointOf(mesh, tri[1]) + pointOf(mesh, tri[2])) / 3;
            facing += f.area[std::size_t(t)] * f.normal[std::size_t(t)].Dot(normalAt(surface, mid));
        }
        surface.inward = facing < 0;
        offBy.push_back(off);
        fitted.push_back(surface);
        for (const int t : group)
            owner[std::size_t(t)] = id;
    };

    stage("Reading sections");
    {
        const double tol = std::max(0.02, size * 2e-4);
        std::vector<Surface> found;
        std::vector<std::vector<int>> seeds;
        sectionSurfaces(f, size, tol, found, seeds);
        std::vector<Surface> screws;
        for (std::size_t i = found.size(); i-- > 0;)
            if (found[i].kind == Surface::Screw) {
                screws.insert(screws.begin(), found[i]);
                found.erase(found.begin() + std::ptrdiff_t(i));
                seeds.erase(seeds.begin() + std::ptrdiff_t(i));
            }
        if (!threads && !screws.empty()) {
            const Mesh cut = cutAlongThread(mesh, screws, tol);
            return fitMesh(cut, share, stage, report, &screws);
        }
        // The cut mesh has an edge on every line between these, so each triangle goes to one.
        if (threads)
            for (const Surface &thread : *threads) {
                found.push_back(thread);
                seeds.emplace_back(mesh.triangles.size());
                std::iota(seeds.back().begin(), seeds.back().end(), 0);
            }
        for (std::size_t i = 0; i < found.size(); ++i) {
            const Surface &surface = found[i];
            const auto fits = [&](int t) {
                if (owner[std::size_t(t)] >= 0)
                    return false;
                const auto &tri = mesh.triangles[std::size_t(t)];
                double worst = 0;
                for (const int p : tri)
                    worst = std::max(worst, distanceTo(surface, pointOf(mesh, p)));
                if (worst > tol * 1.5)
                    return false;
                if (surface.kind == Surface::Screw)
                    for (const int p : tri) {
                        double w, rho;
                        onScrew(surface, pointOf(mesh, p), w, rho);
                        if (w < surface.outline.front()[0] - 6e-3 || w > surface.outline.back()[0] + 6e-3)
                            return false;
                    }
                // Thin slivers along a helix have no useful normal; its corners on the band are enough.
                if (surface.kind == Surface::Screw && threads)
                    return true;
                const V mid = (pointOf(mesh, tri[0]) + pointOf(mesh, tri[1]) + pointOf(mesh, tri[2])) / 3;
                return std::abs(f.normal[std::size_t(t)].Dot(normalAt(surface, mid))) >= std::cos(35 * M_PI / 180);
            };
            std::set<int> in;
            std::vector<int> group;
            for (const int t : seeds[i])
                if (fits(t) && in.insert(t).second)
                    group.push_back(t);
            for (std::size_t g = 0; g < group.size(); ++g)
                for (const int other : f.across[std::size_t(group[g])])
                    if (other >= 0 && !in.count(other) && fits(other)) {
                        in.insert(other);
                        group.push_back(other);
                    }
            if (group.size() >= 8)
                keep(surface, group, tol * 1.5);
        }
    }

    stage("Fitting curved surfaces");
    int tick = 0;
    for (const int seed : order) {
        if (++tick % 4096 == 0)
            stage("Fitting curved surfaces");
        if (owner[std::size_t(seed)] >= 0)
            continue;
        // How sharply this curve bends from facet to facet: its gentlest real bend. Half as much
        // again is still this curve; more is a corner into something else (a 35 degree shoulder
        // beside a 10 degree cylinder), even if that's under the crease limit elsewhere.
        double bend = -1;
        for (const int other : f.across[std::size_t(seed)])
            if (other >= 0) {
                const double dot = f.normal[std::size_t(other)].Dot(f.normal[std::size_t(seed)]);
                if (dot < flatEnough)
                    bend = std::max(bend, dot);
            }
        if (bend < crease)
            continue;
        const double limit = std::max(crease, std::cos(std::acos(std::min(bend, 1.0)) * 1.5 + 1e-3));
        const auto near = [&](const std::vector<int> &from, std::set<int> &in, std::vector<int> &out, std::size_t most) {
            for (std::size_t i = 0; i < from.size() && out.size() < most; ++i)
                for (const int other : f.across[std::size_t(from[i])])
                    if (other >= 0 && !in.count(other) && owner[std::size_t(other)] < 0
                        && f.normal[std::size_t(other)].Dot(f.normal[std::size_t(from[i])]) > limit) {
                        in.insert(other);
                        out.push_back(other);
                    }
        };
        // A patch of smooth, bent neighbours around the seed.
        std::vector<int> patch{seed};
        std::set<int> inPatch{seed};
        near(patch, inPatch, patch, 24);
        if (patch.size() < 6)
            continue;
        double spread = 1;
        for (const int t : patch)
            spread = std::min(spread, f.normal[std::size_t(t)].Dot(f.normal[std::size_t(seed)]));
        if (spread > std::cos(2 * M_PI / 180))
            continue;    // flat here
        Surface surface;
        double off = 0;
        if (!fitRound(f, patch, share, capMm, surface, off))
            continue;
        // Grow while it holds, refit, and again: a bigger patch can show it's a different surface.
        std::vector<int> group = patch;
        std::set<int> inGroup(group.begin(), group.end());
        for (int pass = 0; pass < 64; ++pass) {
            const std::size_t before = group.size();
            const double allowed = std::min(capMm, std::max(surface.radius, 1e-6) * share);
            for (std::size_t i = 0; i < group.size(); ++i)
                for (const int other : f.across[std::size_t(group[i])]) {
                    if (other < 0 || inGroup.count(other) || owner[std::size_t(other)] >= 0)
                        continue;
                    const auto &tri = f.mesh.triangles[std::size_t(other)];
                    bool on = true;
                    for (const int p : tri)
                        on = on && distanceTo(surface, pointOf(mesh, p)) <= allowed;
                    // Facing the way the surface does there, either side: a hole faces the axis.
                    const V mid = (pointOf(mesh, tri[0]) + pointOf(mesh, tri[1]) + pointOf(mesh, tri[2])) / 3;
                    if (!on || std::abs(f.normal[std::size_t(other)].Dot(normalAt(surface, mid))) < std::cos(35 * M_PI / 180))
                        continue;
                    inGroup.insert(other);
                    group.push_back(other);
                }
            Surface better;
            double betterOff = 0;
            if (fitRound(f, group, share, capMm, better, betterOff)) {
                surface = better;
                off = betterOff;
            }
            // One ring further, past what this surface allows: a band of a sphere passes as a
            // cylinder until more of it shows.
            std::vector<int> wider = group;
            std::set<int> inWider = inGroup;
            near(group, inWider, wider, group.size() * 2);
            if (wider.size() > group.size() && fitRound(f, wider, share, capMm, better, betterOff)) {
                surface = better;
                off = betterOff;
                group = std::move(wider);
                inGroup = std::move(inWider);
            }
            if (group.size() == before)
                break;
        }
        if (group.size() < 8)
            continue;
        keep(surface, group, off);
    }

    // Fits start from the smallest triangles, so a little surface can hold what belongs to a big
    // one (a dome's crown). Biggest first, each takes back what it can reach and still fit.
    {
        std::vector<double> areaOf(fitted.size(), 0);
        std::vector<std::vector<int>> members(fitted.size());
        for (std::size_t t = 0; t < owner.size(); ++t)
            if (owner[t] >= 0) {
                areaOf[std::size_t(owner[t])] += f.area[t];
                members[std::size_t(owner[t])].push_back(int(t));
            }
        std::vector<int> big(fitted.size());
        std::iota(big.begin(), big.end(), 0);
        std::sort(big.begin(), big.end(), [&](int a, int b) { return areaOf[std::size_t(a)] > areaOf[std::size_t(b)]; });
        std::vector<int> group;
        std::vector<char> settled(fitted.size(), 0);
        for (const int id : big) {
            const Surface &surface = fitted[std::size_t(id)];
            const double allowed = std::min(capMm, std::max(surface.radius, 1e-6) * share);
            group.clear();
            for (const int t : members[std::size_t(id)])
                if (owner[std::size_t(t)] == id)
                    group.push_back(t);
            for (std::size_t i = 0; i < group.size(); ++i)
                for (const int other : f.across[std::size_t(group[i])]) {
                    if (other < 0 || owner[std::size_t(other)] == id || (owner[std::size_t(other)] >= 0 && settled[std::size_t(owner[std::size_t(other)])]))
                        continue;
                    const auto &tri = mesh.triangles[std::size_t(other)];
                    bool on = true;
                    for (const int p : tri)
                        on = on && distanceTo(surface, pointOf(mesh, p)) <= allowed;
                    const V mid = (pointOf(mesh, tri[0]) + pointOf(mesh, tri[1]) + pointOf(mesh, tri[2])) / 3;
                    const double facing = f.normal[std::size_t(other)].Dot(normalAt(surface, mid));
                    if (!on || (surface.inward ? -facing : facing) < std::cos(35 * M_PI / 180))
                        continue;
                    owner[std::size_t(other)] = id;
                    group.push_back(other);
                }
            settled[std::size_t(id)] = 1;
        }
        // What's left of a robbed surface may be too little to stand alone.
        std::vector<int> left(fitted.size(), 0);
        for (const int o : owner)
            if (o >= 0)
                ++left[std::size_t(o)];
        for (int &o : owner)
            if (o >= 0 && left[std::size_t(o)] < 8)
                o = -1;
    }

    stage("Reading rounded edges");
    {
        const double tol = std::max(0.02, size * 2e-4);
        std::vector<Surface> found;
        std::vector<std::vector<int>> seeds;
        std::vector<char> onBigFlat, soft(owner.size(), 0);
        std::vector<V> beside;
        // A small surface here may only be a near match for a fillet (a ball for a tight torus), and
        // gives way to one that's exact.
        std::vector<double> held(fitted.size(), 0);
        for (std::size_t t = 0; t < owner.size(); ++t)
            if (owner[t] >= 0)
                held[std::size_t(owner[t])] += f.area[t];
        for (std::size_t t = 0; t < owner.size(); ++t)
            soft[t] = owner[t] >= 0 && held[std::size_t(owner[t])] < size * size * 2e-3;
        filletSurfaces(f, labels, surfaces, owner, size, tol, found, seeds, beside, soft, onBigFlat);
        const int firstFillet = int(fitted.size());
        for (std::size_t i = 0; i < found.size(); ++i) {
            const Surface &surface = found[i];
            // Past the edge beside its flat it runs on, level with that flat, and those triangles
            // would fold it over itself: it stays on the side of its centre that it was found on.
            V middle(0, 0, 0);
            const auto normalOn = [&](int t) {
                const auto &tri = mesh.triangles[std::size_t(t)];
                return normalAt(surface, (pointOf(mesh, tri[0]) + pointOf(mesh, tri[1]) + pointOf(mesh, tri[2])) / 3);
            };
            for (const int t : seeds[i])
                middle += normalOn(t) * f.area[std::size_t(t)];
            const double flip = middle.Dot(beside[i]) < 0 ? -1 : 1;
            V sideways = middle * flip - beside[i] * (middle * flip).Dot(beside[i]);
            sideways /= std::max(sideways.Modulus(), 1e-12);
            const auto onItsSide = [&](int t) {
                const auto &tri = mesh.triangles[std::size_t(t)];
                const V n = normalOn(t) * flip;
                V way = sideways;
                if (surface.kind == Surface::Torus) {
                    const V d = (pointOf(mesh, tri[0]) + pointOf(mesh, tri[1]) + pointOf(mesh, tri[2])) / 3 - surface.origin;
                    way = d - surface.axis * d.Dot(surface.axis);
                    way /= std::max(way.Modulus(), 1e-12);
                    way *= std::cos(surface.facing) >= 0 ? flip : -flip;
                }
                return n.Dot(beside[i]) >= -0.05 && n.Dot(way) >= -0.03;
            };
            const auto fits = [&](int t) {
                // At its edge a fillet and its flat are one height: only the flat's own triangles tell.
                if (onBigFlat[std::size_t(t)])
                    return false;
                const auto &tri = mesh.triangles[std::size_t(t)];
                double worst = 0;
                for (const int p : tri)
                    worst = std::max(worst, distanceTo(surface, pointOf(mesh, p)));
                // A fillet's last rows run nearly level with the wall, and a wall's surface, loose
                // about its own fit, takes them. They go to whichever they really lie on.
                const int holder = owner[std::size_t(t)];
                if (holder >= 0 && !soft[std::size_t(t)]) {
                    const Surface &held = fitted[std::size_t(holder)];
                    if (held.kind != Surface::Cone && held.kind != Surface::Cylinder)
                        return false;
                    double there = 0;
                    for (const int p : tri)
                        there = std::max(there, distanceTo(held, pointOf(mesh, p)));
                    if (worst + 0.005 > there)
                        return false;
                }
                const V mid = (pointOf(mesh, tri[0]) + pointOf(mesh, tri[1]) + pointOf(mesh, tri[2])) / 3;
                // A long sliver across a tight curve tilts far from the surface under its middle, though its
                // corners are on it.
                const double tilt = (worst <= tol * 0.5 ? 65 : 35) * M_PI / 180;
                return worst <= tol * 1.5 && onItsSide(t) && std::abs(f.normal[std::size_t(t)].Dot(normalAt(surface, mid))) >= std::cos(tilt);
            };
            std::set<int> in;
            std::vector<int> group;
            for (const int t : seeds[i])
                if (fits(t) && in.insert(t).second)
                    group.push_back(t);
            for (std::size_t g = 0; g < group.size(); ++g)
                for (const int other : f.across[std::size_t(group[g])])
                    if (other >= 0 && !in.count(other) && fits(other)) {
                        in.insert(other);
                        group.push_back(other);
                    }
            // More of a fillet beside it, found separately, is that fillet: an edge between the two
            // would cut notches in both.
            int joins = -1;
            for (const int t : group)
                for (const int other : f.across[std::size_t(t)]) {
                    if (other < 0 || joins >= 0 || in.count(other) || owner[std::size_t(other)] < firstFillet)
                        continue;
                    const Surface &beyond = fitted[std::size_t(owner[std::size_t(other)])];
                    bool lies = beyond.kind == surface.kind;
                    for (const int member : group)
                        for (const int p : mesh.triangles[std::size_t(member)])
                            lies = lies && distanceTo(beyond, pointOf(mesh, p)) <= tol * 1.5;
                    if (lies)
                        joins = owner[std::size_t(other)];
                }
            if (joins >= 0) {
                for (const int t : group) {
                    owner[std::size_t(t)] = joins;
                    soft[std::size_t(t)] = 0;
                }
            } else if (group.size() >= 4) {
                keep(surface, group, tol * 1.5);
                for (const int t : group)
                    soft[std::size_t(t)] = 0;
            }
        }
        // A big ring's mesh has few points, so between two grooves each stretch of it reads as two
        // straight rows, and a cylinder takes each. They lie on the ring.
        {
            std::vector<std::vector<int>> rows(fitted.size());
            for (std::size_t t = 0; t < owner.size(); ++t)
                if (owner[t] >= firstFillet)
                    rows[std::size_t(owner[t])].push_back(int(t));
            for (std::size_t i = std::size_t(firstFillet); i < fitted.size(); ++i) {
                if (fitted[i].kind != Surface::Cylinder)
                    continue;
                for (std::size_t j = std::size_t(firstFillet); j < fitted.size(); ++j) {
                    if (fitted[j].kind != Surface::Torus || std::abs(fitted[j].tube - fitted[i].radius) > tol * 0.5)
                        continue;
                    bool lies = !rows[i].empty();
                    for (const int t : rows[i])
                        for (const int p : mesh.triangles[std::size_t(t)])
                            lies = lies && distanceTo(fitted[j], pointOf(mesh, p)) <= tol * 0.5;
                    if (!lies)
                        continue;
                    for (const int t : rows[i])
                        owner[std::size_t(t)] = int(j);
                    break;
                }
            }
        }
        // What's left of a surface that gave way may be too little to stand alone.
        std::vector<int> left(fitted.size(), 0);
        for (const int o : owner)
            if (o >= 0)
                ++left[std::size_t(o)];
        for (int &o : owner)
            if (o >= 0 && left[std::size_t(o)] < 8)
                o = -1;
        // A plain cone turned around an axis this part already has, like a chamfer at the mouth of a
        // hole, can have facets too coarse to grow from. Round that axis it's a straight line of
        // radius against height.
        {
            std::vector<std::array<V, 2>> axes;
            const auto addAxis = [&](const Surface &surface) {
                if (surface.kind != Surface::Cylinder && surface.kind != Surface::Cone && surface.kind != Surface::Screw)
                    return;
                for (const auto &axis : axes) {
                    const V away = surface.origin - axis[0];
                    if (std::abs(axis[1].Dot(surface.axis)) > 0.9999 && (away - axis[1] * away.Dot(axis[1])).Modulus() < tol * 2)
                        return;
                }
                axes.push_back({surface.origin, surface.axis});
            };
            if (threads)
                for (const Surface &thread : *threads)
                    addAxis(thread);
            for (const Surface &surface : fitted)
                addAxis(surface);
            std::vector<char> seen(owner.size(), 0);
            for (std::size_t first = 0; first < owner.size(); ++first) {
                if (seen[first] || owner[first] >= 0 || onBigFlat[first])
                    continue;
                std::vector<int> patch{int(first)};
                seen[first] = 1;
                for (std::size_t i = 0; i < patch.size(); ++i)
                    for (const int other : f.across[std::size_t(patch[i])])
                        if (other >= 0 && !seen[std::size_t(other)] && owner[std::size_t(other)] < 0 && !onBigFlat[std::size_t(other)]
                            && f.normal[std::size_t(other)].Dot(f.normal[std::size_t(patch[i])]) > std::cos(40 * M_PI / 180)) {
                            seen[std::size_t(other)] = 1;
                            patch.push_back(other);
                        }
                if (patch.size() < 12)
                    continue;
                std::vector<int> best;
                Surface cone;
                for (const auto &axis : axes) {
                    std::vector<int> on = patch;
                    double start = 0, gain = 0, low = 0, high = 0;
                    // Each pass drops what's off the line, tighter each time, so strays don't tilt it.
                    for (const double reach : {8.0, 4.0, 2.0, 1.0, 0.5}) {
                        double sw = 0, sh = 0, sr = 0, shh = 0, shr = 0;
                        low = INFINITY;
                        high = -INFINITY;
                        for (const int t : on)
                            for (const int p : mesh.triangles[std::size_t(t)]) {
                                const V d = pointOf(mesh, p) - axis[0];
                                const double h = d.Dot(axis[1]), r = (d - axis[1] * h).Modulus();
                                sw += 1;
                                sh += h;
                                sr += r;
                                shh += h * h;
                                shr += h * r;
                                low = std::min(low, h);
                                high = std::max(high, h);
                            }
                        const double spread = sw * shh - sh * sh;
                        if (sw < 3 || spread <= 1e-9)
                            break;
                        gain = (sw * shr - sh * sr) / spread;
                        start = (sr - gain * sh) / sw;
                        std::vector<int> held;
                        for (const int t : on) {
                            bool close = true;
                            for (const int p : mesh.triangles[std::size_t(t)]) {
                                const V d = pointOf(mesh, p) - axis[0];
                                const double h = d.Dot(axis[1]), r = (d - axis[1] * h).Modulus();
                                close = close && std::abs(r - start - gain * h) / std::hypot(1.0, gain) <= tol * reach;
                            }
                            if (close)
                                held.push_back(t);
                        }
                        on = std::move(held);
                    }
                    if (on.size() < 12 || on.size() <= best.size() || high - low < tol * 5)
                        continue;
                    best = std::move(on);
                    cone.kind = std::abs(gain) * (high - low) < tol ? Surface::Cylinder : Surface::Cone;
                    cone.axis = axis[1];
                    cone.origin = axis[0] + axis[1] * low;
                    cone.radius = start + gain * low;
                    cone.slope = cone.kind == Surface::Cone ? gain : 0;
                }
                if (!best.empty())
                    keep(cone, best, tol * 1.5);
            }
        }
        // Scraps no surface reached: a few triangles walled off from the fillet they lie on, or just
        // past a surface's own allowance. A surface beside them may stretch a little; one further
        // off only takes them if they're exactly on it.
        const auto lying = [&](int t, const Surface &surface, double most) {
            if (surface.kind == Surface::Flat || surface.kind == Surface::Screw)
                return false;
            const auto &tri = mesh.triangles[std::size_t(t)];
            double worst = 0;
            for (const int p : tri)
                worst = std::max(worst, distanceTo(surface, pointOf(mesh, p)));
            if (worst > most)
                return false;
            const V mid = (pointOf(mesh, tri[0]) + pointOf(mesh, tri[1]) + pointOf(mesh, tri[2])) / 3;
            return std::abs(f.normal[std::size_t(t)].Dot(normalAt(surface, mid))) >= std::cos((worst <= tol * 0.5 ? 65 : 35) * M_PI / 180);
        };
        for (int pass = 0; pass < 8; ++pass) {
            bool changed = false;
            for (std::size_t t = 0; t < owner.size(); ++t) {
                if (owner[t] >= 0 || onBigFlat[t])
                    continue;
                int to = -1;
                for (const int other : f.across[t])
                    if (to < 0 && other >= 0 && owner[std::size_t(other)] >= 0 && lying(int(t), fitted[std::size_t(owner[std::size_t(other)])], tol * 1.75))
                        to = owner[std::size_t(other)];
                for (std::size_t i = 0; to < 0 && i < fitted.size(); ++i)
                    if ((fitted[i].kind == Surface::Cylinder || fitted[i].kind == Surface::Torus) && lying(int(t), fitted[i], 5e-3))
                        to = int(i);
                if (to >= 0) {
                    owner[t] = to;
                    changed = true;
                }
            }
            if (!changed)
                break;
        }
    }
    {
        std::vector<Fitted> all;
        std::vector<std::set<int>> fittedPoints(fitted.size()), flatPoints(surfaces.size());
        std::vector<double> fittedArea(fitted.size(), 0), flatArea(surfaces.size(), 0);
        for (std::size_t t = 0; t < owner.size(); ++t) {
            const auto &tri = mesh.triangles[t];
            if (owner[t] >= 0) {
                fittedPoints[std::size_t(owner[t])].insert(tri.begin(), tri.end());
                fittedArea[std::size_t(owner[t])] += f.area[t];
            } else if (labels[t] >= 0) {
                flatPoints[std::size_t(labels[t])].insert(tri.begin(), tri.end());
                flatArea[std::size_t(labels[t])] += f.area[t];
            }
        }
        for (std::size_t i = 0; i < fitted.size(); ++i)
            all.push_back({&fitted[i], {fittedPoints[i].begin(), fittedPoints[i].end()}, fittedArea[i]});
        for (std::size_t i = 0; i < surfaces.size(); ++i)
            if (surfaces[i].kind == Surface::Flat)
                all.push_back({&surfaces[i], {flatPoints[i].begin(), flatPoints[i].end()}, flatArea[i]});
        forcedOff = regularize(mesh, all, std::max(0.02, size * 2e-4) * 1.5);
    }
    // A surface whose faces won't hold goes back to facets, and the build runs again without it.
    double tolerance = std::max(1e-3, forcedOff * 1.5);
    for (const double off : offBy)
        tolerance = std::max(tolerance, off * 1.5);
    for (int round = 0; round < 12; ++round) {
        // A surface that goes all the way round is cut in two so its faces have ordinary edges;
        // the halves are merged at the end. A part-way one stays whole, its wrap-around line
        // moved behind it, out of the way.
        std::vector<int> side(owner.size(), 0);
        {
            std::vector<V> facing(fitted.size(), V(0, 0, 0));
            std::vector<std::array<char, 36>> bins(fitted.size());
            for (auto &b : bins)
                b.fill(0);
            for (std::size_t t = 0; t < owner.size(); ++t) {
                if (owner[t] < 0)
                    continue;
                const Surface &s = fitted[std::size_t(owner[t])];
                const auto &tri = mesh.triangles[t];
                const V mid = (pointOf(mesh, tri[0]) + pointOf(mesh, tri[1]) + pointOf(mesh, tri[2])) / 3 - s.origin;
                const V radial = mid - s.axis * mid.Dot(s.axis);
                const double length = radial.Modulus();
                if (length < 1e-9)
                    continue;
                facing[std::size_t(owner[t])] += radial / length * f.area[t];
                const gp_Ax3 frame(gp_Pnt(0, 0, 0), gp_Dir(s.axis));
                const double angle = std::atan2(radial.Dot(frame.YDirection().XYZ()), radial.Dot(frame.XDirection().XYZ())) + M_PI;
                for (const int p : tri) {
                    const V at = pointOf(mesh, p) - s.origin;
                    const double spin = std::atan2(at.Dot(frame.YDirection().XYZ()), at.Dot(frame.XDirection().XYZ())) + M_PI;
                    bins[std::size_t(owner[t])][std::min(35, int(spin / (2 * M_PI) * 36))] = 1;
                }
                bins[std::size_t(owner[t])][std::min(35, int(angle / (2 * M_PI) * 36))] = 1;
            }
            for (std::size_t i = 0; i < fitted.size(); ++i) {
                const bool full = fitted[i].kind != Surface::Screw && std::accumulate(bins[i].begin(), bins[i].end(), 0) >= 33;
                fitted[i].seam = full ? V(0, 0, 0) : V(0, 0, 0) - facing[i];
                if (full)
                    bins[i][0] = 2;      // marks full below
            }
            for (std::size_t t = 0; t < owner.size(); ++t) {
                if (owner[t] < 0)
                    continue;
                side[t] = 1;
                const Surface &s = fitted[std::size_t(owner[t])];
                if (bins[std::size_t(owner[t])][0] != 2)
                    continue;
                const auto &tri = mesh.triangles[t];
                const V mid = (pointOf(mesh, tri[0]) + pointOf(mesh, tri[1]) + pointOf(mesh, tri[2])) / 3 - s.origin;
                side[t] = mid.Dot(gp_Ax3(gp_Pnt(0, 0, 0), gp_Dir(s.axis)).YDirection().XYZ()) >= 0 ? 1 : 2;
            }
        }
        tidy(f, fitted, owner, side);

        // Every joined area its own face: taking triangles out can leave a flat in two pieces,
        // and a face is one area.
        std::vector<int> outLabels(owner.size(), -1), fittedOf;
        std::vector<Surface> all;
        std::vector<int> stack;
        for (std::size_t first = 0; first < owner.size(); ++first) {
            if (outLabels[first] >= 0)
                continue;
            const int label = int(all.size());
            all.push_back(owner[first] >= 0 ? fitted[std::size_t(owner[first])] : surfaces[std::size_t(labels[first])]);
            fittedOf.push_back(owner[first]);
            outLabels[first] = label;
            stack = {int(first)};
            while (!stack.empty()) {
                const std::size_t t = std::size_t(stack.back());
                stack.pop_back();
                for (const int other : f.across[t]) {
                    if (other < 0 || outLabels[std::size_t(other)] >= 0 || owner[std::size_t(other)] != owner[t])
                        continue;
                    if (owner[t] >= 0 ? side[std::size_t(other)] != side[t] : labels[std::size_t(other)] != labels[t])
                        continue;
                    outLabels[std::size_t(other)] = label;
                    stack.push_back(other);
                }
            }
        }

        // Pieces of one surface can sit all round it (four valleys of one cone), which leaves no
        // "behind" for the surface as a whole: each piece puts the wrap-around line behind itself.
        {
            std::vector<V> behind(all.size(), V(0, 0, 0));
            for (std::size_t t = 0; t < owner.size(); ++t) {
                if (owner[t] < 0 || side[t] != 1)
                    continue;
                const Surface &s = all[std::size_t(outLabels[t])];
                const auto &tri = mesh.triangles[t];
                const V mid = (pointOf(mesh, tri[0]) + pointOf(mesh, tri[1]) + pointOf(mesh, tri[2])) / 3 - s.origin;
                behind[std::size_t(outLabels[t])] += (mid - s.axis * mid.Dot(s.axis)) * f.area[t];
            }
            for (std::size_t i = 0; i < all.size(); ++i)
                if (fittedOf[i] >= 0 && all[i].seam.Modulus() > 0 && behind[i].Modulus() > 0)
                    all[i].seam = V(0, 0, 0) - behind[i];
        }

        std::vector<std::pair<TopoDS_Face, int>> made;
        TopoDS_Shape shape = assemble(mesh, outLabels, all, tolerance, stage, &made);
        if (shape.IsNull())
            return {};
        stage("Joining faces to the surfaces");
        std::set<int> failed;
        try {
            ShapeFix_Edge edges;
            for (const auto &[face, label] : made)
                for (TopExp_Explorer e(face, TopAbs_EDGE); e.More(); e.Next())
                    {
                    const Surface &on = all[std::size_t(label)];
                    const TopoDS_Edge edge = TopoDS::Edge(e.Current());
                    const bool laid = on.kind == Surface::Screw ? screwEdge(edge, face, on, tolerance)
                                      : on.kind == Surface::Cylinder || on.kind == Surface::Torus ? filletEdge(edge, face, BRep_Tool::Surface(face), tolerance)
                                                                                                  : false;
                    if (!laid)
                    // Tight: at a thin surface's own size it can't tell which way round an edge goes.
                    edges.FixAddPCurve(TopoDS::Edge(e.Current()), face, false, std::min(tolerance, 1e-3));
                    }
            BRepLib::SameParameter(shape, tolerance, true);
            // The half past the wrap-around line needs its edges shifted a full turn along. Only
            // that: the general repair drops tiny faces, which opens the solid.
            for (const auto &[face, label] : made) {
                if (std::size_t(label) >= fittedOf.size() || fittedOf[std::size_t(label)] < 0)
                    continue;
                for (TopExp_Explorer w(face, TopAbs_WIRE); w.More(); w.Next()) {
                    ShapeFix_Wire wire(TopoDS::Wire(w.Current()), face, tolerance);
                    wire.FixShifted();
                }
            }
            // A bad face is its surface's fault, or if it's a facet, the surface beside it.
            std::set<int> badFlats;
            for (const auto &[face, label] : made) {
                if (BRepCheck_Analyzer(face).IsValid())
                    continue;
                if (std::size_t(label) >= fittedOf.size())
                    continue;
                if (fittedOf[std::size_t(label)] >= 0)
                    failed.insert(fittedOf[std::size_t(label)]);
                else
                    badFlats.insert(label);
            }
            if (!badFlats.empty())
                for (std::size_t t = 0; t < labels.size(); ++t)
                    if (owner[t] < 0 && badFlats.count(outLabels[t]))
                        for (const int other : f.across[t])
                            if (other >= 0 && owner[std::size_t(other)] >= 0)
                                failed.insert(owner[std::size_t(other)]);
        } catch (const Standard_Failure &) {
            return {};
        }
        if (!failed.empty()) {
            // Forget those surfaces; the rest keep their numbers.
            for (int &o : owner)
                if (o >= 0 && failed.count(o))
                    o = -1;
            continue;
        }
        // Past this many faces the merge runs for tens of minutes; the halves are valid as they are.
        constexpr std::size_t kMergeFaces = 20000;
        try {
            if (made.size() <= kMergeFaces) {
                stage("Merging the halves");
                // FreeCAD's way first (refine.h), then OpenCascade's for what it leaves.
                if (const TopoDS_Shape refined = refine::sameSurfaces(shape); !refined.IsNull())
                    shape = refined;
                ShapeUpgrade_UnifySameDomain unify(shape, true, true, false);
                unify.Build();
                if (BRepCheck_Analyzer(unify.Shape()).IsValid())
                    shape = unify.Shape();
            }
        } catch (const Standard_Failure &) {
        }
        // The repairs above can leave faces turned the wrong way, or the solid inside out.
        if (shape.ShapeType() == TopAbs_SOLID && !BRepCheck_Analyzer(shape).IsValid()) {
            ShapeFix_Shell faces(TopoDS::Shell(TopExp_Explorer(shape, TopAbs_SHELL).Current()));
            faces.Perform();
            TopoDS_Solid turned;
            BRep_Builder().MakeSolid(turned);
            BRep_Builder().Add(turned, faces.Shell());
            if (BRepLib::OrientClosedSolid(turned))
                shape = turned;
        }
        const BRepCheck_Analyzer checked(shape);
        if (!checked.IsValid()) {
            // Still wrong as a whole: whatever surface sits at the bad faces goes back to facets.
            for (TopExp_Explorer it(shape, TopAbs_FACE); it.More(); it.Next()) {
                if (checked.IsValid(it.Current()))
                    continue;
                Bnd_Box box;
                BRepBndLib::Add(it.Current(), box);
                box.Enlarge(tolerance);
                for (std::size_t t = 0; t < owner.size(); ++t)
                    if (owner[t] >= 0 && !box.IsOut(gp_Pnt(pointOf(mesh, mesh.triangles[t][0]))))
                        failed.insert(owner[t]);
            }
            if (failed.empty())
                return {};
            for (int &o : owner)
                if (o >= 0 && failed.count(o))
                    o = -1;
            continue;
        }
        std::set<int> kept;
        report.facetsLeft = 0;
        for (const int o : owner) {
            report.facetsLeft += o < 0;
            if (o >= 0)
                kept.insert(o);
        }
        for (const int o : kept) {
            report.worstMm = std::max(report.worstMm, offBy[std::size_t(o)]);
            switch (fitted[std::size_t(o)].kind) {
            case Surface::Cylinder: ++report.cylinders; break;
            case Surface::Cone: ++report.cones; break;
            case Surface::Sphere: ++report.spheres; break;
            default: break;
            }
        }
        return shape;
    }
    return {};
}

TopoDS_Shape fit(const Mesh &mesh, double share, const Stage &stage, FitReport &report) {
    return fitMesh(mesh, share, stage, report, nullptr);
}

} // namespace meshsolid
