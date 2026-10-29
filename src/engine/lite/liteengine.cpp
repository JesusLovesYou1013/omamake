// Lite engine: Manifold3D (watertight triangle meshes). Fast and robust booleans;
// curves are approximated by triangles.

#include "engine/engine.h"

#include <manifold/cross_section.h>
#include <manifold/manifold.h>

#include <cmath>
#include <map>
#include <numeric>
#include <unordered_map>
#include <utility>

namespace omamake {
namespace {

// Two triangles meeting at more than this angle draw an outline edge between them.
constexpr double kOutlineAngleDeg = 20.0;

class LiteEngine final : public GeometryEngine {
public:
    std::string name() const override { return "Lite (Manifold3D)"; }

    BodyId box(double x, double y, double z) override {
        return keep(manifold::Manifold::Cube({x, y, z}, false));
    }

    BodyId extrude(const Profile &profile, double height) override {
        manifold::Polygons polygons;
        for (const auto &loop : profile.loops) {
            manifold::SimplePolygon polygon;
            for (const auto &p : loop)
                polygon.push_back({p.x, p.y});
            polygons.push_back(std::move(polygon));
        }
        const manifold::CrossSection section(polygons, manifold::CrossSection::FillRule::EvenOdd);
        return keep(manifold::Manifold::Extrude(section.ToPolygons(), height));
    }

    BodyId combine(BodyId a, BodyId b, Combine op) override {
        const auto &first = m_bodies.at(a);
        const auto &second = m_bodies.at(b);
        switch (op) {
        case Combine::Join: return keep(first + second);
        case Combine::Cut: return keep(first - second);
        case Combine::Intersect: return keep(first ^ second);
        }
        return 0;
    }

    BodyId translate(BodyId body, double x, double y, double z) override {
        return keep(m_bodies.at(body).Translate({x, y, z}));
    }

    Mesh mesh(BodyId body) const override;

    void release(BodyId body) override { m_bodies.erase(body); }

private:
    BodyId keep(manifold::Manifold m) {
        const BodyId id = ++m_lastId;
        m_bodies.emplace(id, std::move(m));
        return id;
    }

    std::unordered_map<BodyId, manifold::Manifold> m_bodies;
    BodyId m_lastId = 0;
};

using Vec3 = manifold::vec3;

Vec3 faceNormal(const Vec3 &a, const Vec3 &b, const Vec3 &c) {
    return manifold::la::normalize(manifold::la::cross(b - a, c - a));
}

// Flat shading: every triangle gets its own three vertices and its face normal.
// Outline edges: where the two triangles sharing an edge bend more than kOutlineAngleDeg.
// Topology from the same rule (a mesh has no true faces, so we rebuild them):
//   face  = triangles joined across non-outline edges (a cylinder's facets make one face)
//   edge  = a chain of outline segments between corner points (or a closed loop, like a circle)
//   point = a vertex where 1 or 3+ outline segments meet
Mesh LiteEngine::mesh(BodyId body) const {
    Mesh out;
    const manifold::MeshGL64 gl = m_bodies.at(body).GetMeshGL64();
    const std::size_t stride = gl.numProp;
    const std::size_t triCount = gl.triVerts.size() / 3;

    auto vertex = [&](std::size_t i) {
        return Vec3(gl.vertProperties[i * stride], gl.vertProperties[i * stride + 1],
                    gl.vertProperties[i * stride + 2]);
    };

    std::vector<Vec3> normals(triCount);
    std::map<std::pair<std::uint64_t, std::uint64_t>, std::vector<std::size_t>> edgeTris;

    for (int k = 0; k < 3; ++k) {
        out.min[k] = triCount ? INFINITY : 0;
        out.max[k] = triCount ? -INFINITY : 0;
    }

    for (std::size_t t = 0; t < triCount; ++t) {
        const std::uint64_t idx[3] = {gl.triVerts[t * 3], gl.triVerts[t * 3 + 1], gl.triVerts[t * 3 + 2]};
        const Vec3 v[3] = {vertex(idx[0]), vertex(idx[1]), vertex(idx[2])};
        normals[t] = faceNormal(v[0], v[1], v[2]);
        for (int k = 0; k < 3; ++k) {
            for (int c = 0; c < 3; ++c) {
                out.positions.push_back(float(v[k][c]));
                out.normals.push_back(float(normals[t][c]));
                out.min[c] = std::min(out.min[c], float(v[k][c]));
                out.max[c] = std::max(out.max[c], float(v[k][c]));
            }
            out.indices.push_back(std::uint32_t(t * 3 + k));
            const std::uint64_t a = idx[k], b = idx[(k + 1) % 3];
            edgeTris[{std::min(a, b), std::max(a, b)}].push_back(t);
        }
    }

    // Faces: union triangles across every edge that isn't an outline.
    std::vector<std::size_t> parent(triCount);
    std::iota(parent.begin(), parent.end(), 0);
    auto root = [&](std::size_t t) {
        while (parent[t] != t)
            t = parent[t] = parent[parent[t]];
        return t;
    };

    const double cosLimit = std::cos(kOutlineAngleDeg * M_PI / 180.0);
    std::unordered_map<std::uint64_t, std::vector<std::uint64_t>> outlineNext;   // vertex → outline neighbours
    for (const auto &[edge, tris] : edgeTris) {
        const bool outline = tris.size() != 2
            || manifold::la::dot(normals[tris[0]], normals[tris[1]]) < cosLimit;
        if (!outline) {
            parent[root(tris[0])] = root(tris[1]);
            continue;
        }
        outlineNext[edge.first].push_back(edge.second);
        outlineNext[edge.second].push_back(edge.first);
        for (const std::uint64_t end : {edge.first, edge.second}) {
            const Vec3 p = vertex(end);
            for (int c = 0; c < 3; ++c)
                out.edgeLines.push_back(float(p[c]));
        }
    }

    std::unordered_map<std::size_t, std::uint32_t> faceOfRoot;
    out.triangleFace.resize(triCount);
    for (std::size_t t = 0; t < triCount; ++t) {
        const auto [it, added] = faceOfRoot.emplace(root(t), std::uint32_t(faceOfRoot.size()));
        out.triangleFace[t] = it->second;
    }
    out.faceCount = std::uint32_t(faceOfRoot.size());

    // Points, then edges: walk the outline graph from every point; what's left are closed loops.
    auto isPoint = [&](std::uint64_t v) { return outlineNext.at(v).size() != 2; };
    auto push = [&](std::vector<float> &list, std::uint64_t v) {
        const Vec3 p = vertex(v);
        for (int c = 0; c < 3; ++c)
            list.push_back(float(p[c]));
    };
    for (const auto &[v, next] : outlineNext)
        if (isPoint(v))
            push(out.points, v);

    std::map<std::pair<std::uint64_t, std::uint64_t>, bool> walked;
    auto key = [](std::uint64_t a, std::uint64_t b) { return std::make_pair(std::min(a, b), std::max(a, b)); };
    auto walk = [&](std::uint64_t from, std::uint64_t to) {
        std::vector<float> line;
        push(line, from);
        std::uint64_t prev = from, cur = to;
        walked[key(prev, cur)] = true;
        while (true) {
            push(line, cur);
            if (isPoint(cur) || cur == from)
                break;
            const auto &n = outlineNext.at(cur);
            const std::uint64_t nxt = n[0] == prev ? n[1] : n[0];
            if (walked[key(cur, nxt)])
                break;
            walked[key(cur, nxt)] = true;
            prev = cur;
            cur = nxt;
        }
        out.edges.push_back(std::move(line));
    };
    for (const auto &[v, next] : outlineNext)
        if (isPoint(v))
            for (const std::uint64_t n : next)
                if (!walked[key(v, n)])
                    walk(v, n);
    for (const auto &[v, next] : outlineNext)
        for (const std::uint64_t n : next)
            if (!walked[key(v, n)])
                walk(v, n);
    return out;
}

} // namespace

std::unique_ptr<GeometryEngine> makeLiteEngine() {
    return std::make_unique<LiteEngine>();
}

} // namespace omamake
