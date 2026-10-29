#include "view/meshgeometry.h"

#include <QVector3D>

void MeshGeometry::setLines(bool lines) {
    if (m_lines == lines)
        return;
    m_lines = lines;
    emit linesChanged();
}

void MeshGeometry::setMesh(const omamake::Mesh &mesh) {
    clear();
    setBounds(QVector3D(mesh.min[0], mesh.min[1], mesh.min[2]),
              QVector3D(mesh.max[0], mesh.max[1], mesh.max[2]));

    if (m_lines) {
        setPrimitiveType(PrimitiveType::Lines);
        setStride(3 * sizeof(float));
        setVertexData(QByteArray(reinterpret_cast<const char *>(mesh.edgeLines.data()),
                                 qsizetype(mesh.edgeLines.size() * sizeof(float))));
        addAttribute(Attribute::PositionSemantic, 0, Attribute::F32Type);
    } else {
        // Interleaved: position then normal.
        const std::size_t count = mesh.positions.size() / 3;
        QByteArray data(qsizetype(count * 6 * sizeof(float)), Qt::Uninitialized);
        auto *out = reinterpret_cast<float *>(data.data());
        for (std::size_t i = 0; i < count; ++i) {
            for (int c = 0; c < 3; ++c) {
                out[i * 6 + c] = mesh.positions[i * 3 + c];
                out[i * 6 + 3 + c] = mesh.normals[i * 3 + c];
            }
        }
        setPrimitiveType(PrimitiveType::Triangles);
        setStride(6 * sizeof(float));
        setVertexData(data);
        setIndexData(QByteArray(reinterpret_cast<const char *>(mesh.indices.data()),
                                qsizetype(mesh.indices.size() * sizeof(std::uint32_t))));
        addAttribute(Attribute::PositionSemantic, 0, Attribute::F32Type);
        addAttribute(Attribute::NormalSemantic, 3 * sizeof(float), Attribute::F32Type);
        addAttribute(Attribute::IndexSemantic, 0, Attribute::U32Type);
    }
    update();
}
