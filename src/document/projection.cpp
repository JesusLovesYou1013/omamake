#include "document/projection.h"

#include <manifold/cross_section.h>

#include <algorithm>
#include <cmath>

namespace projection {

namespace {

double cross(const Flat &a, const Flat &b, const Flat &c) {
    return (b.u - a.u) * (c.v - b.v) - (b.v - a.v) * (c.u - b.u);
}
double turnAt(const Flat &a, const Flat &b, const Flat &c) {
    return std::atan2(cross(a, b, c), (b.u - a.u) * (c.u - b.u) + (b.v - a.v) * (c.v - b.v));
}

// A circle through points by least squares, worked out from the first point to keep it accurate.
bool circleThrough(const std::vector<Flat> &p, std::size_t from, std::size_t to, Flat &centre) {
    const Flat o = p[from];
    double a[3][4] = {};
    for (std::size_t i = from; i <= to; ++i) {
        const double x = p[i].u - o.u, y = p[i].v - o.v, row[3] = {x, y, 1}, value = x * x + y * y;
        for (int r = 0; r < 3; ++r) {
            for (int c = 0; c < 3; ++c)
                a[r][c] += row[r] * row[c];
            a[r][3] += row[r] * value;
        }
    }
    for (int k = 0; k < 3; ++k) {
        int big = k;
        for (int r = k + 1; r < 3; ++r)
            if (std::abs(a[r][k]) > std::abs(a[big][k]))
                big = r;
        if (std::abs(a[big][k]) < 1e-14)
            return false;
        std::swap(a[k], a[big]);
        for (int r = 0; r < 3; ++r) {
            if (r == k)
                continue;
            const double f = a[r][k] / a[k][k];
            for (int c = k; c < 4; ++c)
                a[r][c] -= f * a[k][c];
        }
    }
    centre = {o.u + a[0][3] / a[0][0] / 2, o.v + a[1][3] / a[1][1] / 2};
    return true;
}

bool isLine(const std::vector<Flat> &p, std::size_t from, std::size_t to, double tol) {
    const Flat &a = p[from], &b = p[to];
    const double length = std::hypot(b.u - a.u, b.v - a.v);
    if (length < 1e-9)
        return false;
    for (std::size_t i = from + 1; i < to; ++i)
        if (std::abs((p[i].u - a.u) * (b.v - a.v) - (p[i].v - a.v) * (b.u - a.u)) / length > tol)
            return false;
    return true;
}

// Five points or more, all bending the same way, none at a corner.
bool isArc(const std::vector<Flat> &p, std::size_t from, std::size_t to, double tol, Flat &centre) {
    if (to < from + 4 || !circleThrough(p, from, to, centre))
        return false;
    const double r = std::hypot(p[from].u - centre.u, p[from].v - centre.v);
    double swept = 0, sign = 0;
    for (std::size_t i = from + 1; i < to; ++i) {
        const double turn = turnAt(p[i - 1], p[i], p[i + 1]);
        if (std::abs(turn) > 40 * M_PI / 180 || turn * sign < 0)
            return false;
        if (turn != 0)
            sign = turn;
        swept += std::abs(turn);
    }
    if (swept > 2 * M_PI)
        return false;
    // A long straight with a few points at each end fits a circle too, and nothing between them
    // says otherwise: no one step of a real arc takes up most of it.
    double length = 0, step = 0;
    for (std::size_t i = from; i < to; ++i) {
        const double d = std::hypot(p[i + 1].u - p[i].u, p[i + 1].v - p[i].v);
        length += d;
        step = std::max(step, d);
    }
    if (step > length * 0.4)
        return false;
    for (std::size_t i = from; i <= to; ++i)
        if (std::abs(std::hypot(p[i].u - centre.u, p[i].v - centre.v) - r) > tol * 2)
            return false;
    return true;
}

// The furthest `to` that still passes, doubling and then narrowing down.
template <typename Test>
std::size_t longest(std::size_t first, std::size_t last, Test ok) {
    if (first > last || !ok(first))
        return 0;
    std::size_t good = first, step = 1;
    while (good + step <= last && ok(good + step)) {
        good += step;
        step *= 2;
    }
    for (step /= 2; step > 0; step /= 2)
        if (good + step <= last && ok(good + step))
            good += step;
    return good;
}

std::vector<Piece> pieces(std::vector<Flat> p, double tol) {
    std::vector<Piece> out;
    const std::size_t n = p.size();
    // A closed loop starts at its sharpest corner, so nothing round is cut in two.
    std::size_t start = 0;
    double sharpest = -1;
    for (std::size_t i = 0; i < n; ++i) {
        const double turn = std::abs(turnAt(p[(i + n - 1) % n], p[i], p[(i + 1) % n]));
        if (turn > sharpest) {
            sharpest = turn;
            start = i;
        }
    }
    std::rotate(p.begin(), p.begin() + std::ptrdiff_t(start), p.end());

    // One circle all the way round.
    Flat centre;
    if (n >= 8 && circleThrough(p, 0, n - 1, centre)) {
        double r = 0, low = INFINITY, high = 0;
        for (const Flat &q : p) {
            const double d = std::hypot(q.u - centre.u, q.v - centre.v);
            r += d / double(n);
            low = std::min(low, d);
            high = std::max(high, d);
        }
        if (high - low <= tol * 2 && sharpest < 40 * M_PI / 180) {
            Piece c;
            c.kind = Piece::Circle;
            c.c = centre;
            c.a = c.b = {centre.u + r, centre.v};
            return {c};
        }
    }

    p.push_back(p.front());
    for (std::size_t i = 0; i < n;) {
        const std::size_t line = longest(i + 1, n, [&](std::size_t to) { return isLine(p, i, to, tol); });
        Flat c;
        const std::size_t arc = longest(i + 4, n, [&](std::size_t to) { return isArc(p, i, to, tol, c); });
        if (arc > line && isArc(p, i, arc, tol, c)) {
            Piece k;
            k.kind = Piece::Arc;
            const bool counter = cross(p[i], p[i + 1], p[i + 2]) > 0;
            k.a = p[counter ? i : arc];
            k.b = p[counter ? arc : i];
            // On the line midway between its ends, so both are exactly one radius away.
            const Flat middle{(k.a.u + k.b.u) / 2, (k.a.v + k.b.v) / 2};
            const double du = k.b.u - k.a.u, dv = k.b.v - k.a.v, length = std::hypot(du, dv);
            if (length > 1e-9) {
                const Flat side{-dv / length, du / length};
                const double along = (c.u - middle.u) * side.u + (c.v - middle.v) * side.v;
                k.c = {middle.u + side.u * along, middle.v + side.v * along};
                out.push_back(k);
                i = arc;
                continue;
            }
        }
        Piece l;
        l.a = p[i];
        const std::size_t to = line ? line : i + 1;
        l.b = p[to];
        out.push_back(l);
        i = to;
    }
    return out;
}

// Joined in small neighbouring groups, then group with group: a few thousand overlapping triangles
// at once cross each other so often that the whole lot takes many seconds.
manifold::CrossSection covered(manifold::Polygons &triangles) {
    double low[2] = {INFINITY, INFINITY}, high[2] = {-INFINITY, -INFINITY};
    for (const manifold::SimplePolygon &t : triangles)
        for (const manifold::vec2 &p : t) {
            low[0] = std::min(low[0], p.x);
            low[1] = std::min(low[1], p.y);
            high[0] = std::max(high[0], p.x);
            high[1] = std::max(high[1], p.y);
        }
    constexpr int kCells = 64;
    const auto cell = [&](const manifold::SimplePolygon &t) {
        const double x = (t[0].x + t[1].x + t[2].x) / 3, y = (t[0].y + t[1].y + t[2].y) / 3;
        return std::min(kCells - 1, int((x - low[0]) / std::max(high[0] - low[0], 1e-9) * kCells)) * kCells
             + std::min(kCells - 1, int((y - low[1]) / std::max(high[1] - low[1], 1e-9) * kCells));
    };
    std::vector<manifold::Polygons> groups(kCells * kCells);
    for (manifold::SimplePolygon &t : triangles)
        groups[std::size_t(cell(t))].push_back(std::move(t));
    std::vector<manifold::CrossSection> joined;
    for (const manifold::Polygons &g : groups)
        if (!g.empty())
            joined.emplace_back(g, manifold::CrossSection::FillRule::NonZero);
    while (joined.size() > 1) {
        std::vector<manifold::CrossSection> next;
        for (std::size_t i = 0; i + 1 < joined.size(); i += 2)
            next.push_back(joined[i] + joined[i + 1]);
        if (joined.size() % 2)
            next.push_back(joined.back());
        joined = std::move(next);
    }
    return joined.front();
}

} // namespace

std::vector<std::vector<Flat>> covering(const std::vector<std::array<Flat, 3>> &triangles, double tol) {
    manifold::Polygons input;
    input.reserve(triangles.size());
    for (const auto &t : triangles) {
        const double area = cross(t[0], t[1], t[2]);
        if (std::abs(area) < 1e-10)
            continue;
        const auto point = [](const Flat &f) { return manifold::vec2(f.u, f.v); };
        input.push_back(area > 0 ? manifold::SimplePolygon{point(t[0]), point(t[1]), point(t[2])}
                                 : manifold::SimplePolygon{point(t[0]), point(t[2]), point(t[1])});
    }
    std::vector<std::vector<Flat>> out;
    if (input.empty())
        return out;
    // Neighbouring triangles can leave a crack of no width running in from the edge, which would
    // read as lines inside the outline: grow the shadow to seal them, then shrink it back.
    const double seal = tol / 2;
    const manifold::Polygons shadow = covered(input).Offset(seal, manifold::CrossSection::JoinType::Miter, 8)
                                          .Offset(-seal, manifold::CrossSection::JoinType::Miter, 8).ToPolygons();
    for (const manifold::SimplePolygon &loop : shadow) {
        std::vector<Flat> p;
        double area = 0;
        for (std::size_t i = 0; i < loop.size(); ++i) {
            const manifold::vec2 &a = loop[i], &b = loop[(i + 1) % loop.size()];
            area += a.x * b.y - a.y * b.x;
            if (p.empty() || std::hypot(a.x - p.back().u, a.y - p.back().v) > 1e-5)
                p.push_back({a.x, a.y});
        }
        if (p.size() >= 3 && std::abs(area) / 2 > tol * tol * 4)   // specks between triangles aren't holes
            out.push_back(std::move(p));
    }
    return out;
}

std::vector<std::vector<Piece>> outline(const std::vector<std::array<Flat, 3>> &triangles, double tol) {
    std::vector<std::vector<Piece>> out;
    for (std::vector<Flat> &p : covering(triangles, tol))
        out.push_back(pieces(std::move(p), tol));
    return out;
}

} // namespace projection
