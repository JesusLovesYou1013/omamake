#pragma once

// The open model: its bodies, built by the engine, and the full action log.

#include "engine/engine.h"

#include <QJsonArray>
#include <QObject>
#include <QVariantList>
#include <QVariantMap>
#include <QVector3D>

#include <memory>

class MeshGeometry;

class Document : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString engineName READ engineName CONSTANT)
    Q_PROPERTY(QVector3D boundsMin READ boundsMin NOTIFY changed)
    Q_PROPERTY(QVector3D boundsMax READ boundsMax NOTIFY changed)

public:
    explicit Document(std::unique_ptr<omamake::GeometryEngine> engine, QObject *parent = nullptr);

    QString engineName() const;
    // Engine coordinates (mm, Z up).
    QVector3D boundsMin() const { return m_boundsMin; }
    QVector3D boundsMax() const { return m_boundsMax; }

    // Fill a view's shaded geometry and its outline geometry with the model.
    Q_INVOKABLE void fill(MeshGeometry *faces, MeshGeometry *edges) const;

    // What's under a ray (engine coordinates): {kind: "point" | "edge" | "face" | "", index}.
    // Points win over edges, edges over faces, like Fusion. A point or edge counts when the
    // ray passes within (distance along the ray × pointReach / edgeReach) of it and nothing
    // solid is in front of it.
    Q_INVOKABLE QVariantMap pick(QVector3D origin, QVector3D direction, float pointReach, float edgeReach) const;

    // Shapes that show parts of the model (hover or selection): faces as copies of their
    // triangles, edges as thin tubes, points as small balls. parts: [{kind, index}, ...].
    Q_INVOKABLE void fillParts(MeshGeometry *target, const QVariantList &parts, float edgeRadius) const;

    // The plane a sketch on this face sits on (engine coordinates):
    // {flat, origin, normal, xAxis, yAxis, center}. flat is false for a curved face.
    // The normal points out of the body. The sketch's own Y axis is "up" (world Z, or
    // world Y on a face that lies flat), and the origin is the world origin dropped onto
    // the plane, as in Fusion, so sketches on parallel faces line up.
    Q_INVOKABLE QVariantMap facePlane(int face) const;

    // Placeholder until sketch + extrude exist: a cube sitting centred on the floor.
    void addCube(double sizeMm);

signals:
    void changed();

private:
    void record(const QJsonObject &action);
    void refreshMesh();

    std::unique_ptr<omamake::GeometryEngine> m_engine;
    omamake::BodyId m_body = 0;
    QJsonArray m_actions;   // every change, in order (saved with the file later)
    omamake::Mesh m_mesh;   // the model as last built, with its faces, edges and points
    QVector3D m_boundsMin, m_boundsMax;
};
