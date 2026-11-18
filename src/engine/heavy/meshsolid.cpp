#include "engine/heavy/meshsolid.h"
#include "engine/heavy/refine.h"

#include <BRepBndLib.hxx>
#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepBuilderAPI_MakeVertex.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <BRepCheck_Result.hxx>
#include <BRepCheck_ListOfStatus.hxx>
#include <BRepLib.hxx>
#include <BRep_Builder.hxx>
#include <BRepTools.hxx>
#include <BRepAdaptor_Curve.hxx>
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
#include <TopoDS_Compound.hxx>
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
#include <cstdio>
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

// Fins and doubles: a triangle written twice over the same three points. Turned opposite ways
// the two enclose nothing (OpenSCAD writes such pairs along a joint) and both go; the same way
// round, one copy goes.
void dropDoubles(Mesh &m, std::vector<int> *labels = nullptr) {
    std::map<std::array<int, 3>, std::vector<int>> byCorners;
    for (std::size_t t = 0; t < m.triangles.size(); ++t) {
        auto k = m.triangles[t];
        std::sort(k.begin(), k.end());
        byCorners[k].push_back(int(t));
    }
    // Which way round: the corners turned to start at the lowest.
    auto turn = [&](int t) {
        auto c = m.triangles[std::size_t(t)];
        std::rotate(c.begin(), std::min_element(c.begin(), c.end()), c.end());
        return c[1] < c[2];
    };
    std::vector<char> gone(m.triangles.size(), 0);
    bool any = false;
    for (const auto &[k, list] : byCorners) {
        if (list.size() < 2)
            continue;
        std::vector<int> up, down;
        for (const int t : list)
            (turn(t) ? up : down).push_back(t);
        const std::size_t pairs = std::min(up.size(), down.size());
        for (std::size_t i = 0; i < pairs; ++i)
            gone[std::size_t(up[i])] = gone[std::size_t(down[i])] = 1;
        for (const auto *rest : {&up, &down})
            for (std::size_t i = pairs + 1; i < rest->size(); ++i)
                gone[std::size_t((*rest)[i])] = 1;
        any = true;
    }
    if (!any)
        return;
    std::vector<std::array<int, 3>> kept;
    std::vector<int> keptLabels;
    for (std::size_t t = 0; t < m.triangles.size(); ++t)
        if (!gone[t]) {
            kept.push_back(m.triangles[t]);
            if (labels)
                keptLabels.push_back((*labels)[t]);
        }
    m.triangles = std::move(kept);
    if (labels)
        *labels = std::move(keptLabels);
}

// Points at one place made one (a mesh's own copies, or two corners moved onto one spot); a
// triangle left with two corners the same goes.
void joinSame(Mesh &m, double cell, std::vector<int> *labels = nullptr, std::vector<int> *mapped = nullptr) {
    std::map<std::array<std::int64_t, 3>, int> seen;
    std::vector<int> to(m.points.size());
    std::vector<std::array<double, 3>> points;
    for (std::size_t i = 0; i < m.points.size(); ++i) {
        const auto &p = m.points[i];
        const std::array<std::int64_t, 3> key{std::llround(p[0] / cell), std::llround(p[1] / cell), std::llround(p[2] / cell)};
        const auto [it, added] = seen.emplace(key, int(points.size()));
        if (added)
            points.push_back(p);
        to[i] = it->second;
    }
    std::vector<std::array<int, 3>> kept;
    std::vector<int> keptLabels;
    for (std::size_t t = 0; t < m.triangles.size(); ++t) {
        const auto &tri = m.triangles[t];
        const std::array<int, 3> n{to[std::size_t(tri[0])], to[std::size_t(tri[1])], to[std::size_t(tri[2])]};
        if (n[0] == n[1] || n[1] == n[2] || n[0] == n[2])
            continue;
        kept.push_back(n);
        if (labels)
            keptLabels.push_back((*labels)[t]);
    }
    m.points = std::move(points);
    m.triangles = std::move(kept);
    if (labels)
        *labels = std::move(keptLabels);
    if (mapped)
        *mapped = std::move(to);
}

// Slivers: triangles whose corners lie in a line (exporters write them along long thin walls, and
// they have no facing to build a face from). The corner lying on the long side is put into the
// triangle across that side, which is split in two there; the sliver goes. Still closed after.
void dropSlivers(Mesh &m, double size, std::vector<int> *labels = nullptr) {
    const double thin = std::max(1e-7, size * 1e-9);
    for (int pass = 0; pass < 8; ++pass) {
        std::map<Key, std::vector<int>> sides;
        for (std::size_t t = 0; t < m.triangles.size(); ++t)
            for (int k = 0; k < 3; ++k)
                sides[sideKey(m.triangles[t][std::size_t(k)], m.triangles[t][std::size_t((k + 1) % 3)])].push_back(int(t));
        std::vector<char> gone(m.triangles.size(), 0), touched(m.triangles.size(), 0);
        std::vector<std::array<int, 3>> added;
        std::vector<int> addedLabels;
        int fixed = 0;
        for (std::size_t t = 0; t < m.triangles.size(); ++t) {
            if (touched[t])
                continue;
            const auto tri = m.triangles[t];
            // The longest side, and how far the third corner is off it.
            int k = 0;
            double longest = -1;
            for (int i = 0; i < 3; ++i) {
                const double l = (pointOf(m, tri[std::size_t((i + 1) % 3)]) - pointOf(m, tri[std::size_t(i)])).Modulus();
                if (l > longest)
                    longest = l, k = i;
            }
            const int a = tri[std::size_t(k)], b = tri[std::size_t((k + 1) % 3)], c = tri[std::size_t((k + 2) % 3)];
            const V ab = pointOf(m, b) - pointOf(m, a);
            // Thin, or straight by the rule build() cuts edges at corners with: a -> c -> b turning
            // less than that makes one straight edge of two sides, and the face has no width.
            const V u = pointOf(m, c) - pointOf(m, a), w = pointOf(m, b) - pointOf(m, c);
            const bool straight = u.Crossed(w).Modulus() <= 1e-6 * u.Modulus() * w.Modulus();
            if (longest <= 0 || (ab.Crossed(u).Modulus() / longest > thin && !straight))
                continue;
            const auto &across = sides[sideKey(a, b)];
            if (across.size() != 2)
                continue;
            const int o = across[0] == int(t) ? across[1] : across[0];
            if (touched[std::size_t(o)])
                continue;
            // The other triangle (u, v, x), its side u-v being a-b either way round (facing isn't
            // settled yet), becomes (u, c, x) and (c, v, x).
            auto other = m.triangles[std::size_t(o)];
            for (int turn = 0; turn < 3 && !((other[0] == a && other[1] == b) || (other[0] == b && other[1] == a)); ++turn)
                std::rotate(other.begin(), other.begin() + 1, other.end());
            if (!((other[0] == a && other[1] == b) || (other[0] == b && other[1] == a)))
                continue;
            gone[t] = gone[std::size_t(o)] = 1;
            touched[t] = touched[std::size_t(o)] = 1;
            added.push_back({other[0], c, other[2]});
            added.push_back({c, other[1], other[2]});
            if (labels)
                addedLabels.insert(addedLabels.end(), 2, (*labels)[std::size_t(o)]);
            ++fixed;
        }
        if (!fixed)
            return;
        std::vector<std::array<int, 3>> kept;
        std::vector<int> keptLabels;
        for (std::size_t t = 0; t < m.triangles.size(); ++t)
            if (!gone[t]) {
                kept.push_back(m.triangles[t]);
                if (labels)
                    keptLabels.push_back((*labels)[t]);
            }
        kept.insert(kept.end(), added.begin(), added.end());
        m.triangles = std::move(kept);
        if (labels) {
            keptLabels.insert(keptLabels.end(), addedLabels.begin(), addedLabels.end());
            *labels = std::move(keptLabels);
        }
    }
}

// The mesh's separate pieces, as lists of triangles. A piece lying inside another (the wall of a
// hollow) stays with the one around it.
std::vector<std::vector<int>> pieces(const Mesh &m) {
    std::vector<int> parent(m.points.size());
    std::iota(parent.begin(), parent.end(), 0);
    auto root = [&](int a) {
        while (parent[std::size_t(a)] != a)
            a = parent[std::size_t(a)] = parent[std::size_t(parent[std::size_t(a)])];
        return a;
    };
    for (const auto &t : m.triangles)
        for (int k = 1; k < 3; ++k)
            parent[std::size_t(root(t[std::size_t(k)]))] = root(t[0]);
    std::map<int, std::vector<int>> byRoot;
    for (std::size_t t = 0; t < m.triangles.size(); ++t)
        byRoot[root(m.triangles[t][0])].push_back(int(t));
    std::vector<std::vector<int>> out;
    for (auto &[r, list] : byRoot)
        out.push_back(std::move(list));
    if (out.size() < 2)
        return out;
    // Inside: a ray from one of its points crosses the other piece an odd number of times. The
    // ray's way is skewed so it doesn't run along the mesh's own lines.
    const V ray = V(0.5773, 0.5774, 0.5775) / V(0.5773, 0.5774, 0.5775).Modulus();
    auto inside = [&](const V &p, const std::vector<int> &piece) {
        int crossings = 0;
        for (const int t : piece) {
            const auto &tri = m.triangles[std::size_t(t)];
            const V a = pointOf(m, tri[0]), e1 = pointOf(m, tri[1]) - a, e2 = pointOf(m, tri[2]) - a;
            const V h = ray.Crossed(e2);
            const double det = e1.Dot(h);
            if (std::abs(det) < 1e-15)
                continue;
            const V s = p - a;
            const double u = s.Dot(h) / det;
            if (u < 0 || u > 1)
                continue;
            const V q = s.Crossed(e1);
            const double v = ray.Dot(q) / det;
            if (v < 0 || u + v > 1)
                continue;
            crossings += e2.Dot(q) / det > 0;
        }
        return crossings % 2 == 1;
    };
    std::vector<int> host(out.size(), -1);
    for (std::size_t i = 0; i < out.size(); ++i)
        for (std::size_t j = 0; j < out.size() && host[i] < 0; ++j)
            if (i != j && inside(pointOf(m, m.triangles[std::size_t(out[i][0])][0]), out[j]))
                host[i] = int(j);
    // Join each to the outermost piece around it.
    std::vector<std::vector<int>> joined;
    std::vector<int> slot(out.size(), -1);
    for (std::size_t i = 0; i < out.size(); ++i) {
        std::size_t top = i;
        for (int guard = 0; host[top] >= 0 && guard < int(out.size()); ++guard)
            top = std::size_t(host[top]);
        if (slot[top] < 0) {
            slot[top] = int(joined.size());
            joined.emplace_back();
        }
        auto &into = joined[std::size_t(slot[top])];
        into.insert(into.end(), out[i].begin(), out[i].end());
    }
    return joined;
}

// The triangles listed, as a mesh of their own.
Mesh subMesh(const Mesh &m, const std::vector<int> &tris) {
    Mesh out;
    std::unordered_map<int, int> to;
    for (const int t : tris) {
        std::array<int, 3> n{};
        for (int k = 0; k < 3; ++k) {
            const int p = m.triangles[std::size_t(t)][std::size_t(k)];
            const auto [it, added] = to.emplace(p, int(out.points.size()));
            if (added)
                out.points.push_back(m.points[std::size_t(p)]);
            n[std::size_t(k)] = it->second;
        }
        out.triangles.push_back(n);
    }
    return out;
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
    enum Kind { Flat, Cylinder, Cone, Sphere, Screw, Torus, Bowl } kind = Flat;
    V origin, axis;         // Flat: a point and the normal; round: a point on the axis (sphere: centre) and the axis
    double radius = 0;      // at origin
    double slope = 0;       // Cone: radius gained per mm along axis
    double tube = 0;        // Torus: the fillet's own radius; radius is its centre line's
    double facing = 0;      // Torus: which way round the tube the fillet faces, 0 straight out from the axis
    bool inward = false;    // the solid is on the surface's outer side: a hole
    double step = 0;        // Cylinder, Cone: the angle its facets step round the axis, if measured
    V seam;                 // round: where its wrap-around line goes, if set
    // Screw: one face of a thread's outline swept up a helix. The outline is (w, rho) points across
    // the thread, w always rising; pitch is mm per turn, negative for left-handed; low..high the height.
    double pitch = 0, low = 0, high = 0;
    std::vector<std::array<double, 2>> outline;
    // Bowl: a smooth cap no turned surface fits (spacer_mit_loch's dome, an elliptic paraboloid,
    // round 6), as a height over a plane: h = q0 + q1 u + q2 v + q3 u^2 + q4 u v + q5 v^2, u along
    // `across`, v along axis x across, h along axis, all from origin. box: the u, v range it's drawn
    // over. sag: how far a facet's chord may sit inside it (its points on the hollow side by up to that).
    std::array<double, 6> q{};
    V across;
    double box[4] = {0, 0, 0, 0};
    double sag = 0;
    // Flat: made one from pieces of the very same plane (fitMesh, round 7); kept where it is when
    // corners miss it, not refitted through them.
    bool joined = false;
};

// Where p sits over a bowl's plane (u, v), its signed height off the bowl along the bowl's normal
// (first order: exact at 0, which is all the solves need), and that normal.
void bowlAt(const Surface &s, const V &p, double &u, double &v, double &off, V &way) {
    const V d = p - s.origin, y = s.axis.Crossed(s.across);
    u = d.Dot(s.across);
    v = d.Dot(y);
    const auto &q = s.q;
    const double h = q[0] + q[1] * u + q[2] * v + q[3] * u * u + q[4] * u * v + q[5] * v * v;
    const double hu = q[1] + 2 * q[3] * u + q[4] * v, hv = q[2] + q[4] * u + 2 * q[5] * v;
    const V g = s.axis - s.across * hu - y * hv;
    const double n = g.Modulus();
    off = (d.Dot(s.axis) - h) / n;
    way = g / n;
}

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

// The outline a screw's spline is drawn over (GB-090): each end run on straight past the thread's own
// by kScrewMargin, so a face corner a hair beyond the outline (within the joining tolerance) still lies
// on the spline, not past its edge. A STEP reader re-lays the face's outline on the surface; off the
// edge the spline is extrapolated and folds, and b205d's thread flank read back with negative area.
// Never so far that one turn's outline reaches the next.
constexpr double kScrewMargin = 0.2;
Surface screwDrawn(const Surface &s) {
    Surface drawn = s;
    if (s.outline.size() < 2)
        return drawn;
    const double room = std::max(0.0, (std::abs(s.pitch) - (s.outline.back()[0] - s.outline.front()[0])) / 4);
    const auto past = [&](const std::array<double, 2> &end, const std::array<double, 2> &in) {
        const double dw = end[0] - in[0], dr = end[1] - in[1], length = std::max(std::hypot(dw, dr), 1e-12);
        double run = kScrewMargin;
        if (std::abs(dw) > 1e-12)
            run = std::min(run, room * length / std::abs(dw));
        return std::array<double, 2>{end[0] + dw / length * run, end[1] + dr / length * run};
    };
    drawn.outline.insert(drawn.outline.begin(), past(s.outline[0], s.outline[1]));
    drawn.outline.push_back(past(s.outline.back(), s.outline[s.outline.size() - 2]));
    return drawn;
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
    case Surface::Bowl: {
        double u, v, off;
        V way;
        bowlAt(s, p, u, v, off, way);
        return std::abs(off);
    }
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

// How far p is off a fitted surface, as the mesh it came from would put it: a turned surface's facets
// are chords, so a point on one (where another face crosses it, between the facets' corners) sits
// inside the curve by up to the chord's sag, and that is no miss. Outside, or past the sag, it is.
double fitOff(const Surface &s, const V &p) {
    if (s.kind == Surface::Bowl) {
        double u, v, off;
        V way;
        bowlAt(s, p, u, v, off, way);
        // The hollow side is the one its bend turns to: below a dome, above a dish.
        const double inside = (s.q[3] + s.q[5] < 0 ? -off : off);
        return inside <= 0 ? -inside : std::max(0.0, inside - s.sag);
    }
    if (s.step <= 0 || (s.kind != Surface::Cylinder && s.kind != Surface::Cone))
        return distanceTo(s, p);
    const V d = p - s.origin;
    const double h = d.Dot(s.axis), rho = (d - s.axis * h).Modulus();
    const double r = s.radius + (s.kind == Surface::Cone ? s.slope * h : 0);
    const double across = std::sqrt(1 + (s.kind == Surface::Cone ? s.slope * s.slope : 0));
    const double off = (rho - r) / across, sag = r * (1 - std::cos(s.step / 2)) / across;
    return off >= 0 ? off : std::max(0.0, -off - sag);
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
    case Surface::Bowl: {
        double u, v;
        bowlAt(s, p, u, v, off, way);
        return true;
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
        // A fillet ring with a flat on its top or bottom (square to its axis, a tube away) touches
        // it in one circle, the same way: there it stands for the upright wall through that circle.
        // The speaker mount's shoulder rim met its ribs off that circle, and the ring's faces
        // wouldn't mesh once the rim itself was laid on it.
        if (s->kind == Surface::Torus)
            for (const Surface *f : given)
                if (f->kind == Surface::Flat && std::abs(std::abs(f->axis.Dot(s->axis)) - 1) < 1e-12
                    && std::abs(std::abs((s->origin - f->origin).Dot(f->axis)) - s->tube) < 1e-9) {
                    Surface wall;
                    wall.kind = Surface::Cylinder;
                    wall.origin = s->origin;
                    wall.axis = s->axis;
                    wall.radius = s->radius;
                    stand.push_back(wall);
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
    if (s.kind == Surface::Bowl) {
        double u, v, off;
        V way;
        bowlAt(s, p, u, v, off, way);
        return way;
    }
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
void flatAreas(const Facets &f, double reachMm, std::vector<int> &labels, std::vector<Surface> &surfaces, bool lowPoly = false) {
    // Owner, 2026-10-30: under a degree apart is meant to be flat. Measured against the area's
    // first triangle, never just the neighbour, or a fine cylinder would creep all the way round;
    // and no point may sit further than this from it, so a big gentle curve stays a curve.
    // Low poly (round 4): kLowPolyDegrees, and points as far off as snapToSurfaces may move one
    // (max(0.06 mm, 5e-4 of the part)), so the joined facets are flattened onto one plane.
    const double offPlane = lowPoly ? std::max(0.06, reachMm * 5e-4) : std::max(0.02, reachMm * 1e-5);
    const double cosLimit = std::cos((lowPoly ? kLowPolyDegrees : 1.0) * M_PI / 180);
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
    // Its ends are its corners, where those were put (on all their surfaces), not the mesh points
    // they started from: the speaker mount's shoulder rim, laid on its exact circle, had its ribs'
    // rounds still ending 0.02 mm back at the mesh corners, and the ring between them wouldn't mesh.
    const gp_Pnt startAt = BRep_Tool::Pnt(vs), endAt = BRep_Tool::Pnt(ve);
    const auto corner = [&](std::size_t i) { return i == 0 ? startAt : i + 1 == pts.size() ? endAt : gp_Pnt(pointOf(m, pts[i])); };
    // Point to point: always true to them, if not smooth.
    const auto direct = [&]() {
        TColgp_Array1OfPnt poles(1, int(pts.size()));
        TColStd_Array1OfReal knots(1, int(pts.size()));
        TColStd_Array1OfInteger times(1, int(pts.size()));
        double along = 0;
        for (std::size_t i = 0; i < pts.size(); ++i) {
            if (i)
                along += std::max(corner(i).Distance(corner(i - 1)), 1e-12);
            poles.SetValue(int(i) + 1, corner(i));
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
        through.SetValue(int(i) + 1, corner(i * (pts.size() - 1) / (count - 1)));
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
    double band = 0;        // how far across the circle its points may lie: touching surfaces only
    // Each surface as the rings it holds: radius at a height, or height at a radius.
    const auto radiusAt = [&](const Surface &s, double h, double &rho) {
        if (s.kind == Surface::Cylinder)
            rho = s.radius;
        else if (s.kind == Surface::Cone)
            rho = s.radius + s.slope * h;
        else {
            // A flat on the very top of a fillet ring touches it in one circle; worked out, the
            // square comes a hair under zero (the speaker mount's shoulder: 0.4999995 against itself),
            // and the shoulder's rim was traced through mesh points instead, a circle of its own
            // between every two ribs.
            // And there the points lie on both surfaces within `near` anywhere across a band
            // sqrt(2 tube near) either side of that circle (1.5e-3 mm on the shoulder at 1e-6), so
            // they are held to the band, not to `near`.
            double across = s.tube * s.tube - h * h;
            if (across < 0) {
                if (std::abs(h) - s.tube > near)
                    return false;
                across = 0;
            }
            if (across < 8 * s.tube * near)
                band = 2 * std::sqrt(2 * s.tube * near);
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
        if (std::abs(along - h) > near || std::abs((d - r.axis * along).Modulus() - rho) > std::max(near, band))
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
        const bool whole = pts.front() == pts.back() || vs.IsSame(ve);
        if (whole)
            u1 = u0 + 2 * M_PI;
        while (u1 <= u0 + 1e-12)
            u1 += 2 * M_PI;
        while (mid < u0)
            mid += 2 * M_PI;
        // The arc the chain follows is about as long as the chain. Only near a half circle, where both
        // ways are, does the middle decide: a chain of two points 0.004 mm apart has its middle off the
        // corners' arc by more than the arc, and went the whole way round.
        double run = 0;
        for (std::size_t i = 1; i < pts.size(); ++i)
            run += (pointOf(m, pts[i]) - pointOf(m, pts[i - 1])).Modulus();
        const double ahead = (u1 - u0) * known->Radius(), back = 2 * M_PI * known->Radius() - ahead;
        const bool clear = std::abs(std::abs(ahead - run) - std::abs(back - run)) > 0.1 * std::max(run, 1e-9);
        if (whole || (clear ? std::abs(ahead - run) < std::abs(back - run) : mid <= u1)) {
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
    case Surface::Bowl: {
        // A height of second degree over a plane is exactly a spline of second degree each way,
        // one patch (a Bezier), its parameters the plane's own u and v: one true surface, no joints.
        // Its poles from three points each way: the ends, and twice the middle less half the ends.
        const V y = s.axis.Crossed(s.across);
        const double us[3] = {s.box[0], (s.box[0] + s.box[1]) / 2, s.box[1]}, vs[3] = {s.box[2], (s.box[2] + s.box[3]) / 2, s.box[3]};
        V grid[3][3];
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j) {
                const double u = us[i], v = vs[j];
                grid[i][j] = V(u, v, s.q[0] + s.q[1] * u + s.q[2] * v + s.q[3] * u * u + s.q[4] * u * v + s.q[5] * v * v);
            }
        for (int j = 0; j < 3; ++j)
            grid[1][j] = grid[1][j] * 2 - (grid[0][j] + grid[2][j]) / 2;
        for (int i = 0; i < 3; ++i)
            grid[i][1] = grid[i][1] * 2 - (grid[i][0] + grid[i][2]) / 2;
        TColgp_Array2OfPnt poles(1, 3, 1, 3);
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j)
                poles.SetValue(i + 1, j + 1, gp_Pnt(s.origin + s.across * grid[i][j].X() + y * grid[i][j].Y() + s.axis * grid[i][j].Z()));
        TColStd_Array1OfReal uKnots(1, 2), vKnots(1, 2);
        TColStd_Array1OfInteger ends(1, 2);
        uKnots(1) = s.box[0];
        uKnots(2) = s.box[1];
        vKnots(1) = s.box[2];
        vKnots(2) = s.box[3];
        ends(1) = ends(2) = 3;
        return new Geom_BSplineSurface(poles, uKnots, vKnots, ends, ends, 2, 2);
    }
    case Surface::Screw: {
        // Not a surface OpenCascade has: a spline laid over it, 24 steps a turn.
        const gp_Ax3 frame(gp_Pnt(s.origin), gp_Dir(s.axis));
        const V x = frame.XDirection().XYZ(), y = frame.YDirection().XYZ();
        double first, last;
        screwTurns(s, first, last);
        const int steps = std::max(8, int((last - first) * 24));
        // Rows evenly along the outline: unevenly spaced ones make the spline overshoot and fold.
        // Drawn over the outline run on a little past its ends (screwDrawn), as screwEdge lays edges on it;
        // as many rows as before, so the file is no heavier (2 more a point made b205d's 13,010 poles 19,518).
        const Surface drawn = screwDrawn(s);
        const int rows = std::max(4, 2 * int(s.outline.size()));
        TColgp_Array2OfPnt grid(1, steps + 1, 1, rows);
        for (int i = 0; i <= steps; ++i)
            for (int j = 0; j < rows; ++j) {
                const auto at = outlineAt(drawn, double(j) / (rows - 1));
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
    // A triangle that goes from its surface goes to a round beside it that it lies on (as close as a
    // round's own corner points are held below), not back to facets: a small round fitted to a few
    // triangles at a fillet's end left them as tiny flats where the fillet's wall runs on.
    const auto drop = [&](std::size_t t) {
        const int was = owner[t];
        owner[t] = -1;
        for (const int other : f.across[t]) {
            if (other < 0 || owner[std::size_t(other)] < 0 || owner[std::size_t(other)] == was)
                continue;
            const Surface &on = fitted[std::size_t(owner[std::size_t(other)])];
            bool lies = on.kind == Surface::Torus || on.kind == Surface::Cylinder || on.kind == Surface::Cone;
            for (const int p : m.triangles[t])
                lies = lies && distanceTo(on, pointOf(m, p)) <= 5e-3;
            if (lies) {
                owner[t] = owner[std::size_t(other)];
                side[t] = side[std::size_t(other)];
                return;
            }
        }
    };
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
                drop(t);
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
                    drop(std::size_t(list[i]));
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
            // A corner that has to go far to sit on them all, and then leaves the rest farther off
            // than the mesh point was, tries fewer and keeps the tightest: at the speaker mount's rib
            // foot, five surfaces crowding one corner put it 0.07 mm off on three, and a cut beside
            // it was refused for the 0.12 mm loose corner that made.
            std::vector<int> on = surfacesAt(p);
            std::sort(on.begin(), on.end(), [&](int a, int b) { return areaOf[std::size_t(a)] > areaOf[std::size_t(b)]; });
            double best = INFINITY;
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
                const std::set<int> these_(on.begin(), on.end());
                double off = (at - pointOf(m, p)).Modulus();
                for (const int l : surfacesAt(p))
                    if (!these_.count(l))
                        off = std::max(off, gapAt[p] + distanceTo(surfaces[std::size_t(l)], at));
                if (off >= best)
                    continue;
                best = off;
                meets[p] = at;
                honoured[p] = these_;
                moved[p] = off;
                if (off <= 3 * gapAt[p] + 1e-6)
                    break;
            }
        }
    const auto onBoth = [&](int p, const Chain &c) {
        const auto it = honoured.find(p);
        return it != honoured.end() && it->second.count(c.a) && it->second.count(c.b);
    };
    // The two halves of one whole round (cut in two to be built, merged at the end) meet where the
    // cut ran, down the wall. Its ends were each put on their surfaces on their own, so they can sit
    // a little round the wall from each other, and the line between them is then on neither half:
    // traced instead, it left hinge_female's hole with a crooked wrap-around line its merged face
    // couldn't be drawn with, and the bottom-face cut was refused as loose (round 6). The far end
    // is turned round the axis to the near one's turn, kept only where it stays on every surface it
    // was on, so the meeting line is the wall's own straight line.
    for (const Chain &c : pieces) {
        const Surface &sa = surfaces[std::size_t(c.a)], &sb = surfaces[std::size_t(c.b)];
        if (sa.kind != sb.kind || (sa.kind != Surface::Cylinder && sa.kind != Surface::Cone)
            || (sa.origin - sb.origin).Modulus() > 1e-12 || (sa.axis - sb.axis).Modulus() > 1e-12
            || sa.radius != sb.radius || sa.slope != sb.slope)
            continue;
        const int near = c.points.front(), far = c.points.back();
        if (near == far || !meets.count(near) || !meets.count(far) || !honoured.count(far))
            continue;
        const V axis = sa.axis / sa.axis.Modulus();
        const V a = meets.at(near) - sa.origin, b = meets.at(far) - sa.origin;
        const V ra = a - axis * a.Dot(axis), rb = b - axis * b.Dot(axis);
        if (ra.Modulus() < 1e-9 || rb.Modulus() < 1e-9)
            continue;
        const double turn = std::atan2(rb.Crossed(ra).Dot(axis), rb.Dot(ra));
        if (std::abs(turn) < 1e-15 || std::abs(turn) > 0.1)
            continue;
        const V turned = sa.origin + axis * b.Dot(axis) + rb * std::cos(turn) + axis.Crossed(rb) * std::sin(turn);
        bool stays = true;
        for (const int on : honoured.at(far))
            stays = stays && distanceTo(surfaces[std::size_t(on)], turned) < 1e-9;
        if (stays)
            meets[far] = turned;
    }
    // An edge is exact when it is where its two surfaces meet, end to end: a straight one between two
    // flats, or the circle two turned surfaces share.
    std::vector<char> exactPiece(pieces.size(), 0);
    std::vector<gp_Circ> circles(pieces.size());
    std::vector<double> worstOf(pieces.size(), 0);
    for (std::size_t i = 0; i < pieces.size(); ++i) {
        const Chain &c = pieces[i];
        // Its own two surfaces only: a corner may be loose for a third face meeting there, which
        // the corner's own room covers, not this edge's.
        for (const int p : c.points)
            worstOf[i] = std::max(worstOf[i], gap(p, c));
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
            // Between two halves of one round (the same surface either side) the cut ran down the
            // wall wherever the facets lay, a facet's width round from where its ends were put: the
            // straight line between the ends is on both all the same, so the chain need only stay
            // within a facet of it (hinge_female's hole, round 6).
            const Surface &sa = surfaces[std::size_t(c.a)], &sb = surfaces[std::size_t(c.b)];
            const bool halves = sa.kind == sb.kind && (sa.kind == Surface::Cylinder || sa.kind == Surface::Cone)
                                && (sa.origin - sb.origin).Modulus() <= 1e-12 && (sa.axis - sb.axis).Modulus() <= 1e-12
                                && sa.radius == sb.radius && sa.slope == sb.slope;
            const double room = halves ? std::max(worstOf[i] * kMargin, sa.radius * 0.5) : std::max(worstOf[i] * kMargin, kFloor);
            for (const int p : c.points) {
                const V d = pointOf(m, p) - from;
                on = on && (d - along * d.Dot(along)).Modulus() <= room;
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
        // Two rounds crossing (or a round and a bowl) share no circle: the edge is the line where they
        // meet. A smooth curve through the mesh's rim points alone swung 0.09 mm off the hole between
        // them on spacer_mit_loch's 16-sided hole (its points come in pairs 0.004 mm apart): the line
        // is traced on both surfaces instead, each point and three more between each two put on both,
        // kept only where it holds to both within a thousandth (rim 0.098 -> 0.00004 mm, round 7).
        const auto traced = [&]() -> TopoDS_Edge {
            const Surface &sa = surfaces[std::size_t(c.a)], &sb = surfaces[std::size_t(c.b)];
            const bool bent = (sa.kind != Surface::Flat && sa.kind != Surface::Screw) && (sb.kind != Surface::Flat && sb.kind != Surface::Screw);
            if (!round || exactPiece[i] || !bent || c.points.size() < 3 || c.points.front() == c.points.back())
                return {};
            const std::vector<const Surface *> both{&sa, &sb};
            Mesh line;
            const auto add = [&](const V &v) {
                if (line.points.empty() || (v - pointOf(line, int(line.points.size()) - 1)).Modulus() > 1e-3)
                    line.points.push_back({v.X(), v.Y(), v.Z()});
            };
            const V first = BRep_Tool::Pnt(vs).XYZ(), last = BRep_Tool::Pnt(ve).XYZ();
            add(first);
            for (std::size_t k = 1; k < c.points.size(); ++k) {
                const V a = k == 1 ? first : pointOf(m, c.points[k - 1]);
                const V b = k + 1 == c.points.size() ? last : pointOf(m, c.points[k]);
                for (int j = 1; j <= (k + 1 < c.points.size() ? 4 : 3); ++j) {
                    V at;
                    if (!meetingPoint(both, a + (b - a) * (j / 4.0), std::max((b - a).Modulus(), 0.2), at))
                        return {};
                    add(at);
                }
            }
            if ((last - pointOf(line, int(line.points.size()) - 1)).Modulus() <= 1e-3)
                line.points.pop_back();
            line.points.push_back({last.X(), last.Y(), last.Z()});
            std::vector<int> along(line.points.size());
            for (std::size_t k = 0; k < along.size(); ++k)
                along[k] = int(k);
            if (along.size() < 3)
                return {};
            const TopoDS_Edge e = splineThrough(line, along, vs, ve, 1e-4);
            BRepAdaptor_Curve curve(e);
            for (int k = 0; k <= 64; ++k) {
                const V at = curve.Value(curve.FirstParameter() + (curve.LastParameter() - curve.FirstParameter()) * k / 64).XYZ();
                if (distanceTo(sa, at) > 1e-3 || distanceTo(sb, at) > 1e-3)
                    return {};
            }
            return e;
        };
        try {
            edges[i] = traced();
            if (edges[i].IsNull())
                edges[i] = round && exactPiece[i] != 3 ? curved(m, c.points, vs, ve, std::min(tol, 0.003), exactPiece[i] ? &circles[i] : nullptr)
                                                       : BRepBuilderAPI_MakeEdge(vs, ve).Edge();
        } catch (const Standard_Failure &) {
            // No length (its ends meet): the faces either side are built from their triangles.
            bad.insert(c.a);
            bad.insert(c.b);
            continue;
        }
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

    if (!bad.empty())
        return {};
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
    const Surface drawn = screwDrawn(s);
    std::vector<gp_Pnt2d> uv;
    std::vector<double> at;
    for (int i = 0; i <= kSamples; ++i) {
        const double param = c0 + (c1 - c0) * i / kSamples;
        const V p = curve->Value(param).XYZ();
        double w, rho;
        onScrew(s, p, w, rho);
        double position;
        std::size_t piece;
        nearOutline(drawn, w, rho, position, piece);
        const double along = outlineShare(drawn, position);
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

// Smallest-spread direction of a set of vectors: of unit normals, the axis they turn round; of
// points, the normal of the plane nearest them (`mean` its offset).
// Set while a Low Res fit runs (round 5): the two-facet axis below.
thread_local bool tLowRes = false;
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
    // Normals in only two directions (a round of two facets) don't spread along their own mean
    // either: the axis is then the still direction square to the mean, across the one they spread on.
    int largest = 1;
    for (int i = 2; i <= 3; ++i)
        if (jacobi.Value(i) > jacobi.Value(largest))
            largest = i;
    const int middle = 6 - smallest - largest;
    if (tLowRes && largest != smallest && jacobi.Value(middle) < 1e-6 * jacobi.Value(largest)) {
        math_Vector w(1, 3);
        jacobi.Vector(largest, w);
        const V across = V(w(1), w(2), w(3)).Crossed(centre);
        if (across.Modulus() > 1e-9)
            axis = across;
    }
    axis /= std::max(axis.Modulus(), 1e-12);
    if (mean)
        *mean = centre.Dot(axis);
    return axis;
}

// Everything in one pass, re-splitting pinched flats until it builds.
// Every point onto its surfaces (owner, 2026-11-10: a face that meets another must BE that
// surface, no gap). A point is moved to where the surfaces of the triangles round it truly meet: a
// corner of three flats to their one common point, a point on a fold to the line of it, any other
// to its own surface. Where they don't meet close by (a flat that is only nearly flat, a fit that
// is only near), the smallest of them gives way, and every triangle of it at that point becomes a
// flat of its own through its moved corners, which is exact by itself. So no edge or corner is
// loose by more than a curve's chord. Returns the moved mesh; labels and surfaces grow to match.
// Owner, 2026-11-16 (GB-090): no slivers. A face under kSliverArea, or narrower than kSliverWidth
// (twice its area over its outline's length: a strip's width), is a sliver. b205d left 27, single
// facets at the bore rims, 0.0065 mm wide; Fusion's cut and fillet choke on them.
constexpr double kSliverArea = 0.01, kSliverWidth = 0.06;
// How far a thin run's corners may sit off the surface it is given to: about the fit's own cap; what is
// still thin after those moves goes to its nearest face up to kSliverLastMiss.
constexpr double kSliverMiss = 0.06, kSliverLastMiss = 0.25;

bool sliver(double area, double outline) {
    return area < kSliverArea || 2 * area / std::max(outline, 1e-12) < kSliverWidth;
}

// thinFlats: whether a triangle whose corners won't sit on its surface may be a flat of its own however
// thin (a plain import keeps every facet); a fit gives a thin one to the face beside it instead.
Mesh snapToSurfaces(const Mesh &in, std::vector<int> &labels, std::vector<Surface> &surfaces, bool thinFlats = true) {
    Mesh m = in;
    const double size = reach(in);
    std::vector<double> areaOf(surfaces.size(), 0);
    std::vector<std::vector<int>> around(in.points.size());
    std::vector<double> shortest(in.points.size(), INFINITY);
    for (std::size_t t = 0; t < in.triangles.size(); ++t) {
        const auto &tri = in.triangles[t];
        const V a = pointOf(in, tri[0]), b = pointOf(in, tri[1]), c = pointOf(in, tri[2]);
        if (labels[t] >= 0)
            areaOf[std::size_t(labels[t])] += (b - a).Crossed(c - a).Modulus() / 2;
        for (int k = 0; k < 3; ++k) {
            const int p = tri[std::size_t(k)];
            around[std::size_t(p)].push_back(int(t));
            shortest[std::size_t(p)] = std::min(shortest[std::size_t(p)],
                                                (pointOf(in, tri[std::size_t((k + 1) % 3)]) - pointOf(in, p)).Modulus());
            shortest[std::size_t(p)] = std::min(shortest[std::size_t(p)],
                                                (pointOf(in, tri[std::size_t((k + 2) % 3)]) - pointOf(in, p)).Modulus());
        }
    }
    // Where three surfaces meet in a corner, a mesh often leaves one triangle across it, each of its
    // sides on a different one of them (spacer_mit_loch: where the hole, the front wall and the dome
    // meet, a 0.34 mm2 triangle each side, tilted between them). Left a flat of its own it stays a face
    // and its corners never come together. It belongs to the curved surface it faces most nearly
    // (within 45 degrees): taken into it, its corners want all three surfaces and become the one point
    // where they meet (round 7).
    {
        std::vector<int> count(surfaces.size(), 0);
        for (std::size_t t = 0; t < in.triangles.size(); ++t)
            if (labels[t] >= 0)
                ++count[std::size_t(labels[t])];
        std::map<std::pair<int, int>, std::vector<int>> bySide;
        for (std::size_t t = 0; t < in.triangles.size(); ++t)
            for (int k = 0; k < 3; ++k) {
                const int a = in.triangles[t][std::size_t(k)], b = in.triangles[t][std::size_t((k + 1) % 3)];
                bySide[{std::min(a, b), std::max(a, b)}].push_back(int(t));
            }
        for (std::size_t t = 0; t < in.triangles.size(); ++t) {
            const int own = labels[t];
            if (own < 0 || surfaces[std::size_t(own)].kind != Surface::Flat || count[std::size_t(own)] != 1)
                continue;
            const auto &tri = in.triangles[t];
            std::set<int> beside;
            bool closed = true;
            for (int k = 0; k < 3; ++k) {
                const int a = tri[std::size_t(k)], b = tri[std::size_t((k + 1) % 3)];
                const auto &on = bySide[{std::min(a, b), std::max(a, b)}];
                int other = -1;
                for (const int u : on)
                    if (u != int(t))
                        other = labels[std::size_t(u)];
                closed = closed && on.size() == 2 && other >= 0 && other != own;
                beside.insert(other);
            }
            if (!closed || beside.size() != 3)
                continue;
            const V a = pointOf(in, tri[0]), b = pointOf(in, tri[1]), c = pointOf(in, tri[2]);
            V n = (b - a).Crossed(c - a);
            if (n.Modulus() < 1e-18)
                continue;
            n /= n.Modulus();
            const V middle = (a + b + c) / 3;
            // It must face that one surface only: a facet facing two of its neighbours (lying along a
            // flat and a round alike) is the mesh's own step between them, not a corner filler.
            int best = -1, facing = 0;
            for (const int l : beside) {
                const Surface &round = surfaces[std::size_t(l)];
                if (std::abs(normalAt(round, middle).Dot(n)) < std::cos(M_PI / 4))
                    continue;
                ++facing;
                if (round.kind != Surface::Flat && round.kind != Surface::Screw)
                    best = l;
            }
            if (best < 0 || facing != 1)
                continue;
            const double area = (b - a).Crossed(c - a).Modulus() / 2;
            areaOf[std::size_t(own)] -= area;
            areaOf[std::size_t(best)] += area;
            --count[std::size_t(own)];
            labels[t] = best;
        }
    }
    // wanted[p]: the labels round point p, biggest first; honoured[p]: those it ended up exactly on.
    std::vector<std::vector<int>> wanted(in.points.size()), honoured(in.points.size());
    std::vector<std::array<double, 3>> startAt;
    for (std::size_t p = 0; p < in.points.size(); ++p) {
        std::vector<int> &on = wanted[p];
        for (const int t : around[p])
            if (labels[std::size_t(t)] >= 0)
                on.push_back(labels[std::size_t(t)]);
        std::sort(on.begin(), on.end());
        on.erase(std::unique(on.begin(), on.end()), on.end());
        std::sort(on.begin(), on.end(), [&](int a, int b) { return areaOf[std::size_t(a)] > areaOf[std::size_t(b)]; });
    }
    // A side running mostly across a flat both its ends are on (a step riser being flattened onto it)
    // shrinks to nothing as they land, so it doesn't bound how far they move: the other sides do.
    // spacer_mit_loch's 0.003 mm front-wall ledge, part of the wall, held every corner along it to
    // 0.001 mm and they all missed the wall (round 7).
    {
        std::vector<double> other(in.points.size(), INFINITY);
        std::vector<char> riser(in.points.size(), 0);
        for (const auto &tri : in.triangles)
            for (int k = 0; k < 3; ++k) {
                const int p = tri[std::size_t(k)];
                for (const int j : {1, 2}) {
                    const int q = tri[std::size_t((k + j) % 3)];
                    const V side = pointOf(in, q) - pointOf(in, p);
                    bool across = false;
                    for (const int l : wanted[std::size_t(p)])
                        if (surfaces[std::size_t(l)].kind == Surface::Flat
                            && std::find(wanted[std::size_t(q)].begin(), wanted[std::size_t(q)].end(), l) != wanted[std::size_t(q)].end()
                            && std::abs(side.Dot(surfaces[std::size_t(l)].axis)) >= 0.7 * side.Modulus())
                            across = true;
                    if (across)
                        riser[std::size_t(p)] = 1;
                    else
                        other[std::size_t(p)] = std::min(other[std::size_t(p)], side.Modulus());
                }
            }
        for (std::size_t p = 0; p < in.points.size(); ++p)
            if (riser[p] && std::isfinite(other[p]))
                shortest[p] = other[p];
    }
    // Onto as many of its surfaces as meet near where it was, the smallest giving way first. A thread
    // has no formula to solve on: it keeps the room it had, and isn't moved to.
    const auto place = [&](std::size_t p) {
        std::vector<int> on;
        honoured[p].clear();
        for (const int l : wanted[p])
            (surfaces[std::size_t(l)].kind == Surface::Screw ? honoured[p] : on).push_back(l);
        // As far as a point may move: never across its own triangles, never past how loose a
        // flat or a fit is allowed to be.
        // A point where a face crosses a coarse round one sits on a facet's chord, inside it by up to
        // the sag (0.1 mm in a 16-sided 5 mm hole), and that far it may come out.
        // Kept on a flat as well, it gets there by sliding along the flat: sag / sin of the angle
        // between them (spacer_mit_loch's front wall under its dome: 0.078 mm in at 45 degrees
        // needs 0.11; held to the sag, the wall's corners missed it, the wall was refitted through
        // corners moved onto the dome, tilted 0.04 mm and broke into ~30 flats, round 7).
        // Inside by its sag, give or take what a file written to 3 decimals rounds a point by (up to
        // 0.001 mm): a corner where a wall crosses a facet's very middle is the whole sag in, and
        // 0.0001 mm of fit decided whether it could move at all (spacer_mit_loch's hole/wall corners).
        const auto within = [&](const Surface &round, const V &at) { return fitOff(round, at) <= 1e-3; };
        double sag = 0;
        for (const int l : on) {
            const Surface &round = surfaces[std::size_t(l)];
            if (round.step <= 0 && round.kind != Surface::Bowl)
                continue;
            const V at = pointOf(in, int(p));
            const double inside = within(round, at) ? distanceTo(round, at) : 0.0;
            double slant = 1, off = 0;
            for (const int k : on)
                if (surfaces[std::size_t(k)].kind == Surface::Flat) {
                    if (const double sine = normalAt(round, at).Crossed(surfaces[std::size_t(k)].axis).Modulus(); sine >= 0.3)
                        slant = std::min(slant, sine);
                    off = std::max(off, distanceTo(surfaces[std::size_t(k)], at));
                }
            // Plus however far off the flat it starts (a wall whose two halves were made one plane
            // 0.003 mm apart, round 7), within what a flat is fitted to anyway.
            sag = std::max(sag, inside / slant + std::min(off, std::max(0.02, size * 2e-4) * 1.5));
        }
        // Two coarse rounds crossing (spacer_mit_loch's 16-sided hole coming up through its dome):
        // the rim point is inside both, by each one's sag, and reaching the line where they meet
        // takes the two gaps combined across the angle between them, sqrt(a2 + b2 + 2ab|cos|) / sin
        // (0.07 and 0.096 mm in: 0.12, where either alone allowed 0.096 and most rim points stayed
        // on the dome only, round 7).
        // And a corner on three surfaces, at least one a coarse round it sits inside (where a hole, a
        // wall and a dome meet): the move that lands on all three at once, from their normals and gaps
        // (each gap either way, the worst taken). The mesh's corner there sits down the wall-hole line
        // from the true one (spacer_mit_loch: 0.04 mm off the dome, 0.094 inside the hole, allowed
        // 0.113, round 7).
        if (on.size() >= 3) {
            const V at = pointOf(in, int(p));
            bool inside = false;
            for (std::size_t i = 0; i < 3; ++i) {
                const Surface &r = surfaces[std::size_t(on[i])];
                inside = inside || ((r.step > 0 || r.kind == Surface::Bowl) && within(r, at) && distanceTo(r, at) > 0);
            }
            const V n0 = normalAt(surfaces[std::size_t(on[0])], at), n1 = normalAt(surfaces[std::size_t(on[1])], at), n2 = normalAt(surfaces[std::size_t(on[2])], at);
            const double det = n0.Dot(n1.Crossed(n2));
            if (inside && std::abs(det) >= 0.1) {
                const double g[3] = {distanceTo(surfaces[std::size_t(on[0])], at), distanceTo(surfaces[std::size_t(on[1])], at), distanceTo(surfaces[std::size_t(on[2])], at)};
                double need = 0;
                for (int signs = 0; signs < 8; ++signs) {
                    const double g0 = signs & 1 ? -g[0] : g[0], g1 = signs & 2 ? -g[1] : g[1], g2 = signs & 4 ? -g[2] : g[2];
                    // d with n_i . d = g_i (Cramer's rule)
                    const V d = (n1.Crossed(n2) * g0 + n2.Crossed(n0) * g1 + n0.Crossed(n1) * g2) / det;
                    need = std::max(need, d.Modulus());
                }
                sag = std::max(sag, need);
            }
        }
        for (std::size_t i = 0; i < on.size(); ++i)
            for (std::size_t j = i + 1; j < on.size(); ++j) {
                const Surface &a = surfaces[std::size_t(on[i])], &b = surfaces[std::size_t(on[j])];
                if ((a.step <= 0 && a.kind != Surface::Bowl) || (b.step <= 0 && b.kind != Surface::Bowl))
                    continue;
                const V at = pointOf(in, int(p));
                const V na = normalAt(a, at), nb = normalAt(b, at);
                const double sine = na.Crossed(nb).Modulus();
                if (sine < 0.3)
                    continue;
                const double da = within(a, at) ? distanceTo(a, at) : 0.0, db = within(b, at) ? distanceTo(b, at) : 0.0;
                sag = std::max(sag, std::sqrt(da * da + db * db + 2 * da * db * std::abs(na.Dot(nb))) / sine);
            }
        // A round wall touching a flat (a hinge knuckle on its leaf) meets it along a line, and a
        // facet corner a gap off that line slides sqrt(2 r gap) along to it, not gap: hinge_female's
        // stayed 0.23 mm short at a 0.06 mm limit and its edge was 0.0055 mm loose, which left cuts
        // there empty (round 6). The same test as meetingPoint's stand-in.
        double touch = 0;
        for (const int l : on) {
            const Surface &round = surfaces[std::size_t(l)];
            if (round.kind != Surface::Cylinder)
                continue;
            for (const int k : on) {
                const Surface &flat = surfaces[std::size_t(k)];
                // A real face, not one of the round's own facets left flat beside it (hinge_male's
                // knuckle: 1-2 triangle facets lay exactly a radius off, and sliding their corners
                // loosened the knuckle's edges to 0.0076 mm): at least the radius squared in area.
                if (flat.kind == Surface::Flat && std::abs(flat.axis.Dot(round.axis)) < 1e-12
                    && std::abs(std::abs((round.origin - flat.origin).Dot(flat.axis)) - round.radius) < 1e-9
                    && areaOf[std::size_t(k)] >= round.radius * round.radius)
                    touch = std::max(touch, std::sqrt(2 * round.radius * (distanceTo(round, pointOf(in, int(p))) + distanceTo(flat, pointOf(in, int(p))))) * 1.01);
            }
        }
        const double most = std::min(0.3 * shortest[p], std::max({0.06, size * 5e-4, sag * 1.01}));
        // Past that, up to 0.6 of the shortest side, a move is still taken when it runs across the
        // point's sides rather than along them: along no side more than 0.3 of it, so it can't fold a
        // triangle (spacer_mit_loch's apex corner slides 0.17 mm down the line where its hole and wall
        // meet, square to its 0.375 mm rim side, round 7).
        const double further = std::max({most, std::min(0.6 * shortest[p], sag * 1.01), touch});
        // That slide runs along the flat and only narrows the round's own strip facet, so it is
        // taken whole, however short that facet's side, as long as none of the point's triangles
        // turns over (the speaker mount's key grooves: a 0.043 mm slide across a 0.05 mm side, where
        // 0.6 of it left the notch wall missing its corner, round 7).
        const auto acrossSides = [&](const V &to) {
            const V from = pointOf(in, int(p)), move = to - from;
            if (move.Modulus() <= most)
                return true;
            if (move.Modulus() <= touch) {
                bool folds = false;
                for (const int t : around[p]) {
                    const auto &tri = in.triangles[std::size_t(t)];
                    V a[3], b[3];
                    for (int k = 0; k < 3; ++k)
                        a[k] = b[k] = pointOf(in, tri[std::size_t(k)]);
                    for (int k = 0; k < 3; ++k)
                        if (tri[std::size_t(k)] == int(p))
                            b[k] = to;
                    folds = folds || (a[1] - a[0]).Crossed(a[2] - a[0]).Dot((b[1] - b[0]).Crossed(b[2] - b[0])) <= 0;
                }
                if (!folds)
                    return true;
            }
            for (const int t : around[p])
                for (const int q : in.triangles[std::size_t(t)]) {
                    if (q == int(p))
                        continue;
                    const V side = pointOf(in, q) - from;
                    if (move.Dot(side) / side.Modulus() > 0.3 * side.Modulus())
                        return false;
                }
            return true;
        };
        m.points[p] = in.points[p];
        for (; !on.empty(); on.pop_back()) {
            std::vector<const Surface *> these;
            for (const int l : on)
                these.push_back(&surfaces[std::size_t(l)]);
            V at;
            if (meetingPoint(these, pointOf(in, int(p)), further, at) && acrossSides(at)) {
                m.points[p] = {at.X(), at.Y(), at.Z()};
                honoured[p].insert(honoured[p].end(), on.begin(), on.end());
                break;
            }
        }
        std::sort(honoured[p].begin(), honoured[p].end());
    };
    for (std::size_t p = 0; p < in.points.size(); ++p)
        place(p);
    // Four flats at a corner, each fitted to a few float points 1e-5 mm out of true, never meet in one
    // point, and one would be cut into single triangles. Near the mesh there is a solid whose flats do
    // meet at every corner (it has more freedom than ties): a flat a corner missed is refitted through
    // its corners as they now are, its corners placed again, until none is missed.
    {
        std::vector<std::vector<int>> pointsOf(surfaces.size());
        for (std::size_t p = 0; p < in.points.size(); ++p)
            for (const int l : wanted[p])
                pointsOf[std::size_t(l)].push_back(int(p));
        // Each round settles some; a dozen or so is usual, it stops when none is missed. Flats grown
        // over a gentle curve never settle: the work is held to twice the points, and 20,000 more for
        // small parts (the speaker mount's plain import went 0.4 -> 7.5 s on 64 full rounds).
        // A round that settles under a tenth of a thousand or more missed flats is a stall (the speaker's
        // thread facets: 6950, 5528, 5436, 5290 ... missed, its plain import 0.4 -> 1.0 s), and the rounds
        // stop there. Small parts settle slowly but do settle (a lid's last few flats), so they go on.
        std::size_t budget = in.points.size() * 2 + 20000;
        std::size_t missedBefore = 0;
        for (int round = 0; round < 64; ++round) {
            std::set<int> missed;
            for (std::size_t p = 0; p < in.points.size(); ++p)
                for (const int l : wanted[p])
                    if (surfaces[std::size_t(l)].kind == Surface::Flat && !std::binary_search(honoured[p].begin(), honoured[p].end(), l))
                        missed.insert(l);
            if (missed.empty() || (round > 0 && missedBefore >= 1000 && missed.size() * 10 > missedBefore * 9))
                break;
            missedBefore = missed.size();
            // Refitted only if all their corners can be placed again within what's left.
            std::vector<std::pair<int, Surface>> refit;
            std::set<int> again;
            for (const int l : missed) {
                std::vector<V> corners;
                for (const int p : pointsOf[std::size_t(l)])
                    corners.push_back(pointOf(m, p));
                double offset = 0;
                V normal = leastSpread(corners, std::vector<double>(corners.size(), 1.0), &offset);
                Surface flat = surfaces[std::size_t(l)];
                if (normal.Dot(flat.axis) < 0) {
                    normal *= -1;
                    offset = -offset;
                }
                if (normal.Dot(flat.axis) < std::cos(M_PI / 180))
                    continue;    // not a flat any more: leave it to be split
                // A flat made one from pieces of the very same plane stays where that put it. Refitted
                // through corners already moved onto the curves beside it, spacer_mit_loch's front wall
                // leaned 0.25 degrees and broke up; refitted square but through those corners' mean, it
                // wandered 0.003-0.006 mm round to round and a corner cluster never caught it (round 7).
                // (Held for every flat square to the axes, the 40 mm box went 885 -> 905 faces.)
                if (flat.joined)
                    continue;
                flat.axis = normal;
                flat.origin = normal * offset;
                refit.push_back({l, flat});
                again.insert(pointsOf[std::size_t(l)].begin(), pointsOf[std::size_t(l)].end());
            }
            if (again.empty() || again.size() > budget)
                break;
            budget -= again.size();
            for (const auto &[l, flat] : refit)
                surfaces[std::size_t(l)] = flat;
            for (const int p : again)
                place(std::size_t(p));
        }
    }
    // Points moved onto one spot become one; what each was on, and where it started, go with it.
    // Then, as for a file: triangles doubled by that go, and a point moved onto the line between
    // two others leaves a sliver, which goes, the triangle across keeping its label in both halves.
    std::vector<int> to;
    joinSame(m, std::max(1e-9, size * 1e-12), &labels, &to);
    {
        std::vector<std::vector<int>> on(m.points.size());
        std::vector<std::array<double, 3>> start(m.points.size());
        for (std::size_t p = 0; p < to.size(); ++p) {
            auto &into = on[std::size_t(to[p])];
            into.insert(into.end(), honoured[p].begin(), honoured[p].end());
            start[std::size_t(to[p])] = in.points[p];
        }
        for (auto &list : on) {
            std::sort(list.begin(), list.end());
            list.erase(std::unique(list.begin(), list.end()), list.end());
        }
        honoured = std::move(on);
        startAt = std::move(start);
    }
    dropDoubles(m, &labels);
    dropSlivers(m, size, &labels);
    // A triangle with a corner off its surface, or turned over by the moves, is a flat of its own,
    // unless it is thinner than a flat's fit allowance and a round holds all three of its corners:
    // then it is a sliver of that round's (a key groove's top under its fillet ring: two such flats
    // left a 0.004 mm island in Project's outline, round 7).
    const auto ownFlat = [&](std::size_t t) {
        const auto &tri = m.triangles[t];
        const V a = pointOf(m, tri[0]), b = pointOf(m, tri[1]), c = pointOf(m, tri[2]);
        const V now = (b - a).Crossed(c - a);
        const auto &q0 = startAt[std::size_t(tri[0])], &q1 = startAt[std::size_t(tri[1])], &q2 = startAt[std::size_t(tri[2])];
        const V was = (V(q1[0], q1[1], q1[2]) - V(q0[0], q0[1], q0[2])).Crossed(V(q2[0], q2[1], q2[2]) - V(q0[0], q0[1], q0[2]));
        Surface own;
        own.kind = Surface::Flat;
        own.axis = now.Modulus() > 1e-300 ? now / now.Modulus() : was / std::max(was.Modulus(), 1e-300);
        own.origin = (a + b + c) / 3;
        labels[t] = int(surfaces.size());
        surfaces.push_back(own);
    };
    std::vector<std::size_t> thin;
    for (std::size_t t = 0; t < m.triangles.size(); ++t) {
        const auto &tri = m.triangles[t];
        bool held = labels[t] >= 0;
        // A round surface keeps its triangles where it gives way (a fillet only near tangent to
        // its flat meets it nowhere exactly); that edge keeps the room it needs, as before.
        const bool round = held && surfaces[std::size_t(labels[t])].kind != Surface::Flat;
        for (const int p : tri)
            held = held && (round || std::binary_search(honoured[std::size_t(p)].begin(), honoured[std::size_t(p)].end(), labels[t]));
        const V a = pointOf(m, tri[0]), b = pointOf(m, tri[1]), c = pointOf(m, tri[2]);
        auto startOf = [&](int p) { const auto &q = startAt[std::size_t(p)]; return V(q[0], q[1], q[2]); };
        const V was = (startOf(tri[1]) - startOf(tri[0])).Crossed(startOf(tri[2]) - startOf(tri[0]));
        const V now = (b - a).Crossed(c - a);
        if (now.Dot(was) <= 0 || now.Modulus() < 1e-300)
            held = false;
        if (held)
            continue;
        int holder = -1;
        for (const int l : honoured[std::size_t(tri[0])])
            if (surfaces[std::size_t(l)].kind != Surface::Flat && now.Dot(was) > 0
                && now.Modulus() <= std::max(0.02, size * 2e-4) * 1.5 * std::max({(b - a).Modulus(), (c - b).Modulus(), (a - c).Modulus()})
                && std::binary_search(honoured[std::size_t(tri[1])].begin(), honoured[std::size_t(tri[1])].end(), l)
                && std::binary_search(honoured[std::size_t(tri[2])].begin(), honoured[std::size_t(tri[2])].end(), l))
                holder = l;
        if (holder >= 0) {
            labels[t] = holder;
            continue;
        }
        if (!thinFlats && sliver(now.Modulus() / 2, (b - a).Modulus() + (c - b).Modulus() + (a - c).Modulus())) {
            thin.push_back(t);
            continue;
        }
        ownFlat(t);
    }
    // A fit's thin one goes to the face beside it whose surface its corners lie closest to (GB-090):
    // left a flat of its own, b205d's bore rims kept single facets 0.016-0.054 mm wide. Its far corner
    // stays where it was placed, off that surface by up to kSliverLastMiss: b205d's worst corner there
    // is 0.18 mm loose by OpenCascade's count (0.076 mm once a STEP file is read back), against 0.078.
    if (!thin.empty()) {
        const Facets beside(m);
        std::vector<char> waiting(m.triangles.size(), 0);
        for (const std::size_t t : thin)
            waiting[t] = 1;
        for (bool moved = true; moved;) {
            moved = false;
            for (const std::size_t t : thin) {
                if (!waiting[t])
                    continue;
                double best = kSliverLastMiss;
                int to = -1;
                for (const int other : beside.across[t]) {
                    if (other < 0 || waiting[std::size_t(other)] || labels[std::size_t(other)] < 0)
                        continue;
                    double miss = 0;
                    for (const int p : m.triangles[t])
                        miss = std::max(miss, distanceTo(surfaces[std::size_t(labels[std::size_t(other)])], pointOf(m, p)));
                    if (miss <= best) {
                        best = miss;
                        to = labels[std::size_t(other)];
                    }
                }
                if (to >= 0) {
                    labels[t] = to;
                    waiting[t] = 0;
                    moved = true;
                }
            }
        }
        for (const std::size_t t : thin)
            if (waiting[t])
                ownFlat(t);
    }
    return m;
}

TopoDS_Shape assemble(const Mesh &given, std::vector<int> labels, std::vector<Surface> surfaces, double tol, const Stage &stage,
                      std::vector<std::pair<TopoDS_Face, int>> *made = nullptr, bool thinFlats = true) {
    stage("Putting corners on their surfaces");
    const Mesh m = snapToSurfaces(given, labels, surfaces, thinFlats);
    const Facets f(m);
    TopoDS_Shape shell;
    for (int round = 0; round < 6; ++round) {
        std::set<int> bad;
        if (made)
            made->clear();
        shell = build(m, labels, surfaces, tol, bad, stage, made);
        if (bad.empty())
            break;
        // Split pinched areas: each of their triangles its own face, on the area's own plane when it
        // was a flat (so the merge afterwards joins what it can build as one face again: split onto
        // their own triangles' planes, a flat's pieces stayed as seams 1e-7 mm apart, owner 2026-11-11).
        for (std::size_t t = 0; t < labels.size(); ++t)
            if (bad.count(labels[t])) {
                Surface s;
                const Surface &was = surfaces[std::size_t(labels[t])];
                if (was.kind == Surface::Flat && f.normal[t].Dot(was.axis) > 0) {
                    s = was;
                } else {
                    s.axis = f.normal[t];
                    s.origin = pointOf(m, m.triangles[t][0]);
                }
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

// A turned surface's facets step round its axis evenly, and there are at least two of them: an exporter cuts one curve at one angle.
// Flat walls whose corners happen to sit on one circle (a square pocket with cut corners) step
// unevenly, 15 degrees then 27, and a curve through their corners bows into the wall.
bool evenSteps(const Facets &f, const std::vector<int> &group, const Surface &s, double *usualStep = nullptr) {
    const V a = s.axis;
    V u = a.Crossed(std::abs(a.X()) < 0.9 ? V(1, 0, 0) : V(0, 1, 0));
    u /= std::max(u.Modulus(), 1e-12);
    const V v = a.Crossed(u);
    const auto angle = [&](const V &d) { return std::atan2(d.Dot(v), d.Dot(u)); };
    // One facet is every triangle facing one way round the axis; its step is the angle its corners cover.
    std::vector<std::pair<double, int>> walls;    // facing, triangle
    for (const int t : group) {
        V n = f.normal[std::size_t(t)];
        // Facing out from the axis: a boss and a hole on one circle (a hinge knuckle over the cut its
        // partner turns in) are the same steps, not facets half a turn apart facing one way.
        const auto &tri = f.mesh.triangles[std::size_t(t)];
        if (n.Dot((pointOf(f.mesh, tri[0]) + pointOf(f.mesh, tri[1]) + pointOf(f.mesh, tri[2])) / 3 - s.origin) < 0)
            n *= -1;
        if (std::abs(n.Dot(a)) <= 0.7)    // more is an end, not the wall
            walls.push_back({angle(n - a * n.Dot(a)), t});
    }
    std::sort(walls.begin(), walls.end());
    std::vector<double> steps;
    for (std::size_t i = 0; i < walls.size();) {
        const auto around = [&](int p) {
            const V d = pointOf(f.mesh, p) - s.origin;
            return angle(d - a * d.Dot(a));
        };
        const double first = around(f.mesh.triangles[std::size_t(walls[i].second)][0]);
        double lo = 0, hi = 0;
        std::size_t j = i;
        for (; j < walls.size() && walls[j].first - walls[i].first < 0.2 * M_PI / 180; ++j)
            for (const int p : f.mesh.triangles[std::size_t(walls[j].second)]) {
                const double from = std::remainder(around(p) - first, 2 * M_PI);
                lo = std::min(lo, from);
                hi = std::max(hi, from);
            }
        if (hi - lo >= 0.5 * M_PI / 180)
            steps.push_back(hi - lo);    // less is a sliver along a ruling
        i = j;
    }
    // One facet doesn't turn at all: it's a flat wall, whatever circle its corners are on. Where a
    // curve ends on a flat its last step is cut short; a step longer than the usual one is a wall.
    if (steps.size() < 2)
        return false;
    std::sort(steps.begin(), steps.end());
    const double usual = steps[(steps.size() - 1) / 2];
    if (usualStep)
        *usualStep = usual;
    return steps.back() <= usual * 1.6 + 1e-9 || steps.back() < 3 * M_PI / 180;
}

// A round surface through a group of triangles, from where their normals point: the simplest
// that holds, cylinder (normals square to one axis), cone (at a set angle to it), then sphere
// (normals meeting at one point; points on two circles alone fit one too, so it comes last and
// needs normals that really spread in every direction). false if none is any good.
bool circleOf(const std::vector<std::array<double, 2>> &p, std::size_t from, std::size_t to, double &cu, double &cv, double &r, double &worst);

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
    auto consider = [&](Surface s) {
        if (found)
            return;
        if ((s.kind == Surface::Cylinder || s.kind == Surface::Cone) && !evenSteps(f, group, s, &s.step)) {
            return;
        }
        double worst = 0;
        for (const int p : pts)
            worst = std::max(worst, fitOff(s, pointOf(f.mesh, p)));
        const double allowed = std::min(capMm, std::max(s.radius, 1e-6) * share);
        if (worst > allowed)
            return;
        found = true;
        best = s;
        bestOff = worst;
    };

    // Which way out is the surface? The normals point away from the axis (a boss) or toward it
    // (a hole). `distanceTo` doesn't care; the orientation is settled on the face later.
    double spreadAlong = 0;
    for (std::size_t i = 0; i < dirs.size(); ++i)
        spreadAlong = std::max(spreadAlong, std::abs(dirs[i].Dot(axis) - meanDot));
    const bool ring = spreadAlong < 0.05;    // every normal at one angle to the axis
    if (ring) {
        Surface cyl;
        cyl.kind = Surface::Cylinder;
        cyl.axis = axis;
        // The facets' corners are on it; a point where another face crosses a facet is on its chord,
        // inside by up to the sag. So the circle is fitted to the outermost band of points, as far out
        // as the fit may miss by, not to every point (a 16-sided hole read 0.05 mm small that way).
        // Normal lines from triangles a crossing face cut short miss the axis, so the centre is the
        // band's circle's too (that hole's axis sat 0.04 mm off).
        V u = axis.Crossed(std::abs(axis.X()) < 0.9 ? V(1, 0, 0) : V(0, 1, 0));
        u /= u.Modulus();
        const V v = axis.Crossed(u);
        V centre = onAxis;
        for (int pass = 0; pass < 2; ++pass) {
            std::vector<std::array<double, 2>> flat;
            double outermost = 0;
            for (const int p : pts) {
                const V d = pointOf(f.mesh, p) - centre;
                flat.push_back({d.Dot(u), d.Dot(v)});
                outermost = std::max(outermost, std::hypot(flat.back()[0], flat.back()[1]));
            }
            const double band = std::min(capMm, std::max(outermost, 1e-6) * share);
            std::vector<std::array<double, 2>> rim;
            for (const auto &q : flat)
                if (std::hypot(q[0], q[1]) >= outermost - band)
                    rim.push_back(q);
            double cu, cv, r, worst;
            if (!circleOf(rim, 0, rim.size() - 1, cu, cv, r, worst)) {
                cyl.radius = outermost;
                break;
            }
            centre += u * cu + v * cv;
            cyl.radius = r;
            // The facets' corners are on the true circle; a point where another face cut a facet
            // short is inside it by a share of the sag. Once the band's circle is found, the points
            // near it are taken again without those more than three times the middle miss inside
            // (and over the 0.001 mm a file written to 3 decimals rounds by). spacer_mit_loch's hole:
            // dome-cut points 0.007-0.02 mm in had moved its centre 0.001-0.005 mm, Low Res and
            // Detailed differently, and a wall corner one facet's sag in then missed it (round 7).
            if (pass == 1) {
                std::vector<std::array<double, 2>> near;
                std::vector<double> miss;
                for (const int p : pts) {
                    const V d = pointOf(f.mesh, p) - centre;
                    const std::array<double, 2> q{d.Dot(u), d.Dot(v)};
                    if (std::abs(std::hypot(q[0], q[1]) - r) <= band) {
                        near.push_back(q);
                        miss.push_back(std::abs(std::hypot(q[0], q[1]) - r));
                    }
                }
                std::nth_element(miss.begin(), miss.begin() + long(miss.size() / 2), miss.end());
                const double most = std::max(3 * (miss.empty() ? 0.0 : miss[miss.size() / 2]), 1e-3);
                std::vector<std::array<double, 2>> kept;
                for (const auto &q : near)
                    if (r - std::hypot(q[0], q[1]) <= most)
                        kept.push_back(q);
                if (kept.size() >= 3 && kept.size() < near.size() && circleOf(kept, 0, kept.size() - 1, cu, cv, r, worst)) {
                    centre += u * cu + v * cv;
                    cyl.radius = r;
                }
            }
        }
        cyl.origin = centre;
        consider(cyl);
    }

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
    // Then the ball through its points, as a cylinder's circle is: facet corners are on it, and the
    // facets' normal lines miss the centre (a 6-facet ball corner of r 3 read 2.882 that way).
    {
        math_Matrix a(1, 4, 1, 4, 0.0);
        math_Vector b(1, 4, 0.0);
        for (const int p : pts) {
            const V d = pointOf(f.mesh, p) - onAxis;
            const double row[4] = {2 * d.X(), 2 * d.Y(), 2 * d.Z(), 1}, w = d.SquareModulus();
            for (int i = 0; i < 4; ++i) {
                b(i + 1) += row[i] * w;
                for (int j = 0; j < 4; ++j)
                    a(i + 1, j + 1) += row[i] * row[j];
            }
        }
        math_Gauss solve(a);
        if (solve.IsDone() && pts.size() >= 4) {
            math_Vector x(1, 4);
            solve.Solve(b, x);
            const V c(x(1), x(2), x(3));
            // Only to correct it: a few points lie on some ball whatever the surface.
            if (const double rr = x(4) + c.SquareModulus();
                rr > 0 && std::abs(std::sqrt(rr) - ball.radius) < 0.1 * ball.radius && c.Modulus() < 0.1 * ball.radius) {
                ball.origin = onAxis + c;
                ball.radius = std::sqrt(rr);
            }
        }
    }
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
        // A thread is cut deep for its pitch (ISO: 0.54-0.61 of it; the speaker's 5/8-27, 0.53 mm on
        // 0.94). A plain pin's facets also line up on a long "helix": hinge_male's 6.85 mm pin came
        // out a 5 mm-pitch thread 0.018 mm deep, and the mesh cut along it folded the pin's end over
        // itself (round 6). Under a fifth of the pitch, or a few tolerances, it is no thread.
        {
            double least = INFINITY, most = -INFINITY;
            for (const auto &q : line)
                least = std::min(least, q[1]), most = std::max(most, q[1]);
            if (most - least < std::max(turn * 0.2, tol * 4))
                continue;
        }
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

// Owner, 2026-11-17 (GB-092): an STL is made to be printed, and a piece standing on its own (sharing no
// triangle side with the rest) that is smaller than kDetachedSpeck across can't be: floating debris. Such
// pieces go whole. Nothing else is touched: every triangle of every piece that stays is left exactly as it
// was, and a piece joined to the model, however small, stays. Returns the dropped pieces' sizes (box
// diagonals, mm).
std::vector<double> dropDetachedSpecks(Mesh &m) {
    const std::size_t n = m.triangles.size();
    std::vector<int> parent(n);
    std::iota(parent.begin(), parent.end(), 0);
    auto root = [&](int a) {
        while (parent[std::size_t(a)] != a)
            a = parent[std::size_t(a)] = parent[std::size_t(parent[std::size_t(a)])];
        return a;
    };
    std::map<std::pair<int, int>, int> firstOnSide;
    for (std::size_t t = 0; t < n; ++t)
        for (int k = 0; k < 3; ++k) {
            const int a = m.triangles[t][std::size_t(k)], b = m.triangles[t][std::size_t((k + 1) % 3)];
            const auto side = std::make_pair(std::min(a, b), std::max(a, b));
            const auto at = firstOnSide.find(side);
            if (at == firstOnSide.end())
                firstOnSide.emplace(side, int(t));
            else
                parent[std::size_t(root(int(t)))] = root(at->second);
        }
    // Each piece's box.
    std::map<int, std::array<double, 6>> box;
    for (std::size_t t = 0; t < n; ++t) {
        auto at = box.try_emplace(root(int(t)), std::array<double, 6>{INFINITY, INFINITY, INFINITY, -INFINITY, -INFINITY, -INFINITY}).first;
        for (const int p : m.triangles[t])
            for (int i = 0; i < 3; ++i) {
                at->second[std::size_t(i)] = std::min(at->second[std::size_t(i)], m.points[std::size_t(p)][std::size_t(i)]);
                at->second[std::size_t(i + 3)] = std::max(at->second[std::size_t(i + 3)], m.points[std::size_t(p)][std::size_t(i)]);
            }
    }
    std::vector<double> dropped;
    if (box.size() < 2)
        return dropped;
    std::map<int, double> size;
    int biggest = -1;
    for (const auto &[piece, b] : box) {
        size[piece] = std::hypot(b[3] - b[0], b[4] - b[1], b[5] - b[2]);
        if (biggest < 0 || size[piece] > size[biggest])
            biggest = piece;
    }
    std::set<int> gone;
    for (const auto &[piece, across] : size)
        if (piece != biggest && across < kDetachedSpeck) {
            gone.insert(piece);
            dropped.push_back(across);
        }
    if (gone.empty())
        return dropped;
    // The rest, with only the points they use, in their old order.
    Mesh kept;
    std::vector<int> renumber(m.points.size(), -1);
    for (std::size_t t = 0; t < n; ++t) {
        if (gone.count(root(int(t))))
            continue;
        std::array<int, 3> tri{};
        for (int k = 0; k < 3; ++k) {
            int &to = renumber[std::size_t(m.triangles[t][std::size_t(k)])];
            if (to < 0) {
                to = int(kept.points.size());
                kept.points.push_back(m.points[std::size_t(m.triangles[t][std::size_t(k)])]);
            }
            tri[std::size_t(k)] = to;
        }
        kept.triangles.push_back(tri);
    }
    m = std::move(kept);
    return dropped;
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
    dropDoubles(out);
    dropSlivers(out, reach(out));
    orient(out);
    // GB-092: floating debris under kDetachedSpeck goes; nothing joined to the model is touched.
    const std::vector<double> specks = dropDetachedSpecks(out);
    std::string sizes;   // the first 20 pieces' sizes, mm
    for (std::size_t i = 0; i < specks.size() && i < 20; ++i) {
        char one[32];
        std::snprintf(one, sizeof one, "%s%.6f", i ? ", " : " (", specks[i]);
        sizes += one;
    }
    if (!specks.empty())
        sizes += specks.size() > 20 ? ", ... mm)" : " mm)";
    std::fprintf(stderr, "meshsolid: detached pieces under %.3f mm dropped: %zu%s\n", kDetachedSpeck, specks.size(), sizes.c_str());
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

TopoDS_Shape solidOfOne(const Mesh &mesh, const Stage &stage, Style style = Style::Detailed);

// Several pieces in one file (a box and its lid): each its own solid, together in one compound.
template <typename Make>
TopoDS_Shape eachPiece(const Mesh &mesh, const Make &make) {
    const std::vector<std::vector<int>> parts = pieces(mesh);
    if (parts.size() < 2)
        return make(mesh);
    TopoDS_Compound all;
    BRep_Builder builder;
    builder.MakeCompound(all);
    for (const auto &part : parts) {
        const TopoDS_Shape one = make(subMesh(mesh, part));
        if (one.IsNull())
            return {};
        builder.Add(all, one);
    }
    return all;
}

TopoDS_Shape solid(const Mesh &mesh, const Stage &stage, Style style) {
    return eachPiece(mesh, [&](const Mesh &piece) { return solidOfOne(piece, stage, style); });
}

TopoDS_Shape solidOfOne(const Mesh &mesh, const Stage &stage, Style style) {
    stage("Finding the flat areas");
    const Facets f(mesh);
    std::vector<int> labels;
    std::vector<Surface> surfaces;
    flatAreas(f, reach(mesh), labels, surfaces);
    const TopoDS_Shape plain = assemble(mesh, labels, surfaces, 1e-3, stage);
    if (style != Style::LowPoly)
        return plain;
    // Low poly: near-same-angle facets joined. Said of a smooth part (a rounded box's fine corners
    // joined three degrees at a time) the joins don't flatten and split into single triangles, or a
    // ring of them wraps a hole wrongly: then the ordinary import stands, as it does when the joins
    // left more faces than it has.
    labels.clear();
    surfaces.clear();
    flatAreas(f, reach(mesh), labels, surfaces, true);
    const TopoDS_Shape joined = assemble(mesh, labels, surfaces, 1e-3, stage);
    if (joined.IsNull() || !BRepCheck_Analyzer(joined).IsValid())
        return plain;
    if (!plain.IsNull()) {
        int faces[2] = {0, 0};
        for (TopExp_Explorer e(plain, TopAbs_FACE); e.More(); e.Next())
            ++faces[0];
        for (TopExp_Explorer e(joined, TopAbs_FACE); e.More(); e.Next())
            ++faces[1];
        if (faces[1] >= faces[0])
            return plain;
    }
    return joined;
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

// The area inside the loop where a plane square to `way` at height h cuts the mesh, taking the loop
// that passes nearest `near` (0 if the plane misses). The loop runs round with the outside on its
// right as seen down `way`, so an outline counts positive and a hole negative; the size is returned.
double loopArea(const Mesh &mesh, const V &way, double h, const V &near) {
    struct Piece { std::pair<int, int> to; V a, b; };
    std::map<std::pair<int, int>, Piece> from;
    const auto edge = [](int i, int j) { return std::make_pair(std::min(i, j), std::max(i, j)); };
    for (const auto &t : mesh.triangles) {
        double d[3];
        V p[3];
        for (int k = 0; k < 3; ++k) {
            p[k] = pointOf(mesh, t[std::size_t(k)]);
            d[k] = p[k].Dot(way) - h;
        }
        std::vector<std::pair<std::pair<int, int>, V>> cut;
        for (int k = 0; k < 3; ++k) {
            const int m = (k + 1) % 3;
            if ((d[k] < 0) != (d[m] < 0))
                cut.push_back({edge(t[std::size_t(k)], t[std::size_t(m)]), p[k] + (p[m] - p[k]) * (d[k] / (d[k] - d[m]))});
        }
        if (cut.size() != 2)
            continue;
        const V across = way.Crossed((p[1] - p[0]).Crossed(p[2] - p[0]));
        if ((cut[1].second - cut[0].second).Dot(across) < 0)
            std::swap(cut[0], cut[1]);
        from[cut[0].first] = {cut[1].first, cut[0].second, cut[1].second};
    }
    double best = 1e300, area = 0;
    std::set<std::pair<int, int>> done;
    for (const auto &start : from) {
        if (done.count(start.first))
            continue;
        double sum = 0, closest = 1e300;
        for (auto at = from.find(start.first); at != from.end() && done.insert(at->first).second; at = from.find(at->second.to)) {
            sum += way.Dot(at->second.a.Crossed(at->second.b)) / 2;
            const V ab = at->second.b - at->second.a;
            const double u = std::clamp((near - at->second.a).Dot(ab) / std::max(ab.SquareModulus(), 1e-18), 0.0, 1.0);
            closest = std::min(closest, (at->second.a + ab * u - near).Modulus());
        }
        if (closest < best) {
            best = closest;
            area = std::abs(sum);
        }
    }
    return area;
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
// Returns how far any point ended up from a wall straightened by the short-wall or area rule (0 if none), so
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
    // The owner's rule (2026-11-13), for a wall of any height: cut across it near its bottom and near
    // its top; if the outline it lies on is within 0.5% of the same area at both, the lean is not a
    // taper, and the wall is made straight, midway between (the speaker mount's finger-groove bottoms,
    // cones of 0.014-0.043 degrees down a 21 mm base whose outline is 779.5 against 777.9 mm2).
    const auto sameArea = [&](const std::vector<int> &points, const V &way) {
        double lo = 1e300, hi = -1e300;
        for (const int p : points) {
            const double h = pointOf(mesh, p).Dot(way);
            lo = std::min(lo, h);
            hi = std::max(hi, h);
        }
        if (hi - lo < 4 * allowed)
            return false;
        double area[2];
        for (int end = 0; end < 2; ++end) {
            const double h = end ? hi - (hi - lo) * 0.02 : lo + (hi - lo) * 0.02;
            V near;
            double gap = 1e300;
            for (const int p : points)
                if (const double g = std::abs(pointOf(mesh, p).Dot(way) - h); g < gap) {
                    gap = g;
                    near = pointOf(mesh, p);
                }
            area[end] = loopArea(mesh, way, h, near);
        }
        return area[0] > 0 && area[1] > 0 && std::abs(area[0] - area[1]) < 0.005 * std::max(area[0], area[1]);
    };
    std::vector<std::pair<V, const std::vector<int> *>> straightened;   // walls the area rule made straight
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
            const bool lean = std::sqrt(std::max(0.0, 1 - c * c)) < smallLean && s.kind != Surface::Torus;
            const bool byArea = lean && round && !meant(s, one.points, way) && sameArea(one.points, way);
            const bool forced = lean && (meant(s, one.points, way) || byArea);
            if ((s.kind == Surface::Torus || refit(mesh, one.points, way, s.kind == Surface::Cone && !(forced && std::abs(s.slope) < smallLean), tried))
                && (holds(tried, one.points) || (forced && worst(tried, one.points)))) {
                if (byArea)
                    straightened.push_back({way, &one.points});
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
            const bool byArea = std::abs(c) < smallLean && !meant(s, one.points, way) && sameArea(one.points, round ? way : ways[k]);
            const bool forced = std::abs(c) < smallLean && (meant(s, one.points, way) || byArea);
            if (refit(mesh, one.points, way, s.kind == Surface::Cone && !(forced && std::abs(s.slope) < smallLean), tried) && (holds(tried, one.points) || (forced && worst(tried, one.points)))) {
                if (byArea)
                    straightened.push_back({round ? way : ways[k], &one.points});
                s = tried;
                settled = true;
            }
        }
        if (!settled || std::none_of(ways.begin(), ways.end(), [&](const V &w) { return std::abs(std::abs(w.Dot(s.axis)) - 1) < 1e-15; }))
            ways.push_back(s.axis);
        if (s.kind == Surface::Cone) {
            Surface tried = s;
            const bool byArea = std::abs(s.slope) < smallLean && tall(one.points, s.axis) >= shortWall && sameArea(one.points, s.axis);
            const bool forced = std::abs(s.slope) < smallLean && (tall(one.points, s.axis) < shortWall || byArea);
            if (refit(mesh, one.points, s.axis, false, tried) && (holds(tried, one.points) || (forced && worst(tried, one.points)))) {
                if (byArea)
                    straightened.push_back({s.axis, &one.points});
                s = tried;
            }
        }
        if (!round && s.kind != Surface::Torus)
            continue;
        // On a bigger round wall's axis line, if it fits there. A fillet ring too: the speaker mount's
        // shoulder round, cut into 22 pieces by its ribs, was 22 rings up to 0.003 mm off each other
        // and off the plug's own axis, so the shoulder's outline was 22 circles that never quite met
        // and a cut under it couldn't be made (BRep_API: command not done).
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
            if (tried.kind == Surface::Torus) {
                // the ring keeps its own radius and tube: only its centre moves
            } else if (tried.kind == Surface::Cone && n * shh - sh * sh > 1e-12) {
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
        if (round)
            rounds.push_back(&s);
    }
    // And a flat beside a wall the area rule made straight, within half a degree of square to it (a
    // base under the groove), is made exactly square to it.
    for (const auto &wall : straightened) {
        const std::set<int> on(wall.second->begin(), wall.second->end());
        for (Fitted &one : all) {
            Surface &s = *one.surface;
            const double c = s.axis.Dot(wall.first);
            if (s.kind != Surface::Flat || keep.count(&s) || std::abs(c) >= 1 - 1e-15 || std::abs(c) < std::cos(0.5 * M_PI / 180)
                || std::count_if(one.points.begin(), one.points.end(), [&](int p) { return on.count(p) > 0; }) < 2)
                continue;
            Surface tried = s;
            if (refit(mesh, one.points, c < 0 ? wall.first * -1 : wall.first, false, tried) && worst(tried, one.points))
                s = tried;
        }
    }
    // Then two surfaces that are one: a plane level with a bigger one, or a round wall on a bigger
    // one's axis with its radius, becomes that very surface, as CGAL's regularize_planes also snaps
    // coplanarity. Fitted apart they lie a hair off each other (the speaker mount's base had two side
    // faces 0.00007 mm apart), so the line between them stays and merging them breaks the solid.
    for (std::size_t i = 1; i < all.size(); ++i) {
        Surface &s = *all[i].surface;
        if ((s.kind != Surface::Flat && s.kind != Surface::Cylinder && s.kind != Surface::Cone && s.kind != Surface::Torus)
            || all[i].points.size() < 3 || keep.count(&s))
            continue;
        for (std::size_t j = 0; j < i; ++j) {
            const Surface &big = *all[j].surface;
            const double c = big.axis.Dot(s.axis);
            if (big.kind != s.kind || std::abs(c) < 1 - 1e-15)
                continue;
            const V d = s.origin - big.origin;
            if (s.kind == Surface::Torus) {
                // the same ring: centre, height, both radii and the way it faces, within what's allowed
                if (c < 0 || d.Modulus() > allowed || std::abs(s.radius - big.radius) > allowed || std::abs(s.tube - big.tube) > allowed
                    || std::abs(std::remainder(s.facing - big.facing, 2 * M_PI)) > M_PI / 4 || s.inward != big.inward)
                    continue;
                Surface tried = big;
                tried.seam = s.seam;
                if (holds(tried, all[i].points)) {
                    s = tried;
                    break;
                }
                continue;
            }
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
        if (shift.Modulus() <= allowed && holds(tried, one.points)) {
            // Rounds put on its axis line above (a pin in its knuckle) move with it and stay on one
            // axis, where their points allow (hinge_male's pin stayed 0.005 mm behind its knuckle).
            for (Fitted &other : all) {
                Surface &o = *other.surface;
                const V d = o.origin - s.origin;
                if (&o == &s || keep.count(&o) || (o.kind != Surface::Cylinder && o.kind != Surface::Cone && o.kind != Surface::Torus)
                    || std::abs(o.axis.Dot(s.axis)) < 1 - 1e-15 || (d - s.axis * d.Dot(s.axis)).Modulus() > 1e-9)
                    continue;
                Surface moved = o;
                moved.origin = o.origin + shift;
                if (holds(moved, other.points))
                    o = moved;
            }
            s = tried;
        }
    }
    return forcedOff;
}

// The faces' repairs can leave faces turned the wrong way, or the solid inside out.
// Whether a round's triangles go all the way round its axis: their corners' turns, in 10 degree
// steps, cover 33 of the 36 (each triangle the whole turn its corners span), as the ring cut reads it.
static bool wholeRound(const Facets &f, const std::vector<int> &group, const Surface &s) {
    const gp_Ax3 frame(gp_Pnt(0, 0, 0), gp_Dir(s.axis));
    const V x = frame.XDirection().XYZ(), y = frame.YDirection().XYZ();
    std::array<char, 36> bins{};
    for (const int t : group) {
        std::array<double, 3> spins;
        for (int k = 0; k < 3; ++k) {
            const V at = pointOf(f.mesh, f.mesh.triangles[std::size_t(t)][std::size_t(k)]) - s.origin;
            spins[std::size_t(k)] = std::atan2(at.Dot(y), at.Dot(x)) + M_PI;
        }
        std::sort(spins.begin(), spins.end());
        int gapAfter = 2;
        double gap = spins[0] + 2 * M_PI - spins[2];
        for (int k = 0; k < 2; ++k)
            if (spins[std::size_t(k + 1)] - spins[std::size_t(k)] > gap) {
                gap = spins[std::size_t(k + 1)] - spins[std::size_t(k)];
                gapAfter = k;
            }
        const double from = spins[std::size_t((gapAfter + 1) % 3)], span = 2 * M_PI - gap;
        for (int b = int(from / (2 * M_PI) * 36); b <= int((from + span) / (2 * M_PI) * 36); ++b)
            bins[std::size_t(b % 36)] = 1;
    }
    return std::accumulate(bins.begin(), bins.end(), 0) >= 33;
}

static void turnRight(TopoDS_Shape &shape) {
    if (shape.ShapeType() != TopAbs_SOLID || BRepCheck_Analyzer(shape).IsValid())
        return;
    ShapeFix_Shell faces(TopoDS::Shell(TopExp_Explorer(shape, TopAbs_SHELL).Current()));
    faces.Perform();
    TopoDS_Solid turned;
    BRep_Builder().MakeSolid(turned);
    BRep_Builder().Add(turned, faces.Shell());
    if (BRepLib::OrientClosedSolid(turned))
        shape = turned;
}

// The triangles that will make each face (a fitted surface's side, or a flat): any run of them too thin
// goes to the face beside it whose surface its corners lie closest to, if near enough. Again until
// none moves: a run given away can leave its neighbour's edge thin.
void giveAwayThin(const Facets &f, const std::vector<Surface> &fitted, const std::vector<Surface> &surfaces, std::vector<int> &owner,
                  std::vector<int> &side, std::vector<int> &labels) {
    const Mesh &m = f.mesh;
    const auto keyOf = [&](std::size_t t) { return owner[t] >= 0 ? std::make_pair(owner[t], side[t]) : std::make_pair(-1, labels[t]); };
    const auto surfaceOfKey = [&](const std::pair<int, int> &k) -> const Surface & {
        return k.first >= 0 ? fitted[std::size_t(k.first)] : surfaces[std::size_t(k.second)];
    };
    bool last = false;    // after the near moves: what is still thin goes to its nearest face, up to kSliverLastMiss
    for (int pass = 0; pass < 16; ++pass) {
        std::vector<int> region(owner.size(), -1), stack;
        std::vector<double> area, outline;
        std::vector<std::vector<int>> members;
        for (std::size_t first = 0; first < owner.size(); ++first) {
            if (region[first] >= 0)
                continue;
            const int r = int(area.size());
            area.push_back(0);
            outline.push_back(0);
            members.emplace_back();
            const auto key = keyOf(first);
            region[first] = r;
            stack = {int(first)};
            while (!stack.empty()) {
                const std::size_t t = std::size_t(stack.back());
                stack.pop_back();
                members.back().push_back(int(t));
                area.back() += f.area[t];
                for (const int other : f.across[t])
                    if (other >= 0 && region[std::size_t(other)] < 0 && keyOf(std::size_t(other)) == key) {
                        region[std::size_t(other)] = r;
                        stack.push_back(other);
                    }
            }
        }
        for (std::size_t t = 0; t < owner.size(); ++t)
            for (std::size_t k = 0; k < 3; ++k) {
                const int other = f.across[t][k];
                if (other < 0 || region[std::size_t(other)] != region[t])
                    outline[std::size_t(region[t])] += (pointOf(m, m.triangles[t][k]) - pointOf(m, m.triangles[t][(k + 1) % 3])).Modulus();
            }
        std::vector<char> thin(area.size(), 0);
        for (std::size_t r = 0; r < area.size(); ++r)
            thin[r] = sliver(area[r], outline[r]);
        // A thin run goes whole, to the one face beside it that holds and whose surface all its corners
        // lie nearest: given away triangle by triangle, a rounded box's facet strips split corner to
        // corner between two walls and the build failed (roundedBox1 lid: no solid at all).
        int moved = 0;
        const double limit = last ? kSliverLastMiss : kSliverMiss;
        for (std::size_t r = 0; r < area.size(); ++r) {
            if (!thin[r])
                continue;
            std::set<int> points;
            std::map<int, int> example;
            for (const int member : members[r]) {
                points.insert(m.triangles[std::size_t(member)].begin(), m.triangles[std::size_t(member)].end());
                for (const int other : f.across[std::size_t(member)])
                    if (other >= 0 && region[std::size_t(other)] != int(r) && !thin[std::size_t(region[std::size_t(other)])])
                        example.emplace(region[std::size_t(other)], other);
            }
            double best = limit;
            int from = -1;
            for (const auto &[to, other] : example) {
                const Surface &on = surfaceOfKey(keyOf(std::size_t(other)));
                double miss = 0;
                for (const int p : points)
                    miss = std::max(miss, distanceTo(on, pointOf(m, p)));
                if (miss <= best) {
                    best = miss;
                    from = other;
                }
            }
            if (from < 0)
                continue;
            for (const int member : members[r]) {
                const std::size_t t = std::size_t(member);
                owner[t] = owner[std::size_t(from)];
                side[t] = side[std::size_t(from)];
                labels[t] = labels[std::size_t(from)];
            }
            ++moved;
        }
        if (!moved) {
            if (last)
                return;
            last = true;
        }
    }
}

// noThin: give thin runs away (giveAwayThin) and thin triangles to the face beside them (snapToSurfaces).
static TopoDS_Shape fitMesh(const Mesh &mesh, double share, const Stage &stage, FitReport &report, const std::vector<Surface> *threads,
                            Style style, const std::vector<int> *forced = nullptr, bool noThin = true) {
    // How few triangles start a curve, and keep one: a low-res export's corner is 2-3 facets.
    const std::size_t startFewest = style == Style::LowRes ? std::size_t(kLowResFewest) : 6;
    const std::size_t keepFewest = style == Style::LowRes ? std::size_t(kLowResFewest) : 8;
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
            return fitMesh(cut, share, stage, report, &screws, style, forced, noThin);
        }
        // The cut mesh has an edge on every line between these, so each triangle goes to one.
        if (threads)
            for (const Surface &thread : *threads) {
                found.push_back(thread);
                seeds.emplace_back(mesh.triangles.size());
                std::iota(seeds.back().begin(), seeds.back().end(), 0);
            }
        for (std::size_t i = 0; i < found.size(); ++i) {
            Surface surface = found[i];
            double side = 0;    // 1 out of the surface, -1 into it, 0 either
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
                const double facing = f.normal[std::size_t(t)].Dot(normalAt(surface, mid));
                return (side == 0 ? std::abs(facing) : side * facing) >= std::cos(35 * M_PI / 180);
            };
            std::set<int> in;
            std::vector<int> group;
            const auto gather = [&] {
                in.clear();
                group.clear();
                for (const int t : seeds[i])
                    if (fits(t) && in.insert(t).second)
                        group.push_back(t);
                for (std::size_t g = 0; g < group.size(); ++g)
                    for (const int other : f.across[std::size_t(group[g])])
                        if (other >= 0 && !in.count(other) && fits(other)) {
                            in.insert(other);
                            group.push_back(other);
                        }
            };
            gather();
            // Slices only find where a round is; their circle is read from cuts across facets and can be
            // off (hinge_male's knuckle: r 5.024 for 5.000, so half its facets missed it and stayed
            // strips). What it is comes from its facets, as for any other round, and it gathers again.
            Surface exact;
            double exactOff = 0;
            if ((surface.kind == Surface::Cylinder || surface.kind == Surface::Cone) && group.size() >= 8
                && fitRound(f, group, share, capMm, exact, exactOff) && exact.kind == surface.kind) {
                surface = exact;
            }
            // Its outside and its inside are two surfaces (a knuckle and the cut its partner turns in).
            if (surface.kind == Surface::Screw) {
                if (group.size() >= 8)
                    keep(surface, group, tol * 1.5);
                continue;
            }
            for (side = 1; side >= -1; side -= 2) {
                gather();
                if (group.size() >= 8)
                    keep(surface, group, tol * 1.5);
            }
        }
    }

    // Low Res (round 5): a round cut into a few facets meets its walls at the same bend as its facets
    // meet one another (a 90 degree pocket corner in 30 degree steps), so the patch round a seed takes
    // the walls in too and no surface fits it. Its facets are narrow strips of one width, the walls
    // wide: `narrowPatch` grows from the seed's facet only into facets turning round the same axis
    // and about as wide (`kLowResWidth` either way). Facets are the triangles in one plane, joined.
    std::vector<int> facetOf(mesh.triangles.size(), -1);
    std::vector<std::vector<int>> facetTris;
    const auto facetFor = [&](int t) {
        if (facetOf[std::size_t(t)] >= 0)
            return facetOf[std::size_t(t)];
        const int id = int(facetTris.size());
        facetTris.push_back({t});
        facetOf[std::size_t(t)] = id;
        for (std::size_t i = 0; i < facetTris.back().size(); ++i)
            for (const int o : f.across[std::size_t(facetTris.back()[i])])
                if (o >= 0 && facetOf[std::size_t(o)] < 0 && f.normal[std::size_t(o)].Dot(f.normal[std::size_t(t)]) > flatEnough) {
                    facetOf[std::size_t(o)] = id;
                    facetTris[std::size_t(id)].push_back(o);
                }
        return id;
    };
    const auto widthOf = [&](int facet, const V &across) {
        double lo = 1e300, hi = -1e300;
        for (const int t : facetTris[std::size_t(facet)])
            for (const int p : mesh.triangles[std::size_t(t)]) {
                const double d = pointOf(mesh, p).Dot(across);
                lo = std::min(lo, d);
                hi = std::max(hi, d);
            }
        return hi - lo;
    };
    const auto narrowPatch = [&](int seed, double limit) {
        std::vector<int> out;
        const int first = facetFor(seed);
        const V n0 = f.normal[std::size_t(seed)];
        // Its axis from the bend across its longest bent side, as the seed's bend is: a strip's short
        // end can meet a gentler corner facet, and an axis from that runs across the strip.
        V axis(0, 0, 0);
        double longest = 0;
        for (const int t : facetTris[std::size_t(first)])
            for (int k = 0; k < 3; ++k)
                if (const int o = f.across[std::size_t(t)][std::size_t(k)]; o >= 0 && facetOf[std::size_t(o)] != first) {
                    const double dot = f.normal[std::size_t(o)].Dot(n0);
                    const auto &tri = mesh.triangles[std::size_t(t)];
                    const double side = (pointOf(mesh, tri[std::size_t(k)]) - pointOf(mesh, tri[std::size_t((k + 1) % 3)])).Modulus();
                    if (dot < flatEnough && dot > limit && side > longest)
                        axis = n0.Crossed(f.normal[std::size_t(o)]), longest = side;
                }
        if (axis.SquareModulus() < 1e-12)
            return out;
        axis.Normalize();
        const double w0 = widthOf(first, axis.Crossed(n0));
        // A strip: twice as long along the axis as it is wide (a faceted ball's facets are about
        // square, and two of them make a "cylinder" pointing anywhere).
        if (w0 <= 0 || widthOf(first, axis) < 2 * w0)
            return out;
        std::vector<int> facets{first};
        std::set<int> seen{first};
        for (std::size_t i = 0; i < facets.size() && facets.size() < 64; ++i) {
            const int at = facets[i];
            const V nAt = f.normal[std::size_t(facetTris[std::size_t(at)][0])];
            for (const int t : facetTris[std::size_t(at)])
                for (const int o : f.across[std::size_t(t)]) {
                    if (o < 0 || owner[std::size_t(o)] >= 0)
                        continue;
                    const int g = facetFor(o);
                    if (seen.count(g))
                        continue;
                    const V n = f.normal[std::size_t(o)];
                    if (n.Dot(nAt) <= limit || std::abs(n.Dot(axis)) > 0.02)
                        continue;
                    bool free = true;
                    for (const int u : facetTris[std::size_t(g)])
                        free = free && owner[std::size_t(u)] < 0;
                    const double w = widthOf(g, axis.Crossed(n));
                    if (!free || w > w0 * kLowResWidth || w * kLowResWidth < w0 || widthOf(g, axis) < 2 * w)
                        continue;
                    seen.insert(g);
                    facets.push_back(g);
                }
        }
        if (facets.size() < 2)
            return out;
        // A round meets its walls turning the same way on, by half a step (the wall touches it at a
        // facet corner) to a whole one (the wall is one of its sides). Two equal strips with nothing
        // like that either side are a fold or a V, which a circle would fit just as well.
        const V u0 = axis.Crossed(n0);
        const auto facing = [&](const V &n) { return std::atan2(n.Dot(u0), n.Dot(n0)); };
        std::vector<double> turn;
        for (const int g : facets)
            turn.push_back(facing(f.normal[std::size_t(facetTris[std::size_t(g)][0])]));
        std::sort(turn.begin(), turn.end());
        double step = 0;
        for (std::size_t i = 1; i < turn.size(); ++i)
            step = std::max(step, turn[i] - turn[i - 1]);
        if (step < 1 * M_PI / 180)
            return std::vector<int>{};
        int ends = 0;
        for (const double endTurn : {turn.front(), turn.back()}) {
            const double outward = endTurn == turn.front() ? -1 : 1;
            bool met = false;
            for (const int g : facets) {
                if (std::abs(facing(f.normal[std::size_t(facetTris[std::size_t(g)][0])]) - endTurn) > 1e-6)
                    continue;
                for (const int t : facetTris[std::size_t(g)])
                    for (const int o : f.across[std::size_t(t)]) {
                        if (o < 0 || seen.count(facetFor(o)))
                            continue;
                        const V n = f.normal[std::size_t(o)];
                        if (std::abs(n.Dot(axis)) > 0.02)
                            continue;
                        const double bend = (facing(n) - endTurn) * outward;
                        met = met || (bend > 0.4 * step && bend < 1.2 * step);
                    }
            }
            ends += met ? 1 : 0;
        }
        if (ends < 2)
            return std::vector<int>{};
        for (const int g : facets)
            out.insert(out.end(), facetTris[std::size_t(g)].begin(), facetTris[std::size_t(g)].end());
        return out;
    };

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
        double bend = -1, longest = 0;
        for (int k = 0; k < 3; ++k)
            if (const int other = f.across[std::size_t(seed)][std::size_t(k)]; other >= 0) {
                const double dot = f.normal[std::size_t(other)].Dot(f.normal[std::size_t(seed)]);
                const auto &tri = f.mesh.triangles[std::size_t(seed)];
                const double side = (pointOf(f.mesh, tri[std::size_t(k)]) - pointOf(f.mesh, tri[std::size_t((k + 1) % 3)])).Modulus();
                if (dot < flatEnough && side > longest)
                    bend = dot, longest = side;
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
        if (patch.size() < startFewest)
            continue;

        double spread = 1;
        for (const int t : patch)
            spread = std::min(spread, f.normal[std::size_t(t)].Dot(f.normal[std::size_t(seed)]));
        if (spread > std::cos(2 * M_PI / 180))
            continue;    // flat here

        Surface surface;
        double off = 0;
        bool fitted = fitRound(f, patch, share, capMm, surface, off);
        if (!fitted && style == Style::LowRes) {
            std::vector<int> narrow = narrowPatch(seed, limit);
            const bool okNarrow = narrow.size() >= startFewest && fitRound(f, narrow, share, capMm, surface, off)
                                  && surface.kind == Surface::Cylinder;
            if (okNarrow) {
                patch = std::move(narrow);
                inPatch = std::set<int>(patch.begin(), patch.end());
                fitted = true;
            }
            // Or a curve smaller than its neighbours (a ball corner where three rounded edges meet,
            // 6 facets of 2 mm beside strips of 24): fitted on the triangles near the seed, within
            // three of its longest sides, not on a patch counted out into the strips.
            if (!fitted) {
                const auto &tri = mesh.triangles[std::size_t(seed)];
                const V at = (pointOf(mesh, tri[0]) + pointOf(mesh, tri[1]) + pointOf(mesh, tri[2])) / 3;
                double reachOf = 0;
                for (int k = 0; k < 3; ++k)
                    reachOf = std::max(reachOf, 3 * (pointOf(mesh, tri[std::size_t(k)]) - pointOf(mesh, tri[std::size_t((k + 1) % 3)])).Modulus());
                std::vector<int> local;
                for (const int t : patch) {
                    bool close = true;
                    for (const int q : mesh.triangles[std::size_t(t)])
                        close = close && (pointOf(mesh, q) - at).Modulus() <= reachOf;
                    if (close)
                        local.push_back(t);
                }
                if (local.size() >= startFewest && local.size() < patch.size() && fitRound(f, local, share, capMm, surface, off)) {
                    patch = std::move(local);
                    inPatch = std::set<int>(patch.begin(), patch.end());
                    fitted = true;
                }
            }
        }
        if (!fitted) {
            continue;
        }

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
                        on = on && fitOff(surface, pointOf(mesh, p)) <= allowed;
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
        if (group.size() < keepFewest)
            continue;

        // Grown past the patch it was fitted on: a flat wall's corners can sit on it too.
        double usual = 0;
        if ((surface.kind == Surface::Cylinder || surface.kind == Surface::Cone) && !evenSteps(f, group, surface, &usual))
            continue;
        // Low Res (owner, round 6): a hole or boss with fewer than 10 facets round it stays as it is,
        // 10 or more is a real round (straight or tapered, whichever fits). Round 5 rounded hole09.
        // Counted by its facets' step, for one that goes all the way round; a part-way round (a
        // pocket corner of 2-3 facets) is what Low Res is for, and isn't counted.
        if (style == Style::LowRes && (surface.kind == Surface::Cylinder || surface.kind == Surface::Cone) && usual > 0
            && wholeRound(f, group, surface) && 2 * M_PI / usual < kLowResFewestAround - 0.5)
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
            std::vector<std::pair<int, int>> taken;    // triangle, who held it before
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
                        on = on && fitOff(surface, pointOf(mesh, p)) <= allowed;
                    const V mid = (pointOf(mesh, tri[0]) + pointOf(mesh, tri[1]) + pointOf(mesh, tri[2])) / 3;
                    const double facing = f.normal[std::size_t(other)].Dot(normalAt(surface, mid));
                    if (!on || (surface.inward ? -facing : facing) < std::cos(35 * M_PI / 180))
                        continue;
                    taken.push_back({other, owner[std::size_t(other)]});
                    owner[std::size_t(other)] = id;
                    group.push_back(other);
                }
            if ((surface.kind == Surface::Cylinder || surface.kind == Surface::Cone) && !taken.empty() && !evenSteps(f, group, surface))
                for (const auto &[t, was] : taken)
                    owner[std::size_t(t)] = was;
            settled[std::size_t(id)] = 1;
        }
        // What's left of a robbed surface may be too little to stand alone.
        std::vector<int> left(fitted.size(), 0);
        for (const int o : owner)
            if (o >= 0)
                ++left[std::size_t(o)];
        for (int &o : owner)
            if (o >= 0 && left[std::size_t(o)] < int(keepFewest))
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
            // Whether these triangles lie on it, closer by `by` than on their holder.
            const auto closer = [&](const std::vector<int> &tris, const Surface &held, double by) {
                double mine = 0, there = 0;
                for (const int u : tris)
                    for (const int p : mesh.triangles[std::size_t(u)]) {
                        mine = std::max(mine, distanceTo(surface, pointOf(mesh, p)));
                        there = std::max(there, distanceTo(held, pointOf(mesh, p)));
                    }
                return mine <= tol * 1.5 && mine + by < there;
            };
            std::map<int, bool> whole;
            const auto fits = [&](int t) {
                // At its edge a fillet and its flat are one height: only the flat's own triangles tell.
                if (onBigFlat[std::size_t(t)])
                    return false;
                const auto &tri = mesh.triangles[std::size_t(t)];
                double worst = 0;
                for (const int p : tri)
                    worst = std::max(worst, distanceTo(surface, pointOf(mesh, p)));
                // A fillet's last rows run nearly level with the wall, and a wall's surface, loose
                // about its own fit, takes them. They go to whichever they really lie on. A small
                // surface gives way whole or not at all, split it leaves slivers: a near ball on a tight
                // torus goes to the torus, an exact ball corner keeps all six facets.
                const int holder = owner[std::size_t(t)];
                if (holder >= 0) {
                    const Surface &held = fitted[std::size_t(holder)];
                    if (soft[std::size_t(t)]) {
                        if (!whole.count(holder)) {
                            std::vector<int> all;
                            for (std::size_t u = 0; u < owner.size(); ++u)
                                if (owner[u] == holder)
                                    all.push_back(int(u));
                            whole[holder] = closer(all, held, 0);
                        }
                        if (!whole[holder])
                            return false;
                    } else if ((held.kind != Surface::Cone && held.kind != Surface::Cylinder) || !closer({t}, held, 0.005))
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
        // A small surface no one fillet took whole may still lie on the fillets found here together,
        // each triangle closer on one of them than on itself (a near ball across the seam of a rounded
        // tip and its side fillet): it gives way, each triangle to the closest such fillet that holds
        // a triangle beside it (or is given one), so no stray piece of a fillet is left on its own.
        {
            std::map<int, std::vector<int>> left;
            for (std::size_t t = 0; t < owner.size(); ++t)
                if (soft[t] && owner[t] >= 0 && owner[t] < firstFillet)
                    left[owner[t]].push_back(int(t));
            for (const auto &[holder, tris] : left) {
                std::map<int, std::vector<std::pair<double, int>>> on;    // triangle: (off, fillet), closest first
                for (const int t : tris) {
                    const auto &tri = mesh.triangles[std::size_t(t)];
                    const V mid = (pointOf(mesh, tri[0]) + pointOf(mesh, tri[1]) + pointOf(mesh, tri[2])) / 3;
                    const auto off = [&](const Surface &s) {
                        double worst = 0;
                        for (const int p : tri)
                            worst = std::max(worst, distanceTo(s, pointOf(mesh, p)));
                        return worst;
                    };
                    const double own = off(fitted[std::size_t(holder)]);
                    for (std::size_t j = std::size_t(firstFillet); j < fitted.size(); ++j) {
                        const double o = off(fitted[j]);
                        if (o <= tol * 1.5 && o < own && std::abs(f.normal[std::size_t(t)].Dot(normalAt(fitted[j], mid))) >= std::cos(35 * M_PI / 180))
                            on[t].push_back({o, int(j)});
                    }
                    std::sort(on[t].begin(), on[t].end());
                }
                std::map<int, int> to;
                for (bool grew = true; grew && to.size() < tris.size();) {
                    grew = false;
                    for (const int t : tris)
                        for (const auto &[o, j] : on[t]) {
                            if (to.count(t))
                                break;
                            for (const int u : f.across[std::size_t(t)])
                                if (u >= 0 && (to.count(u) ? to[u] == j : owner[std::size_t(u)] == j)) {
                                    to[t] = j;
                                    grew = true;
                                    break;
                                }
                        }
                }
                if (to.size() < tris.size())
                    continue;
                for (const auto &[t, j] : to) {
                    owner[std::size_t(t)] = j;
                    soft[std::size_t(t)] = 0;
                }
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
        // What's left of a surface that gave way may be too little to stand alone. Nor, in Detailed,
        // is a round drawn in under 3 coarse steps (facets turning 20 degrees or more each): that is
        // chamfering meant as flats, as Low Poly would show it (owner, round 6: the chamfered cube's
        // corners, 2 steps of 26 degrees, came out tori only on the small copy, whose sag fits).
        std::vector<int> left(fitted.size(), 0);
        std::vector<std::vector<double>> steps(fitted.size());
        std::vector<std::vector<V>> facings(fitted.size());
        for (std::size_t t = 0; t < owner.size(); ++t) {
            const int o = owner[t];
            if (o < 0)
                continue;
            ++left[std::size_t(o)];
            for (const int u : f.across[t])
                if (u > int(t) && owner[std::size_t(u)] == o && f.normal[t].Dot(f.normal[std::size_t(u)]) < 0.9999)
                    steps[std::size_t(o)].push_back(std::acos(std::min(1.0, f.normal[t].Dot(f.normal[std::size_t(u)]))));
            auto &seen = facings[std::size_t(o)];
            if (seen.size() <= 16 && std::none_of(seen.begin(), seen.end(), [&](const V &n) { return n.Dot(f.normal[t]) > 0.9999; }))
                seen.push_back(f.normal[t]);
        }
        const auto coarse = [&](std::size_t o) {
            auto &st = steps[o];
            if (style == Style::LowRes || st.empty() || facings[o].size() > 16)
                return false;
            std::nth_element(st.begin(), st.begin() + long(st.size() / 2), st.end());
            const double step = st[st.size() / 2];
            double spread = 0;
            for (const V &a : facings[o])
                for (const V &b : facings[o])
                    spread = std::max(spread, std::acos(std::clamp(a.Dot(b), -1.0, 1.0)));
            return step >= 20 * M_PI / 180 && spread < 3 * step;
        };
        std::vector<char> drop(fitted.size(), 0);
        for (std::size_t o = 0; o < fitted.size(); ++o)
            drop[o] = left[o] < keepFewest || coarse(o);
        for (int &o : owner)
            if (o >= 0 && drop[std::size_t(o)])
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
    // Where a round hole meets a faceted top (round 4: spacer_mit_loch's 16-sided hole under a curved
    // top). The true rim on a top facet is that facet's plane through the hole's surface, and it can
    // leave the facet across its side before it gets to the far corner: the facet's face crosses
    // itself and the whole hole went back to facets. There the rim really runs through the next facet,
    // so that side is split where it meets the hole's surface, and the sliver left between the old
    // chord and the new point is the hole's (`forced`, on the second go). Turned surfaces only.
    if (forced) {
        for (const int t : *forced)
            for (const int u : f.across[std::size_t(t)])
                if (u >= 0 && owner[std::size_t(u)] >= 0 && owner[std::size_t(t)] < 0) {
                    const Surface &round = fitted[std::size_t(owner[std::size_t(u)])];
                    if (round.kind == Surface::Cylinder || round.kind == Surface::Cone)
                        owner[std::size_t(t)] = owner[std::size_t(u)];
                }
    }
    // Run when a facet beside a turned surface comes out crossing itself (below), on those facets only:
    // done everywhere it also split facets that were fine (the speaker: 514 -> 646 faces, Fit 6.5 -> 12 s).
    Mesh split;
    std::vector<int> slivers;
    const auto splitRims = [&](const std::set<int> &only) {
        split = mesh;
        slivers.clear();
        std::set<int> touched;
        const double near = std::max(0.02, size * 2e-4) * 1.5;
        for (std::size_t t = 0; t < owner.size(); ++t) {
            if (owner[t] >= 0 || touched.count(int(t)) || !only.count(int(t)))
                continue;
            const auto tri = mesh.triangles[t];
            for (int k = 0; k < 3; ++k) {
                const int u = f.across[t][std::size_t(k)];
                if (u < 0 || owner[std::size_t(u)] < 0 || touched.count(u))
                    continue;
                const Surface &round = fitted[std::size_t(owner[std::size_t(u)])];
                if (round.kind != Surface::Cylinder && round.kind != Surface::Cone)
                    continue;
                const int ia = tri[std::size_t(k)], ib = tri[std::size_t((k + 1) % 3)], ic = tri[std::size_t((k + 2) % 3)];
                const V a = pointOf(mesh, ia), b = pointOf(mesh, ib), c = pointOf(mesh, ic), n = f.normal[t];
                if (distanceTo(round, a) > near || distanceTo(round, b) > near)
                    continue;
                // The rim in this facet's plane, round the axis from a to b.
                V x = round.axis.Crossed(std::abs(round.axis.X()) < 0.9 ? V(1, 0, 0) : V(0, 1, 0));
                x /= x.Modulus();
                const V y = round.axis.Crossed(x);
                const auto angleOf = [&](const V &p) { const V d = p - round.origin; return std::atan2(d.Dot(y), d.Dot(x)); };
                const double fromAngle = angleOf(a), turn = std::remainder(angleOf(b) - fromAngle, 2 * M_PI);
                bool level = false;
                const auto onRim = [&](double angle, V &at) {
                    const V way = x * std::cos(angle) + y * std::sin(angle);
                    const double slope = round.kind == Surface::Cone ? round.slope : 0;
                    const double under = n.Dot(round.axis) + slope * n.Dot(way);
                    if (std::abs(under) < 0.05) {
                        level = true;     // the facet runs along the surface: no one rim in it
                        return;
                    }
                    const double h = -(n.Dot(round.origin - a) + round.radius * n.Dot(way)) / under;
                    at = round.origin + round.axis * h + way * (round.radius + slope * h);
                };
                // Which side of the facet's other two sides the rim leaves by, if either.
                const auto side = [&](const V &from, const V &to, const V &p) { return (to - from).Crossed(p - from).Dot(n); };
                int out = 0;     // 1: across c-a, 2: across b-c
                for (int i = 1; i < 32 && !out && !level; ++i) {
                    V p;
                    onRim(fromAngle + turn * i / 32, p);
                    if (level)
                        break;
                    const double reach = (b - a).Modulus() * 1e-6;
                    if (side(c, a, p) * side(c, a, b) < 0 && std::abs(side(c, a, p)) > reach * (c - a).Modulus())
                        out = 1;
                    else if (side(b, c, p) * side(b, c, a) < 0 && std::abs(side(b, c, p)) > reach * (c - b).Modulus())
                        out = 2;
                }
                if (!out)
                    continue;
                // Where that side meets the hole's surface: from the rim corner, past the inside, to outside.
                const int ip = out == 1 ? ia : ib;
                const V from = out == 1 ? a : b;
                const int other = f.across[t][std::size_t(out == 1 ? (k + 2) % 3 : (k + 1) % 3)];
                if (other < 0 || owner[std::size_t(other)] >= 0 || touched.count(other))
                    continue;
                V way;
                const auto offAt = [&](double s) { double o = 0; meshsolid::offBy(round, from + (c - from) * s, o, way); return o; };   // offBy the function, not the list
                // Back from the far corner to where the side last crossed the surface (the rim corner
                // itself may sit a hair outside, a crossing right beside it that isn't this one).
                const double far = offAt(1);
                double lo = -1, hi = -1;
                for (int i = 64; i > 0; --i) {
                    const double s0 = (i - 1) / 64.0, s1 = i / 64.0;
                    if (offAt(s0) * far <= 0) {
                        lo = s0;
                        hi = s1;
                        break;
                    }
                }
                if (lo < 0 || std::abs(far) < 1e-9)
                    continue;
                for (int i = 0; i < 60; ++i) {
                    const double mid = (lo + hi) / 2;
                    (offAt(mid) * far > 0 ? hi : lo) = mid;
                }
                const double s0 = (lo + hi) / 2;
                if (s0 < 1e-3 || s0 > 1 - 1e-3)
                    continue;
                const V cut = from + (c - from) * s0;
                const int ix = int(split.points.size());
                split.points.push_back({cut.X(), cut.Y(), cut.Z()});
                // Each triangle on that side in two at the new point; the half of this one holding both
                // rim corners is the sliver.
                const auto halve = [&](int which, int p, int q) {
                    auto &tr = split.triangles[std::size_t(which)];
                    for (int j = 0; j < 3; ++j) {
                        const int e0 = tr[std::size_t(j)], e1 = tr[std::size_t((j + 1) % 3)], e2 = tr[std::size_t((j + 2) % 3)];
                        if ((e0 == p && e1 == q) || (e0 == q && e1 == p)) {
                            tr = {e0, ix, e2};
                            split.triangles.push_back({ix, e1, e2});
                            return std::array<int, 2>{which, int(split.triangles.size()) - 1};
                        }
                    }
                    return std::array<int, 2>{-1, -1};
                };
                const std::array<int, 2> mine = halve(int(t), ip, ic);
                const std::array<int, 2> theirs = halve(other, ip, ic);
                if (mine[0] < 0 || theirs[0] < 0) {
                    split = mesh;      // shouldn't happen; leave everything as it came
                    slivers.clear();
                    touched.clear();
                    break;
                }
                for (const int half : mine) {
                    const auto &h = split.triangles[std::size_t(half)];
                    if (std::count(h.begin(), h.end(), ia) && std::count(h.begin(), h.end(), ib))
                        slivers.push_back(half);
                }
                touched.insert({int(t), other, u});
                break;
            }
        }
    };
    // A smooth cap that no turned surface fits, as one surface (owner, round 6: spacer_mit_loch's
    // dome must be one true surface, not its facets). Its rings are ellipses and its height falls as
    // the square of the distance from its top: an elliptic paraboloid, which no sphere fits (the best
    // one is 0.32 mm off). Each run of smooth triangles left over, bending under 20 degrees from one
    // to the next and more than 5 overall, is read as a height over the plane square to its mean
    // facing, of second degree (paraboloids of every kind, and near enough any gentle cap). Least
    // squares, the worst points set aside a few at a time: a point where a wall or a hole cut across
    // the facets sits on a facet's chord, inside the surface by up to the sag, and must not bend
    // the fit. Kept only if every point it keeps is on it within 0.0001 mm (or 2 millionths of the part),
    // every point set aside is on the hollow side within a chord's sag, and every facet faces its way.
    // At least a fifth, and 12, must be kept.
    {
        const double tight = std::max(1e-4, size * 2e-6);
        std::vector<char> seen(owner.size(), 0);
        const double smooth = std::cos(20 * M_PI / 180);
        std::vector<int> grown;
        for (std::size_t start = 0; start < owner.size(); ++start) {
            if (grown.empty() && (owner[start] >= 0 || seen[start]))
                continue;
            const bool again = !grown.empty();
            std::vector<int> run = again ? std::move(grown) : std::vector<int>{int(start)};
            grown.clear();
            seen[start] = 1;
            for (std::size_t i = 0; i < run.size() && !again; ++i)
                for (const int other : f.across[std::size_t(run[i])])
                    if (other >= 0 && !seen[std::size_t(other)] && owner[std::size_t(other)] < 0
                        && f.normal[std::size_t(other)].Dot(f.normal[std::size_t(run[i])]) > smooth) {
                        seen[std::size_t(other)] = 1;
                        run.push_back(other);
                    }
            const std::size_t before = run.size();
            if (run.size() < 24)
                continue;
            V facing(0, 0, 0);
            for (const int t : run)
                facing += f.normal[std::size_t(t)] * f.area[std::size_t(t)];
            if (facing.Modulus() < 1e-12)
                continue;
            facing /= facing.Modulus();
            double spread = 1, steepest = 1;
            for (const int t : run)
                steepest = std::min(steepest, f.normal[std::size_t(t)].Dot(facing));
            for (const int t : run)
                spread = std::min(spread, f.normal[std::size_t(t)].Dot(f.normal[std::size_t(run.front())]));
            // Curved, and a height over that plane everywhere (no facet stood on edge to it).
            if (spread > std::cos(5 * M_PI / 180) || steepest < std::cos(75 * M_PI / 180))
                continue;
            // Over the drawing axis nearest its facing first, as the part was drawn (regularize's
            // rule): a paraboloid stays one of second degree only over a plane square to its own
            // axis, and spacer_mit_loch's is upright; tilted to its mean facing, it isn't. Then the
            // mean facing itself, for one drawn at a slant.
            std::vector<V> ways;
            for (const V &w : {V(1, 0, 0), V(0, 1, 0), V(0, 0, 1)})
                if (std::abs(w.Dot(facing)) > std::cos(45 * M_PI / 180))
                    ways.push_back(w.Dot(facing) > 0 ? w : w * -1);
            ways.push_back(facing);
            Surface bowl;
            std::vector<int> points;
            std::vector<std::array<double, 3>> uvh;
            std::vector<char> used;
            std::size_t usedCount = 0;
            double longest = 0;
            bool held = false;
            for (const V &way : ways) {
                bool over = true;      // a height over this plane everywhere
                for (const int t : run)
                    over = over && f.normal[std::size_t(t)].Dot(way) > std::cos(75 * M_PI / 180);
                if (!over)
                    continue;
                bowl.kind = Surface::Bowl;
                bowl.axis = way;
                bowl.across = way.Crossed(std::abs(way.X()) < 0.9 ? V(1, 0, 0) : V(0, 1, 0));
                bowl.across /= bowl.across.Modulus();
                std::set<int> pointSet;
                longest = 0;
                for (const int t : run) {
                    const auto &tri = mesh.triangles[std::size_t(t)];
                    pointSet.insert(tri.begin(), tri.end());
                    for (int k = 0; k < 3; ++k)
                        longest = std::max(longest, (pointOf(mesh, tri[std::size_t(k)]) - pointOf(mesh, tri[std::size_t((k + 1) % 3)])).Modulus());
                }
                points.assign(pointSet.begin(), pointSet.end());
                V middle(0, 0, 0);
                for (const int p : points)
                    middle += pointOf(mesh, p);
                bowl.origin = middle / double(points.size());
                const V y = bowl.axis.Crossed(bowl.across);
                uvh.clear();
                for (const int p : points) {
                    const V d = pointOf(mesh, p) - bowl.origin;
                    uvh.push_back({d.Dot(bowl.across), d.Dot(y), d.Dot(bowl.axis)});
                }
                used.assign(points.size(), 1);
                usedCount = points.size();
                for (int pass = 0; pass < 200; ++pass) {
                    math_Matrix a(1, 6, 1, 6, 0.0);
                    math_Vector b(1, 6, 0.0);
                    for (std::size_t i = 0; i < points.size(); ++i) {
                        if (!used[i])
                            continue;
                        const double u = uvh[i][0], v = uvh[i][1];
                        const double row[6] = {1, u, v, u * u, u * v, v * v};
                        for (int r = 0; r < 6; ++r) {
                            b(r + 1) += row[r] * uvh[i][2];
                            for (int c = 0; c < 6; ++c)
                                a(r + 1, c + 1) += row[r] * row[c];
                        }
                    }
                    math_Gauss solve(a);
                    if (!solve.IsDone())
                        break;
                    math_Vector q(1, 6);
                    solve.Solve(b, q);
                    for (int r = 0; r < 6; ++r)
                        bowl.q[std::size_t(r)] = q(r + 1);
                    std::vector<std::pair<double, std::size_t>> misses;
                    for (std::size_t i = 0; i < points.size(); ++i)
                        if (used[i])
                            misses.push_back({distanceTo(bowl, pointOf(mesh, points[i])), i});
                    std::sort(misses.rbegin(), misses.rend());
                    if (misses.front().first <= tight) {
                        held = true;
                        break;
                    }
                    // The worst twentieth set aside (at least one), only those past the tolerance.
                    const std::size_t drop = std::max<std::size_t>(1, misses.size() / 20);
                    for (std::size_t k = 0; k < drop && misses[k].first > tight; ++k) {
                        used[misses[k].second] = 0;
                        --usedCount;
                    }
                    // spacer_mit_loch's dome keeps 63 of its 233: walls, its hole and the facets' own
                    // T-joins put the rest on chords. What is set aside is checked below all the same.
                    if (usedCount * 5 < points.size() || usedCount < 12)
                        break;
                }
                if (held)
                    break;
            }
            if (!held)
                continue;
            // A chord's sag: its bend times its length squared over 8, with room for the longest facet.
            const double bend = 2 * std::max(std::abs(bowl.q[3]), std::abs(bowl.q[5])) + std::abs(bowl.q[4]);
            bowl.sag = bend * longest * longest / 8 * 1.5;
            bowl.radius = 1 / std::max(bend, 1e-12);
            // A point set aside may sit outside by what the file rounded it by: spacer_mit_loch is
            // written to 3 decimals (14.961 for 14.9609375), up to 0.001 mm out. No more than that.
            bool fits = true;
            for (std::size_t i = 0; i < points.size() && fits; ++i)
                fits = fitOff(bowl, pointOf(mesh, points[i])) <= (used[i] ? tight : std::max(tight, std::min(capMm, 2e-3)));
            for (std::size_t k = 0; k < run.size() && fits; ++k) {
                const int t = run[k];
                const auto &tri = mesh.triangles[std::size_t(t)];
                const V mid = (pointOf(mesh, tri[0]) + pointOf(mesh, tri[1]) + pointOf(mesh, tri[2])) / 3;
                fits = std::abs(f.normal[std::size_t(t)].Dot(normalAt(bowl, mid))) >= std::cos(35 * M_PI / 180);
            }
            if (!fits)
                continue;
            // It then takes in the triangles beside it that lie on it as its own set-aside points
            // do, flat leftovers or pieces of a round alike: Low Res starts a curve from 4 facets,
            // and strips of spacer_mit_loch's dome had become cylinders of r 41-52 with flats
            // between before the cap was fitted.
            std::set<int> inRun(run.begin(), run.end());
            for (std::size_t i = 0; i < run.size(); ++i)
                for (const int t : f.across[std::size_t(run[i])]) {
                    if (t < 0 || inRun.count(t) || (owner[std::size_t(t)] >= 0 && fitted[std::size_t(owner[std::size_t(t)])].kind == Surface::Bowl))
                        continue;
                    const auto &tri = mesh.triangles[std::size_t(t)];
                    bool on = std::abs(f.normal[std::size_t(t)].Dot(normalAt(bowl, (pointOf(mesh, tri[0]) + pointOf(mesh, tri[1]) + pointOf(mesh, tri[2])) / 3)))
                              >= std::cos(35 * M_PI / 180);
                    for (const int p : tri)
                        on = on && fitOff(bowl, pointOf(mesh, p)) <= std::max(tight, std::min(capMm, 2e-3));
                    if (!on)
                        continue;
                    inRun.insert(t);
                    run.push_back(t);
                }
            if (!again && run.size() > before) {
                grown = run;     // fitted again on them all, as a cap found whole is
                --start;
                continue;
            }
            double lo[2] = {INFINITY, INFINITY}, hi[2] = {-INFINITY, -INFINITY};
            for (const auto &w : uvh)
                for (int k = 0; k < 2; ++k) {
                    lo[k] = std::min(lo[k], w[std::size_t(k)]);
                    hi[k] = std::max(hi[k], w[std::size_t(k)]);
                }
            for (int k = 0; k < 2; ++k) {
                const double margin = std::max((hi[k] - lo[k]) * 0.05, 1e-3);
                bowl.box[2 * k] = lo[k] - margin;
                bowl.box[2 * k + 1] = hi[k] + margin;
            }
            double worst = 0;
            for (std::size_t i = 0; i < points.size(); ++i)
                if (used[i])
                    worst = std::max(worst, distanceTo(bowl, pointOf(mesh, points[i])));
            keep(bowl, run, worst);
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
    // Flats that are one face. (1) A step finer than the fit: a flat standing steep (over 45 degrees)
    // between a bigger flat and another piece of that very plane, every point of it within what a
    // fit allows of the plane, is the export's noise, not a feature, and becomes part of it (taken
    // beside any big flat, it also swallowed small facets at fillet ends: the 40 mm box 885 -> 1219
    // faces). (3) A flat of one facet, not steep, beside a bigger flat whose plane holds every point of
    // it as close: a thin facet's own facing says little (each key notch wall of the speaker mount's
    // base ended in a fan of five slivers 1 to 9 degrees off it, all within 0.005 mm of it, left as
    // flats whose loose edges kept Project from closing the outline). (2) Flats side by side that the pass above
    // made the very same plane, facing the same way, are one face. spacer_mit_loch's front wall is
    // y -29.700 below z 4.997 and -29.697 above it, both square to y, joined by a 0.003 mm ledge: it came
    // out as an upper and a lower face each side of the hole with the ledge between, and the corner
    // refits then tilted each half 0.25 degrees its own way (round 7, owner: "unless they are
    // measurably different planes it should be ONE face").
    {
        const double allowed = std::max(0.02, size * 2e-4) * 1.5;
        std::vector<int> root(surfaces.size());
        std::iota(root.begin(), root.end(), 0);
        const std::function<int(int)> find = [&](int l) { return root[std::size_t(l)] == l ? l : root[std::size_t(l)] = find(root[std::size_t(l)]); };
        std::vector<std::set<int>> pointsOf(surfaces.size());
        std::vector<double> areaOf(surfaces.size(), 0);
        std::vector<int> facets(surfaces.size(), 0);
        const auto flatAt = [&](long t) { return t >= 0 && owner[std::size_t(t)] < 0 && labels[std::size_t(t)] >= 0
                                                 && surfaces[std::size_t(labels[std::size_t(t)])].kind == Surface::Flat; };
        for (std::size_t t = 0; t < owner.size(); ++t)
            if (flatAt(long(t))) {
                pointsOf[std::size_t(labels[t])].insert(mesh.triangles[t].begin(), mesh.triangles[t].end());
                areaOf[std::size_t(labels[t])] += f.area[t];
                ++facets[std::size_t(labels[t])];
            }
        const auto join = [&](int into, int from) {
            root[std::size_t(from)] = into;
            pointsOf[std::size_t(into)].insert(pointsOf[std::size_t(from)].begin(), pointsOf[std::size_t(from)].end());
            areaOf[std::size_t(into)] += areaOf[std::size_t(from)];
        };
        // Which flats touch which, to tell a step between two pieces of one plane from a small face.
        std::vector<std::set<int>> touching(surfaces.size());
        for (std::size_t t = 0; t < owner.size(); ++t)
            if (flatAt(long(t)))
                for (int k = 0; k < 3; ++k)
                    if (const int o = f.across[t][std::size_t(k)]; flatAt(o) && labels[std::size_t(o)] != labels[t])
                        touching[std::size_t(labels[t])].insert(labels[std::size_t(o)]);
        const auto same = [&](const Surface &x, const Surface &y) { return x.axis.Dot(y.axis) == 1 && x.origin.Dot(x.axis) == y.origin.Dot(y.axis); };
        bool joined = false;
        for (bool again = true; again;) {
            again = false;
            for (std::size_t t = 0; t < owner.size(); ++t) {
                if (!flatAt(long(t)))
                    continue;
                for (int k = 0; k < 3; ++k) {
                    const int o = f.across[t][std::size_t(k)];
                    if (!flatAt(o))
                        continue;
                    const int a = find(labels[t]), b = find(labels[std::size_t(o)]);
                    if (a == b)
                        continue;
                    const Surface &sa = surfaces[std::size_t(a)], &sb = surfaces[std::size_t(b)];
                    if (same(sa, sb)) {
                        join(areaOf[std::size_t(a)] >= areaOf[std::size_t(b)] ? a : b, areaOf[std::size_t(a)] >= areaOf[std::size_t(b)] ? b : a);
                        again = joined = true;
                        continue;
                    }
                    const int big = areaOf[std::size_t(a)] >= areaOf[std::size_t(b)] ? a : b, step = big == a ? b : a;
                    const Surface &plane = surfaces[std::size_t(big)];
                    const bool steep = std::abs(plane.axis.Dot(surfaces[std::size_t(step)].axis)) <= std::cos(M_PI / 4);
                    bool between = !steep && facets[std::size_t(step)] == 1;
                    for (const int n : touching[std::size_t(step)])
                        between = between || (steep && find(n) != big && find(n) != step && same(surfaces[std::size_t(find(n))], plane));
                    if (!between)
                        continue;
                    double worst = 0;
                    for (const int p : pointsOf[std::size_t(step)])
                        worst = std::max(worst, distanceTo(plane, pointOf(mesh, p)));
                    if (worst > allowed)
                        continue;
                    forcedOff = std::max(forcedOff, worst);
                    join(big, step);
                    again = joined = true;
                }
            }
        }
        if (joined)
            for (std::size_t t = 0; t < owner.size(); ++t)
                if (flatAt(long(t))) {
                    if (find(labels[t]) != labels[t])
                        surfaces[std::size_t(find(labels[t]))].joined = true;
                    labels[t] = find(labels[t]);
                }
    }
    // A round between two flats is tangent to both, as a fillet is drawn (owner, round 6: the BIGGER
    // cube's corner rounds weren't). Fitted on its own through coarse facets it isn't quite: the
    // cube's three-facet edges came out r 2.8977, 3 cos 15 degrees, a hair off both flats and leaving
    // a crease. For a cylinder whose two biggest flat neighbours both run along its axis (within 2
    // degrees), the axis becomes the line the two flats cross along, its centre exactly r from each on
    // the side it was, and only r is fitted to its points (kept as below); the corners then meet as a
    // fillet's do.
    {
        std::vector<std::vector<int>> group(fitted.size());
        std::vector<std::map<int, double>> beside(fitted.size());
        for (std::size_t t = 0; t < owner.size(); ++t) {
            if (owner[t] < 0)
                continue;
            group[std::size_t(owner[t])].push_back(int(t));
            const auto &tri = mesh.triangles[t];
            for (int k = 0; k < 3; ++k) {
                const int other = f.across[t][std::size_t(k)];
                if (other < 0 || owner[std::size_t(other)] >= 0 || labels[std::size_t(other)] < 0)
                    continue;
                beside[std::size_t(owner[t])][labels[std::size_t(other)]] += (pointOf(mesh, tri[std::size_t(k)]) - pointOf(mesh, tri[std::size_t((k + 1) % 3)])).Modulus();
            }
        }
        for (std::size_t i = 0; i < fitted.size(); ++i) {
            Surface &round = fitted[i];
            if (round.kind != Surface::Cylinder || beside[i].size() < 2)
                continue;
            std::vector<std::pair<double, int>> flats;
            for (const auto &[label, length] : beside[i])
                if (surfaces[std::size_t(label)].kind == Surface::Flat && std::abs(surfaces[std::size_t(label)].axis.Dot(round.axis)) < std::sin(2 * M_PI / 180))
                    flats.push_back({length, label});
            if (flats.size() < 2)
                continue;
            std::sort(flats.rbegin(), flats.rend());
            const Surface &one = surfaces[std::size_t(flats[0].second)], &two = surfaces[std::size_t(flats[1].second)];
            V axis = one.axis.Crossed(two.axis);
            if (axis.Modulus() < std::sin(10 * M_PI / 180))
                continue;
            axis /= axis.Modulus();
            if (axis.Dot(round.axis) < 0)
                axis = axis * -1;
            const gp_Ax3 frame(gp_Pnt(round.origin), gp_Dir(axis));
            const V e1 = frame.XDirection().XYZ(), e2 = frame.YDirection().XYZ();
            const double d1 = one.axis.Dot(round.origin - one.origin), d2 = two.axis.Dot(round.origin - two.origin);
            const double s1 = d1 < 0 ? -1 : 1, s2 = d2 < 0 ? -1 : 1;
            // (alpha, beta) across the axis from the old centre: n_k . (e1 alpha + e2 beta) = s_k r - d_k.
            const double m11 = one.axis.Dot(e1), m12 = one.axis.Dot(e2), m21 = two.axis.Dot(e1), m22 = two.axis.Dot(e2);
            const double det = m11 * m22 - m12 * m21;
            if (std::abs(det) < 1e-9)
                continue;
            const auto centreFor = [&](double r) {
                const double b1 = s1 * r - d1, b2 = s2 * r - d2;
                return round.origin + e1 * ((b1 * m22 - b2 * m12) / det) + e2 * ((m11 * b2 - m21 * b1) / det);
            };
            std::set<int> pointSet;
            for (const int t : group[i])
                pointSet.insert(mesh.triangles[std::size_t(t)].begin(), mesh.triangles[std::size_t(t)].end());
            // The worst miss as the fit counts it (a point on a facet's chord may sit inside by the
            // sag): the facets' corners on the circle, the chord points inside it. Least squares over
            // all of them pulled the cube's corners from r 2.8977, where their corners lie, to 3.0055.
            const auto miss = [&](double r) {
                Surface trial = round;
                trial.axis = axis;
                trial.radius = r;
                trial.origin = centreFor(r);
                double worst = 0;
                for (const int p : pointSet)
                    worst = std::max(worst, fitOff(trial, pointOf(mesh, p)));
                return worst;
            };
            // r by golden section, from half to one and a half times the fitted one.
            double lo = round.radius * 0.5, hi = round.radius * 1.5;
            for (int k = 0; k < 100; ++k) {
                const double a = hi - (hi - lo) * 0.6180339887498949, b = lo + (hi - lo) * 0.6180339887498949;
                (miss(a) < miss(b) ? hi : lo) = (miss(a) < miss(b) ? b : a);
            }
            Surface tangent = round;
            tangent.radius = (lo + hi) / 2;
            tangent.axis = axis;
            tangent.origin = centreFor(tangent.radius);
            double before = 0, after = 0;
            for (const int p : pointSet) {
                before = std::max(before, fitOff(round, pointOf(mesh, p)));
                after = std::max(after, fitOff(tangent, pointOf(mesh, p)));
            }
            // Only where the tangent round is what the facets show: at least twice as close as the free
            // fit, or within 0.002 mm. A corner whose arc runs further along one flat than the other
            // (BIGGER's chamfer corners: 2.90 from one, 2.12 from the other) isn't a round between
            // those two, and tangent to them it fitted worse (r 2.92 -> 2.55); it keeps its own fit.
            if (after <= std::max(2e-3, before * 0.5)) {
                round = tangent;
                offBy[i] = std::max(offBy[i], after);
            }
        }
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
                // Each triangle covers the whole turn its corners span (the shortest way round them), not
                // just the 10 degree steps its corners and middle land in: a ring of 12 facets (30 degrees
                // each) left steps between empty, read as part-way round, got no seam, and went back to
                // facets (round 5).
                std::array<double, 3> spins;
                for (int k = 0; k < 3; ++k) {
                    const V at = pointOf(mesh, tri[std::size_t(k)]) - s.origin;
                    spins[std::size_t(k)] = std::atan2(at.Dot(frame.YDirection().XYZ()), at.Dot(frame.XDirection().XYZ())) + M_PI;
                }
                std::sort(spins.begin(), spins.end());
                int gapAfter = 2;
                double gap = spins[0] + 2 * M_PI - spins[2];
                for (int k = 0; k < 2; ++k)
                    if (spins[std::size_t(k + 1)] - spins[std::size_t(k)] > gap) {
                        gap = spins[std::size_t(k + 1)] - spins[std::size_t(k)];
                        gapAfter = k;
                    }
                const double from = spins[std::size_t((gapAfter + 1) % 3)], span = 2 * M_PI - gap;
                const int first = int(from / (2 * M_PI) * 36), last = int((from + span) / (2 * M_PI) * 36);
                for (int b = first; b <= last; ++b)
                    bins[std::size_t(owner[t])][std::size_t(b % 36)] = 1;
                bins[std::size_t(owner[t])][std::min(35, int(angle / (2 * M_PI) * 36))] = 1;
            }
            for (std::size_t i = 0; i < fitted.size(); ++i) {
                const bool full = fitted[i].kind != Surface::Screw && fitted[i].kind != Surface::Bowl && std::accumulate(bins[i].begin(), bins[i].end(), 0) >= 33;
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
        // No face too thin to hold (GB-090): a thin run of triangles goes to the face beside it.
        if (noThin)
            giveAwayThin(f, fitted, surfaces, owner, side, labels);

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
        TopoDS_Shape shape = assemble(mesh, outLabels, all, tolerance, stage, &made, !noThin);
        if (shape.IsNull())
            return {};
        stage("Joining faces to the surfaces");
        std::set<int> failed;
        try {
            ShapeFix_Edge edges;
            for (const auto &[face, label] : made)
                for (TopExp_Explorer e(face, TopAbs_EDGE); e.More(); e.Next())
                    {
                    // Flats split off while placing corners are numbered past `all`.
                    static const Surface splitOff;
                    const Surface &on = std::size_t(label) < all.size() ? all[std::size_t(label)] : splitOff;
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
            if (!badFlats.empty() && !forced) {
                std::set<int> only;
                for (std::size_t t = 0; t < labels.size(); ++t)
                    if (owner[t] < 0 && badFlats.count(outLabels[t]))
                        only.insert(int(t));
                splitRims(only);
                if (!slivers.empty())
                    return fitMesh(split, share, stage, report, threads, style, &slivers, noThin);
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
                // Turned right first: OpenCascade's merge refuses a solid with a face turned the
                // wrong way, and hinge_female's countersinks stayed in halves (round 6).
                turnRight(shape);
                // FreeCAD's way first (refine.h), then OpenCascade's for what it leaves.
                if (const TopoDS_Shape refined = refine::sameSurfaces(shape); !refined.IsNull())
                    shape = refined;
                // Within the engine's own merge rule for joins and cuts (heavyengine.cpp kMergeLinear and
                // kMergeAngular: 0.0001 mm and 0.00000001 rad), so flats a rounding error apart become one
                // (the lid's corners, round 5); at OpenCascade's own tolerance if that isn't sound.
                TopoDS_Shape unified = refine::unifyDrawable(shape, 1e-4, 1e-8);
                if (unified.IsNull() || !BRepCheck_Analyzer(unified).IsValid())
                    unified = refine::unifyDrawable(shape, Precision::Confusion(), Precision::Angular());
                if (!unified.IsNull() && BRepCheck_Analyzer(unified).IsValid())
                    shape = unified;
            }
        } catch (const Standard_Failure &) {
        }
        turnRight(shape);
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
            case Surface::Bowl: ++report.bowls; break;
            default: break;
            }
        }
        return shape;
    }
    return {};
}

// fitMesh only gives back a solid that checked sound, so that is not checked again (1.5 s on b205d).
bool thinHolds(const TopoDS_Shape &shape) {
    if (shape.IsNull())
        return false;
    for (TopExp_Explorer e(shape, TopAbs_VERTEX); e.More(); e.Next())
        if (BRep_Tool::Tolerance(TopoDS::Vertex(e.Current())) > kSliverLastMiss)
            return false;
    for (TopExp_Explorer e(shape, TopAbs_EDGE); e.More(); e.Next())
        if (BRep_Tool::Tolerance(TopoDS::Edge(e.Current())) > kSliverLastMiss)
            return false;
    return true;
}

TopoDS_Shape fit(const Mesh &mesh, double share, const Stage &stage, FitReport &report, Style style) {
    report = {};
    struct LowRes {
        explicit LowRes(bool on) { tLowRes = on; }
        ~LowRes() { tLowRes = false; }
    } lowRes(style == Style::LowRes);
    const bool several = pieces(mesh).size() > 1;
    return eachPiece(mesh, [&](const Mesh &piece) {
        FitReport one;
        TopoDS_Shape shape = fitMesh(piece, share, stage, one, nullptr, style);
        // Thin faces given away (GB-090) where that holds: no solid (fitMesh gives none unsound), or a corner looser
        // than the give-away allows (roundedBox1's facet strips: no solid, or 2 mm loose), and the
        // piece is fitted again as before, thin faces and all.
        if (!thinHolds(shape)) {
            FitReport again;
            if (TopoDS_Shape before = fitMesh(piece, share, stage, again, nullptr, style, nullptr, false); !before.IsNull()) {
                shape = before;
                one = again;
            }
        }
        if (!shape.IsNull()) {
            report.cylinders += one.cylinders, report.cones += one.cones, report.spheres += one.spheres, report.bowls += one.bowls;
            report.worstMm = std::max(report.worstMm, one.worstMm);
            report.facetsLeft += one.facetsLeft;
        } else if (several) {
            // One piece that won't fit stays as it came in; the others are still fitted.
            shape = solidOfOne(piece, stage);
            report.facetsLeft += int(piece.triangles.size());
        }
        return shape;
    });
}

} // namespace meshsolid
