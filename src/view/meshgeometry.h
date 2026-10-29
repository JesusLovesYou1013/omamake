#pragma once

// Hands an engine Mesh to Qt Quick 3D: as shaded triangles, or as outline lines.

#include "engine/engine.h"

#include <QQuick3DGeometry>

class MeshGeometry : public QQuick3DGeometry {
    Q_OBJECT
    Q_PROPERTY(bool lines READ lines WRITE setLines NOTIFY linesChanged)

public:
    explicit MeshGeometry(QQuick3DObject *parent = nullptr) : QQuick3DGeometry(parent) {}

    bool lines() const { return m_lines; }
    void setLines(bool lines);

    void setMesh(const omamake::Mesh &mesh);

signals:
    void linesChanged();

private:
    bool m_lines = false;
};
