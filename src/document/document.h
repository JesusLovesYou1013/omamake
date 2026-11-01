#pragma once

// The open model: its bodies, built by the engine, and the full action log.

#include "document/sketchgeometry.h"
#include "document/profiles.h"
#include "engine/engine.h"

#include <QJsonArray>
#include <QObject>
#include <QVariantList>
#include <QVariantMap>
#include <QVector3D>

#include <map>
#include <set>
#include <memory>
#include <vector>

class MeshGeometry;

class Document : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString engineName READ engineName CONSTANT)
    Q_PROPERTY(QVector3D boundsMin READ boundsMin NOTIFY changed)
    Q_PROPERTY(QVector3D boundsMax READ boundsMax NOTIFY changed)
    Q_PROPERTY(int historyLength READ historyLength NOTIFY historyChanged)
    Q_PROPERTY(QVariantList history READ history NOTIFY historyChanged)
    Q_PROPERTY(QVariantMap browser READ browser NOTIFY structureChanged)
    Q_PROPERTY(int activeComponent READ activeComponent WRITE setActiveComponent NOTIFY structureChanged)
    Q_PROPERTY(QString filePath READ filePath NOTIFY fileChanged)
    Q_PROPERTY(bool importing READ importing NOTIFY importChanged)
    Q_PROPERTY(QString importStage READ importStage NOTIFY importChanged)
    Q_PROPERTY(QString importName READ importName NOTIFY importChanged)

public:
    explicit Document(std::unique_ptr<omamake::GeometryEngine> engine, QObject *parent = nullptr);

    QString engineName() const;
    // Engine coordinates (mm, Z up).
    QVector3D boundsMin() const { return m_boundsMin; }
    QVector3D boundsMax() const { return m_boundsMax; }
    // The history: every action, oldest first, each with its time, its type, a one-line
    // summary in plain words, and everything needed to take it back. Ctrl+Z steps back
    // through it one action at a time, in Model and Sketch mode alike. Saved with the file later.
    int historyLength() const { return int(m_actions.size()); }
    QVariantList history() const { return m_actions.toVariantList(); }

    // Ctrl+Z: take back the newest action and drop it from the history. Returns that
    // action, so the window can follow (e.g. back into a sketch whose finishing was undone),
    // or an empty map when there's nothing left to undo (the starting model stays).
    Q_INVOKABLE QVariantMap undo();

    // Fill a view's shaded geometry and its outline geometry with the model.
    Q_INVOKABLE void fill(MeshGeometry *faces, MeshGeometry *edges) const;

    // What's under a ray (engine coordinates): {kind: "point" | "edge" | "face" | "", index}.
    // Points win over edges, edges over faces, like Fusion. A point or edge counts when the
    // ray passes within (distance along the ray × pointReach / edgeReach) of it and nothing
    // solid is in front of it.
    Q_INVOKABLE QVariantMap pick(QVector3D origin, QVector3D direction, float pointReach, float edgeReach) const;

    // Shapes that show parts of the model (hover or selection): faces as copies of their
    // triangles, edges as thin tubes, points as small balls. parts: [{kind, index}, ...].
    // With perMm (perspective views, body parts) thickness is perMm × the part's own distance from
    // eye, so a highlight close to the camera stays thin however far away the pivot is.
    Q_INVOKABLE void fillParts(MeshGeometry *target, const QVariantList &parts, float edgeRadius,
                               QVector3D eye = {}, float perMm = 0) const;

    // The plane a sketch on this face sits on (engine coordinates):
    // {flat, origin, normal, xAxis, yAxis, center}. flat is false for a curved face.
    // The normal points out of the body. The sketch's own Y axis is "up" (world Z, or
    // world Y on a face that lies flat), and the origin is the world origin dropped onto
    // the plane, as in Fusion, so sketches on parallel faces line up.
    Q_INVOKABLE QVariantMap facePlane(int face) const;
    // Find in Window (Fusion's name; owner, 2026-10-30): how to bring a Browser row's things
    // (`key`: a body, sketch, component, folder or origin part) back into full view. `from` is
    // the way the camera looks from now (scene coordinates, pivot → camera), `halfAngle` the
    // view's narrower half-angle in degrees. Returns {center, radius, direction, distance} in
    // scene coordinates, `turned` when the view must turn because shown bodies hide it from
    // here, `inside` when it's wholly inside a body (then it only zooms), and `found` false
    // for a key with nothing to show.
    Q_INVOKABLE QVariantMap findInWindow(const QString &key, QVector3D from, double halfAngle) const;

    // The Browser's tree (Fusion's names): the root component, then nested components.
    // Every component can hold Origin, Bodies, Sketches and further components: Fusion's
    // "hybrid design", always on here. Each node: {key, kind, name, id?, children: [...]};
    // kind is "component", "folder", "origin", "plane", "axis", "point", "body" or "sketch".
    QVariantMap browser() const;

    // Show/hide (the eye on each Browser row), by node key. Every node in browser() also
    // carries shown (its own eye) and visible (shown, and every row above it shown too:
    // hiding a component or folder hides all inside it). Each change is an action in the
    // history. The top row (the design itself) is always shown. Origins start hidden, as in Fusion.
    Q_INVOKABLE void setShown(const QString &key, bool shown);

    // New sketches and components go into the active component (Fusion's radio button).
    int activeComponent() const { return m_active; }
    void setActiveComponent(int id);

    // Fusion's New Component: an empty component inside the active one, which it then
    // activates, so the next one nests inside it. Returns its id.
    Q_INVOKABLE int newComponent();

    // Sketches. Starting one is an action, and so is finishing it: undoing the finish
    // goes back into the sketch, undoing the start takes the sketch away. plane is
    // Sketch.qml's {origin, normal, xAxis, yAxis, label}. Returns the new sketch's id.
    Q_INVOKABLE int addSketch(const QVariantMap &plane);
    Q_INVOKABLE void finishSketch(int id);
    // Fusion's Edit Sketch: back into a finished sketch (double-click it in the Timeline or
    // the Browser). An action, so Ctrl+Z leaves the sketch again; what's done inside joins
    // that sketch's own history.
    Q_INVOKABLE void editSketch(int id);
    Q_INVOKABLE QVariantMap sketchPlane(int id) const;

    // What's drawn in a sketch, in its own flat coordinates (u, v in mm):
    // {points: [{id, u, v, fixed, full}], lines: [{id, a, b, full}],
    //  constraints: [{id, type, first, second, u, v}]}. fixed: held by Fix; full: fully
    // constrained (can't move any more).
    Q_INVOKABLE QVariantMap sketchGeometry(int sketch) const;

    // Fusion's Line: one straight line, an action in the sketch's own history. from and to
    // are a point id (to join a line already there) or {u, v} for a new point. Returns
    // {line, from, to} (the point ids used), or {} if the two ends are the same point.
    // from / to: a point id, or {u, v} for a new point; a new point may carry what it was
    // snapped onto (owner, 2026-10-30), held there by a constraint made with the line:
    // on: "origin" (Coincident with the Origin), "midpoint" with line: id (Midpoint), or "line"
    // with line: id (on that line or axis, free to slide along it).
    // hold: "horizontal" / "vertical" when the line was snapped level / upright, else "".
    // construction: the line is a construction line (Toggle Construction).
    Q_INVOKABLE QVariantMap addSketchLine(int sketch, const QVariant &from, const QVariant &to,
                                          const QString &hold = QString(), bool construction = false);

    // Fusion's 2-Point Rectangle: four lines square to the sketch's axes between two
    // opposite corners (point id or {u, v}), held as a rectangle by Perpendicular and
    // Parallel only, so it can be turned (owner, 2026-10-30). One action. Returns {lines: [4 ids]}, or {} if it's flat.
    Q_INVOKABLE QVariantMap addSketchRectangle(int sketch, const QVariant &corner, const QVariant &opposite,
                                               bool construction = false);

    // Any drawing or modifying tool's work in one go (owner, 2026-10-30: all of Fusion's sketch
    // tools; the tools work their shapes out in QML, this makes them). spec:
    //   points:  [point id | {u, v, on?, line?, arc?, helper?}]   (a new point holds what it snapped to)
    //   lines:   [[i, j, helper?]]   indexes into points
    //   arcs:    [[c, a, b, helper?]] centre, ends (counter-clockwise a → b); a circle is [c, rim, rim]
    //   curves:  [{type, points: [indexes], value?, text?}]   (SketchGeometry::Curve)
    //   constraints: [{type, first, second, third, value?, unit?, u?, v?}]  each id is a number (an
    //                existing point / line / arc / reference) or "p<i>" / "l<i>" / "a<i>" / "k<i>" /
    //                "c<i>" (this spec's own; "c0" lets a follower name its leader as `third`)
    //   rewire:  [{line | arc: id, end: "a" | "b" | "c", to: point}]   ends moved onto other points
    //   move:    [{point, u, v}]     points put somewhere else
    //   remove:  {points, lines, arcs, curves, constraints}   (a point only if nothing uses it now;
    //            constraints on anything gone go with it)
    //   helpers: [point]             points to hide (a chamfer's lost corner)
    //   values:  [{constraint, value}]   dimensions changed (Sketch Scale)
    //   steady:  [point]             what should stay put if anything has to move
    //   summary: the history's name ("Drew a Center Diameter Circle")
    //   construction: what it draws is construction (the Construction switch, X while drawing)
    //   like:    [{made: "l<i>" | "a<i>" | "k<i>", from: id}]   a copy (Trim, Offset, Mirror…):
    //            construction exactly when its original is, whatever the switch says
    // Solves once if anything is off. One action; Ctrl+Z puts the sketch back exactly as it was.
    // Returns {points, lines, arcs, curves, constraints} (ids), or {why} if it couldn't hold.
    Q_INVOKABLE QVariantMap addSketchShape(int sketch, const QVariantMap &spec);
    // Text's outlines `height` mm tall from (0, 0) (Text's preview): [[{u, v}…]…].
    Q_INVOKABLE QVariantList sketchTextOutline(const QString &text, double height) const;

    // A constraint between two lines ("parallel", "perpendicular" or "equal"): first stays where it
    // can and second turns to meet it (or the first turns, when the second is held), then the whole sketch is solved again. An action.
    // Returns "" when done, or why not, in plain words.
    // The others (SketchGeometry's types; what each takes is SketchGeometry::kinds): "horizontal"
    // / "vertical" (a line), "horizontalpoints" / "verticalpoints", "coincident" (two points),
    // "pointonline", "midpoint" (a point, a line), "collinear" (two lines), "symmetry" (two
    // points and the line between), "symmetrylines" (two lines and the line between). The first
    // picked stays put where it can (keep: which of first / second was picked first, 0 or 1);
    // as few points move as possible.
    Q_INVOKABLE QString addSketchConstraint(int sketch, const QString &type, int first, int second, int third = 0,
                                            int keep = 0);

    // Dragging points of a sketch: begin, then drag as the mouse moves (moves = [{id, u, v}],
    // where those points should go; the rest follows as little as it must), then end, which
    // makes the whole drag ONE action named `summary`. drag returns false when the
    // constraints can't let them go there (nothing moves).
    Q_INVOKABLE void beginSketchDrag(int sketch);
    Q_INVOKABLE bool dragSketch(int sketch, const QVariantList &moves);
    // During a drag: a circle or arc grabbed by its edge takes `radius` (mm); false when its
    // size is held (then the edge carries it instead, SketchTools.dragMove).
    Q_INVOKABLE bool dragSketchSize(int sketch, int arc, double radius);
    // `hold`: where the drag snapped, to hold it there (owner, 2026-10-30: drags snap like
    // drawing): {point: the dragged point, on: "point" | "origin" | "midpoint" | "line",
    // target: the point or line snapped to}. Its Coincident / Midpoint goes in the same step;
    // dropped (the move stays) when it already holds that way or can't hold.
    // Returns the constraint's name ("Coincident", "Midpoint"), or "" if none was made.
    Q_INVOKABLE QString endSketchDrag(int sketch, const QString &summary, const QVariantMap &hold = {});

    // Fusion's Fix/UnFix on these points: if every one is already fixed they're all let go,
    // otherwise the ones not fixed yet are held where they are. An action. Returns what
    // was done ("Fixed Point3"), or "" if nothing.
    Q_INVOKABLE QString fixSketchPoints(int sketch, const QVariantList &pointIds);

    // Takes these constraints (ids, dimensions too) off a sketch, leaving its points and lines
    // where they are (owner, 2026-10-30: change a design without redrawing it). One action
    // named `summary`; Ctrl+Z puts them all back. Returns how many went.
    Q_INVOKABLE int deleteSketchConstraints(int sketch, const QVariantList &ids, const QString &summary);

    // Fusion's Normal/Construction on these parts ({kind: "sketch_line" | "sketch_arc" |
    // "sketch_curve", index}): all become construction if any wasn't, else all normal again.
    // One action; Ctrl+Z puts them back. Returns what was done ("Made Line3 construction"), or
    // "" if none of them can be (points, the axes).
    Q_INVOKABLE QString toggleSketchConstruction(int sketch, const QVariantList &parts);

    // Fusion's Sketch Dimension. type "length" (line first), "distance" (points first and
    // second), "pointline" (point first, line second) or "lineline" (two parallel lines: the
    // gap between them), "angle" (two lines; otherSide: the angle with the second line taken
    // end to start, the corner's other side). value in mm (radians for an angle); unit is what it was
    // typed in ("in", "ft", "mm", "cm", "m"), kept as the dimension's main unit; label is
    // the value as shown ("2.000 in"), for the history and messages. Moves as few points as
    // it can. An action. Returns "" when done, or why not.
    Q_INVOKABLE QString addSketchDimension(int sketch, const QString &type, int first, int second,
                                           double value, const QString &unit, const QString &label,
                                           bool otherSide = false);
    // Where a dimension's value shows, dragged by hand: (u, v) mm from the middle of what it
    // measures. Kept with the dimension and saved; not a history step (owner, 2026-10-30).
    Q_INVOKABLE void placeDimensionLabel(int sketch, int constraint, double u, double v, bool placed = true);
    // A new value for a dimension already there (constraint id). An action.
    Q_INVOKABLE QString setSketchDimension(int sketch, int constraint, double value,
                                           const QString &unit, const QString &label);
    // What a dimension of that kind would read now: {value (mm), a, b} (engine coordinates of
    // the two places it spans), or {} if those parts can't be dimensioned.
    Q_INVOKABLE QVariantMap sketchMeasure(int sketch, const QString &type, int first, int second,
                                          bool otherSide = false) const;
    // Every dimension in a sketch: [{id, type, value, unit, a, b}].
    Q_INVOKABLE QVariantList sketchDimensions(int sketch) const;
    // The sketch's profiles and gaps (profiles.h), from its normal lines, arcs and curves:
    // {profiles: [{outer: [{u, v}], holes: [[{u, v}]], area}], gaps: [{a, b: {u, v, at},
    // size}]} (u, v on the sketch in mm; at: the point in the model, for the screen).
    Q_INVOKABLE QVariantMap sketchProfiles(int sketch, double gapMax = 1.0) const;

    // The open sketch's point or line under a ray (engine coordinates):
    // {kind: "sketch_point" | "sketch_line" | "", sketch, index, depth}. Points win over lines.
    Q_INVOKABLE void setSketchShows(bool points, bool projected);
    Q_INVOKABLE QVariantList sketchOutlines(int sketch) const;
    Q_INVOKABLE QVariantMap pickSketch(int sketch, QVector3D origin, QVector3D direction,
                                       float pointReach, float edgeReach) const;

    // Every line of these sketches as a thin tube, every point as a small ball: what can
    // still move into `target`, what's fully constrained into `fullTarget`.
    // Construction lines go dashed into `construction` / `constructionFull` the same way.
    // Fusion's Project: the shadow of the whole model (face -1) or one face, straight down onto
    // the sketch, as lines, arcs and circles held where the model put them.
    Q_INVOKABLE QVariantMap projectIntoSketch(int sketch, int face);
    // The face a ray hits first, edges and points ignored; -1 for none.
    Q_INVOKABLE int faceAt(QVector3D origin, QVector3D direction) const;
    Q_INVOKABLE void fillSketches(MeshGeometry *target, MeshGeometry *fullTarget, MeshGeometry *construction,
                                  MeshGeometry *constructionFull, MeshGeometry *projected, MeshGeometry *projectedDashed, const QVariantList &sketches,
                                  float radius) const;
    // Loose segments as tubes (a tool's preview): points in engine coordinates, two per segment;
    // dashed when what's drawn will be construction.
    // Every closed profile of the given sketches, filled flat on its sketch plane (the light
    // shading that shows a shape is closed). Recomputed only when sketches change.
    Q_INVOKABLE void fillProfiles(MeshGeometry *target, const QVariantList &sketches) const;
    // The profile under a ray, nearest first, among the given sketches' profiles:
    // {kind: "profile", sketch, index, u, v (the spot hit, inside it), depth}, or {kind: ""}.
    Q_INVOKABLE QVariantMap pickProfile(const QVariantList &sketches, QVector3D origin, QVector3D direction) const;
    // Which of a sketch's profiles holds the spot (u, v): its index now, or -1 (a picked
    // profile is kept by a spot inside it, as profiles are renumbered when lines change).
    Q_INVOKABLE int profileAt(int sketch, double u, double v) const;
    // The given profiles ({sketch, index}), filled (their hover and selection colours).
    Q_INVOKABLE void fillProfileParts(MeshGeometry *target, const QVariantList &parts) const;
    Q_INVOKABLE void fillSegments(MeshGeometry *target, const QVariantList &ends, float radius, bool dashed = false) const;

    // Saving (Fusion's Save / Save As), as an .omamake file: plain, indented JSON holding
    // everything — components, bodies (how each was made, and its mesh in numbers), sketches
    // with their planes, points, lines and constraints, what's hidden, and the whole
    // history — so it can be read by a person as well as opened again. The design takes the
    // file's name (instead of "(Unsaved)"). Not an action in the history. Returns "" when
    // saved, or why not.
    QString filePath() const { return m_filePath; }
    Q_INVOKABLE QString saveTo(const QString &path);
    // Import a STEP, STL or OBJ: read in the background (importing, importStage), then importFinished. Holds
    // one body for now, so it says no while there is one. The file itself is kept in the design.
    Q_INVOKABLE void importFile(const QString &path);
    // Export (owner, 2026-10-31): the body as a STEP file for now. Returns "" when written, or why not.
    Q_INVOKABLE QString exportTo(const QString &path);
    Q_INVOKABLE QString exportName() const;
    // Fit Curved Surfaces (owner, 2026-10-30): asked for, not automatic; background, undoable.
    Q_INVOKABLE void fitSurfaces();
    bool importing() const { return m_importing; }
    QString importStage() const { return m_importStage; }
    QString importName() const { return m_importName; }
    // Load (Fusion's Open; owner, 2026-10-30): reads an .omamake file back into this window, in place
    // of what's here: components, bodies (rebuilt from the history), sketches exactly as saved,
    // what's hidden, and the history (shown in the Timeline; Ctrl+Z can't take back what was
    // done before it was saved, as in Fusion). Opens with no sketch being edited. Returns "" when
    // opened, or why not (and then nothing here has changed).
    Q_INVOKABLE QString openFrom(const QString &path);
    // Nothing done in this window yet and never saved: Load can use it instead of a new window.
    Q_INVOKABLE bool untouched() const;
    // Opens `path` in a new OmaMake window (Load while this one holds work).
    Q_INVOKABLE void openInNewWindow(const QString &path) const;

    // Placeholder until sketch + extrude exist: a cube sitting centred on the floor.
    void addCube(double sizeMm);
    // The cube itself, joined to the body there is (no history step).
    void buildCube(double sizeMm);

signals:
    void changed();
    void historyChanged();
    void structureChanged();
    void sketchesChanged();        // something drawn in a sketch was added, moved or taken away
    void fileChanged();
    void importChanged();
    void importFinished(const QString &why, double seconds, int faces, int triangles);
    void fitFinished(const QString &why, double seconds, int cylinders, int cones, int spheres, int trianglesLeft, double worstMm);

private:
    // Add an action to the history; summary is the one line a person reads.
    void record(const QJsonObject &action, const QString &summary);
    struct Named;
    QString addOtherConstraint(Named &named, int sketch, const QString &type, int first, int second, int third, int keep);
    void refreshMesh();
    void placeImport(omamake::BodyId body, const QString &name);
    void swapBody(omamake::BodyId body);

    std::unique_ptr<omamake::GeometryEngine> m_engine;
    bool m_importing = false;
    QString m_importStage, m_importName;
    QByteArray m_stepFile;     // a design saved before body_file: the imported file, packed
    QByteArray m_bodyFile;     // the imported body's exact solid, packed, saved with the design
    struct Fitted { omamake::BodyId body; QByteArray file; };
    std::vector<Fitted> m_beforeFit;   // what each Fit Curved Surfaces replaced, for undo
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
        QVariantMap plane;            // sketches only
        SketchGeometry geometry;      // sketches only
    };
    QVector3D onSketch(const Named &sketch, double u, double v) const;
    std::pair<std::vector<profiles::Path>, std::vector<int>> profilePaths(const SketchGeometry &g) const;
    // Each sketch's profiles, worked out once per change (hovering asks on every mouse move).
    const std::vector<profiles::Profile> &profilesOf(int id, const Named &sketch) const;
    void sketchesEdited();
    bool lineHidden(const SketchGeometry &g, int a, int b) const;
    bool pointHidden(const SketchGeometry &g, int id) const;
    bool m_showPoints = true, m_showProjected = true;
    void addProfile(omamake::Mesh &out, const Named &sketch, const profiles::Profile &p) const;
    mutable std::map<int, std::vector<profiles::Profile>> m_profiles;
    // "Point3", "Line2", or the origin's own: "the Origin", "the X axis" (named after the model's
    // axis it lies along). kind: 'P' a point, 'L' a line.
    QString sketchPartName(const Named &sketch, QChar kind, int id) const;
    // A new point snapped onto the origin, a line's middle or a line (`at`'s "on"): the constraint that
    // keeps it there, its id added to `constraints`.
    void holdSnap(SketchGeometry &g, int point, const QVariant &at, const QJsonArray &created, QJsonArray &constraints);
    int sketchPoint(Named &sketch, const QVariant &at, QJsonArray &created);
    void forgetSketchLines(Named &sketch, const QJsonObject &action);
    void removeSketch(int id);
    void removeComponent(int id);
    QString componentName(int id) const;
    QVariantMap markVisible(QVariantMap node, bool parentVisible) const;
    static Component rootComponent() {
        Component root;
        root.name = QStringLiteral("(Unsaved)");   // the document's name, until it's saved
        return root;
    }
    Component *component(int id);
    QVariantMap componentNode(const Component &c) const;

    std::vector<Component> m_components = {rootComponent()};
    std::map<int, Named> m_bodies, m_sketches;   // by id
    std::set<QString> m_hidden;   // keys of hidden rows (the design's own origin starts shown)
    int m_active = 0;
    std::map<int, SketchGeometry::Point> m_dragBefore;
    std::map<int, SketchGeometry> m_snapshots;   // sketches as they were before each "shape" action (Ctrl+Z)
    int m_nextSnapshot = 1;   // where the dragged sketch's points were
    int m_openSketch = 0;   // the sketch being edited (0: none); what's done meanwhile is its history
    int m_nextComponent = 1;
    int m_nextSketch = 1;
    QJsonArray m_actions;   // every change, in order (saved with the file)
    QString m_filePath;     // where it was last saved ("" until then)
    QJsonObject toJson() const;
    omamake::Mesh m_mesh;   // the model as last built, with its faces, edges and points
    QVector3D m_boundsMin, m_boundsMax;
};
