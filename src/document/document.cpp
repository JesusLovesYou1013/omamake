#include "document/document.h"

#include "view/meshgeometry.h"

#include <QDateTime>
#include <QDebug>
#include <QJsonObject>

#include <algorithm>
#include <functional>
#include <cmath>
#include <limits>
#include <set>

Document::Document(std::unique_ptr<omamake::GeometryEngine> engine, QObject *parent)
    : QObject(parent), m_engine(std::move(engine)) {}

QString Document::engineName() const {
    return QString::fromStdString(m_engine->name());
}

void Document::fill(MeshGeometry *faces, MeshGeometry *edges) const {
    if (faces)
        faces->setMesh(m_mesh);
    if (edges)
        edges->setMesh(m_mesh);
}

void Document::addCube(double sizeMm) {
    const omamake::BodyId box = m_engine->box(sizeMm, sizeMm, sizeMm);
    const omamake::BodyId placed = m_engine->translate(box, -sizeMm / 2, -sizeMm / 2, 0);
    m_engine->release(box);
    if (m_body) {
        const omamake::BodyId joined = m_engine->combine(m_body, placed, omamake::Combine::Join);
        m_engine->release(m_body);
        m_engine->release(placed);
        m_body = joined;
    } else {
        m_body = placed;
        m_bodies[int(m_body)] = {0, QStringLiteral("Body1"), {}};
        m_components[0].bodies.push_back(int(m_body));
        emit structureChanged();
    }
    record({{QStringLiteral("type"), QStringLiteral("cube")}, {QStringLiteral("size_mm"), sizeMm},
            {QStringLiteral("undoable"), false}},   // the starting model, until real modelling exists
           QStringLiteral("Placed a %1 mm cube (Body1)").arg(sizeMm));
    refreshMesh();
    emit changed();
}

namespace {

QJsonArray xyz(const QVariant &v) {
    const QVector3D p = v.value<QVector3D>();
    return {p.x(), p.y(), p.z()};
}

} // namespace

int Document::addSketch(const QVariantMap &plane) {
    const int id = m_nextSketch++;
    Component *owner = component(m_active);
    const QString name = QStringLiteral("Sketch%1").arg(owner->nextSketch++);
    owner->sketches.push_back(id);
    m_sketches[id] = {owner->id, name, plane};
    const QString on = plane.value(QStringLiteral("label")).toString();
    record({{QStringLiteral("type"), QStringLiteral("sketch")},
            {QStringLiteral("id"), id},
            {QStringLiteral("name"), name},
            {QStringLiteral("component"), owner->id},
            {QStringLiteral("origin"), xyz(plane.value(QStringLiteral("origin")))},
            {QStringLiteral("normal"), xyz(plane.value(QStringLiteral("normal")))},
            {QStringLiteral("x_axis"), xyz(plane.value(QStringLiteral("xAxis")))},
            {QStringLiteral("y_axis"), xyz(plane.value(QStringLiteral("yAxis")))},
            {QStringLiteral("on"), on},
            {QStringLiteral("active_component"), m_active}},
           QStringLiteral("Created %1 in %2 on %3").arg(name, owner->name, on.isEmpty() ? QStringLiteral("a plane") : on));
    m_openSketch = id;
    emit structureChanged();
    return id;
}

void Document::finishSketch(int id) {
    const auto found = m_sketches.find(id);
    if (found == m_sketches.end())
        return;
    record({{QStringLiteral("type"), QStringLiteral("finish_sketch")}, {QStringLiteral("sketch"), id},
            {QStringLiteral("name"), found->second.name}},
           QStringLiteral("Finished %1").arg(found->second.name));
    m_openSketch = 0;
}

QVariantMap Document::sketchPlane(int id) const {
    const auto found = m_sketches.find(id);
    return found == m_sketches.end() ? QVariantMap{} : found->second.plane;
}

QVariantMap Document::undo() {
    if (m_actions.isEmpty())
        return {};
    const QJsonObject last = m_actions.last().toObject();
    if (!last.value(QStringLiteral("undoable")).toBool(true))
        return {};
    const QString type = last.value(QStringLiteral("type")).toString();
    if (type == QLatin1String("sketch")) {
        removeSketch(last.value(QStringLiteral("id")).toInt());
        m_openSketch = 0;
    } else if (type == QLatin1String("component")) {
        removeComponent(last.value(QStringLiteral("id")).toInt());
        m_active = last.value(QStringLiteral("previous_active")).toInt();
        emit structureChanged();
    } else if (type == QLatin1String("visibility")) {
        const QString key = last.value(QStringLiteral("key")).toString();
        if (last.value(QStringLiteral("shown")).toBool())
            m_hidden.insert(key);
        else
            m_hidden.erase(key);
        emit structureChanged();
    } else if (type == QLatin1String("activate")) {
        m_active = last.value(QStringLiteral("from")).toInt();
        emit structureChanged();
    } else if (type == QLatin1String("finish_sketch")) {
        m_openSketch = last.value(QStringLiteral("sketch")).toInt();   // the window goes back into it
    }
    m_actions.removeLast();
    qInfo().noquote() << "history: undid:" << last.value(QStringLiteral("summary")).toString();
    emit historyChanged();
    return last.toVariantMap();
}

void Document::removeSketch(int id) {
    const auto found = m_sketches.find(id);
    if (found == m_sketches.end())
        return;
    // Undoing the newest sketch hands its number back, so the next is named the same.
    Component *owner = component(found->second.component);
    auto &list = owner->sketches;
    if (!list.empty() && list.back() == id)
        --owner->nextSketch;
    list.erase(std::remove(list.begin(), list.end(), id), list.end());
    m_sketches.erase(found);
    emit structureChanged();
}

void Document::removeComponent(int id) {
    if (id == 0)
        return;
    Component *gone = component(id);
    auto &siblings = component(gone->parent)->components;
    siblings.erase(std::remove(siblings.begin(), siblings.end(), id), siblings.end());
    m_components.erase(std::remove_if(m_components.begin(), m_components.end(),
                                      [id](const Component &c) { return c.id == id; }),
                       m_components.end());
    if (id == m_nextComponent - 1)
        --m_nextComponent;   // the next new one gets the same number again
}

QString Document::componentName(int id) const {
    for (const Component &c : m_components)
        if (c.id == id)
            return c.name;
    return {};
}

Document::Component *Document::component(int id) {
    for (Component &c : m_components)
        if (c.id == id)
            return &c;
    return &m_components[0];
}

void Document::setActiveComponent(int id) {
    if (id != m_active && component(id)->id == id) {
        record({{QStringLiteral("type"), QStringLiteral("activate")}, {QStringLiteral("from"), m_active},
                {QStringLiteral("to"), id}},
               QStringLiteral("Activated %1 (was %2)").arg(componentName(id), componentName(m_active)));
        m_active = id;
        emit structureChanged();
    }
}

int Document::newComponent() {
    const int id = m_nextComponent++;
    Component c;
    c.id = id;
    c.parent = m_active;
    c.name = QStringLiteral("Component%1:1").arg(id);   // name:occurrence, as Fusion shows it
    component(m_active)->components.push_back(id);
    m_components.push_back(c);
    m_hidden.insert(QStringLiteral("c%1/origin").arg(id));   // origins start hidden, as in Fusion
    record({{QStringLiteral("type"), QStringLiteral("component")}, {QStringLiteral("id"), id},
            {QStringLiteral("name"), c.name}, {QStringLiteral("parent"), c.parent},
            {QStringLiteral("previous_active"), m_active}},
           QStringLiteral("New Component %1 inside %2, made active").arg(c.name, componentName(c.parent)));
    m_active = id;
    emit structureChanged();
    return id;
}

QVariantMap Document::browser() const {
    return markVisible(componentNode(m_components[0]), true);
}

QVariantMap Document::markVisible(QVariantMap node, bool parentVisible) const {
    const QString key = node.value(QStringLiteral("key")).toString();
    const bool shown = key == QLatin1String("c0") || !m_hidden.count(key);
    const bool visible = parentVisible && shown;
    node.insert(QStringLiteral("shown"), shown);
    node.insert(QStringLiteral("visible"), visible);
    QVariantList children;
    for (const QVariant &child : node.value(QStringLiteral("children")).toList())
        children << markVisible(child.toMap(), visible);
    node.insert(QStringLiteral("children"), children);
    return node;
}

void Document::setShown(const QString &key, bool shown) {
    if (key == QLatin1String("c0") || shown == !m_hidden.count(key))
        return;
    if (shown)
        m_hidden.erase(key);
    else
        m_hidden.insert(key);
    // The row's name, and the component it sits in, for the summary.
    QString name = key, inside;
    std::function<void(const QVariantMap &, const QString &)> find = [&](const QVariantMap &n, const QString &comp) {
        const bool isComp = n.value(QStringLiteral("kind")).toString() == QLatin1String("component");
        const QString here = isComp ? n.value(QStringLiteral("name")).toString() : comp;
        if (n.value(QStringLiteral("key")).toString() == key) {
            name = n.value(QStringLiteral("name")).toString();
            inside = comp;
        }
        for (const QVariant &child : n.value(QStringLiteral("children")).toList())
            find(child.toMap(), here);
    };
    find(componentNode(m_components[0]), QString());
    record({{QStringLiteral("type"), QStringLiteral("visibility")}, {QStringLiteral("key"), key},
            {QStringLiteral("name"), name}, {QStringLiteral("shown"), shown}},
           QStringLiteral("%1 %2%3").arg(shown ? QStringLiteral("Showed") : QStringLiteral("Hid"), name,
                                         inside.isEmpty() ? QString() : QStringLiteral(" in ") + inside));
    emit structureChanged();
}

QVariantMap Document::componentNode(const Component &c) const {
    auto node = [](const QString &key, const char *kind, const QString &name, QVariantList children = {}) {
        return QVariantMap{{QStringLiteral("key"), key}, {QStringLiteral("kind"), QString::fromLatin1(kind)},
                           {QStringLiteral("name"), name}, {QStringLiteral("children"), children}};
    };
    const QString k = QStringLiteral("c%1").arg(c.id);
    QVariantList children;

    // Fusion's origin: the centre point, the three axes and the three planes.
    QVariantList origin;
    origin << node(k + QStringLiteral("/O"), "point", QStringLiteral("O"));
    for (const char *axis : {"X", "Y", "Z"})
        origin << node(k + QStringLiteral("/") + QLatin1String(axis), "axis", QString::fromLatin1(axis));
    for (const char *plane : {"XY", "XZ", "YZ"})
        origin << node(k + QStringLiteral("/") + QLatin1String(plane), "plane", QString::fromLatin1(plane));
    children << node(k + QStringLiteral("/origin"), "origin", QStringLiteral("Origin"), origin);

    // Folders show only once they hold something, as in Fusion.
    QVariantList bodies, sketches;
    for (int id : c.bodies)
        bodies << node(QStringLiteral("b%1").arg(id), "body", m_bodies.at(id).name);
    for (int id : c.sketches)
        sketches << node(QStringLiteral("s%1").arg(id), "sketch", m_sketches.at(id).name);
    if (!bodies.isEmpty())
        children << node(k + QStringLiteral("/bodies"), "folder", QStringLiteral("Bodies"), bodies);
    if (!sketches.isEmpty())
        children << node(k + QStringLiteral("/sketches"), "folder", QStringLiteral("Sketches"), sketches);

    for (int id : c.components)
        for (const Component &sub : m_components)
            if (sub.id == id)
                children << componentNode(sub);

    QVariantMap me = node(k, "component", c.name, children);
    me.insert(QStringLiteral("id"), c.id);
    return me;
}

void Document::record(const QJsonObject &action, const QString &summary) {
    QJsonObject entry = action;
    // Done inside a sketch: that sketch's own history (its own row in the Timeline). Starting
    // and finishing it belong to the design's history, where the sketch is one point.
    const QString type = action.value(QStringLiteral("type")).toString();
    if (m_openSketch && type != QLatin1String("sketch") && type != QLatin1String("finish_sketch"))
        entry.insert(QStringLiteral("in_sketch"), m_openSketch);
    entry.insert(QStringLiteral("at"), QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
    entry.insert(QStringLiteral("summary"), summary);
    qInfo().noquote() << "history:" << (entry.contains(QStringLiteral("in_sketch"))
                                            ? QStringLiteral("[in %1] %2").arg(m_sketches[m_openSketch].name, summary) : summary);
    m_actions.append(entry);
    emit historyChanged();
}

void Document::refreshMesh() {
    m_mesh = m_body ? m_engine->mesh(m_body) : omamake::Mesh{};
    m_boundsMin = QVector3D(m_mesh.min[0], m_mesh.min[1], m_mesh.min[2]);
    m_boundsMax = QVector3D(m_mesh.max[0], m_mesh.max[1], m_mesh.max[2]);
}

namespace {

QVector3D at(const std::vector<float> &xyz, std::size_t i) {
    return QVector3D(xyz[i * 3], xyz[i * 3 + 1], xyz[i * 3 + 2]);
}

// Distance along the ray to a triangle, or infinity (Möller–Trumbore, both sides).
float rayTriangle(const QVector3D &o, const QVector3D &d, const QVector3D &a, const QVector3D &b, const QVector3D &c) {
    const QVector3D e1 = b - a, e2 = c - a;
    const QVector3D p = QVector3D::crossProduct(d, e2);
    const float det = QVector3D::dotProduct(e1, p);
    if (std::abs(det) < 1e-9f)
        return INFINITY;
    const QVector3D s = o - a;
    const float u = QVector3D::dotProduct(s, p) / det;
    if (u < 0 || u > 1)
        return INFINITY;
    const QVector3D q = QVector3D::crossProduct(s, e1);
    const float v = QVector3D::dotProduct(d, q) / det;
    if (v < 0 || u + v > 1)
        return INFINITY;
    const float t = QVector3D::dotProduct(e2, q) / det;
    return t > 0 ? t : INFINITY;
}

// Closest approach between the ray (o + t·d, d unit length) and the segment a→b:
// {distance along the ray, gap between them}.
std::pair<float, float> raySegment(const QVector3D &o, const QVector3D &d, const QVector3D &a, const QVector3D &b) {
    const QVector3D u = b - a, w = o - a;
    const float uu = QVector3D::dotProduct(u, u), ud = QVector3D::dotProduct(u, d);
    const float uw = QVector3D::dotProduct(u, w), dw = QVector3D::dotProduct(d, w);
    const float denom = uu - ud * ud;
    float s = denom > 1e-9f ? (uw - ud * dw) / denom : 0.0f;   // 0 = at a, 1 = at b
    s = std::clamp(s, 0.0f, 1.0f);
    const QVector3D onSeg = a + u * s;
    const float t = std::max(0.0f, QVector3D::dotProduct(onSeg - o, d));
    return {t, (o + d * t - onSeg).length()};
}

void addTriangle(omamake::Mesh &m, const QVector3D &a, const QVector3D &b, const QVector3D &c,
                 const QVector3D &na, const QVector3D &nb, const QVector3D &nc) {
    const QVector3D v[3] = {a, b, c}, n[3] = {na, nb, nc};
    for (int k = 0; k < 3; ++k) {
        m.indices.push_back(std::uint32_t(m.positions.size() / 3));
        for (int i = 0; i < 3; ++i) {
            m.positions.push_back(v[k][i]);
            m.normals.push_back(n[k][i]);
            m.min[i] = std::min(m.min[i], v[k][i]);
            m.max[i] = std::max(m.max[i], v[k][i]);
        }
    }
}

// Two unit vectors at right angles to d.
std::pair<QVector3D, QVector3D> sideways(const QVector3D &d) {
    const QVector3D helper = std::abs(d.z()) < 0.9f ? QVector3D(0, 0, 1) : QVector3D(1, 0, 0);
    const QVector3D x = QVector3D::crossProduct(d, helper).normalized();
    return {x, QVector3D::crossProduct(d, x).normalized()};
}

void addTube(omamake::Mesh &m, const QVector3D &a, const QVector3D &b, float r) {
    constexpr int sides = 8;
    const QVector3D d = (b - a).normalized();
    if ((b - a).length() < 1e-6f)
        return;
    const auto [x, y] = sideways(d);
    for (int i = 0; i < sides; ++i) {
        const float a0 = float(2 * M_PI * i / sides), a1 = float(2 * M_PI * (i + 1) / sides);
        const QVector3D n0 = x * std::cos(a0) + y * std::sin(a0), n1 = x * std::cos(a1) + y * std::sin(a1);
        addTriangle(m, a + n0 * r, b + n0 * r, b + n1 * r, n0, n0, n1);
        addTriangle(m, a + n0 * r, b + n1 * r, a + n1 * r, n0, n1, n1);
    }
}

void addBall(omamake::Mesh &m, const QVector3D &c, float r) {
    constexpr int rings = 6, segments = 10;
    auto p = [&](int ring, int seg) {
        const float th = float(M_PI * ring / rings), ph = float(2 * M_PI * seg / segments);
        return QVector3D(std::sin(th) * std::cos(ph), std::sin(th) * std::sin(ph), std::cos(th));
    };
    for (int i = 0; i < rings; ++i)
        for (int j = 0; j < segments; ++j) {
            const QVector3D a = p(i, j), b = p(i + 1, j), cc = p(i + 1, j + 1), d = p(i, j + 1);
            addTriangle(m, c + a * r, c + b * r, c + cc * r, a, b, cc);
            addTriangle(m, c + a * r, c + cc * r, c + d * r, a, cc, d);
        }
}

} // namespace

QVariantMap Document::pick(QVector3D origin, QVector3D direction, float pointReach, float edgeReach) const {
    const QVector3D d = direction.normalized();

    // The nearest solid surface along the ray hides anything behind it.
    float hitT = INFINITY;
    std::uint32_t hitFace = 0;
    const std::size_t triCount = m_mesh.indices.size() / 3;
    for (std::size_t t = 0; t < triCount; ++t) {
        const float h = rayTriangle(origin, d, at(m_mesh.positions, m_mesh.indices[t * 3]),
                                    at(m_mesh.positions, m_mesh.indices[t * 3 + 1]),
                                    at(m_mesh.positions, m_mesh.indices[t * 3 + 2]));
        if (h < hitT) {
            hitT = h;
            hitFace = m_mesh.triangleFace[t];
        }
    }
    auto visible = [&](float t, float reach) { return t <= hitT + reach * 2; };

    // depth: how far along the ray the nearest surface is (none: -1), so the origin's
    // planes (Selection.qml) can tell whether they're in front of the model or behind it.
    auto result = [&](const char *kind, int index) {
        return QVariantMap{{QStringLiteral("kind"), QString::fromLatin1(kind)}, {QStringLiteral("index"), index},
                           {QStringLiteral("depth"), std::isfinite(hitT) ? hitT : -1.0f}};
    };

    int best = -1;
    float bestT = INFINITY;
    for (std::size_t i = 0; i < m_mesh.points.size() / 3; ++i) {
        const QVector3D p = at(m_mesh.points, i);
        const float t = QVector3D::dotProduct(p - origin, d);
        const float reach = std::max(t, 1.0f) * pointReach;
        if (t > 0 && (origin + d * t - p).length() <= reach && visible(t, reach) && t < bestT) {
            best = int(i);
            bestT = t;
        }
    }
    if (best >= 0)
        return result("point", best);

    float bestGap = INFINITY;
    for (std::size_t e = 0; e < m_mesh.edges.size(); ++e) {
        const auto &line = m_mesh.edges[e];
        for (std::size_t i = 0; i + 1 < line.size() / 3; ++i) {
            const auto [t, gap] = raySegment(origin, d, at(line, i), at(line, i + 1));
            const float reach = std::max(t, 1.0f) * edgeReach;
            if (gap <= reach && visible(t, reach) && gap / reach < bestGap) {
                best = int(e);
                bestGap = gap / reach;
            }
        }
    }
    if (best >= 0)
        return result("edge", best);

    if (std::isfinite(hitT))
        return result("face", int(hitFace));
    return result("", -1);
}

QVariantMap Document::facePlane(int face) const {
    QVariantMap plane{{QStringLiteral("flat"), false}};
    if (face < 0 || std::uint32_t(face) >= m_mesh.faceCount)
        return plane;

    // Every triangle of the face must point the same way; area-weighted, so slivers
    // from the triangulation can't tip the normal.
    QVector3D sum, first;
    QVector3D center;
    float area = 0;
    bool flat = true;
    const std::size_t triCount = m_mesh.indices.size() / 3;
    for (std::size_t t = 0; t < triCount; ++t) {
        if (m_mesh.triangleFace[t] != std::uint32_t(face))
            continue;
        const QVector3D a = at(m_mesh.positions, m_mesh.indices[t * 3]);
        const QVector3D b = at(m_mesh.positions, m_mesh.indices[t * 3 + 1]);
        const QVector3D c = at(m_mesh.positions, m_mesh.indices[t * 3 + 2]);
        const QVector3D cross = QVector3D::crossProduct(b - a, c - a);
        const float twice = cross.length();
        if (twice < 1e-9f)
            continue;
        const QVector3D n = cross / twice;
        if (first.isNull())
            first = n;
        else if (QVector3D::dotProduct(n, first) < 0.9999f)
            flat = false;
        sum += cross;
        center += (a + b + c) / 3 * twice;
        area += twice;
    }
    if (!flat || area <= 0)
        return plane;

    const QVector3D normal = sum.normalized();
    center /= area;
    // Up on the sketch: world Z, unless the face lies (nearly) flat; then world Y.
    const QVector3D up = std::abs(normal.z()) < 0.9f ? QVector3D(0, 0, 1) : QVector3D(0, 1, 0);
    const QVector3D yAxis = (up - normal * QVector3D::dotProduct(up, normal)).normalized();
    const QVector3D xAxis = QVector3D::crossProduct(yAxis, normal).normalized();

    plane.insert(QStringLiteral("flat"), true);
    plane.insert(QStringLiteral("origin"), normal * QVector3D::dotProduct(center, normal));
    plane.insert(QStringLiteral("normal"), normal);
    plane.insert(QStringLiteral("xAxis"), xAxis);
    plane.insert(QStringLiteral("yAxis"), yAxis);
    plane.insert(QStringLiteral("center"), center);
    return plane;
}

void Document::fillParts(MeshGeometry *target, const QVariantList &parts, float edgeRadius) const {
    if (!target)
        return;
    omamake::Mesh out;
    for (int i = 0; i < 3; ++i) {
        out.min[i] = std::numeric_limits<float>::max();
        out.max[i] = std::numeric_limits<float>::lowest();
    }
    std::set<std::uint32_t> faces;
    for (const QVariant &v : parts) {
        const QVariantMap part = v.toMap();
        const QString kind = part.value(QStringLiteral("kind")).toString();
        const int index = part.value(QStringLiteral("index")).toInt();
        if (kind == QLatin1String("face") && index >= 0 && std::uint32_t(index) < m_mesh.faceCount) {
            faces.insert(std::uint32_t(index));
        } else if (kind == QLatin1String("edge") && index >= 0 && std::size_t(index) < m_mesh.edges.size()) {
            const auto &line = m_mesh.edges[std::size_t(index)];
            for (std::size_t k = 0; k + 1 < line.size() / 3; ++k) {
                addTube(out, at(line, k), at(line, k + 1), edgeRadius);
                addBall(out, at(line, k + 1), edgeRadius);   // smooth joints
            }
        } else if (kind == QLatin1String("point") && index >= 0 && std::size_t(index) < m_mesh.points.size() / 3) {
            addBall(out, at(m_mesh.points, std::size_t(index)), edgeRadius * 2.4f);
        }
    }
    const std::size_t triCount = m_mesh.indices.size() / 3;
    for (std::size_t t = 0; t < triCount && !faces.empty(); ++t) {
        if (!faces.count(m_mesh.triangleFace[t]))
            continue;
        // Lifted a hair off the surface: a copy in exactly the same place would lose the depth test.
        const std::uint32_t *idx = &m_mesh.indices[t * 3];
        const float lift = edgeRadius * 0.3f;
        QVector3D v[3], n[3];
        for (int k = 0; k < 3; ++k) {
            n[k] = at(m_mesh.normals, idx[k]);
            v[k] = at(m_mesh.positions, idx[k]) + n[k] * lift;
        }
        addTriangle(out, v[0], v[1], v[2], n[0], n[1], n[2]);
    }
    if (out.positions.empty())
        for (int i = 0; i < 3; ++i)
            out.min[i] = out.max[i] = 0;
    target->setMesh(out);
}
