#include "document/document.h"

#include "view/meshgeometry.h"

#include <QDateTime>
#include <QJsonObject>

Document::Document(std::unique_ptr<omamake::GeometryEngine> engine, QObject *parent)
    : QObject(parent), m_engine(std::move(engine)) {}

QString Document::engineName() const {
    return QString::fromStdString(m_engine->name());
}

void Document::fill(MeshGeometry *faces, MeshGeometry *edges) const {
    const omamake::Mesh mesh = m_body ? m_engine->mesh(m_body) : omamake::Mesh{};
    if (faces)
        faces->setMesh(mesh);
    if (edges)
        edges->setMesh(mesh);
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
    }
    record({{QStringLiteral("type"), QStringLiteral("cube")}, {QStringLiteral("size_mm"), sizeMm}});
    updateBounds();
    emit changed();
}

void Document::record(const QJsonObject &action) {
    QJsonObject entry = action;
    entry.insert(QStringLiteral("at"), QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
    m_actions.append(entry);
}

void Document::updateBounds() {
    const omamake::Mesh mesh = m_engine->mesh(m_body);
    m_boundsMin = QVector3D(mesh.min[0], mesh.min[1], mesh.min[2]);
    m_boundsMax = QVector3D(mesh.max[0], mesh.max[1], mesh.max[2]);
}
