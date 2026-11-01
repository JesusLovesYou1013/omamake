#include "sketchgeometry.h"

#include <QFont>
#include <QPainterPath>
#include <QPolygonF>

#include <algorithm>
#include <cmath>
#include <limits>

namespace {

// Solve a·y = b in place (Gaussian elimination, partial pivoting); a is n×n, row by row.
bool solveLinear(std::vector<double> a, std::vector<double> &b, std::size_t n) {
    for (std::size_t col = 0; col < n; ++col) {
        std::size_t pivot = col;
        for (std::size_t r = col + 1; r < n; ++r)
            if (std::abs(a[r * n + col]) > std::abs(a[pivot * n + col]))
                pivot = r;
        if (std::abs(a[pivot * n + col]) < 1e-18)
            return false;
        if (pivot != col) {
            for (std::size_t c = 0; c < n; ++c)
                std::swap(a[col * n + c], a[pivot * n + c]);
            std::swap(b[col], b[pivot]);
        }
        for (std::size_t r = col + 1; r < n; ++r) {
            const double f = a[r * n + col] / a[col * n + col];
            for (std::size_t c = col; c < n; ++c)
                a[r * n + c] -= f * a[col * n + c];
            b[r] -= f * b[col];
        }
    }
    for (std::size_t r = n; r-- > 0;) {
        double s = b[r];
        for (std::size_t c = r + 1; c < n; ++c)
            s -= a[r * n + c] * b[c];
        b[r] = s / a[r * n + r];
    }
    return true;
}

} // namespace

// One number per constraint, 0 when it holds: the sine of the angle between the lines
// (parallel) or its cosine (perpendicular), so long and short lines count the same.
// Fix gives two: how far the point is from where it's held, across and up.
// Horizontal / Vertical: the sine of the line's angle to the sketch's X / Y axis.
// Dimensions and Equal: how many mm off they are.
SketchGeometry::Point SketchGeometry::point(int id) const {
    if (id == Origin)
        return origin;
    if (id == -2)
        return {origin.u + 1, origin.v};
    if (id == -3)
        return {origin.u, origin.v + 1};
    return points.at(id);
}

SketchGeometry::Line SketchGeometry::line(int id) const {
    if (id == XAxis)
        return {Origin, -2};
    if (id == YAxis)
        return {Origin, -3};
    return lines.at(id);
}

std::vector<double> SketchGeometry::residuals(const std::vector<double> &x, const std::vector<int> &ids) const {
    auto index = [&](int id) { return std::size_t(std::lower_bound(ids.begin(), ids.end(), id) - ids.begin()); };
    // A point where x has it; the origin and the axes' ends where they always are.
    auto at = [&](int pointId, double &px, double &py) {
        if (isReference(pointId) || projectedPoints.count(pointId)) {
            const Point p = point(pointId);
            px = p.u;
            py = p.v;
            return;
        }
        const std::size_t k = index(pointId);
        px = x[k * 2];
        py = x[k * 2 + 1];
    };
    auto direction = [&](int lineId, double &dx, double &dy) {
        const Line l = line(lineId);
        double ax, ay, bx, by;
        at(l.a, ax, ay);
        at(l.b, bx, by);
        dx = bx - ax;
        dy = by - ay;
        return std::hypot(dx, dy);
    };
    // An arc's centre and radius where x has them.
    auto circleOf = [&](int arcId, double &cx, double &cy) {
        const Arc &k = arcs.at(arcId);
        double ax, ay;
        at(k.c, cx, cy);
        at(k.a, ax, ay);
        return std::hypot(ax - cx, ay - cy);
    };
    std::vector<double> out;
    double x1h, y1h;
    // An ellipse keeps its two axes square to each other.
    for (const auto &[id, k] : curves) {
        if (k.type != QLatin1String("ellipse") || k.points.size() < 3)
            continue;
        double cx, cy, ax, ay, bx, by;
        at(k.points[0], cx, cy);
        at(k.points[1], ax, ay);
        at(k.points[2], bx, by);
        const double la = std::hypot(ax - cx, ay - cy), lb = std::hypot(bx - cx, by - cy);
        out.push_back(la < 1e-9 || lb < 1e-9 ? 0 : ((ax - cx) * (bx - cx) + (ay - cy) * (by - cy)) / (la * lb));
    }
    // Arcs hold themselves: both ends as far from the centre; a circle's rim point level with it.
    for (const auto &[id, arc] : arcs) {
        double cx, cy, ax, ay, bx, by;
        at(arc.c, cx, cy);
        at(arc.a, ax, ay);
        if (arc.circle()) {
            out.push_back(ay - cy);
            continue;
        }
        at(arc.b, bx, by);
        out.push_back(std::hypot(bx - cx, by - cy) - std::hypot(ax - cx, ay - cy));
    }
    for (const auto &[id, c] : constraints) {
        if (c.type == QLatin1String("heading")) {      // a drag's own: line first keeps direction (u, v)
            const double l = direction(c.first, x1h, y1h);
            out.push_back(l < 1e-9 ? 0 : (x1h * c.v - y1h * c.u) / l);
            continue;
        }
        if (c.type == QLatin1String("fix")) {
            const std::size_t p = index(c.first);
            out.push_back(x[p * 2] - c.u);
            out.push_back(x[p * 2 + 1] - c.v);
            continue;
        }
        double x1, y1, x2, y2;
        if (c.type == QLatin1String("distance")) {
            double ax, ay, bx, by;
            at(c.first, ax, ay);
            at(c.second, bx, by);
            out.push_back(std::hypot(bx - ax, by - ay) - valueOf(c));
            continue;
        }
        if (c.type == QLatin1String("length")) {
            out.push_back(direction(c.first, x1, y1) - valueOf(c));
            continue;
        }
        if (c.type == QLatin1String("radius") || c.type == QLatin1String("diameter")) {
            double cx, cy;
            const double r = circleOf(c.first, cx, cy);
            out.push_back((c.type == QLatin1String("radius") ? r : 2 * r) - valueOf(c));
            continue;
        }
        if (c.type == QLatin1String("equalradius")) {
            double cx, cy, dx, dy;
            out.push_back(circleOf(c.first, cx, cy) - circleOf(c.second, dx, dy));
            continue;
        }
        if (c.type == QLatin1String("concentric") || c.type == QLatin1String("offsetarcs")) {
            double cx, cy, dx, dy;
            const double r1 = circleOf(c.first, cx, cy), r2 = circleOf(c.second, dx, dy);
            out.push_back(dx - cx);
            out.push_back(dy - cy);
            if (c.type == QLatin1String("offsetarcs"))
                out.push_back(r2 - r1 - valueOf(c) * (c.u ? c.u : 1));
            continue;
        }
        if (c.type == QLatin1String("pointonarc")) {
            double px, py, cx, cy;
            at(c.first, px, py);
            const double r = circleOf(c.second, cx, cy);
            out.push_back(std::hypot(px - cx, py - cy) - r);
            continue;
        }
        if (c.type == QLatin1String("tangentarcs")) {
            double cx, cy, dx, dy;
            const double r1 = circleOf(c.first, cx, cy), r2 = circleOf(c.second, dx, dy);
            out.push_back(std::hypot(dx - cx, dy - cy) - (c.u > 0.5 ? r1 + r2 : std::abs(r1 - r2)));
            continue;
        }
        if (c.type == QLatin1String("midpoints") || c.type == QLatin1String("rotation")
            || c.type == QLatin1String("translation")) {
            double px, py, qx, qy, ox, oy;
            at(c.first, px, py);
            at(c.second, qx, qy);
            if (c.type == QLatin1String("translation")) {
                out.push_back(qx - px - c.u);
                out.push_back(qy - py - c.v);
            } else if (c.type == QLatin1String("midpoints")) {
                at(c.third, ox, oy);           // first = the middle of second and third
                out.push_back(px - (qx + ox) / 2);
                out.push_back(py - (qy + oy) / 2);
            } else {
                at(c.third, ox, oy);
                const double cs = std::cos(c.value), sn = std::sin(c.value);
                out.push_back(qx - (ox + cs * (px - ox) - sn * (py - oy)));
                out.push_back(qy - (oy + sn * (px - ox) + cs * (py - oy)));
            }
            continue;
        }
        if (c.type == QLatin1String("angle")) {
            const double l1 = direction(c.first, x1, y1), l2 = direction(c.second, x2, y2);
            if (c.u < 0) {                     // the other side of the corner
                x2 = -x2;
                y2 = -y2;
            }
            out.push_back(l1 < 1e-9 || l2 < 1e-9 ? 0
                          : std::atan2(std::abs(x1 * y2 - y1 * x2), x1 * x2 + y1 * y2) - valueOf(c));
            continue;
        }
        if (c.type == QLatin1String("pointline")) {
            const Line l = line(c.second);
            double px, py, ax, ay;
            at(c.first, px, py);
            at(l.a, ax, ay);
            const double len = direction(c.second, x1, y1);
            out.push_back(len < 1e-9 ? 0 : std::abs(x1 * (py - ay) - y1 * (px - ax)) / len - valueOf(c));
            continue;
        }
        // Signed distance (mm) from a point to a line's extension, and the point's position along it.
        auto across = [&](int pointId, int lineId) {
            const Line l = line(lineId);
            double px, py, ax, ay, dx, dy;
            at(pointId, px, py);
            at(l.a, ax, ay);
            const double len = direction(lineId, dx, dy);
            return len < 1e-9 ? 0.0 : (dx * (py - ay) - dy * (px - ax)) / len;
        };
        // Two places mirrored across a line: their middle on it, and the gap square to it.
        auto mirrored = [&](double px, double py, double qx, double qy, int axis) {
            const Line l = line(axis);
            double ax, ay, dx, dy;
            at(l.a, ax, ay);
            const double len = direction(axis, dx, dy);
            if (len < 1e-9) {
                out.push_back(0);
                out.push_back(0);
                return;
            }
            const double mx = (px + qx) / 2, my = (py + qy) / 2;
            out.push_back((dx * (my - ay) - dy * (mx - ax)) / len);
            out.push_back((dx * (qx - px) + dy * (qy - py)) / len);
        };
        if (c.type == QLatin1String("horizontalpoints") || c.type == QLatin1String("verticalpoints")) {
            double ax, ay, bx, by;
            at(c.first, ax, ay);
            at(c.second, bx, by);
            out.push_back(c.type == QLatin1String("horizontalpoints") ? by - ay : bx - ax);
            continue;
        }
        if (c.type == QLatin1String("coincident")) {
            double ax, ay, bx, by;
            at(c.first, ax, ay);
            at(c.second, bx, by);
            out.push_back(bx - ax);
            out.push_back(by - ay);
            continue;
        }
        if (c.type == QLatin1String("pointonline")) {
            out.push_back(across(c.first, c.second));
            continue;
        }
        if (c.type == QLatin1String("tangent")) {
            const Arc &k = arcs.at(c.second);
            double cx, cy;
            const double r = circleOf(c.second, cx, cy);
            out.push_back(std::abs(across(k.c, c.first)) - r);
            continue;
        }
        if (c.type == QLatin1String("offset")) {
            const Line m = line(c.second);
            const double want = valueOf(c) * (c.u ? c.u : 1);
            out.push_back(across(m.a, c.first) - want);
            out.push_back(across(m.b, c.first) - want);
            continue;
        }
        if (c.type == QLatin1String("midpoint")) {
            const Line l = line(c.second);
            double px, py, ax, ay, bx, by;
            at(c.first, px, py);
            at(l.a, ax, ay);
            at(l.b, bx, by);
            out.push_back(px - (ax + bx) / 2);
            out.push_back(py - (ay + by) / 2);
            continue;
        }
        if (c.type == QLatin1String("collinear")) {
            const Line m = line(c.second);
            out.push_back(across(m.a, c.first));
            out.push_back(across(m.b, c.first));
            continue;
        }
        if (c.type == QLatin1String("symmetry")) {
            double px, py, qx, qy;
            at(c.first, px, py);
            at(c.second, qx, qy);
            mirrored(px, py, qx, qy, c.third);
            continue;
        }
        if (c.type == QLatin1String("symmetrylines")) {
            const Line l = line(c.first), m = line(c.second);
            const int pairA = c.u > 0.5 ? m.b : m.a, pairB = c.u > 0.5 ? m.a : m.b;
            double px, py, qx, qy;
            at(l.a, px, py);
            at(pairA, qx, qy);
            mirrored(px, py, qx, qy, c.third);
            at(l.b, px, py);
            at(pairB, qx, qy);
            mirrored(px, py, qx, qy, c.third);
            continue;
        }
        if (c.type == QLatin1String("lineline")) {
            const Line m = line(c.second);
            double ax, ay, bx, by, ox, oy;
            at(m.a, ax, ay);
            at(m.b, bx, by);
            at(line(c.first).a, ox, oy);
            const double px = (ax + bx) / 2, py = (ay + by) / 2;
            const double len = direction(c.first, x1, y1);
            out.push_back(len < 1e-9 ? 0 : std::abs(x1 * (py - oy) - y1 * (px - ox)) / len - valueOf(c));
            continue;
        }
        if (c.type == QLatin1String("equal")) {
            out.push_back(direction(c.first, x1, y1) - direction(c.second, x2, y2));
            continue;
        }
        if (c.type == QLatin1String("horizontal") || c.type == QLatin1String("vertical")) {
            const double l = direction(c.first, x1, y1);
            out.push_back(l < 1e-9 ? 0 : (c.type == QLatin1String("horizontal") ? y1 : x1) / l);
            continue;
        }
        const double l1 = direction(c.first, x1, y1), l2 = direction(c.second, x2, y2);
        if (l1 < 1e-9 || l2 < 1e-9) {
            out.push_back(0);
            continue;
        }
        out.push_back(c.type == QLatin1String("parallel") ? (x1 * y2 - y1 * x2) / (l1 * l2)
                                                          : (x1 * x2 + y1 * y2) / (l1 * l2));
    }
    return out;
}

double SketchGeometry::error() const {
    std::vector<int> ids;
    std::vector<double> x;
    for (const auto &[id, p] : points) {
        if (projectedPoints.count(id))
            continue;
        ids.push_back(id);
        x.push_back(p.u);
        x.push_back(p.v);
    }
    double sum = 0;
    for (double r : residuals(x, ids))
        sum += r * r;
    return std::sqrt(sum);
}

// Gauss–Newton for the smallest (weighted) move from where the points are that meets the
// constraints: each step x = x0 + W Jᵀ (J W Jᵀ)⁻¹ (J (x − x0) − r), W = how freely each
// may move. The smallest move from where the points are keeps the drawing as
// close as it can be to what was drawn.
bool SketchGeometry::solve(const std::set<int> &steady) {
    std::map<int, double> freedom;
    for (int id : steady)
        freedom[id] = 1e-6;
    return solveWeighted(freedom);
}

// How freely each point may move: 1 unless `weights` says otherwise.
bool SketchGeometry::solveWeighted(const std::map<int, double> &weights) {
    if (holdsNothing())
        return true;
    std::vector<int> ids;
    std::vector<double> x, freedom;
    for (const auto &[id, p] : points) {
        if (projectedPoints.count(id))
            continue;
        ids.push_back(id);
        x.push_back(p.u);
        x.push_back(p.v);
        const auto w = weights.find(id);
        const double f = w == weights.end() ? 1.0 : w->second;
        freedom.push_back(f);
        freedom.push_back(f);
    }
    const std::size_t n = x.size();
    const std::vector<double> x0 = x;
    double lastWorst = INFINITY;
    const double tolerance = 1e-8;      // angles in sine terms, Fix in mm: far below anything visible

    for (int iteration = 0; iteration < 200; ++iteration) {
        const std::vector<double> r = residuals(x, ids);
        const std::size_t m = r.size();
        double worst = 0;
        for (double v : r)
            worst = std::max(worst, std::abs(v));
        // Done when it holds, or when it has stopped getting closer and what's left is far
        // too small to see (constraints that repeat each other can leave a hair).
        const bool stalled = worst < 1e-6 && worst >= lastWorst * 0.99;
        lastWorst = worst;
        if (worst < tolerance || stalled) {
            for (std::size_t i = 0; i < ids.size(); ++i)
                points[ids[i]] = {x[i * 2], x[i * 2 + 1]};
            return true;
        }
        // J by small nudges either way, column by column.
        std::vector<double> jac(m * n);
        for (std::size_t j = 0; j < n; ++j) {
            const double h = 1e-6 * std::max(1.0, std::abs(x[j]));
            std::vector<double> up = x, down = x;
            up[j] += h;
            down[j] -= h;
            const std::vector<double> ru = residuals(up, ids), rd = residuals(down, ids);
            for (std::size_t i = 0; i < m; ++i)
                jac[i * n + j] = (ru[i] - rd[i]) / (2 * h);
        }
        std::vector<double> a(m * m, 0.0);
        for (std::size_t i = 0; i < m; ++i)
            for (std::size_t k = 0; k < m; ++k) {
                double s = 0;
                for (std::size_t j = 0; j < n; ++j)
                    s += jac[i * n + j] * freedom[j] * jac[k * n + j];
                a[i * m + k] = s;
            }
        // A touch on the diagonal copes with constraints that repeat each other. Scaled to
        // the matrix: a fixed amount stalled the solve when the points that could move
        // were all ones asked to stay put (their tiny weights shrink every entry).
        double biggest = 0;
        for (std::size_t i = 0; i < m; ++i)
            biggest = std::max(biggest, a[i * m + i]);
        for (std::size_t i = 0; i < m; ++i)
            a[i * m + i] += 1e-12 * std::max(biggest, 1e-300);
        // Measured from where the points started (x0), not from the last step, so the
        // answer really is the smallest move overall and doesn't drift on the way.
        std::vector<double> y(m);
        for (std::size_t i = 0; i < m; ++i) {
            double s = 0;
            for (std::size_t j = 0; j < n; ++j)
                s += jac[i * n + j] * (x[j] - x0[j]);
            y[i] = s - r[i];
        }
        if (!solveLinear(a, y, m))
            return false;
        for (std::size_t j = 0; j < n; ++j) {
            double s = 0;
            for (std::size_t i = 0; i < m; ++i)
                s += jac[i * n + j] * y[i];
            x[j] = x0[j] + freedom[j] * s;
        }
    }
    return false;
}

bool SketchGeometry::tiedToBoth() const {
    if (projectedPoints.empty())
        return false;
    std::map<int, int> parent;
    std::function<int(int)> root = [&](int p) {
        const auto found = parent.find(p);
        if (found == parent.end() || found->second == p)
            return parent[p] = p;
        return found->second = root(found->second);
    };
    const auto join = [&](int a, int b) { parent[root(a)] = root(b); };
    for (const auto &[id, l] : lines)
        join(l.a, l.b);
    for (const auto &[id, k] : arcs) {
        join(k.c, k.a);
        join(k.c, k.b);
    }
    for (const auto &[id, k] : curves)
        for (const int p : k.points)
            join(k.points.front(), p);
    for (const auto &[id, c] : constraints) {
        const std::vector<int> tied = pointsOf(c);
        for (const int p : tied)
            join(tied.front(), p);
    }
    std::set<int> toModel;
    for (const int p : projectedPoints)
        toModel.insert(root(p));
    for (const auto &[p, up] : std::map<int, int>(parent))
        if (p < 0 && toModel.count(root(p)))
            return true;
    return false;
}

bool SketchGeometry::isFixed(int point) const {
    if (projectedPoints.count(point))
        return true;
    for (const auto &[id, c] : constraints)
        if (c.type == QLatin1String("fix") && c.first == point)
            return true;
    return false;
}

// Moving as few points as possible (owner, 2026-10-29): first only the dragged points and
// their nearest neighbours may move (joined by a line, or by a constraint between their
// lines); the rest are held. If the constraints can't be met like that, the ring of points
// allowed to move grows one step at a time, and in the end everything may.
bool SketchGeometry::drag(const std::map<int, Point> &targets) {
    const std::map<int, Point> before = points;
    std::set<int> dragged;
    for (const auto &[id, at] : targets) {
        if (!points.count(id) || isFixed(id))
            continue;
        points[id] = at;
        dragged.insert(id);
    }
    if (dragged.empty())
        return false;
    // A circle or arc dragged whole that's tangent to one that isn't dragged goes round it,
    // touching, instead of pushing it away or changing either size (owner, 2026-10-30).
    const auto whole = [&](int arc) {
        const Arc &k = arcs.at(arc);
        return dragged.count(k.c) && dragged.count(k.a) && dragged.count(k.b);
    };
    const auto untouched = [&](const std::vector<int> &ids) {
        for (int id : ids)
            if (dragged.count(id))
                return false;
        return true;
    };
    for (const auto &[id, c] : constraints) {
        if (c.type == QLatin1String("tangentarcs") && arcs.count(c.first) && arcs.count(c.second)) {
            const Arc &k1 = arcs.at(c.first), &k2 = arcs.at(c.second);
            if (whole(c.first) && untouched({k2.c, k2.a, k2.b}))
                touch(c, c.first);
            else if (whole(c.second) && untouched({k1.c, k1.a, k1.b}))
                touch(c, c.second);
        } else if (c.type == QLatin1String("tangent") && arcs.count(c.second) && hasLine(c.first)
                   && whole(c.second) && untouched({line(c.first).a, line(c.first).b})) {
            touch(c, c.second);
        }
    }
    if (holdsNothing())
        return true;
    // Size first, then shape, then place (owner, 2026-10-30). In this order, the first that
    // works: (1) only what was grabbed changes (its lines' other ends, its arc's points) while
    // every other line keeps its direction, so it stretches; (2) the same, its angles free;
    // (3) things further out may stretch too, every line not being dragged keeping its
    // direction; (4) anything, ring by ring, so moving whole shapes comes last.
    std::set<int> own = dragged;
    for (const auto &[lid, l] : lines)
        if (dragged.count(l.a) || dragged.count(l.b)) {
            own.insert(l.a);
            own.insert(l.b);
        }
    for (const auto &[aid, k] : arcs)
        if (dragged.count(k.c) || dragged.count(k.a) || dragged.count(k.b))
            own.insert({k.c, k.a, k.b});
    // Every line not being dragged keeps its direction (temporary constraints, taken off again).
    std::vector<int> held;
    const auto hold = [&] {
        int temp = std::numeric_limits<int>::max();
        for (const auto &[lid, l] : lines) {
            if (dragged.count(l.a) || dragged.count(l.b))
                continue;
            const Point a = points.at(l.a), b = points.at(l.b);
            const double len = std::hypot(b.u - a.u, b.v - a.v);
            if (len < 1e-9)
                continue;
            constraints[temp] = Constraint(QStringLiteral("heading"), lid, 0, (b.u - a.u) / len, (b.v - a.v) / len);
            held.push_back(temp--);
        }
    };
    const auto unhold = [&] {
        for (int id : held)
            constraints.erase(id);
        held.clear();
    };
    hold();
    const bool grabbedStretched = solveOnly(own, dragged);
    unhold();
    if (grabbedStretched || solveOnly(own, dragged))
        return true;
    hold();
    const bool stretched = !held.empty() && solveRings(dragged, dragged, true);
    unhold();
    if (stretched || solveRings(dragged, dragged, true))
        return true;
    points = before;
    return false;
}

// A circle or arc dragged by its edge (owner, 2026-10-30: size first): it takes radius r, its
// centre staying where it can; only its own points move if that works, else further out ring
// by ring. false (nothing moved) when its size is held.
bool SketchGeometry::resize(int arc, double r) {
    if (!arcs.count(arc) || !(r > 0) || projectedPoints.count(arcs.at(arc).c))
        return false;
    const std::map<int, Point> before = points;
    const Arc &k = arcs.at(arc);
    const std::set<int> own{k.c, k.a, k.b};
    const int temp = std::numeric_limits<int>::max();
    Constraint size(QStringLiteral("radius"), arc);
    size.value = r;
    constraints[temp] = size;
    // The edge alone (the centre stays), then the centre too, then further out.
    const bool done = solveOnly({k.a, k.b}, {}) || solveOnly(own, {}) || solveRings(own, {}, false);
    constraints.erase(temp);
    if (!done)
        points = before;
    return done;
}

// Only the points in `ring` may move (those in `steady` hardly); the rest must stay put.
// false (and nothing moved) when that can't work.
bool SketchGeometry::solveOnly(const std::set<int> &ring, const std::set<int> &steady) {
    const std::map<int, Point> start = points;
    std::map<int, double> freedom;
    for (const auto &[id, p] : points)
        freedom[id] = steady.count(id) ? 1e-6 : ring.count(id) ? 1.0 : 1e-9;
    if (solveWeighted(freedom)) {
        bool held = true;
        for (const auto &[id, p] : start)
            if (!ring.count(id) && std::hypot(points[id].u - p.u, points[id].v - p.v) > 1e-6)
                held = false;
        if (held)
            return true;
    }
    points = start;
    return false;
}

void SketchGeometry::touch(const Constraint &c, int moving) {
    const Arc &k = arcs.at(moving);
    const Point o = point(k.c);
    double du = 0, dv = 0;
    if (c.type == QLatin1String("tangentarcs")) {
        const Point q = point(arcs.at(c.first == moving ? c.second : c.first).c);
        const double d = std::hypot(o.u - q.u, o.v - q.v), r1 = radius(c.first), r2 = radius(c.second);
        const double want = c.u > 0.5 ? r1 + r2 : std::abs(r1 - r2);
        const double eu = d < 1e-9 ? 1 : (o.u - q.u) / d, ev = d < 1e-9 ? 0 : (o.v - q.v) / d;
        du = eu * (want - d);
        dv = ev * (want - d);
    } else {
        const Point a = point(line(c.first).a), b = point(line(c.first).b);
        const double len = std::hypot(b.u - a.u, b.v - a.v);
        if (len < 1e-9)
            return;
        const double nu = -(b.v - a.v) / len, nv = (b.u - a.u) / len;   // square to the line
        const double s = nu * (o.u - a.u) + nv * (o.v - a.v), r = radius(moving);
        const double want = s < 0 ? -r : r;
        du = nu * (want - s);
        dv = nv * (want - s);
    }
    for (int id : std::set<int>{k.c, k.a, k.b}) {
        points[id].u += du;
        points[id].v += dv;
    }
}

bool SketchGeometry::solveNear(const std::set<int> &seeds) {
    const std::map<int, Point> before = points;
    if (solveRings(seeds, {}, false))
        return true;
    points = before;
    return false;
}

bool SketchGeometry::solveNear(const std::set<int> &seeds, const std::set<int> &steady) {
    const std::map<int, Point> before = points;
    if (solveRings(seeds, steady, false))
        return true;
    points = before;
    if (!steady.empty() && solveRings(seeds, {}, false))
        return true;
    points = before;
    return false;
}

// Nudged from one corner, an even polygon throws the solver about until it gives up: its sides and
// tangents repeat each other, so the steps have nothing to hold them. Slid whole, nothing has to be
// solved at all. Only for a shape that's free: not tied to `to`, the origin, a Fix or projected points.
bool SketchGeometry::slideTogether(int moving, int to) {
    std::map<int, std::set<int>> next;
    const auto tie = [&](const std::vector<int> &group) {
        for (int a : group)
            for (int b : group)
                if (a != b)
                    next[a].insert(b);
    };
    for (const auto &[id, l] : lines)
        tie({l.a, l.b});
    for (const auto &[id, arc] : arcs)
        tie({arc.c, arc.a, arc.b});
    for (const auto &[id, k] : curves)
        tie(k.points);
    for (const auto &[id, c] : constraints)
        tie(pointsOf(c));
    std::set<int> shape{moving};
    std::vector<int> open{moving};
    while (!open.empty()) {
        const int p = open.back();
        open.pop_back();
        for (int n : next[p])
            if (shape.insert(n).second)
                open.push_back(n);
    }
    if (shape.count(to))
        return false;
    for (int p : shape)
        if (!points.count(p) || projectedPoints.count(p))
            return false;
    for (const auto &[id, c] : constraints)
        if (c.type == QLatin1String("fix") && shape.count(c.first))
            return false;
    const Point from = point(moving), target = point(to);
    for (int p : shape) {
        points[p].u += target.u - from.u;
        points[p].v += target.v - from.v;
    }
    return true;
}

QString SketchGeometry::kinds(const QString &type) {
    static const std::map<QString, QString> made = {
        {QStringLiteral("parallel"), QStringLiteral("LL")}, {QStringLiteral("perpendicular"), QStringLiteral("LL")},
        {QStringLiteral("equal"), QStringLiteral("LL")}, {QStringLiteral("collinear"), QStringLiteral("LL")},
        {QStringLiteral("horizontal"), QStringLiteral("L")}, {QStringLiteral("vertical"), QStringLiteral("L")},
        {QStringLiteral("horizontalpoints"), QStringLiteral("PP")}, {QStringLiteral("verticalpoints"), QStringLiteral("PP")},
        {QStringLiteral("fix"), QStringLiteral("P")}, {QStringLiteral("coincident"), QStringLiteral("PP")},
        {QStringLiteral("pointonline"), QStringLiteral("PL")}, {QStringLiteral("midpoint"), QStringLiteral("PL")},
        {QStringLiteral("symmetry"), QStringLiteral("PPL")}, {QStringLiteral("symmetrylines"), QStringLiteral("LLL")},
        {QStringLiteral("length"), QStringLiteral("L")}, {QStringLiteral("distance"), QStringLiteral("PP")},
        {QStringLiteral("pointline"), QStringLiteral("PL")}, {QStringLiteral("lineline"), QStringLiteral("LL")},
        {QStringLiteral("tangent"), QStringLiteral("LA")}, {QStringLiteral("tangentarcs"), QStringLiteral("AA")},
        {QStringLiteral("concentric"), QStringLiteral("AA")}, {QStringLiteral("equalradius"), QStringLiteral("AA")},
        {QStringLiteral("pointonarc"), QStringLiteral("PA")}, {QStringLiteral("radius"), QStringLiteral("A")},
        {QStringLiteral("diameter"), QStringLiteral("A")}, {QStringLiteral("angle"), QStringLiteral("LL")},
        {QStringLiteral("midpoints"), QStringLiteral("PPP")}, {QStringLiteral("rotation"), QStringLiteral("PPP")},
        {QStringLiteral("translation"), QStringLiteral("PP")}, {QStringLiteral("offset"), QStringLiteral("LL")},
        {QStringLiteral("offsetarcs"), QStringLiteral("AA")},
    };
    const auto found = made.find(type);
    return found == made.end() ? QString() : found->second;
}

std::vector<int> SketchGeometry::pointsOf(const Constraint &c) const {
    std::vector<int> out;
    // A leader moves its followers: their points count as its own.
    if (c.isDimension() && !c.follows())
        for (const auto &[id, k] : constraints)
            if (k.follows() && constraints.count(k.third) && &constraints.at(k.third) == &c)
                for (int p : pointsOf(k))
                    out.push_back(p);
    const QString made = kinds(c.type);
    const int ids[3] = {c.first, c.second, c.third};
    for (int i = 0; i < made.size() && i < 3; ++i) {
        if (made[i] == QLatin1Char('P')) {
            out.push_back(ids[i]);
        } else if (made[i] == QLatin1Char('A')) {
            if (arcs.count(ids[i])) {
                const Arc &k = arcs.at(ids[i]);
                out.push_back(k.c);
                out.push_back(k.a);
                if (!k.circle())
                    out.push_back(k.b);
            }
        } else if (hasLine(ids[i])) {
            out.push_back(line(ids[i]).a);
            out.push_back(line(ids[i]).b);
        }
    }
    return out;
}

// Ring by ring: the points in `ring` may move (those in `steady` hardly), the rest are held
// and checked to have stayed put; when that can't work the ring takes in its neighbours
// (sharing a line, or tied by a constraint), until in the end everything may move.
// growFirst: start one ring out (a drag's own points never give way on their own).
bool SketchGeometry::solveRings(std::set<int> ring, const std::set<int> &steady, bool growFirst) {
    std::map<int, std::set<int>> next;   // point → the points it's tied to
    auto tie = [&](const std::vector<int> &group) {
        for (int a : group)
            for (int b : group)
                if (a != b)
                    next[a].insert(b);
    };
    for (const auto &[id, l] : lines)
        tie({l.a, l.b});
    for (const auto &[id, arc] : arcs)
        tie({arc.c, arc.a, arc.b});
    for (const auto &[id, k] : curves)
        tie(k.points);
    for (const auto &[id, c] : constraints)
        tie(pointsOf(c));

    const std::map<int, Point> start = points;
    bool grow = growFirst;
    while (true) {
        std::set<int> grown = ring;
        if (grow)
            for (int id : ring)
                for (int n : next[id])
                    grown.insert(n);
        // Growing took in nothing new: all that's tied to the seeds may move. Last try.
        const bool last = grow && grown.size() == ring.size();
        grow = true;
        ring = grown;
        std::map<int, double> freedom;
        for (const auto &[id, p] : points)
            freedom[id] = steady.count(id) ? 1e-6 : ring.count(id) ? 1.0 : 1e-9;
        if (solveWeighted(freedom)) {
            bool held = true;
            for (const auto &[id, p] : start)
                if (!ring.count(id) && std::hypot(points[id].u - p.u, points[id].v - p.v) > 1e-6)
                    held = false;
            if (held)
                return true;
        }
        points = start;
        if (last)
            return false;
    }
}

bool SketchGeometry::measure(const QString &type, int first, int second, double &value, Point &from, Point &to) const {
    // The foot of the square from `p` on line `l`'s extension; false when the line has no length.
    auto foot = [&](const Point &p, int l, Point &out) {
        const Point a = point(line(l).a), b = point(line(l).b);
        const double dx = b.u - a.u, dy = b.v - a.v, len2 = dx * dx + dy * dy;
        if (len2 < 1e-18)
            return false;
        const double s = ((p.u - a.u) * dx + (p.v - a.v) * dy) / len2;
        out = {a.u + dx * s, a.v + dy * s};
        return true;
    };
    if (type == QLatin1String("length")) {
        if (!lines.count(first))           // an axis has no length
            return false;
        from = point(lines.at(first).a);
        to = point(lines.at(first).b);
    } else if (type == QLatin1String("distance")) {
        if (!hasPoint(first) || !hasPoint(second) || first == second)
            return false;
        from = point(first);
        to = point(second);
    } else if (type == QLatin1String("pointline")) {
        if (!hasPoint(first) || !hasLine(second))
            return false;
        from = point(first);
        if (!foot(from, second, to))
            return false;
    } else if (type == QLatin1String("lineline")) {
        if (!hasLine(first) || !hasLine(second) || first == second)
            return false;
        const Point c = point(line(second).a), d = point(line(second).b);
        from = {(c.u + d.u) / 2, (c.v + d.v) / 2};
        if (!foot(from, first, to))
            return false;
    } else if (type == QLatin1String("radius") || type == QLatin1String("diameter")) {
        if (!arcs.count(first))
            return false;
        // Across the middle of the arc (a circle: up and to the right), out from the centre.
        const Arc &k = arcs.at(first);
        const auto path = arcPath(first).front();
        const Point c = point(k.c), mid = path[path.size() / (k.circle() ? 8 : 2)];
        to = mid;
        from = type == QLatin1String("radius") ? c : Point{2 * c.u - mid.u, 2 * c.v - mid.v};
    } else if (type == QLatin1String("offset")) {
        if (!hasLine(first) || !hasLine(second))
            return false;
        const Point c = point(line(second).a), d = point(line(second).b);
        from = {(c.u + d.u) / 2, (c.v + d.v) / 2};
        if (!foot(from, first, to))
            return false;
        std::swap(from, to);
    } else if (type == QLatin1String("offsetarcs")) {
        if (!arcs.count(first) || !arcs.count(second))
            return false;
        const auto path = arcPath(second).front();
        to = path[path.size() / 2];
        const Point c = point(arcs.at(first).c);
        const double r = radius(first), d = std::hypot(to.u - c.u, to.v - c.v);
        if (d < 1e-9)
            return false;
        from = {c.u + (to.u - c.u) * r / d, c.v + (to.v - c.v) * r / d};
    } else if (type == QLatin1String("angle")) {
        if (!hasLine(first) || !hasLine(second) || first == second)
            return false;
        const Point a = point(line(first).a), b = point(line(first).b), c = point(line(second).a), d = point(line(second).b);
        const double x1 = b.u - a.u, y1 = b.v - a.v, x2 = d.u - c.u, y2 = d.v - c.v;
        if (std::hypot(x1, y1) < 1e-9 || std::hypot(x2, y2) < 1e-9)
            return false;
        value = std::atan2(std::abs(x1 * y2 - y1 * x2), x1 * x2 + y1 * y2);
        from = {(a.u + b.u) / 2, (a.v + b.v) / 2};    // where the label goes: between the two middles
        to = {(c.u + d.u) / 2, (c.v + d.v) / 2};
        return true;
    } else {
        return false;
    }
    value = std::hypot(to.u - from.u, to.v - from.v);
    return true;
}

// Which points can still move: the constraints' Jacobian J says how each small movement
// changes them; the movements that change nothing (J's null space) are the freedom left.
// A coordinate is pinned when no such movement touches it. J is row-reduced to find them:
// a pinned coordinate is a pivot column with nothing in any free column of its row.
std::set<int> SketchGeometry::fullyConstrained() const {
    std::set<int> out = projectedPoints;
    if (constraints.empty())
        return out;                       // an arc alone holds only its own shape, never its place
    std::vector<int> ids;
    std::vector<double> x;
    for (const auto &[id, p] : points) {
        if (projectedPoints.count(id))
            continue;
        ids.push_back(id);
        x.push_back(p.u);
        x.push_back(p.v);
    }
    const std::size_t n = x.size();
    const std::vector<double> r = residuals(x, ids);
    const std::size_t m = r.size();
    std::vector<double> a(m * n);
    for (std::size_t j = 0; j < n; ++j) {
        const double h = 1e-6 * std::max(1.0, std::abs(x[j]));
        std::vector<double> up = x, down = x;
        up[j] += h;
        down[j] -= h;
        const std::vector<double> ru = residuals(up, ids), rd = residuals(down, ids);
        for (std::size_t i = 0; i < m; ++i)
            a[i * n + j] = (ru[i] - rd[i]) / (2 * h);
    }
    // Each row scaled to its largest entry, so mm (Fix) and angles (the rest) compare.
    for (std::size_t i = 0; i < m; ++i) {
        double big = 0;
        for (std::size_t j = 0; j < n; ++j)
            big = std::max(big, std::abs(a[i * n + j]));
        if (big > 0)
            for (std::size_t j = 0; j < n; ++j)
                a[i * n + j] /= big;
    }
    const double tiny = 1e-6;
    std::vector<int> pivotRow(n, -1);
    std::size_t row = 0;
    for (std::size_t col = 0; col < n && row < m; ++col) {
        std::size_t best = row;
        for (std::size_t i = row + 1; i < m; ++i)
            if (std::abs(a[i * n + col]) > std::abs(a[best * n + col]))
                best = i;
        if (std::abs(a[best * n + col]) < tiny)
            continue;
        for (std::size_t c = 0; c < n; ++c)
            std::swap(a[row * n + c], a[best * n + c]);
        const double lead = a[row * n + col];
        for (std::size_t c = 0; c < n; ++c)
            a[row * n + c] /= lead;
        for (std::size_t i = 0; i < m; ++i) {
            if (i == row || a[i * n + col] == 0)
                continue;
            const double f = a[i * n + col];
            for (std::size_t c = 0; c < n; ++c)
                a[i * n + c] -= f * a[row * n + c];
        }
        pivotRow[col] = int(row++);
    }
    auto pinned = [&](std::size_t col) {
        if (pivotRow[col] < 0)
            return false;
        const std::size_t i = std::size_t(pivotRow[col]);
        for (std::size_t c = 0; c < n; ++c)
            if (pivotRow[c] < 0 && std::abs(a[i * n + c]) > tiny)
                return false;
        return true;
    };
    for (std::size_t k = 0; k < ids.size(); ++k)
        if (pinned(k * 2) && pinned(k * 2 + 1))
            out.insert(ids[k]);
    return out;
}

bool SketchGeometry::isRim(int point) const {
    for (const auto &[id, arc] : arcs)
        if (arc.circle() && arc.a == point)
            return true;
    return false;
}

double SketchGeometry::valueOf(const Constraint &c) const {
    if (c.follows()) {
        const auto leader = constraints.find(c.third);
        if (leader != constraints.end())
            return leader->second.value;
    }
    return c.value;
}

bool SketchGeometry::holdsNothing() const {
    return constraints.empty() && arcs.empty()
        && std::none_of(curves.begin(), curves.end(), [](const auto &k) { return k.second.type == QLatin1String("ellipse"); });
}

std::vector<std::vector<SketchGeometry::Point>> SketchGeometry::arcPath(int arc) const {
    const Arc &k = arcs.at(arc);
    const Point c = point(k.c), a = point(k.a), b = point(k.b);
    const double r = std::hypot(a.u - c.u, a.v - c.v);
    const double from = std::atan2(a.v - c.v, a.u - c.u);
    double sweep = k.circle() ? 2 * M_PI : std::atan2(b.v - c.v, b.u - c.u) - from;
    while (sweep <= 0)
        sweep += 2 * M_PI;
    const int steps = std::max(8, int(std::ceil(sweep / (2 * M_PI) * 96)));
    std::vector<Point> out;
    for (int i = 0; i <= steps; ++i) {
        const double t = from + sweep * i / steps;
        out.push_back({c.u + r * std::cos(t), c.v + r * std::sin(t)});
    }
    return {out};
}

namespace {
// A cubic B-spline over these control points, starting at the first and ending at the last
// (clamped, uniform knots; fewer than 4 points: as high a degree as they allow).
std::vector<SketchGeometry::Point> bspline(const std::vector<SketchGeometry::Point> &ctrl, int samples) {
    const int n = int(ctrl.size());
    if (n < 2)
        return ctrl;
    const int p = std::min(3, n - 1);
    std::vector<double> knots;
    for (int i = 0; i <= p; ++i)
        knots.push_back(0);
    for (int i = 1; i < n - p; ++i)
        knots.push_back(double(i) / (n - p));
    for (int i = 0; i <= p; ++i)
        knots.push_back(1);
    std::vector<SketchGeometry::Point> out;
    for (int s = 0; s <= samples; ++s) {
        const double t = std::min(double(s) / samples, 1.0 - 1e-12);
        int k = p;
        while (k < n - 1 && t >= knots[k + 1])
            ++k;
        // de Boor
        std::vector<SketchGeometry::Point> d;
        for (int j = 0; j <= p; ++j)
            d.push_back(ctrl[k - p + j]);
        for (int r = 1; r <= p; ++r)
            for (int j = p; j >= r; --j) {
                const int i = k - p + j;
                const double span = knots[i + p - r + 1] - knots[i];
                const double alpha = span < 1e-12 ? 0 : (t - knots[i]) / span;
                d[j] = {(1 - alpha) * d[j - 1].u + alpha * d[j].u, (1 - alpha) * d[j - 1].v + alpha * d[j].v};
            }
        out.push_back(s == samples ? ctrl.back() : d[p]);
    }
    return out;
}
} // namespace

std::vector<std::vector<SketchGeometry::Point>> SketchGeometry::curvePath(int curve) const {
    const Curve &k = curves.at(curve);
    std::vector<Point> at;
    for (int id : k.points)
        at.push_back(point(id));
    if (k.type == QLatin1String("text")) {
        auto shapes = textOutline(k.text, k.value);
        for (auto &shape : shapes)
            for (Point &p : shape)
                p = {p.u + at[0].u, p.v + at[0].v};
        return shapes;
    }
    std::vector<Point> out;
    if (k.type == QLatin1String("ellipse") && at.size() == 3) {
        const double ax = at[1].u - at[0].u, ay = at[1].v - at[0].v;
        const double la = std::hypot(ax, ay), lb = std::hypot(at[2].u - at[0].u, at[2].v - at[0].v);
        if (la < 1e-9)
            return {};
        const double ux = ax / la, uy = ay / la;
        for (int i = 0; i <= 96; ++i) {
            const double t = 2 * M_PI * i / 96;
            out.push_back({at[0].u + la * std::cos(t) * ux - lb * std::sin(t) * uy,
                           at[0].v + la * std::cos(t) * uy + lb * std::sin(t) * ux});
        }
    } else if (k.type == QLatin1String("conic") && at.size() == 3) {
        // A rational quadratic Bézier: weight w on the corner, from Rho = w / (1 + w).
        const double rho = std::clamp(k.value, 0.01, 0.99), w = rho / (1 - rho);
        for (int i = 0; i <= 48; ++i) {
            const double t = double(i) / 48, b0 = (1 - t) * (1 - t), b1 = 2 * t * (1 - t) * w, b2 = t * t;
            const double sum = b0 + b1 + b2;
            out.push_back({(b0 * at[0].u + b1 * at[2].u + b2 * at[1].u) / sum,
                           (b0 * at[0].v + b1 * at[2].v + b2 * at[1].v) / sum});
        }
    } else if (k.type == QLatin1String("controlspline")) {
        out = bspline(at, 24 * int(at.size()));
    } else if (k.type == QLatin1String("fitspline")) {
        // Centripetal Catmull-Rom through every point (ends doubled, so it starts and stops there).
        if (at.size() < 2)
            return {};
        std::vector<Point> q;
        q.push_back({2 * at[0].u - at[1].u, 2 * at[0].v - at[1].v});
        q.insert(q.end(), at.begin(), at.end());
        const Point &l = at.back(), &m = at[at.size() - 2];
        q.push_back({2 * l.u - m.u, 2 * l.v - m.v});
        for (std::size_t i = 1; i + 2 < q.size(); ++i) {
            const Point p0 = q[i - 1], p1 = q[i], p2 = q[i + 1], p3 = q[i + 2];
            auto knot = [](const Point &a, const Point &b) { return std::max(1e-6, std::sqrt(std::hypot(b.u - a.u, b.v - a.v))); };
            const double t1 = knot(p0, p1), t2 = t1 + knot(p1, p2), t3 = t2 + knot(p2, p3);
            for (int s = (i == 1 ? 0 : 1); s <= 24; ++s) {
                const double t = t1 + (t2 - t1) * s / 24;
                auto lerp = [](const Point &a, const Point &b, double ta, double tb, double t) {
                    const double f = std::abs(tb - ta) < 1e-12 ? 0 : (t - ta) / (tb - ta);
                    return Point{a.u + (b.u - a.u) * f, a.v + (b.v - a.v) * f};
                };
                const Point a1 = lerp(p0, p1, 0, t1, t), a2 = lerp(p1, p2, t1, t2, t), a3 = lerp(p2, p3, t2, t3, t);
                const Point b1 = lerp(a1, a2, 0, t2, t), b2 = lerp(a2, a3, t1, t3, t);
                out.push_back(lerp(b1, b2, t1, t2, t));
            }
        }
    }
    return {out};
}

std::vector<std::vector<SketchGeometry::Point>> SketchGeometry::textOutline(const QString &text, double height) {
    QFont font(QStringLiteral("Sans Serif"));
    font.setPixelSize(100);
    QPainterPath path;
    path.addText(0, 0, font, text);
    // Capital height ≈ 0.72 of the pixel size; the text's `height` is its capitals' height (Fusion).
    const double scale = height / 72.0;
    std::vector<std::vector<Point>> out;
    for (const QPolygonF &poly : path.toSubpathPolygons()) {
        std::vector<Point> shape;
        for (const QPointF &p : poly)
            shape.push_back({p.x() * scale, -p.y() * scale});
        if (shape.size() > 1)
            out.push_back(shape);
    }
    return out;
}

double SketchGeometry::radius(int arc) const {
    const Arc &k = arcs.at(arc);
    const Point c = point(k.c), a = point(k.a);
    return std::hypot(a.u - c.u, a.v - c.v);
}

bool SketchGeometry::parallel(int lineA, int lineB) const {
    if (!hasLine(lineA) || !hasLine(lineB))
        return false;
    const Point a = point(line(lineA).a), b = point(line(lineA).b);
    const Point c = point(line(lineB).a), d = point(line(lineB).b);
    const double x1 = b.u - a.u, y1 = b.v - a.v, x2 = d.u - c.u, y2 = d.v - c.v;
    const double l1 = std::hypot(x1, y1), l2 = std::hypot(x2, y2);
    if (l1 < 1e-9 || l2 < 1e-9)
        return false;
    return std::abs(x1 * y2 - y1 * x2) / (l1 * l2) < std::sin(0.1 * M_PI / 180);
}
