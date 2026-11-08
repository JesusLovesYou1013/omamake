#include <QQuickWindow>
#include <QKeyEvent>
#include <qpa/qwindowsysteminterface.h>
#include <QMouseEvent>
#include <QCoreApplication>
#include "document/document.h"

#include "view/meshgeometry.h"
#include "document/profiles.h"
#include "document/projection.h"
#include "document/expression.h"
#include "document/parameterfile.h"
#include <QRegularExpression>
#include <QSignalBlocker>

#include <QDateTime>
#include <QFontDatabase>
#include <QElapsedTimer>
#include <QThread>
#include <QTimer>
#include <QPointer>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <unordered_set>
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
#include <map>
#include <array>
#include <set>

Document::Document(std::unique_ptr<omamake::GeometryEngine> engine, QObject *parent, bool background)
    : QObject(parent), m_engine(std::move(engine)) {
    m_background = background;
    connect(this, &Document::structureChanged, this, [this] { m_profiles.clear(); });
    if (background)
        return;
    m_buildTimer = new QTimer(this);
    m_buildTimer->setInterval(12);
    connect(m_buildTimer, &QTimer::timeout, this, &Document::loadSome);
    // The background walk: a few ms at a time while there's something to work out, so the window never waits.
    m_walkTimer = new QTimer(this);
    m_walkTimer->setInterval(12);
    connect(m_walkTimer, &QTimer::timeout, this, &Document::walkSome);
    connect(this, &Document::historyChanged, this, [this] { if (!m_walkTimer->isActive()) m_walkTimer->start(); });
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

// A sketch plane at full precision: its "exact" origin and axes when it has them (a plane on a
// body's face, since 2026-11-03), else its vectors, which are only floats (exact for the origin
// planes). An extrude up to a face reached 126.999997 for 127 from the floats, and the join left
// a line where it should have been one face (owner, 2026-11-03).
void planeAxes(const QVariantMap &plane, double o[3], double x[3], double y[3]) {
    const QVariantList exact = plane.value(QStringLiteral("exact")).toList();
    if (exact.size() == 9) {
        for (int k = 0; k < 3; ++k) {
            o[k] = exact[k].toDouble();
            x[k] = exact[3 + k].toDouble();
            y[k] = exact[6 + k].toDouble();
        }
        return;
    }
    const QVector3D fo = plane.value(QStringLiteral("origin")).value<QVector3D>();
    const QVector3D fx = plane.value(QStringLiteral("xAxis")).value<QVector3D>();
    const QVector3D fy = plane.value(QStringLiteral("yAxis")).value<QVector3D>();
    for (int k = 0; k < 3; ++k) {
        o[k] = fo[k];
        x[k] = fx[k];
        y[k] = fy[k];
    }
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

// A sketch's drawing as saved (also each shape tool's record of what it changed: sketchDiff). `s`: the sketch,
// for each point's place in the model too (null: left out).
QJsonObject Document::geometryJson(const SketchGeometry &g, const Named *s) const {
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
            o.insert(QStringLiteral("font"), k.font);
            o.insert(QStringLiteral("flags"), k.flags);
            if (k.flags & SketchGeometry::TextAnchored)
                o.insert(QStringLiteral("anchor"), k.anchor);
            if (!k.path.empty()) {
                QJsonArray along;
                for (int code : k.path)
                    along.append(code);
                o.insert(QStringLiteral("path"), along);
            }
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
        QJsonObject one{{QStringLiteral("id"), pid}, {QStringLiteral("u"), p.u}, {QStringLiteral("v"), p.v},
                                  {QStringLiteral("fixed"), g.isFixed(pid)}, {QStringLiteral("circle_rim"), g.isRim(pid)},
                                  {QStringLiteral("helper"), bool(g.helperPoints.count(pid))},
                                  {QStringLiteral("projected"), bool(g.projectedPoints.count(pid))},
                                  {QStringLiteral("fully_constrained"), bool(full.count(pid))}};
        if (s) {
            const QVector3D at = onSketch(*s, p.u, p.v);
            one.insert(QStringLiteral("xyz_mm"), QJsonArray{at.x(), at.y(), at.z()});
        }
        points.append(one);
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
        if (c.driven)
            o.insert(QStringLiteral("driven"), true);
        if (c.isDimension()) {
            double now = 0;
            SketchGeometry::Point a, b;
            g.measure(c.type, c.first, c.second, now, a, b);
            if (c.type == QLatin1String("angle") && c.u < 0)
                now = M_PI - now;
            o.insert(c.type == QLatin1String("angle") ? QStringLiteral("value_rad") : QStringLiteral("value_mm"), g.valueOf(c));
            o.insert(QStringLiteral("unit"), c.unit);
            if (!c.expr.isEmpty()) {
                o.insert(QStringLiteral("expr"), c.expr);
                if (c.exprFlip)
                    o.insert(QStringLiteral("expr_flip"), true);
            }
            o.insert(QStringLiteral("measured_mm"), now);   // what it really is (differs only if it didn't solve)
            if (c.labelPlaced)
                o.insert(c.type == QLatin1String("angle") ? QStringLiteral("label_along_lines_mm") : QStringLiteral("label_offset_mm"),
                         QJsonArray{c.labelU, c.labelV});
        }
        constraints.append(o);
    }
    return {{QStringLiteral("origin_uv"), QJsonArray{g.origin.u, g.origin.v}},
            {QStringLiteral("points"), points}, {QStringLiteral("lines"), lines}, {QStringLiteral("arcs"), arcs},
            {QStringLiteral("curves"), curves}, {QStringLiteral("constraints"), constraints},
            {QStringLiteral("links"), linksJson(g)},
            {QStringLiteral("unsolved_error"), g.error()},
            {QStringLiteral("next"), QJsonObject{{QStringLiteral("point"), g.nextPoint}, {QStringLiteral("line"), g.nextLine},
                                                 {QStringLiteral("arc"), g.nextArc}, {QStringLiteral("curve"), g.nextCurve},
                                                 {QStringLiteral("constraint"), g.nextConstraint}}}};
}

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
        if (id == int(m_body)) {   // (the whole drawn model's numbers, on the newest body)
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
        QJsonObject o = geometryJson(s.geometry, &s);
        o.insert(QStringLiteral("id"), id);
        o.insert(QStringLiteral("component"), s.component);
        o.insert(QStringLiteral("name"), s.name);
        o.insert(QStringLiteral("key"), QStringLiteral("s%1").arg(id));
        o.insert(QStringLiteral("shown"), !m_hidden.count(QStringLiteral("s%1").arg(id)));
        o.insert(QStringLiteral("open"), id == m_openSketch);
        o.insert(QStringLiteral("plane"), planeJson(s.plane));
        sketches.append(o);
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
                                                 {QStringLiteral("sketch"), m_nextSketch},
                                                 {QStringLiteral("body"), m_nextBody}}},
            {QStringLiteral("components"), components},
            {QStringLiteral("bodies"), bodies},
            {QStringLiteral("sketches"), sketches},
            {QStringLiteral("hidden"), hidden},
            {QStringLiteral("parameters"), [&] {
                 QJsonArray all;
                 for (const Parameter &p : m_params)
                     all.append(QJsonObject{{QStringLiteral("id"), p.id}, {QStringLiteral("name"), p.name},
                                            {QStringLiteral("unit"), p.unit}, {QStringLiteral("expr"), p.expr},
                                            {QStringLiteral("comment"), p.comment}});
                 return all;
             }()},
            {QStringLiteral("next_parameter"), m_nextParameter},
            {QStringLiteral("history"), [&] {          // a Project run still open is saved as done
                 QJsonArray all = m_actions;
                 for (int i = 0; i < all.size(); ++i) {
                     QJsonObject a = all[i].toObject();
                     if (a.contains(QStringLiteral("pending"))) {
                         a.remove(QStringLiteral("pending"));
                         all[i] = a;
                     }
                     // An import's solid, for the background rebuild of the Timeline, only when nothing else in
                     // the file holds it: the body unchanged since (it's in "solids") or the last import (body_file).
                     const auto own = m_importSolids.find(a.value(QStringLiteral("at")).toString());
                     if (own != m_importSolids.end() && !m_bodies.count(a.value(QStringLiteral("body")).toInt()) && own->second != m_bodyFile) {
                         a.insert(QStringLiteral("solid"), QString::fromLatin1(own->second.toBase64()));
                         all[i] = a;
                     }
                 }
                 return all;
             }()},
            {QStringLiteral("solids"), [&] {           // every body's exact solid (Heavy engine; Lite can't save)
                 QJsonArray all;
                 for (const auto &entry : m_bodies) {
                     const std::string bytes = m_engine->saveBody(omamake::BodyId(entry.first));
                     if (bytes.empty())
                         continue;
                     all.append(QJsonObject{{QStringLiteral("id"), entry.first},
                                            {QStringLiteral("file"), QString::fromLatin1(qCompress(QByteArray::fromRawData(bytes.data(), qsizetype(bytes.size()))).toBase64())}});
                 }
                 return all;
             }()},
            {QStringLiteral("body_file"), QString::fromLatin1(m_bodyFile.toBase64())},
            {QStringLiteral("step_file"), QString::fromLatin1(m_stepFile.toBase64())}};
}

QString Document::saveTo(const QString &path) {
    if (m_importing || m_working)
        return QStringLiteral("Wait for the import to finish.");
    // The Timeline's marker back (until the steps behind it are saved too, history/2026-11-07_timeline-rewind.md
    // phase 4): the whole design is saved, with the marker put back where it was afterwards.
    if (!m_future.empty()) {
        if (futureStale())
            return QStringLiteral("A step was made with the Timeline's marker back, and the steps after it can't be "
                                  "built again on top of it yet, so the design can't be saved whole. Ctrl+Z that step first.");
        const int here = int(m_actions.size());
        rewindTo(here + int(m_future.size()));
        const QString why = saveTo(path);
        rewindTo(here);
        return why;
    }
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
    if (m_importing || m_working)
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
    // Every body, joined into one solid for the file (STEP here holds a single shape).
    omamake::BodyId all = 0;
    for (const auto &entry : m_bodies) {
        const omamake::BodyId one = omamake::BodyId(entry.first);
        if (!all) {
            all = one;
            continue;
        }
        const omamake::BodyId joined = m_engine->combine(all, one, omamake::Combine::Join);
        if (all != m_body && !m_bodies.count(int(all)))
            m_engine->release(all);
        if (!joined) {
            why = "Couldn't join the bodies for the file.";
            return QString::fromStdString(why);
        }
        all = joined;
    }
    const bool ok = m_engine->exportStep(all, target.toStdString(), why);
    if (!m_bodies.count(int(all)))
        m_engine->release(all);
    if (!ok)
        return QString::fromStdString(why);
    return {};
}

void Document::importFile(const QString &path) {
    const QString file = path.startsWith(QLatin1String("file://")) ? QUrl(path).toLocalFile() : path;
    if (m_importing || m_working)
        return;
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
            m_importSolids[QString()] = packed;           // keyed by the record's time stamp just below
            record({{QStringLiteral("type"), meshFile ? QStringLiteral("import_mesh") : QStringLiteral("import_step")},
                    {QStringLiteral("body"), int(body)},
                    {QStringLiteral("name"), m_importName}},
                   QStringLiteral("Imported %1").arg(m_importName));
            m_importSolids[m_actions.last().toObject().value(QStringLiteral("at")).toString()] = m_importSolids[QString()];
            m_importSolids.erase(QString());
            m_meshCache[int(body)] = std::move(mesh);     // already made off the main thread
            refreshMesh();
            emit changed();
            emit importFinished({}, seconds, int(m_mesh.faceCount), int(m_mesh.indices.size() / 3));
        }, Qt::QueuedConnection);
    });
    connect(worker, &QThread::finished, worker, &QObject::deleteLater);
    worker->start();
}

void Document::fitSurfaces() {
    if (m_importing || m_working)
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
        QMetaObject::invokeMethod(this, [this, from, body, result, mesh = std::move(mesh), packed, seconds, why] {
            m_importing = false;
            emit importChanged();
            if (!body) {
                emit fitFinished(QString::fromStdString(why), seconds, 0, 0, 0, 0, 0);
                return;
            }
            m_beforeFit.push_back({m_body, m_bodyFile});
            swapBody(int(from), body);
            m_bodyFile = packed;
            record({{QStringLiteral("type"), QStringLiteral("fit_surfaces")}, {QStringLiteral("body"), int(body)}},
                   QStringLiteral("Fit Curved Surfaces"));
            // Its result, for the Timeline's background rebuild of an opened file: Fit works from the triangles the
            // mesh came in as, which a file doesn't keep, so the rebuild takes the fitted body as it was made.
            m_importSolids[m_actions.last().toObject().value(QStringLiteral("at")).toString()] = packed;
            m_meshCache[int(body)] = std::move(mesh);     // already made off the main thread
            refreshMesh();
            emit changed();
            emit fitFinished({}, seconds, result.cylinders, result.cones, result.spheres, result.trianglesLeft, result.worstMm);
        }, Qt::QueuedConnection);
    });
    connect(worker, &QThread::finished, worker, &QObject::deleteLater);
    worker->start();
}

// The same body in the tree, a different solid behind it. The old solid is left to the caller.
void Document::swapBody(int was, omamake::BodyId body) {
    const int now = int(body);
    m_bodies[now] = m_bodies[was];
    m_bodies.erase(was);
    for (Component &c : m_components)
        std::replace(c.bodies.begin(), c.bodies.end(), was, now);
    if (m_hidden.erase(QStringLiteral("b%1").arg(was)))
        m_hidden.insert(QStringLiteral("b%1").arg(now));
    m_body = body;
    // The old solid's mesh stays cached (owner, 2026-11-07): the Timeline's marker, undo and redo
    // show it again without meshing it again. It goes when the solid is released (releaseBody).
    emit structureChanged();
}

void Document::placeImport(omamake::BodyId body, const QString &name) {
    addBody(body, m_active, name);
    emit structureChanged();
}

void Document::addCube(double sizeMm) {
    buildCube(sizeMm);
    record({{QStringLiteral("type"), QStringLiteral("cube")}, {QStringLiteral("size_mm"), sizeMm},
            {QStringLiteral("body"), int(m_body)},
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

// What a step changed in a sketch, small enough to keep in its record (owner, 2026-11-07: the Timeline works from
// what's saved, no shapes): every point, line, arc, curve and constraint as it was before (`put`) where it differs
// from after, the ids only after has (`drop`), and the counters, links and origin as they were. Measured fields
// (lengths, fully constrained...) aren't compared: they follow from the rest.
static QJsonObject leanElement(QJsonObject o) {
    for (const char *k : {"fully_constrained", "length_mm", "radius_mm", "measured_mm", "fixed", "circle_rim", "xyz_mm"})
        o.remove(QLatin1String(k));
    return o;
}

QJsonObject Document::sketchDiff(const SketchGeometry &before, const SketchGeometry &after) const {
    const QJsonObject b = geometryJson(before, nullptr), a = geometryJson(after, nullptr);
    QJsonObject put, drop;
    for (const char *kind : {"points", "lines", "arcs", "curves", "constraints"}) {
        const QString key = QLatin1String(kind);
        std::map<int, QJsonObject> was, now;
        for (const QJsonValue &v : b.value(key).toArray())
            was[v.toObject().value(QStringLiteral("id")).toInt()] = leanElement(v.toObject());
        for (const QJsonValue &v : a.value(key).toArray())
            now[v.toObject().value(QStringLiteral("id")).toInt()] = leanElement(v.toObject());
        QJsonArray puts, drops;
        for (const auto &[id, o] : was) {
            const auto it = now.find(id);
            if (it == now.end() || it->second != o)
                puts.append(o);
        }
        for (const auto &[id, o] : now)
            if (!was.count(id))
                drops.append(id);
        if (!puts.isEmpty())
            put.insert(key, puts);
        if (!drops.isEmpty())
            drop.insert(key, drops);
    }
    QJsonObject out{{QStringLiteral("next"), b.value(QStringLiteral("next"))}};
    if (!put.isEmpty())
        out.insert(QStringLiteral("put"), put);
    if (!drop.isEmpty())
        out.insert(QStringLiteral("drop"), drop);
    if (b.value(QStringLiteral("links")) != a.value(QStringLiteral("links")))
        out.insert(QStringLiteral("links"), b.value(QStringLiteral("links")));
    if (b.value(QStringLiteral("origin_uv")) != a.value(QStringLiteral("origin_uv")))
        out.insert(QStringLiteral("origin_uv"), b.value(QStringLiteral("origin_uv")));
    return out;
}

// Back from after to before with a sketchDiff.
void Document::unDiff(SketchGeometry &g, const QJsonObject &d) {
    const QJsonObject drop = d.value(QStringLiteral("drop")).toObject(), put = d.value(QStringLiteral("put")).toObject();
    const auto forget = [&](const QString &kind, int id) {
        if (kind == QLatin1String("points")) { g.points.erase(id); g.helperPoints.erase(id); g.projectedPoints.erase(id); }
        else if (kind == QLatin1String("lines")) { g.lines.erase(id); g.helperLines.erase(id); g.constructionLines.erase(id); }
        else if (kind == QLatin1String("arcs")) { g.arcs.erase(id); g.helperArcs.erase(id); g.constructionArcs.erase(id); }
        else if (kind == QLatin1String("curves")) { g.curves.erase(id); g.constructionCurves.erase(id); }
        else if (kind == QLatin1String("constraints")) g.constraints.erase(id);
    };
    for (auto it = drop.begin(); it != drop.end(); ++it)
        for (const QJsonValue &v : it.value().toArray())
            forget(it.key(), v.toInt());
    QJsonObject read;
    for (auto it = put.begin(); it != put.end(); ++it) {
        for (const QJsonValue &v : it.value().toArray())
            forget(it.key(), v.toObject().value(QStringLiteral("id")).toInt());
        read.insert(it.key(), it.value());
    }
    // readGeometry adds what's listed; the counters and links come from the diff (as they were before).
    const std::vector<SketchGeometry::Link> links = g.links;
    const int nl = g.nextLink;
    read.insert(QStringLiteral("next"), d.value(QStringLiteral("next")));
    readGeometry(g, read);
    g.links = links;
    g.nextLink = nl;
    if (d.contains(QStringLiteral("links"))) {
        g.links.clear();
        readLinks(g, d.value(QStringLiteral("links")).toArray());
    }
    const QJsonArray o = d.value(QStringLiteral("origin_uv")).toArray();
    if (o.size() == 2)
        g.origin = {o[0].toDouble(), o[1].toDouble()};
}

// Reads what geometryJson wrote (origin_uv apart: older files work it out from the plane).
void Document::readGeometry(SketchGeometry &g, const QJsonObject &o) {
    auto ints = [](const QJsonValue &v) {
        std::vector<int> out;
        for (const QJsonValue &x : v.toArray())
            out.push_back(x.toInt());
        return out;
    };
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
        c.font = k.value(QStringLiteral("font")).toString();
        c.flags = k.value(QStringLiteral("flags")).toInt();
        c.path = ints(k.value(QStringLiteral("path")));
        c.anchor = k.value(QStringLiteral("anchor")).toDouble();
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
        c.driven = k.value(QStringLiteral("driven")).toBool();
        c.expr = k.value(QStringLiteral("expr")).toString();
        c.exprFlip = k.value(QStringLiteral("expr_flip")).toBool();
        const QJsonArray label = k.contains(QStringLiteral("label_offset_mm")) ? k.value(QStringLiteral("label_offset_mm")).toArray()
                                                                               : k.value(QStringLiteral("label_along_lines_mm")).toArray();
        if (label.size() == 2) {
            c.labelPlaced = true;
            c.labelU = label[0].toDouble();
            c.labelV = label[1].toDouble();
        }
        g.constraints[k.value(QStringLiteral("id")).toInt()] = c;
    }
    readLinks(g, o.value(QStringLiteral("links")).toArray());
    const QJsonObject n = o.value(QStringLiteral("next")).toObject();
    g.nextPoint = n.value(QStringLiteral("point")).toInt(1);
    g.nextLine = n.value(QStringLiteral("line")).toInt(1);
    g.nextArc = n.value(QStringLiteral("arc")).toInt(1);
    g.nextCurve = n.value(QStringLiteral("curve")).toInt(1);
    g.nextConstraint = n.value(QStringLiteral("constraint")).toInt(1);
}

QString Document::openFrom(const QString &path) {
    if (m_importing || m_working)
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
    cancelBuild();
    m_importSolids.clear();
    for (const auto &entry : m_bodies)
        m_engine->release(omamake::BodyId(entry.first));
    if (m_body && !m_bodies.count(int(m_body)))
        m_engine->release(m_body);
    m_body = 0;
    m_shownSet = false;
    m_nextBody = 1;
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
    m_tween.reset();
    m_liveSketch = 0;
    m_liveWaiting = false;
    m_actions = {};
    clearRedo();
    for (const Redo &r : m_future)
        for (const omamake::BodyId b : r.parked)
            m_engine->release(b);
    m_future.clear();
    m_meshCache.clear();
    m_params.clear();
    m_nextParameter = 1;

    // Bodies: made again by their history (so far only the starting cube), then the file's
    // components and bodies take over; the engine may number bodies differently, so their keys follow.
    m_components = {rootComponent()};
    // A design saved with every body's solid (several bodies): they come back as they were; the
    // saved ids stand for the new ones. Older files rebuild their one body from the history below.
    std::map<int, int> solidOf;
    for (const QJsonValue &v : d.value(QStringLiteral("solids")).toArray()) {
        const QJsonObject o = v.toObject();
        const QByteArray packed = QByteArray::fromBase64(o.value(QStringLiteral("file")).toString().toLatin1());
        const omamake::BodyId id = m_engine->loadBody(qUncompress(packed).toStdString());
        if (id)
            solidOf[o.value(QStringLiteral("id")).toInt()] = int(id);
        else
            qWarning().noquote() << "open: a saved body couldn't be read";
    }
    const bool haveSolids = !solidOf.empty();
    for (const QJsonValue &v : d.value(QStringLiteral("history")).toArray()) {
        QJsonObject a = v.toObject();
        if (!haveSolids && a.value(QStringLiteral("type")).toString() == QLatin1String("cube"))
            buildCube(a.value(QStringLiteral("size_mm")).toDouble());
        m_bodyFile = QByteArray::fromBase64(d.value(QStringLiteral("body_file")).toString().toLatin1());
        const QString kind = a.value(QStringLiteral("type")).toString();
        if (haveSolids) {
            // the solids are already back
        } else if ((kind == QLatin1String("import_step") || kind == QLatin1String("import_mesh")) && !m_bodyFile.isEmpty()) {
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
        // Body ids in the records: the file's bodies take this window's ids; a body gone before the save gets
        // its id negated, so it can never be taken for one of this window's (the background rebuild follows
        // bodies by these ids, step by step).
        if (haveSolids) {
            const auto id = [&solidOf](int was) { return solidOf.count(was) ? solidOf.at(was) : (was > 0 ? -was : was); };
            const auto ids = [&id](const QJsonArray &list) { QJsonArray out; for (const QJsonValue &v : list) out.append(id(v.toInt())); return out; };
            if (kind == QLatin1String("extrude") || kind == QLatin1String("cube") || kind == QLatin1String("import_step")
                || kind == QLatin1String("import_mesh")) {
                if (a.contains(QStringLiteral("body")))
                    a.insert(QStringLiteral("body"), id(a.value(QStringLiteral("body")).toInt()));
                a.insert(QStringLiteral("created"), ids(a.value(QStringLiteral("created")).toArray()));
                QJsonArray swaps;
                for (const QJsonValue &v : a.value(QStringLiteral("swaps")).toArray())
                    swaps.append(ids(v.toArray()));
                a.insert(QStringLiteral("swaps"), swaps);
                if (kind == QLatin1String("extrude")) {
                    QJsonObject spec = a.value(QStringLiteral("spec")).toObject();
                    spec.insert(QStringLiteral("skip1"), ids(spec.value(QStringLiteral("skip1")).toArray()));
                    spec.insert(QStringLiteral("skip2"), ids(spec.value(QStringLiteral("skip2")).toArray()));
                    a.insert(QStringLiteral("spec"), spec);
                }
            } else if (kind == QLatin1String("visibility") && a.value(QStringLiteral("key")).toString().startsWith(QLatin1Char('b'))) {
                a.insert(QStringLiteral("key"), QStringLiteral("b%1").arg(id(a.value(QStringLiteral("key")).toString().mid(1).toInt())));
            }
        }
        if (a.contains(QStringLiteral("solid"))) {      // an import's own solid (see save)
            m_importSolids[a.value(QStringLiteral("at")).toString()] = QByteArray::fromBase64(a.value(QStringLiteral("solid")).toString().toLatin1());
            a.remove(QStringLiteral("solid"));
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
    m_nextBody = next.value(QStringLiteral("body")).toInt(1);
    for (const QJsonValue &v : d.value(QStringLiteral("hidden")).toArray())
        m_hidden.insert(v.toString());
    for (const QJsonValue &v : d.value(QStringLiteral("parameters")).toArray()) {
        const QJsonObject o = v.toObject();
        Parameter p;
        p.id = o.value(QStringLiteral("id")).toInt();
        p.name = o.value(QStringLiteral("name")).toString();
        p.unit = o.value(QStringLiteral("unit")).toString(QStringLiteral("in"));
        p.expr = o.value(QStringLiteral("expr")).toString();
        p.comment = o.value(QStringLiteral("comment")).toString();
        m_params.push_back(p);
        m_nextParameter = std::max(m_nextParameter, p.id + 1);
    }
    m_nextParameter = std::max(m_nextParameter, d.value(QStringLiteral("next_parameter")).toInt(1));

    std::map<int, int> renumbered;        // saved id -> id now (engine numbers differ between runs)
    for (const QJsonValue &v : d.value(QStringLiteral("bodies")).toArray()) {
        const QJsonObject o = v.toObject();
        const int was = o.value(QStringLiteral("id")).toInt();
        const int now = haveSolids ? (solidOf.count(was) ? solidOf.at(was) : -1) : (m_body ? int(m_body) : was);
        if (now < 0)
            continue;      // its solid couldn't be read
        m_bodies[now] = {o.value(QStringLiteral("component")).toInt(), o.value(QStringLiteral("name")).toString(), {}, {}};
        renumbered[was] = now;
    }
    for (Component &c : m_components) {
        std::vector<int> kept;
        for (int id : c.bodies)
            if (renumbered.count(id))
                kept.push_back(renumbered.at(id));
        c.bodies = kept;
    }
    {
        std::set<QString> hidden;
        for (const QString &key : m_hidden) {
            if (key.startsWith(QLatin1Char('b')) && renumbered.count(key.mid(1).toInt()))
                hidden.insert(QStringLiteral("b%1").arg(renumbered.at(key.mid(1).toInt())));
            else
                hidden.insert(key);
        }
        m_hidden = hidden;
    }
    if (!m_bodies.empty() && !m_body)
        m_body = omamake::BodyId(m_bodies.rbegin()->first);

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
        readGeometry(g, o);
        m_sketches[o.value(QStringLiteral("id")).toInt()] = s;
    }

    m_filePath = source;
    restartWalk();         // the sketches' earlier states, worked out in the background from here
    // The design's steps from the file: built again in the background once the sketches' states are known.
    m_loaded = int(m_actions.size());
    m_buildDone = true;
    for (const QJsonValue &v : m_actions)
        if (changesBodies(v.toObject()) && v.toObject().value(QStringLiteral("type")).toString() != QLatin1String("cube"))
            m_buildDone = false;
    m_builtTo = m_buildDone ? m_loaded : 0;
    emit buildChanged();
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
        addBody(placed, 0);
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
    {
        double o[3], x[3], y[3];
        planeAxes(plane, o, x, y);
        m_sketches[id].geometry.origin = {-(o[0] * x[0] + o[1] * x[1] + o[2] * x[2]), -(o[0] * y[0] + o[1] * y[1] + o[2] * y[2])};
    }
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
    emit structureChanged();            // what was made after it comes back
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
    emit structureChanged();            // what was made after it goes out of sight
}

QVariantMap Document::sketchPlane(int id) const {
    const auto found = m_sketches.find(id);
    return found == m_sketches.end() ? QVariantMap{} : found->second.plane;
}

QVariantMap Document::undo() {
    QJsonObject undone;
    return undoInto(m_redo, undone) ? undone.toVariantMap() : QVariantMap{};
}

// A sketch step taken back on the sketches `sketchOf` gives (Ctrl+Z on the document's own, the Timeline's
// working out on copies): 1 done, 0 not a step that changes a sketch's drawing, -1 it can't be (nothing kept).
// `snapshots`: whole copies kept in memory this session (null: only what the records hold).
int Document::unstepSketch(const QJsonObject &last, const std::function<SketchGeometry *(int)> &sketchOf,
                           std::map<int, SketchGeometry> *snapshots) {
    const QString type = last.value(QStringLiteral("type")).toString();
    if (last.contains(QStringLiteral("snapshot"))) {
        // Kept whole before the edit (every tool from Midpoint Line on): put back exactly that.
        SketchGeometry *found = sketchOf(last.value(QStringLiteral("sketch")).toInt());
        const auto kept = snapshots ? snapshots->find(last.value(QStringLiteral("snapshot")).toInt()) : std::map<int, SketchGeometry>::iterator{};
        if (found && snapshots && kept != snapshots->end()) {
            (*found) = kept->second;
            snapshots->erase(kept);
        } else if (found && last.contains(QStringLiteral("before"))) {
            unDiff((*found), last.value(QStringLiteral("before")).toObject());   // opened from a file
        } else {
            return -1;                       // made before this file was opened, by an older OmaMake: nothing kept
        }
    } else if (type == QLatin1String("line") || type == QLatin1String("rectangle") || type == QLatin1String("shape")) {
        SketchGeometry *found = sketchOf(last.value(QStringLiteral("sketch")).toInt());
        if (found)
            forgetSketchLines(*found, last);
    } else if (type == QLatin1String("text_edit")) {
        SketchGeometry *found = sketchOf(last.value(QStringLiteral("sketch")).toInt());
        if (found) {
            const QJsonObject was = last.value(QStringLiteral("was")).toObject();
            const auto k = (*found).curves.find(was.value(QStringLiteral("id")).toInt());
            if (k != (*found).curves.end()) {
                k->second.text = was.value(QStringLiteral("text")).toString();
                k->second.font = was.value(QStringLiteral("font")).toString();
                k->second.flags = was.value(QStringLiteral("flags")).toInt();
                k->second.anchor = was.value(QStringLiteral("anchor")).toDouble();
                k->second.value = was.value(QStringLiteral("height_mm")).toDouble();
            }
        }
    } else if (type == QLatin1String("parameters")) {
        for (const QJsonValue &v : last.value(QStringLiteral("dims")).toArray()) {
            const QJsonObject o = v.toObject();
            SketchGeometry *found = sketchOf(o.value(QStringLiteral("sketch")).toInt());
            if (!found)
                continue;
            const auto c = (*found).constraints.find(o.value(QStringLiteral("id")).toInt());
            if (c == (*found).constraints.end())
                continue;
            c->second.value = o.value(QStringLiteral("value")).toDouble();
            c->second.unit = o.value(QStringLiteral("unit")).toString();
            c->second.expr = o.value(QStringLiteral("expr")).toString();
            c->second.exprFlip = o.value(QStringLiteral("expr_flip")).toBool();
        }
        for (const QJsonValue &v : last.value(QStringLiteral("moved")).toArray()) {
            const QJsonObject o = v.toObject();
            SketchGeometry *found = sketchOf(o.value(QStringLiteral("sketch")).toInt());
            if (found)
                (*found).points[o.value(QStringLiteral("id")).toInt()] = {o.value(QStringLiteral("u")).toDouble(),
                                                                                        o.value(QStringLiteral("v")).toDouble()};
        }
    } else if (type == QLatin1String("dimension_value")) {
        SketchGeometry *found = sketchOf(last.value(QStringLiteral("sketch")).toInt());
        if (found) {
            SketchGeometry &g = (*found);
            const auto c = g.constraints.find(last.value(QStringLiteral("id")).toInt());
            if (c != g.constraints.end()) {
                c->second.value = last.value(QStringLiteral("value")).toDouble();
                c->second.unit = last.value(QStringLiteral("unit")).toString();
                c->second.expr = last.value(QStringLiteral("expr")).toString();
                c->second.exprFlip = last.value(QStringLiteral("expr_flip")).toBool();
            }
            for (const QJsonValue &v : last.value(QStringLiteral("moved")).toArray()) {
                const QJsonObject m = v.toObject();
                g.points[m.value(QStringLiteral("id")).toInt()] = {m.value(QStringLiteral("u")).toDouble(),
                                                                    m.value(QStringLiteral("v")).toDouble()};
            }
        }
    } else if (type == QLatin1String("drag") || type == QLatin1String("fix")) {
        SketchGeometry *found = sketchOf(last.value(QStringLiteral("sketch")).toInt());
        if (found) {
            SketchGeometry &g = (*found);
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
    } else if (type == QLatin1String("delete_constraints")) {
        SketchGeometry *found = sketchOf(last.value(QStringLiteral("sketch")).toInt());
        if (found) {
            SketchGeometry &g = (*found);
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
    } else if (type == QLatin1String("constraint")) {
        SketchGeometry *found = sketchOf(last.value(QStringLiteral("sketch")).toInt());
        if (found) {
            SketchGeometry &g = (*found);
            g.constraints.erase(last.value(QStringLiteral("id")).toInt());
            for (const QJsonValue &v : last.value(QStringLiteral("moved")).toArray()) {
                const QJsonObject m = v.toObject();
                g.points[m.value(QStringLiteral("id")).toInt()] = {m.value(QStringLiteral("u")).toDouble(),
                                                                    m.value(QStringLiteral("v")).toDouble()};
            }
        }
    }
    else
        return 0;
    return 1;
}

bool Document::undoInto(std::vector<Redo> &into, QJsonObject &undone) {
    if (m_actions.isEmpty())
        return false;
    if (m_importing || m_working)
        return false;
    const QJsonObject last = m_actions.last().toObject();
    const QString type = last.value(QStringLiteral("type")).toString();
    // A step from an opened file (or the starting cube): one that changed bodies goes back to the design the
    // background rebuild made for before it; any other by its own record (a sketch step by its diff).
    const Built *built = nullptr;
    if (!last.value(QStringLiteral("undoable")).toBool(true)) {
        if (type == QLatin1String("cube") || int(m_actions.size()) > m_loaded)
            return false;
        if (changesBodies(last) && !(built = builtAt(int(m_actions.size()) - 1)))
            return false;
    }
    // The document as it is now, kept whole for Ctrl+Y; what the undo lets go of is parked in it.
    Redo kept{last, m_components, m_bodies, m_sketches, m_hidden, m_active, m_openSketch, m_nextComponent, m_nextBody,
              m_snapshots, m_params, m_extrudeKeep, m_beforeFit, m_body, m_stepFile, m_bodyFile, {}};
    m_parking = &kept;
    struct Unpark { Redo *&p; ~Unpark() { p = nullptr; } } unpark{m_parking};
    if (built) {
        applyBuilt(*built);
    } else if (type == QLatin1String("sketch")) {
        removeSketch(last.value(QStringLiteral("id")).toInt());
        m_openSketch = 0;
    } else if (type == QLatin1String("import_step") || type == QLatin1String("import_mesh")) {
        dropBody(last.contains(QStringLiteral("body")) ? last.value(QStringLiteral("body")).toInt() : int(m_body));
        m_stepFile.clear();
        m_bodyFile.clear();
        refreshMesh();
        emit changed();
        emit structureChanged();
    } else if (type == QLatin1String("fit_surfaces")) {
        const Fitted before = m_beforeFit.back();
        m_beforeFit.pop_back();
        const omamake::BodyId fitted = m_body;
        swapBody(int(fitted), before.body);
        releaseBody(fitted);
        m_bodyFile = before.file;
        refreshMesh();
        emit changed();
    } else if (type == QLatin1String("extrude")) {
        const auto kept = m_extrudeKeep.find(last.value(QStringLiteral("token")).toInt());
        if (kept == m_extrudeKeep.end())
            return false;                    // made before this file was opened: the old solids aren't kept
        for (const QJsonValue &b : last.value(QStringLiteral("created")).toArray())
            dropBody(b.toInt());
        for (const QJsonValue &c : last.value(QStringLiteral("components")).toArray())
            removeComponent(c.toInt());
        for (const auto &[old, now] : kept->second) {
            swapBody(now, omamake::BodyId(old));
            releaseBody(omamake::BodyId(now));
        }
        m_extrudeKeep.erase(kept);
        refreshMesh();
        emit changed();
        emit structureChanged();
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
        emit structureChanged();
    } else if (type == QLatin1String("edit_sketch")) {
        m_openSketch = 0;                                               // the window leaves it
        if (last.value(QStringLiteral("was_hidden")).toBool())
            m_hidden.insert(QStringLiteral("s%1").arg(last.value(QStringLiteral("sketch")).toInt()));
        emit structureChanged();
    } else if (const int done = unstepSketch(last, [this](int id) -> SketchGeometry * {
                   const auto found = m_sketches.find(id);
                   return found == m_sketches.end() ? nullptr : &found->second.geometry;
               }, &m_snapshots); done != 0) {
        if (done < 0)
            return false;
        if (type == QLatin1String("parameters")) {
            m_params.clear();
            for (const QJsonValue &v : last.value(QStringLiteral("was")).toArray()) {
                const QJsonObject o = v.toObject();
                m_params.push_back({o.value(QStringLiteral("id")).toInt(), o.value(QStringLiteral("name")).toString(),
                                    o.value(QStringLiteral("unit")).toString(), o.value(QStringLiteral("expr")).toString(),
                                    o.value(QStringLiteral("comment")).toString()});
            }
            emit parametersChanged();
        }
        sketchesEdited();
    }
    m_actions.removeLast();
    m_lowSeen = std::min(m_lowSeen, int(m_actions.size()));
    if (m_runStart > int(m_actions.size()))
        m_runStart = int(m_actions.size());
    m_parking = nullptr;
    into.push_back(std::move(kept));
    if (!m_quietSteps)
        qInfo().noquote() << "history: undid:" << last.value(QStringLiteral("summary")).toString();
    emit historyChanged();
    undone = last;
    return true;
}

QVariantMap Document::redo() {
    QJsonObject redone;
    return redoFrom(m_redo, redone) ? redone.toVariantMap() : QVariantMap{};
}

bool Document::redoFrom(std::vector<Redo> &from, QJsonObject &redone) {
    if (from.empty() || m_importing || m_working)
        return false;
    Redo r = std::move(from.back());
    from.pop_back();
    m_components = std::move(r.components);
    m_bodies = std::move(r.bodies);
    m_sketches = std::move(r.sketches);
    m_hidden = std::move(r.hidden);
    m_active = r.active;
    m_openSketch = r.openSketch;
    m_nextComponent = r.nextComponent;
    m_nextBody = r.nextBody;
    m_snapshots = std::move(r.snapshots);
    m_params = std::move(r.params);
    m_extrudeKeep = std::move(r.extrudeKeep);
    m_beforeFit = std::move(r.beforeFit);
    m_body = r.body;
    m_stepFile = std::move(r.stepFile);
    m_bodyFile = std::move(r.bodyFile);
    m_actions.append(r.action);
    m_textEditItem = -1;                        // a redone step is never added to by what comes next
    m_paramItem = -1;
    redone = r.action.toObject();
    if (!m_quietSteps)
        qInfo().noquote() << "history: redid:" << redone.value(QStringLiteral("summary")).toString();
    announceAll();
    return true;
}

QVariantMap Document::rewindStep(int actions) {
    if (actions < 1 || std::size_t(actions) > m_future.size())
        return {};
    const Redo &after = m_future[m_future.size() - std::size_t(actions)];
    const auto shown = [this](const std::map<int, Named> &bodies, const std::set<QString> &hidden) {
        QVariantList ids;
        for (const auto &entry : bodies)
            if (!hidden.count(QStringLiteral("b%1").arg(entry.first)) && m_meshCache.count(entry.first))
                ids << entry.first;
        return ids;
    };
    const QVariantList before = shown(m_bodies, m_hidden), then = shown(after.bodies, after.hidden);
    const QJsonObject last = after.action.toObject();
    QVariantMap out{{QStringLiteral("before"), before}, {QStringLiteral("after"), then},
                    {QStringLiteral("type"), last.value(QStringLiteral("type")).toString()}};
    if (last.value(QStringLiteral("type")).toString() != QLatin1String("extrude"))
        return out;
    // The extrude's plane, read the way it was made: the design is as it was just before it.
    ExtrudeJob job;
    QString why;
    if (!readExtrude(last.value(QStringLiteral("spec")).toObject().toVariantMap(), job, why) || job.places.empty())
        return out;
    const omamake::Placement &place = job.places.front();
    const QVector3D origin(float(place.origin[0]), float(place.origin[1]), float(place.origin[2]));
    const QVector3D axis = QVector3D::crossProduct(QVector3D(float(place.x[0]), float(place.x[1]), float(place.x[2])),
                                                   QVector3D(float(place.y[0]), float(place.y[1]), float(place.y[2]))).normalized();
    // How far the extrude goes from its plane: its own length each side, as it was worked out (To Object
    // and All included). Only when that says nothing, how far the bodies that changed reach.
    // `low` and `high`: the span it fills along the axis (negative: the other side of the plane), so
    // the rewind only moves on the extrude's own side(s).
    float low = 0, high = 0;
    for (const ExtrudeSide &side : job.sides)
        for (std::size_t i = 0; i < side.from.size() && i < side.to.size(); ++i)
            for (const double d : {side.from[i], side.to[i]}) {
                low = std::min(low, float(d));
                high = std::max(high, float(d));
            }
    if (high - low > 0) {
        out.insert(QStringLiteral("origin"), origin);
        out.insert(QStringLiteral("axis"), axis);
        out.insert(QStringLiteral("low"), low);
        out.insert(QStringLiteral("high"), high);
        // Where on the plane it happens: the areas' box in the plane's own directions, grown by as much as
        // a taper can widen them over the length, so the split and its glow stay on what changes.
        double widen = 0;
        for (const ExtrudeSide &side : job.sides)
            widen = std::max(widen, std::abs(std::tan(side.taper * M_PI / 180)) * double(std::max(-low, high)));
        double u0 = 1e300, u1 = -1e300, v0 = 1e300, v1 = -1e300;
        for (std::size_t a = 0; a < job.profiles.size() && a < job.places.size(); ++a) {
            // Each area's own placement, measured in the first one's directions (all lie on one plane).
            const omamake::Placement &pl = job.places[a];
            for (const auto &loop : job.profiles[a].loops)
                for (const omamake::Point2 &p : loop) {
                    double w[3];
                    for (int k = 0; k < 3; ++k)
                        w[k] = pl.origin[k] + p.x * pl.x[k] + p.y * pl.y[k] - place.origin[k];
                    const double u = w[0] * place.x[0] + w[1] * place.x[1] + w[2] * place.x[2];
                    const double v = w[0] * place.y[0] + w[1] * place.y[1] + w[2] * place.y[2];
                    u0 = std::min(u0, u); u1 = std::max(u1, u);
                    v0 = std::min(v0, v); v1 = std::max(v1, v);
                }
        }
        if (u0 <= u1) {
            const double margin = widen + 0.01 * std::max(u1 - u0, v1 - v0) + 0.05;
            out.insert(QStringLiteral("xAxis"), QVector3D(float(place.x[0]), float(place.x[1]), float(place.x[2])));
            out.insert(QStringLiteral("yAxis"), QVector3D(float(place.y[0]), float(place.y[1]), float(place.y[2])));
            out.insert(QStringLiteral("box"), QVariantList{u0 - margin, u1 + margin, v0 - margin, v1 + margin});
        }
        return out;
    }
    std::set<int> was, now;
    for (const QVariant &v : before)
        was.insert(v.toInt());
    for (const QVariant &v : then)
        now.insert(v.toInt());
    float reach = 0;
    const auto measure = [&](int id) {
        const omamake::Mesh &m = m_meshCache.at(id);
        for (std::size_t i = 0; i + 2 < m.positions.size(); i += 3)
            reach = std::max(reach, std::abs(QVector3D::dotProduct(QVector3D(m.positions[i], m.positions[i + 1], m.positions[i + 2]) - origin, axis)));
    };
    for (const int id : was)
        if (!now.count(id))
            measure(id);
    for (const int id : now)
        if (!was.count(id))
            measure(id);
    out.insert(QStringLiteral("origin"), origin);
    out.insert(QStringLiteral("axis"), axis);
    out.insert(QStringLiteral("low"), -reach);
    out.insert(QStringLiteral("high"), reach);
    return out;
}

void Document::fillBodies(MeshGeometry *faces, MeshGeometry *edges, const QVariantList &ids) const {
    omamake::Mesh joined;
    bool first = true;
    for (const QVariant &v : ids) {
        const auto found = m_meshCache.find(v.toInt());
        if (found == m_meshCache.end())
            continue;
        const omamake::Mesh &one = found->second;
        const auto base = std::uint32_t(joined.positions.size() / 3);
        joined.positions.insert(joined.positions.end(), one.positions.begin(), one.positions.end());
        joined.normals.insert(joined.normals.end(), one.normals.begin(), one.normals.end());
        for (const std::uint32_t i : one.indices)
            joined.indices.push_back(i + base);
        joined.edgeLines.insert(joined.edgeLines.end(), one.edgeLines.begin(), one.edgeLines.end());
        for (int k = 0; k < 3; ++k) {
            joined.min[k] = first ? one.min[k] : std::min(joined.min[k], one.min[k]);
            joined.max[k] = first ? one.max[k] : std::max(joined.max[k], one.max[k]);
        }
        first = false;
    }
    if (faces)
        faces->setMesh(joined);
    if (edges)
        edges->setMesh(joined);
}

void Document::announceAll() {
    refreshMesh();
    sketchesEdited();
    emit changed();
    emit structureChanged();
    emit parametersChanged();
    emit historyChanged();
}

int Document::rewindTo(int count, bool inSketch) {
    count = std::max(0, count);
    if (m_importing || m_working || count == int(m_actions.size()))
        return int(m_actions.size());
    const int open = m_openSketch;
    {
        // Every step's own announcements held back: one of each at the end, so a sketch of fifty
        // steps costs one redraw.
        const QSignalBlocker quiet(this);
        QJsonObject step;
        if (m_future.empty())
            m_staleAt = 0;
        while (int(m_actions.size()) > count && undoInto(m_future, step)) {}
        // Forward: anything Ctrl+Z took back before the marker comes back first (the steps behind
        // the marker were kept on top of it), and nothing goes forward over a step made meanwhile.
        if (int(m_actions.size()) < count && !futureStale()) {
            // Not a step made while the marker was back and taken back since: that one is another way
            // the design could have gone, and is dropped once the steps behind the marker come forward.
            while (!m_redo.empty() && (!m_staleAt || int(m_actions.size()) + 1 < m_staleAt) && redoFrom(m_redo, step)) {}
            if (!m_redo.empty())
                clearRedo();
            m_staleAt = 0;
            while (int(m_actions.size()) < count && !futureStale() && redoFrom(m_future, step)) {}
        }
        // A sketch's steps go together: never left inside one, unless the marker is in the open
        // sketch's own row (its steps only, so it stays open).
        m_openSketch = inSketch ? open : 0;
    }
    if (m_tween && m_tween->actions)
        m_tween.reset();         // the marker's in-between was for where the design was
    announceAll();
    return int(m_actions.size());
}

void Document::releaseBody(omamake::BodyId body) {
    if (m_builtOwned.count(int(body)))
        return;                          // kept for the opened file's earlier steps (m_built)
    if (m_parking) {
        m_parking->parked.push_back(body);
    } else {
        m_engine->release(body);
        m_meshCache.erase(int(body));
    }
}

void Document::clearRedo() {
    for (const Redo &r : m_redo)
        for (const omamake::BodyId b : r.parked) {
            m_engine->release(b);
            m_meshCache.erase(int(b));
        }
    m_redo.clear();
}

// Every point's neighbours through what's drawn: lines, arcs and circles, curves.
static std::map<int, std::vector<int>> joined(const SketchGeometry &g) {
    std::map<int, std::vector<int>> out;
    const auto tie = [&](int a, int b) {
        if (a == b)
            return;
        out[a].push_back(b);
        out[b].push_back(a);
    };
    for (const auto &[id, l] : g.lines)
        tie(l.a, l.b);
    for (const auto &[id, k] : g.arcs) {
        tie(k.c, k.a);
        tie(k.c, k.b);
    }
    for (const auto &[id, k] : g.curves)
        for (std::size_t i = 1; i < k.points.size(); ++i)
            tie(k.points[i - 1], k.points[i]);
    return out;
}

// Where each point `born` (in `in`, not in the other state) starts from: the point its shape was
// started at. A shape drawn on from something already there (a line from an existing end, a rectangle
// on an existing corner) starts there; one drawn by itself starts at its own first point, the first
// click (the lowest id: points are numbered as they are made). Text is simply there (laid out in a box
// shrunk to nothing it would only jumble): its points start where they end.
static std::map<int, int> startsOf(const SketchGeometry &in, const std::set<int> &born) {
    const auto near = joined(in);
    std::set<int> text;
    for (const auto &[id, k] : in.curves)
        if (k.type == QLatin1String("text"))
            text.insert(k.points.begin(), k.points.end());
    std::map<int, int> out;
    for (const int seed : born) {               // lowest id first: each group's first click
        if (out.count(seed))
            continue;
        if (text.count(seed)) {
            out[seed] = seed;
            continue;
        }
        // The group of new points joined to it, and the oldest point already there that it touches.
        std::vector<int> group{seed}, todo{seed};
        std::set<int> seen{seed};
        int existing = 0;
        bool touches = false;
        while (!todo.empty()) {
            const int at = todo.back();
            todo.pop_back();
            const auto n = near.find(at);
            if (n == near.end())
                continue;
            for (const int next : n->second) {
                if (seen.count(next))
                    continue;
                seen.insert(next);
                if (born.count(next) && !text.count(next)) {
                    group.push_back(next);
                    todo.push_back(next);
                } else if (!born.count(next) && (!touches || next < existing)) {
                    existing = next;
                    touches = true;
                }
            }
        }
        for (const int p : group)
            out[p] = touches ? existing : seed;
    }
    return out;
}

// Nothing added or taken away between two states of a sketch (only moved).
static bool sameShape(const SketchGeometry &a, const SketchGeometry &b) {
    const auto same = [](const auto &x, const auto &y) {
        return x.size() == y.size() && std::equal(x.begin(), x.end(), y.begin(), [](const auto &p, const auto &q) { return p.first == q.first; });
    };
    return same(a.points, b.points) && same(a.lines, b.lines) && same(a.arcs, b.arcs) && same(a.curves, b.curves);
}

void Document::blendTween() {
    SketchTween &w = *m_tween;
    const double e = std::clamp(w.t, 0.0, 1.0);
    const auto lerp = [e](const SketchGeometry::Point &a, const SketchGeometry::Point &b) {
        return SketchGeometry::Point{a.u + (b.u - a.u) * e, a.v + (b.v - a.v) * e};
    };
    SketchGeometry g = w.to;
    std::set<int> born, gone;
    for (const auto &[id, p] : w.to.points)
        if (!w.from.points.count(id))
            born.insert(id);
    if (w.grow)
        for (const auto &[id, p] : w.from.points)
            if (!w.to.points.count(id))
                gone.insert(id);
    // Points in both glide.
    for (auto &[id, p] : g.points) {
        const auto was = w.from.points.find(id);
        if (was != w.from.points.end())
            p = lerp(was->second, p);
    }
    if (w.grow) {
        // What goes away shrinks back to where it was started, drawn from the state it's still in.
        for (const auto &[id, l] : w.from.lines)
            if (!g.lines.count(id))
                g.lines[id] = l;
        for (const auto &[id, k] : w.from.arcs)
            if (!g.arcs.count(id))
                g.arcs[id] = k;
        for (const auto &[id, k] : w.from.curves)
            if (!g.curves.count(id))
                g.curves[id] = k;
        g.helperLines.insert(w.from.helperLines.begin(), w.from.helperLines.end());
        g.helperPoints.insert(w.from.helperPoints.begin(), w.from.helperPoints.end());
        g.helperArcs.insert(w.from.helperArcs.begin(), w.from.helperArcs.end());
        g.constructionLines.insert(w.from.constructionLines.begin(), w.from.constructionLines.end());
        g.constructionArcs.insert(w.from.constructionArcs.begin(), w.from.constructionArcs.end());
        g.constructionCurves.insert(w.from.constructionCurves.begin(), w.from.constructionCurves.end());
        g.projectedPoints.insert(w.from.projectedPoints.begin(), w.from.projectedPoints.end());
        const auto from = startsOf(w.from, gone);
        for (const int id : gone) {
            const int start = from.at(id);
            const SketchGeometry::Point end = start == id ? w.from.points.at(id)
                                            : g.points.count(start) ? g.points.at(start) : w.from.points.at(start);
            const SketchGeometry::Point was = w.from.points.at(id);
            g.points[id] = {was.u + (end.u - was.u) * e, was.v + (end.v - was.v) * e};
        }
        // What's new grows out of where it was started.
        const auto to = startsOf(w.to, born);
        for (const int id : born) {
            const int start = to.at(id);
            const SketchGeometry::Point at = start == id ? w.to.points.at(id) : g.points.at(start);
            const SketchGeometry::Point end = w.to.points.at(id);
            g.points[id] = {at.u + (end.u - at.u) * e, at.v + (end.v - at.v) * e};
        }
    }
    w.shown = std::move(g);
}

bool Document::setSketchTween(int sketch, int actions, double t) {
    if (!sketch || actions < 1 || t <= 0 || t >= 1 || std::size_t(actions) > m_future.size()) {
        if (m_tween && m_tween->actions)
            m_tween.reset();
        return false;
    }
    const auto now = m_sketches.find(sketch);
    if (now == m_sketches.end())
        return false;
    if (!m_tween || m_tween->sketch != sketch || m_tween->actions != actions) {
        const Redo &after = m_future[m_future.size() - std::size_t(actions)];
        const auto then = after.sketches.find(sketch);
        if (then == after.sketches.end()) {
            m_tween.reset();
            return false;
        }
        SketchTween w;
        w.sketch = sketch;
        w.actions = actions;
        w.grow = true;
        w.from = now->second.geometry;
        w.to = then->second.geometry;
        w.heldFrom = w.from.fullyConstrained();
        w.heldTo = w.to.fullyConstrained();
        w.sameShape = sameShape(w.from, w.to);
        m_tween = std::move(w);
    }
    m_tween->t = t;
    blendTween();
    return true;
}

void Document::restartWalk() {
    const int n = int(m_actions.size());
    m_before.assign(std::size_t(n), {});
    m_walkAt = n;
    m_beforeTop = n;
    m_walkStuck = false;
    m_walkCur.clear();
    for (const auto &[id, sk] : m_sketches)
        m_walkCur[id] = sk.geometry;
    m_lowSeen = m_dirtyFrom = INT_MAX;
    if (m_walkTimer)
        m_walkTimer->start();
    emit walkChanged();
}

// Step i taken back on `cur` (the sketches when the history was i + 1 long), keeping what it changed as it was before.
bool Document::stepBack(int i, std::map<int, SketchGeometry> &cur) {
    const QJsonObject a = m_actions[i].toObject();
    Before b;
    b.at = a.value(QStringLiteral("at")).toString();
    if (a.value(QStringLiteral("type")).toString() == QLatin1String("sketch")) {
        const int id = a.value(QStringLiteral("id")).toInt();
        b.sketches[id] = std::nullopt;
        cur.erase(id);
    } else {
        std::set<int> touched;
        const int done = unstepSketch(a, [&](int id) -> SketchGeometry * {
            const auto found = cur.find(id);
            if (found == cur.end())
                return nullptr;
            touched.insert(id);
            return &found->second;
        }, nullptr);
        if (done < 0)
            return false;
        for (const int id : touched)
            b.sketches[id] = cur.at(id);
    }
    m_before[std::size_t(i)] = std::move(b);
    return true;
}

// What the history did since: steps gone, changed or added. What was worked out for steps still there stays (a step's
// before depends only on the steps before it).
void Document::syncWalk() {
    const int n = int(m_actions.size());
    int top = std::min({m_beforeTop, n, m_dirtyFrom});
    for (int i = std::max(m_walkAt, std::min(m_lowSeen, top)); i < top; ++i)
        if (m_before[std::size_t(i)].at != m_actions[i].toObject().value(QStringLiteral("at")).toString()) {
            top = i;
            break;
        }
    m_lowSeen = m_dirtyFrom = INT_MAX;
    if (top < m_walkAt || n < m_walkAt) {
        restartWalk();       // the walk's own starting point is gone
        return;
    }
    m_before.resize(std::size_t(n));
    if (top < n) {
        // New steps on top: from the sketches as they are now, back to the ones already known.
        std::map<int, SketchGeometry> cur;
        for (const auto &[id, sk] : m_sketches)
            cur[id] = sk.geometry;
        for (int i = n - 1; i >= top; --i)
            if (!stepBack(i, cur)) {
                restartWalk();
                m_walkStuck = true;
                return;
            }
    }
    m_beforeTop = n;
}

void Document::walkSome() {
    if (m_importing || m_working)
        return;
    const int was = m_walkAt;
    syncWalk();
    QElapsedTimer clock;
    clock.start();
    while (!m_walkStuck && m_walkAt > 0 && clock.elapsed() < 4) {
        if (!stepBack(m_walkAt - 1, m_walkCur)) {
            m_walkStuck = true;
            qInfo().noquote() << "timeline: sketches worked out back to step" << m_walkAt << "(the one before can't be taken back)";
            break;
        }
        --m_walkAt;
    }
    if (m_walkAt == 0 || m_walkStuck) {
        m_walkCur.clear();
        m_walkTimer->stop();
        if (!m_buildDone && !m_buildStarted && !qEnvironmentVariableIsSet("OMAMAKE_NO_BUILD"))   // (that: a test's baseline)
            startBuild();
    }
    if (m_walkAt != was)
        emit walkChanged();
}

static QVariantMap pointsOf(const SketchGeometry &g) {
    QVariantMap out;
    for (const auto &[id, p] : g.points)
        out[QString::number(id)] = QVariantList{p.u, p.v};
    return out;
}

QVariantMap Document::walkedSketchPoints(int sketch, int count) const {
    SketchGeometry g;
    return sketchAt(sketch, count, g) ? pointsOf(g) : QVariantMap{};
}

QVariantMap Document::shownSketchPoints(int sketch) const {
    const auto found = m_sketches.find(sketch);
    return found == m_sketches.end() ? QVariantMap{} : pointsOf(shownGeometry(sketch, found->second.geometry));
}

bool Document::sketchAt(int sketch, int count, SketchGeometry &out) const {
    const int n = int(m_actions.size());
    if (count < m_walkAt || count > n || m_beforeTop < n)
        return false;
    for (int j = count; j < n; ++j) {
        const auto found = m_before[std::size_t(j)].sketches.find(sketch);
        if (found != m_before[std::size_t(j)].sketches.end()) {
            if (!found->second)
                return false;
            out = *found->second;
            return true;
        }
    }
    const auto now = m_sketches.find(sketch);
    if (now == m_sketches.end())
        return false;
    out = now->second.geometry;
    return true;
}

// ---- The background rebuild of an opened file's design steps (modelBuiltTo) ----

struct Document::Arrived {
    std::map<int, Built> states;            // the rebuild's own solid ids
    std::map<int, Named> finalBodies;       // its bodies after the last step: the file's own bodies here
    struct Solid { std::string bytes; omamake::Mesh mesh; };
    std::map<int, Solid> solids;            // every other solid a state uses
    std::vector<int> order;                 // to load here, oldest state first
    std::size_t next = 0;
    std::map<int, int> toHere;              // its solid id -> this Document's
    bool matched = false;
    QString why;                            // set: it stopped (states only before `stoppedAt`)
};

bool Document::changesBodies(const QJsonObject &a) {
    const QString type = a.value(QStringLiteral("type")).toString();
    return type == QLatin1String("extrude") || type == QLatin1String("import_step") || type == QLatin1String("import_mesh")
        || type == QLatin1String("fit_surfaces") || type == QLatin1String("cube")
        || (type == QLatin1String("visibility") && a.value(QStringLiteral("key")).toString().startsWith(QLatin1Char('b')));
}

void Document::cancelBuild() {
    if (m_buildCancel)
        *m_buildCancel = true;
    m_buildCancel.reset();
    ++m_buildGen;
    m_arrived.reset();
    if (m_buildTimer)
        m_buildTimer->stop();
    for (const int id : m_builtOwned)
        if (!m_bodies.count(id))
            m_engine->release(omamake::BodyId(id));
    m_builtOwned.clear();
    m_built.clear();
    m_buildStarted = false;
    m_buildDone = true;
    m_buildWhy.clear();
    m_builtTo = -1;
    m_loaded = 0;
}

void Document::startBuild() {
    m_buildStarted = true;
    const auto stop = [this](const QString &why) {
        m_buildWhy = why;
        m_buildDone = true;
        qInfo().noquote() << "timeline: the opened file's design steps can't be built again:" << why;
        emit buildChanged();
    };
    // What the thread needs, taken here: each step's record; a new sketch's plane; for an extrude, every sketch as
    // it was then (from the walk).
    struct Step { QJsonObject action; int count; std::map<int, Named> sketches; std::string solid; QByteArray step; };
    std::vector<Step> plan;
    std::set<int> made;
    const int n = std::min(m_loaded, int(m_actions.size()));
    for (int i = 0; i < n; ++i) {
        Step s{m_actions[i].toObject(), i + 1, {}, {}, {}};
        const QString type = s.action.value(QStringLiteral("type")).toString();
        if (type == QLatin1String("import_step") || type == QLatin1String("import_mesh") || type == QLatin1String("fit_surfaces")) {
            // The imported solid: its own (kept with the step), the body itself when nothing has changed it
            // since, the file's last import (body_file, when no Fit came after), or an older file's STEP file.
            bool last = true;
            for (int j = i + 1; j < n; ++j) {
                const QString later = m_actions[j].toObject().value(QStringLiteral("type")).toString();
                last = last && later != QLatin1String("import_step") && later != QLatin1String("import_mesh") && later != QLatin1String("fit_surfaces");
            }
            const int body = s.action.value(QStringLiteral("body")).toInt();
            const auto own = m_importSolids.find(s.action.value(QStringLiteral("at")).toString());
            if (own != m_importSolids.end())
                s.solid = qUncompress(own->second).toStdString();
            else if (m_bodies.count(body))
                s.solid = m_engine->saveBody(omamake::BodyId(body));
            else if (last && !m_bodyFile.isEmpty())
                s.solid = qUncompress(m_bodyFile).toStdString();
            else if (last && !m_stepFile.isEmpty() && type != QLatin1String("fit_surfaces"))
                s.step = qUncompress(m_stepFile);
            if (s.solid.empty() && s.step.isEmpty())
                return stop(QStringLiteral("step %1 (%2): the imported body isn't in the file as it was imported")
                                .arg(i + 1).arg(s.action.value(QStringLiteral("summary")).toString()));
        } else if (type == QLatin1String("sketch")) {
            const int id = s.action.value(QStringLiteral("id")).toInt();
            const auto found = m_sketches.find(id);
            if (found == m_sketches.end())
                return stop(QStringLiteral("sketch %1 isn't in the file").arg(id));
            Named named = found->second;
            named.geometry = {};
            s.sketches[id] = named;
            made.insert(id);
        } else if (type == QLatin1String("extrude")) {
            for (const int id : made) {
                Named named = m_sketches.at(id);
                if (!sketchAt(id, i, named.geometry))
                    continue;
                s.sketches[id] = std::move(named);
            }
            for (const QVariant &v : s.action.value(QStringLiteral("spec")).toObject().toVariantMap().value(QStringLiteral("areas")).toList()) {
                const QVariantMap area = v.toMap();
                if (area.value(QStringLiteral("kind")).toString() != QLatin1String("face") && !s.sketches.count(area.value(QStringLiteral("sketch")).toInt()))
                    return stop(QStringLiteral("the sketch of step %1 isn't known as it was then").arg(i + 1));
            }
        } else if (!changesBodies(s.action) && type != QLatin1String("component") && type != QLatin1String("activate")
                   && type != QLatin1String("visibility")) {
            continue;                    // sketch steps: the sketches come with each extrude
        }
        plan.push_back(std::move(s));
    }
    const bool lite = m_engine->name().rfind("Lite", 0) == 0;
    auto cancel = std::make_shared<std::atomic_bool>(false);
    m_buildCancel = cancel;
    const int gen = m_buildGen;
    QPointer<Document> self(this);
    qInfo().noquote() << "timeline: building the opened file's" << plan.size() << "design steps again in the background";
    QThread *worker = QThread::create([self, gen, cancel, lite, plan = std::move(plan)] {
        QElapsedTimer clock;
        clock.start();
        auto out = std::make_shared<Arrived>();
        Document r(lite ? omamake::makeLiteEngine() : omamake::makeHeavyEngine(), nullptr, true);
        std::map<int, int> toR;          // the file's body ids (as they were then) -> the rebuild's
        const auto snap = [&](int count, const QString &at) {
            Built b;
            b.at = at;
            b.components = r.m_components;
            b.bodies = r.m_bodies;
            for (const QString &h : r.m_hidden)
                if (h.startsWith(QLatin1Char('b')))
                    b.hiddenBodies.insert(h);
            b.nextBody = r.m_nextBody;
            b.nextComponent = r.m_nextComponent;
            b.body = r.m_body;
            out->states[count] = std::move(b);
        };
        snap(0, {});
        for (const Step &s : plan) {
            if (*cancel)
                return;
            if (!out->why.isEmpty())
                break;
            const QJsonObject &a = s.action;
            const QString type = a.value(QStringLiteral("type")).toString();
            const QString at = a.value(QStringLiteral("at")).toString();
            if (self.isNull())
                return;
            const auto fail = [&](const QString &why) {
                out->why = QStringLiteral("%1 (%2)").arg(why, a.value(QStringLiteral("summary")).toString());
            };
            if (type == QLatin1String("sketch")) {
                const int id = a.value(QStringLiteral("id")).toInt();
                const int was = r.m_active;
                r.m_active = a.value(QStringLiteral("component")).toInt();
                r.m_nextSketch = id;
                r.component(r.m_active)->nextSketch = 0;   // set from the name below
                const QString name = a.value(QStringLiteral("name")).toString();
                bool okNumber = false;
                const int number = name.mid(6).toInt(&okNumber);
                r.component(r.m_active)->nextSketch = okNumber ? number : 1;
                r.addSketch(s.sketches.at(id).plane);
                r.m_openSketch = 0;
                r.m_active = was;
            } else if (type == QLatin1String("component")) {
                r.m_nextComponent = a.value(QStringLiteral("id")).toInt();
                r.m_active = a.value(QStringLiteral("parent")).toInt();
                r.newComponent();
            } else if (type == QLatin1String("activate")) {
                r.m_active = a.value(QStringLiteral("to")).toInt();
            } else if (type == QLatin1String("visibility")) {
                QString key = a.value(QStringLiteral("key")).toString();
                if (key.startsWith(QLatin1Char('b'))) {
                    const auto found = toR.find(key.mid(1).toInt());
                    if (found == toR.end()) {
                        fail(QStringLiteral("a body shown or hidden that can't be followed"));
                        continue;
                    }
                    key = QStringLiteral("b%1").arg(found->second);
                }
                if (a.value(QStringLiteral("shown")).toBool())
                    r.m_hidden.erase(key);
                else
                    r.m_hidden.insert(key);
            } else if (type == QLatin1String("cube")) {
                r.addCube(a.value(QStringLiteral("size_mm")).toDouble());
                toR[a.value(QStringLiteral("body")).toInt()] = int(r.m_body);
            } else if (type == QLatin1String("import_step") || type == QLatin1String("import_mesh")) {
                omamake::BodyId body = 0;
                std::string why;
                if (!s.solid.empty()) {
                    body = r.m_engine->loadBody(s.solid);
                } else {
                    const QString temp = QDir::temp().filePath(QStringLiteral("omamake-rebuild-%1.step").arg(QCoreApplication::applicationPid()));
                    QFile file(temp);
                    if (file.open(QIODevice::WriteOnly) && file.write(s.step) > 0) {
                        file.close();
                        body = r.m_engine->importStep(temp.toStdString(), why, [](const char *) {});
                    }
                    QFile::remove(temp);
                }
                if (!body) {
                    fail(QStringLiteral("the imported body couldn't be read back ") + QString::fromStdString(why));
                    continue;
                }
                r.placeImport(body, a.value(QStringLiteral("name")).toString());
                toR[a.value(QStringLiteral("body")).toInt()] = int(body);
            } else if (type == QLatin1String("fit_surfaces")) {
                // The body it was run on (the newest, as then) takes the fitted solid as it was made.
                const omamake::BodyId from = r.m_body;
                const omamake::BodyId body = from ? r.m_engine->loadBody(s.solid) : 0;
                if (!body) {
                    fail(QStringLiteral("the fitted body couldn't be read back"));
                    continue;
                }
                for (auto &[was, now] : toR)
                    if (now == int(from))
                        now = int(body);
                r.swapBody(int(from), body);
            } else if (type == QLatin1String("extrude")) {
                for (const auto &[id, named] : s.sketches) {
                    const auto found = r.m_sketches.find(id);
                    if (found != r.m_sketches.end())
                        found->second.geometry = named.geometry;
                }
                r.m_profiles.clear();
                QVariantMap spec = a.value(QStringLiteral("spec")).toObject().toVariantMap();
                bool lost = false;
                for (const char *key : {"skip1", "skip2"}) {
                    QVariantList ids;
                    for (const QVariant &v : spec.value(QLatin1String(key)).toList()) {
                        const auto found = toR.find(v.toInt());
                        lost = lost || found == toR.end();
                        ids << (found == toR.end() ? 0 : found->second);
                    }
                    spec.insert(QLatin1String(key), ids);
                }
                if (lost) {
                    fail(QStringLiteral("a body left out of an extrude that can't be followed"));
                    continue;
                }
                const QVariantMap done = r.extrude(spec);
                if (!done.value(QStringLiteral("ok")).toBool()) {
                    fail(QStringLiteral("an extrude couldn't be made again: ") + done.value(QStringLiteral("why")).toString());
                    continue;
                }
                const QVariantList made = done.value(QStringLiteral("created")).toList();
                const QJsonArray was = a.value(QStringLiteral("created")).toArray();
                if (made.size() != was.size()) {
                    fail(QStringLiteral("an extrude made a different number of bodies"));
                    continue;
                }
                for (int k = 0; k < was.size(); ++k)
                    toR[was[k].toInt()] = made[k].toInt();
                const auto &swaps = r.m_extrudeKeep.at(r.m_extrudeToken - 1);
                for (const QJsonValue &v : a.value(QStringLiteral("swaps")).toArray()) {
                    const QJsonArray pair = v.toArray();
                    const auto from = toR.find(pair[0].toInt());
                    if (from == toR.end())
                        continue;
                    for (const auto &[old, now] : swaps)
                        if (old == from->second)
                            toR[pair[1].toInt()] = now;
                }
            }
            if (out->why.isEmpty() && changesBodies(a))
                snap(s.count, at);
        }
        // The solids the states use: the last state's bodies are the file's own; the others travel as bytes.
        out->finalBodies = r.m_bodies;
        if (!out->why.isEmpty())
            out->states.clear();
        std::set<int> seen;
        for (const auto &[c, b] : out->states)
            for (const auto &[id, named] : b.bodies)
                if (seen.insert(id).second) {
                    out->order.push_back(id);
                    if (!r.m_bodies.count(id))
                        out->solids[id] = {r.m_engine->saveBody(omamake::BodyId(id)), r.bodyMesh(id)};
                }
        const double seconds = clock.elapsed() / 1000.0;
        if (self.isNull())
            return;
        QMetaObject::invokeMethod(self.data(), [self, gen, out, seconds] {
            if (self.isNull() || self->m_buildGen != gen)
                return;
            qInfo().noquote() << "timeline: built again in" << seconds << "s;" << out->states.size() << "states,"
                              << out->solids.size() << "earlier solids to load" << (out->why.isEmpty() ? QString() : QStringLiteral("; stopped: ") + out->why);
            self->m_arrived = out;
            self->m_buildTimer->start();
        }, Qt::QueuedConnection);
    });
    connect(worker, &QThread::finished, worker, &QObject::deleteLater);
    worker->setPriority(QThread::LowPriority);
    worker->start();
    m_buildDone = false;
    emit buildChanged();
}

// The rebuild's solids loaded here a few ms at a time (never while an extrude or import has the engine); each
// state is handed over once every solid it uses is here, oldest first.
void Document::loadSome() {
    QElapsedTimer whole;
    whole.start();
    struct Took { QElapsedTimer &w; ~Took() { if (w.elapsed() > 8) qInfo().noquote() << "timeline: a load tick took" << w.elapsed() << "ms"; } } took{whole};
    if (!m_arrived) {
        m_buildTimer->stop();
        return;
    }
    if (m_importing || m_working)
        return;
    Arrived &a = *m_arrived;
    const auto stop = [this](const QString &why) {
        m_arrived.reset();
        m_buildTimer->stop();
        m_buildWhy = why;
        m_buildDone = true;
        qInfo().noquote() << "timeline: the opened file's design steps can't be built again:" << why;
        emit buildChanged();
    };
    if (!a.why.isEmpty())
        return stop(a.why);
    if (!a.matched) {
        // The bodies it ended with are the file's own: matched by name and component, all of them.
        if (a.finalBodies.size() != m_bodies.size())
            return stop(QStringLiteral("it ended with %1 bodies, the file has %2").arg(a.finalBodies.size()).arg(m_bodies.size()));
        for (const auto &[id, named] : a.finalBodies) {
            int here = 0;
            for (const auto &[mine, n] : m_bodies)
                if (n.name == named.name && n.component == named.component)
                    here = mine;
            if (!here)
                return stop(QStringLiteral("it made %1, which the file doesn't have").arg(named.name));
            a.toHere[id] = here;
        }
        a.matched = true;
    }
    QElapsedTimer clock;
    clock.start();
    while (a.next < a.order.size() && clock.elapsed() < 4) {
        const int id = a.order[a.next];
        if (!a.toHere.count(id)) {
            auto &solid = a.solids.at(id);
            const omamake::BodyId here = m_engine->loadBody(solid.bytes);
            if (!here)
                return stop(QStringLiteral("an earlier body couldn't be read back"));
            m_builtOwned.insert(int(here));
            m_meshCache[int(here)] = std::move(solid.mesh);
            a.toHere[id] = int(here);
            a.solids.erase(id);
        }
        ++a.next;
    }
    // Hand over the states whose solids are all here.
    const int was = m_builtTo;
    while (!a.states.empty()) {
        auto first = a.states.begin();
        Built &b = first->second;
        bool ready = true;
        for (const auto &[id, named] : b.bodies)
            ready = ready && a.toHere.count(id);
        if (!ready)
            break;
        const auto map = [&a](int id) { return a.toHere.count(id) ? a.toHere.at(id) : id; };
        Built mine;
        mine.at = b.at;
        mine.components = b.components;
        for (Component &c : mine.components)
            for (int &body : c.bodies)
                body = map(body);
        for (const auto &[id, named] : b.bodies)
            mine.bodies[map(id)] = named;
        for (const QString &h : b.hiddenBodies)
            mine.hiddenBodies.insert(QStringLiteral("b%1").arg(map(h.mid(1).toInt())));
        mine.nextBody = b.nextBody;
        mine.nextComponent = b.nextComponent;
        mine.body = omamake::BodyId(map(int(b.body)));
        const int count = first->first;
        m_built[count] = std::move(mine);
        a.states.erase(first);
        // Every history length up to the next state (or the file's end) now has its design.
        m_builtTo = a.states.empty() ? m_loaded : a.states.begin()->first - 1;
    }
    if (a.states.empty() && a.next >= a.order.size()) {
        m_arrived.reset();
        m_buildTimer->stop();
        m_buildDone = true;
        m_builtTo = m_loaded;
        qInfo().noquote() << "timeline: the opened file's design steps are all ready to go back over";
        emit buildChanged();
        return;
    }
    if (m_builtTo != was)
        emit buildChanged();
}

// The opened file's design as it was when the history was `count` long, if it's here and still the same.
const Document::Built *Document::builtAt(int count) const {
    if (count > m_builtTo || count < 0)
        return nullptr;
    auto it = m_built.upper_bound(count);
    if (it == m_built.begin())
        return nullptr;
    --it;
    if (it->first > 0 && (it->first > int(m_actions.size())
                          || m_actions[it->first - 1].toObject().value(QStringLiteral("at")).toString() != it->second.at))
        return nullptr;
    return &it->second;
}

// Taking back a step of the opened file that changed bodies: the bodies and components as they were (the sketches
// and everything else are taken back by their own steps). What goes is parked for Ctrl+Y, as any undo's.
void Document::applyBuilt(const Built &b) {
    for (const auto &[id, named] : std::map<int, Named>(m_bodies))
        if (!b.bodies.count(id))
            releaseBody(omamake::BodyId(id));
    std::vector<Component> components = b.components;
    for (Component &c : components)
        for (const Component &now : m_components)
            if (now.id == c.id) {
                c.sketches = now.sketches;
                c.nextSketch = now.nextSketch;
            }
    m_components = std::move(components);
    m_bodies = b.bodies;
    for (auto it = m_hidden.begin(); it != m_hidden.end();)
        it = it->startsWith(QLatin1Char('b')) ? m_hidden.erase(it) : std::next(it);
    m_hidden.insert(b.hiddenBodies.begin(), b.hiddenBodies.end());
    m_nextBody = b.nextBody;
    m_nextComponent = b.nextComponent;
    m_body = b.body;
    refreshMesh();
    emit changed();
    emit structureChanged();
}

// The states of a sketch for its Timeline row before this opening (the document stays where it is): from the
// background walk, newest first. Returns how many of the newest of `counts` are known (they are the last ones).
int Document::prepareSketchPast(int sketch, const QVariantList &counts) {
    syncWalk();
    m_pastSketch = sketch;
    m_past.assign(std::size_t(counts.size()), SketchGeometry{});
    m_pastFrom = int(counts.size());
    for (int k = int(counts.size()) - 1; k >= 0; --k) {
        if (!sketchAt(sketch, counts[k].toInt(), m_past[std::size_t(k)]))
            break;
        m_pastFrom = k;
    }
    return int(counts.size()) - m_pastFrom;
}

bool Document::showSketchPast(int sketch, int k, double t) {
    const auto now = m_sketches.find(sketch);
    if (k < m_pastFrom || sketch != m_pastSketch || std::size_t(k) >= m_past.size() || now == m_sketches.end()) {
        if (m_tween && m_tween->actions < 0)
            m_tween.reset();
        return false;
    }
    // Resting on a state (t 0): exactly it. Part way: towards the next (the last: as it is now).
    const bool between = t > 0;
    const int key = -1 - 2 * k - (between ? 1 : 0);          // below 0: a past state (the document doesn't move)
    if (!m_tween || m_tween->sketch != sketch || m_tween->actions != key) {
        SketchTween w;
        w.sketch = sketch;
        w.actions = key;
        w.grow = true;
        w.from = m_past[std::size_t(k)];
        w.to = !between ? w.from : std::size_t(k) + 1 < m_past.size() ? m_past[std::size_t(k) + 1] : now->second.geometry;
        w.heldFrom = w.from.fullyConstrained();
        w.heldTo = w.to.fullyConstrained();
        w.sameShape = sameShape(w.from, w.to);
        m_tween = std::move(w);
    }
    m_tween->t = std::clamp(t, 0.0, 1.0);
    blendTween();
    return true;
}

void Document::noteLiveMove() {
    const auto open = m_sketches.find(m_openSketch);
    if (!m_openSketch || open == m_sketches.end()) {
        m_liveSketch = 0;
        if (m_tween && !m_tween->actions)
            m_tween.reset();
        return;
    }
    const SketchGeometry &now = open->second.geometry;
    // The mouse moving it (a drag, text slid along its path) is followed at once; so is the marker.
    const bool mouse = !m_dragBefore.empty() || m_slide.on;
    if (m_liveSketch == m_openSketch && !mouse && !m_liveHeld && !(m_tween && m_tween->actions)) {
        // Starts from what's on show (part way through a glide still running: from there).
        const SketchGeometry &shown = m_tween && m_tween->sketch == m_openSketch ? m_tween->shown : m_liveLast;
        bool moved = false;
        for (const auto &[id, p] : now.points) {
            const auto was = shown.points.find(id);
            if (was != shown.points.end() && (std::abs(was->second.u - p.u) > 1e-7 || std::abs(was->second.v - p.v) > 1e-7)) {
                moved = true;
                break;
            }
        }
        if (moved) {
            SketchTween w;
            w.sketch = m_openSketch;
            w.from = shown;
            w.to = now;
            w.heldFrom = w.from.fullyConstrained();
            w.heldTo = w.to.fullyConstrained();
            w.sameShape = sameShape(w.from, w.to);
            w.t = 0;
            m_tween = std::move(w);
            blendTween();
            m_liveWaiting = true;
        } else if (m_tween && !m_tween->actions) {
            m_tween.reset();
            m_liveWaiting = false;
        }
    } else if (m_tween && !m_tween->actions) {
        m_tween.reset();
        m_liveWaiting = false;
    }
    m_liveSketch = m_openSketch;
    m_liveLast = now;
}

bool Document::takeLiveTween() {
    const bool waiting = m_liveWaiting && m_tween && !m_tween->actions;
    m_liveWaiting = false;
    return waiting;
}

void Document::liveTween(double t) {
    if (!m_tween || m_tween->actions)
        return;
    if (t >= 1) {
        m_tween.reset();
        return;
    }
    m_tween->t = t;
    blendTween();
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
    return markVisible(componentNode(m_components[0]), true, futureKeys());
}

bool Document::rewinds(int sketch) const {
    return !futureKeys(sketch == 0 ? -1 : sketch).empty();
}

std::set<QString> Document::futureKeys(int sketch) const {
    std::set<QString> out;
    if (sketch < 0)
        sketch = m_openSketch;
    if (!sketch)
        return out;
    // The row each action made, and where in the history it was made.
    int opened = -1;
    std::map<QString, int> born;
    for (int i = 0; i < m_actions.size(); ++i) {
        const QJsonObject a = m_actions[i].toObject();
        const QString type = a.value(QStringLiteral("type")).toString();
        if (type == QLatin1String("sketch")) {
            const QString key = QStringLiteral("s%1").arg(a.value(QStringLiteral("id")).toInt());
            born[key] = i;
            if (a.value(QStringLiteral("id")).toInt() == sketch)
                opened = i;
        } else if (type == QLatin1String("component")) {
            born[QStringLiteral("c%1").arg(a.value(QStringLiteral("id")).toInt())] = i;
        } else if (type == QLatin1String("extrude")) {
            for (const QJsonValue &b : a.value(QStringLiteral("created")).toArray())
                born[QStringLiteral("b%1").arg(b.toInt())] = i;
            for (const QJsonValue &c : a.value(QStringLiteral("components")).toArray())
                born[QStringLiteral("c%1").arg(c.toInt())] = i;
        } else if ((type == QLatin1String("import_step") || type == QLatin1String("import_mesh") || type == QLatin1String("cube"))
                   && m_body) {
            born[QStringLiteral("b%1").arg(a.contains(QStringLiteral("body")) ? a.value(QStringLiteral("body")).toInt() : int(m_body))] = i;
        }
    }
    if (opened < 0)
        return out;
    for (const auto &[key, at] : born)
        if (at > opened)
            out.insert(key);
    return out;
}

bool Document::bodyIsFuture() const {
    if (m_bodies.empty())
        return false;
    const std::set<QString> later = futureKeys();
    for (const auto &entry : m_bodies)
        if (!later.count(QStringLiteral("b%1").arg(entry.first)))
            return false;
    return true;      // every body was made after the open sketch
}

QVariantMap Document::markVisible(QVariantMap node, bool parentVisible, const std::set<QString> &future) const {
    const QString key = node.value(QStringLiteral("key")).toString();
    const bool later = future.count(key) > 0;
    const bool shown = key == QLatin1String("c0") || (!m_hidden.count(key) && !later);
    const bool visible = parentVisible && shown;
    node.insert(QStringLiteral("shown"), shown);
    node.insert(QStringLiteral("visible"), visible);
    node.insert(QStringLiteral("future"), later);       // made after the sketch being edited
    QVariantList children;
    for (const QVariant &child : node.value(QStringLiteral("children")).toList())
        children << markVisible(child.toMap(), visible, future);
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
    // Hiding something and showing it again straight away (or the reverse), with nothing else done
    // in between, was only a look, not an edit: neither goes in the history (owner, 2026-11-02).
    if (!m_actions.isEmpty()) {
        const QJsonObject last = m_actions.last().toObject();
        if (last.value(QStringLiteral("type")).toString() == QLatin1String("visibility")
            && last.value(QStringLiteral("key")).toString() == key
            && last.value(QStringLiteral("shown")).toBool() != shown
            && last.value(QStringLiteral("in_sketch")).toInt() == m_openSketch) {
            m_actions.removeLast();
            m_lowSeen = std::min(m_lowSeen, int(m_actions.size()));
            if (m_runStart > int(m_actions.size()))
                m_runStart = int(m_actions.size());
            qInfo().noquote() << "history: hide and show cancelled each other:" << name;
            emit historyChanged();
            emit structureChanged();
            return;
        }
    }
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

// A step undone from a whole copy of the sketch kept in memory (`snapshot`) also keeps what it changed in its record
// (`before`, a sketchDiff), so it can still be taken back, and shown on the Timeline, after the file is opened again.
void Document::keepBefore(QJsonObject &entry) const {
    if (!entry.contains(QStringLiteral("snapshot")))
        return;
    const auto kept = m_snapshots.find(entry.value(QStringLiteral("snapshot")).toInt());
    const auto now = m_sketches.find(entry.value(QStringLiteral("sketch")).toInt());
    if (kept != m_snapshots.end() && now != m_sketches.end())
        entry.insert(QStringLiteral("before"), sketchDiff(kept->second, now->second.geometry));
}

void Document::record(const QJsonObject &action, const QString &summary) {
    clearRedo();           // something new: what was taken back can't be put back any more
    if (!m_future.empty())  // made while the marker is back: the steps behind it now need building again
        m_staleAt = m_staleAt ? std::min(m_staleAt, int(m_actions.size()) + 1) : int(m_actions.size()) + 1;
    m_textEditItem = -1;   // anything recorded ends the run of edits of one text (see editSketchText)
    QJsonObject entry = action;
    // Done inside a sketch: that sketch's own history (its own row in the Timeline). Starting,
    // finishing and editing it belong to the design's history, where the sketch is one point.
    const QString type = action.value(QStringLiteral("type")).toString();
    if (m_openSketch && type != QLatin1String("sketch") && type != QLatin1String("finish_sketch")
        && type != QLatin1String("edit_sketch") && type != QLatin1String("parameters"))
        entry.insert(QStringLiteral("in_sketch"), m_openSketch);
    entry.insert(QStringLiteral("at"), QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
    entry.insert(QStringLiteral("summary"), summary);
    // A Project run is ONE history item from its first projection on (owner, 2026-11-02, t36u):
    // the later projections of the run are added to that item, never made as steps of their own.
    // The item keeps the snapshot of the state before the run, so one undo takes the whole run back.
    const bool runProjection = m_runStart >= 0 && type == QLatin1String("shape")
        && entry.contains(QStringLiteral("link")) && entry.contains(QStringLiteral("snapshot"))
        && !entry.contains(QStringLiteral("break_link")) && summary.startsWith(QLatin1String("Projected"))
        && entry.value(QStringLiteral("sketch")).toInt() == m_runSketch;
    // Anything else done while a run's item is waiting for Done commits it first (it was made before).
    if (!runProjection && m_runStart >= 0 && m_runStart < int(m_actions.size())) {
        QJsonObject waiting = m_actions[m_runStart].toObject();
        if (waiting.contains(QStringLiteral("pending"))) {
            waiting.remove(QStringLiteral("pending"));
            m_actions[m_runStart] = waiting;
        }
    }
    if (runProjection && m_runStart < int(m_actions.size()) - 1)   // something came after the item: a new one
        m_runStart = int(m_actions.size());
    if (runProjection && m_runStart == int(m_actions.size()))
        entry.insert(QStringLiteral("pending"), true);   // the run's item: in the history only from Done on
    if (runProjection && m_runStart < int(m_actions.size())) {
        QJsonObject run = m_actions[m_runStart].toObject();
        if (run.value(QStringLiteral("type")).toString() == QLatin1String("shape")
            && run.value(QStringLiteral("sketch")).toInt() == m_runSketch && run.contains(QStringLiteral("snapshot"))
            && !run.contains(QStringLiteral("break_link"))
            && run.value(QStringLiteral("summary")).toString().startsWith(QLatin1String("Projected"))) {
            QJsonArray links = run.value(QStringLiteral("links")).toArray(), parts = run.value(QStringLiteral("parts")).toArray();
            if (links.isEmpty())
                links.append(run.value(QStringLiteral("link")).toInt());
            if (parts.isEmpty())
                parts.append(run.value(QStringLiteral("summary")).toString());
            links.append(entry.value(QStringLiteral("link")).toInt());
            parts.append(summary);
            m_snapshots.erase(entry.value(QStringLiteral("snapshot")).toInt());   // the first one is the run's
            run.insert(QStringLiteral("links"), links);
            run.insert(QStringLiteral("parts"), parts);
            run.insert(QStringLiteral("summary"), QStringLiteral("Projected %1 items onto the sketch").arg(parts.size()));
            keepBefore(run);
            m_dirtyFrom = std::min(m_dirtyFrom, m_runStart);
            m_actions[m_runStart] = run;
            qInfo().noquote() << "history: added to the Project run:" << summary;
            emit historyChanged();
            return;
        }
    }
    keepBefore(entry);
    if (!m_background)
        qInfo().noquote() << "history:" << (entry.contains(QStringLiteral("in_sketch"))
                                            ? QStringLiteral("[in %1] %2").arg(m_sketches[m_openSketch].name, summary) : summary);
    m_actions.append(entry);
    emit historyChanged();
}

int Document::addBody(omamake::BodyId solid, int component, const QString &name) {
    const int id = int(solid);
    m_bodies[id] = {component, name.isEmpty() ? QStringLiteral("Body%1").arg(m_nextBody++) : name, {}, {}};
    for (Component &c : m_components)
        if (c.id == component)
            c.bodies.push_back(id);
    m_body = solid;
    return id;
}

void Document::dropBody(int id) {
    for (Component &c : m_components)
        c.bodies.erase(std::remove(c.bodies.begin(), c.bodies.end(), id), c.bodies.end());
    m_bodies.erase(id);
    m_hidden.erase(QStringLiteral("b%1").arg(id));
    releaseBody(omamake::BodyId(id));
    if (int(m_body) == id)
        m_body = m_bodies.empty() ? 0 : omamake::BodyId(m_bodies.rbegin()->first);
}

void Document::showBodies(const QVariantList &ids) {
    std::set<int> now;
    for (const QVariant &v : ids)
        now.insert(v.toInt());
    if (m_shownSet && now == m_shown)
        return;
    m_shown = now;
    m_shownSet = true;
    refreshMesh();
    emit changed();
}

int Document::bodyOfPart(const QString &kind, int index) const {
    const std::vector<int> *map = kind == QLatin1String("face") ? &m_faceBody
                                : kind == QLatin1String("edge") ? &m_edgeBody
                                : kind == QLatin1String("point") ? &m_pointBody : nullptr;
    if (!map || index < 0 || std::size_t(index) >= map->size())
        return 0;
    return (*map)[std::size_t(index)];
}

void Document::refreshMesh() {
    if (!m_bodies.count(int(m_body)))
        m_body = m_bodies.empty() ? 0 : omamake::BodyId(m_bodies.rbegin()->first);
    // Every shown body's mesh, joined end to end (faces, edges and corners numbered on through).
    m_mesh = omamake::Mesh{};
    m_faceBody.clear();
    m_edgeBody.clear();
    m_pointBody.clear();
    bool first = true;
    for (const auto &entry : m_bodies) {
        if (m_shownSet && !m_shown.count(entry.first))
            continue;
        auto cached = m_meshCache.find(entry.first);
        if (cached == m_meshCache.end())
            cached = m_meshCache.emplace(entry.first, m_engine->mesh(omamake::BodyId(entry.first))).first;
        const omamake::Mesh &one = cached->second;
        const auto vertexBase = std::uint32_t(m_mesh.positions.size() / 3);
        const std::uint32_t faceBase = m_mesh.faceCount;
        m_mesh.positions.insert(m_mesh.positions.end(), one.positions.begin(), one.positions.end());
        m_mesh.normals.insert(m_mesh.normals.end(), one.normals.begin(), one.normals.end());
        for (std::uint32_t i : one.indices)
            m_mesh.indices.push_back(i + vertexBase);
        m_mesh.edgeLines.insert(m_mesh.edgeLines.end(), one.edgeLines.begin(), one.edgeLines.end());
        for (std::uint32_t f : one.triangleFace)
            m_mesh.triangleFace.push_back(f + faceBase);
        m_mesh.faceCount += one.faceCount;
        m_mesh.edges.insert(m_mesh.edges.end(), one.edges.begin(), one.edges.end());
        m_mesh.points.insert(m_mesh.points.end(), one.points.begin(), one.points.end());
        m_faceBody.insert(m_faceBody.end(), one.faceCount, entry.first);
        m_edgeBody.insert(m_edgeBody.end(), one.edges.size(), entry.first);
        m_pointBody.insert(m_pointBody.end(), one.points.size() / 3, entry.first);
        for (int k = 0; k < 3; ++k) {
            m_mesh.min[k] = first ? one.min[k] : std::min(m_mesh.min[k], one.min[k]);
            m_mesh.max[k] = first ? one.max[k] : std::max(m_mesh.max[k], one.max[k]);
        }
        first = false;
    }
    m_bvh.clear();
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

void addTube(omamake::Mesh &m, const QVector3D &a, const QVector3D &b, float r, int sides = 8) {
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

void addBall(omamake::Mesh &m, const QVector3D &c, float r, int rings = 6, int segments = 10) {
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

QVariantMap Document::pick(QVector3D origin, QVector3D direction, float pointReach, float edgeReach, bool through) const {
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
    auto visible = [&](float t, float reach) { return through || t <= hitT + reach * 2; };

    // depth: how far along the ray the nearest surface is (none: -1), so the origin's
    // planes (Selection.qml) can tell whether they're in front of the model or behind it.
    auto result = [&](const char *kind, int index) {
        return QVariantMap{{QStringLiteral("kind"), QString::fromLatin1(kind)}, {QStringLiteral("index"), index},
                           {QStringLiteral("body"), bodyOfPart(QString::fromLatin1(kind), index)},
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

    // The nearest to the pointer wins; edges about as near (an edge exactly behind another, seen
    // end-on, with Select Through on) go to the one nearer the camera (owner, 2026-11-02).
    float bestGap = INFINITY, bestEdgeT = INFINITY;
    for (std::size_t e = 0; e < m_mesh.edges.size(); ++e) {
        const auto &line = m_mesh.edges[e];
        for (std::size_t i = 0; i + 1 < line.size() / 3; ++i) {
            const auto [t, gap] = raySegment(origin, d, at(line, i), at(line, i + 1));
            const float reach = std::max(t, 1.0f) * edgeReach;
            if (gap > reach || !visible(t, reach))
                continue;
            const float score = gap / reach;
            if (score < bestGap - 0.05f || (score < bestGap + 0.05f && t < bestEdgeT)) {
                best = int(e);
                bestGap = std::min(bestGap, score);
                bestEdgeT = t;
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
            const int bodyId = k.mid(1).toInt();
            if (m_bodies.count(bodyId)) {
                body = true;
                for (std::size_t t = 0; t < m_mesh.indices.size(); ++t)
                    if (m_faceBody[m_mesh.triangleFace[t / 3]] == bodyId)
                        points.push_back(at(m_mesh.positions, m_mesh.indices[t]));
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
        if (n.value(QStringLiteral("key")).toString().startsWith(QLatin1Char('b')) && n.value(QStringLiteral("visible")).toBool())
            bodySeen = true;
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
    if (area <= 0)
        return plane;
    QVector3D normal = sum.normalized();
    center /= area;
    // The engine's own plane when it has one: the triangles of a converted body's face can sit a
    // hair off it (0.0001 mm on the speaker mount's base), and a sketch placed on them then left a
    // seam where its extrude met the face (owner, 2026-11-03). Tiny triangles along a merged face's
    // edge can also tip a little, and the face is still flat.
    {
        const int body = m_faceBody[std::size_t(face)];
        const int first = int(std::find(m_faceBody.begin(), m_faceBody.end(), body) - m_faceBody.begin());
        double o[3], n[3];
        if (m_engine->planeOfFace(omamake::BodyId(body), face - first, o, n)) {
            const QVector3D exact{float(n[0]), float(n[1]), float(n[2])};
            if (QVector3D::dotProduct(exact, normal) > 0.99f) {
                flat = true;
                normal = exact;
                const QVector3D on{float(o[0]), float(o[1]), float(o[2])};
                center -= normal * QVector3D::dotProduct(center - on, normal);
                // The same plane at full precision, for whatever is measured or built from it.
                const double away = o[0] * n[0] + o[1] * n[1] + o[2] * n[2];
                const double up[3] = {0, std::abs(n[2]) < 0.9 ? 0.0 : 1.0, std::abs(n[2]) < 0.9 ? 1.0 : 0.0};
                const double along = up[0] * n[0] + up[1] * n[1] + up[2] * n[2];
                double y[3] = {up[0] - n[0] * along, up[1] - n[1] * along, up[2] - n[2] * along};
                const double ly = std::sqrt(y[0] * y[0] + y[1] * y[1] + y[2] * y[2]);
                for (double &c : y)
                    c /= ly;
                const double x[3] = {y[1] * n[2] - y[2] * n[1], y[2] * n[0] - y[0] * n[2], y[0] * n[1] - y[1] * n[0]};
                plane.insert(QStringLiteral("exact"), QVariantList{n[0] * away, n[1] * away, n[2] * away, x[0], x[1], x[2], y[0], y[1], y[2]});
            }
        }
    }
    if (!flat)
        return plane;

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

void Document::fillParts(MeshGeometry *target, const QVariantList &parts, float edgeRadius, QVector3D eye, float perMm, bool lean) const {
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
        } else if (kind == QLatin1String("body")) {         // a whole body: all of its faces
            for (std::uint32_t f = 0; f < m_mesh.faceCount; ++f)
                if (m_faceBody[f] == index)
                    faces.insert(f);
        } else if (kind == QLatin1String("edge") && index >= 0 && std::size_t(index) < m_mesh.edges.size()) {
            const auto &line = m_mesh.edges[std::size_t(index)];
            // lean (the Project highlight, hundreds of edges): 5-sided tubes, a ball only at the two ends.
            const std::size_t last = line.size() / 3 - 1;
            for (std::size_t k = 0; k + 1 < line.size() / 3; ++k) {
                const float r = radiusAt(at(line, k + 1));
                if (lean) {
                    addTube(out, at(line, k), at(line, k + 1), r, 5);
                    if (k == 0)
                        addBall(out, at(line, k), r, 3, 6);
                    if (k + 1 == last)
                        addBall(out, at(line, k + 1), r, 3, 6);
                } else {
                    addTube(out, at(line, k), at(line, k + 1), r);
                    addBall(out, at(line, k + 1), r);   // smooth joints
                }
            }
        } else if (kind == QLatin1String("point") && index >= 0 && std::size_t(index) < m_mesh.points.size() / 3) {
            addBall(out, at(m_mesh.points, std::size_t(index)), radiusAt(at(m_mesh.points, std::size_t(index))) * 2.4f,
                    lean ? 4 : 6, lean ? 8 : 10);
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
                auto paths = kind == QLatin1String("sketch_arc") ? g.arcPath(index) : g.curvePath(index);
                if (kind == QLatin1String("sketch_curve"))
                    if (auto frame = g.textFrame(index); !frame.empty())
                        paths.push_back(frame);
                for (const auto &path : paths)
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
void Document::forgetSketchLines(SketchGeometry &g, const QJsonObject &action) {
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
        QVariantList ids, along;
        for (int code : k.path)
            along << code;
        for (int p : k.points)
            ids << p;
        curves << QVariantMap{{QStringLiteral("id"), id}, {QStringLiteral("type"), k.type}, {QStringLiteral("points"), ids},
                              {QStringLiteral("value"), k.value}, {QStringLiteral("text"), k.text},
                              {QStringLiteral("font"), k.font}, {QStringLiteral("flags"), k.flags},
                              {QStringLiteral("path"), along}, {QStringLiteral("anchor"), k.anchor},
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
                                   {QStringLiteral("follows"), c.follows()}, {QStringLiteral("driven"), c.driven},
                                   {QStringLiteral("joint"), c.type == QLatin1String("curvature") && g.curves.count(c.first)
                                        ? (c.u > 0.5 ? g.curves.at(c.first).points.back() : g.curves.at(c.first).points.front()) : 0}};
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

// The traced outline put on the body's exact edges (owner, 2026-11-04: a projected face must be exactly
// that face, or an extrude from it is never the same surface as the wall beside it; FreeCAD projects
// the edges themselves, SketchObjectExternal.cpp processEdge). Each traced piece takes the exact line or
// circle it runs along, within `tol`. Where two pieces meet, the end goes to the body's corner there if
// it lies on both, else to where the two exact curves cross, or onto the one curve both run along. A
// piece whose ends can't both go on its curve keeps what was traced, and its neighbours meet it there.
static void snapToExact(std::vector<std::vector<projection::Piece>> &loops, const std::vector<omamake::FlatCurve> &exact,
                        const std::vector<omamake::Point2> &corners, double tol) {
    using projection::Flat;
    using projection::Piece;
    if (exact.empty())
        return;
    const auto off = [&](const omamake::FlatCurve &e, const Flat &p) {
        if (e.round)
            return std::abs(std::hypot(p.u - e.c.x, p.v - e.c.y) - e.r);
        const double dx = e.b.x - e.a.x, dy = e.b.y - e.a.y, l = std::hypot(dx, dy);
        return std::abs((p.u - e.a.x) * dy - (p.v - e.a.y) * dx) / l;
    };
    const auto onto = [&](const omamake::FlatCurve &e, const Flat &p) {
        if (e.round) {
            const double du = p.u - e.c.x, dv = p.v - e.c.y, l = std::hypot(du, dv);
            return Flat{e.c.x + du / l * e.r, e.c.y + dv / l * e.r};
        }
        const double dx = e.b.x - e.a.x, dy = e.b.y - e.a.y, s = ((p.u - e.a.x) * dx + (p.v - e.a.y) * dy) / (dx * dx + dy * dy);
        return Flat{e.a.x + dx * s, e.a.y + dy * s};
    };
    const auto same = [](const omamake::FlatCurve &e, const omamake::FlatCurve &f) {
        if (e.round != f.round)
            return false;
        if (e.round)
            return std::hypot(e.c.x - f.c.x, e.c.y - f.c.y) < 1e-9 && std::abs(e.r - f.r) < 1e-9;
        const double dx = e.b.x - e.a.x, dy = e.b.y - e.a.y, l = std::hypot(dx, dy);
        const double fx = f.b.x - f.a.x, fy = f.b.y - f.a.y, m = std::hypot(fx, fy);
        return std::abs(dx / l * fy / m - dy / l * fx / m) < 1e-12 && std::abs((f.a.x - e.a.x) * dy - (f.a.y - e.a.y) * dx) / l < 1e-9;
    };
    // Where two exact curves cross, nearest p.
    const auto cross = [&](const omamake::FlatCurve &e, const omamake::FlatCurve &f, const Flat &p, Flat &out) {
        std::vector<Flat> at;
        if (!e.round && !f.round) {
            const double ax = e.b.x - e.a.x, ay = e.b.y - e.a.y, bx = f.b.x - f.a.x, by = f.b.y - f.a.y;
            const double d = ax * by - ay * bx;
            if (std::abs(d) < 1e-15)
                return false;
            const double s = ((f.a.x - e.a.x) * by - (f.a.y - e.a.y) * bx) / d;
            at.push_back({e.a.x + ax * s, e.a.y + ay * s});
        } else if (e.round != f.round) {
            const omamake::FlatCurve &l = e.round ? f : e, &c = e.round ? e : f;
            const double dx = l.b.x - l.a.x, dy = l.b.y - l.a.y, len = std::hypot(dx, dy), ux = dx / len, uy = dy / len;
            const double s0 = (c.c.x - l.a.x) * ux + (c.c.y - l.a.y) * uy;
            const double hx = l.a.x + ux * s0 - c.c.x, hy = l.a.y + uy * s0 - c.c.y, h2 = hx * hx + hy * hy;
            const double left = c.r * c.r - h2;
            if (left < -tol * tol)
                return false;
            const double half = std::sqrt(std::max(0.0, left));
            at.push_back({l.a.x + ux * (s0 - half), l.a.y + uy * (s0 - half)});
            at.push_back({l.a.x + ux * (s0 + half), l.a.y + uy * (s0 + half)});
        } else {
            const double dx = f.c.x - e.c.x, dy = f.c.y - e.c.y, d = std::hypot(dx, dy);
            if (d < 1e-15)
                return false;
            const double a = (e.r * e.r - f.r * f.r + d * d) / (2 * d), h2 = e.r * e.r - a * a;
            if (h2 < -tol * tol)
                return false;
            const double h = std::sqrt(std::max(0.0, h2)), mx = e.c.x + dx * a / d, my = e.c.y + dy * a / d;
            at.push_back({mx - dy * h / d, my + dx * h / d});
            at.push_back({mx + dy * h / d, my - dx * h / d});
        }
        double best = INFINITY;
        for (const Flat &q : at)
            if (const double d = std::hypot(q.u - p.u, q.v - p.v); d < best) {
                best = d;
                out = q;
            }
        return best <= tol;
    };

    for (auto &loop : loops) {
        const std::size_t n = loop.size();
        std::vector<int> on(n, -1);
        for (std::size_t i = 0; i < n; ++i) {
            const Piece &p = loop[i];
            double best = tol;
            for (std::size_t k = 0; k < exact.size(); ++k) {
                const omamake::FlatCurve &e = exact[k];
                double miss;
                if (p.kind == Piece::Line) {
                    if (e.round)
                        continue;
                    miss = std::max(off(e, p.a), off(e, p.b));
                } else {
                    if (!e.round)
                        continue;
                    miss = std::max(std::hypot(p.c.u - e.c.x, p.c.v - e.c.y), std::abs(std::hypot(p.a.u - p.c.u, p.a.v - p.c.v) - e.r));
                }
                if (miss <= best) {
                    best = miss;
                    on[i] = int(k);
                }
            }
        }
        if (n == 1 && loop[0].kind == Piece::Circle) {
            if (on[0] >= 0) {
                const omamake::FlatCurve &e = exact[std::size_t(on[0])];
                loop[0].c = {e.c.x, e.c.y};
                loop[0].a = loop[0].b = onto(e, loop[0].a);
            }
            continue;
        }
        // Each end where piece i meets piece i+1, settled again until nothing more gives way. A piece
        // that is only traced may bend its end onto an exact curve only if it is a straight line.
        std::vector<Flat> meet(n);
        for (bool changed = true; changed;) {
            changed = false;
            for (std::size_t i = 0; i < n; ++i) {
                const std::size_t k = (i + 1) % n;
                const Flat p = loop[i].b;
                const int ei = on[i], ek = on[k];
                meet[i] = p;
                if (ei < 0 && ek < 0)
                    continue;
                const bool bends = (ei >= 0 || loop[i].kind == Piece::Line) && (ek >= 0 || loop[k].kind == Piece::Line);
                bool done = false;
                double nearest = tol;
                for (const omamake::Point2 &c : corners) {
                    const Flat q{c.x, c.y};
                    const double d = std::hypot(q.u - p.u, q.v - p.v);
                    if (d <= nearest && (ei < 0 || off(exact[std::size_t(ei)], q) < 1e-9) && (ek < 0 || off(exact[std::size_t(ek)], q) < 1e-9)) {
                        nearest = d;
                        meet[i] = q;
                        done = true;
                    }
                }
                if (!done && ei >= 0 && ek >= 0) {
                    const omamake::FlatCurve &e = exact[std::size_t(ei)], &f = exact[std::size_t(ek)];
                    if (same(e, f)) {
                        meet[i] = onto(e, p);
                        done = true;
                    } else {
                        done = cross(e, f, p, meet[i]);
                    }
                } else if (!done) {
                    meet[i] = onto(exact[std::size_t(ei >= 0 ? ei : ek)], p);
                    done = true;
                }
                if (done && bends)
                    continue;
                on[i] = on[k] = -1;
                meet[i] = p;
                changed = true;
            }
        }
        for (std::size_t i = 0; i < n; ++i) {
            loop[i].b = meet[i];
            loop[(i + 1) % n].a = meet[i];
            if (on[i] >= 0 && loop[i].kind != Piece::Line) {
                const omamake::FlatCurve &e = exact[std::size_t(on[i])];
                loop[i].c = {e.c.x, e.c.y};
            }
        }
    }
}

// The outline of triangles (already flat on a sketch) put onto the sketch: the lines, arcs and
// circles it adds are counted; returns why nothing went on, or empty. Shared by one face and a batch.
static QString putOutlineOnSketch(SketchGeometry &g, const std::vector<std::array<projection::Flat, 3>> &triangles, double size,
                                  int &lines, int &arcs, int &circles, const std::vector<omamake::FlatCurve> &exact = {},
                                  const std::vector<omamake::Point2> &corners = {}) {
    const double tol = std::max(0.012, size / 25000);   // twice how far the drawn triangles may stray
    std::vector<std::vector<projection::Piece>> loops = projection::outline(triangles, tol);
    snapToExact(loops, exact, corners, 2 * tol);
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
            return QStringLiteral("That face throws no outline on this sketch.");
        loops.push_back({projection::Piece{projection::Piece::Line, a, b, {}}});
    }

    // A corner two pieces share, or an earlier projection's, is one point.
    std::map<std::pair<long long, long long>, int> known;
    const auto cell = [](const projection::Flat &p) { return std::pair{std::llround(p.u * 1e5), std::llround(p.v * 1e5)}; };
    for (const int id : g.projectedPoints)
        if (g.points.count(id))
            known[cell({g.points[id].u, g.points[id].v})] = id;
    const int firstPoint = g.nextPoint;
    std::vector<int> madePoints;
    const auto pointAt = [&](const projection::Flat &p) {
        const auto [it, fresh] = known.try_emplace(cell(p), 0);
        if (fresh) {
            it->second = g.nextPoint++;
            madePoints.push_back(it->second);
            g.points[it->second] = {p.u, p.v};
            g.projectedPoints.insert(it->second);
        }
        return it->second;
    };
    lines = arcs = circles = 0;
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
    if (!lines && !arcs && !circles) {              // nothing new: take back the points made for it
        for (const int id : madePoints) {
            g.points.erase(id);
            g.projectedPoints.erase(id);
            g.helperPoints.erase(id);
        }
        g.nextPoint = firstPoint;
        return QStringLiteral("That's already projected.");
    }
    return {};
}

// Exact pieces put on the sketch (owner, 2026-11-06: Project must give precisely what the model has).
// A corner two pieces share, or an earlier projection's, is one point. Counts what went on; returns why
// nothing did, or empty.
struct PutCounts {
    int points = 0, lines = 0, arcs = 0, circles = 0, curves = 0;
    bool any() const { return points || lines || arcs || circles || curves; }
};
static QString putPiecesOnSketch(SketchGeometry &g, const std::vector<std::vector<omamake::FlatPiece>> &loops, PutCounts &n) {
    using omamake::FlatPiece;
    std::map<std::pair<long long, long long>, int> known;
    const auto cell = [](const omamake::Point2 &p) { return std::pair{std::llround(p.x * 1e7), std::llround(p.y * 1e7)}; };
    for (const int id : g.projectedPoints)
        if (g.points.count(id))
            known[cell({g.points[id].u, g.points[id].v})] = id;
    const int firstPoint = g.nextPoint;
    std::vector<int> madePoints;
    const auto pointAt = [&](const omamake::Point2 &p, bool helper = false) {
        const auto [it, fresh] = known.try_emplace(cell(p), 0);
        if (fresh) {
            it->second = g.nextPoint++;
            madePoints.push_back(it->second);
            g.points[it->second] = {p.x, p.y};
            g.projectedPoints.insert(it->second);
            if (helper)
                g.helperPoints.insert(it->second);
        } else if (!helper) {
            g.helperPoints.erase(it->second);       // a corner too now: shown
        }
        return it->second;
    };
    n = {};
    for (const auto &loop : loops)
        for (const FlatPiece &piece : loop) {
            switch (piece.kind) {
            case FlatPiece::Point: {
                const std::size_t had = g.points.size();
                pointAt(piece.a);
                n.points += int(g.points.size() - had);
                break;
            }
            case FlatPiece::Line: {
                const int a = pointAt(piece.a), b = pointAt(piece.b);
                if (a == b || std::any_of(g.lines.begin(), g.lines.end(), [&](const auto &l) {
                        return (l.second.a == a && l.second.b == b) || (l.second.a == b && l.second.b == a); }))
                    break;
                g.lines[g.nextLine++] = {a, b};
                ++n.lines;
                break;
            }
            case FlatPiece::Arc:
            case FlatPiece::Circle: {
                const bool circle = piece.kind == FlatPiece::Circle;
                const int c = pointAt(piece.c, !circle);    // only a circle's centre is worth showing
                const int a = pointAt(piece.a, circle);     // a circle's rim point is never shown
                const int b = circle ? a : pointAt(piece.b);
                if ((!circle && a == b) || std::any_of(g.arcs.begin(), g.arcs.end(), [&](const auto &k) {
                        return k.second.c == c && k.second.a == a && k.second.b == b; }))
                    break;
                g.arcs[g.nextArc++] = {c, a, b};
                ++(circle ? n.circles : n.arcs);
                break;
            }
            case FlatPiece::Ellipse:
            case FlatPiece::Spline: {
                SketchGeometry::Curve k;
                if (piece.kind == FlatPiece::Ellipse) {
                    k.type = QStringLiteral("ellipse");
                    k.points = {pointAt(piece.c), pointAt(piece.major, true), pointAt(piece.minor, true)};
                } else {
                    k.type = QStringLiteral("controlspline");
                    for (std::size_t i = 0; i < piece.poles.size(); ++i)
                        k.points.push_back(pointAt(piece.poles[i], i != 0 && i + 1 != piece.poles.size()));
                }
                const auto same = [&](const auto &o) {
                    const auto &q = o.second.points;
                    return o.second.type == k.type && (q == k.points || std::equal(q.begin(), q.end(), k.points.rbegin(), k.points.rend()));
                };
                if (std::any_of(g.curves.begin(), g.curves.end(), same))
                    break;
                g.curves[g.nextCurve++] = k;
                ++n.curves;
                break;
            }
            }
        }
    if (!n.any()) {             // nothing new: take back the points made for it
        for (const int id : madePoints) {
            g.points.erase(id);
            g.projectedPoints.erase(id);
            g.helperPoints.erase(id);
        }
        g.nextPoint = firstPoint;
        return QStringLiteral("That's already projected.");
    }
    return {};
}

int Document::exactShadow(const QVariantMap &plane, int face, std::vector<std::vector<omamake::FlatPiece>> &loops, QString &why) const {
    omamake::Placement place;
    planeAxes(plane, place.origin, place.x, place.y);
    std::vector<omamake::BodyId> bodies;
    int own = face;
    if (face >= 0) {
        if (std::size_t(face) >= m_faceBody.size()) {
            why = QStringLiteral("Click a face of the model.");
            return -1;
        }
        const int body = m_faceBody[std::size_t(face)];
        own = face - int(std::find(m_faceBody.begin(), m_faceBody.end(), body) - m_faceBody.begin());
        bodies.push_back(omamake::BodyId(body));
    } else {
        for (const int body : std::set<int>(m_faceBody.begin(), m_faceBody.end()))
            bodies.push_back(omamake::BodyId(body));
    }
    std::string whyNot;
    if (m_engine->shadow(bodies, own, place, loops, whyNot))
        return 1;
    if (whyNot == "this engine has no exact edges")
        return 0;
    why = QStringLiteral("Project couldn't work out the exact outline: %1.").arg(QString::fromStdString(whyNot));
    return -1;
}

void Document::exactEdges(const QVariantMap &plane, int face, std::vector<omamake::FlatCurve> &curves,
                          std::vector<omamake::Point2> &corners) const {
    omamake::Placement place;
    planeAxes(plane, place.origin, place.x, place.y);
    if (face >= 0) {
        if (std::size_t(face) >= m_faceBody.size())
            return;
        const int body = m_faceBody[std::size_t(face)];
        const int first = int(std::find(m_faceBody.begin(), m_faceBody.end(), body) - m_faceBody.begin());
        m_engine->flatCurves(omamake::BodyId(body), face - first, place, curves, corners);
        return;
    }
    std::set<int> bodies(m_faceBody.begin(), m_faceBody.end());
    for (const int body : bodies)
        m_engine->flatCurves(omamake::BodyId(body), -1, place, curves, corners);
}

QVariantMap Document::projectIntoSketch(int sketch, int face) {
    const auto found = m_sketches.find(sketch);
    if (found == m_sketches.end())
        return {};
    auto refuse = [](const QString &why) { return QVariantMap{{QStringLiteral("why"), why}}; };
    if (m_mesh.indices.empty())
        return refuse(QStringLiteral("There's no model to project."));
    if (bodyIsFuture())
        return refuse(QStringLiteral("The model is made after this sketch, so it can't be projected into it."));
    Named &s = found->second;
    SketchGeometry &g = s.geometry;
    // Exactly what the model has (owner, 2026-11-06), from the engine's own edges and surfaces.
    {
        std::vector<std::vector<omamake::FlatPiece>> loops;
        QString why;
        const int exact = exactShadow(s.plane, face, loops, why);
        if (exact < 0)
            return refuse(why);
        if (exact > 0) {
            const SketchGeometry before = g;
            PutCounts n;
            if (const QString nothing = putPiecesOnSketch(g, loops, n); !nothing.isEmpty()) {
                g = before;
                return refuse(nothing);
            }
            const int snapshot = m_nextSnapshot++;
            m_snapshots[snapshot] = before;
            QString summary = QStringLiteral("Projected %1 onto the sketch (%2 lines, %3 arcs, %4 circles")
                                  .arg(face < 0 ? QStringLiteral("the whole model") : QStringLiteral("a face"))
                                  .arg(n.lines).arg(n.arcs).arg(n.circles);
            summary += n.curves ? QStringLiteral(", %1 curves)").arg(n.curves) : QStringLiteral(")");
            const int link = addBodyLink(g, before, face < 0);
            record({{QStringLiteral("type"), QStringLiteral("shape")}, {QStringLiteral("sketch"), sketch},
                    {QStringLiteral("snapshot"), snapshot}, {QStringLiteral("link"), link}}, summary);
            sketchesEdited();
            return {{QStringLiteral("summary"), summary}, {QStringLiteral("lines"), n.lines}, {QStringLiteral("arcs"), n.arcs},
                    {QStringLiteral("circles"), n.circles}, {QStringLiteral("curves"), n.curves}};
        }
    }
    // The Lite engine has only the mesh: traced off its triangles.
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

    int lines = 0, arcs = 0, circles = 0;
    const double size = std::hypot(std::hypot(m_mesh.max[0] - m_mesh.min[0], m_mesh.max[1] - m_mesh.min[1]), m_mesh.max[2] - m_mesh.min[2]);
    const SketchGeometry before = g;
    std::vector<omamake::FlatCurve> exact;
    std::vector<omamake::Point2> corners;
    exactEdges(s.plane, face, exact, corners);
    if (const QString why = putOutlineOnSketch(g, triangles, size, lines, arcs, circles, exact, corners); !why.isEmpty()) {
        g = before;
        return refuse(why);
    }
    const int snapshot = m_nextSnapshot++;
    m_snapshots[snapshot] = before;
    const QString summary = QStringLiteral("Projected %1 onto the sketch (%2 lines, %3 arcs, %4 circles)")
                                .arg(face < 0 ? QStringLiteral("the whole model") : QStringLiteral("a face"))
                                .arg(lines).arg(arcs).arg(circles);
    const int link = addBodyLink(g, before, face < 0);
    record({{QStringLiteral("type"), QStringLiteral("shape")}, {QStringLiteral("sketch"), sketch},
            {QStringLiteral("snapshot"), snapshot}, {QStringLiteral("link"), link}}, summary);
    sketchesEdited();
    return {{QStringLiteral("summary"), summary}, {QStringLiteral("lines"), lines}, {QStringLiteral("arcs"), arcs},
            {QStringLiteral("circles"), circles}};
}

// A box of faces: all of them one step in the history, the sketch redrawn and solved once at
// the end (one by one took about 20 ms a face). Each result says whether that face went on.
QVariantList Document::projectFacesIntoSketch(int sketch, const QVariantList &faces) {
    QVariantList results;
    const auto found = m_sketches.find(sketch);
    if (found == m_sketches.end())
        return results;
    const auto refuse = [](const QString &why) { return QVariant(QVariantMap{{QStringLiteral("why"), why}}); };
    if (m_mesh.indices.empty() || bodyIsFuture()) {
        const QString why = m_mesh.indices.empty() ? QStringLiteral("There's no model to project.")
                                                   : QStringLiteral("The model is made after this sketch, so it can't be projected into it.");
        for (int i = 0; i < faces.size(); ++i)
            results << refuse(why);
        return results;
    }
    Named &s = found->second;
    SketchGeometry &g = s.geometry;
    const QVector3D origin = s.plane.value(QStringLiteral("origin")).value<QVector3D>();
    const QVector3D xAxis = s.plane.value(QStringLiteral("xAxis")).value<QVector3D>();
    const QVector3D yAxis = s.plane.value(QStringLiteral("yAxis")).value<QVector3D>();
    const auto flat = [&](std::uint32_t i) {
        const QVector3D p = at(m_mesh.positions, i) - origin;
        return projection::Flat{QVector3D::dotProduct(p, xAxis), QVector3D::dotProduct(p, yAxis)};
    };
    std::map<int, std::vector<std::array<projection::Flat, 3>>> byFace;   // one pass over the mesh for all faces
    for (const QVariant &f : faces)
        byFace[f.toInt()];
    for (std::size_t t = 0; t < m_mesh.indices.size() / 3; ++t) {
        const auto it = byFace.find(int(m_mesh.triangleFace[t]));
        if (it != byFace.end())
            it->second.push_back({flat(m_mesh.indices[t * 3]), flat(m_mesh.indices[t * 3 + 1]), flat(m_mesh.indices[t * 3 + 2])});
    }
    const double size = std::hypot(std::hypot(m_mesh.max[0] - m_mesh.min[0], m_mesh.max[1] - m_mesh.min[1]), m_mesh.max[2] - m_mesh.min[2]);
    const SketchGeometry before = g;
    int made = 0;
    for (const QVariant &f : faces) {
        // Exactly what the face has (owner, 2026-11-06).
        std::vector<std::vector<omamake::FlatPiece>> loops;
        QString exactWhy;
        const int exactKind = exactShadow(s.plane, f.toInt(), loops, exactWhy);
        if (exactKind < 0) {
            results << refuse(exactWhy);
            continue;
        }
        if (exactKind > 0) {
            PutCounts n;
            if (const QString nothing = putPiecesOnSketch(g, loops, n); !nothing.isEmpty()) {
                results << refuse(nothing);
                continue;
            }
            ++made;
            results << QVariant(QVariantMap{{QStringLiteral("lines"), n.lines}, {QStringLiteral("arcs"), n.arcs},
                                            {QStringLiteral("circles"), n.circles}, {QStringLiteral("curves"), n.curves}});
            continue;
        }
        const auto &triangles = byFace[f.toInt()];
        if (triangles.empty()) {
            results << refuse(QStringLiteral("Click a face of the model."));
            continue;
        }
        int lines = 0, arcs = 0, circles = 0;
        std::vector<omamake::FlatCurve> exact;
        std::vector<omamake::Point2> corners;
        exactEdges(s.plane, f.toInt(), exact, corners);
        const QString why = putOutlineOnSketch(g, triangles, size, lines, arcs, circles, exact, corners);
        if (!why.isEmpty()) {
            results << refuse(why);
            continue;
        }
        ++made;
        results << QVariant(QVariantMap{{QStringLiteral("lines"), lines}, {QStringLiteral("arcs"), arcs}, {QStringLiteral("circles"), circles}});
    }
    if (made) {
        const int snapshot = m_nextSnapshot++;
        m_snapshots[snapshot] = before;
        const int link = addBodyLink(g, before, false);
        record({{QStringLiteral("type"), QStringLiteral("shape")}, {QStringLiteral("sketch"), sketch},
                {QStringLiteral("snapshot"), snapshot}, {QStringLiteral("link"), link}},
               QStringLiteral("Projected %1 faces onto the sketch").arg(made));
        sketchesEdited();
    }
    return results;
}

QVariantMap Document::projectPartIntoSketch(int sketch, const QString &kind, int index) {
    return projectPart(sketch, kind, index, true);
}

// A box of edges and corners: all of them one step in the history (one Ctrl+Z takes the box
// back), the sketch redrawn and solved once at the end. Each result says whether that part went on.
QVariantList Document::projectPartsIntoSketch(int sketch, const QVariantList &parts) {
    QVariantList results;
    const auto found = m_sketches.find(sketch);
    if (found == m_sketches.end())
        return results;
    const SketchGeometry before = found->second.geometry;
    int made = 0;
    for (const QVariant &v : parts) {
        const QVariantMap part = v.toMap();
        const QVariantMap r = projectPart(sketch, part.value(QStringLiteral("kind")).toString(),
                                          part.value(QStringLiteral("index")).toInt(), false);
        if (!r.contains(QStringLiteral("why")))
            ++made;
        results << QVariant(r);
    }
    if (made) {
        const int snapshot = m_nextSnapshot++;
        m_snapshots[snapshot] = before;
        const int link = addBodyLink(found->second.geometry, before, false);
        record({{QStringLiteral("type"), QStringLiteral("shape")}, {QStringLiteral("sketch"), sketch},
                {QStringLiteral("snapshot"), snapshot}, {QStringLiteral("link"), link}},
               QStringLiteral("Projected %1 edges and points onto the sketch").arg(made));
        sketchesEdited();
    }
    return results;
}

QVariantMap Document::projectPart(int sketch, const QString &kind, int index, bool notify) {
    const auto found = m_sketches.find(sketch);
    if (found == m_sketches.end())
        return {};
    auto refuse = [](const QString &why) { return QVariantMap{{QStringLiteral("why"), why}}; };
    if (bodyIsFuture())
        return refuse(QStringLiteral("The model is made after this sketch, so it can't be projected into it."));
    const bool wantPoint = kind == QLatin1String("point");
    if (!wantPoint && kind != QLatin1String("edge"))
        return refuse(QStringLiteral("Click an edge or a corner of the model."));
    if (wantPoint ? index < 0 || std::size_t(index) >= m_mesh.points.size() / 3
                  : index < 0 || std::size_t(index) >= m_mesh.edges.size())
        return refuse(QStringLiteral("Click an edge or a corner of the model."));
    Named &s = found->second;
    SketchGeometry &g = s.geometry;
    const QVector3D origin = s.plane.value(QStringLiteral("origin")).value<QVector3D>();
    const QVector3D xAxis = s.plane.value(QStringLiteral("xAxis")).value<QVector3D>();
    const QVector3D yAxis = s.plane.value(QStringLiteral("yAxis")).value<QVector3D>();
    const auto flatten = [&](const QVector3D &w) {
        const QVector3D p = w - origin;
        return projection::Flat{QVector3D::dotProduct(p, xAxis), QVector3D::dotProduct(p, yAxis)};
    };
    // Exactly what the edge or corner is (owner, 2026-11-06), from the engine's own shape.
    {
        const std::vector<int> &owner = wantPoint ? m_pointBody : m_edgeBody;
        const int body = std::size_t(index) < owner.size() ? owner[std::size_t(index)] : 0;
        const int own = index - int(std::find(owner.begin(), owner.end(), body) - owner.begin());
        omamake::Placement place;
        planeAxes(s.plane, place.origin, place.x, place.y);
        std::vector<omamake::FlatPiece> pieces;
        std::string whyNot;
        const bool ok = body && m_engine->flatPart(omamake::BodyId(body), wantPoint ? 'p' : 'e', own, place, pieces, whyNot);
        if (!ok && whyNot != "this engine has no exact edges")
            return refuse(QStringLiteral("Project couldn't lay that %1 flat exactly: %2.")
                              .arg(wantPoint ? QStringLiteral("corner") : QStringLiteral("edge"), QString::fromStdString(whyNot)));
        if (ok) {
            const SketchGeometry before = g;
            PutCounts n;
            if (const QString nothing = putPiecesOnSketch(g, {pieces}, n); !nothing.isEmpty()) {
                g = before;
                return refuse(nothing);
            }
            const QString what = wantPoint ? QStringLiteral("a corner point") : n.lines ? QStringLiteral("an edge (a line)")
                               : n.circles ? QStringLiteral("an edge (a circle)") : n.arcs ? QStringLiteral("an edge (an arc)")
                               : n.curves ? QStringLiteral("an edge (a curve)") : QStringLiteral("an edge (a point)");
            const QString summary = QStringLiteral("Projected %1 onto the sketch").arg(what);
            if (notify) {
                const int snapshot = m_nextSnapshot++;
                m_snapshots[snapshot] = before;
                const int link = addBodyLink(g, before, false);
                record({{QStringLiteral("type"), QStringLiteral("shape")}, {QStringLiteral("sketch"), sketch},
                        {QStringLiteral("snapshot"), snapshot}, {QStringLiteral("link"), link}}, summary);
                sketchesEdited();
            }
            return {{QStringLiteral("summary"), summary}, {QStringLiteral("points"), n.points}, {QStringLiteral("lines"), n.lines},
                    {QStringLiteral("arcs"), n.arcs}, {QStringLiteral("circles"), n.circles}, {QStringLiteral("curves"), n.curves}};
        }
    }
    // The Lite engine has only the mesh: read off the drawn edge's points.
    const double size = std::hypot(std::hypot(m_mesh.max[0] - m_mesh.min[0], m_mesh.max[1] - m_mesh.min[1]), m_mesh.max[2] - m_mesh.min[2]);
    const double tol = std::max(0.012, size / 25000);

    // The edge's points laid flat, without the ones that land on top of each other.
    std::vector<projection::Flat> p;
    if (wantPoint) {
        p.push_back(flatten(at(m_mesh.points, std::size_t(index))));
    } else {
        const auto &line = m_mesh.edges[std::size_t(index)];
        for (std::size_t i = 0; i < line.size() / 3; ++i) {
            const projection::Flat f = flatten(at(line, i));
            if (p.empty() || std::hypot(f.u - p.back().u, f.v - p.back().v) > 1e-6)
                p.push_back(f);
        }
    }
    if (p.empty())
        return refuse(QStringLiteral("That edge has no points."));
    const bool closed = p.size() > 2 && std::hypot(p.front().u - p.back().u, p.front().v - p.back().v) <= 1e-6;
    if (closed)
        p.pop_back();

    const SketchGeometry before = g;
    std::map<std::pair<long long, long long>, int> known;
    const auto cell = [](const projection::Flat &q) { return std::pair{std::llround(q.u * 1e5), std::llround(q.v * 1e5)}; };
    for (const int id : g.projectedPoints)
        if (g.points.count(id))
            known[cell({g.points[id].u, g.points[id].v})] = id;
    const auto pointAt = [&](const projection::Flat &q) {
        const auto [it, fresh] = known.try_emplace(cell(q), 0);
        if (fresh) {
            it->second = g.nextPoint++;
            g.points[it->second] = {q.u, q.v};
            g.projectedPoints.insert(it->second);
        }
        return it->second;
    };
    // The circle through three points; false when they're in a line.
    const auto circle = [](const projection::Flat &a, const projection::Flat &b, const projection::Flat &c, projection::Flat &centre) {
        const double d = 2 * (a.u * (b.v - c.v) + b.u * (c.v - a.v) + c.u * (a.v - b.v));
        if (std::abs(d) < 1e-9)
            return false;
        const double a2 = a.u * a.u + a.v * a.v, b2 = b.u * b.u + b.v * b.v, c2 = c.u * c.u + c.v * c.v;
        centre.u = (a2 * (b.v - c.v) + b2 * (c.v - a.v) + c2 * (a.v - b.v)) / d;
        centre.v = (a2 * (c.u - b.u) + b2 * (a.u - c.u) + c2 * (b.u - a.u)) / d;
        return true;
    };

    int points = 0, lines = 0, arcs = 0, circles = 0, curves = 0;
    const std::size_t n = p.size();
    if (n == 1) {                       // a corner, or an edge seen end-on
        const std::size_t had = g.points.size();
        pointAt(p[0]);
        points = int(g.points.size() - had);
    } else {
        // Straight when every point is within tol of the line between the ends.  A closed loop that
        // lands flat (a circle seen edge-on goes out and back along one line) is straight too, between
        // its two farthest-apart points.
        projection::Flat e0 = p.front(), e1 = p.back();
        if (closed) {
            const auto farthest = [&](const projection::Flat &from) {
                std::size_t best = 0;
                double most = -1;
                for (std::size_t i = 0; i < n; ++i) {
                    const double d = std::hypot(p[i].u - from.u, p[i].v - from.v);
                    if (d > most) { most = d; best = i; }
                }
                return p[best];
            };
            e0 = farthest(p.front());
            e1 = farthest(e0);
        }
        const double len = std::hypot(e1.u - e0.u, e1.v - e0.v);
        bool straight = len > 1e-9;
        for (std::size_t i = 0; straight && i < n; ++i)
            straight = std::abs((e1.u - e0.u) * (p[i].v - e0.v) - (e1.v - e0.v) * (p[i].u - e0.u)) / len <= tol * 2;
        projection::Flat c;
        const projection::Flat &mid = p[n / 2];
        const projection::Flat &last = closed ? p[n / 4] : p.back();
        bool round = !straight && n >= 4 && circle(p.front(), mid, last, c);
        bool arcCounter = false;     // the open arc runs counter-clockwise from the first point to the last
        double r = 0;
        if (round) {
            r = std::hypot(p.front().u - c.u, p.front().v - c.v);
            for (const projection::Flat &q : p)
                round = round && std::abs(std::hypot(q.u - c.u, q.v - c.v) - r) <= tol * 2;
            // A circle far bigger than the model is a near-straight run, not a circle.
            round = round && r <= size * 4;
            // An open arc runs one way round the centre: points that fold back on themselves are no arc.
            if (round && !closed) {
                double total = 0;
                bool up = false, down = false;
                for (std::size_t i = 1; i < n; ++i) {
                    double step = std::atan2(p[i].v - c.v, p[i].u - c.u) - std::atan2(p[i - 1].v - c.v, p[i - 1].u - c.u);
                    step = std::remainder(step, 2 * M_PI);
                    (step > 1e-9 ? up : down) = (step > 1e-9 ? up : down) || std::abs(step) > 1e-9;
                    total += step;
                }
                arcCounter = total > 0;
                round = !(up && down) && std::abs(total) <= 2 * M_PI;
            }
            if (round && closed) {      // a real closed circle spans about its own diameter
                double lo[2] = {p[0].u, p[0].v}, hi[2] = {p[0].u, p[0].v};
                for (const projection::Flat &q : p) {
                    lo[0] = std::min(lo[0], q.u); hi[0] = std::max(hi[0], q.u);
                    lo[1] = std::min(lo[1], q.v); hi[1] = std::max(hi[1], q.v);
                }
                round = std::max(hi[0] - lo[0], hi[1] - lo[1]) >= r * 1.6;
            }
        }
        if (straight) {
            const int a = pointAt(e0), b = pointAt(e1);
            if (a != b && !std::any_of(g.lines.begin(), g.lines.end(), [&](const auto &l) {
                    return (l.second.a == a && l.second.b == b) || (l.second.a == b && l.second.b == a); })) {
                g.lines[g.nextLine++] = {a, b};
                lines = 1;
            }
        } else if (round) {
            const bool newCentre = !known.count(cell(c));
            const int cid = pointAt(c);
            if (closed) {
                const int a = pointAt({c.u + r, c.v});
                if (!std::any_of(g.arcs.begin(), g.arcs.end(), [&](const auto &k) { return k.second.c == cid && k.second.circle(); })) {
                    g.arcs[g.nextArc++] = {cid, a, a};
                    circles = 1;
                }
            } else {
                // The arc runs counter-clockwise from a to b: whichever end comes first on that way round.
                const int a = pointAt(arcCounter ? p.front() : p.back()), b = pointAt(arcCounter ? p.back() : p.front());
                if (newCentre)
                    g.helperPoints.insert(cid);
                if (a != b && !std::any_of(g.arcs.begin(), g.arcs.end(), [&](const auto &k) {
                        return k.second.c == cid && k.second.a == a && k.second.b == b; })) {
                    g.arcs[g.nextArc++] = {cid, a, b};
                    arcs = 1;
                }
            }
        } else {
            // Any other curve: a smooth curve through up to 24 of its points, both ends included.
            SketchGeometry::Curve k;
            k.type = QStringLiteral("fitspline");
            const std::size_t count = std::min<std::size_t>(n, 24);
            for (std::size_t i = 0; i < count; ++i)
                k.points.push_back(pointAt(p[i * (n - 1) / (count - 1)]));
            if (closed)
                k.points.push_back(k.points.front());
            k.points.erase(std::unique(k.points.begin(), k.points.end()), k.points.end());
            // The same curve (its points the same way or the other way round) is never put on twice.
            const auto same = [&](const auto &o) {
                const auto &q = o.second.points;
                return q == k.points || std::equal(q.begin(), q.end(), k.points.rbegin(), k.points.rend());
            };
            if (k.points.size() >= 2 && !std::any_of(g.curves.begin(), g.curves.end(), same)) {
                g.curves[g.nextCurve++] = k;
                curves = 1;
            }
        }
    }
    if (!points && !lines && !arcs && !circles && !curves) {
        g = before;
        return refuse(QStringLiteral("That's already projected."));
    }
    QString what = wantPoint ? QStringLiteral("a corner point") : lines ? QStringLiteral("an edge (a line)")
                 : circles ? QStringLiteral("an edge (a circle)") : arcs ? QStringLiteral("an edge (an arc)")
                 : curves ? QStringLiteral("an edge (a curve)") : QStringLiteral("an edge (a point)");
    const QString summary = QStringLiteral("Projected %1 onto the sketch").arg(what);
    if (notify) {
        const int snapshot = m_nextSnapshot++;
        m_snapshots[snapshot] = before;
        const int link = addBodyLink(g, before, false);
        record({{QStringLiteral("type"), QStringLiteral("shape")}, {QStringLiteral("sketch"), sketch},
                {QStringLiteral("snapshot"), snapshot}, {QStringLiteral("link"), link}}, summary);
        sketchesEdited();
    }
    return {{QStringLiteral("summary"), summary}, {QStringLiteral("points"), points}, {QStringLiteral("lines"), lines},
            {QStringLiteral("arcs"), arcs}, {QStringLiteral("circles"), circles}, {QStringLiteral("curves"), curves}};
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
        k.font = m.value(QStringLiteral("font")).toString();
        k.flags = m.value(QStringLiteral("flags")).toInt();
        for (const QVariant &code : m.value(QStringLiteral("path")).toList())
            k.path.push_back(code.toInt());
        k.anchor = m.value(QStringLiteral("anchor")).toDouble();
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
            if (it->second.type == QLatin1String("curvature")) {
                const auto &k = it->second;
                gone = !g.curves.count(k.first) || (k.third == 1 ? !g.hasLine(k.second) : k.third == 2 ? !g.arcs.count(k.second)
                                                                                                        : !g.curves.count(k.second));
            }
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
            {QStringLiteral("constraints"), constraintIds}, {QStringLiteral("new_points"), created},
            {QStringLiteral("tool"), spec.value(QStringLiteral("tool")).toString()}},   // which tool, for its icon
           spec.value(QStringLiteral("summary")).toString());
    sketchesEdited();
    return {{QStringLiteral("points"), pointIds}, {QStringLiteral("lines"), lineList}, {QStringLiteral("arcs"), arcList},
            {QStringLiteral("curves"), curveList}, {QStringLiteral("constraints"), constraintList}};
}

QStringList Document::fontFamilies() const {
    return QFontDatabase::families();
}

QVariantList Document::sketchTextInBox(const QString &text, const QString &font, int flags, double height, double w, double h) const {
    QVariantList out;
    for (const auto &shape : SketchGeometry::textInBox(text, font, flags, height, w, h)) {
        QVariantList path;
        for (const auto &p : shape)
            path << QVariantMap{{QStringLiteral("u"), p.u}, {QStringLiteral("v"), p.v}};
        out << QVariant(path);
    }
    return out;
}

QVariantMap Document::sketchTextOnPath(int sketch, const QString &text, const QString &font, int flags, double height,
                                       const QVariantList &codes) const {
    const auto found = m_sketches.find(sketch);
    if (found == m_sketches.end())
        return {};
    SketchGeometry::Curve k;
    k.type = QStringLiteral("text");
    k.flags = flags;
    for (const QVariant &c : codes)
        k.path.push_back(c.toInt());
    bool closed = false;
    const auto chain = found->second.geometry.pathChain(k, closed);
    if (chain.size() < 2)
        return {};
    const auto laid = SketchGeometry::textAlong(text, font, flags, height, chain, closed);
    const auto list = [](const std::vector<SketchGeometry::Point> &path) {
        QVariantList out;
        for (const auto &p : path)
            out << QVariantMap{{QStringLiteral("u"), p.u}, {QStringLiteral("v"), p.v}};
        return out;
    };
    QVariantList shapes;
    for (const auto &shape : laid.shapes)
        shapes << QVariant(list(shape));
    return {{QStringLiteral("shapes"), shapes}, {QStringLiteral("frame"), list(laid.frame)}};
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
    if (type == QLatin1String("curvature"))
        return addCurvatureConstraint(found->second, sketch, first, second, third);
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
    const bool joining = type == QLatin1String("coincident");
    g.directOnly = joining;
    bool holds = g.solveNear({touched.begin(), touched.end()}, steady);
    g.directOnly = false;
    if (!holds && joining) {
        g.constraints.erase(id);
        const bool slid = g.slideTogether(ids[1 - kept], ids[kept]) || g.slideTogether(ids[kept], ids[1 - kept]);
        g.constraints[id] = c;
        holds = slid && g.solveNear({touched.begin(), touched.end()}, steady);
        if (!holds) {
            g.points = before;
            holds = g.solveNear({touched.begin(), touched.end()}, steady);
        }
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

// Curvature (G2, owner 2026-11-02): a spline's end runs on from a line, an arc or another spline's
// end, where they meet, in the same direction and with the same bend. first: the spline that
// adjusts; second: what it joins (third: 1 line, 2 arc, 3 spline); the second stays put.
QString Document::addCurvatureConstraint(Named &named, int sketch, int first, int second, int third) {
    SketchGeometry &g = named.geometry;
    const auto isSpline = [&](int id) {
        return g.curves.count(id) && (g.curves.at(id).type == QLatin1String("fitspline") || g.curves.at(id).type == QLatin1String("controlspline"));
    };
    if (!isSpline(first))
        return QStringLiteral("Curvature needs a spline: pick one by its curve.");
    if (third == 1 ? !g.lines.count(second) : third == 2 ? !g.arcs.count(second) : !isSpline(second))
        return third == 1 && SketchGeometry::isReference(second)
                   ? QStringLiteral("An axis can't be bent to meet a spline: pick a line you drew.")
                   : QStringLiteral("Curvature joins a spline to a line, an arc or another spline.");
    if (third == 3 && first == second)
        return QStringLiteral("Pick two different ones.");
    if (third == 2 && g.arcs.at(second).circle())
        return QStringLiteral("A circle has no ends to join: pick an arc.");
    // The two ends that meet: the same point, tied by Coincident, or on top of each other.
    const auto endsOf = [&](int kind, int id) -> std::pair<int, int> {
        if (kind == 1)
            return {g.lines.at(id).a, g.lines.at(id).b};
        if (kind == 2)
            return {g.arcs.at(id).a, g.arcs.at(id).b};
        return {g.curves.at(id).points.front(), g.curves.at(id).points.back()};
    };
    const auto tied = [&](int p, int q) {
        if (p == q)
            return true;
        for (const auto &[cid, c] : g.constraints)
            if (c.type == QLatin1String("coincident") && !c.driven && ((c.first == p && c.second == q) || (c.first == q && c.second == p)))
                return true;
        return false;
    };
    const auto mine = endsOf(3, first), theirs = endsOf(third, second);
    int bestU = -1, bestV = -1;
    double bestGap = 1e300;
    for (int u = 0; u < 2; ++u)
        for (int v = 0; v < 2; ++v) {
            const int p = u ? mine.second : mine.first, q = v ? theirs.second : theirs.first;
            const auto a = g.point(p), b = g.point(q);
            const double gap = std::hypot(a.u - b.u, a.v - b.v);
            if ((tied(p, q) || gap < 1e-4) && gap < bestGap) {
                bestGap = gap;
                bestU = u;
                bestV = v;
            }
        }
    if (bestU < 0)
        return QStringLiteral("Curvature joins two curves where their ends meet. Put the ends together first (Coincident), then Curvature.");
    SketchGeometry::Constraint c(QStringLiteral("curvature"), first, second, bestU, bestV);
    c.third = third;
    for (const auto &[cid, k] : g.constraints)
        if (k.type == c.type && k.third == third && ((k.first == first && k.second == second && k.u == c.u && k.v == c.v)))
            return QStringLiteral("That's already there.");
    if (g.leaveDot(c) > 0.3)
        return QStringLiteral("They fold back over each other at that join, so they can't run on smoothly. Curvature needs them to leave the join in opposite directions.");

    const std::map<int, SketchGeometry::Point> before = g.points;
    const int id = g.nextConstraint++;
    g.constraints[id] = c;
    const std::vector<int> touched = g.pointsOf(c);
    std::set<int> steady;                              // what it joins stays where it is, the spline gives way
    if (third == 1) {
        steady = {g.lines.at(second).a, g.lines.at(second).b};
    } else if (third == 2) {
        const auto &k = g.arcs.at(second);
        steady = {k.c, k.a, k.b};
    } else {
        for (int p : g.curves.at(second).points)
            steady.insert(p);
    }
    bool holds = g.solveNear({touched.begin(), touched.end()}, steady);
    if (holds && g.leaveDot(c) > 0)
        holds = false;
    if (!holds) {
        g.constraints.erase(id);
        g.nextConstraint--;
        g.points = before;
        return QStringLiteral("That can't hold with the constraints already there.");
    }
    const QString what = third == 1 ? QStringLiteral("Line%1").arg(second) : third == 2 ? QStringLiteral("Arc%1").arg(second)
                                                                                         : QStringLiteral("Spline%1").arg(second);
    record({{QStringLiteral("type"), QStringLiteral("constraint")}, {QStringLiteral("sketch"), sketch},
            {QStringLiteral("id"), id}, {QStringLiteral("constraint"), QStringLiteral("curvature")},
            {QStringLiteral("first"), first}, {QStringLiteral("second"), second}, {QStringLiteral("third"), third},
            {QStringLiteral("moved"), movedSince(g, before)}},
           QStringLiteral("Made Spline%1 curvature-continuous with %2 (same direction and bend at their join)").arg(first).arg(what));
    sketchesEdited();
    return {};
}


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
    const SketchGeometry &g = shownGeometry(sketch, s.geometry);      // part way through a glide: where it is now
    for (const auto &[id, c] : g.constraints) {
        double now = 0;
        SketchGeometry::Point a, b;
        if (!c.isDimension() || c.follows() || !g.measure(c.type, c.first, c.second, now, a, b))
            continue;
        // A gap between lines is shown a quarter of the way along them, not at the middle, so
        // the two gaps of a rectangle don't sit on top of each other in its centre.
        if (c.type == QLatin1String("lineline")) {
            const auto l = g.line(c.second);
            const SketchGeometry::Point p = g.point(l.a), q = g.point(l.b);
            const double du = (p.u - q.u) / 4, dv = (p.v - q.v) / 4;
            a = {a.u + du, a.v + dv};
            b = {b.u + du, b.v + dv};
        }
        QVariantMap d{{QStringLiteral("id"), id}, {QStringLiteral("type"), c.type},
                           {QStringLiteral("value"), c.driven ? g.valueOf(c) : c.value}, {QStringLiteral("unit"), c.unit},
                           {QStringLiteral("driven"), c.driven},
                           {QStringLiteral("expr"), c.expr}, {QStringLiteral("expr_flip"), c.exprFlip},
                           {QStringLiteral("a"), onSketch(s, a.u, a.v)}, {QStringLiteral("b"), onSketch(s, b.u, b.v)},
                           {QStringLiteral("mid_u"), (a.u + b.u) / 2}, {QStringLiteral("mid_v"), (a.v + b.v) / 2},
                           {QStringLiteral("placed"), c.labelPlaced},
                           {QStringLiteral("label_u"), c.labelU}, {QStringLiteral("label_v"), c.labelV}};
        // An angle: its corner, and which side of it the value means (u -1: the first line
        // against the second taken end to start). Its label_u/v are along the two lines from
        // the corner, so it keeps to its side of the corner as the lines turn.
        if (c.type == QLatin1String("angle")) {
            d.insert(QStringLiteral("frame"), angleFrame(g, c.first, c.second));
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

// Would this dimension be driven (the others already decide it)? Asked before the value box opens.
bool Document::dimensionIsDriven(int sketch, const QString &type, int first, int second, bool otherSide) const {
    const auto found = m_sketches.find(sketch);
    if (found == m_sketches.end())
        return false;
    SketchGeometry::Constraint c{type, first, second};
    if (type == QLatin1String("angle") && otherSide)
        c.u = -1;
    return found->second.geometry.redundant(c);
}

QString Document::addSketchDimension(int sketch, const QString &type, int first, int second, double value,
                                     const QString &unit, const QString &label, bool otherSide, const QString &expr) {
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
    c.expr = expr;
    if (type == QLatin1String("angle") && otherSide)
        c.u = -1;
    // The others already decide it (owner, t88u): a driven dimension, it shows what it is and holds nothing.
    if (g.redundant(c)) {
        c.driven = true;
        c.value = now;
        c.expr.clear();
        g.constraints[id] = c;
        const QString made = SketchGeometry::kinds(type);
        const QString what = made.size() == 1 ? sketchPartName(found->second, made[0], first)
                                              : QStringLiteral("%1 to %2").arg(sketchPartName(found->second, made[0], first),
                                                                              sketchPartName(found->second, made[1], second));
        record({{QStringLiteral("type"), QStringLiteral("constraint")}, {QStringLiteral("sketch"), sketch},
                {QStringLiteral("id"), id}, {QStringLiteral("constraint"), type},
                {QStringLiteral("first"), first}, {QStringLiteral("second"), second},
                {QStringLiteral("moved"), QJsonArray()}},
               QStringLiteral("Dimensioned %1: (%2) driven by the other dimensions").arg(what, label));
        sketchesEdited();
        return {};
    }
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

QString Document::setSketchDimension(int sketch, int constraint, double value, const QString &unit, const QString &label,
                                     const QString &expr, bool exprFlip) {
    const auto found = m_sketches.find(sketch);
    if (found == m_sketches.end())
        return QStringLiteral("No sketch is open.");
    SketchGeometry &g = found->second.geometry;
    const auto c = g.constraints.find(constraint);
    if (c == g.constraints.end() || !c->second.isDimension())
        return QStringLiteral("That dimension is gone.");
    if (c->second.driven)
        return QStringLiteral("That one is driven by the other dimensions, so it can't be typed. Change those instead.");
    if (!(value > 1e-6))
        return QStringLiteral("A dimension has to be more than zero.");
    const std::map<int, SketchGeometry::Point> before = g.points;
    const double oldValue = c->second.value;
    const QString oldUnit = c->second.unit, oldExpr = c->second.expr;
    const bool oldFlip = c->second.exprFlip;
    c->second.value = value;
    c->second.unit = unit;
    c->second.expr = expr;
    c->second.exprFlip = !expr.isEmpty() && exprFlip;
    const std::vector<int> touched = g.pointsOf(c->second);
    if (!g.solveNear({touched.begin(), touched.end()})) {
        c->second.value = oldValue;
        c->second.unit = oldUnit;
        c->second.expr = oldExpr;
        c->second.exprFlip = oldFlip;
        g.points = before;
        return QStringLiteral("It can't be %1 with the constraints already there.").arg(label);
    }
    record({{QStringLiteral("type"), QStringLiteral("dimension_value")}, {QStringLiteral("sketch"), sketch},
            {QStringLiteral("id"), constraint}, {QStringLiteral("value"), oldValue}, {QStringLiteral("unit"), oldUnit},
            {QStringLiteral("expr"), oldExpr}, {QStringLiteral("expr_flip"), oldFlip},
            {QStringLiteral("moved"), movedSince(g, before)}},
           expr.isEmpty() ? QStringLiteral("Changed a dimension to %1").arg(label)
                          : QStringLiteral("Changed a dimension to %1 = %2").arg(expr, label));
    sketchesEdited();
    return {};
}

void Document::testMouse(QObject *window, const QString &kind, double x, double y) {
    if (!qEnvironmentVariableIsSet("OMAMAKE_TEST_INPUT"))
        return;
    auto *w = qobject_cast<QQuickWindow *>(window);
    if (!w)
        return;
    const QPointF at(x, y);
    const bool press = kind == QLatin1String("press"), release = kind == QLatin1String("release");
    QMouseEvent ev(press ? QEvent::MouseButtonPress : release ? QEvent::MouseButtonRelease : QEvent::MouseMove, at, w->mapToGlobal(at),
                   press || release ? Qt::LeftButton : Qt::NoButton, release ? Qt::NoButton : (press ? Qt::LeftButton : Qt::LeftButton), Qt::NoModifier);
    const bool ok = QCoreApplication::sendEvent(w, &ev);
    Q_UNUSED(ok)
}

// Test-only (env OMAMAKE_TEST_INPUT): one key press and release sent to the window, so Enter in a
// field can be tried the way a keyboard does it.
void Document::testKey(QObject *window, int key, const QString &text, int modifiers) {
    if (!qEnvironmentVariableIsSet("OMAMAKE_TEST_INPUT"))
        return;
    auto *w = qobject_cast<QQuickWindow *>(window);
    if (!w)
        return;
    // Through the window system path, as a real keyboard does, so shortcuts (Enter) fire too.
    QWindowSystemInterface::handleKeyEvent<QWindowSystemInterface::SynchronousDelivery>(w, QEvent::KeyPress, key, Qt::KeyboardModifiers(modifiers), text);
    QWindowSystemInterface::handleKeyEvent<QWindowSystemInterface::SynchronousDelivery>(w, QEvent::KeyRelease, key, Qt::KeyboardModifiers(modifiers), text);
}

// Changes a text's words, font, size or look (owner, 2026-11-02): `props` holds any of text, font,
// flags, height. One undoable history step, holding what it was.
QString Document::editSketchText(int sketch, int curve, const QVariantMap &props) {
    const auto found = m_sketches.find(sketch);
    if (found == m_sketches.end())
        return QStringLiteral("No sketch is open.");
    SketchGeometry &g = found->second.geometry;
    const auto k = g.curves.find(curve);
    if (k == g.curves.end() || k->second.type != QLatin1String("text"))
        return QStringLiteral("That isn't a text.");
    SketchGeometry::Curve &c = k->second;
    const QJsonObject was{{QStringLiteral("id"), curve}, {QStringLiteral("text"), c.text}, {QStringLiteral("font"), c.font},
                          {QStringLiteral("flags"), c.flags}, {QStringLiteral("height_mm"), c.value},
                          {QStringLiteral("anchor"), c.anchor}};
    const QString text = props.contains(QStringLiteral("text")) ? props.value(QStringLiteral("text")).toString() : c.text;
    if (text.trimmed().isEmpty())
        return QStringLiteral("A text needs some words.");
    const QString font = props.contains(QStringLiteral("font")) ? props.value(QStringLiteral("font")).toString() : c.font;
    int flags = props.contains(QStringLiteral("flags")) ? props.value(QStringLiteral("flags")).toInt() : c.flags;
    double anchor = c.anchor;
    if (!c.path.empty()) {                   // a dragged anchor stays; Flip turns the path round, so it turns with it
        flags = (flags & ~SketchGeometry::TextAnchored) | (c.flags & SketchGeometry::TextAnchored);
        if ((flags & SketchGeometry::TextFlip) != (c.flags & SketchGeometry::TextFlip) && (c.flags & SketchGeometry::TextAnchored)) {
            double total = 0;
            bool closed = false;
            g.textPathPosition(c, {0, 0}, total, closed);
            anchor = total - c.anchor;
        }
    }
    const double height = props.contains(QStringLiteral("height")) ? props.value(QStringLiteral("height")).toDouble() : c.value;
    if (text == c.text && font == c.font && flags == c.flags && std::abs(height - c.value) < 1e-9)
        return {};
    c.anchor = anchor;
    c.text = text;
    c.font = font;
    c.flags = flags;
    c.value = std::max(height, 1e-3);
    recordTextEdit(sketch, curve, was, QStringLiteral("Edited the text \"%1\"").arg(text), QStringLiteral("Edited the text \"%1\"").arg(text));
    sketchesEdited();
    return {};
}

// One edit session of one text is ONE history item, however many letters, changes or drags it takes
// (owner, 2026-11-02, t78u/t79u): while the newest item is this text's edit item, later edits only
// update it; it keeps the first "was", so one undo takes the whole session back.
void Document::recordTextEdit(int sketch, int curve, const QJsonObject &was, const QString &first, const QString &merged) {
    if (m_textEditItem >= 0 && m_textEditItem == int(m_actions.size()) - 1 && m_textEditSketch == sketch && m_textEditCurve == curve) {
        QJsonObject item = m_actions[m_textEditItem].toObject();
        if (item.value(QStringLiteral("type")).toString() == QLatin1String("text_edit")) {
            item.insert(QStringLiteral("summary"), merged);
            m_dirtyFrom = std::min(m_dirtyFrom, m_textEditItem);
            m_actions[m_textEditItem] = item;
            emit historyChanged();
            return;
        }
    }
    record({{QStringLiteral("type"), QStringLiteral("text_edit")}, {QStringLiteral("sketch"), sketch}, {QStringLiteral("was"), was}}, first);
    m_textEditItem = int(m_actions.size()) - 1;
    m_textEditSketch = sketch;
    m_textEditCurve = curve;
}

void Document::closeTextEdit() {
    m_textEditItem = -1;
}

bool Document::beginSketchTextSlide(int sketch, int curve, double u, double v) {
    m_slide = {};
    const auto found = m_sketches.find(sketch);
    if (found == m_sketches.end())
        return false;
    const SketchGeometry &g = found->second.geometry;
    const auto k = g.curves.find(curve);
    if (k == g.curves.end() || k->second.type != QLatin1String("text") || k->second.path.empty())
        return false;
    const SketchGeometry::Curve &c = k->second;
    double total = 0;
    bool closed = false;
    m_slide.from = g.textPathPosition(c, {u, v}, total, closed);
    if (total <= 0)
        return false;
    m_slide.anchor = (c.flags & SketchGeometry::TextAnchored) ? c.anchor : ((c.flags >> 4) & 3) * total / 2;
    m_slide.was = QJsonObject{{QStringLiteral("id"), curve}, {QStringLiteral("text"), c.text}, {QStringLiteral("font"), c.font},
                              {QStringLiteral("flags"), c.flags}, {QStringLiteral("height_mm"), c.value},
                              {QStringLiteral("anchor"), c.anchor}};
    m_slide.on = true;
    return true;
}

bool Document::slideSketchText(int sketch, int curve, double u, double v) {
    const auto found = m_sketches.find(sketch);
    if (!m_slide.on || found == m_sketches.end())
        return false;
    SketchGeometry &g = found->second.geometry;
    const auto k = g.curves.find(curve);
    if (k == g.curves.end())
        return false;
    SketchGeometry::Curve &c = k->second;
    double total = 0;
    bool closed = false;
    const double now = g.textPathPosition(c, {u, v}, total, closed);
    if (total <= 0)
        return false;
    double delta = now - m_slide.from;
    double a = m_slide.anchor;
    if (closed) {
        delta -= std::round(delta / total) * total;
        a = std::fmod(std::fmod(a + delta, total) + total, total);
    } else {
        a = std::clamp(a + delta, 0.0, total);
    }
    c.anchor = a;
    c.flags |= SketchGeometry::TextAnchored;
    sketchesEdited();
    return true;
}

bool Document::endSketchTextSlide(int sketch) {
    if (!m_slide.on)
        return false;
    m_slide.on = false;
    const auto found = m_sketches.find(sketch);
    if (found == m_sketches.end())
        return false;
    const auto k = found->second.geometry.curves.find(m_slide.was.value(QStringLiteral("id")).toInt());
    if (k == found->second.geometry.curves.end())
        return false;
    const SketchGeometry::Curve &c = k->second;
    if ((c.flags == m_slide.was.value(QStringLiteral("flags")).toInt()) && std::abs(c.anchor - m_slide.was.value(QStringLiteral("anchor")).toDouble()) < 1e-9)
        return false;
    recordTextEdit(sketch, m_slide.was.value(QStringLiteral("id")).toInt(), m_slide.was,
                   QStringLiteral("Moved the text \"%1\" along its path").arg(c.text), QStringLiteral("Edited the text \"%1\"").arg(c.text));
    sketchesEdited();
    return true;
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

namespace {

// The Selectable menu's kinds (see Document::sketchPartClass).
constexpr int kLineClass = 1, kPointClass = 2, kConstructionClass = 4, kProjectedClass = 8;

int pointClass(const SketchGeometry &g, int id) {
    return g.projectedPoints.count(id) ? kProjectedClass : kPointClass;
}
int lineClass(const SketchGeometry &g, int id, const SketchGeometry::Line &l) {
    if (g.projectedPoints.count(l.a) && g.projectedPoints.count(l.b))
        return kProjectedClass;
    return g.constructionLines.count(id) ? kConstructionClass : kLineClass;
}
int arcClass(const SketchGeometry &g, int id, const SketchGeometry::Arc &k) {
    if (g.projectedPoints.count(k.c))
        return kProjectedClass;
    return g.constructionArcs.count(id) ? kConstructionClass : kLineClass;
}
// A curve's outline plus, for a text in a box, the box (so it can be picked by its frame).
std::vector<std::vector<SketchGeometry::Point>> withFrame(const SketchGeometry &g, int id) {
    auto paths = g.curvePath(id);
    if (auto frame = g.textFrame(id); !frame.empty())
        paths.push_back(frame);
    return paths;
}

int curveClass(const SketchGeometry &g, int id, const SketchGeometry::Curve &k) {
    if (!k.points.empty() && g.projectedPoints.count(k.points.front()))
        return kProjectedClass;
    return g.constructionCurves.count(id) ? kConstructionClass : kLineClass;
}

} // namespace

int Document::sketchPartClass(int sketch, const QString &kind, int index) const {
    const auto found = m_sketches.find(sketch);
    if (found == m_sketches.end())
        return 0;
    const SketchGeometry &g = found->second.geometry;
    if (kind == QLatin1String("sketch_point"))
        return g.points.count(index) ? pointClass(g, index) : 0;
    if (kind == QLatin1String("sketch_line")) {
        const auto l = g.lines.find(index);
        return l == g.lines.end() ? 0 : lineClass(g, index, l->second);
    }
    if (kind == QLatin1String("sketch_arc")) {
        const auto k = g.arcs.find(index);
        return k == g.arcs.end() ? 0 : arcClass(g, index, k->second);
    }
    if (kind == QLatin1String("sketch_curve")) {
        const auto k = g.curves.find(index);
        return k == g.curves.end() ? 0 : curveClass(g, index, k->second);
    }
    return 0;
}

// Which points the model hides from the camera (see the header). A tree of the triangles makes
// each test fast enough for thousands of points.
QVariantList Document::hiddenFrom(const QVariantList &points, QVector3D eye, bool ortho) const {
    QVariantList out;
    const std::size_t tris = m_mesh.indices.size() / 3;
    if (tris == 0 || m_mesh.positions.empty()) {
        for (int i = 0; i < points.size(); ++i)
            out << false;
        return out;
    }
    if (m_bvh.empty()) {
        m_bvhTriangles.resize(tris);
        for (std::size_t t = 0; t < tris; ++t)
            m_bvhTriangles[t] = int(t);
        std::vector<QVector3D> mid(tris);
        for (std::size_t t = 0; t < tris; ++t)
            mid[t] = (at(m_mesh.positions, m_mesh.indices[t * 3]) + at(m_mesh.positions, m_mesh.indices[t * 3 + 1])
                      + at(m_mesh.positions, m_mesh.indices[t * 3 + 2])) / 3.0f;
        std::function<int(int, int)> build = [&](int from, int to) {
            BvhNode n{};
            for (int k = 0; k < 3; ++k) { n.lo[k] = INFINITY; n.hi[k] = -INFINITY; }
            for (int i = from; i < to; ++i)
                for (int c = 0; c < 3; ++c) {
                    const QVector3D v = at(m_mesh.positions, m_mesh.indices[std::size_t(m_bvhTriangles[i]) * 3 + c]);
                    for (int k = 0; k < 3; ++k) { n.lo[k] = std::min(n.lo[k], v[k]); n.hi[k] = std::max(n.hi[k], v[k]); }
                }
            n.left = n.right = -1;
            n.first = from;
            n.count = to - from;
            const int me = int(m_bvh.size());
            m_bvh.push_back(n);
            if (to - from > 4) {
                int axis = 0;
                for (int k = 1; k < 3; ++k)
                    if (n.hi[k] - n.lo[k] > n.hi[axis] - n.lo[axis])
                        axis = k;
                const int m = (from + to) / 2;
                std::nth_element(m_bvhTriangles.begin() + from, m_bvhTriangles.begin() + m, m_bvhTriangles.begin() + to,
                                 [&](int a, int b) { return mid[std::size_t(a)][axis] < mid[std::size_t(b)][axis]; });
                const int l = build(from, m), r = build(m, to);
                m_bvh[std::size_t(me)].left = l;
                m_bvh[std::size_t(me)].right = r;
                m_bvh[std::size_t(me)].count = 0;
            }
            return me;
        };
        build(0, int(tris));
    }
    const float away = 0.03f;                      // off the point's own surface, in mm
    for (const QVariant &v : points) {
        const QVector3D p = v.value<QVector3D>();
        QVector3D d;
        float reach;
        if (ortho) {
            d = -eye.normalized();
            reach = 1e7f;
        } else {
            d = eye - p;
            reach = d.length();
            if (reach < 1e-6f) {
                out << false;
                continue;
            }
            d /= reach;
        }
        const QVector3D o = p + d * away;
        reach -= away;
        const float inv[3] = {d.x() != 0 ? 1.0f / d.x() : INFINITY, d.y() != 0 ? 1.0f / d.y() : INFINITY,
                              d.z() != 0 ? 1.0f / d.z() : INFINITY};
        bool hidden = false;
        int stack[64], top = 0;
        stack[top++] = 0;
        while (top > 0 && !hidden) {
            const BvhNode &n = m_bvh[std::size_t(stack[--top])];
            float t0 = 0, t1 = reach;
            bool miss = false;
            for (int k = 0; k < 3 && !miss; ++k) {
                if (std::isinf(inv[k])) {
                    miss = o[k] < n.lo[k] || o[k] > n.hi[k];
                    continue;
                }
                float a = (n.lo[k] - o[k]) * inv[k], b = (n.hi[k] - o[k]) * inv[k];
                if (a > b)
                    std::swap(a, b);
                t0 = std::max(t0, a);
                t1 = std::min(t1, b);
                miss = t0 > t1;
            }
            if (miss)
                continue;
            if (n.left < 0) {
                for (int i = n.first; i < n.first + n.count && !hidden; ++i) {
                    const std::size_t t = std::size_t(m_bvhTriangles[std::size_t(i)]);
                    // Like rayTriangle but a hair generous at the triangles' borders, so a ray that runs
                    // exactly along the seam of two of them (axis-aligned views) cannot slip through.
                    const QVector3D ta = at(m_mesh.positions, m_mesh.indices[t * 3]);
                    const QVector3D e1 = at(m_mesh.positions, m_mesh.indices[t * 3 + 1]) - ta;
                    const QVector3D e2 = at(m_mesh.positions, m_mesh.indices[t * 3 + 2]) - ta;
                    const QVector3D pv = QVector3D::crossProduct(d, e2);
                    const float det = QVector3D::dotProduct(e1, pv);
                    if (std::abs(det) < 1e-12f)
                        continue;
                    const QVector3D sv = o - ta;
                    const float u = QVector3D::dotProduct(sv, pv) / det;
                    if (u < -1e-4f || u > 1.0001f)
                        continue;
                    const QVector3D qv = QVector3D::crossProduct(sv, e1);
                    const float vv = QVector3D::dotProduct(d, qv) / det;
                    if (vv < -1e-4f || u + vv > 1.0001f)
                        continue;
                    const float h = QVector3D::dotProduct(e2, qv) / det;
                    hidden = h > 0 && h < reach;
                }
            } else if (top < 62) {
                stack[top++] = n.left;
                stack[top++] = n.right;
            }
        }
        out << hidden;
    }
    return out;
}

QVariantList Document::faceSamples(int face, int most) const {
    QVariantList out;
    const auto &tri = m_mesh.triangleFace;
    std::vector<std::size_t> mine;
    const std::size_t count = std::min(tri.size(), m_mesh.indices.size() / 3);
    for (std::size_t t = 0; t < count; ++t)
        if (int(tri[t]) == face)
            mine.push_back(t);
    if (mine.empty() || most < 1)
        return out;
    const std::size_t step = std::max<std::size_t>(1, mine.size() / std::size_t(most));
    for (std::size_t i = 0; i < mine.size(); i += step) {
        const std::size_t t = mine[i];
        out << (at(m_mesh.positions, m_mesh.indices[t * 3]) + at(m_mesh.positions, m_mesh.indices[t * 3 + 1])
                + at(m_mesh.positions, m_mesh.indices[t * 3 + 2])) / 3.0f;
    }
    return out;
}

// Everything pickable in a sketch as paths in space, for a box drag to test on screen.
QVariantList Document::bodyOutlines(bool edges, bool points) const {
    QVariantList out;
    if (points)
        for (std::size_t i = 0; i < m_mesh.points.size() / 3; ++i)
            out << QVariantMap{{QStringLiteral("kind"), QStringLiteral("point")}, {QStringLiteral("index"), int(i)},
                               {QStringLiteral("paths"), QVariantList{QVariant(QVariantList{QVariant(at(m_mesh.points, i))})}}};
    if (edges)
        for (std::size_t e = 0; e < m_mesh.edges.size(); ++e) {
            const auto &line = m_mesh.edges[e];
            QVariantList run;
            for (std::size_t i = 0; i < line.size() / 3; ++i)
                run << at(line, i);
            out << QVariantMap{{QStringLiteral("kind"), QStringLiteral("edge")}, {QStringLiteral("index"), int(e)},
                               {QStringLiteral("paths"), QVariantList{QVariant(run)}}};
        }
    return out;
}

// {kind: "face", index, paths}: the lines round each face, as runs of points in space. A line of a
// triangle that only one triangle of the face has is on its rim; points are joined by position.
QVariantList Document::faceOutlines() const {
    QVariantList out;
    const auto &tri = m_mesh.triangleFace;
    const std::size_t count = std::min(tri.size(), m_mesh.indices.size() / 3);
    using Key = std::array<long long, 3>;
    std::map<Key, int> welded;
    std::vector<QVector3D> where;
    std::vector<int> weld(m_mesh.positions.size() / 3);
    for (std::size_t i = 0; i < weld.size(); ++i) {
        const Key k{std::llround(m_mesh.positions[i * 3] * 1e3), std::llround(m_mesh.positions[i * 3 + 1] * 1e3),
                    std::llround(m_mesh.positions[i * 3 + 2] * 1e3)};
        const auto it = welded.try_emplace(k, int(where.size()));
        if (it.second)
            where.push_back(at(m_mesh.positions, i));
        weld[i] = it.first->second;
    }
    std::vector<std::map<std::pair<int, int>, int>> rims(m_mesh.faceCount);
    for (std::size_t t = 0; t < count; ++t) {
        if (tri[t] >= m_mesh.faceCount)
            continue;
        for (int e = 0; e < 3; ++e) {
            const int a = weld[m_mesh.indices[t * 3 + e]], b = weld[m_mesh.indices[t * 3 + (e + 1) % 3]];
            if (a != b)
                ++rims[tri[t]][{std::min(a, b), std::max(a, b)}];
        }
    }
    for (std::uint32_t f = 0; f < m_mesh.faceCount; ++f) {
        std::map<int, std::vector<int>> next;       // point -> where its unused rim lines lead
        for (const auto &r : rims[f])
            if (r.second == 1) {
                next[r.first.first].push_back(r.first.second);
                next[r.first.second].push_back(r.first.first);
            }
        QVariantList paths;
        for (auto &start : next)
            while (!start.second.empty()) {
                int from = start.first, to = start.second.back();
                start.second.pop_back();
                auto drop = [&](int a, int b) {
                    auto &v = next[a];
                    v.erase(std::find(v.begin(), v.end(), b));
                };
                drop(to, from);
                QVariantList run{QVariant(where[from]), QVariant(where[to])};
                while (!next[to].empty()) {
                    const int n = next[to].back();
                    next[to].pop_back();
                    drop(n, to);
                    run << where[n];
                    to = n;
                }
                paths << QVariant(run);
            }
        if (!paths.isEmpty())
            out << QVariantMap{{QStringLiteral("kind"), QStringLiteral("face")}, {QStringLiteral("index"), int(f)},
                               {QStringLiteral("paths"), paths}};
    }
    return out;
}

QVariantList Document::sketchOutlines(int sketch, int allow) const {
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
        if (!pointHidden(g, id) && (allow & pointClass(g, id)))
            add("sketch_point", id, {{p}});
    for (const auto &[id, l] : g.lines)
        if (!g.helperLines.count(id) && !lineHidden(g, l.a, l.b) && (allow & lineClass(g, id, l)))
            add("sketch_line", id, {{g.points.at(l.a), g.points.at(l.b)}});
    for (const auto &[id, k] : g.arcs)
        if (!g.helperArcs.count(id) && !lineHidden(g, k.c, k.c) && (allow & arcClass(g, id, k)))
            add("sketch_arc", id, g.arcPath(id));
    for (const auto &[id, k] : g.curves)
        if (allow & curveClass(g, id, k))
            add("sketch_curve", id, withFrame(g, id));
    return out;
}

QVariantMap Document::pickSketch(int sketch, QVector3D origin, QVector3D direction, float pointReach, float edgeReach,
                                 int allow) const {
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
        if (pointHidden(g, id) || !(allow & pointClass(g, id)))
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
        if (g.helperLines.count(id) || lineHidden(g, l.a, l.b) || !(allow & lineClass(g, id, l)))
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
        if (!g.helperArcs.count(id) && !lineHidden(g, k.c, k.c) && (allow & arcClass(g, id, k)))
            along(g.arcPath(id), "sketch_arc", id);
    for (const auto &[id, k] : g.curves)
        if (allow & curveClass(g, id, k))
            along(withFrame(g, id), "sketch_curve", id);
    // A text is grabbed anywhere on its letters, not only on their outlines (owner, t62u): a press
    // inside a letter's body counts as right on it.
    {
        const QVector3D o = s.plane.value(QStringLiteral("origin")).value<QVector3D>();
        const QVector3D x = s.plane.value(QStringLiteral("xAxis")).value<QVector3D>();
        const QVector3D y = s.plane.value(QStringLiteral("yAxis")).value<QVector3D>();
        const QVector3D n = QVector3D::crossProduct(x, y).normalized();
        const float facing = QVector3D::dotProduct(d, n);
        if (std::abs(facing) > 1e-6f) {
            const float t = QVector3D::dotProduct(o - origin, n) / facing;
            if (t > 0) {
                const QVector3D at = origin + d * t - o;
                const double pu = QVector3D::dotProduct(at, x) / x.lengthSquared(), pv = QVector3D::dotProduct(at, y) / y.lengthSquared();
                for (const auto &[id, k] : g.curves) {
                    if (k.type != QLatin1String("text") || !(allow & curveClass(g, id, k)))
                        continue;
                    // Inside a letter, or within a third of the letter height of one: the thin strokes
                    // and the gaps between letters count too, so a press that only misses a stroke
                    // by a hair still grabs the text instead of the line under it (owner, t66u).
                    int crossings = 0;
                    const double band = std::max(0.3 * k.value, 1e-3), band2 = band * band;
                    bool near = false;
                    for (const auto &path : g.curvePath(id))
                        for (std::size_t i = 0; i < path.size(); ++i) {
                            const auto &a = path[i], &b = path[(i + 1) % path.size()];
                            if ((a.v > pv) != (b.v > pv) && pu < a.u + (pv - a.v) / (b.v - a.v) * (b.u - a.u))
                                ++crossings;
                            if (!near) {
                                const double ex = b.u - a.u, ey = b.v - a.v, len2 = ex * ex + ey * ey;
                                const double f = len2 > 1e-12 ? std::clamp(((pu - a.u) * ex + (pv - a.v) * ey) / len2, 0.0, 1.0) : 0.0;
                                const double dx = pu - (a.u + f * ex), dy = pv - (a.v + f * ey);
                                near = dx * dx + dy * dy <= band2;
                            }
                        }
                    if (near)
                        crossings = 1;
                    if (crossings % 2 == 1 && bestScore > 0.0f) {
                        best = id;
                        bestKind = "sketch_curve";
                        bestScore = 0.0f;
                        bestT = t;
                        break;
                    }
                }
            }
        }
    }
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
        const bool between = m_tween && m_tween->sketch == found->first;
        const std::set<int> held = between ? (m_tween->t < 0.5 ? m_tween->heldFrom : m_tween->heldTo) : s.geometry.fullyConstrained();
        const SketchGeometry &g = shownGeometry(found->first, s.geometry);
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
        for (const auto &[id, k] : g.curves) {
            const bool whole = std::all_of(k.points.begin(), k.points.end(), [&](int p) { return held.count(p); });
            tubes(g.curvePath(id), whole, g.constructionCurves.count(id));
            if (const auto frame = g.textFrame(id); !frame.empty())
                tubes({frame}, whole, true);          // a text's box, drawn light like a construction line
        }
        for (const auto &[id, p] : g.points)
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
        if (m_tween && m_tween->sketch == found->first && (m_tween->sameShape || m_tween->actions < 0)) {
            // Part way through a step that only moves things: the shading follows the lines. Drawn as it
            // was before this opening: that state's shading (part way across a step that adds or takes
            // away, the state it starts from: an area fills once its shape is whole).
            const auto [paths, groups] = profilePaths(m_tween->sameShape ? m_tween->shown : m_tween->from);
            for (const profiles::Profile &p : profiles::find(paths, 0, 1e-4, groups).profiles)
                addProfile(out, s, p);
            continue;
        }
        for (const profiles::Profile &p : profilesOf(v.toInt(), s))
            addProfile(out, s, p);
    }
    finish(out);
    target->setMesh(out);
}

// QML's handlers run before any C++ connection to a signal, so the old profiles go first: a
// connection clearing them would leave the shading drawn from the old ones.
void Document::sketchesEdited() {
    if (syncLinks())
        emit historyChanged();       // a link's issue appeared or went: the marks in the timeline
    m_profiles.clear();
    noteLiveMove();
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

QVariantList Document::profileSpots(int sketch) const {
    const auto it = m_sketches.find(sketch);
    if (it == m_sketches.end())
        return {};
    QVariantList out;
    for (const profiles::Profile &p : profilesOf(sketch, it->second)) {
        if (p.outer.size() < 3)
            continue;
        double u = 0, v = 0;
        for (const auto &q : p.outer) {
            u += q.u / double(p.outer.size());
            v += q.v / double(p.outer.size());
        }
        out << QVariantMap{{QStringLiteral("u"), u}, {QStringLiteral("v"), v}};
    }
    return out;
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

// --- Links (owner, 2026-11-01) ---------------------------------------------------------

// The projection from the model: recorded so the history can show a chain and Break Link has
// something to break. The model can't change yet, so nothing is kept in step.
int Document::addBodyLink(SketchGeometry &g, const SketchGeometry &before, bool whole) const {
    SketchGeometry::Link link;
    for (const int p : g.projectedPoints)
        if (!before.projectedPoints.count(p))
            link.owned.insert(p);
    if (link.owned.empty())
        return 0;
    link.id = g.nextLink++;
    link.whole = whole;
    g.links.push_back(link);
    return link.id;
}

int Document::sketchOrder(int id) const {
    for (int i = 0; i < m_actions.size(); ++i) {
        const QJsonObject a = m_actions[i].toObject();
        if (a.value(QStringLiteral("type")).toString() == QLatin1String("sketch") && a.value(QStringLiteral("id")).toInt() == id)
            return i;
    }
    return -1;
}

QVariantList Document::earlierSketches(int sketch) const {
    QVariantList out;
    const int mine = sketchOrder(sketch);
    for (const auto &[id, s] : m_sketches) {
        const int at = sketchOrder(id);
        if (id != sketch && at >= 0 && at < mine)
            out << id;
    }
    return out;
}

namespace {
bool parallelPlanes(const QVariantMap &a, const QVariantMap &b) {
    const auto normal = [](const QVariantMap &p) {
        return QVector3D::crossProduct(p.value(QStringLiteral("xAxis")).value<QVector3D>(),
                                       p.value(QStringLiteral("yAxis")).value<QVector3D>()).normalized();
    };
    return std::abs(QVector3D::dotProduct(normal(a), normal(b))) > 1 - 1e-5f;
}
}

// Brings one link up to date with its source: moves what it follows, adds what the source gained
// (all of it for a whole link, only the ends and corners of the picked lines and arcs for the
// rest, which is how a Fillet or Chamfer on a picked line comes in), takes away the points
// nothing needs now. Nothing of the user's is ever taken away: what the source lost stays,
// and the link says so (red).
void Document::syncLink(const Named &target, SketchGeometry &g, SketchGeometry::Link &L, const Named &source,
                        QString &issue, QString &level) const {
    const SketchGeometry &sg = source.geometry;
    const bool flat = parallelPlanes(source.plane, target.plane);
    const QVector3D tx = target.plane.value(QStringLiteral("xAxis")).value<QVector3D>();
    const QVector3D ty = target.plane.value(QStringLiteral("yAxis")).value<QVector3D>();
    const QVector3D to = target.plane.value(QStringLiteral("origin")).value<QVector3D>();
    const auto flatten = [&](const SketchGeometry::Point &p) {
        const QVector3D d = onSketch(source, p.u, p.v) - to;
        return SketchGeometry::Point{double(QVector3D::dotProduct(d, tx)), double(QVector3D::dotProduct(d, ty))};
    };
    bool moved = false, lost = false;

    if (L.whole) {
        std::set<int> used;
        for (const auto &[id, l] : sg.lines)
            if (!sg.helperLines.count(id)) {
                L.lines.try_emplace(id, 0);
                used.insert(l.a);
                used.insert(l.b);
            }
        if (flat)
            for (const auto &[id, k] : sg.arcs)
                if (!sg.helperArcs.count(id)) {
                    L.arcs.try_emplace(id, 0);
                    used.insert(k.c);
                    used.insert(k.a);
                    used.insert(k.b);
                }
        for (const auto &[id, p] : sg.points)
            if (!sg.isHidden(id) && !used.count(id))
                L.wantPoints.insert(id);
    }

    // Ours for a source point, made if it isn't there; 0 when the user took it off.
    const auto ensure = [&](int sp, bool picked) -> int {
        const auto src = sg.points.find(sp);
        if (src == sg.points.end())
            return 0;
        const SketchGeometry::Point at = flatten(src->second);
        int &mine = L.points[sp];
        if (mine < 0)
            return 0;
        auto it = g.points.find(mine);
        if (mine > 0 && it == g.points.end() && picked) {
            mine = -1;                                   // a point picked by itself, deleted by the user
            return 0;
        }
        if (mine == 0 || it == g.points.end()) {
            mine = g.nextPoint++;
            g.points[mine] = at;
        } else if (std::hypot(it->second.u - at.u, it->second.v - at.v) > 1e-9) {
            it->second = at;
            moved = true;
        }
        g.projectedPoints.insert(mine);
        L.owned.insert(mine);
        if (sg.helperPoints.count(sp))
            g.helperPoints.insert(mine);
        return mine;
    };

    for (auto &[sl, ol] : L.lines) {
        if (ol < 0)
            continue;
        const auto s = sg.lines.find(sl);
        if (s == sg.lines.end()) {
            lost = true;
            continue;
        }
        if (ol > 0 && !g.lines.count(ol)) {
            ol = -1;                                     // the user deleted it: it stays deleted
            continue;
        }
        const int a = ensure(s->second.a, false), b = ensure(s->second.b, false);
        if (!a || !b)
            continue;
        if (ol == 0)
            ol = g.nextLine++;
        g.lines[ol] = {a, b};
        if (sg.constructionLines.count(sl))
            g.constructionLines.insert(ol);
    }
    for (auto &[sa, oa] : L.arcs) {
        if (oa < 0 || !flat)
            continue;
        const auto s = sg.arcs.find(sa);
        if (s == sg.arcs.end()) {
            lost = true;
            continue;
        }
        if (oa > 0 && !g.arcs.count(oa)) {
            oa = -1;
            continue;
        }
        const int c = ensure(s->second.c, false), a = ensure(s->second.a, false);
        const int b = s->second.circle() ? a : ensure(s->second.b, false);
        if (!c || !a || !b)
            continue;
        if (oa == 0)
            oa = g.nextArc++;
        g.arcs[oa] = {c, a, b};
        if (sg.constructionArcs.count(sa))
            g.constructionArcs.insert(oa);
    }
    for (const int sp : L.wantPoints) {
        if (!sg.points.count(sp))
            lost = true;
        else
            ensure(sp, true);
    }

    // The points nothing needs now (a corner a Fillet cut away), unless the user built on them.
    std::set<int> needed;
    for (const auto &[id, l] : g.lines) {
        needed.insert(l.a);
        needed.insert(l.b);
    }
    for (const auto &[id, k] : g.arcs) {
        needed.insert(k.c);
        needed.insert(k.a);
        needed.insert(k.b);
    }
    for (const auto &[id, k] : g.curves)
        needed.insert(k.points.begin(), k.points.end());
    for (const auto &[id, k] : g.constraints) {
        needed.insert(k.first);
        needed.insert(k.second);
        needed.insert(k.third);
    }
    for (auto it = L.points.begin(); it != L.points.end();) {
        const int mine = it->second;
        if (sg.points.count(it->first)) {
            ++it;
            continue;
        }
        if (mine > 0 && needed.count(mine)) {
            lost = true;
            ++it;
            continue;
        }
        if (mine > 0) {
            g.points.erase(mine);
            g.projectedPoints.erase(mine);
            g.helperPoints.erase(mine);
            L.owned.erase(mine);
        }
        L.wantPoints.erase(it->first);
        it = L.points.erase(it);
    }

    issue.clear();
    level.clear();
    if (moved && !g.solve()) {
        level = QStringLiteral("yellow");
        issue = QStringLiteral("What was projected moved, and the constraints on it can't all hold now");
    }
    if (lost) {                                          // wins over yellow: a reference is gone
        level = QStringLiteral("red");
        issue = QStringLiteral("Something this was projected from is gone; what was projected stays as it was");
    }
}

// True when any link's issue changed.
bool Document::syncLinks() {
    bool changed = false;
    for (auto &[tid, T] : m_sketches) {
        SketchGeometry &g = T.geometry;
        for (SketchGeometry::Link &L : g.links) {
            if (L.broken || L.source == 0)
                continue;
            QString issue, level;
            const auto sf = m_sketches.find(L.source);
            if (sf == m_sketches.end()) {
                level = QStringLiteral("red");
                issue = QStringLiteral("The sketch this was projected from is gone; what was projected stays as it was");
            } else {
                syncLink(T, g, L, sf->second, issue, level);
            }
            if (L.issue != issue || L.level != level) {
                L.issue = issue;
                L.level = level;
                changed = true;
            }
        }
    }
    return changed;
}

QVariantMap Document::projectSketchIntoSketch(int sketch, int source, const QVariantList &parts) {
    auto refuse = [](const QString &why) { return QVariantMap{{QStringLiteral("why"), why}}; };
    const auto tf = m_sketches.find(sketch), sf = m_sketches.find(source);
    if (tf == m_sketches.end() || sf == m_sketches.end())
        return refuse(QStringLiteral("That sketch isn't there."));
    if (sketch == source)
        return refuse(QStringLiteral("A sketch can't be projected onto itself."));
    const int from = sketchOrder(source), to = sketchOrder(sketch);
    if (from < 0 || to < 0 || from > to)
        return refuse(QStringLiteral("That sketch was made after this one, so it isn't there to project yet."));
    SketchGeometry &g = tf->second.geometry;
    const SketchGeometry before = g;
    SketchGeometry::Link L;
    L.id = g.nextLink++;
    L.source = source;
    L.whole = parts.isEmpty();
    const SketchGeometry &sg = sf->second.geometry;
    for (const SketchGeometry::Link &other : before.links)
        if (!other.broken && other.source == source && other.whole && L.whole)
            return refuse(QStringLiteral("That whole sketch is already projected here."));
    // A whole sketch after some parts of it: what is there already is shared, not drawn twice.
    if (L.whole)
        for (const SketchGeometry::Link &other : before.links) {
            if (other.broken || other.source != source)
                continue;
            for (const auto &e : other.lines) if (e.second > 0) L.lines.try_emplace(e.first, e.second);
            for (const auto &e : other.arcs) if (e.second > 0) L.arcs.try_emplace(e.first, e.second);
            for (const auto &e : other.points) if (e.second > 0) L.points.try_emplace(e.first, e.second);
        }
    int asked = 0;
    for (const QVariant &v : parts) {
        const QVariantMap p = v.toMap();
        const QString kind = p.value(QStringLiteral("kind")).toString();
        const int index = p.value(QStringLiteral("index")).toInt();
        bool already = false;
        for (const SketchGeometry::Link &other : before.links) {
            if (other.broken || other.source != source)
                continue;
            const auto &m = kind.endsWith(QLatin1String("line")) ? other.lines : kind.endsWith(QLatin1String("arc")) ? other.arcs : other.points;
            const auto at = m.find(index);
            already = already || other.whole || (at != m.end() && at->second > 0)
                      || (kind.endsWith(QLatin1String("point")) && other.wantPoints.count(index));
        }
        if (already)
            continue;
        if (kind.endsWith(QLatin1String("line")) && sg.lines.count(index))
            L.lines.try_emplace(index, 0), ++asked;
        else if (kind.endsWith(QLatin1String("arc")) && sg.arcs.count(index))
            L.arcs.try_emplace(index, 0), ++asked;
        else if (kind.endsWith(QLatin1String("point")) && sg.points.count(index))
            L.wantPoints.insert(index), ++asked;
    }
    if (!L.whole && !asked)
        return refuse(QStringLiteral("That's already projected."));
    g.links.push_back(L);
    QString issue, level;
    syncLink(tf->second, g, g.links.back(), sf->second, issue, level);
    const SketchGeometry::Link &made = g.links.back();
    const auto count = [](const std::map<int, int> &m) { return int(std::count_if(m.begin(), m.end(), [](const auto &e) { return e.second > 0; })); };
    const int lines = count(made.lines), arcs = count(made.arcs), points = int(made.owned.size());
    if (!lines && !arcs && !(made.wantPoints.size() && points)) {
        g = before;
        return refuse(QStringLiteral("There's nothing in that sketch to project."));
    }
    const int id = made.id;
    const QString summary = L.whole
        ? QStringLiteral("Projected the whole of %1 onto the sketch, linked (%2 lines, %3 arcs)").arg(sf->second.name).arg(lines).arg(arcs)
        : QStringLiteral("Projected %1 parts of %2 onto the sketch, linked").arg(asked).arg(sf->second.name);
    const int snapshot = m_nextSnapshot++;
    m_snapshots[snapshot] = before;
    record({{QStringLiteral("type"), QStringLiteral("shape")}, {QStringLiteral("sketch"), sketch},
            {QStringLiteral("snapshot"), snapshot}, {QStringLiteral("link"), id}}, summary);
    sketchesEdited();
    return {{QStringLiteral("summary"), summary}, {QStringLiteral("link"), id}, {QStringLiteral("lines"), lines},
            {QStringLiteral("arcs"), arcs}};
}

// Break Link: what was projected stays, now plain geometry (it can be dragged, constrained and
// dimensioned), and nothing keeps it in step with its source. A step of its own, so Ctrl+Z puts
// the link back.
QVariantMap Document::breakLink(int sketch, int link) {
    auto refuse = [](const QString &why) { return QVariantMap{{QStringLiteral("why"), why}}; };
    const auto found = m_sketches.find(sketch);
    if (found == m_sketches.end())
        return refuse(QStringLiteral("That sketch isn't there."));
    SketchGeometry &g = found->second.geometry;
    // A step that holds several links (one Project run) breaks them all together.
    std::vector<int> ids{link};
    for (const QJsonValue &v : m_actions) {
        const QJsonObject a = v.toObject();
        if (a.value(QStringLiteral("sketch")).toInt() != sketch)
            continue;
        std::vector<int> group;
        for (const QJsonValue &l : a.value(QStringLiteral("links")).toArray())
            group.push_back(l.toInt());
        if (std::find(group.begin(), group.end(), link) != group.end())
            ids = group;
    }
    const SketchGeometry before = g;
    int broke = 0;
    QString from;
    for (const int id : ids) {
        const auto it = std::find_if(g.links.begin(), g.links.end(), [&](const auto &l) { return l.id == id; });
        if (it == g.links.end() || it->broken)
            continue;
        for (const int p : it->owned)
            g.projectedPoints.erase(p);
        if (!broke)
            from = it->source ? m_sketches.count(it->source) ? m_sketches.at(it->source).name : QStringLiteral("a sketch")
                              : QStringLiteral("the model");
        it->broken = true;
        it->issue.clear();
        it->level.clear();
        ++broke;
    }
    if (!broke)
        return refuse(QStringLiteral("That link is already broken."));
    const int snapshot = m_nextSnapshot++;
    m_snapshots[snapshot] = before;
    const QString summary = broke == 1 ? QStringLiteral("Broke the link to %1").arg(from)
                                       : QStringLiteral("Broke %1 links").arg(broke);
    record({{QStringLiteral("type"), QStringLiteral("shape")}, {QStringLiteral("sketch"), sketch},
            {QStringLiteral("snapshot"), snapshot}, {QStringLiteral("break_link"), link}}, summary);
    sketchesEdited();
    return {{QStringLiteral("summary"), summary}};
}

void Document::beginProjectRun(int sketch) {
    m_runStart = int(m_actions.size());
    m_runSketch = sketch;
}

// The Project run's item (made by record(): the run's projections are all added to one history
// item, never merged afterwards). `items` is what the tool's badge counts: it words the item.
QVariantMap Document::foldProjectRun(int sketch, int items) {
    const int start = m_runStart;
    if (start < 0 || m_runSketch != sketch || start >= int(m_actions.size()))
        return {};
    QJsonObject run = m_actions[start].toObject();
    if (!run.contains(QStringLiteral("parts")))
        return {};                                   // one projection: it keeps its own words
    const int count = items > 0 ? items : int(run.value(QStringLiteral("parts")).toArray().size());
    const QString summary = count == 1 ? QStringLiteral("Projected 1 item onto the sketch")
                                       : QStringLiteral("Projected %1 items onto the sketch").arg(count);
    run.insert(QStringLiteral("summary"), summary);
    m_dirtyFrom = std::min(m_dirtyFrom, start);
    m_actions[start] = run;
    emit historyChanged();
    return {{QStringLiteral("summary"), summary}};
}

QVariantMap Document::endProjectRun(int sketch, int items) {
    const QVariantMap r = foldProjectRun(sketch, items);
    if (m_runStart >= 0 && m_runStart < int(m_actions.size())) {      // Done: the item appears now
        QJsonObject done = m_actions[m_runStart].toObject();
        if (done.contains(QStringLiteral("pending"))) {
            done.remove(QStringLiteral("pending"));
            m_dirtyFrom = std::min(m_dirtyFrom, m_runStart);
            m_actions[m_runStart] = done;
            qInfo().noquote() << "history:" << done.value(QStringLiteral("summary")).toString();
        }
    }
    m_runStart = -1;
    m_runSketch = 0;
    return r;
}

// Takes the whole run back (its one item), for retaking a pick.
void Document::undoProjectRun() {
    if (m_runStart < 0)
        return;
    const int start = m_runStart;
    while (int(m_actions.size()) > start)
        undo();
    m_runStart = start;
    clearRedo();           // a run cancelled, not undone step by step
}

// The history as the Timeline gets it: a projection's step carries its link (so it can show a
// chain and offer Break Link), and any step or sketch with a link in trouble carries the colour.
QVariantList Document::future() const {
    QVariantList out;
    for (auto it = m_future.rbegin(); it != m_future.rend(); ++it) {
        QVariantMap a = it->action.toObject().toVariantMap();
        a.insert(QStringLiteral("step"), int(m_actions.size()) + int(out.size()));
        a.insert(QStringLiteral("future"), true);
        out << a;
    }
    return out;
}

QVariantList Document::history() const {
    QVariantList out;
    int step = -1;
    for (const QJsonValue &v : m_actions) {
        ++step;
        QVariantMap a = v.toObject().toVariantMap();
        a.insert(QStringLiteral("step"), step);     // its place in the whole history (pending ones are left out here)
        if (a.value(QStringLiteral("pending")).toBool())
            continue;       // a Project run still going: its one item appears when the user clicks Done
        const QString type = a.value(QStringLiteral("type")).toString();
        if (a.contains(QStringLiteral("link"))) {
            const auto s = m_sketches.find(a.value(QStringLiteral("sketch")).toInt());
            if (s != m_sketches.end()) {
                // One link, or the several of a Project run: broken when all are, the worst colour shows.
                std::vector<int> ids{a.value(QStringLiteral("link")).toInt()};
                if (a.contains(QStringLiteral("links"))) {
                    ids.clear();
                    for (const QVariant &l : a.value(QStringLiteral("links")).toList())
                        ids.push_back(l.toInt());
                }
                bool seen = false, allBroken = true;
                int source = 0;
                QString level, text;
                for (const auto &l : s->second.geometry.links) {
                    if (std::find(ids.begin(), ids.end(), l.id) == ids.end())
                        continue;
                    if (!seen)
                        source = l.source;
                    seen = true;
                    allBroken = allBroken && l.broken;
                    if (!l.level.isEmpty() && level != QLatin1String("red")) {
                        level = l.level;
                        text = l.issue;
                    }
                }
                if (seen) {
                    a.insert(QStringLiteral("link_broken"), allBroken);
                    a.insert(QStringLiteral("link_source"), source);
                    if (!level.isEmpty()) {
                        a.insert(QStringLiteral("issue"), level);
                        a.insert(QStringLiteral("issue_text"), text);
                    }
                }
            }
        }
        if (type == QLatin1String("sketch")) {
            const auto s = m_sketches.find(a.value(QStringLiteral("id")).toInt());
            if (s != m_sketches.end())
                for (const auto &l : s->second.geometry.links) {
                    if (l.level.isEmpty() || (a.value(QStringLiteral("issue")).toString() == QLatin1String("red") && l.level == QLatin1String("yellow")))
                        continue;
                    a.insert(QStringLiteral("issue"), l.level);
                    a.insert(QStringLiteral("issue_text"), l.issue);
                }
        }
        out << a;
    }
    return out;
}

QJsonArray Document::linksJson(const SketchGeometry &g) {
    const auto map = [](const std::map<int, int> &m) {
        QJsonArray out;
        for (const auto &[from, to] : m)
            out.append(QJsonArray{from, to});
        return out;
    };
    const auto set = [](const std::set<int> &s) {
        QJsonArray out;
        for (const int i : s)
            out.append(i);
        return out;
    };
    QJsonArray out;
    for (const SketchGeometry::Link &l : g.links)
        out.append(QJsonObject{{QStringLiteral("id"), l.id}, {QStringLiteral("source"), l.source}, {QStringLiteral("whole"), l.whole},
                               {QStringLiteral("broken"), l.broken}, {QStringLiteral("points"), map(l.points)},
                               {QStringLiteral("lines"), map(l.lines)}, {QStringLiteral("arcs"), map(l.arcs)},
                               {QStringLiteral("want_points"), set(l.wantPoints)}, {QStringLiteral("owned"), set(l.owned)}});
    return out;
}

void Document::readLinks(SketchGeometry &g, const QJsonArray &list) {
    const auto map = [](const QJsonValue &v, std::map<int, int> &m) {
        for (const QJsonValue &e : v.toArray())
            m[e.toArray().at(0).toInt()] = e.toArray().at(1).toInt();
    };
    const auto set = [](const QJsonValue &v, std::set<int> &s) {
        for (const QJsonValue &e : v.toArray())
            s.insert(e.toInt());
    };
    for (const QJsonValue &x : list) {
        const QJsonObject o = x.toObject();
        SketchGeometry::Link l;
        l.id = o.value(QStringLiteral("id")).toInt();
        l.source = o.value(QStringLiteral("source")).toInt();
        l.whole = o.value(QStringLiteral("whole")).toBool();
        l.broken = o.value(QStringLiteral("broken")).toBool();
        map(o.value(QStringLiteral("points")), l.points);
        map(o.value(QStringLiteral("lines")), l.lines);
        map(o.value(QStringLiteral("arcs")), l.arcs);
        set(o.value(QStringLiteral("want_points")), l.wantPoints);
        set(o.value(QStringLiteral("owned")), l.owned);
        g.links.push_back(l);
        g.nextLink = std::max(g.nextLink, l.id + 1);
    }
}

// The model's body that picks of faces, edges and corners belong to (one body at a time so far).
int Document::currentBody() const {
    return m_body && m_bodies.count(int(m_body)) ? int(m_body) : 0;
}

// Where a Browser row sits: its own key, then each component above it, nearest first, and not
// the design itself at the top (the tree's dotted underline, owner 2026-11-02).
QStringList Document::lineageOf(const QString &key) const {
    QStringList out;
    int component = -2;
    const int id = key.mid(1).toInt();
    if (key.startsWith(QLatin1Char('b')) && m_bodies.count(id))
        component = m_bodies.at(id).component;
    else if (key.startsWith(QLatin1Char('s')) && m_sketches.count(id))
        component = m_sketches.at(id).component;
    else if (key.startsWith(QLatin1Char('c')))
        component = id;
    else
        return out;
    if (!key.startsWith(QLatin1Char('c')))
        out << key;
    for (int guard = 0; guard < 1000; ++guard) {
        const Component *c = nullptr;
        for (const Component &k : m_components)
            if (k.id == component)
                c = &k;
        if (!c || c->parent < 0)
            break;                                           // the design itself is left out
        out << QStringLiteral("c%1").arg(c->id);
        component = c->parent;
    }
    return out;
}

// The bodies in a component and in the components inside it.
QVariantList Document::bodiesUnder(int componentId) const {
    QVariantList out;
    for (const Component &c : m_components)
        if (c.id == componentId) {
            for (const int b : c.bodies)
                out << b;
            for (const int sub : c.components)
                out << bodiesUnder(sub);
        }
    return out;
}

// ---------------------------------------------------------------------------------------------
// Parameters (owner, 2026-11-02, t91u): named values with formulas that can use each other, used by
// dimensions now and by extrudes and other actions later.
// ---------------------------------------------------------------------------------------------
namespace {

namespace ex = omamake::expr;

const QStringList kParameterUnits = {QStringLiteral("in"), QStringLiteral("ft"), QStringLiteral("mm"), QStringLiteral("cm"),
                                     QStringLiteral("m"), QStringLiteral("deg"), QStringLiteral("rad"), QStringLiteral("none")};

struct ParamEval {
    QString nameError, exprError;
    int exprAt = -1;
    bool ok = false;
    ex::Quantity q;           // base units
    QStringList deps;
    QString shown;
};

ex::Context contextFor(const QString &unit) {
    ex::Context c;
    if (ex::isAngleUnit(unit))
        c.angScale = ex::unitScale(unit);
    else if (kParameterUnits.contains(unit) && unit != QLatin1String("none"))
        c.lenScale = ex::unitScale(unit);
    return c;
}

QString trimmed(double v, int decimals) {
    return QString::number(v, 'f', decimals).replace(QRegularExpression(QStringLiteral("(\\.\\d*?)0+$")), QStringLiteral("\\1"))
        .replace(QRegularExpression(QStringLiteral("\\.$")), QString());
}

QString shownValue(const ex::Quantity &q, const QString &unit) {
    if (unit == QLatin1String("none"))
        return trimmed(q.v, 6);
    const double v = q.v / ex::unitScale(unit);
    if (unit == QLatin1String("in") || unit == QLatin1String("ft"))
        return QString::number(v, 'f', 3);
    if (unit == QLatin1String("deg"))
        return trimmed(v, 2);
    if (unit == QLatin1String("rad"))
        return trimmed(v, 4);
    return trimmed(v, unit == QLatin1String("m") ? 4 : unit == QLatin1String("cm") ? 3 : 2);
}

struct Row {
    int id = 0;
    QString name, unit, expr, comment;
};

// Checks and works out every row: names, formulas, the order they depend on each other.
std::vector<ParamEval> evaluateRows(const std::vector<Row> &rows) {
    const size_t n = rows.size();
    std::vector<ParamEval> out(n);
    std::map<QString, size_t> byName;
    for (size_t i = 0; i < n; ++i) {
        out[i].nameError = ex::nameProblem(rows[i].name);
        if (out[i].nameError.isEmpty()) {
            if (byName.count(rows[i].name))
                out[i].nameError = QStringLiteral("Another parameter is already called \"%1\".").arg(rows[i].name);
            else
                byName[rows[i].name] = i;
        }
        if (!kParameterUnits.contains(rows[i].unit) && out[i].nameError.isEmpty())
            out[i].nameError = QStringLiteral("Pick a unit from the list.");
    }
    std::vector<int> state(n, 0);                 // 0 not yet, 1 working on it, 2 done
    std::function<void(size_t)> work = [&](size_t i) {
        if (state[i] == 2)
            return;
        state[i] = 1;
        ParamEval &e = out[i];
        const Row &row = rows[i];
        const ex::Resolver resolve = [&](const QString &name, ex::Quantity &q, QString &unit, QString &why) {
            const auto it = byName.find(name);
            if (it == byName.end()) {
                why = QStringLiteral("There's no parameter called \"%1\".").arg(name);
                return false;
            }
            if (it->second == i) {
                why = QStringLiteral("\"%1\" can't use itself.").arg(name);
                return false;
            }
            if (state[it->second] == 1) {
                why = QStringLiteral("\"%1\" and \"%2\" use each other, going round in a circle.").arg(row.name, name);
                return false;
            }
            work(it->second);
            const ParamEval &d = out[it->second];
            if (!d.ok) {
                why = QStringLiteral("\"%1\" has a problem, so it can't be used yet.").arg(name);
                return false;
            }
            q = d.q;
            unit = rows[it->second].unit;
            return true;
        };
        const ex::Result r = ex::evaluate(row.expr, contextFor(row.unit), resolve);
        e.deps = r.names;
        state[i] = 2;
        if (!r.ok) {
            e.exprError = r.error;
            e.exprAt = r.errorAt;
            return;
        }
        ex::Quantity q = r.q;
        const bool angle = ex::isAngleUnit(row.unit);
        if (row.unit == QLatin1String("none")) {
            if (q.len || q.ang) {
                e.exprError = QStringLiteral("That works out as %1, but this parameter has No Units. Pick a unit or change the formula.").arg(ex::describe(q));
                return;
            }
        } else if (q.len == 0 && q.ang == 0) {                  // a bare number: in this parameter's unit
            q.v *= ex::unitScale(row.unit);
            (angle ? q.ang : q.len) = 1;
        } else if (angle ? !(q.ang == 1 && q.len == 0) : !(q.len == 1 && q.ang == 0)) {
            e.exprError = QStringLiteral("That works out as %1, but this parameter's unit needs %2.")
                              .arg(ex::describe(q), angle ? QStringLiteral("an angle") : QStringLiteral("a length"));
            return;
        }
        e.q = q;
        e.ok = true;
        e.shown = shownValue(q, row.unit);
    };
    for (size_t i = 0; i < n; ++i) {
        if (!out[i].nameError.isEmpty()) {
            state[i] = 2;
            // its formula is still checked, so a typo shows even before the name is right
            const ex::Result r = ex::evaluate(rows[i].expr, contextFor(rows[i].unit),
                [&](const QString &name, ex::Quantity &, QString &, QString &why) {
                    if (!byName.count(name)) {
                        why = QStringLiteral("There's no parameter called \"%1\".").arg(name);
                        return false;
                    }
                    return true;
                });
            if (!r.ok) {
                out[i].exprError = r.error;
                out[i].exprAt = r.errorAt;
            }
            out[i].deps = r.names;
        }
    }
    for (size_t i = 0; i < n; ++i)
        if (out[i].nameError.isEmpty())
            work(i);
    return out;
}

QVariantMap rowMap(const Row &r) {
    return {{QStringLiteral("id"), r.id}, {QStringLiteral("name"), r.name}, {QStringLiteral("unit"), r.unit},
            {QStringLiteral("expr"), r.expr}, {QStringLiteral("comment"), r.comment}};
}

std::vector<Row> rowsFrom(const QVariantList &list) {
    std::vector<Row> rows;
    for (const QVariant &v : list) {
        const QVariantMap m = v.toMap();
        Row r;
        r.id = m.value(QStringLiteral("id")).toInt();
        r.name = m.value(QStringLiteral("name")).toString().trimmed();
        r.unit = m.value(QStringLiteral("unit")).toString();
        r.expr = m.value(QStringLiteral("expr")).toString().trimmed();
        r.comment = m.value(QStringLiteral("comment")).toString();
        rows.push_back(r);
    }
    return rows;
}

QString renamed(const QString &text, const QString &from, const QString &to) {
    if (from.isEmpty() || from == to)
        return text;
    const QRegularExpression re(QStringLiteral("(?<![\\p{L}\\p{N}_])%1(?![\\p{L}\\p{N}_])").arg(QRegularExpression::escape(from)));
    return QString(text).replace(re, to);
}

// A dimension's formula under the given parameters: its value (mm or radians as the dimension stores
// it, before an angle's flip), the unit it shows in, or why not.
bool dimensionFormula(const QString &text, bool angle, const std::vector<Row> &rows, const std::vector<ParamEval> &eval,
                      double &value, QString &unit, QString &why, QStringList *names = nullptr) {
    std::map<QString, size_t> byName;
    for (size_t i = 0; i < rows.size(); ++i)
        if (eval[i].ok && eval[i].nameError.isEmpty())
            byName[rows[i].name] = i;
    ex::Context ctx;                                    // bare numbers: inches and degrees
    const ex::Result r = ex::evaluate(text, ctx, [&](const QString &name, ex::Quantity &q, QString &u, QString &w) {
        const auto it = byName.find(name);
        if (it == byName.end()) {
            w = QStringLiteral("There's no parameter called \"%1\". Open Parameters to add it.").arg(name);
            return false;
        }
        q = eval[it->second].q;
        u = rows[it->second].unit;
        return true;
    });
    if (names)
        *names = r.names;
    if (!r.ok) {
        why = r.error;
        return false;
    }
    ex::Quantity q = r.q;
    if (q.len == 0 && q.ang == 0) {
        q.v *= angle ? ex::unitScale(QStringLiteral("deg")) : ex::unitScale(QStringLiteral("in"));
        (angle ? q.ang : q.len) = 1;
    }
    if (angle ? !(q.ang == 1 && q.len == 0) : !(q.len == 1 && q.ang == 0)) {
        why = QStringLiteral("That works out as %1, but this dimension is %2.")
                  .arg(ex::describe(q), angle ? QStringLiteral("an angle") : QStringLiteral("a length"));
        return false;
    }
    value = q.v;
    QString first = r.firstUnit;
    if (angle)
        first = QStringLiteral("deg");
    else if (first.isEmpty() || ex::isAngleUnit(first) || first == QLatin1String("none"))
        first = QStringLiteral("in");
    unit = first;
    return true;
}

} // namespace

QVariantList Document::parameters() const {
    std::vector<Row> rows;
    for (const Parameter &p : m_params)
        rows.push_back({p.id, p.name, p.unit, p.expr, p.comment});
    const std::vector<ParamEval> eval = evaluateRows(rows);
    QVariantList out;
    for (size_t i = 0; i < rows.size(); ++i) {
        QVariantMap m = rowMap(rows[i]);
        m.insert(QStringLiteral("shown"), eval[i].ok ? eval[i].shown : QString());
        m.insert(QStringLiteral("ok"), eval[i].ok);
        m.insert(QStringLiteral("error"), !eval[i].nameError.isEmpty() ? eval[i].nameError : eval[i].exprError);
        out << m;
    }
    return out;
}

QVariantList Document::checkParameters(const QVariantList &list) const {
    const std::vector<Row> rows = rowsFrom(list);
    const std::vector<ParamEval> eval = evaluateRows(rows);
    QVariantList out;
    for (size_t i = 0; i < rows.size(); ++i)
        out << QVariantMap{{QStringLiteral("id"), rows[i].id}, {QStringLiteral("nameError"), eval[i].nameError},
                           {QStringLiteral("exprError"), eval[i].exprError}, {QStringLiteral("exprAt"), eval[i].exprAt},
                           {QStringLiteral("shown"), eval[i].ok ? eval[i].shown : QString()}, {QStringLiteral("ok"), eval[i].ok}};
    return out;
}

QString Document::parameterNameProblem(const QString &name) const {
    return ex::nameProblem(name.trimmed());
}

QString Document::renameInExpression(const QString &text, const QString &from, const QString &to) const {
    return renamed(text, from, to);
}

QVariantMap Document::evaluateExpression(const QString &text, const QString &kind) const {
    std::vector<Row> rows;
    for (const Parameter &p : m_params)
        rows.push_back({p.id, p.name, p.unit, p.expr, p.comment});
    const std::vector<ParamEval> eval = evaluateRows(rows);
    double value = 0;
    QString unit, why;
    QStringList names;
    const bool ok = dimensionFormula(text, kind == QLatin1String("angle"), rows, eval, value, unit, why, &names);
    return {{QStringLiteral("ok"), ok}, {QStringLiteral("error"), why}, {QStringLiteral("value"), value},
            {QStringLiteral("unit"), unit}, {QStringLiteral("names"), names}};
}

// Every formula-following dimension takes the value its formula gives under `params` (renamed first
// by `renames`). Sketches are solved; any failure puts everything back and says why.
QString Document::followParameters(const std::vector<Parameter> &params, QJsonArray &dimsWas, QJsonArray &movedWas,
                                   const std::map<QString, QString> &renames) {
    std::vector<Row> rows;
    for (const Parameter &p : params)
        rows.push_back({p.id, p.name, p.unit, p.expr, p.comment});
    const std::vector<ParamEval> eval = evaluateRows(rows);
    std::map<int, SketchGeometry> keep;                  // each touched sketch as it was
    QString problem;
    for (auto &[id, named] : m_sketches) {
        SketchGeometry &g = named.geometry;
        std::set<int> touched;
        bool any = false;
        for (auto &[cid, c] : g.constraints) {
            if (c.expr.isEmpty() || !c.isDimension() || c.driven)
                continue;
            if (!keep.count(id))
                keep[id] = g;
            const QJsonObject was{{QStringLiteral("sketch"), id}, {QStringLiteral("id"), cid}, {QStringLiteral("value"), c.value},
                                  {QStringLiteral("unit"), c.unit}, {QStringLiteral("expr"), c.expr}, {QStringLiteral("expr_flip"), c.exprFlip}};
            QString text = c.expr;
            for (const auto &[from, to] : renames)
                text = renamed(text, from, to);
            double value = 0;
            QString unit, why;
            if (!dimensionFormula(text, c.type == QLatin1String("angle"), rows, eval, value, unit, why)) {
                problem = QStringLiteral("%1 has a dimension whose formula \"%2\" would stop working: %3").arg(named.name, text, why);
                break;
            }
            if (c.type == QLatin1String("angle")) {
                if (!(value > 1e-9 && value < M_PI - 1e-9)) {
                    problem = QStringLiteral("%1 has an angle dimension (\"%2\") that would leave 0 to 180 degrees.").arg(named.name, text);
                    break;
                }
                if (c.exprFlip)
                    value = M_PI - value;
            } else if (!(value > 1e-6)) {
                problem = QStringLiteral("%1 has a dimension (\"%2\") that would come to zero or less.").arg(named.name, text);
                break;
            }
            dimsWas.append(was);
            c.expr = text;
            c.unit = unit;
            if (std::fabs(c.value - value) > 1e-9) {
                c.value = value;
                any = true;
                for (int p : g.pointsOf(c))
                    touched.insert(p);
            }
        }
        if (!problem.isEmpty())
            break;
        if (any && !g.solveNear(touched)) {
            problem = QStringLiteral("%1 can't hold those values with its constraints.").arg(named.name);
            break;
        }
    }
    if (!problem.isEmpty()) {
        for (auto &[id, g] : keep)
            m_sketches[id].geometry = g;
        dimsWas = QJsonArray();
        return problem;
    }
    for (auto &[id, before] : keep)
        for (const auto &[pid, pt] : before.points) {
            const auto now = m_sketches[id].geometry.points.find(pid);
            if (now != m_sketches[id].geometry.points.end() && (std::fabs(now->second.u - pt.u) > 1e-9 || std::fabs(now->second.v - pt.v) > 1e-9))
                movedWas.append(QJsonObject{{QStringLiteral("sketch"), id}, {QStringLiteral("id"), pid},
                                            {QStringLiteral("u"), pt.u}, {QStringLiteral("v"), pt.v}});
        }
    return {};
}

void Document::beginParameterSession() {
    m_paramSession = true;
    m_paramItem = -1;
}

void Document::endParameterSession() {
    m_paramSession = false;
    m_paramItem = -1;
}

QString Document::applyParameters(const QVariantList &list) {
    std::vector<Row> rows = rowsFrom(list);
    const std::vector<ParamEval> eval = evaluateRows(rows);
    for (size_t i = 0; i < rows.size(); ++i) {
        const QString what = rows[i].name.isEmpty() ? QStringLiteral("A parameter") : QStringLiteral("\"%1\"").arg(rows[i].name);
        if (!eval[i].nameError.isEmpty())
            return QStringLiteral("%1: %2").arg(what, eval[i].nameError);
        if (!eval[i].exprError.isEmpty())
            return QStringLiteral("%1: %2").arg(what, eval[i].exprError);
    }
    std::map<int, const Parameter *> old;
    for (const Parameter &p : m_params)
        old[p.id] = &p;
    std::map<QString, QString> renames;
    for (const Row &r : rows) {
        const auto it = old.find(r.id);
        if (r.id && it != old.end() && it->second->name != r.name)
            renames[it->second->name] = r.name;
    }
    std::vector<Parameter> next;
    int nextId = m_nextParameter;
    for (Row &r : rows) {
        if (!r.id)
            r.id = nextId++;
        next.push_back({r.id, r.name, r.unit, r.expr, r.comment});
    }
    // What changed compared with `base` (the parameters before this window session began, or
    // just now), as the history line says it; empty when nothing differs.
    const auto summarize = [&](const std::vector<Parameter> &base, const std::vector<Row> &nextRows, const std::vector<ParamEval> &nowEval) {
        std::map<int, const Parameter *> was;
        for (const Parameter &p : base)
            was[p.id] = &p;
        QStringList added, changed, removed;
        for (const Parameter &p : next) {
            const auto it = was.find(p.id);
            if (it == was.end())
                added << p.name;
            else if (it->second->name != p.name || it->second->unit != p.unit || it->second->expr != p.expr || it->second->comment != p.comment)
                changed << p.name;
        }
        for (const Parameter &p : base)
            if (std::none_of(next.begin(), next.end(), [&](const Parameter &q) { return q.id == p.id; }))
                removed << p.name;
        const auto valueOf = [&](const QString &name) {
            for (size_t i = 0; i < nextRows.size(); ++i)
                if (nextRows[i].name == name)
                    return QStringLiteral("%1 %2").arg(nowEval[i].shown, nextRows[i].unit == QLatin1String("none") ? QString() : nextRows[i].unit).trimmed();
            return QString();
        };
        const int total = added.size() + changed.size() + removed.size();
        if (total == 0)
            return QString();
        if (total > 1)
            return QStringLiteral("Changed %1 parameters").arg(total);
        if (!added.isEmpty())
            return QStringLiteral("Added parameter %1 = %2").arg(added[0], valueOf(added[0]));
        if (!removed.isEmpty())
            return QStringLiteral("Removed parameter %1").arg(removed[0]);
        return QStringLiteral("Changed parameter %1 to %2").arg(changed[0], valueOf(changed[0]));
    };
    std::vector<Row> nextRows;
    for (const Parameter &p : next)
        nextRows.push_back({p.id, p.name, p.unit, p.expr, p.comment});
    const std::vector<ParamEval> nowEval = evaluateRows(nextRows);
    if (summarize(m_params, nextRows, nowEval).isEmpty())
        return {};

    QJsonArray dimsWas, movedWas;
    const QString why = followParameters(next, dimsWas, movedWas, renames);
    if (!why.isEmpty()) {
        // a removed or broken parameter that a dimension uses: say which one
        return why;
    }
    QJsonArray was;
    for (const Parameter &p : m_params)
        was.append(QJsonObject{{QStringLiteral("id"), p.id}, {QStringLiteral("name"), p.name}, {QStringLiteral("unit"), p.unit},
                               {QStringLiteral("expr"), p.expr}, {QStringLiteral("comment"), p.comment}});
    m_params = next;
    m_nextParameter = nextId;

    // One window session = ONE history item (owner, t104u): the edits made while the Parameters
    // window stays open join the item the first of them made, which keeps the state from before
    // the session. If they end up back where the session began, the item goes away entirely.
    const bool merge = m_paramSession && m_paramItem >= 0 && m_paramItem == int(m_actions.size()) - 1
        && m_actions.last().toObject().value(QStringLiteral("type")).toString() == QLatin1String("parameters");
    if (merge) {
        QJsonObject item = m_actions[m_paramItem].toObject();
        const auto addNew = [](QJsonArray &into, const QJsonArray &more) {
            for (const QJsonValue &v : more) {
                const QJsonObject o = v.toObject();
                bool known = false;
                for (const QJsonValue &w : into)
                    if (w.toObject().value(QStringLiteral("sketch")) == o.value(QStringLiteral("sketch"))
                        && w.toObject().value(QStringLiteral("id")) == o.value(QStringLiteral("id")))
                        known = true;
                if (!known)
                    into.append(o);
            }
        };
        QJsonArray dims = item.value(QStringLiteral("dims")).toArray(), moved = item.value(QStringLiteral("moved")).toArray();
        addNew(dims, dimsWas);
        addNew(moved, movedWas);
        item.insert(QStringLiteral("dims"), dims);
        item.insert(QStringLiteral("moved"), moved);
        std::vector<Parameter> base;
        for (const QJsonValue &v : item.value(QStringLiteral("was")).toArray()) {
            const QJsonObject o = v.toObject();
            base.push_back({o.value(QStringLiteral("id")).toInt(), o.value(QStringLiteral("name")).toString(),
                            o.value(QStringLiteral("unit")).toString(), o.value(QStringLiteral("expr")).toString(),
                            o.value(QStringLiteral("comment")).toString()});
        }
        const QString summary = summarize(base, nextRows, nowEval);
        if (summary.isEmpty()) {
            m_dirtyFrom = std::min(m_dirtyFrom, m_paramItem);
            m_actions[m_paramItem] = item;
            m_paramItem = -1;
            undo();     // back where the session began: take the item (and what it moved) away
            qInfo().noquote() << "history: parameters back as they were, item removed";
            emit parametersChanged();
            return {};
        }
        item.insert(QStringLiteral("summary"), summary);
        item.insert(QStringLiteral("at"), QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
        m_dirtyFrom = std::min(m_dirtyFrom, m_paramItem);
        m_actions[m_paramItem] = item;
        qInfo().noquote() << "history: parameters item now:" << summary;
        emit historyChanged();
    } else {
        record({{QStringLiteral("type"), QStringLiteral("parameters")}, {QStringLiteral("was"), was},
                {QStringLiteral("dims"), dimsWas}, {QStringLiteral("moved"), movedWas}},
               summarize(std::vector<Parameter>(), nextRows, nowEval).isEmpty() ? QString() : [&] {
                   std::vector<Parameter> base;
                   for (const QJsonValue &v : was) {
                       const QJsonObject o = v.toObject();
                       base.push_back({o.value(QStringLiteral("id")).toInt(), o.value(QStringLiteral("name")).toString(),
                                       o.value(QStringLiteral("unit")).toString(), o.value(QStringLiteral("expr")).toString(),
                                       o.value(QStringLiteral("comment")).toString()});
                   }
                   return summarize(base, nextRows, nowEval);
               }());
        m_paramItem = m_paramSession ? int(m_actions.size()) - 1 : -1;
    }
    sketchesEdited();
    emit parametersChanged();
    return {};
}

// ---- Parameters as a spreadsheet file (ODS or CSV) -------------------------------------------

namespace {

QString localPath(const QString &path) {
    return path.startsWith(QLatin1String("file://")) ? QUrl(path).toLocalFile() : path;
}

// What a file calls a unit, as one of ours ("" when it is not one).
QString unitFromFile(const QString &text) {
    const QString u = text.trimmed().toLower();
    static const QMap<QString, QString> names{
        {"", "none"}, {"no units", "none"}, {"nounits", "none"}, {"none", "none"}, {"unitless", "none"}, {"ul", "none"},
        {"in", "in"}, {"inch", "in"}, {"inches", "in"}, {"\"", "in"},
        {"ft", "ft"}, {"foot", "ft"}, {"feet", "ft"}, {"'", "ft"},
        {"mm", "mm"}, {"millimeter", "mm"}, {"millimeters", "mm"}, {"millimetre", "mm"}, {"millimetres", "mm"},
        {"cm", "cm"}, {"centimeter", "cm"}, {"centimeters", "cm"}, {"centimetre", "cm"}, {"centimetres", "cm"},
        {"m", "m"}, {"meter", "m"}, {"meters", "m"}, {"metre", "m"}, {"metres", "m"},
        {"deg", "deg"}, {"degree", "deg"}, {"degrees", "deg"}, {"\u00b0", "deg"},
        {"rad", "rad"}, {"radian", "rad"}, {"radians", "rad"}};
    return names.value(u);
}

}   // namespace

QString Document::exportParameters(const QString &path, const QVariantList &list) const {
    const QString target = localPath(path);
    if (target.isEmpty())
        return QStringLiteral("No file name.");
    const std::vector<Row> rows = rowsFrom(list);
    const std::vector<ParamEval> eval = evaluateRows(rows);
    std::vector<omamake::parameterfile::Cells> cells;
    for (size_t i = 0; i < rows.size(); ++i) {
        omamake::parameterfile::Cells c;
        c.name = rows[i].name;
        c.unit = rows[i].unit == QLatin1String("none") ? QStringLiteral("No Units") : rows[i].unit;
        c.expr = rows[i].expr;
        c.comment = rows[i].comment;
        bool number = false;
        const double v = eval[i].ok ? eval[i].shown.toDouble(&number) : 0;
        if (number)
            c.value = QString::number(v, 'g', 12);
        cells.push_back(c);
    }
    return omamake::parameterfile::write(target, cells);
}

QVariantMap Document::importParameters(const QString &path) const {
    std::vector<omamake::parameterfile::Cells> cells;
    const QString why = omamake::parameterfile::read(localPath(path), cells);
    QVariantList rows;
    QStringList skipped;
    for (const auto &c : cells) {
        const QString unit = unitFromFile(c.unit);
        if (unit.isEmpty()) {
            skipped << QStringLiteral("%1: the unit \"%2\" isn't one OmaMake has").arg(c.name, c.unit);
            continue;
        }
        rows << QVariantMap{{QStringLiteral("name"), c.name}, {QStringLiteral("unit"), unit},
                            {QStringLiteral("expr"), c.expr}, {QStringLiteral("comment"), c.comment}};
    }
    return {{QStringLiteral("error"), why}, {QStringLiteral("rows"), rows}, {QStringLiteral("skipped"), skipped}};
}

// ---------------------------------------------------------------------------------------------
// EXTRUDE (owner, 2026-11-02; GB-042). Several areas on ONE plane, pushed out by a distance each
// side; every side is New Body, New Component, Join or Cut (auto picks from what the pushed shape
// meets). Bodies are only looked at by box first, so many bodies stay fast.

const omamake::Mesh &Document::bodyMesh(int id) const {
    auto found = m_meshCache.find(id);
    if (found == m_meshCache.end())
        found = m_meshCache.emplace(id, m_engine->mesh(omamake::BodyId(id))).first;
    return found->second;
}

// Which steps of a profile's loops lie on one of the sketch's arcs: arcs reach a profile as points at most
// 3.75 degrees apart (arcPath), so two neighbours on one circle, that close round it, are on it.
// To Object: where the part lies along the normal, measured from the areas' middle. A face counts where
// the line through the middle along the normal meets it (or, missing it, its own middle); an edge or a
// sketch line at its point farthest from the profile's plane, so a slanted one is reached all along
// (owner, 2026-11-03; the offset brings it back); a point exactly. The extrude ends flat there.
bool Document::extrudeReach(const QVariantMap &part, const double exactStart[3], const double exactNormal[3], double &mm, QString &why) const {
    const QString kind = part.value(QStringLiteral("kind")).toString();
    const int index = part.value(QStringLiteral("index")).toInt();
    // Measured in doubles (a float misses by 0.000003 mm at 127 mm: owner, 2026-11-03). Points from
    // the drawn mesh are floats themselves; planes, sketches and the origin are exact.
    auto alongExact = [&](const double p[3]) {
        return (p[0] - exactStart[0]) * exactNormal[0] + (p[1] - exactStart[1]) * exactNormal[1] + (p[2] - exactStart[2]) * exactNormal[2];
    };
    auto along = [&](const QVector3D &p) { const double q[3] = {p.x(), p.y(), p.z()}; return alongExact(q); };
    const QVector3D start{float(exactStart[0]), float(exactStart[1]), float(exactStart[2])};
    const QVector3D normal{float(exactNormal[0]), float(exactNormal[1]), float(exactNormal[2])};
    // A point of a sketch at full precision.
    auto onSketchExact = [&](const Named &sk, double u, double v) {
        double o[3], x[3], y[3];
        planeAxes(sk.plane, o, x, y);
        return std::array<double, 3>{o[0] + x[0] * u + y[0] * v, o[1] + x[1] * u + y[1] * v, o[2] + x[2] * u + y[2] * v};
    };
    // Of some points, the one farthest from the profile's plane, either way.
    auto farthestOf = [&](const std::vector<QVector3D> &points) {
        double best = -1;
        for (const QVector3D &p : points)
            if (std::abs(along(p)) > best) {
                best = std::abs(along(p));
                mm = along(p);
            }
        return best >= 0;
    };
    auto at = [](const std::vector<float> &list, std::size_t k) { return QVector3D(list[k * 3], list[k * 3 + 1], list[k * 3 + 2]); };
    // On the exact shape when the engine can (a curved face, an edge, a corner); the mesh, which is
    // single precision and up to 0.01 mm off a curve, only when it can't (owner, 2026-11-04).
    auto exactReach = [&](const std::vector<int> &owners, char what) {
        if (index < 0 || std::size_t(index) >= owners.size())
            return false;
        const int body = owners[std::size_t(index)];
        const int first = int(std::find(owners.begin(), owners.end(), body) - owners.begin());
        return m_engine->reach(omamake::BodyId(body), what, index - first, exactStart, exactNormal, mm);
    };
    if (kind == QLatin1String("face") && index >= 0 && std::uint32_t(index) < m_mesh.faceCount) {
        // A flat face facing the profile's way: its exact plane.
        {
            const int body = m_faceBody[std::size_t(index)];
            const int first = int(std::find(m_faceBody.begin(), m_faceBody.end(), body) - m_faceBody.begin());
            double o[3], n[3];
            if (m_engine->planeOfFace(omamake::BodyId(body), index - first, o, n)
                && std::abs(n[0] * exactNormal[0] + n[1] * exactNormal[1] + n[2] * exactNormal[2]) > 1 - 1e-9) {
                mm = alongExact(o);
                return true;
            }
        }
        if (exactReach(m_faceBody, 'f'))
            return true;
        bool hit = false;
        double sum = 0;
        int count = 0;
        for (std::size_t t = 0; t < m_mesh.triangleFace.size(); ++t) {
            if (m_mesh.triangleFace[t] != std::uint32_t(index))
                continue;
            const QVector3D a = at(m_mesh.positions, m_mesh.indices[t * 3]), b = at(m_mesh.positions, m_mesh.indices[t * 3 + 1]),
                            c = at(m_mesh.positions, m_mesh.indices[t * 3 + 2]);
            sum += along(a) + along(b) + along(c);
            count += 3;
            const QVector3D e1 = b - a, e2 = c - a, h = QVector3D::crossProduct(normal, e2);
            const float det = QVector3D::dotProduct(e1, h);
            if (std::abs(det) < 1e-12f)
                continue;
            const QVector3D sv = start - a;
            const float u = QVector3D::dotProduct(sv, h) / det;
            const QVector3D q = QVector3D::crossProduct(sv, e1);
            const float v = QVector3D::dotProduct(normal, q) / det;
            if (u < -1e-6f || v < -1e-6f || u + v > 1 + 1e-6f)
                continue;
            const double t2 = QVector3D::dotProduct(e2, q) / det;
            if (!hit || std::abs(t2) < std::abs(mm))
                mm = t2;
            hit = true;
        }
        if (!hit && count)
            mm = sum / count;
        return hit || count;
    }
    if (kind == QLatin1String("edge") && index >= 0 && std::size_t(index) < m_mesh.edges.size()) {
        if (exactReach(m_edgeBody, 'e'))
            return true;
        std::vector<QVector3D> points;
        for (std::size_t k = 0; k < m_mesh.edges[std::size_t(index)].size() / 3; ++k)
            points.push_back(at(m_mesh.edges[std::size_t(index)], k));
        return farthestOf(points);
    }
    if (kind == QLatin1String("point") && index >= 0 && std::size_t(index) < m_mesh.points.size() / 3) {
        if (exactReach(m_pointBody, 'p'))
            return true;
        mm = along(at(m_mesh.points, std::size_t(index)));
        return true;
    }
    if (kind.startsWith(QLatin1String("sketch_"))) {
        const auto found = m_sketches.find(part.value(QStringLiteral("sketch")).toInt());
        if (found != m_sketches.end()) {
            const Named &sk = found->second;
            const SketchGeometry &g = sk.geometry;
            std::vector<std::array<double, 3>> points;
            if (kind == QLatin1String("sketch_point") && g.points.count(index)) {
                points.push_back(onSketchExact(sk, g.points.at(index).u, g.points.at(index).v));
            } else if (kind == QLatin1String("sketch_line") && g.lines.count(index)) {
                const auto &a = g.points.at(g.lines.at(index).a), &b = g.points.at(g.lines.at(index).b);
                for (int k = 0; k <= 64; ++k)
                    points.push_back(onSketchExact(sk, a.u + (b.u - a.u) * k / 64, a.v + (b.v - a.v) * k / 64));
            } else if (kind == QLatin1String("sketch_arc") && g.arcs.count(index)) {
                for (const auto &path : g.arcPath(index))
                    for (const auto &p : path)
                        points.push_back(onSketchExact(sk, p.u, p.v));
            }
            double best = -1;
            for (const auto &p : points)
                if (std::abs(alongExact(p.data())) > best) {
                    best = std::abs(alongExact(p.data()));
                    mm = alongExact(p.data());
                }
            if (best >= 0)
                return true;
        }
    }
    if (kind == QLatin1String("profile")) {           // an area of another sketch: that sketch's plane, where clicked
        const auto found = m_sketches.find(part.value(QStringLiteral("sketch")).toInt());
        if (found != m_sketches.end()) {
            mm = alongExact(onSketchExact(found->second, part.value(QStringLiteral("u")).toDouble(), part.value(QStringLiteral("v")).toDouble()).data());
            return true;
        }
    }
    if (kind == QLatin1String("origin")) {
        const QString name = part.value(QStringLiteral("name")).toString();
        // Everything of the origin passes through its point; a plane must face the way the profile
        // does, an axis lie across it, or "how far" means nothing.
        const QVector3D way = name.contains(QLatin1Char('X')) && name.contains(QLatin1Char('Y')) ? QVector3D(0, 0, 1)
                            : name.contains(QLatin1Char('X')) && name.contains(QLatin1Char('Z')) ? QVector3D(0, 1, 0)
                            : name.contains(QLatin1Char('Y')) && name.contains(QLatin1Char('Z')) ? QVector3D(1, 0, 0)
                            : QVector3D();
        const QVector3D axis = name == QLatin1String("X") ? QVector3D(1, 0, 0) : name == QLatin1String("Y") ? QVector3D(0, 1, 0)
                             : name == QLatin1String("Z") ? QVector3D(0, 0, 1) : QVector3D();
        if (name.size() == 2 && std::abs(QVector3D::dotProduct(way, normal)) < 0.9999f) {
            why = QStringLiteral("That plane isn't parallel to the profile; pick one that is, or a point.");
            return false;
        }
        if (name.size() == 1 && name != QLatin1String("O") && std::abs(QVector3D::dotProduct(axis, normal)) > 1e-4f) {
            why = QStringLiteral("That axis doesn't lie across the profile's way; pick one that does, or a point.");
            return false;
        }
        const double zero[3] = {0, 0, 0};
        mm = alongExact(zero);
        return true;
    }
    why = QStringLiteral("Pick a face, an edge or a point to extrude up to.");
    return false;
}

void Document::arcCentres(const SketchGeometry &g, omamake::Profile &profile) const {
    struct Circle { double u, v, r; std::vector<SketchGeometry::Point> drawn; };
    std::vector<Circle> circles;
    for (const auto &[id, a] : g.arcs)
        if (!g.helperArcs.count(id) && !g.constructionArcs.count(id)) {
            const SketchGeometry::Point c = g.point(a.c);
            const auto path = g.arcPath(id);
            circles.push_back({c.u, c.v, g.radius(id), path.empty() ? std::vector<SketchGeometry::Point>{} : path.front()});
        }
    // Where along an arc's drawn steps a point lies (step k = k, part way a fraction), or -1. A place
    // the outline crossed something on the arc lies on one of these straight steps, a hair inside the
    // circle, so it doesn't pass the radius test; it is still the arc.
    auto onDrawn = [](const Circle &c, const omamake::Point2 &p) {
        const double near = 1e-9 * std::max(1.0, c.r);
        for (std::size_t k = 0; k + 1 < c.drawn.size(); ++k) {
            const double dx = c.drawn[k + 1].u - c.drawn[k].u, dy = c.drawn[k + 1].v - c.drawn[k].v, len2 = dx * dx + dy * dy;
            if (len2 <= 0)
                continue;
            const double f = std::clamp(((p.x - c.drawn[k].u) * dx + (p.y - c.drawn[k].v) * dy) / len2, 0.0, 1.0);
            if (std::hypot(c.drawn[k].u + f * dx - p.x, c.drawn[k].v + f * dy - p.y) <= near)
                return double(k) + f;
        }
        return -1.0;
    };
    const double none = std::numeric_limits<double>::quiet_NaN();
    profile.centres.clear();
    for (const auto &loop : profile.loops) {
        std::vector<omamake::Point2> centres(loop.size(), {none, none});
        for (std::size_t i = 0; i < loop.size(); ++i) {
            const omamake::Point2 &p = loop[i], &q = loop[(i + 1) % loop.size()];
            for (const Circle &c : circles) {
                const double tol = 1e-6 * std::max(1.0, c.r);
                if (std::abs(std::hypot(p.x - c.u, p.y - c.v) - c.r) > tol || std::abs(std::hypot(q.x - c.u, q.y - c.v) - c.r) > tol) {
                    // a piece of one drawn step (an end where the outline crossed the arc)
                    const double at = onDrawn(c, p), to = onDrawn(c, q);
                    if (at >= 0 && to >= 0 && std::abs(to - at) <= 1 + 1e-9 && std::abs(to - at) > 1e-12) {
                        centres[i] = {c.u, c.v};
                        break;
                    }
                    continue;
                }
                double turn = std::abs(std::atan2(q.y - c.v, q.x - c.u) - std::atan2(p.y - c.v, p.x - c.u));
                turn = std::min(turn, 2 * M_PI - turn);
                if (turn <= 3.8 * M_PI / 180) {
                    centres[i] = {c.u, c.v};
                    break;
                }
            }
        }
        profile.centres.push_back(std::move(centres));
    }
}

// A sketch curve exactly, with its parameter lined up with the samples curvePath draws it with
// (sketchgeometry.cpp): false for what has no exact form here (text).
static bool exactCurve(const SketchGeometry &g, const SketchGeometry::Curve &k, omamake::Profile::Curve &out) {
    std::vector<omamake::Point2> at;
    for (const int id : k.points) {
        const SketchGeometry::Point p = g.point(id);
        at.push_back({p.u, p.v});
    }
    if (k.type == QLatin1String("ellipse") && at.size() == 3) {
        // Sample i at angle 2πi/96: centre + la cos t (long axis) + lb sin t (long axis turned a quarter).
        const double ax = at[1].x - at[0].x, ay = at[1].y - at[0].y;
        const double la = std::hypot(ax, ay), lb = std::hypot(at[2].x - at[0].x, at[2].y - at[0].y);
        if (la < 1e-9 || lb < 1e-9)
            return false;
        out.ellipse = true;
        out.centre = at[0];
        out.scale = 2 * M_PI / 96;
        if (la >= lb) {
            out.major = at[1];
            out.minor = lb;
        } else {
            // The short way is the long one: the same points, from the turned axis, a quarter turn behind.
            out.major = {at[0].x - ay / la * lb, at[0].y + ax / la * lb};
            out.minor = la;
            out.shift = -M_PI / 2;
        }
        return true;
    }
    if (k.type == QLatin1String("conic") && at.size() == 3) {
        // A rational quadratic Bézier (start, corner, end), sample i at t = i/48.
        const double rho = std::clamp(k.value, 0.01, 0.99);
        out.degree = 2;
        out.poles = {at[0], at[2], at[1]};
        out.weights = {1, rho / (1 - rho), 1};
        out.knots = {0, 48};
        out.mults = {3, 3};
        return true;
    }
    if (k.type == QLatin1String("controlspline") && at.size() >= 2) {
        // Clamped, uniform knots on [0, 1], sampled 24 to a point.
        const int n = int(at.size()), p = std::min(3, n - 1), samples = 24 * n;
        out.degree = p;
        out.poles = at;
        out.knots.push_back(0);
        out.mults.push_back(p + 1);
        for (int i = 1; i < n - p; ++i) {
            out.knots.push_back(double(i) / (n - p));
            out.mults.push_back(1);
        }
        out.knots.push_back(1);
        out.mults.push_back(p + 1);
        out.scale = 1.0 / samples;
        return true;
    }
    if (k.type == QLatin1String("fitspline") && at.size() >= 2) {
        // Centripetal Catmull-Rom, ends doubled: each span a cubic, written as its Bézier (the
        // Barry-Goldman tangents at its two ends), 24 samples to a span.
        std::vector<omamake::Point2> q;
        q.push_back({2 * at[0].x - at[1].x, 2 * at[0].y - at[1].y});
        q.insert(q.end(), at.begin(), at.end());
        q.push_back({2 * at.back().x - at[at.size() - 2].x, 2 * at.back().y - at[at.size() - 2].y});
        auto gap = [](const omamake::Point2 &a, const omamake::Point2 &b) { return std::max(1e-6, std::sqrt(std::hypot(b.x - a.x, b.y - a.y))); };
        out.degree = 3;
        const std::size_t spans = q.size() - 3;
        for (std::size_t i = 1; i + 2 < q.size(); ++i) {
            const omamake::Point2 &p0 = q[i - 1], &p1 = q[i], &p2 = q[i + 1], &p3 = q[i + 2];
            const double d01 = gap(p0, p1), d12 = gap(p1, p2), d23 = gap(p2, p3);
            auto tangent = [](const omamake::Point2 &a, const omamake::Point2 &b, const omamake::Point2 &c, double dab, double dbc) {
                // at b, for a-b-c with knot gaps dab, dbc: (b-a)/dab - (c-a)/(dab+dbc) + (c-b)/dbc
                return omamake::Point2{(b.x - a.x) / dab - (c.x - a.x) / (dab + dbc) + (c.x - b.x) / dbc,
                                       (b.y - a.y) / dab - (c.y - a.y) / (dab + dbc) + (c.y - b.y) / dbc};
            };
            const omamake::Point2 m1 = tangent(p0, p1, p2, d01, d12), m2 = tangent(p1, p2, p3, d12, d23);
            if (i == 1)
                out.poles.push_back(p1);
            out.poles.push_back({p1.x + d12 / 3 * m1.x, p1.y + d12 / 3 * m1.y});
            out.poles.push_back({p2.x - d12 / 3 * m2.x, p2.y - d12 / 3 * m2.y});
            out.poles.push_back(p2);
        }
        for (std::size_t i = 0; i <= spans; ++i) {
            out.knots.push_back(24.0 * double(i));
            out.mults.push_back(i == 0 || i == spans ? 4 : 3);
        }
        return true;
    }
    return false;
}

// Which steps of each loop lie on one of the sketch's curves, and where along it: a step's two ends
// must both be on the same drawn piece of it (two samples next to each other, or a point where the
// outline crossed something, part way between them).
void Document::exactCurves(const SketchGeometry &g, omamake::Profile &profile) const {
    profile.curves.clear();
    profile.onCurve.clear();
    profile.along.clear();
    struct Drawn { std::vector<SketchGeometry::Point> samples; };
    std::vector<Drawn> drawn;
    for (const auto &[id, k] : g.curves) {
        if (g.constructionCurves.count(id))
            continue;
        omamake::Profile::Curve c;
        if (!exactCurve(g, k, c))
            continue;
        const auto paths = g.curvePath(id);
        if (paths.size() != 1 || paths[0].size() < 2)
            continue;
        profile.curves.push_back(std::move(c));
        drawn.push_back({paths[0]});
    }
    if (drawn.empty())
        return;
    // Samples by where they are (a 1e-6 mm grid), for the points that are samples themselves.
    const double cell = 1e-6, near = 1e-4;
    std::map<std::pair<long long, long long>, std::vector<std::pair<int, double>>> bySpot;
    for (std::size_t c = 0; c < drawn.size(); ++c)
        for (std::size_t s = 0; s < drawn[c].samples.size(); ++s)
            bySpot[{std::llround(drawn[c].samples[s].u / cell), std::llround(drawn[c].samples[s].v / cell)}].push_back({int(c), double(s)});
    // Every place a point is on a curve: (curve, along).
    auto places = [&](const omamake::Point2 &p) {
        std::vector<std::pair<int, double>> out;
        const auto found = bySpot.find({std::llround(p.x / cell), std::llround(p.y / cell)});
        if (found != bySpot.end())
            return found->second;
        for (std::size_t c = 0; c < drawn.size(); ++c) {
            const auto &s = drawn[c].samples;
            for (std::size_t i = 0; i + 1 < s.size(); ++i) {
                const double dx = s[i + 1].u - s[i].u, dy = s[i + 1].v - s[i].v, len2 = dx * dx + dy * dy;
                if (len2 <= 0)
                    continue;
                const double f = std::clamp(((p.x - s[i].u) * dx + (p.y - s[i].v) * dy) / len2, 0.0, 1.0);
                if (std::hypot(s[i].u + f * dx - p.x, s[i].v + f * dy - p.y) < near)
                    out.push_back({int(c), double(i) + f});
            }
        }
        return out;
    };
    for (std::size_t l = 0; l < profile.loops.size(); ++l) {
        const auto &loop = profile.loops[l];
        const std::size_t n = loop.size();
        std::vector<std::vector<std::pair<int, double>>> at(n);
        for (std::size_t i = 0; i < n; ++i)
            at[i] = places(loop[i]);
        std::vector<int> on(n, -1);
        std::vector<std::pair<double, double>> along(n, {0, 0});
        const bool arcs = l < profile.centres.size() && profile.centres[l].size() == n;
        for (std::size_t i = 0; i < n; ++i) {
            if (arcs && !std::isnan(profile.centres[l][i].x))
                continue;                                   // a true arc already
            double best = 2;
            for (const auto &[c, a] : at[i])
                for (const auto &[d, b] : at[(i + 1) % n])
                    if (c == d && std::abs(b - a) <= 1 + 1e-9 && std::abs(b - a) > 1e-12 && std::abs(b - a) < best) {
                        best = std::abs(b - a);
                        on[i] = c;
                        along[i] = {a, b};
                    }
        }
        profile.onCurve.push_back(std::move(on));
        profile.along.push_back(std::move(along));
    }
}

bool Document::readExtrude(const QVariantMap &spec, ExtrudeJob &job, QString &why) const {
    const QVariantList areas = spec.value(QStringLiteral("areas")).toList();
    if (areas.isEmpty()) {
        why = QStringLiteral("Pick a profile or a flat face first.");
        return false;
    }
    for (const QVariant &v : areas) {
        const QVariantMap a = v.toMap();
        omamake::Placement place;
        omamake::Profile profile;
        std::pair<int, int> source{0, -1};
        if (a.value(QStringLiteral("kind")).toString() == QLatin1String("face")) {
            const int face = a.value(QStringLiteral("face")).toInt();
            const int body = bodyOfPart(QStringLiteral("face"), face);
            if (!body) {
                why = QStringLiteral("That face isn't there any more.");
                return false;
            }
            const int first = int(std::find(m_faceBody.begin(), m_faceBody.end(), body) - m_faceBody.begin());
            if (!m_engine->faceProfile(omamake::BodyId(body), face - first, place, profile)) {
                why = QStringLiteral("Only flat faces can be extruded.");
                return false;
            }
            source = {body, face - first};
        } else {
            const auto found = m_sketches.find(a.value(QStringLiteral("sketch")).toInt());
            if (found == m_sketches.end()) {
                why = QStringLiteral("That sketch isn't there any more.");
                return false;
            }
            const auto &all = profilesOf(found->first, found->second);
            const int index = a.value(QStringLiteral("profile")).toInt();
            if (index < 0 || std::size_t(index) >= all.size()) {
                why = QStringLiteral("That profile isn't there any more.");
                return false;
            }
            planeAxes(found->second.plane, place.origin, place.x, place.y);
            auto loop = [](const profiles::Path &path) {
                std::vector<omamake::Point2> out;
                for (const auto &p : path)
                    out.push_back({p.u, p.v});
                return out;
            };
            profile.loops.push_back(loop(all[std::size_t(index)].outer));
            for (const auto &h : all[std::size_t(index)].holes)
                profile.loops.push_back(loop(h));
            arcCentres(found->second.geometry, profile);
            exactCurves(found->second.geometry, profile);
        }
        if (!job.places.empty()) {              // every area on the first one's plane, facing the same way
            const omamake::Placement &f = job.places.front();
            auto vec = [](const double *a) { return QVector3D(float(a[0]), float(a[1]), float(a[2])); };
            const QVector3D n1 = QVector3D::crossProduct(vec(f.x), vec(f.y)).normalized();
            const QVector3D n2 = QVector3D::crossProduct(vec(place.x), vec(place.y)).normalized();
            const float apart = std::abs(QVector3D::dotProduct(vec(place.origin) - vec(f.origin), n1));
            if (QVector3D::dotProduct(n1, n2) < 0.9999f || apart > 1e-3f) {
                why = QStringLiteral("All areas must lie on the same plane, facing the same way.");
                return false;
            }
        }
        job.places.push_back(place);
        job.profiles.push_back(profile);
        job.faces.push_back(source);
    }

    const QString mode = spec.value(QStringLiteral("mode"), QStringLiteral("one")).toString();
    const double d1 = spec.value(QStringLiteral("d1")).toDouble();
    const double d2 = spec.value(QStringLiteral("d2")).toDouble();
    const QVariantList per = spec.value(QStringLiteral("perArea")).toList();
    const std::size_t n = job.places.size();
    auto skipOf = [&](const char *key) {
        std::set<int> out;
        for (const QVariant &v : spec.value(QLatin1String(key)).toList())
            out.insert(v.toInt());
        return out;
    };
    auto opOf = [&](const char *key) {
        const QString op = spec.value(QLatin1String(key), QStringLiteral("auto")).toString();
        return op.isEmpty() ? QStringLiteral("auto") : op;
    };
    // Extent (Fusion's): Distance (the typed length), To Object (up to a picked face, edge or point, plus
    // an offset) or All (out past every shown body, the way the typed length points). Either of the
    // last two sets the length for every area and leaves "distance per area" out.
    // In doubles: how far up to an object is measured from here, and floats miss by 0.000003 mm at
    // 127 mm, enough to leave a line where a join meets a face (owner, 2026-11-03).
    const omamake::Placement &f = job.places.front();
    double exactNormal[3] = {f.x[1] * f.y[2] - f.x[2] * f.y[1], f.x[2] * f.y[0] - f.x[0] * f.y[2], f.x[0] * f.y[1] - f.x[1] * f.y[0]};
    {
        const double l = std::sqrt(exactNormal[0] * exactNormal[0] + exactNormal[1] * exactNormal[1] + exactNormal[2] * exactNormal[2]);
        for (double &c : exactNormal)
            c /= l;
    }
    double exactMiddle[3] = {0, 0, 0};
    for (std::size_t i = 0; i < n; ++i) {
        double lo[2] = {1e300, 1e300}, hi[2] = {-1e300, -1e300};
        for (const auto &q : job.profiles[i].loops.front()) {
            lo[0] = std::min(lo[0], q.x); hi[0] = std::max(hi[0], q.x);
            lo[1] = std::min(lo[1], q.y); hi[1] = std::max(hi[1], q.y);
        }
        const omamake::Placement &p = job.places[i];
        for (int k = 0; k < 3; ++k)
            exactMiddle[k] += (p.origin[k] + p.x[k] * (lo[0] + hi[0]) / 2 + p.y[k] * (lo[1] + hi[1]) / 2) / double(n);
    }
    const QVector3D normal{float(exactNormal[0]), float(exactNormal[1]), float(exactNormal[2])};
    const QVector3D middle{float(exactMiddle[0]), float(exactMiddle[1]), float(exactMiddle[2])};
    auto farthest = [&](double way) {          // the furthest any shown body reaches that way
        double most = 0;
        for (const auto &[id, named] : m_bodies) {
            Q_UNUSED(named)
            if (m_hidden.count(QStringLiteral("b%1").arg(id)))
                continue;
            const omamake::Mesh &bm = bodyMesh(id);
            for (int c = 0; c < 8; ++c) {
                const QVector3D corner(c & 1 ? bm.max[0] : bm.min[0], c & 2 ? bm.max[1] : bm.min[1], c & 4 ? bm.max[2] : bm.min[2]);
                most = std::max(most, double(QVector3D::dotProduct(corner - middle, normal)) * way);
            }
        }
        return most;
    };
    // The length side `k` goes along the normal (side 2's way is reversed by the caller), or NaN.
    auto reach = [&](int k, double typed, double &out) {
        const QString extent = spec.value(k == 0 ? QStringLiteral("extent1") : QStringLiteral("extent2")).toString();
        if (extent == QLatin1String("all")) {
            const double way = (typed < 0 ? -1 : 1) * (k == 0 ? 1 : -1);
            const double most = farthest(way);
            if (most < 1e-3) {
                why = QStringLiteral("No body lies that way to go through; click All again to go the other way.");
                return false;
            }
            out = (most + 0.01) * (typed < 0 ? -1 : 1);       // a hair past, so a cut leaves no skin
            return true;
        }
        if (extent == QLatin1String("object")) {
            const QVariantMap target = spec.value(k == 0 ? QStringLiteral("target1") : QStringLiteral("target2")).toMap();
            if (target.isEmpty()) {
                why = QStringLiteral("Click a face, edge or point to extrude up to.");
                return false;
            }
            double r = 0;
            if (!extrudeReach(target, exactMiddle, exactNormal, r, why))
                return false;
            if (k == 1)
                r = -r;
            const double offset = spec.value(k == 0 ? QStringLiteral("offset1") : QStringLiteral("offset2")).toDouble();
            out = r + (r < 0 ? -offset : offset);
            if (std::abs(out) < 1e-6) {
                why = QStringLiteral("That lies on the profile's own plane; pick something in front of it or behind it.");
                return false;
            }
            return true;
        }
        out = typed;
        return true;
    };
    for (const char *key : {"taper1", "taper2"}) {
        const double t = spec.value(QLatin1String(key)).toDouble();
        if (std::abs(t) < 1e-9)
            continue;
        if (std::abs(t) >= 89) {
            why = QStringLiteral("A taper must be less than 89°.");
            return false;
        }
        if (m_engine->name().rfind("Lite", 0) == 0) {
            why = QStringLiteral("Taper needs the Heavy engine (Settings, Engine).");
            return false;
        }
    }
    double reach1 = d1, reach2 = d2;
    const bool fixed1 = spec.value(QStringLiteral("extent1")).toString() == QLatin1String("all")
                     || spec.value(QStringLiteral("extent1")).toString() == QLatin1String("object");
    if (!reach(0, d1, reach1))
        return false;
    if (mode == QLatin1String("symmetric") && spec.value(QStringLiteral("extent1")).toString() == QLatin1String("all"))
        reach1 = std::max(farthest(1), farthest(-1)) + 0.01;
    if (mode == QLatin1String("two") && !reach(1, d2, reach2))
        return false;

    ExtrudeSide first;
    first.op = opOf("op1");
    first.skip = skipOf("skip1");
    first.taper = spec.value(QStringLiteral("taper1")).toDouble();
    for (std::size_t i = 0; i < n; ++i) {
        const double d = fixed1 ? reach1
                       : (mode == QLatin1String("one") && std::size_t(per.size()) > i) ? per[int(i)].toDouble() : d1;
        if (mode == QLatin1String("symmetric")) {
            const double half = fixed1 ? std::abs(d) : spec.value(QStringLiteral("wholeLength")).toBool() ? d / 2 : d;
            first.from.push_back(-half);
            first.to.push_back(half);
        } else {
            first.from.push_back(0);
            first.to.push_back(d);
        }
    }
    job.sides.push_back(first);
    if (mode == QLatin1String("two")) {
        ExtrudeSide second;
        second.op = opOf("op2");
        second.skip = skipOf("skip2");
        second.taper = spec.value(QStringLiteral("taper2")).toDouble();
        for (std::size_t i = 0; i < n; ++i) {
            second.from.push_back(0);
            second.to.push_back(-reach2);
        }
        job.sides.push_back(second);
    }
    return true;
}

omamake::BodyId Document::extrudeArea(const ExtrudeJob &job, std::size_t area, double from, double to, double taper) const {
    const auto &[body, face] = job.faces[area];
    if (body && face >= 0)
        if (const omamake::BodyId t = m_engine->extrudeFace(omamake::BodyId(body), face, job.places[area], from, to, taper))
            return t;
    return m_engine->extrudeOn(job.profiles[area], job.places[area], from, to, taper);
}

std::vector<omamake::BodyId> Document::extrudeTools(const ExtrudeJob &job, int side, std::vector<std::size_t> *areas,
                                                    QStringList *problems) const {
    std::vector<omamake::BodyId> tools;
    auto missed = [&] {
        const QString why = QString::fromStdString(m_engine->whyNot());
        if (problems && !why.isEmpty() && !problems->contains(why))
            problems->append(why);
    };
    const ExtrudeSide &s = job.sides[std::size_t(side)];
    for (std::size_t i = 0; i < job.places.size(); ++i) {
        if (std::abs(s.to[i] - s.from[i]) < 1e-9)
            continue;
        // A tapered symmetric one is two, each widening (or narrowing) away from the profile.
        if (std::abs(s.taper) > 1e-9 && s.from[i] < 0 && s.to[i] > 0) {
            for (const double end : {s.to[i], s.from[i]})
                if (const omamake::BodyId t = extrudeArea(job, i, 0, end, s.taper)) {
                    tools.push_back(t);
                    if (areas)
                        areas->push_back(i);
                } else {
                    missed();
                }
            continue;
        }
        const omamake::BodyId t = extrudeArea(job, i, s.from[i], s.to[i], s.taper);
        if (t) {
            tools.push_back(t);
            if (areas)
                areas->push_back(i);
        } else {
            missed();
        }
    }
    return tools;
}

namespace {

// Points well inside a profile (inside its outer loop, outside its holes), on a grid over it.
std::vector<omamake::Point2> insidePoints(const omamake::Profile &profile) {
    std::vector<omamake::Point2> out;
    if (profile.loops.empty() || profile.loops.front().size() < 3)
        return out;
    double low[2] = {INFINITY, INFINITY}, high[2] = {-INFINITY, -INFINITY};
    for (const auto &p : profile.loops.front()) {
        low[0] = std::min(low[0], p.x);
        low[1] = std::min(low[1], p.y);
        high[0] = std::max(high[0], p.x);
        high[1] = std::max(high[1], p.y);
    }
    const auto inside = [&](double u, double v) {
        bool in = false;
        for (const auto &loop : profile.loops)
            for (std::size_t i = 0, k = loop.size() - 1; i < loop.size(); k = i++)
                if ((loop[i].y > v) != (loop[k].y > v)
                    && u < (loop[k].x - loop[i].x) * (v - loop[i].y) / (loop[k].y - loop[i].y) + loop[i].x)
                    in = !in;
        return in;
    };
    for (const int steps : {9, 31}) {          // a thin profile can slip between the first grid's points
        for (int a = 0; a < steps; ++a)
            for (int b = 0; b < steps; ++b) {
                const double u = low[0] + (high[0] - low[0]) * (a + 0.5) / steps;
                const double v = low[1] + (high[1] - low[1]) * (b + 0.5) / steps;
                if (inside(u, v))
                    out.push_back({u, v});
            }
        if (!out.empty())
            break;
    }
    return out;
}

// Inside a closed mesh: a ray crosses its skin an odd number of times.
bool insideMesh(const omamake::Mesh &m, const QVector3D &p) {
    for (int k = 0; k < 3; ++k)
        if (p[k] < m.min[k] || p[k] > m.max[k])
            return false;
    const QVector3D dir(0.5773f, 0.5774f, 0.5775f);    // askew, so it doesn't run along an edge
    int crossings = 0;
    const auto at = [&](std::uint32_t i) { return QVector3D(m.positions[i * 3], m.positions[i * 3 + 1], m.positions[i * 3 + 2]); };
    for (std::size_t t = 0; t + 2 < m.indices.size(); t += 3) {
        const QVector3D a = at(m.indices[t]), b = at(m.indices[t + 1]), c = at(m.indices[t + 2]);
        const QVector3D e1 = b - a, e2 = c - a, h = QVector3D::crossProduct(dir, e2);
        const float det = QVector3D::dotProduct(e1, h);
        if (std::abs(det) < 1e-12f)
            continue;
        const QVector3D s = p - a;
        const float u = QVector3D::dotProduct(s, h) / det;
        if (u < 0 || u > 1)
            continue;
        const QVector3D q = QVector3D::crossProduct(s, e1);
        const float v = QVector3D::dotProduct(dir, q) / det;
        if (v < 0 || u + v > 1)
            continue;
        if (QVector3D::dotProduct(e2, q) / det > 0)
            ++crossings;
    }
    return crossings % 2 == 1;
}

} // namespace

// What Auto comes to, from points of each extrude tested against the bodies' meshes: a point of the
// extrude inside a body means Cut, a point just behind its start inside one means it sits against
// that body (Join). Solid booleans gave the exact answer but took seconds, or minutes on a threaded
// body, and this runs again every time the distance changes.
void Document::planExtrude(ExtrudeJob &job) {
    auto vec = [](const double *a) { return QVector3D(float(a[0]), float(a[1]), float(a[2])); };
    for (ExtrudeSide &s : job.sides) {
        std::set<int> hit, touch;
        for (std::size_t i = 0; i < job.places.size(); ++i) {
            const double from = s.from[i], to = s.to[i];
            if (std::abs(to - from) < 1e-9)
                continue;
            const omamake::Placement &pl = job.places[i];
            const QVector3D o = vec(pl.origin), x = vec(pl.x), y = vec(pl.y), n = QVector3D::crossProduct(x, y).normalized();
            const std::vector<omamake::Point2> spots = insidePoints(job.profiles[i]);
            const double way = to > from ? 1 : -1, length = std::abs(to - from);
            const double behind = from - way * std::min(0.02, length * 0.1);
            for (const auto &[id, named] : m_bodies) {
                Q_UNUSED(named)
                if (m_hidden.count(QStringLiteral("b%1").arg(id)))
                    continue;
                const omamake::Mesh &bm = bodyMesh(id);
                bool in = false, against = false;
                for (const omamake::Point2 &p : spots) {
                    const QVector3D flat = o + x * float(p.x) + y * float(p.y);
                    for (const double f : {0.02, 0.25, 0.5, 0.75, 0.98})
                        in = in || insideMesh(bm, flat + n * float(from + (to - from) * f));
                    if (in)
                        break;
                    against = against || insideMesh(bm, flat + n * float(behind));
                }
                if (in)
                    hit.insert(id);
                else if (against)
                    touch.insert(id);
            }
        }
        s.hits.assign(hit.begin(), hit.end());
        s.touches.assign(touch.begin(), touch.end());
        s.resolved = s.op != QLatin1String("auto") ? s.op
                   : !hit.empty() ? QStringLiteral("cut")
                   : !touch.empty() ? QStringLiteral("join") : QStringLiteral("new");
    }
}

QVariantMap Document::extrudePlan(const QVariantMap &spec) {
    ExtrudeJob job;
    QString why;
    if (!readExtrude(spec, job, why))
        return {{QStringLiteral("ok"), false}, {QStringLiteral("why"), why}};
    planExtrude(job);
    auto list = [&](const std::vector<int> &ids) {
        QVariantList out;
        for (const int id : ids)
            out << QVariantMap{{QStringLiteral("body"), id}, {QStringLiteral("name"), m_bodies.at(id).name}};
        return out;
    };
    QVariantList sides;
    for (const ExtrudeSide &s : job.sides)
        sides << QVariantMap{{QStringLiteral("op"), s.resolved}, {QStringLiteral("auto"), s.op == QLatin1String("auto")},
                             {QStringLiteral("hits"), list(s.hits)}, {QStringLiteral("touches"), list(s.touches)}};
    return {{QStringLiteral("ok"), true}, {QStringLiteral("sides"), sides}};
}

// The slow half of an extrude: the solid work only (the engine and the bodies' names, nothing else of
// the document), so it can run off the main thread. Results are applied by applyExtrude.
// Fuzzy profile (owner, 2026-11-06; automatic since 2026-11-07, no longer a tick box). A model whose own faces
// don't quite meet (one converted from a mesh: neighbours up to 0.05 mm apart) can't always be cut exactly:
// where the tool crosses two faces that should share an edge, the two crossings miss each other and nothing
// closes. Every extrude is first worked out exactly; only if a cut, join or intersect couldn't be built is the
// whole extrude worked out again (computeExtrudeTrying), each Boolean then still tried exactly first and only
// on failing taken again with points this close (mm) taken as one (0.00004 in). The result must still pass
// every check (sound solid, no looser than what went in). Exact models never get here.
static constexpr double kFuzzyProfile = 0.001;

Document::ExtrudeResult Document::computeExtrude(const ExtrudeJob &job) const {
    ExtrudeResult out;
    std::map<int, QString> was;                  // each body's name as it was
    for (const auto &[id, named] : m_bodies)
        was[id] = named.name;
    std::map<int, omamake::BodyId> current;      // body as it was -> the solid it is now
    auto currentOf = [&](int id) { return current.count(id) ? current[id] : omamake::BodyId(id); };
    auto replace = [&](int original, omamake::BodyId result) {
        if (current.count(original))
            m_engine->release(current[original]);   // an in-between result
        current[original] = result;
    };
    // This extrude, top to bottom, for a failure's log file (owner, 2026-11-06): each step, after what the
    // engine itself did for it.
    QElapsedTimer clock;
    clock.start();
    m_engine->takeTrace();   // anything from before this extrude isn't part of it
    auto step = [&](const QString &what) {
        const QString engine = QString::fromStdString(m_engine->takeTrace()).trimmed();
        if (!engine.isEmpty())
            for (const QString &line : engine.split(QLatin1Char('\n')))
                out.trace << QStringLiteral("      ") + line;
        out.trace << QStringLiteral("[%1 s] %2").arg(clock.elapsed() / 1000.0, 0, 'f', 2).arg(what);
        qInfo().noquote() << QStringLiteral("extrude: [%1 s] %2").arg(clock.elapsed() / 1000.0, 0, 'f', 2).arg(what.left(160));
    };
    auto described = [&](omamake::BodyId body) {
        std::string report;
        const bool sound = m_engine->inspect(body, report);
        for (const QString &line : QString::fromStdString(report).trimmed().split(QLatin1Char('\n')))
            out.trace << QStringLiteral("      ") + line;
        return sound;
    };
    auto xyz = [](const double *p) { return QStringLiteral("(%1, %2, %3)").arg(p[0], 0, 'f', 6).arg(p[1], 0, 'f', 6).arg(p[2], 0, 'f', 6); };
    auto ids = [&](const std::vector<int> &list) {
        QStringList named;
        for (const int id : list)
            named << QStringLiteral("%1 (body %2)").arg(was.count(id) ? was[id] : QStringLiteral("?")).arg(id);
        return named.isEmpty() ? QStringLiteral("none") : named.join(QStringLiteral(", "));
    };
    step(QStringLiteral("extrude: %1 area(s), %2 side(s)").arg(job.places.size()).arg(job.sides.size()));
    step(job.fuzzy ? QStringLiteral("Fuzzy profile: on (if a body's cut, join or intersect can't be built exactly, tried again with points %1 mm apart taken as one)").arg(kFuzzyProfile)
                   : QStringLiteral("Fuzzy profile: off"));
    for (std::size_t i = 0; i < job.places.size(); ++i) {
        const omamake::Placement &pl = job.places[i];
        const omamake::Profile &pr = job.profiles[i];
        QStringList loops;
        for (const auto &loop : pr.loops)
            loops << QString::number(loop.size());
        int arcs = 0;
        for (const auto &loop : pr.centres)
            for (const omamake::Point2 &c : loop)
                arcs += !std::isnan(c.x);
        out.trace << QStringLiteral("  area %1: origin %2, x %3, y %4; %5 loop(s) of %6 point(s), %7 arc step(s), %8 exact curve(s)%9")
                         .arg(i).arg(xyz(pl.origin), xyz(pl.x), xyz(pl.y)).arg(pr.loops.size()).arg(loops.join(QLatin1Char('/'))).arg(arcs).arg(pr.curves.size())
                         .arg(job.faces[i].second >= 0 ? QStringLiteral("; a face of body %1 (face %2)").arg(job.faces[i].first).arg(job.faces[i].second) : QString());
    }
    for (std::size_t si = 0; si < job.sides.size(); ++si) {
        const ExtrudeSide &s = job.sides[si];
        QStringList spans;
        for (std::size_t i = 0; i < s.from.size(); ++i)
            spans << QStringLiteral("%1 to %2 mm").arg(s.from[i], 0, 'f', 6).arg(s.to[i], 0, 'f', 6);
        out.trace << QStringLiteral("  side %1: %2 (asked %3), taper %4 deg; per area %5; reaches %6; touches %7; cutting off for %8")
                         .arg(si).arg(s.resolved, s.op).arg(s.taper).arg(spans.join(QStringLiteral(", ")), ids(s.hits), ids(s.touches),
                                                                       ids(std::vector<int>(s.skip.begin(), s.skip.end())));
    }
    std::vector<std::pair<QString, omamake::BodyId>> spent;   // tools: kept to the end, so a failure's log can describe them
    std::set<int> reached;                                     // bodies an operation was tried on
    // One body's cut or join by every area of a side at once. An area that is a face of this very body
    // is moved itself (its neighbours carried on along their own surfaces, as a local face move does:
    // no second shape, so no line where they'd meet; owner, 2026-11-05: "taking what is already there
    // and pulling it out"). Every other area goes into one boolean, as OpenCascade takes a list of
    // tools: one pass and one check, however many areas a profile makes. The result is checked
    // by the engine (what the boolean itself says it changed, and a valid solid). 0 when it couldn't be
    // done; the body then stays as it was.
    auto applyAll = [&](const ExtrudeSide &s, int id, const std::vector<omamake::BodyId> &tools,
                        const std::vector<std::size_t> &areas, omamake::Combine how) -> omamake::BodyId {
        omamake::BodyId now = currentOf(id);
        const omamake::BodyId start = now;
        std::vector<omamake::BodyId> rest;
        std::vector<std::size_t> restAreas;
        for (std::size_t k = 0; k < tools.size(); ++k) {
            const std::size_t area = areas[k];
            // Face numbers are the body's as it was: only while nothing else has changed it.
            if (now == omamake::BodyId(id) && job.faces[area].first == id && job.faces[area].second >= 0
                && std::abs(s.taper) < 1e-9 && std::abs(s.from[area]) < 1e-9) {
                std::string why;
                if (const omamake::BodyId moved = m_engine->pushFace(now, job.faces[area].second, job.places[area], s.to[area], why)) {
                    qInfo().noquote() << QStringLiteral("extrude: %1's face %2 moved %3 mm, no second shape")
                                             .arg(was[id]).arg(job.faces[area].second).arg(s.to[area], 0, 'f', 4);
                    now = moved;
                    continue;
                }
                qInfo().noquote() << QStringLiteral("extrude: %1's face couldn't be moved (%2); %3 as a separate shape instead")
                                         .arg(was[id], QString::fromStdString(why), QLatin1String(how == omamake::Combine::Cut ? "cut" : "joined"));
                out.notes << QStringLiteral("The face couldn't be moved itself (%1), so it was %2 as a separate shape.")
                                 .arg(QString::fromStdString(why), QLatin1String(how == omamake::Combine::Cut ? "cut" : "joined"));
            }
            rest.push_back(tools[k]);
            restAreas.push_back(area);
        }
        if (rest.empty())
            return now;
        const omamake::BodyId r = m_engine->combine(now, rest, how, job.fuzzy ? kFuzzyProfile : 0);
        if (now != start)
            m_engine->release(now);                 // the face moved first, an in-between result
        return r;
    };

    for (std::size_t si = 0; si < job.sides.size(); ++si) {
        const ExtrudeSide &s = job.sides[si];
        std::vector<std::size_t> areas;
        QStringList problems;
        std::vector<omamake::BodyId> tools = extrudeTools(job, int(si), &areas, &problems);
        {
            QStringList made;
            for (std::size_t k = 0; k < tools.size(); ++k)
                made << QStringLiteral("solid %1 from area %2").arg(tools[k]).arg(k < areas.size() ? int(areas[k]) : -1);
            step(QStringLiteral("side %1: %2 tool(s) made: %3%4").arg(si).arg(tools.size()).arg(made.join(QStringLiteral(", ")))
                     .arg(problems.isEmpty() ? QString() : QStringLiteral("; problems: ") + problems.join(QStringLiteral("; "))));
        }
        if (tools.empty() && !problems.isEmpty())
            out.failed = true;
        if (!problems.isEmpty())
            out.notes << (tools.empty() ? QStringLiteral("The extrude couldn't be made: %1.") : QStringLiteral("Part of the profile couldn't be extruded: %1."))
                             .arg(problems.join(QStringLiteral("; ")));
        if (tools.empty())
            continue;
        const QString op = s.resolved;
        // A body whose boolean couldn't be built stays as it was, and the note says so.
        // The engine's reason goes with it, said plainly as Fusion does.
        QStringList failed, reasons;
        auto failedOn = [&](int id) {
            out.failed = true;
            out.booleanFailed = true;
            reached.insert(id);
            step(QStringLiteral("side %1: %2 on %3 (body %4) couldn't be built: %5").arg(si).arg(s.resolved, was[id]).arg(id)
                     .arg(QString::fromStdString(m_engine->whyNot())));
            failed << was[id];
            const QString why = QString::fromStdString(m_engine->whyNot());
            if (!why.isEmpty() && !reasons.contains(why))
                reasons << why;
        };
        auto sayFailed = [&](const char *what) {
            if (failed.isEmpty())
                return;
            const QString because = reasons.isEmpty() ? QString() : QStringLiteral(": %1").arg(reasons.join(QStringLiteral("; ")));
            out.notes << QStringLiteral("The %1 couldn't be built on %2%3. It was left as it was.")
                             .arg(QLatin1String(what), failed.join(QStringLiteral(", ")), because);
        };
        if (op == QLatin1String("cut")) {
            QStringList names;
            bool any = false;
            for (const int id : s.hits) {
                if (s.skip.count(id))
                    continue;
                const omamake::BodyId cut = applyAll(s, id, tools, areas, omamake::Combine::Cut);
                if (cut) {
                    reached.insert(id);
                    step(QStringLiteral("side %1: cut %2 (body %3): done, now solid %4").arg(si).arg(was[id]).arg(id).arg(cut));
                    replace(id, cut);
                    any = true;
                } else {
                    failedOn(id);
                }
                names << was[id];
            }
            sayFailed("cut");
            if (!any && !s.hits.empty() && failed.isEmpty())
                out.notes << QStringLiteral("Every body it reaches has cutting switched off.");
            else if (s.hits.empty())
                out.notes << QStringLiteral("Nothing to cut there.");
            out.summary << QStringLiteral("cut %1").arg(names.join(QStringLiteral(", ")));
            for (std::size_t k = 0; k < tools.size(); ++k)
                spent.push_back({QStringLiteral("side %1, tool %2 (area %3)").arg(si).arg(k).arg(k < areas.size() ? int(areas[k]) : -1), tools[k]});
            continue;
        }
        // Intersect (Fusion's): of each body it reaches, only what lies inside the extrude stays.
        if (op == QLatin1String("intersect")) {
            omamake::BodyId whole = tools.front();
            for (std::size_t k = 1; k < tools.size(); ++k) {
                const omamake::BodyId fused = m_engine->combine(whole, tools[k], omamake::Combine::Join);
                if (!fused)
                    continue;
                if (whole != tools.front())
                    m_engine->release(whole);
                whole = fused;
            }
            QStringList names;
            for (const int id : s.hits) {
                if (s.skip.count(id))
                    continue;
                const omamake::BodyId kept = m_engine->combine(currentOf(id), whole, omamake::Combine::Intersect, job.fuzzy ? kFuzzyProfile : 0);
                if (kept) {
                    reached.insert(id);
                    step(QStringLiteral("side %1: intersect %2 (body %3): done, now solid %4").arg(si).arg(was[id]).arg(id).arg(kept));
                    replace(id, kept);
                    names << was[id];
                } else {
                    failedOn(id);
                }
            }
            sayFailed("intersect");
            if (names.isEmpty() && failed.isEmpty())
                out.notes << QStringLiteral("Nothing to intersect there.");
            out.summary << QStringLiteral("intersect %1").arg(names.join(QStringLiteral(", ")));
            if (whole != tools.front())
                m_engine->release(whole);
            for (std::size_t k = 0; k < tools.size(); ++k)
                spent.push_back({QStringLiteral("side %1, tool %2 (area %3)").arg(si).arg(k).arg(k < areas.size() ? int(areas[k]) : -1), tools[k]});
            continue;
        }
        if (op == QLatin1String("join") && (!s.touches.empty() || !s.hits.empty())) {
            const int target = !s.touches.empty() ? s.touches.front() : s.hits.front();
            if (const omamake::BodyId fused = applyAll(s, target, tools, areas, omamake::Combine::Join)) {
                reached.insert(target);
                step(QStringLiteral("side %1: join to %2 (body %3): done, now solid %4").arg(si).arg(was[target]).arg(target).arg(fused));
                replace(target, fused);
            } else
                failedOn(target);
            sayFailed("join");
            for (std::size_t k = 0; k < tools.size(); ++k)
                spent.push_back({QStringLiteral("side %1, tool %2 (area %3)").arg(si).arg(k).arg(k < areas.size() ? int(areas[k]) : -1), tools[k]});
            if (s.touches.size() + s.hits.size() > 1)
                out.notes << QStringLiteral("It touches more than one body; it was joined to %1 only.").arg(was[target]);
            out.summary << QStringLiteral("join %1").arg(was[target]);
            continue;
        }
        if (op == QLatin1String("join"))
            out.notes << QStringLiteral("Nothing to join to, so it became a new body.");

        // New body / new component: areas that touch each other make one body.
        std::vector<omamake::BodyId> groups;
        std::unordered_set<omamake::BodyId> fusedOnes;   // made by a join: its faces already merged
        for (const omamake::BodyId t : tools) {
            bool merged = false;
            for (omamake::BodyId &g : groups) {
                if (m_engine->gap(g, t, 1e-3) < 1e-6) {
                    const omamake::BodyId fused = m_engine->combine(g, t, omamake::Combine::Join);
                    if (fused) {
                        m_engine->release(g);
                        m_engine->release(t);
                        g = fused;
                        fusedOnes.insert(fused);
                        merged = true;
                        break;
                    }
                }
            }
            if (!merged)
                groups.push_back(t);
        }
        // Faces in one surface made one, as a join's or cut's result already is (owner, 2026-11-06): a sketch
        // circle's side otherwise comes out in two halves with a line down it.
        for (omamake::BodyId &g : groups) {
            if (fusedOnes.count(g))
                continue;
            if (const omamake::BodyId tidy = m_engine->merged(g)) {
                m_engine->release(g);
                g = tidy;
            }
        }
        step(QStringLiteral("side %1: %2 new solid(s) from %3 tool(s)").arg(si).arg(groups.size()).arg(tools.size()));
        out.made.push_back({op == QLatin1String("component"), groups});
        out.summary << (op == QLatin1String("component") ? QStringLiteral("new component") : QStringLiteral("new body"));
    }
    // The whole extrude checked once it's done (owner, 2026-11-06): every body it changed or made must be a
    // sound solid (one closed shell per solid, no face left inside the material, OpenCascade's own check, no
    // edge with a face on one side only). If any isn't, or any body's operation couldn't be built, all of it
    // is put back as it was, and the log says why, top to bottom.
    for (const auto &[original, now] : current) {
        step(QStringLiteral("check: %1 (body %2) as it came out, solid %3").arg(was[original]).arg(original).arg(now));
        if (!described(now)) {
            out.failed = true;
            out.notes << QStringLiteral("%1 wouldn't be a sound solid after it.").arg(was[original]);
        }
    }
    for (const auto &[component, groups] : out.made)
        for (const omamake::BodyId g : groups) {
            step(QStringLiteral("check: new solid %1").arg(g));
            if (!described(g)) {
                out.failed = true;
                out.notes << QStringLiteral("The new body wouldn't be a sound solid.");
            }
        }
    if (out.failed) {
        for (const int id : reached) {
            step(QStringLiteral("as it went in: %1 (body %2)").arg(was[id]).arg(id));
            described(omamake::BodyId(id));
        }
        for (const auto &[label, tool] : spent) {
            step(QStringLiteral("as it went in: %1, solid %2").arg(label).arg(tool));
            described(tool);
        }
        for (const auto &[original, now] : current)
            m_engine->release(now);
        current.clear();
        for (const auto &[component, groups] : out.made)
            for (const omamake::BodyId g : groups)
                m_engine->release(g);
        out.made.clear();
        step(QStringLiteral("not sound or not built: everything put back as it was"));
    } else {
        step(QStringLiteral("done, every body sound"));
        if (job.fuzzy)
            for (const int id : reached)
                out.modelLoose = std::max(out.modelLoose, m_engine->looseness(omamake::BodyId(id)));
        // Pieces left apart (owner, 2026-11-06, as Fusion): where a body came out as separate solids, the
        // biggest stays that body and each other one becomes a body of its own in the active component.
        std::vector<omamake::BodyId> apart;
        for (auto &[original, now] : current) {
            const std::vector<omamake::BodyId> split = m_engine->pieces(now);
            if (split.empty())
                continue;
            step(QStringLiteral("%1 (body %2) is %3 separate pieces: the biggest stays it, the rest are new bodies")
                     .arg(was[original]).arg(original).arg(split.size()));
            m_engine->release(now);
            now = split.front();
            apart.insert(apart.end(), split.begin() + 1, split.end());
        }
        for (auto &[component, groups] : out.made) {
            std::vector<omamake::BodyId> each;
            for (const omamake::BodyId g : groups) {
                const std::vector<omamake::BodyId> split = m_engine->pieces(g);
                if (split.empty()) {
                    each.push_back(g);
                    continue;
                }
                step(QStringLiteral("new solid %1 is %2 separate pieces: each a body of its own").arg(g).arg(split.size()));
                m_engine->release(g);
                each.insert(each.end(), split.begin(), split.end());
            }
            groups = std::move(each);
        }
        if (!apart.empty())
            out.made.push_back({false, apart});
    }
    for (const auto &[label, tool] : spent)
        m_engine->release(tool);
    const qint64 checked = clock.elapsed();
    // The triangles to draw, made here too: a cut thread takes a second or two to mesh.
    for (const auto &[original, now] : current) {
        out.swaps.push_back({original, int(now)});
        out.meshes[int(now)] = m_engine->mesh(now);
    }
    for (const auto &[component, groups] : out.made)
        for (const omamake::BodyId g : groups)
            out.meshes[int(g)] = m_engine->mesh(g);
    qInfo().noquote() << QStringLiteral("extrude: %1 s worked out and checked, %2 s meshed")
                             .arg(checked / 1000.0, 0, 'f', 2).arg((clock.elapsed() - checked) / 1000.0, 0, 'f', 2);
    return out;
}

Document::ExtrudeResult Document::computeExtrudeTrying(ExtrudeJob job) const {
    job.fuzzy = false;
    ExtrudeResult exact = computeExtrude(job);
    // Fuzzy profile only changes how a cut, join or intersect is built: any other failure would come out the same.
    if (!exact.failed || !exact.booleanFailed)
        return exact;
    qInfo().noquote() << QStringLiteral("extrude: couldn't be built exactly; trying again with Fuzzy profile (%1 mm)").arg(kFuzzyProfile);
    job.fuzzy = true;
    ExtrudeResult fuzzy = computeExtrude(job);
    fuzzy.fuzzyTried = true;
    QStringList trace;
    trace << QStringLiteral("first try, exact:") << exact.trace << QString()
          << QStringLiteral("second try, automatic, with Fuzzy profile (a cut, join or intersect that can't be built exactly is "
                            "tried again with points %1 mm apart taken as one):").arg(kFuzzyProfile)
          << fuzzy.trace;
    fuzzy.trace = trace;
    return fuzzy;
}

// A failed extrude's log (owner, 2026-11-06): one file per failure in the source folder's logs/ (dev
// builds only; gitignored), with what was asked, every step it took with what the engine did for each,
// and each body and tool described as it went in and as it came out. Its path, or "" if not written.
QString Document::writeExtrudeLog(const ExtrudeResult &r, const QVariantMap &spec, const QString &said) const {
#ifdef OMAMAKE_SOURCE_DIR
    const QString folder = QStringLiteral(OMAMAKE_SOURCE_DIR "/logs");
    if (!QDir().mkpath(folder))
        return {};
    const QDateTime now = QDateTime::currentDateTime();
    QString path = QStringLiteral("%1/extrude-failed-%2.log").arg(folder, now.toString(QStringLiteral("yyyy-MM-dd_HH-mm-ss")));
    for (int n = 2; QFileInfo::exists(path); ++n)
        path = QStringLiteral("%1/extrude-failed-%2-%3.log").arg(folder, now.toString(QStringLiteral("yyyy-MM-dd_HH-mm-ss"))).arg(n);
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text))
        return {};
    QStringList text;
    text << QStringLiteral("OmaMake extrude that failed, %1").arg(now.toString(Qt::ISODateWithMs))
         << QStringLiteral("file: %1").arg(m_filePath.isEmpty() ? QStringLiteral("(not saved yet)") : m_filePath)
         << QStringLiteral("engine: %1").arg(QString::fromStdString(m_engine->name()))
         << QStringLiteral("told the person: %1").arg(said)
         << QString()
         << QStringLiteral("asked for (the extrude's settings):")
         << QString::fromUtf8(QJsonDocument(QJsonObject::fromVariantMap(spec)).toJson(QJsonDocument::Indented)).trimmed()
         << QString()
         << QStringLiteral("what was done, top to bottom (times from the start; engine lines indented above the step they belong to):")
         << r.trace;
    file.write(text.join(QLatin1Char('\n')).toUtf8() + "\n");
    qInfo().noquote() << QStringLiteral("extrude: failed; log written to %1").arg(path);
    return path;
#else
    (void)r; (void)spec; (void)said;
    return {};
#endif
}

QVariantMap Document::applyExtrude(ExtrudeResult &&r, const QVariantMap &spec) {
    QJsonArray created, components;
    // The meshes first: swapping tells the window, which draws the new body at once, and meshing it
    // again here took seconds on the main thread for a tapered cut.
    for (auto &[id, mesh] : r.meshes)
        m_meshCache[id] = std::move(mesh);
    for (const auto &[original, now] : r.swaps)
        swapBody(original, omamake::BodyId(now));
    for (const auto &[component, groups] : r.made) {
        int into = m_active;
        if (component) {
            const int id = m_nextComponent++;
            Component c;
            c.id = id;
            c.parent = m_active;
            c.name = QStringLiteral("Component%1:1").arg(id);
            this->component(m_active)->components.push_back(id);
            m_components.push_back(c);
            m_hidden.insert(QStringLiteral("c%1/origin").arg(id));
            components.append(id);
            into = id;
        }
        for (const omamake::BodyId g : groups)
            created.append(addBody(g, into));
    }
    if (r.swaps.empty() && created.isEmpty()) {
        QString why = r.notes.isEmpty() ? QStringLiteral("Nothing was made.") : r.notes.join(QLatin1Char(' '));
        if (r.failed) {
            if (!why.contains(QLatin1String("left as it was")))
                why += QStringLiteral(" Nothing was changed.");
            if (r.fuzzyTried)
                why += QStringLiteral(" It was tried again with Fuzzy profile (points up to %1 mm apart taken as one), and that failed too.")
                           .arg(kFuzzyProfile);
            const QString log = writeExtrudeLog(r, spec, why);
            if (!log.isEmpty())
                why += r.fuzzyTried ? QStringLiteral(" Why, step by step (both tries): %1").arg(log)
                                    : QStringLiteral(" Why, step by step: %1").arg(log);
        }
        return {{QStringLiteral("ok"), false}, {QStringLiteral("why"), why}};
    }
    const int token = m_extrudeToken++;
    m_extrudeKeep[token] = r.swaps;

    QJsonObject action{{QStringLiteral("type"), QStringLiteral("extrude")}, {QStringLiteral("token"), token},
                       {QStringLiteral("created"), created}, {QStringLiteral("components"), components},
                       {QStringLiteral("spec"), QJsonObject::fromVariantMap(spec)}};
    if (!created.isEmpty())
        action.insert(QStringLiteral("body"), created.first());
    // Which body became which solid: an opened file's background rebuild follows bodies by them.
    QJsonArray swaps;
    for (const auto &[was, now] : r.swaps)
        swaps.append(QJsonArray{was, now});
    action.insert(QStringLiteral("swaps"), swaps);
    record(action, QStringLiteral("Extrude: %1").arg(r.summary.join(QStringLiteral("; "))));
    refreshMesh();
    emit changed();
    emit structureChanged();
    QVariantList createdList;
    for (const QJsonValue &v : created)
        createdList << v.toInt();
    if (r.fuzzyTried) {
        // How far off it may be (owner, 2026-11-07): where the profile meets the model, points up to the fuzzy
        // value apart were taken as one, so the cut or join there can sit that far from where it was drawn; the
        // model itself is only exact to its loosest edge anyway.
        const auto inches = [](double mm) { return QString::number(mm / 25.4, 'f', mm / 25.4 < 0.001 ? 5 : 4); };
        QString note = QStringLiteral("This extrude needed Fuzzy profile: the model's faces don't quite meet, so it couldn't be built exactly. "
                                      "Where the profile meets the model it may be off by up to %1 mm (%2 in).")
                           .arg(kFuzzyProfile).arg(inches(kFuzzyProfile));
        if (r.modelLoose > kFuzzyProfile)
            note += QStringLiteral(" The model itself is only exact to %1 mm (%2 in) there.")
                        .arg(QString::number(r.modelLoose, 'g', 2), inches(r.modelLoose));
        r.notes.prepend(note);
    }
    return {{QStringLiteral("ok"), true}, {QStringLiteral("created"), createdList}, {QStringLiteral("note"), r.notes.join(QLatin1Char(' '))}};
}

QVariantMap Document::extrude(const QVariantMap &spec) {
    if (m_working)
        return {{QStringLiteral("ok"), false}, {QStringLiteral("why"), QStringLiteral("Still working on the last one.")}};
    ExtrudeJob job;
    QString why;
    if (!readExtrude(spec, job, why))
        return {{QStringLiteral("ok"), false}, {QStringLiteral("why"), why}};
    planExtrude(job);
    return applyExtrude(computeExtrudeTrying(job), spec);
}

// The same, with the solid work off the main thread: the window keeps drawing and turning while a cut
// through a thread takes its seconds. Undo, save, open and another extrude wait (`working`); the
// answer comes back as extrudeFinished.
QVariantMap Document::extrudeInBackground(const QVariantMap &spec) {
    if (m_working || m_importing)
        return {{QStringLiteral("ok"), false}, {QStringLiteral("why"), QStringLiteral("Still working on the last one.")}};
    ExtrudeJob job;
    QString why;
    if (!readExtrude(spec, job, why))
        return {{QStringLiteral("ok"), false}, {QStringLiteral("why"), why}};
    planExtrude(job);
    m_working = true;
    emit workingChanged();
    QThread *worker = QThread::create([this, job = std::move(job), spec] {
        QElapsedTimer clock;
        clock.start();
        ExtrudeResult r = computeExtrudeTrying(job);
        const double seconds = clock.elapsed() / 1000.0;
        QMetaObject::invokeMethod(this, [this, r = std::move(r), spec, seconds]() mutable {
            m_working = false;
            emit workingChanged();
            QVariantMap result = applyExtrude(std::move(r), spec);
            result.insert(QStringLiteral("seconds"), seconds);
            emit extrudeFinished(result);
        }, Qt::QueuedConnection);
    });
    connect(worker, &QThread::finished, worker, &QObject::deleteLater);
    worker->start();
    return {{QStringLiteral("ok"), true}};
}

// The strongly drawn red of a cut: the extrude's own shape, when it reaches a body it may cut.
void Document::fillExtrudeCutaway(MeshGeometry *target, const QVariantMap &spec, int side) {
    if (!target)
        return;
    omamake::Mesh out = emptyMesh();
    ExtrudeJob job;
    QString why;
    if (readExtrude(spec, job, why) && side >= 0 && std::size_t(side) < job.sides.size()) {
        planExtrude(job);
        const ExtrudeSide &s = job.sides[std::size_t(side)];
        if (s.resolved == QLatin1String("cut")) {
            // The extrude itself, drawn strongly: intersecting it with each body showed exactly what
            // goes, but took seconds (minutes on a thread) every time a drag paused.
            bool any = false;
            for (const int id : s.hits)
                any = any || (!s.skip.count(id) && m_bodies.count(id));
            if (any)
                for (const omamake::BodyId t : extrudeTools(job, side)) {
                    const omamake::Mesh m = m_engine->mesh(t);
                    m_engine->release(t);
                    const std::uint32_t base = std::uint32_t(out.positions.size() / 3);
                    out.positions.insert(out.positions.end(), m.positions.begin(), m.positions.end());
                    out.normals.insert(out.normals.end(), m.normals.begin(), m.normals.end());
                    for (const std::uint32_t i : m.indices)
                        out.indices.push_back(base + i);
                    out.edgeLines.insert(out.edgeLines.end(), m.edgeLines.begin(), m.edgeLines.end());
                }
        }
    }
    finish(out);
    target->setMesh(out);
}

void Document::fillExtrudePreview(MeshGeometry *target, MeshGeometry *wire, const QVariantMap &spec, int side) {
    if (!target)
        return;
    omamake::Mesh out = emptyMesh();
    ExtrudeJob job;
    QString why;
    if (readExtrude(spec, job, why) && side >= 0 && std::size_t(side) < job.sides.size()) {
        for (const omamake::BodyId t : extrudeTools(job, side)) {
            const omamake::Mesh m = m_engine->mesh(t);
            m_engine->release(t);
            const std::uint32_t base = std::uint32_t(out.positions.size() / 3);
            out.positions.insert(out.positions.end(), m.positions.begin(), m.positions.end());
            out.normals.insert(out.normals.end(), m.normals.begin(), m.normals.end());
            for (const std::uint32_t i : m.indices)
                out.indices.push_back(base + i);
            out.edgeLines.insert(out.edgeLines.end(), m.edgeLines.begin(), m.edgeLines.end());
        }
    }
    finish(out);
    target->setMesh(out);
    if (wire)
        wire->setMesh(out);
}

QVariantMap Document::extrudeFrame(const QVariantMap &spec) {
    ExtrudeJob job;
    QString why;
    if (!readExtrude(spec, job, why))
        return {{QStringLiteral("ok"), false}, {QStringLiteral("why"), why}};
    QVariantList anchors;
    QVector3D normal;
    for (std::size_t i = 0; i < job.places.size(); ++i) {
        const omamake::Placement &p = job.places[i];
        const QVector3D o(float(p.origin[0]), float(p.origin[1]), float(p.origin[2]));
        const QVector3D x(float(p.x[0]), float(p.x[1]), float(p.x[2])), y(float(p.y[0]), float(p.y[1]), float(p.y[2]));
        normal = QVector3D::crossProduct(x, y).normalized();
        double lo[2] = {1e300, 1e300}, hi[2] = {-1e300, -1e300};
        for (const auto &q : job.profiles[i].loops.front()) {
            lo[0] = std::min(lo[0], q.x); hi[0] = std::max(hi[0], q.x);
            lo[1] = std::min(lo[1], q.y); hi[1] = std::max(hi[1], q.y);
        }
        anchors << QVariant::fromValue(o + x * float((lo[0] + hi[0]) / 2) + y * float((lo[1] + hi[1]) / 2));
    }
    // How far each side goes, for the arrows when the length comes from To Object or All.
    const double reach1 = job.sides[0].to.front(), reach2 = job.sides.size() > 1 ? -job.sides[1].to.front() : 0;
    return {{QStringLiteral("ok"), true}, {QStringLiteral("normal"), normal}, {QStringLiteral("anchors"), anchors},
            {QStringLiteral("reach1"), reach1}, {QStringLiteral("reach2"), reach2}};
}
