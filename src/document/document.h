#pragma once

// The open model: its bodies, built by the engine, and the full action log.

#include "engine/engine.h"

#include <QJsonArray>
#include <QObject>
#include <QVariantList>
#include <QVariantMap>
#include <QVector3D>

#include <map>
#include <memory>
#include <vector>

class MeshGeometry;

class Document : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString engineName READ engineName CONSTANT)
    Q_PROPERTY(QVector3D boundsMin READ boundsMin NOTIFY changed)
    Q_PROPERTY(QVector3D boundsMax READ boundsMax NOTIFY changed)
    Q_PROPERTY(int historyLength READ historyLength NOTIFY historyChanged)
    Q_PROPERTY(QVariantMap browser READ browser NOTIFY structureChanged)
    Q_PROPERTY(int activeComponent READ activeComponent WRITE setActiveComponent NOTIFY structureChanged)

public:
    explicit Document(std::unique_ptr<omamake::GeometryEngine> engine, QObject *parent = nullptr);

    QString engineName() const;
    // Engine coordinates (mm, Z up).
    QVector3D boundsMin() const { return m_boundsMin; }
    QVector3D boundsMax() const { return m_boundsMax; }
    // How many actions the history holds (the timeline, later).
    int historyLength() const { return int(m_actions.size()); }

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

    // The Browser's tree (Fusion's names): the root component, then nested components.
    // Every component can hold Origin, Bodies, Sketches and further components: Fusion's
    // "hybrid design", always on here. Each node: {key, kind, name, id?, children: [...]};
    // kind is "component", "folder", "origin", "plane", "axis", "point", "body" or "sketch".
    QVariantMap browser() const;

    // New sketches and components go into the active component (Fusion's radio button).
    int activeComponent() const { return m_active; }
    void setActiveComponent(int id);

    // Fusion's New Component: an empty component inside the active one, which it then
    // activates, so the next one nests inside it. Returns its id.
    Q_INVOKABLE int newComponent();

    // Sketches. Starting one adds it to the action log and to the active component; plane is Sketch.qml's
    // {origin, normal, xAxis, yAxis}. Returns the new sketch's id.
    Q_INVOKABLE int addSketch(const QVariantMap &plane);
    // Take a sketch back out as if it was never made (Ctrl+Z in a sketch): its
    // action leaves the log, so nothing of it is left in the history.
    Q_INVOKABLE void removeSketch(int id);

    // Placeholder until sketch + extrude exist: a cube sitting centred on the floor.
    void addCube(double sizeMm);

signals:
    void changed();
    void historyChanged();
    void structureChanged();

private:
    void record(const QJsonObject &action);
    void refreshMesh();

    std::unique_ptr<omamake::GeometryEngine> m_engine;
    omamake::BodyId m_body = 0;
    struct Component {
        int id = 0;
        int parent = -1;              // -1: the root component
        QString name;
        int nextSketch = 1;           // Sketch1, Sketch2… counted per component, as in Fusion
        std::vector<int> components, bodies, sketches;
    };
    struct Named {
        int component = 0;
        QString name;
    };
    static Component rootComponent() {
        Component root;
        root.name = QStringLiteral("(Unsaved)");   // the document's name, until it's saved
        return root;
    }
    Component *component(int id);
    QVariantMap componentNode(const Component &c) const;

    std::vector<Component> m_components = {rootComponent()};
    std::map<int, Named> m_bodies, m_sketches;   // by id
    int m_active = 0;
    int m_nextComponent = 1;
    int m_nextSketch = 1;
    QJsonArray m_actions;   // every change, in order (saved with the file later)
    omamake::Mesh m_mesh;   // the model as last built, with its faces, edges and points
    QVector3D m_boundsMin, m_boundsMax;
};
