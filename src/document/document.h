#pragma once

// The open model: its bodies, built by the engine, and the full action log.

#include "engine/engine.h"

#include <QJsonArray>
#include <QObject>
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

    // Placeholder until sketch + extrude exist: a cube sitting centred on the floor.
    void addCube(double sizeMm);

signals:
    void changed();

private:
    void record(const QJsonObject &action);
    void updateBounds();

    std::unique_ptr<omamake::GeometryEngine> m_engine;
    omamake::BodyId m_body = 0;
    QJsonArray m_actions;   // every change, in order (saved with the file later)
    QVector3D m_boundsMin, m_boundsMax;
};
