#include "document/document.h"

#include "view/meshgeometry.h"
#include "document/profiles.h"
#include "document/projection.h"

#include <QDateTime>
#include <QElapsedTimer>
#include <QThread>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QSaveFile>
#include <QCoreApplication>
#include <QFile>
#include <QProcess>
#include <QUrl>
#include <QDebug>
#include <QJsonObject>

#include <algorithm>
#include <functional>
#include <cmath>
#include <limits>
#include <set>

Document::Document(std::unique_ptr<omamake::GeometryEngine> engine, QObject *parent)
    : QObject(parent), m_engine(std::move(engine)) {
    connect(this, &Document::structureChanged, this, [this] { m_profiles.clear(); });
}

QString Document::engineName() const {
    return QString::fromStdString(m_engine->name());
}

void Document::fill(MeshGeometry *faces, MeshGeometry *edges) const {
    if (faces)
        faces->setMesh(m_mesh);
    if (edges)
        edges->setMesh(m_mesh);
}

namespace {

// Fit Curved Surfaces keeps every point of the source within this share of its surface's radius
// (owner, 2026-10-30: "not past the facets by more than 1%").
constexpr double kFitShare = 0.01;


// Two lines dimensioned against each other must be parallel (owner, 2026-10-29): otherwise the
// distance changes along them and the number means nothing.
QString notParallel() {
    return QStringLiteral("Those lines aren't parallel. Make them Parallel first, then dimension the gap.");
}

// A sketch plane for the file: its vectors as [x, y, z] in mm (JSON has no vector type).
QJsonObject planeJson(const QVariantMap &plane) {
    QJsonObject out;
    for (auto it = plane.begin(); it != plane.end(); ++it) {
        if (it.value().canConvert<QVector3D>() && it.value().typeId() == QMetaType::QVector3D) {
            const QVector3D v = it.value().value<QVector3D>();
            out.insert(it.key(), QJsonArray{v.x(), v.y(), v.z()});
        } else {
            out.insert(it.key(), QJsonValue::fromVariant(it.value()));
        }
    }
    return out;
}

} // namespace

QJsonObject Document::toJson() const {
    auto ids = [](const std::vector<int> &v) {
        QJsonArray out;
        for (int id : v)
            out.append(id);
        return out;
    };
    QJsonArray components;
    for (const Component &c : m_components)
        components.append(QJsonObject{{QStringLiteral("id"), c.id}, {QStringLiteral("parent"), c.parent},
                                      {QStringLiteral("name"), c.name}, {QStringLiteral("next_sketch"), c.nextSketch},
                                      {QStringLiteral("components"), ids(c.components)},
                                      {QStringLiteral("bodies"), ids(c.bodies)},
                                      {QStringLiteral("sketches"), ids(c.sketches)}});

    // Bodies: what they are made of, and the mesh as numbers (enough to check a shape by
    // reading). For now the only body is the starting cube, made by the history's "cube".
    QJsonArray bodies;
    for (const auto &[id, b] : m_bodies) {
        QJsonObject body{{QStringLiteral("id"), id}, {QStringLiteral("component"), b.component},
                         {QStringLiteral("name"), b.name}, {QStringLiteral("key"), QStringLiteral("b%1").arg(id)},
                         {QStringLiteral("shown"), !m_hidden.count(QStringLiteral("b%1").arg(id))}};
        if (id == int(m_body)) {
            body.insert(QStringLiteral("mesh"), QJsonObject{
                {QStringLiteral("triangles"), int(m_mesh.indices.size() / 3)},
                {QStringLiteral("faces"), int(m_mesh.faceCount)},
                {QStringLiteral("edges"), int(m_mesh.edges.size())},
                {QStringLiteral("points"), int(m_mesh.points.size() / 3)},
                {QStringLiteral("bounds_min_mm"), QJsonArray{m_boundsMin.x(), m_boundsMin.y(), m_boundsMin.z()}},
                {QStringLiteral("bounds_max_mm"), QJsonArray{m_boundsMax.x(), m_boundsMax.y(), m_boundsMax.z()}}});
        }
        bodies.append(body);
    }

    QJsonArray sketches;
    for (const auto &[id, s] : m_sketches) {
        const SketchGeometry &g = s.geometry;
        const std::set<int> full = g.fullyConstrained();
        QJsonArray points, lines, arcs, curves, constraints;
        for (const auto &[kid, k] : g.curves) {
            QJsonArray ids;
            for (int p : k.points)
                ids.append(p);
            QJsonObject o{{QStringLiteral("id"), kid}, {QStringLiteral("type"), k.type}, {QStringLiteral("points"), ids},
                          {QStringLiteral("construction"), bool(g.constructionCurves.count(kid))}};
            if (k.type == QLatin1String("conic"))
                o.insert(QStringLiteral("rho"), k.value);
            if (k.type == QLatin1String("text")) {
                o.insert(QStringLiteral("text"), k.text);
                o.insert(QStringLiteral("height_mm"), k.value);
            }
            curves.append(o);
        }
        for (const auto &[aid, k] : g.arcs)
            arcs.append(QJsonObject{{QStringLiteral("id"), aid}, {QStringLiteral("centre"), k.c}, {QStringLiteral("a"), k.a},
                                    {QStringLiteral("b"), k.b}, {QStringLiteral("circle"), k.circle()},
                                    {QStringLiteral("helper"), bool(g.helperArcs.count(aid))},
                                    {QStringLiteral("construction"), bool(g.constructionArcs.count(aid))},
                                    {QStringLiteral("radius_mm"), g.radius(aid)},
                                    {QStringLiteral("fully_constrained"), full.count(k.c) && full.count(k.a) && full.count(k.b)}});
        for (const auto &[pid, p] : g.points) {
            const QVector3D at = onSketch(s, p.u, p.v);
            points.append(QJsonObject{{QStringLiteral("id"), pid}, {QStringLiteral("u"), p.u}, {QStringLiteral("v"), p.v},
                                      {QStringLiteral("xyz_mm"), QJsonArray{at.x(), at.y(), at.z()}},
                                      {QStringLiteral("fixed"), g.isFixed(pid)}, {QStringLiteral("circle_rim"), g.isRim(pid)},
                                      {QStringLiteral("helper"), bool(g.helperPoints.count(pid))},
                                      {QStringLiteral("projected"), bool(g.projectedPoints.count(pid))},
                                      {QStringLiteral("fully_constrained"), bool(full.count(pid))}});
        }
        for (const auto &[lid, l] : g.lines) {
            const auto &a = g.points.at(l.a), &b = g.points.at(l.b);
            lines.append(QJsonObject{{QStringLiteral("id"), lid}, {QStringLiteral("a"), l.a}, {QStringLiteral("b"), l.b},
                                     {QStringLiteral("helper"), bool(g.helperLines.count(lid))},
                                     {QStringLiteral("construction"), bool(g.constructionLines.count(lid))},
                                     {QStringLiteral("length_mm"), std::hypot(b.u - a.u, b.v - a.v)},
                                     {QStringLiteral("fully_constrained"), full.count(l.a) && full.count(l.b)}});
        }
        for (const auto &[cid, c] : g.constraints) {
            QJsonObject o{{QStringLiteral("id"), cid}, {QStringLiteral("type"), c.type},
                          {QStringLiteral("first"), c.first}, {QStringLiteral("second"), c.second}};
            if (c.third)
                o.insert(QStringLiteral("third"), c.third);
            if (c.type == QLatin1String("symmetrylines"))
                o.insert(QStringLiteral("ends_swapped"), c.u > 0.5);
            // u, v as kept (what Open reads back; the named fields below say what they mean).
            if (c.u != 0 || c.v != 0 || c.type == QLatin1String("fix") || c.type == QLatin1String("translation")) {
                o.insert(QStringLiteral("u"), c.u);
                o.insert(QStringLiteral("v"), c.v);
            }
            if (c.type == QLatin1String("rotation"))
                o.insert(QStringLiteral("angle_rad"), c.value);
            if (c.type == QLatin1String("tangentarcs"))
                o.insert(QStringLiteral("outside"), c.u > 0.5);
            if (c.type == QLatin1String("offset") || c.type == QLatin1String("offsetarcs"))
                o.insert(QStringLiteral("side"), c.u ? c.u : 1);
            if (c.follows())
                o.insert(QStringLiteral("follows"), c.third);
            if (c.isDimension()) {
                double now = 0;
                SketchGeometry::Point a, b;
                g.measure(c.type, c.first, c.second, now, a, b);
                if (c.type == QLatin1String("angle") && c.u < 0)
                    now = M_PI - now;
                o.insert(c.type == QLatin1String("angle") ? QStringLiteral("value_rad") : QStringLiteral("value_mm"), g.valueOf(c));
                o.insert(QStringLiteral("unit"), c.unit);
                o.insert(QStringLiteral("measured_mm"), now);   // what it really is (differs only if it didn't solve)
                if (c.labelPlaced)
                    o.insert(c.type == QLatin1String("angle") ? QStringLiteral("label_along_lines_mm") : QStringLiteral("label_offset_mm"),
                             QJsonArray{c.labelU, c.labelV});
            }
            constraints.append(o);
        }
        sketches.append(QJsonObject{
            {QStringLiteral("id"), id}, {QStringLiteral("component"), s.component}, {QStringLiteral("name"), s.name},
            {QStringLiteral("key"), QStringLiteral("s%1").arg(id)},
            {QStringLiteral("shown"), !m_hidden.count(QStringLiteral("s%1").arg(id))},
            {QStringLiteral("open"), id == m_openSketch},
            {QStringLiteral("plane"), planeJson(s.plane)},
            {QStringLiteral("origin_uv"), QJsonArray{g.origin.u, g.origin.v}},
            {QStringLiteral("points"), points}, {QStringLiteral("lines"), lines}, {QStringLiteral("arcs"), arcs},
            {QStringLiteral("curves"), curves}, {QStringLiteral("constraints"), constraints},
            {QStringLiteral("unsolved_error"), g.error()},
            {QStringLiteral("next"), QJsonObject{{QStringLiteral("point"), g.nextPoint}, {QStringLiteral("line"), g.nextLine},
                                                 {QStringLiteral("arc"), g.nextArc}, {QStringLiteral("curve"), g.nextCurve},
                                                 {QStringLiteral("constraint"), g.nextConstraint}}}});
    }

    QJsonArray hidden;
    for (const QString &k : m_hidden)
        hidden.append(k);

    return {{QStringLiteral("format"), QStringLiteral("omamake")},
            {QStringLiteral("version"), 1},
            {QStringLiteral("saved_at"), QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},
            {QStringLiteral("engine"), engineName()},
            {QStringLiteral("units"), QStringLiteral("mm (shown in inches unless a dimension says otherwise)")},
            {QStringLiteral("name"), m_components.front().name},
            {QStringLiteral("active_component"), m_active},
            {QStringLiteral("open_sketch"), m_openSketch},
            {QStringLiteral("next"), QJsonObject{{QStringLiteral("component"), m_nextComponent},
                                                 {QStringLiteral("sketch"), m_nextSketch}}},
            {QStringLiteral("components"), components},
            {QStringLiteral("bodies"), bodies},
            {QStringLiteral("sketches"), sketches},
            {QStringLiteral("hidden"), hidden},
            {QStringLiteral("history"), m_actions},
            {QStringLiteral("body_file"), QString::fromLatin1(m_bodyFile.toBase64())},
            {QStringLiteral("step_file"), QString::fromLatin1(m_stepFile.toBase64())}};
}

QString Document::saveTo(const QString &path) {
    if (m_importing)
        return QStringLiteral("Wait for the import to finish.");
    QString target = path;
    if (target.startsWith(QLatin1String("file://")))
        target = QUrl(target).toLocalFile();
    if (target.isEmpty())
        return QStringLiteral("No file name.");
    if (!target.endsWith(QLatin1String(".omamake"), Qt::CaseInsensitive))
        target += QStringLiteral(".omamake");
    const QString oldName = m_components.front().name;
    m_components.front().name = QFileInfo(target).completeBaseName();   // the design is named after its file
    QSaveFile file(target);
    if (!QDir().mkpath(QFileInfo(target).absolutePath()) || !file.open(QIODevice::WriteOnly)) {
        m_components.front().name = oldName;
        return QStringLiteral("Couldn't write %1: %2").arg(target, file.errorString());
    }
    file.write(QJsonDocument(toJson()).toJson(QJsonDocument::Indented));
    if (!file.commit()) {
        m_components.front().name = oldName;
        return QStringLiteral("Couldn't write %1: %2").arg(target, file.errorString());
    }
    qInfo().noquote() << "saved:" << target;
    if (target != m_filePath) {
        m_filePath = target;
        emit fileChanged();
    }
    if (oldName != m_components.front().name)
        emit structureChanged();
    return {};
}

QString Document::exportName() const {
    return m_body ? m_bodies.at(int(m_body)).name : m_components.front().name;
}

QString Document::exportTo(const QString &path) {
    if (m_importing)
        return QStringLiteral("Wait for the import to finish.");
    if (!m_body)
        return QStringLiteral("There's no body to export.");
    QString target = path;
    if (target.startsWith(QLatin1String("file://")))
        target = QUrl(target).toLocalFile();
    if (target.isEmpty())
        return QStringLiteral("No file name.");
    if (!target.endsWith(QLatin1String(".step"), Qt::CaseInsensitive) && !target.endsWith(QLatin1String(".stp"), Qt::CaseInsensitive))
        target += QStringLiteral(".step");
    std::string why;
    if (!m_engine->exportStep(m_body, target.toStdString(), why))
        return QString::fromStdString(why);
    return {};
}

void Document::importFile(const QString &path) {
    const QString file = path.startsWith(QLatin1String("file://")) ? QUrl(path).toLocalFile() : path;
    if (m_importing)
        return;
    if (m_body) {
        emit importFinished(QStringLiteral("One body at a time for now: import into an empty design."), 0, 0, 0);
        return;
    }
    m_importing = true;
    m_importName = QFileInfo(file).completeBaseName();
    m_importStage = QStringLiteral("Reading the file");
    emit importChanged();

    // Off the main thread so the window stays alive; the engine is left alone until it's back
    // (undo, save and load wait while `importing`).
    QThread *worker = QThread::create([this, file] {
        QElapsedTimer clock;
        clock.start();
        auto stage = [this](const char *text) {
            QMetaObject::invokeMethod(this, [this, what = QString::fromUtf8(text)] {
                m_importStage = what;
                emit importChanged();
            }, Qt::QueuedConnection);
        };
        std::string why;
        const bool meshFile = QRegularExpression(QStringLiteral("\\.(stl|obj)$"), QRegularExpression::CaseInsensitiveOption).match(file).hasMatch();
        const omamake::BodyId body = meshFile ? m_engine->importMesh(file.toStdString(), why, stage)
                                              : m_engine->importStep(file.toStdString(), why, stage);
        omamake::Mesh mesh;
        QByteArray packed;
        if (body) {
            stage("Making the triangles to draw");
            mesh = m_engine->mesh(body);
            const std::string bytes = m_engine->saveBody(body);
            packed = qCompress(QByteArray::fromRawData(bytes.data(), qsizetype(bytes.size())));
        }
        const double seconds = clock.elapsed() / 1000.0;
        QMetaObject::invokeMethod(this, [this, body, meshFile, mesh = std::move(mesh), packed, seconds, why] {
            m_importing = false;
            emit importChanged();
            if (!body) {
                emit importFinished(QString::fromStdString(why), seconds, 0, 0);
                return;
            }
            m_bodyFile = packed;
            placeImport(body, m_importName);
            record({{QStringLiteral("type"), meshFile ? QStringLiteral("import_mesh") : QStringLiteral("import_step")},
                    {QStringLiteral("name"), m_importName}},
                   QStringLiteral("Imported %1").arg(m_importName));
            m_mesh = mesh;
            m_boundsMin = QVector3D(m_mesh.min[0], m_mesh.min[1], m_mesh.min[2]);
            m_boundsMax = QVector3D(m_mesh.max[0], m_mesh.max[1], m_mesh.max[2]);
            emit changed();
            emit importFinished({}, seconds, int(m_mesh.faceCount), int(m_mesh.indices.size() / 3));
        }, Qt::QueuedConnection);
    });
    connect(worker, &QThread::finished, worker, &QObject::deleteLater);
    worker->start();
}

void Document::fitSurfaces() {
    if (m_importing)
        return;
    if (!m_body) {
        emit fitFinished(QStringLiteral("There's no body to fit."), 0, 0, 0, 0, 0, 0);
        return;
    }
    // Fitting a fit only blurs it: the first went from the facets, the second from its own drawing.
    for (const QJsonValue &v : m_actions)
        if (v.toObject().value(QStringLiteral("type")).toString() == QLatin1String("fit_surfaces")) {
            emit fitFinished(QStringLiteral("This body is already fitted; undo that to fit it again."), 0, 0, 0, 0, 0, 0);
            return;
        }
    m_importing = true;
    m_importName = m_bodies[int(m_body)].name;
    m_importStage = QStringLiteral("Reading the facets");
    emit importChanged();

    const omamake::BodyId from = m_body;
    QThread *worker = QThread::create([this, from] {
        QElapsedTimer clock;
        clock.start();
        auto stage = [this](const char *text) {
            QMetaObject::invokeMethod(this, [this, what = QString::fromUtf8(text)] {
                m_importStage = what;
                emit importChanged();
            }, Qt::QueuedConnection);
        };
        std::string why;
        omamake::FitResult result;
        const omamake::BodyId body = m_engine->fitSurfaces(from, kFitShare, result, why, stage);
        omamake::Mesh mesh;
        QByteArray packed;
        if (body) {
            stage("Making the triangles to draw");
            mesh = m_engine->mesh(body);
            const std::string bytes = m_engine->saveBody(body);
            packed = qCompress(QByteArray::fromRawData(bytes.data(), qsizetype(bytes.size())));
        }
        const double seconds = clock.elapsed() / 1000.0;
        QMetaObject::invokeMethod(this, [this, body, result, mesh = std::move(mesh), packed, seconds, why] {
            m_importing = false;
            emit importChanged();
            if (!body) {
                emit fitFinished(QString::fromStdString(why), seconds, 0, 0, 0, 0, 0);
                return;
            }
            m_beforeFit.push_back({m_body, m_bodyFile});
            swapBody(body);
            m_bodyFile = packed;
            record({{QStringLiteral("type"), QStringLiteral("fit_surfaces")}}, QStringLiteral("Fit Curved Surfaces"));
            m_mesh = mesh;
            m_boundsMin = QVector3D(m_mesh.min[0], m_mesh.min[1], m_mesh.min[2]);
            m_boundsMax = QVector3D(m_mesh.max[0], m_mesh.max[1], m_mesh.max[2]);
            emit changed();
            emit fitFinished({}, seconds, result.cylinders, result.cones, result.spheres, result.trianglesLeft, result.worstMm);
        }, Qt::QueuedConnection);
    });
    connect(worker, &QThread::finished, worker, &QObject::deleteLater);
    worker->start();
}

// The same body in the tree, a different solid behind it. The old solid is left to the caller.
void Document::swapBody(omamake::BodyId body) {
    const int was = int(m_body), now = int(body);
    m_bodies[now] = m_bodies[was];
    m_bodies.erase(was);
    for (Component &c : m_components)
        std::replace(c.bodies.begin(), c.bodies.end(), was, now);
    if (m_hidden.erase(QStringLiteral("b%1").arg(was)))
        m_hidden.insert(QStringLiteral("b%1").arg(now));
    m_body = body;
    emit structureChanged();
}

void Document::placeImport(omamake::BodyId body, const QString &name) {
    m_body = body;
    m_bodies[int(m_body)] = {m_active, name, {}, {}};
    for (Component &c : m_components)
        if (c.id == m_active)
            c.bodies.push_back(int(m_body));
    emit structureChanged();
}

void Document::addCube(double sizeMm) {
    buildCube(sizeMm);
    record({{QStringLiteral("type"), QStringLiteral("cube")}, {QStringLiteral("size_mm"), sizeMm},
            {QStringLiteral("undoable"), false}},   // the starting model, until real modelling exists
           QStringLiteral("Placed a %1 mm cube (Body1)").arg(sizeMm));
    refreshMesh();
    emit changed();
}

bool Document::untouched() const {
    if (!m_filePath.isEmpty())
        return false;
    for (const QJsonValue &v : m_actions)
        if (v.toObject().value(QStringLiteral("undoable")).toBool(true))
            return false;
    return true;
}

void Document::openInNewWindow(const QString &path) const {
    const QString file = path.startsWith(QLatin1String("file://")) ? QUrl(path).toLocalFile() : path;
    QProcess::startDetached(QCoreApplication::applicationFilePath(), {file});
}

QString Document::openFrom(const QString &path) {
    if (m_importing)
        return QStringLiteral("Wait for the import to finish.");
    const QString source = path.startsWith(QLatin1String("file://")) ? QUrl(path).toLocalFile() : path;
    QFile file(source);
    if (!file.open(QIODevice::ReadOnly))
        return QStringLiteral("Couldn't open %1: %2").arg(source, file.errorString());
    QJsonParseError bad;
    const QJsonDocument parsed = QJsonDocument::fromJson(file.readAll(), &bad);
    const QJsonObject d = parsed.object();
    if (parsed.isNull() || d.value(QStringLiteral("format")).toString() != QLatin1String("omamake"))
        return QStringLiteral("%1 isn't an OmaMake design.").arg(QFileInfo(source).fileName());
    if (d.value(QStringLiteral("version")).toInt() > 1)
        return QStringLiteral("%1 was saved by a newer OmaMake.").arg(QFileInfo(source).fileName());

    auto ints = [](const QJsonValue &v) {
        std::vector<int> out;
        for (const QJsonValue &x : v.toArray())
            out.push_back(x.toInt());
        return out;
    };
    // Everything here goes; the file's design takes its place.
    if (m_body)
        m_engine->release(m_body);
    m_body = 0;
    m_stepFile.clear();
    m_bodyFile.clear();
    for (const Fitted &before : m_beforeFit)
        m_engine->release(before.body);
    m_beforeFit.clear();
    m_components.clear();
    m_bodies.clear();
    m_sketches.clear();
    m_hidden.clear();
    m_snapshots.clear();
    m_dragBefore.clear();
    m_openSketch = 0;
    m_actions = {};

    // Bodies: made again by their history (so far only the starting cube), then the file's
    // components and bodies take over; the engine may number bodies differently, so their keys follow.
    m_components = {rootComponent()};
    for (const QJsonValue &v : d.value(QStringLiteral("history")).toArray()) {
        QJsonObject a = v.toObject();
        if (a.value(QStringLiteral("type")).toString() == QLatin1String("cube"))
            buildCube(a.value(QStringLiteral("size_mm")).toDouble());
        m_bodyFile = QByteArray::fromBase64(d.value(QStringLiteral("body_file")).toString().toLatin1());
        const QString kind = a.value(QStringLiteral("type")).toString();
        if ((kind == QLatin1String("import_step") || kind == QLatin1String("import_mesh")) && !m_bodyFile.isEmpty()) {
            m_body = m_engine->loadBody(qUncompress(m_bodyFile).toStdString());
            if (!m_body)
                qWarning().noquote() << "open: the saved body couldn't be read";
        } else if (kind == QLatin1String("import_step")) {
            m_stepFile = QByteArray::fromBase64(d.value(QStringLiteral("step_file")).toString().toLatin1());
            const QString temp = QDir::temp().filePath(QStringLiteral("omamake-%1.step").arg(QCoreApplication::applicationPid()));
            QFile out(temp);
            std::string why;
            if (out.open(QIODevice::WriteOnly) && out.write(qUncompress(m_stepFile)) > 0) {
                out.close();
                m_body = m_engine->importStep(temp.toStdString(), why, [](const char *) {});
            }
            QFile::remove(temp);
            if (!m_body)
                qWarning().noquote() << "open: the imported STEP file couldn't be rebuilt:" << QString::fromStdString(why);
        }
        a.insert(QStringLiteral("undoable"), false);     // done before it was saved
        m_actions.append(a);
    }
    m_components.clear();
    m_bodies.clear();

    for (const QJsonValue &v : d.value(QStringLiteral("components")).toArray()) {
        const QJsonObject o = v.toObject();
        Component c;
        c.id = o.value(QStringLiteral("id")).toInt();
        c.parent = o.value(QStringLiteral("parent")).toInt(-1);
        c.name = o.value(QStringLiteral("name")).toString();
        c.nextSketch = o.value(QStringLiteral("next_sketch")).toInt(1);
        c.components = ints(o.value(QStringLiteral("components")));
        c.bodies = ints(o.value(QStringLiteral("bodies")));
        c.sketches = ints(o.value(QStringLiteral("sketches")));
        m_components.push_back(c);
    }
    if (m_components.empty())
        m_components.push_back(rootComponent());
    m_active = d.value(QStringLiteral("active_component")).toInt();
    const QJsonObject next = d.value(QStringLiteral("next")).toObject();
    m_nextComponent = next.value(QStringLiteral("component")).toInt(1);
    m_nextSketch = next.value(QStringLiteral("sketch")).toInt(1);
    for (const QJsonValue &v : d.value(QStringLiteral("hidden")).toArray())
        m_hidden.insert(v.toString());

    for (const QJsonValue &v : d.value(QStringLiteral("bodies")).toArray()) {
        const QJsonObject o = v.toObject();
        const int was = o.value(QStringLiteral("id")).toInt(), now = m_body ? int(m_body) : was;
        m_bodies[now] = {o.value(QStringLiteral("component")).toInt(), o.value(QStringLiteral("name")).toString(), {}, {}};
        if (now != was) {
            for (Component &c : m_components)
                std::replace(c.bodies.begin(), c.bodies.end(), was, now);
            if (m_hidden.erase(QStringLiteral("b%1").arg(was)))
                m_hidden.insert(QStringLiteral("b%1").arg(now));
        }
    }

    for (const QJsonValue &v : d.value(QStringLiteral("sketches")).toArray()) {
        const QJsonObject o = v.toObject();
        Named s;
        s.component = o.value(QStringLiteral("component")).toInt();
        s.name = o.value(QStringLiteral("name")).toString();
        // The plane's vectors were written as [x, y, z].
        const QJsonObject plane = o.value(QStringLiteral("plane")).toObject();
        for (auto it = plane.begin(); it != plane.end(); ++it) {
            const QJsonArray a = it.value().toArray();
            if (it.value().isArray() && a.size() == 3)
                s.plane.insert(it.key(), QVector3D(float(a[0].toDouble()), float(a[1].toDouble()), float(a[2].toDouble())));
            else
                s.plane.insert(it.key(), it.value().toVariant());
        }
        SketchGeometry &g = s.geometry;
        const QJsonArray origin = o.value(QStringLiteral("origin_uv")).toArray();
        if (origin.size() == 2) {
            g.origin = {origin[0].toDouble(), origin[1].toDouble()};
        } else {                                         // saved before 2026-10-30: as addSketch works it out
            const QVector3D toOrigin = -s.plane.value(QStringLiteral("origin")).value<QVector3D>();
            g.origin = {QVector3D::dotProduct(toOrigin, s.plane.value(QStringLiteral("xAxis")).value<QVector3D>()),
                        QVector3D::dotProduct(toOrigin, s.plane.value(QStringLiteral("yAxis")).value<QVector3D>())};
        }
        for (const QJsonValue &x : o.value(QStringLiteral("points")).toArray()) {
            const QJsonObject p = x.toObject();
            const int id = p.value(QStringLiteral("id")).toInt();
            g.points[id] = {p.value(QStringLiteral("u")).toDouble(), p.value(QStringLiteral("v")).toDouble()};
            if (p.value(QStringLiteral("helper")).toBool())
                g.helperPoints.insert(id);
            if (p.value(QStringLiteral("projected")).toBool())
                g.projectedPoints.insert(id);
        }
        for (const QJsonValue &x : o.value(QStringLiteral("lines")).toArray()) {
            const QJsonObject l = x.toObject();
            const int id = l.value(QStringLiteral("id")).toInt();
            g.lines[id] = {l.value(QStringLiteral("a")).toInt(), l.value(QStringLiteral("b")).toInt()};
            if (l.value(QStringLiteral("helper")).toBool())
                g.helperLines.insert(id);
            if (l.value(QStringLiteral("construction")).toBool())
                g.constructionLines.insert(id);
        }
        for (const QJsonValue &x : o.value(QStringLiteral("arcs")).toArray()) {
            const QJsonObject k = x.toObject();
            const int id = k.value(QStringLiteral("id")).toInt();
            g.arcs[id] = {k.value(QStringLiteral("centre")).toInt(), k.value(QStringLiteral("a")).toInt(), k.value(QStringLiteral("b")).toInt()};
            if (k.value(QStringLiteral("helper")).toBool())
                g.helperArcs.insert(id);
            if (k.value(QStringLiteral("construction")).toBool())
                g.constructionArcs.insert(id);
        }
        for (const QJsonValue &x : o.value(QStringLiteral("curves")).toArray()) {
            const QJsonObject k = x.toObject();
            const int id = k.value(QStringLiteral("id")).toInt();
            SketchGeometry::Curve c;
            c.type = k.value(QStringLiteral("type")).toString();
            c.points = ints(k.value(QStringLiteral("points")));
            c.value = k.contains(QStringLiteral("rho")) ? k.value(QStringLiteral("rho")).toDouble() : k.value(QStringLiteral("height_mm")).toDouble();
            c.text = k.value(QStringLiteral("text")).toString();
            g.curves[id] = c;
            if (k.value(QStringLiteral("construction")).toBool())
                g.constructionCurves.insert(id);
        }
        for (const QJsonValue &x : o.value(QStringLiteral("constraints")).toArray()) {
            const QJsonObject k = x.toObject();
            SketchGeometry::Constraint c(k.value(QStringLiteral("type")).toString(), k.value(QStringLiteral("first")).toInt(),
                                         k.value(QStringLiteral("second")).toInt(), k.value(QStringLiteral("u")).toDouble(),
                                         k.value(QStringLiteral("v")).toDouble());
            c.third = k.value(QStringLiteral("third")).toInt();
            // Files saved before u was always written: from the fields that say what it means.
            if (!k.contains(QStringLiteral("u"))) {
                if (k.contains(QStringLiteral("ends_swapped")))
                    c.u = k.value(QStringLiteral("ends_swapped")).toBool() ? 1 : 0;
                else if (k.contains(QStringLiteral("outside")))
                    c.u = k.value(QStringLiteral("outside")).toBool() ? 1 : 0;
                else if (k.contains(QStringLiteral("side")))
                    c.u = k.value(QStringLiteral("side")).toDouble();
            }
            c.value = k.contains(QStringLiteral("value_mm")) ? k.value(QStringLiteral("value_mm")).toDouble()
                    : k.contains(QStringLiteral("value_rad")) ? k.value(QStringLiteral("value_rad")).toDouble()
                    : k.value(QStringLiteral("angle_rad")).toDouble();
            c.unit = k.value(QStringLiteral("unit")).toString();
            const QJsonArray label = k.contains(QStringLiteral("label_offset_mm")) ? k.value(QStringLiteral("label_offset_mm")).toArray()
                                                                                   : k.value(QStringLiteral("label_along_lines_mm")).toArray();
            if (label.size() == 2) {
                c.labelPlaced = true;
                c.labelU = label[0].toDouble();
                c.labelV = label[1].toDouble();
            }
            g.constraints[k.value(QStringLiteral("id")).toInt()] = c;
        }
        const QJsonObject n = o.value(QStringLiteral("next")).toObject();
        g.nextPoint = n.value(QStringLiteral("point")).toInt(1);
        g.nextLine = n.value(QStringLiteral("line")).toInt(1);
        g.nextArc = n.value(QStringLiteral("arc")).toInt(1);
        g.nextCurve = n.value(QStringLiteral("curve")).toInt(1);
        g.nextConstraint = n.value(QStringLiteral("constraint")).toInt(1);
        m_sketches[o.value(QStringLiteral("id")).toInt()] = s;
    }

    m_filePath = source;
    m_components.front().name = QFileInfo(source).completeBaseName();
    qInfo().noquote() << "opened:" << source;
    refreshMesh();
    emit changed();
    emit structureChanged();
    sketchesEdited();
    emit historyChanged();
    emit fileChanged();
    return {};
}

void Document::buildCube(double sizeMm) {
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
        m_bodies[int(m_body)] = {0, QStringLiteral("Body1"), {}, {}};
        m_components[0].bodies.push_back(int(m_body));
        emit structureChanged();
    }
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
    m_sketches[id] = {owner->id, name, plane, {}};
    // Where the model's origin falls on the plane (square onto it, for a face that doesn't
    // pass through it): the sketch's Origin, which constraints can tie the sketch to.
    const QVector3D toOrigin = -plane.value(QStringLiteral("origin")).value<QVector3D>();
    m_sketches[id].geometry.origin = {QVector3D::dotProduct(toOrigin, plane.value(QStringLiteral("xAxis")).value<QVector3D>()),
                                      QVector3D::dotProduct(toOrigin, plane.value(QStringLiteral("yAxis")).value<QVector3D>())};
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

void Document::editSketch(int id) {
    const auto found = m_sketches.find(id);
    if (found == m_sketches.end() || m_openSketch == id)
        return;
    // A hidden sketch shows again to be edited (its eye can hide it once inside); Ctrl+Z hides
    // it again.
    const bool wasHidden = m_hidden.erase(QStringLiteral("s%1").arg(id)) > 0;
    record({{QStringLiteral("type"), QStringLiteral("edit_sketch")}, {QStringLiteral("sketch"), id},
            {QStringLiteral("name"), found->second.name}, {QStringLiteral("was_hidden"), wasHidden}},
           QStringLiteral("Edited %1").arg(found->second.name));
    m_openSketch = id;
    if (wasHidden)
        emit structureChanged();
}

QVariantMap Document::sketchPlane(int id) const {
    const auto found = m_sketches.find(id);
    return found == m_sketches.end() ? QVariantMap{} : found->second.plane;
}

QVariantMap Document::undo() {
    if (m_actions.isEmpty())
        return {};
    if (m_importing)
        return {};
    const QJsonObject last = m_actions.last().toObject();
    if (!last.value(QStringLiteral("undoable")).toBool(true))
        return {};
    const QString type = last.value(QStringLiteral("type")).toString();
    if (type == QLatin1String("sketch")) {
        removeSketch(last.value(QStringLiteral("id")).toInt());
        m_openSketch = 0;
    } else if (type == QLatin1String("import_step") || type == QLatin1String("import_mesh")) {
        for (Component &c : m_components)
            c.bodies.erase(std::remove(c.bodies.begin(), c.bodies.end(), int(m_body)), c.bodies.end());
        m_bodies.erase(int(m_body));
        m_engine->release(m_body);
        m_body = 0;
        m_stepFile.clear();
        m_bodyFile.clear();
        refreshMesh();
        emit changed();
        emit structureChanged();
    } else if (type == QLatin1String("fit_surfaces")) {
        const Fitted before = m_beforeFit.back();
        m_beforeFit.pop_back();
        const omamake::BodyId fitted = m_body;
        swapBody(before.body);
        m_engine->release(fitted);
        m_bodyFile = before.file;
        refreshMesh();
        emit changed();
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
    } else if (type == QLatin1String("edit_sketch")) {
        m_openSketch = 0;                                               // the window leaves it
        if (last.value(QStringLiteral("was_hidden")).toBool()) {
            m_hidden.insert(QStringLiteral("s%1").arg(last.value(QStringLiteral("sketch")).toInt()));
            emit structureChanged();
        }
    } else if (last.contains(QStringLiteral("snapshot"))) {
        // Kept whole before the edit (every tool from Midpoint Line on): put back exactly that.
        const auto found = m_sketches.find(last.value(QStringLiteral("sketch")).toInt());
        const auto kept = m_snapshots.find(last.value(QStringLiteral("snapshot")).toInt());
        if (found != m_sketches.end() && kept != m_snapshots.end()) {
            found->second.geometry = kept->second;
            m_snapshots.erase(kept);
        }
        sketchesEdited();
    } else if (type == QLatin1String("line") || type == QLatin1String("rectangle") || type == QLatin1String("shape")) {
        const auto found = m_sketches.find(last.value(QStringLiteral("sketch")).toInt());
        if (found != m_sketches.end())
            forgetSketchLines(found->second, last);
        sketchesEdited();
    } else if (type == QLatin1String("dimension_value")) {
        const auto found = m_sketches.find(last.value(QStringLiteral("sketch")).toInt());
        if (found != m_sketches.end()) {
            SketchGeometry &g = found->second.geometry;
            const auto c = g.constraints.find(last.value(QStringLiteral("id")).toInt());
            if (c != g.constraints.end()) {
                c->second.value = last.value(QStringLiteral("value")).toDouble();
                c->second.unit = last.value(QStringLiteral("unit")).toString();
            }
            for (const QJsonValue &v : last.value(QStringLiteral("moved")).toArray()) {
                const QJsonObject m = v.toObject();
                g.points[m.value(QStringLiteral("id")).toInt()] = {m.value(QStringLiteral("u")).toDouble(),
                                                                    m.value(QStringLiteral("v")).toDouble()};
            }
        }
        sketchesEdited();
    } else if (type == QLatin1String("drag") || type == QLatin1String("fix")) {
        const auto found = m_sketches.find(last.value(QStringLiteral("sketch")).toInt());
        if (found != m_sketches.end()) {
            SketchGeometry &g = found->second.geometry;
            for (const QJsonValue &v : last.value(QStringLiteral("moved")).toArray()) {
                const QJsonObject m = v.toObject();
                g.points[m.value(QStringLiteral("id")).toInt()] = {m.value(QStringLiteral("u")).toDouble(),
                                                                    m.value(QStringLiteral("v")).toDouble()};
            }
            for (const QJsonValue &v : last.value(QStringLiteral("added")).toArray())
                g.constraints.erase(v.toInt());
            for (const QJsonValue &v : last.value(QStringLiteral("removed")).toArray()) {
                const QJsonObject c = v.toObject();
                g.constraints[c.value(QStringLiteral("id")).toInt()] = {
                    QStringLiteral("fix"), c.value(QStringLiteral("point")).toInt(), 0,
                    c.value(QStringLiteral("u")).toDouble(), c.value(QStringLiteral("v")).toDouble()};
            }
        }
        sketchesEdited();
    } else if (type == QLatin1String("delete_constraints")) {
        const auto found = m_sketches.find(last.value(QStringLiteral("sketch")).toInt());
        if (found != m_sketches.end()) {
            SketchGeometry &g = found->second.geometry;
            for (const QJsonValue &v : last.value(QStringLiteral("removed")).toArray()) {
                const QJsonObject o = v.toObject();
                SketchGeometry::Constraint c(o.value(QStringLiteral("type")).toString(), o.value(QStringLiteral("first")).toInt(),
                                             o.value(QStringLiteral("second")).toInt(), o.value(QStringLiteral("u")).toDouble(),
                                             o.value(QStringLiteral("v")).toDouble());
                c.third = o.value(QStringLiteral("third")).toInt();
                c.value = o.value(QStringLiteral("value")).toDouble();
                c.unit = o.value(QStringLiteral("unit")).toString();
                g.constraints[o.value(QStringLiteral("id")).toInt()] = c;
            }
        }
        sketchesEdited();
    } else if (type == QLatin1String("constraint")) {
        const auto found = m_sketches.find(last.value(QStringLiteral("sketch")).toInt());
        if (found != m_sketches.end()) {
            SketchGeometry &g = found->second.geometry;
            g.constraints.erase(last.value(QStringLiteral("id")).toInt());
            for (const QJsonValue &v : last.value(QStringLiteral("moved")).toArray()) {
                const QJsonObject m = v.toObject();
                g.points[m.value(QStringLiteral("id")).toInt()] = {m.value(QStringLiteral("u")).toDouble(),
                                                                    m.value(QStringLiteral("v")).toDouble()};
            }
        }
        sketchesEdited();
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
    // Done inside a sketch: that sketch's own history (its own row in the Timeline). Starting,
    // finishing and editing it belong to the design's history, where the sketch is one point.
    const QString type = action.value(QStringLiteral("type")).toString();
    if (m_openSketch && type != QLatin1String("sketch") && type != QLatin1String("finish_sketch")
        && type != QLatin1String("edit_sketch"))
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
        addTriangle(m, a + n0 * r, b + n1 * r, b + n0 * r, n0, n1, n0);
        addTriangle(m, a + n0 * r, a + n1 * r, b + n1 * r, n0, n1, n1);
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

// A path as dashes and dots, Fusion's construction look in the owner's pattern (2026-10-30):
// dash, gap, dot, gap, measured in the line's own thickness so it reads the same at any zoom.
// The pattern carries on round corners and along curves.
void addDashed(omamake::Mesh &m, const std::vector<QVector3D> &path, float r) {
    const float pattern[4] = {11 * r, 4 * r, 2 * r, 4 * r};
    int k = 0;
    float left = pattern[0];
    for (std::size_t i = 0; i + 1 < path.size(); ++i) {
        const QVector3D a = path[i], d = path[i + 1] - path[i];
        const float length = d.length();
        float t = 0;
        while (length - t > 1e-6f) {
            const float step = std::min(left, length - t);
            if (k % 2 == 0)
                addTube(m, a + d * (t / length), a + d * ((t + step) / length), r);
            t += step;
            left -= step;
            if (left <= 1e-6f) {
                k = (k + 1) % 4;
                left = pattern[k];
            }
        }
    }
}

} // namespace

int Document::faceAt(QVector3D origin, QVector3D direction) const {
    const QVector3D d = direction.normalized();
    float nearest = INFINITY;
    int face = -1;
    for (std::size_t t = 0; t < m_mesh.indices.size() / 3; ++t) {
        const float h = rayTriangle(origin, d, at(m_mesh.positions, m_mesh.indices[t * 3]),
                                    at(m_mesh.positions, m_mesh.indices[t * 3 + 1]),
                                    at(m_mesh.positions, m_mesh.indices[t * 3 + 2]));
        if (h < nearest) {
            nearest = h;
            face = int(m_mesh.triangleFace[t]);
        }
    }
    return face;
}

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

// A sketch's normal lines, arcs and curves as point chains for profiles::find, with a group
// per path (a text's letters share one, so they're never a gap to each other).
std::pair<std::vector<profiles::Path>, std::vector<int>> Document::profilePaths(const SketchGeometry &g) const {
    std::vector<profiles::Path> paths;
    std::vector<int> groups;                       // a text's letters share one (never gaps to each other)
    auto add = [&](const std::vector<std::vector<SketchGeometry::Point>> &chains, int group = 0) {
        for (const auto &chain : chains) {
            profiles::Path p;
            for (const auto &q : chain)
                p.push_back({q.u, q.v});
            paths.push_back(std::move(p));
            groups.push_back(group);
        }
    };
    for (const auto &[id, l] : g.lines)
        if (!g.helperLines.count(id) && !g.constructionLines.count(id))
            add({{g.points.at(l.a), g.points.at(l.b)}});
    for (const auto &[id, a] : g.arcs)
        if (!g.helperArcs.count(id) && !g.constructionArcs.count(id))
            add(g.arcPath(id));
    for (const auto &[id, k] : g.curves)
        if (!g.constructionCurves.count(id))
            add(g.curvePath(id), k.type == QLatin1String("text") ? id + 1 : 0);
    return {paths, groups};
}

QVariantMap Document::sketchProfiles(int sketch, double gapMax) const {
    const auto it = m_sketches.find(sketch);
    if (it == m_sketches.end())
        return {};
    const auto [paths, groups] = profilePaths(it->second.geometry);
    const profiles::Result found = profiles::find(paths, gapMax, 1e-4, groups);

    auto point = [&](const profiles::P &p) {
        return QVariantMap{{QStringLiteral("u"), p.u}, {QStringLiteral("v"), p.v},
                           {QStringLiteral("at"), onSketch(it->second, p.u, p.v)}};
    };
    auto loop = [](const profiles::Path &path) {
        QVariantList out;
        for (const auto &p : path)
            out << QVariantMap{{QStringLiteral("u"), p.u}, {QStringLiteral("v"), p.v}};
        return out;
    };
    QVariantList shapes, gaps;
    for (const auto &p : found.profiles) {
        QVariantList holes;
        for (const auto &h : p.holes)
            holes << QVariant(loop(h));
        shapes << QVariantMap{{QStringLiteral("outer"), loop(p.outer)}, {QStringLiteral("holes"), holes},
                              {QStringLiteral("area"), p.area}};
    }
    for (const auto &gp : found.gaps)
        gaps << QVariantMap{{QStringLiteral("a"), point(gp.a)}, {QStringLiteral("b"), point(gp.b)},
                            {QStringLiteral("size"), gp.size}};
    return {{QStringLiteral("profiles"), shapes}, {QStringLiteral("gaps"), gaps}};
}

QVariantMap Document::findInWindow(const QString &key, QVector3D from, double halfAngle) const {
    // Points standing for each kind of row, in engine coordinates; `body` notes whether the
    // model's body is among them (it doesn't hide itself).
    std::vector<QVector3D> points;
    bool body = false;
    const float originSize = 150;   // the origin planes' half width (OriginShapes.qml)
    std::function<void(const QString &)> collect = [&](const QString &k) {
        const int slash = k.indexOf(QLatin1Char('/'));
        const QString part = slash < 0 ? QString() : k.mid(slash + 1);
        if (k.startsWith(QLatin1Char('b'))) {
            if (k.mid(1).toInt() == int(m_body) && m_bodies.count(int(m_body))) {
                body = true;
                for (std::size_t i = 0; i < m_mesh.positions.size() / 3; ++i)
                    points.push_back(at(m_mesh.positions, i));
            }
        } else if (k.startsWith(QLatin1Char('s'))) {
            const auto it = m_sketches.find(k.mid(1).toInt());
            if (it == m_sketches.end())
                return;
            const SketchGeometry &g = it->second.geometry;
            for (const auto &[id, p] : g.points)
                points.push_back(onSketch(it->second, p.u, p.v));
            for (const auto &[id, a] : g.arcs) {   // a circle's whole rim, not just its points
                const auto &c = g.points.at(a.c);
                const double r = g.radius(id);
                for (int q = 0; q < 4; ++q)
                    points.push_back(onSketch(it->second, c.u + r * std::cos(q * M_PI / 2), c.v + r * std::sin(q * M_PI / 2)));
            }
        } else if (part.isEmpty()) {   // a component: what it holds; an empty one, its origin
            const int id = k.mid(1).toInt();
            for (const Component &c : m_components)
                if (c.id == id) {
                    const std::size_t before = points.size();
                    for (int b : c.bodies)
                        collect(QStringLiteral("b%1").arg(b));
                    for (int s : c.sketches)
                        collect(QStringLiteral("s%1").arg(s));
                    for (int sub : c.components)
                        collect(QStringLiteral("c%1").arg(sub));
                    if (points.size() == before)
                        collect(k + QStringLiteral("/origin"));
                }
        } else if (part == QLatin1String("bodies") || part == QLatin1String("sketches")) {
            for (const Component &c : m_components)
                if (c.id == k.mid(1, slash - 1).toInt())
                    for (int id : part == QLatin1String("bodies") ? c.bodies : c.sketches)
                        collect(QStringLiteral("%1%2").arg(part.at(0)).arg(id));
        } else if (part == QLatin1String("origin")) {
            for (int i = 0; i < 8; ++i)
                points.push_back(QVector3D(i & 1 ? originSize : -originSize, i & 2 ? originSize : -originSize,
                                           i & 4 ? originSize : -originSize));
        } else if (part == QLatin1String("O")) {
            points.push_back(QVector3D());
        } else if (part.size() == 1) {   // an axis, as long as it's drawn
            QVector3D along;
            along[part.at(0).unicode() - 'X'] = originSize * 1.3f;
            points.push_back(along);
            points.push_back(-along);
        } else if (part.size() == 2) {   // a plane: its square
            for (int i = 0; i < 4; ++i) {
                QVector3D p;
                p[part.at(0).unicode() - 'X'] = i & 1 ? originSize : -originSize;
                p[part.at(1).unicode() - 'X'] = i & 2 ? originSize : -originSize;
                points.push_back(p);
            }
        }
    };
    collect(key);
    if (points.empty())
        return {{QStringLiteral("found"), false}};

    QVector3D lo = points[0], hi = points[0];
    for (const QVector3D &p : points) {
        lo = QVector3D(std::min(lo.x(), p.x()), std::min(lo.y(), p.y()), std::min(lo.z(), p.z()));
        hi = QVector3D(std::max(hi.x(), p.x()), std::max(hi.y(), p.y()), std::max(hi.z(), p.z()));
    }
    const QVector3D center = (lo + hi) / 2;
    const float radius = std::max(10.0f, (hi - lo).length() / 2);   // a lone point: a little room round it
    const float distance = radius / float(std::sin(halfAngle * M_PI / 180)) * 1.15f;
    auto toScene = [](const QVector3D &e) { return QVector3D(e.x(), e.z(), -e.y()); };
    auto toEngine = [](const QVector3D &s) { return QVector3D(s.x(), -s.z(), s.y()); };

    // What could hide it: the body, while it's seen and isn't what's being found.
    bool bodySeen = false;
    std::function<void(const QVariantMap &)> seen = [&](const QVariantMap &n) {
        if (n.value(QStringLiteral("key")).toString() == QStringLiteral("b%1").arg(m_body))
            bodySeen = n.value(QStringLiteral("visible")).toBool();
        for (const QVariant &c : n.value(QStringLiteral("children")).toList())
            seen(c.toMap());
    };
    seen(browser());
    const bool blockers = bodySeen && !body && !m_mesh.indices.empty();

    // How much of it a camera looking from `dir` (engine coordinates) would see: the share of
    // its points with no body anywhere on the way out towards the camera (not just up to it:
    // close up, the camera itself could stand inside the body). At most ~200 points are tried.
    const std::size_t step = std::max<std::size_t>(1, points.size() / 200);
    auto share = [&](const QVector3D &dir) {
        if (!blockers)
            return 1.0;
        const QVector3D camera = center + dir * distance;
        int clear = 0, tried = 0;
        for (std::size_t i = 0; i < points.size(); i += step, ++tried) {
            const QVector3D toCamera = camera - points[i];
            const QVector3D d = toCamera.normalized();
            const QVector3D o = points[i] + d * 0.01f;   // off its own surface (a sketch on a face)
            bool hidden = false;
            for (std::size_t t = 0; t + 2 < m_mesh.indices.size() && !hidden; t += 3)
                hidden = rayTriangle(o, d, at(m_mesh.positions, m_mesh.indices[t]), at(m_mesh.positions, m_mesh.indices[t + 1]),
                                     at(m_mesh.positions, m_mesh.indices[t + 2])) < INFINITY;
            clear += !hidden;
        }
        return double(clear) / tried;
    };

    // Look from where the camera is now if that sees (nearly) all of it; else the nearest way
    // round that does (or sees the most); wholly hidden from everywhere means it's inside a
    // body: stay looking this way and just zoom to where it is.
    const QVector3D now = toEngine(from.normalized());
    QVector3D best = now;
    const double here = share(now);
    bool inside = false;
    if (here < 0.9) {
        std::vector<std::pair<QVector3D, double>> ways;
        const int n = 300;
        double most = here;
        for (int i = 0; i < n; ++i) {   // evenly round a ball (a Fibonacci sphere)
            const double y = 1 - 2 * (i + 0.5) / n, r = std::sqrt(1 - y * y), a = i * M_PI * (3 - std::sqrt(5.0));
            const QVector3D dir(float(r * std::cos(a)), float(r * std::sin(a)), float(y));
            ways.push_back({dir, share(dir)});
            most = std::max(most, ways.back().second);
        }
        if (most <= 0) {
            inside = true;
        } else {
            // The nearest way that sees enough and stays clear of the edge of what's hidden
            // (every way within 25° of it sees enough too), so a sketch on top of a body isn't
            // shown skimming its surface; failing that, the nearest that sees enough.
            const double enough = std::min(0.9, most) - 1e-9;
            const float near25 = float(std::cos(25 * M_PI / 180));
            double nearest = -2, nearestSafe = -2;
            QVector3D safe;
            for (const auto &[dir, s] : ways) {
                if (s < enough)
                    continue;
                const double towards = QVector3D::dotProduct(dir, now);
                if (towards > nearest) {
                    nearest = towards;
                    best = dir;
                }
                bool clear = true;
                for (const auto &[other, t] : ways)
                    if (t < enough && QVector3D::dotProduct(dir, other) > near25) {
                        clear = false;
                        break;
                    }
                if (clear && towards > nearestSafe) {
                    nearestSafe = towards;
                    safe = dir;
                }
            }
            if (nearestSafe > -2)
                best = safe;
        }
    }
    return {{QStringLiteral("found"), true}, {QStringLiteral("center"), toScene(center)},
            {QStringLiteral("radius"), radius}, {QStringLiteral("distance"), distance},
            {QStringLiteral("direction"), toScene(best)}, {QStringLiteral("turned"), best != now},
            {QStringLiteral("inside"), inside}};
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
    // How far the face reaches from its centre: the sketch bar's look-at button frames it.
    float reach = 0;
    for (std::size_t t = 0; t < triCount; ++t)
        if (m_mesh.triangleFace[t] == std::uint32_t(face))
            for (int k = 0; k < 3; ++k)
                reach = std::max(reach, (at(m_mesh.positions, m_mesh.indices[t * 3 + k]) - center).length());
    plane.insert(QStringLiteral("radius"), reach);
    return plane;
}

void Document::fillParts(MeshGeometry *target, const QVariantList &parts, float edgeRadius, QVector3D eye, float perMm) const {
    auto radiusAt = [&](const QVector3D &p) { return perMm > 0 ? (p - eye).length() * perMm : edgeRadius; };
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
                const float r = radiusAt(at(line, k + 1));
                addTube(out, at(line, k), at(line, k + 1), r);
                addBall(out, at(line, k + 1), r);   // smooth joints
            }
        } else if (kind == QLatin1String("point") && index >= 0 && std::size_t(index) < m_mesh.points.size() / 3) {
            addBall(out, at(m_mesh.points, std::size_t(index)), radiusAt(at(m_mesh.points, std::size_t(index))) * 2.4f);
        } else if (kind.startsWith(QLatin1String("sketch_"))) {
            const auto found = m_sketches.find(part.value(QStringLiteral("sketch")).toInt());
            if (found == m_sketches.end())
                continue;
            const Named &s = found->second;
            const SketchGeometry &g = s.geometry;
            if (kind == QLatin1String("sketch_line") && g.lines.count(index)) {
                const auto &a = g.points.at(g.lines.at(index).a), &b = g.points.at(g.lines.at(index).b);
                addTube(out, onSketch(s, a.u, a.v), onSketch(s, b.u, b.v), edgeRadius);
            } else if ((kind == QLatin1String("sketch_arc") && g.arcs.count(index))
                       || (kind == QLatin1String("sketch_curve") && g.curves.count(index))) {
                for (const auto &path : kind == QLatin1String("sketch_arc") ? g.arcPath(index) : g.curvePath(index))
                    for (std::size_t i = 0; i + 1 < path.size(); ++i)
                        addTube(out, onSketch(s, path[i].u, path[i].v), onSketch(s, path[i + 1].u, path[i + 1].v), edgeRadius);
            } else if (kind == QLatin1String("sketch_point") && g.points.count(index)) {
                const auto &p = g.points.at(index);
                addBall(out, onSketch(s, p.u, p.v), edgeRadius * 2.4f);
            }
        }
    }
    const std::size_t triCount = m_mesh.indices.size() / 3;
    for (std::size_t t = 0; t < triCount && !faces.empty(); ++t) {
        if (!faces.count(m_mesh.triangleFace[t]))
            continue;
        // Lifted a hair off the surface: a copy in exactly the same place would lose the depth test.
        const std::uint32_t *idx = &m_mesh.indices[t * 3];
        QVector3D v[3], n[3];
        for (int k = 0; k < 3; ++k) {
            n[k] = at(m_mesh.normals, idx[k]);
            v[k] = at(m_mesh.positions, idx[k]);
            v[k] += n[k] * radiusAt(v[k]) * 0.3f;
        }
        addTriangle(out, v[0], v[1], v[2], n[0], n[1], n[2]);
    }
    if (out.positions.empty())
        for (int i = 0; i < 3; ++i)
            out.min[i] = out.max[i] = 0;
    target->setMesh(out);
}

// --- Drawing in sketches -------------------------------------------------------------

QVector3D Document::onSketch(const Named &sketch, double u, double v) const {
    const QVariantMap &p = sketch.plane;
    return p.value(QStringLiteral("origin")).value<QVector3D>()
         + p.value(QStringLiteral("xAxis")).value<QVector3D>() * float(u)
         + p.value(QStringLiteral("yAxis")).value<QVector3D>() * float(v);
}

QString Document::sketchPartName(const Named &sketch, QChar kind, int id) const {
    if (kind == QLatin1Char('A')) {
        const auto k = sketch.geometry.arcs.find(id);
        return QStringLiteral("%1%2").arg(k != sketch.geometry.arcs.end() && k->second.circle() ? QStringLiteral("Circle")
                                                                                               : QStringLiteral("Arc")).arg(id);
    }
    const bool point = kind == QLatin1Char('P');
    if (!SketchGeometry::isReference(id))
        return QStringLiteral("%1%2").arg(point ? QStringLiteral("Point") : QStringLiteral("Line")).arg(id);
    if (point)
        return QStringLiteral("the Origin");
    const QVector3D along = sketch.plane.value(id == SketchGeometry::XAxis ? QStringLiteral("xAxis") : QStringLiteral("yAxis"))
                                .value<QVector3D>();
    for (int i = 0; i < 3; ++i)
        if (std::abs(along[i]) > 0.9999f)
            return QStringLiteral("the %1 axis").arg(QLatin1Char("XYZ"[i]));
    return id == SketchGeometry::XAxis ? QStringLiteral("the sketch's horizontal axis") : QStringLiteral("the sketch's vertical axis");
}

// A point id as given, or a new point at {u, v} (its id added to created).
int Document::sketchPoint(Named &sketch, const QVariant &at, QJsonArray &created) {
    SketchGeometry &g = sketch.geometry;
    if (at.typeId() != QMetaType::QVariantMap) {
        const int id = at.toInt();
        return g.points.count(id) ? id : 0;
    }
    const QVariantMap uv = at.toMap();
    const int id = g.nextPoint++;
    g.points[id] = {uv.value(QStringLiteral("u")).toDouble(), uv.value(QStringLiteral("v")).toDouble()};
    created.append(id);
    return id;
}

// Takes back a Line or 2-Point Rectangle: its constraints, its lines, the points it made.
void Document::forgetSketchLines(Named &sketch, const QJsonObject &action) {
    SketchGeometry &g = sketch.geometry;
    for (const QJsonValue &v : action.value(QStringLiteral("constraints")).toArray())
        g.constraints.erase(v.toInt());
    for (const QJsonValue &v : action.value(QStringLiteral("lines")).toArray()) {
        g.lines.erase(v.toInt());
        g.constructionLines.erase(v.toInt());
    }
    for (const QJsonValue &v : action.value(QStringLiteral("arcs")).toArray()) {
        g.arcs.erase(v.toInt());
        g.constructionArcs.erase(v.toInt());
    }
    for (const QJsonValue &v : action.value(QStringLiteral("new_points")).toArray())
        g.points.erase(v.toInt());
    // The newest numbers go back, so the next line is named the same again.
    g.nextLine = g.lines.empty() ? 1 : g.lines.rbegin()->first + 1;
    g.nextArc = g.arcs.empty() ? 1 : g.arcs.rbegin()->first + 1;
    g.nextPoint = g.points.empty() ? 1 : g.points.rbegin()->first + 1;
    g.nextConstraint = g.constraints.empty() ? 1 : g.constraints.rbegin()->first + 1;
}

QVariantMap Document::sketchGeometry(int sketch) const {
    const auto found = m_sketches.find(sketch);
    if (found == m_sketches.end())
        return {};
    const SketchGeometry &g = found->second.geometry;
    QVariantList points, lines, arcs, constraints;
    const std::set<int> full = g.fullyConstrained();
    for (const auto &[id, p] : g.points)
        points << QVariantMap{{QStringLiteral("id"), id}, {QStringLiteral("u"), p.u}, {QStringLiteral("v"), p.v},
                              {QStringLiteral("projected"), bool(g.projectedPoints.count(id))}, {QStringLiteral("fixed"), g.isFixed(id)}, {QStringLiteral("full"), bool(full.count(id))},
                              {QStringLiteral("rim"), g.isRim(id)}, {QStringLiteral("hidden"), g.isHidden(id)}};
    // Helper lines and arcs (a polygon's circle…) are left out: nothing picks or snaps to them.
    QVariantList curves;
    for (const auto &[id, k] : g.curves) {
        QVariantList ids;
        for (int p : k.points)
            ids << p;
        curves << QVariantMap{{QStringLiteral("id"), id}, {QStringLiteral("type"), k.type}, {QStringLiteral("points"), ids},
                              {QStringLiteral("value"), k.value}, {QStringLiteral("text"), k.text},
                              {QStringLiteral("construction"), bool(g.constructionCurves.count(id))}};
    }
    for (const auto &[id, k] : g.arcs)
        if (!g.helperArcs.count(id))
            arcs << QVariantMap{{QStringLiteral("id"), id}, {QStringLiteral("c"), k.c}, {QStringLiteral("a"), k.a},
                            {QStringLiteral("b"), k.b}, {QStringLiteral("circle"), k.circle()},
                            {QStringLiteral("r"), g.radius(id)},
                            {QStringLiteral("construction"), bool(g.constructionArcs.count(id))},
                            {QStringLiteral("full"), full.count(k.c) && full.count(k.a) && full.count(k.b)}};
    for (const auto &[id, l] : g.lines)
        if (!g.helperLines.count(id))
            lines << QVariantMap{{QStringLiteral("id"), id}, {QStringLiteral("a"), l.a}, {QStringLiteral("b"), l.b},
                                 {QStringLiteral("construction"), bool(g.constructionLines.count(id))},
                                 {QStringLiteral("full"), full.count(l.a) && full.count(l.b)}};
    for (const auto &[id, c] : g.constraints)
        constraints << QVariantMap{{QStringLiteral("id"), id}, {QStringLiteral("type"), c.type},
                                   {QStringLiteral("first"), c.first}, {QStringLiteral("second"), c.second},
                                   {QStringLiteral("third"), c.third}, {QStringLiteral("u"), c.u}, {QStringLiteral("v"), c.v},
                                   {QStringLiteral("value"), g.valueOf(c)}, {QStringLiteral("unit"), c.unit},
                                   {QStringLiteral("follows"), c.follows()}};
    const SketchGeometry::Point &o = found->second.geometry.origin;
    return {{QStringLiteral("points"), points}, {QStringLiteral("lines"), lines}, {QStringLiteral("arcs"), arcs},
            {QStringLiteral("curves"), curves}, {QStringLiteral("constraints"), constraints},
            {QStringLiteral("origin"), QVariantMap{{QStringLiteral("u"), o.u}, {QStringLiteral("v"), o.v}}}};
}

void Document::holdSnap(SketchGeometry &g, int point, const QVariant &at, const QJsonArray &created, QJsonArray &constraints) {
    if (at.typeId() != QMetaType::QVariantMap || !created.contains(point))
        return;
    const QVariantMap m = at.toMap();
    const QString on = m.value(QStringLiteral("on")).toString();
    SketchGeometry::Constraint c;
    if (on == QLatin1String("origin"))
        c = {QStringLiteral("coincident"), SketchGeometry::Origin, point};
    else if (on == QLatin1String("midpoint") && g.lines.count(m.value(QStringLiteral("line")).toInt()))
        c = {QStringLiteral("midpoint"), point, m.value(QStringLiteral("line")).toInt()};
    else if (on == QLatin1String("line") && g.hasLine(m.value(QStringLiteral("line")).toInt()))
        c = {QStringLiteral("pointonline"), point, m.value(QStringLiteral("line")).toInt()};
    else if (on == QLatin1String("arc") && g.arcs.count(m.value(QStringLiteral("arc")).toInt()))
        c = {QStringLiteral("pointonarc"), point, m.value(QStringLiteral("arc")).toInt()};
    else
        return;
    const int id = g.nextConstraint++;
    g.constraints[id] = c;
    if (g.tiedToBoth()) {              // it stays there, just not tied
        g.constraints.erase(id);
        g.nextConstraint--;
        return;
    }
    constraints.append(id);
}

QVariantMap Document::addSketchLine(int sketch, const QVariant &from, const QVariant &to, const QString &hold,
                                    bool construction) {
    const auto found = m_sketches.find(sketch);
    if (found == m_sketches.end())
        return {};
    Named &s = found->second;
    SketchGeometry &g = s.geometry;
    QJsonArray created;
    const int a = sketchPoint(s, from, created);
    const int b = sketchPoint(s, to, created);
    auto dropCreated = [&] {
        for (const QJsonValue &v : created)
            g.points.erase(v.toInt());
        g.nextPoint -= int(created.size());
    };
    if (!a || !b || a == b || std::hypot(g.points[a].u - g.points[b].u, g.points[a].v - g.points[b].v) < 1e-6) {
        dropCreated();
        return {};
    }
    const int line = g.nextLine++;
    g.lines[line] = {a, b};
    if (construction)
        g.constructionLines.insert(line);
    // What it was snapped to (owner, 2026-10-30): placed exactly there already, so nothing moves.
    QJsonArray constraints;
    holdSnap(g, a, from, created, constraints);
    holdSnap(g, b, to, created, constraints);
    if (hold == QLatin1String("horizontal") || hold == QLatin1String("vertical")) {
        const int id = g.nextConstraint++;
        g.constraints[id] = {hold, line, 0};
        constraints.append(id);
    }
    auto uv = [&](int id) { return QJsonArray{g.points[id].u, g.points[id].v}; };
    record({{QStringLiteral("type"), QStringLiteral("line")}, {QStringLiteral("sketch"), sketch},
            {QStringLiteral("lines"), QJsonArray{line}}, {QStringLiteral("from"), a}, {QStringLiteral("to"), b},
            {QStringLiteral("from_uv"), uv(a)}, {QStringLiteral("to_uv"), uv(b)},
            {QStringLiteral("new_points"), created}, {QStringLiteral("constraints"), constraints}},
           QStringLiteral("Drew Line%1").arg(line));
    sketchesEdited();
    return {{QStringLiteral("line"), line}, {QStringLiteral("from"), a}, {QStringLiteral("to"), b}};
}

QVariantMap Document::addSketchRectangle(int sketch, const QVariant &corner, const QVariant &opposite, bool construction) {
    const auto found = m_sketches.find(sketch);
    if (found == m_sketches.end())
        return {};
    Named &s = found->second;
    SketchGeometry &g = s.geometry;
    QJsonArray created;
    const int p0 = sketchPoint(s, corner, created);
    const int p2 = sketchPoint(s, opposite, created);
    if (!p0 || !p2 || p0 == p2 || std::abs(g.points[p0].u - g.points[p2].u) < 1e-6
        || std::abs(g.points[p0].v - g.points[p2].v) < 1e-6) {
        for (const QJsonValue &v : created)
            g.points.erase(v.toInt());
        g.nextPoint -= int(created.size());
        return {};
    }
    // The other two corners, square to the sketch's axes.
    const int p1 = sketchPoint(s, QVariantMap{{QStringLiteral("u"), g.points[p2].u}, {QStringLiteral("v"), g.points[p0].v}}, created);
    const int p3 = sketchPoint(s, QVariantMap{{QStringLiteral("u"), g.points[p0].u}, {QStringLiteral("v"), g.points[p2].v}}, created);
    const int corners[4] = {p0, p1, p2, p3};
    QJsonArray lines, constraints;
    int ids[4];
    for (int i = 0; i < 4; ++i) {
        ids[i] = g.nextLine++;
        g.lines[ids[i]] = {corners[i], corners[(i + 1) % 4]};
        if (construction)
            g.constructionLines.insert(ids[i]);
        lines.append(ids[i]);
    }
    // Held as a rectangle by its own sides only: square corners and opposite sides parallel,
    // never Horizontal or Vertical (owner, 2026-10-30: it's drawn level, but stays free to be
    // turned to any angle).
    auto hold = [&](const char *type, int line, int other) {
        const int id = g.nextConstraint++;
        g.constraints[id] = {QString::fromLatin1(type), line, other};
        constraints.append(id);
    };
    hold("perpendicular", ids[0], ids[1]);
    hold("parallel", ids[0], ids[2]);
    hold("parallel", ids[1], ids[3]);
    holdSnap(g, p0, corner, created, constraints);
    holdSnap(g, p2, opposite, created, constraints);
    record({{QStringLiteral("type"), QStringLiteral("rectangle")}, {QStringLiteral("sketch"), sketch},
            {QStringLiteral("lines"), lines}, {QStringLiteral("constraints"), constraints},
            {QStringLiteral("corners"), QJsonArray{p0, p1, p2, p3}}, {QStringLiteral("new_points"), created}},
           QStringLiteral("Drew a 2-Point Rectangle (Line%1 to Line%2)").arg(ids[0]).arg(ids[3]));
    sketchesEdited();
    return {{QStringLiteral("lines"), QVariantList{ids[0], ids[1], ids[2], ids[3]}}};
}

QVariantMap Document::projectIntoSketch(int sketch, int face) {
    const auto found = m_sketches.find(sketch);
    if (found == m_sketches.end())
        return {};
    auto refuse = [](const QString &why) { return QVariantMap{{QStringLiteral("why"), why}}; };
    if (m_mesh.indices.empty())
        return refuse(QStringLiteral("There's no model to project."));
    Named &s = found->second;
    SketchGeometry &g = s.geometry;
    const QVector3D origin = s.plane.value(QStringLiteral("origin")).value<QVector3D>();
    const QVector3D xAxis = s.plane.value(QStringLiteral("xAxis")).value<QVector3D>();
    const QVector3D yAxis = s.plane.value(QStringLiteral("yAxis")).value<QVector3D>();
    const auto flat = [&](std::uint32_t i) {
        const QVector3D p = at(m_mesh.positions, i) - origin;
        return projection::Flat{QVector3D::dotProduct(p, xAxis), QVector3D::dotProduct(p, yAxis)};
    };
    std::vector<std::array<projection::Flat, 3>> triangles;
    for (std::size_t t = 0; t < m_mesh.indices.size() / 3; ++t)
        if (face < 0 || int(m_mesh.triangleFace[t]) == face) {
            const projection::Flat a = flat(m_mesh.indices[t * 3]), b = flat(m_mesh.indices[t * 3 + 1]), c = flat(m_mesh.indices[t * 3 + 2]);
            // A closed model's shadow is all in the triangles facing one way; a face may fold.
            if (face >= 0 || (b.u - a.u) * (c.v - a.v) - (b.v - a.v) * (c.u - a.u) > 0)
                triangles.push_back({a, b, c});
        }
    if (triangles.empty())
        return refuse(QStringLiteral("Click a face of the model."));

    const double size = std::hypot(std::hypot(m_mesh.max[0] - m_mesh.min[0], m_mesh.max[1] - m_mesh.min[1]), m_mesh.max[2] - m_mesh.min[2]);
    const double tol = std::max(0.012, size / 25000);   // twice how far the drawn triangles may stray
    std::vector<std::vector<projection::Piece>> loops = projection::outline(triangles, tol);
    if (loops.empty()) {
        // A flat face seen edge on throws no shadow: it's the line between its furthest points.
        projection::Flat a = triangles[0][0], b = a;
        for (int round = 0; round < 2; ++round) {
            const projection::Flat from = round ? a : b;
            double far = -1;
            for (const auto &t : triangles)
                for (const projection::Flat &p : t) {
                    const double d = std::hypot(p.u - from.u, p.v - from.v);
                    if (d > far) {
                        far = d;
                        (round ? b : a) = p;
                    }
                }
        }
        if (std::hypot(a.u - b.u, a.v - b.v) < 1e-6)
            return refuse(QStringLiteral("That face throws no outline on this sketch."));
        loops.push_back({projection::Piece{projection::Piece::Line, a, b, {}}});
    }

    const SketchGeometry before = g;
    // A corner two pieces share, or an earlier projection's, is one point.
    std::map<std::pair<long long, long long>, int> known;
    const auto cell = [](const projection::Flat &p) { return std::pair{std::llround(p.u * 1e5), std::llround(p.v * 1e5)}; };
    for (const int id : g.projectedPoints)
        if (g.points.count(id))
            known[cell({g.points[id].u, g.points[id].v})] = id;
    const auto pointAt = [&](const projection::Flat &p) {
        const auto [it, fresh] = known.try_emplace(cell(p), 0);
        if (fresh) {
            it->second = g.nextPoint++;
            g.points[it->second] = {p.u, p.v};
            g.projectedPoints.insert(it->second);
        }
        return it->second;
    };
    int lines = 0, arcs = 0, circles = 0;
    for (const auto &loop : loops)
        for (const projection::Piece &piece : loop) {
            const int a = pointAt(piece.a);
            if (piece.kind == projection::Piece::Line) {
                const int b = pointAt(piece.b);
                if (a == b || std::any_of(g.lines.begin(), g.lines.end(), [&](const auto &l) {
                        return (l.second.a == a && l.second.b == b) || (l.second.a == b && l.second.b == a); }))
                    continue;
                g.lines[g.nextLine++] = {a, b};
                ++lines;
            } else {
                const bool newCentre = !known.count(cell(piece.c));
                const int c = pointAt(piece.c);
                if (piece.kind == projection::Piece::Arc && newCentre)   // only a circle's is worth showing:
                    g.helperPoints.insert(c);                            // a shallow arc's floats far outside
                const int b = piece.kind == projection::Piece::Circle ? a : pointAt(piece.b);
                if (a == b && piece.kind == projection::Piece::Arc)
                    continue;
                if (std::any_of(g.arcs.begin(), g.arcs.end(), [&](const auto &k) {
                        return k.second.c == c && k.second.a == a && k.second.b == b; }))
                    continue;
                g.arcs[g.nextArc++] = {c, a, b};
                ++(piece.kind == projection::Piece::Circle ? circles : arcs);
            }
        }
    if (!lines && !arcs && !circles) {
        g = before;
        return refuse(QStringLiteral("That's already projected."));
    }
    const int snapshot = m_nextSnapshot++;
    m_snapshots[snapshot] = before;
    const QString summary = QStringLiteral("Projected %1 onto the sketch (%2 lines, %3 arcs, %4 circles)")
                                .arg(face < 0 ? QStringLiteral("the whole model") : QStringLiteral("a face"))
                                .arg(lines).arg(arcs).arg(circles);
    record({{QStringLiteral("type"), QStringLiteral("shape")}, {QStringLiteral("sketch"), sketch},
            {QStringLiteral("snapshot"), snapshot}}, summary);
    sketchesEdited();
    return {{QStringLiteral("summary"), summary}, {QStringLiteral("lines"), lines}, {QStringLiteral("arcs"), arcs},
            {QStringLiteral("circles"), circles}};
}

QVariantMap Document::addSketchShape(int sketch, const QVariantMap &spec) {
    const auto found = m_sketches.find(sketch);
    if (found == m_sketches.end())
        return {};
    Named &s = found->second;
    SketchGeometry &g = s.geometry;
    const SketchGeometry before = g;
    auto refuse = [&](const QString &why) {
        g = before;
        return QVariantMap{{QStringLiteral("why"), why}};
    };
    QJsonArray created, lineIds, arcIds, curveIds, constraintIds;
    QVariantList pointIds, lineList, arcList, curveList, constraintList;
    const QVariantList specPoints = spec.value(QStringLiteral("points")).toList();
    for (const QVariant &at : specPoints) {
        const int id = sketchPoint(s, at, created);
        if (!id)
            return refuse(QStringLiteral("That point is gone."));
        if (at.toMap().value(QStringLiteral("helper")).toBool())
            g.helperPoints.insert(id);
        pointIds << id;
    }
    // "p2" → this shape's third point, "l0" its first line, "a1" its second arc, "k0" its first
    // curve, "c0" its first constraint; plain numbers are ids already in the sketch.
    auto ref = [&](const QVariant &v) {
        const QString text = v.toString();
        if (v.typeId() == QMetaType::QString && !text.isEmpty() && text[0].isLetter()) {
            const int i = text.mid(1).toInt();
            const QChar k = text[0];
            const QVariantList &list = k == QLatin1Char('p') ? pointIds : k == QLatin1Char('l') ? lineList
                                     : k == QLatin1Char('a') ? arcList : k == QLatin1Char('k') ? curveList : constraintList;
            return list.value(i).toInt();
        }
        return v.toInt();
    };
    // Lines', arcs' and curves' ends are indexes into `points` (list an existing point there by its id).
    auto end = [&](const QVariant &v) { return v.typeId() == QMetaType::QString ? ref(v) : pointIds.value(v.toInt()).toInt(); };
    for (const QVariant &v : spec.value(QStringLiteral("lines")).toList()) {
        const QVariantList ends = v.toList();
        const int id = g.nextLine++;
        g.lines[id] = {end(ends.value(0)), end(ends.value(1))};
        if (ends.value(2).toBool())
            g.helperLines.insert(id);
        lineIds.append(id);
        lineList << id;
    }
    for (const QVariant &v : spec.value(QStringLiteral("arcs")).toList()) {
        const QVariantList k = v.toList();
        const int id = g.nextArc++;
        g.arcs[id] = {end(k.value(0)), end(k.value(1)), end(k.value(2))};
        if (k.value(3).toBool())
            g.helperArcs.insert(id);
        arcIds.append(id);
        arcList << id;
    }
    for (const QVariant &v : spec.value(QStringLiteral("curves")).toList()) {
        const QVariantMap m = v.toMap();
        SketchGeometry::Curve k;
        k.type = m.value(QStringLiteral("type")).toString();
        for (const QVariant &p : m.value(QStringLiteral("points")).toList())
            k.points.push_back(end(p));
        k.value = m.value(QStringLiteral("value")).toDouble();
        k.text = m.value(QStringLiteral("text")).toString();
        const int id = g.nextCurve++;
        g.curves[id] = k;
        curveIds.append(id);
        curveList << id;
    }
    // Construction: everything new when the switch is on, except copies, which follow their originals.
    {
        const bool on = spec.value(QStringLiteral("construction")).toBool();
        for (const QVariant &v : lineList)
            if (on && !g.helperLines.count(v.toInt()))
                g.constructionLines.insert(v.toInt());
        for (const QVariant &v : arcList)
            if (on && !g.helperArcs.count(v.toInt()))
                g.constructionArcs.insert(v.toInt());
        for (const QVariant &v : curveList)
            if (on)
                g.constructionCurves.insert(v.toInt());
        for (const QVariant &v : spec.value(QStringLiteral("like")).toList()) {
            const QVariantMap m = v.toMap();
            const QString made = m.value(QStringLiteral("made")).toString();
            const int id = ref(made), from = m.value(QStringLiteral("from")).toInt();
            std::set<int> &marks = made.startsWith(QLatin1Char('l')) ? g.constructionLines
                                 : made.startsWith(QLatin1Char('a')) ? g.constructionArcs : g.constructionCurves;
            if (marks.count(from))
                marks.insert(id);
            else
                marks.erase(id);
        }
    }
    // Ends moved onto other points (Fillet, Trim…): {line | arc: id, end: "a" | "b" | "c", to: point}.
    std::set<int> seeds;
    for (const QJsonValue &v : created)
        seeds.insert(v.toInt());
    for (const QVariant &v : spec.value(QStringLiteral("rewire")).toList()) {
        const QVariantMap m = v.toMap();
        const QString which = m.value(QStringLiteral("end")).toString();
        const int to = ref(m.value(QStringLiteral("to")));
        if (m.contains(QStringLiteral("line"))) {
            const auto l = g.lines.find(ref(m.value(QStringLiteral("line"))));
            if (l != g.lines.end())
                (which == QLatin1String("a") ? l->second.a : l->second.b) = to;
        } else {
            const auto k = g.arcs.find(ref(m.value(QStringLiteral("arc"))));
            if (k != g.arcs.end())
                (which == QLatin1String("c") ? k->second.c : which == QLatin1String("a") ? k->second.a : k->second.b) = to;
        }
        seeds.insert(to);
    }
    for (const QVariant &v : spec.value(QStringLiteral("move")).toList()) {
        const QVariantMap m = v.toMap();
        const int id = ref(m.value(QStringLiteral("point")));
        if (g.points.count(id)) {
            g.points[id] = {m.value(QStringLiteral("u")).toDouble(), m.value(QStringLiteral("v")).toDouble()};
            seeds.insert(id);
        }
    }
    const QVariantMap remove = spec.value(QStringLiteral("remove")).toMap();
    for (const QVariant &v : remove.value(QStringLiteral("constraints")).toList())
        g.constraints.erase(ref(v));
    for (const QVariant &v : remove.value(QStringLiteral("lines")).toList()) {
        g.lines.erase(ref(v));
        g.helperLines.erase(ref(v));
        g.constructionLines.erase(ref(v));
    }
    for (const QVariant &v : remove.value(QStringLiteral("arcs")).toList()) {
        g.arcs.erase(ref(v));
        g.helperArcs.erase(ref(v));
        g.constructionArcs.erase(ref(v));
    }
    for (const QVariant &v : remove.value(QStringLiteral("curves")).toList()) {
        g.curves.erase(ref(v));
        g.constructionCurves.erase(ref(v));
    }
    // A point goes only when nothing drawn uses it any more.
    auto used = [&](int p) {
        for (const auto &[id, l] : g.lines)
            if (l.a == p || l.b == p)
                return true;
        for (const auto &[id, k] : g.arcs)
            if (k.c == p || k.a == p || k.b == p)
                return true;
        for (const auto &[id, k] : g.curves)
            if (std::find(k.points.begin(), k.points.end(), p) != k.points.end())
                return true;
        return false;
    };
    for (const QVariant &v : remove.value(QStringLiteral("points")).toList()) {
        const int p = ref(v);
        if (!used(p)) {
            g.points.erase(p);
            g.helperPoints.erase(p);
            g.projectedPoints.erase(p);
        }
    }
    for (const QVariant &v : spec.value(QStringLiteral("helpers")).toList())
        if (g.points.count(ref(v)))
            g.helperPoints.insert(ref(v));
    for (const QVariant &v : spec.value(QStringLiteral("constraints")).toList()) {
        const QVariantMap m = v.toMap();
        SketchGeometry::Constraint c(m.value(QStringLiteral("type")).toString(), ref(m.value(QStringLiteral("first"))),
                                     ref(m.value(QStringLiteral("second"))), m.value(QStringLiteral("u")).toDouble(),
                                     m.value(QStringLiteral("v")).toDouble());
        c.third = ref(m.value(QStringLiteral("third")));
        c.value = m.value(QStringLiteral("value")).toDouble();
        c.unit = m.value(QStringLiteral("unit")).toString();
        if (c.type == QLatin1String("tangentarcs") && !m.contains(QStringLiteral("u")) && g.arcs.count(c.first) && g.arcs.count(c.second)) {
            const auto a = g.point(g.arcs.at(c.first).c), b = g.point(g.arcs.at(c.second).c);
            const double d = std::hypot(b.u - a.u, b.v - a.v), r1 = g.radius(c.first), r2 = g.radius(c.second);
            c.u = std::abs(d - (r1 + r2)) <= std::abs(d - std::abs(r1 - r2)) ? 1 : 0;
        }
        const int id = g.nextConstraint++;
        g.constraints[id] = c;
        constraintIds.append(id);
        constraintList << id;
    }
    // Sketch Scale: dimensions take new values ({constraint, value}).
    for (const QVariant &v : spec.value(QStringLiteral("values")).toList()) {
        const QVariantMap m = v.toMap();
        const auto c = g.constraints.find(m.value(QStringLiteral("constraint")).toInt());
        if (c != g.constraints.end())
            c->second.value = m.value(QStringLiteral("value")).toDouble();
    }
    // Constraints on anything that's gone go too; a follower whose leader went keeps its value.
    const auto dropDangling = [&] {
        for (auto it = g.constraints.begin(); it != g.constraints.end();) {
            const QString made = SketchGeometry::kinds(it->second.type);
            const int ids[3] = {it->second.first, it->second.second, it->second.third};
            bool gone = false;
            for (int i = 0; i < made.size(); ++i)
                gone = gone || (made[i] == QLatin1Char('P') ? !g.hasPoint(ids[i])
                                : made[i] == QLatin1Char('A') ? !g.arcs.count(ids[i]) : !g.hasLine(ids[i]));
            if (it->second.type == QLatin1String("fix") && !g.points.count(it->second.first))
                gone = true;
            if (gone) {
                it = g.constraints.erase(it);
                continue;
            }
            if (it->second.follows() && !g.constraints.count(it->second.third)) {
                it->second.value = before.constraints.count(it->second.third) ? before.valueOf(it->second) : it->second.value;
                it->second.third = 0;
            }
            ++it;
        }
    };
    dropDangling();
    // Helpers (a polygon's circle, a chamfer's lost corner) live only for what they hold: once a
    // delete leaves one tied to nothing that's seen, it goes too; so does one whose point was deleted.
    if (!remove.isEmpty()) {
        std::set<int> deletedPoints;
        for (const QVariant &v : remove.value(QStringLiteral("points")).toList())
            deletedPoints.insert(ref(v));
        const auto seenUses = [](const SketchGeometry &in, int p) {
            for (const auto &[id, l] : in.lines)
                if (!in.helperLines.count(id) && (l.a == p || l.b == p))
                    return true;
            for (const auto &[id, k] : in.arcs)
                if (!in.helperArcs.count(id) && (k.c == p || k.a == p || k.b == p))
                    return true;
            for (const auto &[id, k] : in.curves)
                if (std::find(k.points.begin(), k.points.end(), p) != k.points.end())
                    return true;
            return false;
        };
        // Tied: something seen uses one of its points, or a constraint joins it to something seen.
        const auto tied = [&](const SketchGeometry &in, std::set<int> own, char kind, int id) {
            if (std::any_of(own.begin(), own.end(), [&](int p) { return seenUses(in, p); }))
                return true;
            for (const auto &[cid, c] : in.constraints) {
                const QString made = SketchGeometry::kinds(c.type);
                const int ids[3] = {c.first, c.second, c.third};
                bool mine = false, seen = false;
                for (int i = 0; i < made.size(); ++i) {
                    const char slot = char(made[i].toLatin1());
                    if ((slot == 'P' && own.count(ids[i])) || (slot == kind && ids[i] == id))
                        mine = true;
                    else if (slot == 'P' ? (in.points.count(ids[i]) && seenUses(in, ids[i]))
                             : slot == 'A' ? (in.arcs.count(ids[i]) && !in.helperArcs.count(ids[i]))
                                           : (in.lines.count(ids[i]) && !in.helperLines.count(ids[i])))
                        seen = true;
                }
                if (mine && seen)
                    return true;
            }
            return false;
        };
        bool swept = false;
        for (const int id : std::set<int>(g.helperArcs)) {
            if (!g.arcs.count(id))
                continue;
            const auto k = g.arcs.at(id);
            const std::set<int> own{k.c, k.a, k.b};
            const bool pointDeleted = deletedPoints.count(k.c) || deletedPoints.count(k.a);
            if (pointDeleted || (tied(before, own, 'A', id) && !tied(g, own, 'A', id))) {
                g.arcs.erase(id);
                g.helperArcs.erase(id);
                for (const int p : own)
                    if (!used(p)) {
                        g.points.erase(p);
                        g.helperPoints.erase(p);
                    }
                swept = true;
            }
        }
        for (const int id : std::set<int>(g.helperLines)) {
            if (!g.lines.count(id))
                continue;
            const auto l = g.lines.at(id);
            const std::set<int> own{l.a, l.b};
            if (tied(before, own, 'L', id) && !tied(g, own, 'L', id)) {
                g.lines.erase(id);
                g.helperLines.erase(id);
                for (const int p : own)
                    if (!used(p)) {
                        g.points.erase(p);
                        g.helperPoints.erase(p);
                    }
                swept = true;
            }
        }
        for (const int p : std::set<int>(g.helperPoints))
            if (!used(p) && tied(before, {p}, 'P', 0) && !tied(g, {p}, 'P', 0)) {
                g.points.erase(p);
                g.helperPoints.erase(p);
                swept = true;
            }
        for (const int p : deletedPoints)
            if (g.points.count(p) && !used(p)) {
                g.points.erase(p);
                g.helperPoints.erase(p);
            }
        if (swept)
            dropDangling();
    }
    for (int i = 0; i < specPoints.size(); ++i)
        holdSnap(g, pointIds[i].toInt(), specPoints[i], created, constraintIds);
    if (g.error() > 1e-6) {
        std::set<int> steady;
        for (const QVariant &v : spec.value(QStringLiteral("steady")).toList())
            steady.insert(ref(v));
        if (!g.solveNear(seeds, steady))
            return refuse(QStringLiteral("That can't hold with the constraints already there."));
    }
    const int snapshot = m_nextSnapshot++;
    m_snapshots[snapshot] = before;
    record({{QStringLiteral("type"), QStringLiteral("shape")}, {QStringLiteral("sketch"), sketch},
            {QStringLiteral("snapshot"), snapshot},
            {QStringLiteral("lines"), lineIds}, {QStringLiteral("arcs"), arcIds}, {QStringLiteral("curves"), curveIds},
            {QStringLiteral("constraints"), constraintIds}, {QStringLiteral("new_points"), created}},
           spec.value(QStringLiteral("summary")).toString());
    sketchesEdited();
    return {{QStringLiteral("points"), pointIds}, {QStringLiteral("lines"), lineList}, {QStringLiteral("arcs"), arcList},
            {QStringLiteral("curves"), curveList}, {QStringLiteral("constraints"), constraintList}};
}

QVariantList Document::sketchTextOutline(const QString &text, double height) const {
    QVariantList out;
    for (const auto &shape : SketchGeometry::textOutline(text, height)) {
        QVariantList path;
        for (const auto &p : shape)
            path << QVariantMap{{QStringLiteral("u"), p.u}, {QStringLiteral("v"), p.v}};
        out << QVariant(path);
    }
    return out;
}

namespace {
const QString kTiedToBoth = QStringLiteral("That would tie projected geometry to the origin: they'd fight when the model moves.");
bool wouldTieBoth(SketchGeometry g, const QString &type, int first, int second, int third = 0) {
    if (g.projectedPoints.empty())
        return false;
    SketchGeometry::Constraint c(type, first, second);
    c.third = third;
    g.constraints[g.nextConstraint] = c;
    return g.tiedToBoth();
}
}

QString Document::addSketchConstraint(int sketch, const QString &type, int first, int second, int third, int keep) {
    const auto found = m_sketches.find(sketch);
    if (found == m_sketches.end())
        return QStringLiteral("No sketch is open.");
    SketchGeometry &g = found->second.geometry;
    if (wouldTieBoth(g, type, first, second, third))
        return kTiedToBoth;
    if (type != QLatin1String("parallel") && type != QLatin1String("perpendicular") && type != QLatin1String("equal"))
        return addOtherConstraint(found->second, sketch, type, first, second, third, keep);
    if (!g.hasLine(first) || !g.hasLine(second) || first == second)
        return QStringLiteral("Select two lines first.");
    if (type == QLatin1String("equal") && (SketchGeometry::isReference(first) || SketchGeometry::isReference(second)))
        return QStringLiteral("An axis has no length to match.");
    const QString name = type == QLatin1String("parallel") ? QStringLiteral("Parallel")
                       : type == QLatin1String("equal")  ? QStringLiteral("Equal")
                                                         : QStringLiteral("Perpendicular");
    for (const auto &[id, c] : g.constraints)
        if (c.type == type && ((c.first == first && c.second == second) || (c.first == second && c.second == first)))
            return QStringLiteral("Those two lines are already %1.").arg(name.toLower());

    const std::map<int, SketchGeometry::Point> before = g.points;
    const int id = g.nextConstraint++;
    g.constraints[id] = {type, first, second};
    // The second line turns to meet the first, as in Fusion; when the second is held (a
    // rectangle's side), the first turns instead.
    if (!g.solve({g.line(first).a, g.line(first).b}) && (g.points = before, !g.solve())) {
        g.constraints.erase(id);
        g.nextConstraint--;
        g.points = before;
        return QStringLiteral("Those lines can't be made %1 with the constraints they already have.").arg(name.toLower());
    }
    QJsonArray moved;
    for (const auto &[pid, p] : before) {
        const auto &now = g.points[pid];
        if (std::abs(now.u - p.u) > 1e-12 || std::abs(now.v - p.v) > 1e-12)
            moved.append(QJsonObject{{QStringLiteral("id"), pid}, {QStringLiteral("u"), p.u}, {QStringLiteral("v"), p.v}});
    }
    record({{QStringLiteral("type"), QStringLiteral("constraint")}, {QStringLiteral("sketch"), sketch},
            {QStringLiteral("id"), id}, {QStringLiteral("constraint"), type},
            {QStringLiteral("first"), first}, {QStringLiteral("second"), second},
            {QStringLiteral("moved"), moved}},   // where those points were, for Ctrl+Z
           QStringLiteral("Made %1 and %2 %3").arg(sketchPartName(found->second, QLatin1Char('L'), first),
                                                   sketchPartName(found->second, QLatin1Char('L'), second), name.toLower()));
    sketchesEdited();
    return {};
}

namespace {
QJsonArray movedSince(const SketchGeometry &g, const std::map<int, SketchGeometry::Point> &before);
}

// Everything but Parallel, Perpendicular and Equal (owner, 2026-10-29: the rest of Fusion's
// constraints, two at a time).
QString Document::addOtherConstraint(Named &named, int sketch, const QString &type, int first, int second, int third, int keep) {
    SketchGeometry &g = named.geometry;
    const QString made = SketchGeometry::kinds(type);
    if (made.isEmpty() || type == QLatin1String("fix") || SketchGeometry::Constraint(type, 0).isDimension())
        return QStringLiteral("There's no such constraint.");
    const int ids[3] = {first, second, third};
    auto label = [&](int i) { return sketchPartName(named, made[i], ids[i]); };
    for (int i = 0; i < made.size(); ++i)
        if (made[i] == QLatin1Char('P') ? !g.hasPoint(ids[i])
            : made[i] == QLatin1Char('A') ? !g.arcs.count(ids[i]) : !g.hasLine(ids[i]))
            return QStringLiteral("Those can't take that constraint.");
    // The origin and the axes never move, so some things can't be asked of them.
    const bool ref0 = SketchGeometry::isReference(first), ref1 = made.size() > 1 && SketchGeometry::isReference(second);
    if (ref0 && (made.size() == 1 || ref1))
        return made.size() == 1 ? QStringLiteral("The axes are already level and upright.")
                                : QStringLiteral("Those are both part of the origin: they're already where they are.");
    if (type == QLatin1String("midpoint") && ref1)
        return QStringLiteral("An axis has no middle: pick a line.");
    if (type == QLatin1String("symmetrylines") && (ref0 || ref1))
        return QStringLiteral("An axis can be the mirror line, but not one of the pair.");
    if (made.size() >= 2 && made[0] == made[1] && first == second)
        return QStringLiteral("Pick two different ones.");
    for (const auto &[id, c] : g.constraints)
        if (c.type == type && c.third == third
            && ((c.first == first && c.second == second) || (made[0] == made[1] && c.first == second && c.second == first)))
            return QStringLiteral("That's already there.");

    const auto endsOf = [&](int line) { const auto l = g.line(line); return std::set<int>{l.a, l.b}; };
    const auto arcPoints = [&](int arc) { const auto &k = g.arcs.at(arc); return std::set<int>{k.c, k.a, k.b}; };
    SketchGeometry::Constraint c(type, first, second);
    c.third = third;
    QString summary;
    if (type == QLatin1String("horizontal") || type == QLatin1String("vertical")) {
        summary = QStringLiteral("Made %1 %2").arg(label(0), type);
    } else if (type == QLatin1String("horizontalpoints") || type == QLatin1String("verticalpoints")) {
        summary = QStringLiteral("Lined up %1 and %2 %3").arg(label(0), label(1),
                      type == QLatin1String("horizontalpoints") ? QStringLiteral("level") : QStringLiteral("one above the other"));
    } else if (type == QLatin1String("coincident")) {
        for (const auto &[lid, l] : g.lines)
            if ((l.a == first && l.b == second) || (l.a == second && l.b == first))
                return QStringLiteral("Those are the two ends of Line%1: they can't meet.").arg(lid);
        summary = QStringLiteral("Made %1 coincident with %2").arg(label(0), label(1));
    } else if (type == QLatin1String("pointonline")) {
        if (endsOf(second).count(first))
            return QStringLiteral("That point is already on that line (it's one of its ends).");
        summary = QStringLiteral("Put %1 on %2").arg(label(0), label(1));
    } else if (type == QLatin1String("midpoint")) {
        if (endsOf(second).count(first))
            return QStringLiteral("An end of a line can't be its own midpoint.");
        summary = QStringLiteral("Put %1 at the midpoint of %2").arg(label(0), label(1));
    } else if (type == QLatin1String("collinear")) {
        summary = QStringLiteral("Made %1 and %2 collinear").arg(label(0), label(1));
    } else if (type == QLatin1String("symmetry") || type == QLatin1String("symmetrylines")) {
        if (third == first || third == second)
            return QStringLiteral("Pick a different line to mirror across.");
        const std::set<int> axis = endsOf(third);
        if (type == QLatin1String("symmetry") && (axis.count(first) || axis.count(second)))
            return QStringLiteral("A point on the mirror line can't mirror another point.");
        if (type == QLatin1String("symmetrylines")) {
            // Pair the ends the way they nearly mirror already.
            const auto l = g.line(first), m = g.line(second), x = g.line(third);
            const auto o = g.point(x.a), e = g.point(x.b);
            const double dx = e.u - o.u, dy = e.v - o.v, len2 = dx * dx + dy * dy;
            if (len2 < 1e-18)
                return QStringLiteral("The mirror line has no length.");
            auto mirror = [&](const SketchGeometry::Point &p) {
                const double s = ((p.u - o.u) * dx + (p.v - o.v) * dy) / len2;
                const double fu = o.u + dx * s, fv = o.v + dy * s;
                return SketchGeometry::Point{2 * fu - p.u, 2 * fv - p.v};
            };
            auto gap = [&](int p, int q) {
                const auto mp = mirror(g.point(p));
                const auto pq = g.point(q);
                return std::hypot(mp.u - pq.u, mp.v - pq.v);
            };
            c.u = gap(l.a, m.a) + gap(l.b, m.b) <= gap(l.a, m.b) + gap(l.b, m.a) ? 0 : 1;
        }
        summary = QStringLiteral("Made %1 and %2 symmetric about %3").arg(label(0), label(1), label(2));
    } else if (type == QLatin1String("tangent")) {
        summary = QStringLiteral("Made %1 tangent to %2").arg(label(0), label(1));
    } else if (type == QLatin1String("tangentarcs")) {
        // Touching from outside, or one inside the other: whichever they're nearer now.
        const auto a = g.point(g.arcs.at(first).c), b = g.point(g.arcs.at(second).c);
        const double d = std::hypot(b.u - a.u, b.v - a.v), r1 = g.radius(first), r2 = g.radius(second);
        c.u = std::abs(d - (r1 + r2)) <= std::abs(d - std::abs(r1 - r2)) ? 1 : 0;
        summary = QStringLiteral("Made %1 tangent to %2").arg(label(0), label(1));
    } else if (type == QLatin1String("concentric")) {
        summary = QStringLiteral("Made %1 and %2 concentric").arg(label(0), label(1));
    } else if (type == QLatin1String("equalradius")) {
        summary = QStringLiteral("Made %1 and %2 equal").arg(label(0), label(1));
    } else if (type == QLatin1String("pointonarc")) {
        if (arcPoints(second).count(first) && g.arcs.at(second).c != first)
            return QStringLiteral("That point is already on it (it's one of its ends).");
        if (g.arcs.at(second).c == first)
            return QStringLiteral("A centre can't be on its own curve.");
        summary = QStringLiteral("Put %1 on %2").arg(label(0), label(1));
    } else {
        return QStringLiteral("There's no such constraint.");
    }

    const std::map<int, SketchGeometry::Point> before = g.points;
    const int id = g.nextConstraint++;
    g.constraints[id] = c;
    const std::vector<int> touched = g.pointsOf(c);
    // The first thing picked stays put where it can (Fusion moves the second to the first).
    std::set<int> steady;
    const int kept = keep == 1 && made.size() > 1 ? 1 : 0;
    if (made.size() > 1)
        steady = made[kept] == QLatin1Char('P') ? std::set<int>{ids[kept]}
               : made[kept] == QLatin1Char('A') ? arcPoints(ids[kept]) : endsOf(ids[kept]);
    if (made.size() == 3)
        for (int p : endsOf(third))
            steady.insert(p);              // and the mirror line stays where it is
    // Tangent: the circle or arc that isn't kept slides over whole until it touches, keeping its
    // size, so it can then be dragged round the other (owner, 2026-10-30: the solve alone grew
    // it and swung it level with the first). Not when one of its points is fixed in place.
    const bool slides = type == QLatin1String("tangentarcs") || (type == QLatin1String("tangent") && kept == 0);
    const int moving = type == QLatin1String("tangent") ? second : ids[1 - kept];
    bool pinned = false;
    for (const auto &[cid, k] : g.constraints)
        if (slides && k.type == QLatin1String("fix") && arcPoints(moving).count(k.first))
            pinned = true;
    if (slides && !pinned)
        g.touch(c, moving);
    bool holds = g.solveNear({touched.begin(), touched.end()}, steady);
    if (!holds && type == QLatin1String("coincident")) {
        g.constraints.erase(id);
        const bool slid = g.slideTogether(ids[1 - kept], ids[kept]) || g.slideTogether(ids[kept], ids[1 - kept]);
        g.constraints[id] = c;
        holds = slid && g.solveNear({touched.begin(), touched.end()}, steady);
    }
    if (!holds) {
        g.constraints.erase(id);
        g.nextConstraint--;
        g.points = before;
        return QStringLiteral("That can't hold with the constraints already there.");
    }
    record({{QStringLiteral("type"), QStringLiteral("constraint")}, {QStringLiteral("sketch"), sketch},
            {QStringLiteral("id"), id}, {QStringLiteral("constraint"), type},
            {QStringLiteral("first"), first}, {QStringLiteral("second"), second}, {QStringLiteral("third"), third},
            {QStringLiteral("moved"), movedSince(g, before)}},
           summary);
    sketchesEdited();
    return {};
}

namespace {

// Where the points that moved since `before` were then (for Ctrl+Z).
QJsonArray movedSince(const SketchGeometry &g, const std::map<int, SketchGeometry::Point> &before) {
    QJsonArray moved;
    for (const auto &[pid, p] : before) {
        const auto now = g.points.find(pid);
        if (now != g.points.end() && (std::abs(now->second.u - p.u) > 1e-12 || std::abs(now->second.v - p.v) > 1e-12))
            moved.append(QJsonObject{{QStringLiteral("id"), pid}, {QStringLiteral("u"), p.u}, {QStringLiteral("v"), p.v}});
    }
    return moved;
}

// An angle dimension's corner, in sketch mm, for drawing its arc (owner, 2026-10-30): where
// the two lines cross (or would), each line's direction (start to end) and both ends of each.
// "parallel" when they don't cross; then "cross" is between their middles. "axis1"/"axis2": that
// one is an axis (endless, so it never needs a line drawn out to the arc).
QVariantMap angleFrame(const SketchGeometry &g, int first, int second) {
    const SketchGeometry::Point a = g.point(g.line(first).a), b = g.point(g.line(first).b);
    const SketchGeometry::Point c = g.point(g.line(second).a), d = g.point(g.line(second).b);
    const double l1 = std::hypot(b.u - a.u, b.v - a.v), l2 = std::hypot(d.u - c.u, d.v - c.v);
    if (l1 < 1e-9 || l2 < 1e-9)
        return {};
    const double x1 = (b.u - a.u) / l1, y1 = (b.v - a.v) / l1, x2 = (d.u - c.u) / l2, y2 = (d.v - c.v) / l2;
    const double den = x1 * y2 - y1 * x2;
    double xu = (a.u + b.u + c.u + d.u) / 4, xv = (a.v + b.v + c.v + d.v) / 4;
    const bool parallel = std::abs(den) < 1e-6;
    if (!parallel) {
        const double t = ((c.u - a.u) * y2 - (c.v - a.v) * x2) / den;
        xu = a.u + x1 * t;
        xv = a.v + y1 * t;
    }
    return {{QStringLiteral("parallel"), parallel}, {QStringLiteral("cross"), QVariantList{xu, xv}},
            {QStringLiteral("e1"), QVariantList{x1, y1}}, {QStringLiteral("e2"), QVariantList{x2, y2}},
            {QStringLiteral("ends1"), QVariantList{a.u, a.v, b.u, b.v}},
            {QStringLiteral("ends2"), QVariantList{c.u, c.v, d.u, d.v}},
            {QStringLiteral("axis1"), first < 0}, {QStringLiteral("axis2"), second < 0}};
}

} // namespace

QVariantMap Document::sketchMeasure(int sketch, const QString &type, int first, int second, bool otherSide) const {
    const auto found = m_sketches.find(sketch);
    if (found == m_sketches.end())
        return {};
    double value = 0;
    SketchGeometry::Point a, b;
    if (!found->second.geometry.measure(type, first, second, value, a, b))
        return {};
    if (type == QLatin1String("lineline") && !found->second.geometry.parallel(first, second))
        return {{QStringLiteral("why"), notParallel()}};
    if (type == QLatin1String("angle") && otherSide)
        value = M_PI - value;
    const Named &s = found->second;
    QVariantMap out{{QStringLiteral("value"), value},
                    {QStringLiteral("a"), onSketch(s, a.u, a.v)}, {QStringLiteral("b"), onSketch(s, b.u, b.v)}};
    if (type == QLatin1String("angle"))
        out.insert(QStringLiteral("frame"), angleFrame(s.geometry, first, second));
    return out;
}

QVariantList Document::sketchDimensions(int sketch) const {
    QVariantList out;
    const auto found = m_sketches.find(sketch);
    if (found == m_sketches.end())
        return out;
    const Named &s = found->second;
    for (const auto &[id, c] : s.geometry.constraints) {
        double now = 0;
        SketchGeometry::Point a, b;
        if (!c.isDimension() || c.follows() || !s.geometry.measure(c.type, c.first, c.second, now, a, b))
            continue;
        // A gap between lines is shown a quarter of the way along them, not at the middle, so
        // the two gaps of a rectangle don't sit on top of each other in its centre.
        if (c.type == QLatin1String("lineline")) {
            const auto l = s.geometry.line(c.second);
            const SketchGeometry::Point p = s.geometry.point(l.a), q = s.geometry.point(l.b);
            const double du = (p.u - q.u) / 4, dv = (p.v - q.v) / 4;
            a = {a.u + du, a.v + dv};
            b = {b.u + du, b.v + dv};
        }
        QVariantMap d{{QStringLiteral("id"), id}, {QStringLiteral("type"), c.type},
                           {QStringLiteral("value"), c.value}, {QStringLiteral("unit"), c.unit},
                           {QStringLiteral("a"), onSketch(s, a.u, a.v)}, {QStringLiteral("b"), onSketch(s, b.u, b.v)},
                           {QStringLiteral("mid_u"), (a.u + b.u) / 2}, {QStringLiteral("mid_v"), (a.v + b.v) / 2},
                           {QStringLiteral("placed"), c.labelPlaced},
                           {QStringLiteral("label_u"), c.labelU}, {QStringLiteral("label_v"), c.labelV}};
        // An angle: its corner, and which side of it the value means (u -1: the first line
        // against the second taken end to start). Its label_u/v are along the two lines from
        // the corner, so it keeps to its side of the corner as the lines turn.
        if (c.type == QLatin1String("angle")) {
            d.insert(QStringLiteral("frame"), angleFrame(s.geometry, c.first, c.second));
            d.insert(QStringLiteral("side"), c.u < 0 ? -1 : 1);
        }
        out << d;
    }
    return out;
}

// placed false: an angle's side of its corner only (where it was made), not a place.
void Document::placeDimensionLabel(int sketch, int constraint, double u, double v, bool placed) {
    const auto found = m_sketches.find(sketch);
    if (found == m_sketches.end())
        return;
    const auto c = found->second.geometry.constraints.find(constraint);
    if (c == found->second.geometry.constraints.end() || !c->second.isDimension())
        return;
    c->second.labelPlaced = placed;
    c->second.labelU = u;
    c->second.labelV = v;
    sketchesEdited();
}

QString Document::addSketchDimension(int sketch, const QString &type, int first, int second, double value,
                                     const QString &unit, const QString &label, bool otherSide) {
    const auto found = m_sketches.find(sketch);
    if (found == m_sketches.end())
        return QStringLiteral("No sketch is open.");
    SketchGeometry &g = found->second.geometry;
    if (wouldTieBoth(g, type, first, second))
        return kTiedToBoth;
    double now = 0;
    SketchGeometry::Point a, b;
    if (!g.measure(type, first, second, now, a, b))
        return QStringLiteral("Pick a line, or two points, or a point and a line, or two parallel lines.");
    if (type == QLatin1String("lineline") && !g.parallel(first, second))
        return notParallel();
    if (!(value > 1e-6))
        return QStringLiteral("A dimension has to be more than zero.");
    for (const auto &[id, c] : g.constraints)
        if (c.type == type && ((c.first == first && c.second == second) || (c.first == second && c.second == first)))
            return QStringLiteral("That's already dimensioned: double-click its value to change it.");

    const std::map<int, SketchGeometry::Point> before = g.points;
    const int id = g.nextConstraint++;
    SketchGeometry::Constraint c{type, first, second};
    c.value = value;
    c.unit = unit;
    if (type == QLatin1String("angle") && otherSide)
        c.u = -1;
    g.constraints[id] = c;
    const std::vector<int> touched = g.pointsOf(c);
    if (!g.solveNear({touched.begin(), touched.end()})) {
        g.constraints.erase(id);
        g.nextConstraint--;
        g.points = before;
        return QStringLiteral("It can't be %1 with the constraints already there.").arg(label);
    }
    const QString made = SketchGeometry::kinds(type);
    const QString what = made.size() == 1 ? sketchPartName(found->second, made[0], first)
                                          : QStringLiteral("%1 to %2").arg(sketchPartName(found->second, made[0], first),
                                                                          sketchPartName(found->second, made[1], second));
    record({{QStringLiteral("type"), QStringLiteral("constraint")}, {QStringLiteral("sketch"), sketch},
            {QStringLiteral("id"), id}, {QStringLiteral("constraint"), type},
            {QStringLiteral("first"), first}, {QStringLiteral("second"), second},
            {QStringLiteral("moved"), movedSince(g, before)}},
           QStringLiteral("Dimensioned %1: %2").arg(what, label));
    sketchesEdited();
    return {};
}

QString Document::setSketchDimension(int sketch, int constraint, double value, const QString &unit, const QString &label) {
    const auto found = m_sketches.find(sketch);
    if (found == m_sketches.end())
        return QStringLiteral("No sketch is open.");
    SketchGeometry &g = found->second.geometry;
    const auto c = g.constraints.find(constraint);
    if (c == g.constraints.end() || !c->second.isDimension())
        return QStringLiteral("That dimension is gone.");
    if (!(value > 1e-6))
        return QStringLiteral("A dimension has to be more than zero.");
    const std::map<int, SketchGeometry::Point> before = g.points;
    const double oldValue = c->second.value;
    const QString oldUnit = c->second.unit;
    c->second.value = value;
    c->second.unit = unit;
    const std::vector<int> touched = g.pointsOf(c->second);
    if (!g.solveNear({touched.begin(), touched.end()})) {
        c->second.value = oldValue;
        c->second.unit = oldUnit;
        g.points = before;
        return QStringLiteral("It can't be %1 with the constraints already there.").arg(label);
    }
    record({{QStringLiteral("type"), QStringLiteral("dimension_value")}, {QStringLiteral("sketch"), sketch},
            {QStringLiteral("id"), constraint}, {QStringLiteral("value"), oldValue}, {QStringLiteral("unit"), oldUnit},
            {QStringLiteral("moved"), movedSince(g, before)}},
           QStringLiteral("Changed a dimension to %1").arg(label));
    sketchesEdited();
    return {};
}

void Document::beginSketchDrag(int sketch) {
    const auto found = m_sketches.find(sketch);
    m_dragBefore = found == m_sketches.end() ? std::map<int, SketchGeometry::Point>{} : found->second.geometry.points;
}

bool Document::dragSketch(int sketch, const QVariantList &moves) {
    const auto found = m_sketches.find(sketch);
    if (found == m_sketches.end())
        return false;
    std::map<int, SketchGeometry::Point> targets;
    for (const QVariant &v : moves) {
        const QVariantMap m = v.toMap();
        targets[m.value(QStringLiteral("id")).toInt()] = {m.value(QStringLiteral("u")).toDouble(),
                                                          m.value(QStringLiteral("v")).toDouble()};
    }
    if (!found->second.geometry.drag(targets))
        return false;
    sketchesEdited();
    return true;
}

bool Document::dragSketchSize(int sketch, int arc, double radius) {
    const auto found = m_sketches.find(sketch);
    if (found == m_sketches.end() || !found->second.geometry.resize(arc, radius))
        return false;
    sketchesEdited();
    return true;
}

QString Document::endSketchDrag(int sketch, const QString &summary, const QVariantMap &hold) {
    const auto found = m_sketches.find(sketch);
    if (found == m_sketches.end())
        return {};
    SketchGeometry &g = found->second.geometry;
    QJsonArray added;
    QString made;
    const int point = hold.value(QStringLiteral("point")).toInt();
    const QString on = hold.value(QStringLiteral("on")).toString();
    const int target = hold.value(QStringLiteral("target")).toInt();
    SketchGeometry::Constraint c;
    if (!g.points.count(point))
        ;
    else if (on == QLatin1String("point") && g.hasPoint(target) && target != point)
        c = {QStringLiteral("coincident"), target, point};
    else if (on == QLatin1String("origin"))
        c = {QStringLiteral("coincident"), SketchGeometry::Origin, point};
    else if (on == QLatin1String("midpoint") && g.lines.count(target))
        c = {QStringLiteral("midpoint"), point, target};
    else if (on == QLatin1String("line") && g.hasLine(target))
        c = {QStringLiteral("pointonline"), point, target};
    else if (on == QLatin1String("arc") && g.arcs.count(target))
        c = {QStringLiteral("pointonarc"), point, target};
    const bool twin = !c.type.isEmpty() && std::any_of(g.constraints.begin(), g.constraints.end(), [&](const auto &k) {
        const auto &o = k.second;
        return o.type == c.type && ((o.first == c.first && o.second == c.second)
                                    || (c.type == QLatin1String("coincident") && o.first == c.second && o.second == c.first));
    });
    if (!c.type.isEmpty() && !twin) {
        const int id = g.nextConstraint++;
        g.constraints[id] = c;
        // The drop put it there already; if the constraints held it a hair off, settle it,
        // moving as little as possible (the snapped-to thing stays put).
        const std::vector<int> mine = g.pointsOf(c);
        std::set<int> others;
        for (int id : mine)
            if (id >= 0 && id != point)
                others.insert(id);
        if (g.error() > 1e-6 && !g.solveNear({point}, others)) {
            g.constraints.erase(id);
            --g.nextConstraint;
        } else {
            added.append(id);
            made = on == QLatin1String("midpoint") ? QStringLiteral("Midpoint") : QStringLiteral("Coincident");
        }
    }
    QJsonArray moved;
    for (const auto &[pid, p] : m_dragBefore) {
        const auto now = found->second.geometry.points.find(pid);
        if (now != found->second.geometry.points.end()
            && (std::abs(now->second.u - p.u) > 1e-9 || std::abs(now->second.v - p.v) > 1e-9))
            moved.append(QJsonObject{{QStringLiteral("id"), pid}, {QStringLiteral("u"), p.u}, {QStringLiteral("v"), p.v}});
    }
    m_dragBefore.clear();
    if (moved.isEmpty() && added.isEmpty())
        return {};
    record({{QStringLiteral("type"), QStringLiteral("drag")}, {QStringLiteral("sketch"), sketch},
            {QStringLiteral("moved"), moved},    // where those points were, for Ctrl+Z
            {QStringLiteral("added"), added}},   // the constraint the drop made, if any
           made.isEmpty() ? summary : summary + QStringLiteral(" (") + made + QStringLiteral(")"));
    if (!added.isEmpty())
        sketchesEdited();
    return made;
}

QString Document::fixSketchPoints(int sketch, const QVariantList &pointIds) {
    const auto found = m_sketches.find(sketch);
    if (found == m_sketches.end())
        return {};
    SketchGeometry &g = found->second.geometry;
    std::set<int> ids;
    for (const QVariant &v : pointIds)
        if (g.points.count(v.toInt()))
            ids.insert(v.toInt());
    for (const int id : ids)
        if (g.projectedPoints.count(id))
            return QStringLiteral("Projected geometry is already held by the model.");
    if (ids.empty())
        return {};
    const bool letGo = std::all_of(ids.begin(), ids.end(), [&](int id) { return g.isFixed(id); });
    QJsonArray added, removed;
    if (letGo) {
        for (auto it = g.constraints.begin(); it != g.constraints.end();) {
            const auto &c = it->second;
            if (c.type == QLatin1String("fix") && ids.count(c.first)) {
                removed.append(QJsonObject{{QStringLiteral("id"), it->first}, {QStringLiteral("point"), c.first},
                                           {QStringLiteral("u"), c.u}, {QStringLiteral("v"), c.v}});
                it = g.constraints.erase(it);
            } else {
                ++it;
            }
        }
    } else {
        for (int id : ids) {
            if (g.isFixed(id))
                continue;
            const int cid = g.nextConstraint++;
            g.constraints[cid] = {QStringLiteral("fix"), id, 0, g.points[id].u, g.points[id].v};
            added.append(cid);
        }
    }
    const QString what = ids.size() == 1 ? QStringLiteral("Point%1").arg(*ids.begin())
                                         : QStringLiteral("%1 points").arg(ids.size());
    const QString summary = (letGo ? QStringLiteral("Unfixed ") : QStringLiteral("Fixed ")) + what;
    record({{QStringLiteral("type"), QStringLiteral("fix")}, {QStringLiteral("sketch"), sketch},
            {QStringLiteral("added"), added}, {QStringLiteral("removed"), removed}},
           summary);
    sketchesEdited();
    return summary;
}

QString Document::toggleSketchConstruction(int sketch, const QVariantList &parts) {
    const auto found = m_sketches.find(sketch);
    if (found == m_sketches.end())
        return {};
    SketchGeometry &g = found->second.geometry;
    struct Part { std::set<int> *marks; int id; QString name; };
    std::vector<Part> picked;
    for (const QVariant &v : parts) {
        const QVariantMap m = v.toMap();
        const QString kind = m.value(QStringLiteral("kind")).toString();
        const int id = m.value(QStringLiteral("index")).toInt();
        if (kind == QLatin1String("sketch_line") && g.lines.count(id) && !g.helperLines.count(id))
            picked.push_back({&g.constructionLines, id, QStringLiteral("Line%1").arg(id)});
        else if (kind == QLatin1String("sketch_arc") && g.arcs.count(id) && !g.helperArcs.count(id))
            picked.push_back({&g.constructionArcs, id, (g.arcs.at(id).circle() ? QStringLiteral("Circle%1") : QStringLiteral("Arc%1")).arg(id)});
        else if (kind == QLatin1String("sketch_curve") && g.curves.count(id))
            picked.push_back({&g.constructionCurves, id, QStringLiteral("Curve%1").arg(id)});
    }
    if (picked.empty())
        return {};
    const bool on = std::any_of(picked.begin(), picked.end(), [](const Part &p) { return !p.marks->count(p.id); });
    const int snapshot = m_nextSnapshot++;
    m_snapshots[snapshot] = g;
    for (const Part &p : picked) {
        if (on)
            p.marks->insert(p.id);
        else
            p.marks->erase(p.id);
    }
    const QString what = picked.size() == 1 ? picked[0].name : QStringLiteral("%1 pieces").arg(picked.size());
    const QString summary = on ? QStringLiteral("Made %1 construction").arg(what) : QStringLiteral("Made %1 normal").arg(what);
    record({{QStringLiteral("type"), QStringLiteral("construction")}, {QStringLiteral("sketch"), sketch},
            {QStringLiteral("snapshot"), snapshot}},
           summary);
    sketchesEdited();
    return summary;
}

int Document::deleteSketchConstraints(int sketch, const QVariantList &ids, const QString &summary) {
    const auto found = m_sketches.find(sketch);
    if (found == m_sketches.end())
        return 0;
    SketchGeometry &g = found->second.geometry;
    QJsonArray removed;   // each one whole, so Ctrl+Z puts back exactly what was there
    for (const QVariant &v : ids) {
        const auto c = g.constraints.find(v.toInt());
        if (c == g.constraints.end())
            continue;
        const SketchGeometry::Constraint &k = c->second;
        removed.append(QJsonObject{{QStringLiteral("id"), c->first}, {QStringLiteral("type"), k.type},
                                   {QStringLiteral("first"), k.first}, {QStringLiteral("second"), k.second},
                                   {QStringLiteral("third"), k.third}, {QStringLiteral("u"), k.u},
                                   {QStringLiteral("v"), k.v}, {QStringLiteral("value"), k.value},
                                   {QStringLiteral("unit"), k.unit}});
        g.constraints.erase(c);
    }
    if (removed.isEmpty())
        return 0;
    record({{QStringLiteral("type"), QStringLiteral("delete_constraints")}, {QStringLiteral("sketch"), sketch},
            {QStringLiteral("removed"), removed}},
           summary);
    sketchesEdited();
    return int(removed.size());
}

// Show Points and Show Projected Geometries: what's switched off is neither drawn nor picked.
void Document::setSketchShows(bool points, bool projected) {
    m_showPoints = points;
    m_showProjected = projected;
}

bool Document::lineHidden(const SketchGeometry &g, int a, int b) const {
    return !m_showProjected && g.projectedPoints.count(a) && g.projectedPoints.count(b);
}

bool Document::pointHidden(const SketchGeometry &g, int id) const {
    return g.isHidden(id) || !m_showPoints || (!m_showProjected && g.projectedPoints.count(id));
}

// Everything pickable in a sketch as paths in space, for a box drag to test on screen.
QVariantList Document::sketchOutlines(int sketch) const {
    QVariantList out;
    const auto found = m_sketches.find(sketch);
    if (found == m_sketches.end())
        return out;
    const Named &s = found->second;
    const SketchGeometry &g = s.geometry;
    const auto add = [&](const char *kind, int id, const std::vector<std::vector<SketchGeometry::Point>> &paths) {
        QVariantList runs;
        for (const auto &path : paths) {
            QVariantList run;
            for (const auto &p : path)
                run << onSketch(s, p.u, p.v);
            runs << QVariant(run);
        }
        out << QVariantMap{{QStringLiteral("kind"), QString::fromLatin1(kind)}, {QStringLiteral("index"), id}, {QStringLiteral("paths"), runs}};
    };
    for (const auto &[id, p] : g.points)
        if (!pointHidden(g, id))
            add("sketch_point", id, {{p}});
    for (const auto &[id, l] : g.lines)
        if (!g.helperLines.count(id) && !lineHidden(g, l.a, l.b))
            add("sketch_line", id, {{g.points.at(l.a), g.points.at(l.b)}});
    for (const auto &[id, k] : g.arcs)
        if (!g.helperArcs.count(id) && !lineHidden(g, k.c, k.c))
            add("sketch_arc", id, g.arcPath(id));
    for (const auto &[id, k] : g.curves)
        add("sketch_curve", id, g.curvePath(id));
    return out;
}

QVariantMap Document::pickSketch(int sketch, QVector3D origin, QVector3D direction, float pointReach, float edgeReach) const {
    QVariantMap none{{QStringLiteral("kind"), QString()}};
    const auto found = m_sketches.find(sketch);
    if (found == m_sketches.end())
        return none;
    const Named &s = found->second;
    const SketchGeometry &g = s.geometry;
    const QVector3D d = direction.normalized();
    auto hit = [&](const char *kind, int index, float t) {
        return QVariantMap{{QStringLiteral("kind"), QString::fromLatin1(kind)}, {QStringLiteral("sketch"), sketch},
                           {QStringLiteral("index"), index}, {QStringLiteral("depth"), t}};
    };
    int best = 0;
    float bestScore = INFINITY, bestT = 0;
    for (const auto &[id, p] : g.points) {
        if (pointHidden(g, id))
            continue;                     // a circle's rim point or a helper isn't a point you can see
        const QVector3D at = onSketch(s, p.u, p.v);
        const float t = QVector3D::dotProduct(at - origin, d);
        const float reach = std::max(t, 1.0f) * pointReach;
        const float gap = (origin + d * t - at).length();
        if (t > 0 && gap <= reach && gap / reach < bestScore) {
            best = id;
            bestScore = gap / reach;
            bestT = t;
        }
    }
    if (best)
        return hit("sketch_point", best, bestT);
    for (const auto &[id, l] : g.lines) {
        if (g.helperLines.count(id) || lineHidden(g, l.a, l.b))
            continue;
        const auto [t, gap] = raySegment(origin, d, onSketch(s, g.points.at(l.a).u, g.points.at(l.a).v),
                                         onSketch(s, g.points.at(l.b).u, g.points.at(l.b).v));
        const float reach = std::max(t, 1.0f) * edgeReach;
        if (gap <= reach && gap / reach < bestScore) {
            best = id;
            bestScore = gap / reach;
            bestT = t;
        }
    }
    // Arcs and the other curves, piece by piece along them.
    const char *bestKind = "sketch_line";
    auto along = [&](const std::vector<std::vector<SketchGeometry::Point>> &paths, const char *kind, int id) {
        for (const auto &path : paths)
            for (std::size_t i = 0; i + 1 < path.size(); ++i) {
                const auto [t, gap] = raySegment(origin, d, onSketch(s, path[i].u, path[i].v),
                                                 onSketch(s, path[i + 1].u, path[i + 1].v));
                const float reach = std::max(t, 1.0f) * edgeReach;
                if (gap <= reach && gap / reach < bestScore) {
                    best = id;
                    bestKind = kind;
                    bestScore = gap / reach;
                    bestT = t;
                }
            }
    };
    for (const auto &[id, k] : g.arcs)
        if (!g.helperArcs.count(id) && !lineHidden(g, k.c, k.c))
            along(g.arcPath(id), "sketch_arc", id);
    for (const auto &[id, k] : g.curves)
        along(g.curvePath(id), "sketch_curve", id);
    return best ? hit(bestKind, best, bestT) : none;
}

namespace {

omamake::Mesh emptyMesh() {
    omamake::Mesh out;
    for (int i = 0; i < 3; ++i) {
        out.min[i] = std::numeric_limits<float>::max();
        out.max[i] = std::numeric_limits<float>::lowest();
    }
    return out;
}

void finish(omamake::Mesh &out) {
    if (out.positions.empty())
        for (int i = 0; i < 3; ++i)
            out.min[i] = out.max[i] = 0;
}

} // namespace

void Document::fillSketches(MeshGeometry *target, MeshGeometry *fullTarget, MeshGeometry *construction,
                            MeshGeometry *constructionFull, MeshGeometry *projected, MeshGeometry *projectedDashed,
                            const QVariantList &sketches, float radius) const {
    if (!target || !fullTarget || !construction || !constructionFull || !projected || !projectedDashed)
        return;
    omamake::Mesh free = emptyMesh(), full = emptyMesh(), dashed = emptyMesh(), dashedFull = emptyMesh(), fromModel = emptyMesh(),
                  fromModelDashed = emptyMesh();
    for (const QVariant &v : sketches) {
        const auto found = m_sketches.find(v.toInt());
        if (found == m_sketches.end())
            continue;
        const Named &s = found->second;
        const std::set<int> held = s.geometry.fullyConstrained();
        const SketchGeometry &g = s.geometry;
        const auto solidIn = [&](bool isProjected, bool fully) -> omamake::Mesh & {
            return isProjected ? fromModel : fully ? full : free;
        };
        const auto dashedIn = [&](bool isProjected, bool fully) -> omamake::Mesh & {
            return isProjected ? fromModelDashed : fully ? dashedFull : dashed;
        };
        for (const auto &[id, l] : g.lines) {
            if (g.helperLines.count(id) || lineHidden(g, l.a, l.b))
                continue;
            const auto &a = g.points.at(l.a), &b = g.points.at(l.b);
            const bool fully = held.count(l.a) && held.count(l.b);
            const bool isProjected = g.projectedPoints.count(l.a) && g.projectedPoints.count(l.b);
            if (g.constructionLines.count(id))
                addDashed(dashedIn(isProjected, fully), {onSketch(s, a.u, a.v), onSketch(s, b.u, b.v)}, radius);
            else
                addTube(solidIn(isProjected, fully),
                        onSketch(s, a.u, a.v), onSketch(s, b.u, b.v), radius);
        }
        auto tubes = [&](const std::vector<std::vector<SketchGeometry::Point>> &paths, bool fully, bool isConstruction,
                         bool isProjected = false) {
            for (const auto &path : paths) {
                std::vector<QVector3D> at;
                for (const auto &p : path)
                    at.push_back(onSketch(s, p.u, p.v));
                if (isConstruction)
                    addDashed(dashedIn(isProjected, fully), at, radius);
                else
                    for (std::size_t i = 0; i + 1 < at.size(); ++i)
                        addTube(solidIn(isProjected, fully), at[i], at[i + 1], radius);
            }
        };
        for (const auto &[id, k] : g.arcs)
            if (!g.helperArcs.count(id) && !lineHidden(g, k.c, k.c))
                tubes(g.arcPath(id), held.count(k.c) && held.count(k.a) && held.count(k.b), g.constructionArcs.count(id),
                      g.projectedPoints.count(k.c));
        for (const auto &[id, k] : g.curves)
            tubes(g.curvePath(id), std::all_of(k.points.begin(), k.points.end(), [&](int p) { return held.count(p); }),
                  g.constructionCurves.count(id));
        for (const auto &[id, p] : s.geometry.points)
            if (!pointHidden(g, id))
                addBall(solidIn(g.projectedPoints.count(id), held.count(id)), onSketch(s, p.u, p.v), radius * 2.2f);
    }
    finish(free);
    finish(full);
    finish(dashed);
    finish(dashedFull);
    finish(fromModel);
    finish(fromModelDashed);
    projected->setMesh(fromModel);
    projectedDashed->setMesh(fromModelDashed);
    target->setMesh(free);
    fullTarget->setMesh(full);
    construction->setMesh(dashed);
    constructionFull->setMesh(dashedFull);
}

void Document::fillProfiles(MeshGeometry *target, const QVariantList &sketches) const {
    if (!target)
        return;
    omamake::Mesh out = emptyMesh();
    for (const QVariant &v : sketches) {
        const auto found = m_sketches.find(v.toInt());
        if (found == m_sketches.end())
            continue;
        const Named &s = found->second;
        for (const profiles::Profile &p : profilesOf(v.toInt(), s))
            addProfile(out, s, p);
    }
    finish(out);
    target->setMesh(out);
}

// QML's handlers run before any C++ connection to a signal, so the old profiles go first: a
// connection clearing them would leave the shading drawn from the old ones.
void Document::sketchesEdited() {
    m_profiles.clear();
    emit sketchesChanged();
}

const std::vector<profiles::Profile> &Document::profilesOf(int id, const Named &sketch) const {
    const auto known = m_profiles.find(id);
    if (known != m_profiles.end())
        return known->second;
    const auto [paths, groups] = profilePaths(sketch.geometry);
    return m_profiles[id] = profiles::find(paths, 0, 1e-4, groups).profiles;
}

void Document::addProfile(omamake::Mesh &out, const Named &s, const profiles::Profile &p) const {
    const QVector3D normal = QVector3D::crossProduct(s.plane.value(QStringLiteral("xAxis")).value<QVector3D>(),
                                                     s.plane.value(QStringLiteral("yAxis")).value<QVector3D>()).normalized();
    std::vector<QVector3D> at;
    for (const auto &q : p.outer)
        at.push_back(onSketch(s, q.u, q.v));
    for (const auto &h : p.holes)
        for (const auto &q : h)
            at.push_back(onSketch(s, q.u, q.v));
    const std::vector<int> t = profiles::triangles(p);
    for (std::size_t i = 0; i + 2 < t.size(); i += 3)
        addTriangle(out, at[t[i]], at[t[i + 1]], at[t[i + 2]], normal, normal, normal);
}

QVariantMap Document::pickProfile(const QVariantList &sketches, QVector3D origin, QVector3D direction) const {
    QVariantMap best{{QStringLiteral("kind"), QString()}};
    const QVector3D d = direction.normalized();
    float bestT = INFINITY;
    for (const QVariant &v : sketches) {
        const int id = v.toInt();
        const auto found = m_sketches.find(id);
        if (found == m_sketches.end())
            continue;
        const Named &s = found->second;
        const QVector3D o = s.plane.value(QStringLiteral("origin")).value<QVector3D>();
        const QVector3D x = s.plane.value(QStringLiteral("xAxis")).value<QVector3D>();
        const QVector3D y = s.plane.value(QStringLiteral("yAxis")).value<QVector3D>();
        const QVector3D n = QVector3D::crossProduct(x, y).normalized();
        const float facing = QVector3D::dotProduct(d, n);
        if (std::abs(facing) < 1e-6f)
            continue;                                   // looking along the plane
        const float t = QVector3D::dotProduct(o - origin, n) / facing;
        if (t <= 0 || t >= bestT)
            continue;
        const QVector3D at = origin + d * t - o;
        const profiles::P p{QVector3D::dotProduct(at, x) / x.lengthSquared(), QVector3D::dotProduct(at, y) / y.lengthSquared()};
        const auto &all = profilesOf(id, s);
        for (std::size_t i = 0; i < all.size(); ++i)
            if (profiles::contains(all[i], p)) {
                bestT = t;
                best = {{QStringLiteral("kind"), QStringLiteral("profile")}, {QStringLiteral("sketch"), id},
                        {QStringLiteral("index"), int(i)}, {QStringLiteral("u"), p.u}, {QStringLiteral("v"), p.v},
                        {QStringLiteral("depth"), t}};
                break;
            }
    }
    return best;
}

int Document::profileAt(int sketch, double u, double v) const {
    const auto found = m_sketches.find(sketch);
    if (found == m_sketches.end())
        return -1;
    const auto &all = profilesOf(sketch, found->second);
    for (std::size_t i = 0; i < all.size(); ++i)
        if (profiles::contains(all[i], {u, v}))
            return int(i);
    return -1;
}

void Document::fillProfileParts(MeshGeometry *target, const QVariantList &parts) const {
    if (!target)
        return;
    omamake::Mesh out = emptyMesh();
    for (const QVariant &v : parts) {
        const QVariantMap part = v.toMap();
        const int id = part.value(QStringLiteral("sketch")).toInt(), index = part.value(QStringLiteral("index")).toInt();
        const auto found = m_sketches.find(id);
        if (found == m_sketches.end())
            continue;
        const auto &all = profilesOf(id, found->second);
        if (index >= 0 && std::size_t(index) < all.size())
            addProfile(out, found->second, all[index]);
    }
    finish(out);
    target->setMesh(out);
}

void Document::fillSegments(MeshGeometry *target, const QVariantList &ends, float radius, bool dashed) const {
    if (!target)
        return;
    omamake::Mesh out = emptyMesh();
    // Dashed: segments that carry on from the one before are one path, so a curve's preview
    // keeps its pattern instead of starting it again at every short piece.
    std::vector<QVector3D> path;
    for (qsizetype i = 0; i + 1 < ends.size(); i += 2) {
        const QVector3D a = ends[i].value<QVector3D>(), b = ends[i + 1].value<QVector3D>();
        if (!dashed) {
            addTube(out, a, b, radius);
            continue;
        }
        if (!path.empty() && (path.back() - a).length() > radius * 0.01f) {
            addDashed(out, path, radius);
            path.clear();
        }
        if (path.empty())
            path.push_back(a);
        path.push_back(b);
    }
    addDashed(out, path, radius);
    finish(out);
    target->setMesh(out);
}
