#include "document/profiles.h"

#include <manifold/polygon.h>

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <map>
#include <numeric>
#include <queue>
#include <set>

namespace profiles {

namespace {

double cross(P a, P b) { return a.u * b.v - a.v * b.u; }
P minus(P a, P b) { return {a.u - b.u, a.v - b.v}; }
double length(P a) { return std::hypot(a.u, a.v); }
double gap(P a, P b) { return length(minus(a, b)); }

// The point of segment a→b nearest p, as its parameter 0…1.
double nearestOn(P a, P b, P p) {
    const P r = minus(b, a);
    const double rr = r.u * r.u + r.v * r.v;
    if (rr <= 0)
        return 0;
    return std::clamp(((p.u - a.u) * r.u + (p.v - a.v) * r.v) / rr, 0.0, 1.0);
}
P along(P a, P b, double t) { return {a.u + (b.u - a.u) * t, a.v + (b.v - a.v) * t}; }

double signedArea(const Path &loop) {
    double s = 0;
    for (std::size_t i = 0; i < loop.size(); ++i)
        s += cross(loop[i], loop[(i + 1) % loop.size()]);
    return s / 2;
}

bool inside(P p, const Path &loop) {
    bool in = false;
    for (std::size_t i = 0, j = loop.size() - 1; i < loop.size(); j = i++) {
        const P a = loop[i], b = loop[j];
        if ((a.v > p.v) != (b.v > p.v) && p.u < (b.u - a.u) * (p.v - a.v) / (b.v - a.v) + a.u)
            in = !in;
    }
    return in;
}

struct Segment {
    P a, b;
    int path = 0, index = 0;          // which path, and which piece of it
    double slack = 0;                 // how far a curve can bow away from this straight piece
    std::vector<double> cuts{0, 1};   // where it's split (0…1): crossings, ends of others on it
    std::vector<int> nodes;           // the graph point at each cut
};

// The nearest two points of two segments that don't cross: one of them is always an end.
struct Near {
    double t, u, size;                // where on the first (t) and second (u), and how far apart
};
Near nearest(const Segment &s, const Segment &q) {
    Near best{0, nearestOn(q.a, q.b, s.a), 0};
    best.size = gap(s.a, along(q.a, q.b, best.u));
    auto take = [&](double t, double u) {
        const double size = gap(along(s.a, s.b, t), along(q.a, q.b, u));
        if (size < best.size)
            best = {t, u, size};
    };
    take(1, nearestOn(q.a, q.b, s.b));
    take(nearestOn(s.a, s.b, q.a), 0);
    take(nearestOn(s.a, s.b, q.b), 1);
    return best;
}

// Points closer than `touching` become one: a grid of touching-sized cells, neighbours checked.
struct Points {
    double cell;
    std::vector<P> at;
    std::map<std::pair<long long, long long>, std::vector<int>> grid;
    int add(P p) {
        const long long cu = std::llround(std::floor(p.u / cell)), cv = std::llround(std::floor(p.v / cell));
        for (long long du = -1; du <= 1; ++du)
            for (long long dv = -1; dv <= 1; ++dv) {
                const auto it = grid.find({cu + du, cv + dv});
                if (it != grid.end())
                    for (int id : it->second)
                        if (gap(at[id], p) <= cell)
                            return id;
            }
        at.push_back(p);
        grid[{cu, cv}].push_back(int(at.size()) - 1);
        return int(at.size()) - 1;
    }
};

} // namespace

Result find(const std::vector<Path> &paths, double gapMax, double touching, const std::vector<int> &groups) {
    Result result;

    // 1. Every piece of every path, split wherever another crosses it or ends on it.
    std::vector<Segment> segs;
    std::vector<int> pieces(paths.size());      // segments per path
    std::vector<bool> closed(paths.size());     // the path comes back to its start
    for (std::size_t k = 0; k < paths.size(); ++k) {
        const Path &path = paths[k];
        for (std::size_t i = 0; i + 1 < path.size(); ++i)
            if (gap(path[i], path[i + 1]) > touching)
                segs.push_back({path[i], path[i + 1], int(k), pieces[k]++, 0, {0, 1}, {}});
        closed[k] = path.size() > 2 && gap(path.front(), path.back()) <= touching;
    }
    // A curve followed as straight pieces stands up to L·θ/8 off the true curve (θ: how far it
    // turns at the piece's ends), so two curves that really touch can look a hair apart. Sharp
    // corners (over about 30°) are real corners, not a curve.
    for (std::size_t i = 0; i < segs.size(); ++i) {
        double turn = 0;
        for (std::size_t j : {i - 1, i + 1}) {
            if (j >= segs.size() || segs[j].path != segs[i].path)
                continue;
            const P a = minus(segs[i].b, segs[i].a), b = minus(segs[j].b, segs[j].a);
            const double angle = std::abs(std::atan2(cross(a, b), a.u * b.u + a.v * b.v));
            if (angle < 0.5)
                turn = std::max(turn, angle);
        }
        segs[i].slack = 1.5 * gap(segs[i].a, segs[i].b) * turn / 8;
    }

    std::vector<int> order(segs.size());
    std::iota(order.begin(), order.end(), 0);
    auto lo = [&](int i) { return std::min(segs[i].a.u, segs[i].b.u); };
    auto hi = [&](int i) { return std::max(segs[i].a.u, segs[i].b.u); };
    std::sort(order.begin(), order.end(), [&](int x, int y) { return lo(x) < lo(y); });
    auto touch = [&](Segment &s, P p) {   // p lies on s: split s there
        const double t = nearestOn(s.a, s.b, p);
        if (gap(along(s.a, s.b, t), p) <= touching)
            s.cuts.push_back(t);
    };
    for (std::size_t x = 0; x < order.size(); ++x) {
        Segment &s = segs[order[x]];
        const double sLo = std::min(s.a.v, s.b.v) - touching, sHi = std::max(s.a.v, s.b.v) + touching;
        for (std::size_t y = x + 1; y < order.size() && lo(order[y]) <= hi(order[x]) + touching; ++y) {
            Segment &q = segs[order[y]];
            if (std::max(q.a.v, q.b.v) < sLo || std::min(q.a.v, q.b.v) > sHi)
                continue;
            touch(s, q.a);
            touch(s, q.b);
            touch(q, s.a);
            touch(q, s.b);
            const P r = minus(s.b, s.a), d = minus(q.b, q.a);
            const double den = cross(r, d);
            if (std::abs(den) > 1e-12 * length(r) * length(d)) {   // not parallel: a crossing?
                const P ca = minus(q.a, s.a);
                const double t = cross(ca, d) / den, u = cross(ca, r) / den;
                if (t > 0 && t < 1 && u > 0 && u < 1) {
                    s.cuts.push_back(t);
                    q.cuts.push_back(u);
                }
            }
        }
    }

    // 2. The pieces as a graph: points (joined when touching) and the edges between them.
    Points pts{touching, {}, {}};
    std::vector<std::set<int>> next;
    for (Segment &s : segs) {
        std::sort(s.cuts.begin(), s.cuts.end());
        int prev = -1;
        for (double t : s.cuts) {
            const int id = pts.add(along(s.a, s.b, t));
            s.nodes.push_back(id);
            if (int(next.size()) <= id)
                next.resize(id + 1);
            if (prev >= 0 && prev != id) {
                next[prev].insert(id);
                next[id].insert(prev);
            }
            prev = id;
        }
    }
    const int n = int(pts.at.size());
    next.resize(n);

    if (gapMax > 0) {
        // 3. Gaps: any two lines that come within the gap distance without touching, and aren't
        //    joined near by: the way round along the lines from one near point to the other must
        //    be well over the gap itself, so a curve's own next stretch, or the far side of a tiny
        //    circle, doesn't count. Where two lines run close for a while (two arcs side by side)
        //    the run is one gap, at its narrowest.
        auto at = [&](const Segment &s, double t, std::vector<std::pair<int, double>> &out) {
            // The graph points either side of s at t, and how far along the line each is.
            const std::size_t i = std::min<std::size_t>(std::upper_bound(s.cuts.begin(), s.cuts.end(), t) - s.cuts.begin(),
                                                         s.cuts.size() - 1);
            const P p = along(s.a, s.b, t);
            for (std::size_t k : {i - 1, i})
                out.push_back({s.nodes[k], gap(p, pts.at[s.nodes[k]])});
        };
        // Shortest way along the lines from the near point on s to the one on q, looked for only
        // as far as `limit` (gaps are small, so this stays local however big the sketch).
        auto roundabout = [&](const Segment &s, double t, const Segment &q, double u, double limit) {
            std::vector<std::pair<int, double>> from, to;
            at(s, t, from);
            at(q, u, to);
            std::map<int, double> d;
            using Item = std::pair<double, int>;
            std::priority_queue<Item, std::vector<Item>, std::greater<Item>> todo;
            for (const auto &[v, dv] : from)
                if (!d.count(v) || dv < d[v]) {
                    d[v] = dv;
                    todo.push({dv, v});
                }
            while (!todo.empty()) {
                const auto [dv, v] = todo.top();
                todo.pop();
                if (dv > limit)
                    break;
                if (dv > d[v])
                    continue;
                for (int w : next[v]) {
                    const double nd = dv + gap(pts.at[v], pts.at[w]);
                    const auto it = d.find(w);
                    if (nd <= limit && (it == d.end() || nd < it->second)) {
                        d[w] = nd;
                        todo.push({nd, w});
                    }
                }
            }
            double best = std::numeric_limits<double>::infinity();
            for (const auto &[v, dv] : to)
                if (d.count(v))
                    best = std::min(best, d[v] + dv);
            return best;
        };
        // Close stretches are gathered with the places lines touch or cross (contacts): a close
        // stretch running into a contact (two tangent circles either side of where they touch, a
        // line crossing a curve at a shallow angle) is that contact, not a gap.
        struct Candidate {
            int s, q;
            Near near;
            bool contact;
        };
        std::vector<Candidate> found;
        auto ownNeighbour = [&](const Segment &a, const Segment &b) {   // joined pieces of one path
            if (a.path != b.path)
                return false;
            const int step = std::abs(a.index - b.index);
            return step <= 1 || (closed[a.path] && step == pieces[a.path] - 1);
        };
        for (std::size_t x = 0; x < order.size(); ++x) {
            const Segment &s = segs[order[x]];
            const double sLo = std::min(s.a.v, s.b.v) - gapMax, sHi = std::max(s.a.v, s.b.v) + gapMax;
            for (std::size_t y = x + 1; y < order.size() && lo(order[y]) <= hi(order[x]) + gapMax; ++y) {
                const Segment &q = segs[order[y]];
                if (std::max(q.a.v, q.b.v) < sLo || std::min(q.a.v, q.b.v) > sHi)
                    continue;
                if (s.path < int(groups.size()) && q.path < int(groups.size()) && groups[s.path] > 0
                    && groups[s.path] == groups[q.path])
                    continue;                               // the same text
                const P r = minus(s.b, s.a), d = minus(q.b, q.a);
                const double den = cross(r, d);
                if (std::abs(den) > 1e-12 * length(r) * length(d)) {
                    const P ca = minus(q.a, s.a);
                    const double t = cross(ca, d) / den, u = cross(ca, r) / den;
                    if (t >= 0 && t <= 1 && u >= 0 && u <= 1) {
                        if (!ownNeighbour(s, q))
                            found.push_back({order[x], order[y], {t, u, 0}, true});
                        continue;                           // they cross: no gap
                    }
                }
                const Near near = nearest(s, q);
                if (near.size > gapMax)
                    continue;
                if (near.size <= touching + s.slack + q.slack) {
                    if (!ownNeighbour(s, q))
                        found.push_back({order[x], order[y], near, true});
                } else {
                    // Joined right there (the way round is short): like a contact, so the close
                    // stretch beside a join (two circles with a short line between them) isn't a gap.
                    const bool joined = roundabout(s, near.t, q, near.u, 2 * near.size + touching) <= 2 * near.size + touching;
                    if (!joined || !ownNeighbour(s, q))
                        found.push_back({order[x], order[y], near, joined});
                }
            }
        }
        // A run: the same two paths, each piece the same as or next to the one before on its path.
        auto beside = [&](int a, int b) { return a == b || ownNeighbour(segs[a], segs[b]); };
        std::vector<int> run(found.size());
        std::iota(run.begin(), run.end(), 0);
        std::function<int(int)> head = [&](int i) { return run[i] == i ? i : run[i] = head(run[i]); };
        std::map<std::pair<int, int>, std::vector<int>> byPaths;   // only the same two paths can be a run
        for (std::size_t i = 0; i < found.size(); ++i) {
            const int a = segs[found[i].s].path, b = segs[found[i].q].path;
            byPaths[{std::min(a, b), std::max(a, b)}].push_back(int(i));
        }
        for (const auto &[paths2, list] : byPaths)
            for (std::size_t x = 0; x < list.size(); ++x)
                for (std::size_t y = x + 1; y < list.size(); ++y) {
                    const Candidate &a = found[list[x]], &b = found[list[y]];
                    if ((beside(a.s, b.s) && beside(a.q, b.q)) || (beside(a.s, b.q) && beside(a.q, b.s)))
                        run[head(list[x])] = head(list[y]);
                }
        std::map<int, int> narrowest;                       // run → its narrowest candidate
        std::set<int> touched;                              // runs with a contact in them
        for (std::size_t i = 0; i < found.size(); ++i) {
            const int r = head(int(i));
            if (found[i].contact)
                touched.insert(r);
            const auto it = narrowest.find(r);
            if (it == narrowest.end() || found[i].near.size < found[it->second].near.size)
                narrowest[r] = int(i);
        }
        std::vector<int> picked;
        for (const auto &[r, i] : narrowest)
            if (!touched.count(r))
                picked.push_back(i);
        // Loose ends, on their own: an end near another line is a gap unless joined to it right
        // there, even where its line meets that one further off (a V whose tips nearly touch).
        for (int e = 0; e < n; ++e) {
            if (next[e].size() != 1)
                continue;
            const P end = pts.at[e];
            int bestS = -1, bestQ = -1;
            Near best{0, 0, gapMax + 1};
            for (std::size_t k = 0; k < segs.size(); ++k) {
                const Segment &s = segs[k];
                if (s.nodes.front() == e || s.nodes.back() == e)
                    bestS = bestS < 0 ? int(k) : bestS;
            }
            if (bestS < 0)
                continue;
            const Segment &own = segs[bestS];
            const double t = own.nodes.front() == e ? 0 : 1;
            for (std::size_t k = 0; k < segs.size(); ++k) {
                const Segment &q = segs[k];
                if (std::find(q.nodes.begin(), q.nodes.end(), e) != q.nodes.end())
                    continue;
                if (std::min(q.a.u, q.b.u) > end.u + gapMax || std::max(q.a.u, q.b.u) < end.u - gapMax
                    || std::min(q.a.v, q.b.v) > end.v + gapMax || std::max(q.a.v, q.b.v) < end.v - gapMax)
                    continue;
                const double u = nearestOn(q.a, q.b, end), size = gap(end, along(q.a, q.b, u));
                if (size <= touching + own.slack + q.slack || size >= best.size)
                    continue;
                if (roundabout(own, t, q, u, 2 * size + touching) > 2 * size + touching) {
                    best = {t, u, size};
                    bestQ = int(k);
                }
            }
            if (bestQ >= 0)
                found.push_back({bestS, bestQ, best, false}), picked.push_back(int(found.size()) - 1);
        }
        std::sort(picked.begin(), picked.end(), [&](int a, int b) { return found[a].near.size < found[b].near.size; });
        for (int i : picked) {
            const Candidate &c = found[i];
            const Gap g{along(segs[c.s].a, segs[c.s].b, c.near.t), along(segs[c.q].a, segs[c.q].b, c.near.u), c.near.size};
            const P mid{(g.a.u + g.b.u) / 2, (g.a.v + g.b.v) / 2};
            const bool again = std::any_of(result.gaps.begin(), result.gaps.end(), [&](const Gap &o) {
                return gap(P{(o.a.u + o.b.u) / 2, (o.a.v + o.b.v) / 2}, mid) < gapMax;
            });
            if (!again)
                result.gaps.push_back(g);
        }
        // Left to right (then bottom to top), so stepping through them goes across the sketch.
        auto middle = [](const Gap &g) { return P{(g.a.u + g.b.u) / 2, (g.a.v + g.b.v) / 2}; };
        std::sort(result.gaps.begin(), result.gaps.end(), [&](const Gap &x, const Gap &y) {
            const P a = middle(x), b = middle(y);
            return std::abs(a.u - b.u) > gapMax ? a.u < b.u : a.v < b.v;
        });
    }

    // 4. Loose ends lead nowhere: take them off, again and again, until only loops are left.
    std::vector<std::set<int>> loops = next;
    std::vector<int> loose;
    for (int v = 0; v < n; ++v)
        if (loops[v].size() == 1)
            loose.push_back(v);
    while (!loose.empty()) {
        const int v = loose.back();
        loose.pop_back();
        if (loops[v].size() != 1)
            continue;
        const int w = *loops[v].begin();
        loops[v].clear();
        loops[w].erase(v);
        if (loops[w].size() == 1)
            loose.push_back(w);
    }

    // 5. Walk round every area: from each edge, at each point turn to the next line clockwise
    //    from the one arrived by. Areas come out anticlockwise (positive); each connected
    //    group's outside comes out clockwise (negative).
    std::vector<std::vector<int>> round(n);
    for (int v = 0; v < n; ++v) {
        round[v].assign(loops[v].begin(), loops[v].end());
        std::sort(round[v].begin(), round[v].end(), [&](int a, int b) {
            return std::atan2(pts.at[a].v - pts.at[v].v, pts.at[a].u - pts.at[v].u)
                 < std::atan2(pts.at[b].v - pts.at[v].v, pts.at[b].u - pts.at[v].u);
        });
    }
    std::vector<int> group(n);
    std::iota(group.begin(), group.end(), 0);
    std::function<int(int)> root = [&](int v) { return group[v] == v ? v : group[v] = root(group[v]); };
    for (int v = 0; v < n; ++v)
        for (int w : round[v])
            group[root(v)] = root(w);

    struct Face {
        Path loop;
        double area;
        int group;
    };
    std::vector<Face> areas, outsides;
    std::set<std::pair<int, int>> walked;
    for (int v = 0; v < n; ++v)
        for (int w : round[v]) {
            if (walked.count({v, w}))
                continue;
            Path loop;
            int a = v, b = w;
            for (int guard = 0; guard < 4 * n + 8; ++guard) {
                walked.insert({a, b});
                loop.push_back(pts.at[a]);
                const auto &at = round[b];
                const int i = int(std::find(at.begin(), at.end(), a) - at.begin());
                const int c = at[(i + int(at.size()) - 1) % int(at.size())];
                a = b;
                b = c;
                if (a == v && b == w)
                    break;
            }
            const double area = signedArea(loop);
            if (area > touching * touching)
                areas.push_back({loop, area, root(v)});
            else if (area < -touching * touching)
                outsides.push_back({loop, area, root(v)});
        }

    // 6. Islands: a group lying inside an area of another group is a hole in the smallest
    //    such area (and still its own profile).
    std::vector<std::vector<Path>> holes(areas.size());
    for (const Face &o : outsides) {
        int in = -1;
        for (std::size_t i = 0; i < areas.size(); ++i)
            if (areas[i].group != o.group && inside(o.loop[0], areas[i].loop)
                && (in < 0 || areas[i].area < areas[in].area))
                in = int(i);
        if (in >= 0)
            holes[in].push_back(o.loop);
    }
    for (std::size_t i = 0; i < areas.size(); ++i) {
        Profile p{areas[i].loop, holes[i], areas[i].area};
        for (const Path &h : holes[i])
            p.area -= std::abs(signedArea(h));
        result.profiles.push_back(std::move(p));
    }
    return result;
}

bool contains(const Profile &profile, P p) {
    return inside(p, profile.outer)
        && std::none_of(profile.holes.begin(), profile.holes.end(), [&](const Path &h) { return inside(p, h); });
}

// Manifold's triangulator (the Lite engine's library): outline anticlockwise, holes clockwise,
// as find() gives them.
std::vector<int> triangles(const Profile &profile) {
    manifold::PolygonsIdx polys;
    int next = 0;
    auto add = [&](const Path &loop) {
        manifold::SimplePolygonIdx poly;
        for (const P &p : loop)
            poly.push_back({manifold::vec2(p.u, p.v), next++});
        polys.push_back(std::move(poly));
    };
    add(profile.outer);
    for (const Path &h : profile.holes)
        add(h);
    std::vector<int> out;
    try {
        for (const manifold::ivec3 &t : manifold::TriangulateIdx(polys))
            out.insert(out.end(), {t.x, t.y, t.z});
    } catch (...) {
        out.clear();
    }
    return out;
}

} // namespace profiles
