#pragma once

// The open model: its bodies, built by the engine, and the full action log.

#include "document/sketchgeometry.h"
#include "document/profiles.h"
#include "engine/engine.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QObject>
#include <QVariantList>
#include <QVariantMap>
#include <QVector3D>

#include <climits>
#include <atomic>
#include <memory>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <memory>
#include <vector>

class MeshGeometry;
class QTimer;

class Document : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString engineName READ engineName CONSTANT)
    Q_PROPERTY(QVector3D boundsMin READ boundsMin NOTIFY changed)
    Q_PROPERTY(QVector3D boundsMax READ boundsMax NOTIFY changed)
    Q_PROPERTY(int historyLength READ historyLength NOTIFY historyChanged)
    Q_PROPERTY(QVariantList history READ history NOTIFY historyChanged)
    Q_PROPERTY(bool canRedo READ canRedo NOTIFY historyChanged)
    Q_PROPERTY(bool rolledBack READ rolledBack NOTIFY historyChanged)
    Q_PROPERTY(QVariantList future READ future NOTIFY historyChanged)
    Q_PROPERTY(int presentCount READ presentCount NOTIFY historyChanged)
    Q_PROPERTY(bool futureStale READ futureStale NOTIFY historyChanged)
    Q_PROPERTY(int sketchReadyFrom READ sketchReadyFrom NOTIFY walkChanged)
    Q_PROPERTY(bool sketchWalkDone READ sketchWalkDone NOTIFY walkChanged)
    Q_PROPERTY(int modelBuiltTo READ modelBuiltTo NOTIFY buildChanged)
    Q_PROPERTY(int modelReadyFrom READ modelReadyFrom NOTIFY buildChanged)
    Q_PROPERTY(bool modelBuildDone READ modelBuildDone NOTIFY buildChanged)
    Q_PROPERTY(QVariantMap browser READ browser NOTIFY structureChanged)
    Q_PROPERTY(int activeComponent READ activeComponent WRITE setActiveComponent NOTIFY structureChanged)
    Q_PROPERTY(QString filePath READ filePath NOTIFY fileChanged)
    Q_PROPERTY(bool importing READ importing NOTIFY importChanged)
    Q_PROPERTY(bool working READ working NOTIFY workingChanged)
    Q_PROPERTY(QString importStage READ importStage NOTIFY importChanged)
    Q_PROPERTY(QString importName READ importName NOTIFY importChanged)

public:
    // `background`: a Document of the background rebuild's own (no timers, nothing logged).
    explicit Document(std::unique_ptr<omamake::GeometryEngine> engine, QObject *parent = nullptr, bool background = false);

    QString engineName() const;
    // Engine coordinates (mm, Z up).
    QVector3D boundsMin() const { return m_boundsMin; }
    QVector3D boundsMax() const { return m_boundsMax; }
    // The history: every action, oldest first, each with its time, its type, a one-line
    // summary in plain words, and everything needed to take it back. Ctrl+Z steps back
    // through it one action at a time, in Model and Sketch mode alike. Saved with the file later.
    int historyLength() const { return int(m_actions.size()); }
    QVariantList history() const;

    // Ctrl+Z: take back the newest action and drop it from the history. Returns that
    // action, so the window can follow (e.g. back into a sketch whose finishing was undone),
    // or an empty map when there's nothing left to undo (the starting model stays).
    Q_INVOKABLE QVariantMap undo();
    // Ctrl+Y (owner, 2026-11-07): put back the newest action Ctrl+Z took back, exactly as it was
    // (the state just before that undo, kept whole, solids included: nothing is worked out again).
    // Anything new done after an undo ends what can be redone. Returns that action, or an empty map.
    Q_INVOKABLE QVariantMap redo();
    bool canRedo() const { return !m_redo.empty(); }

    // The Timeline's marker (owner, 2026-11-07; history/2026-11-07_timeline-rewind.md). Moving it back takes
    // steps back the way Ctrl+Z does, but keeps them as the steps behind the marker (greyed in the
    // Timeline, `future` in `history`); moving it forward puts them back as Ctrl+Y does (nothing built
    // again). rewindTo(n): leave the first n history items in place; returns how many are now. Stops
    // early at a step that can't be taken back. No window side effects: a sketch's steps go together,
    // unless `inSketch` (the open sketch's own Timeline row, owner 2026-11-07): then the sketch stays open.
    Q_INVOKABLE int rewindTo(int count, bool inSketch = false);
    // Sketches drawn in between (owner, 2026-11-07: "any way that we can animate things, go ahead").
    // setSketchTween(sketch, actions, t): sketch `sketch` is drawn `t` (0 to 1) of the way to how the
    // next `actions` steps behind the marker leave it: what they draw grows from where it was started
    // (a line from its first end, a rectangle from its first corner, a circle from its centre), what a
    // constraint or dimension moves glides there, what they take away shrinks back. t 0 or 1, or
    // sketch 0: drawn as it is. Returns whether it is drawn in between.
    Q_INVOKABLE bool setSketchTween(int sketch, int actions, double t);
    // A sketch opened again: its steps from the times before (owner, 2026-11-07: the marker must move
    // over all of them). The document can't go back past this opening without leaving the sketch, so
    // those are only shown: prepareSketchPast(sketch, counts) works out how the sketch looked at each of
    // those history lengths (the design taken back and put forward again, nothing built, nothing
    // announced); showSketchPast(sketch, k, t) draws it `t` of the way from state k to state k + 1 (the
    // last one: as it is now). k -1: drawn as it is. Returns how many states it has / whether shown.
    // The background walk (owner, 2026-11-07: "generate those previews in the background"): every sketch as it
    // was before each step, worked out from the records alone (newest first, a few ms at a time, so the app stays
    // usable; nothing saved). sketchReadyFrom: the history length from which on every state is known (0: all of it;
    // it falls as the walk goes back). sketchWalkDone: it has gone as far back as it can.
    int sketchReadyFrom() const { return m_walkAt; }
    bool sketchWalkDone() const { return m_walkStuck || m_walkAt == 0; }
    // A sketch as it was when the history was `count` long (false: not worked out yet, or it didn't exist).
    bool sketchAt(int sketch, int count, SketchGeometry &out) const;
    Q_INVOKABLE int prepareSketchPast(int sketch, const QVariantList &counts);
    Q_INVOKABLE bool showSketchPast(int sketch, int k, double t);
    // While sketching, whatever moves the open sketch's lines other than the mouse (a constraint, a
    // dimension, Ctrl+Z...) glides there instead of jumping (at most 250 ms; owner 2026-11-07).
    // takeLiveTween(): a glide is waiting (the window then runs it with liveTween(t), t 0 to 1).
    // holdLive(true): none started (the marker is being moved inside the sketch).
    Q_INVOKABLE bool takeLiveTween();
    Q_INVOKABLE void liveTween(double t);
    Q_INVOKABLE void holdLive(bool hold) { m_liveHeld = hold; }
    // The design's steps from an opened file, built again in the background (owner, 2026-11-07: "whether it was a
    // sketch made this session or opening up an old file ... generate those previews in the background"). A file
    // keeps only each body as it is now and each step's record; a second Document with its own engine, on a thread
    // of its own, makes every step again from its record, oldest first, and the bodies as they were after each step
    // come back here (solids and meshes; nothing saved). From then on the marker and Ctrl+Z go back over those steps
    // as over this session's. modelBuiltTo: the history length up to which they're here (-1: nothing to build);
    // modelReadyFrom: from which history length on the marker can go back now (the file's steps: once all are
    // built); modelBuildDone: nothing more is coming (built, or it couldn't be: buildWhy says why).
    int modelBuiltTo() const { return m_builtTo; }
    int modelReadyFrom() const { return m_buildDone && m_buildWhy.isEmpty() ? 0 : std::min(m_loaded, int(m_actions.size())); }
    bool modelBuildDone() const { return m_buildDone; }
    Q_INVOKABLE QString buildWhy() const { return m_buildWhy; }
signals:
    void walkChanged();
    void buildChanged();
public:
    // For the rewind animation of the next Timeline item behind the marker (its `actions` history
    // steps, put back in one): `before` (the bodies shown now) and `after` (the bodies once it is
    // put back), `type` (the item's last step), and for an extrude its plane (`origin`, `axis`, in
    // engine coordinates) and the span it fills along the axis (`low`..`high`, mm, from the plane), and
    // where on that plane: `box` [u0, u1, v0, v1] in the plane's `xAxis` / `yAxis`, taper allowed for.
    Q_INVOKABLE QVariantMap rewindStep(int actions);
    // These bodies' meshes, joined, into a view's shaded and outline geometry (the rewind's after shape).
    Q_INVOKABLE void fillBodies(MeshGeometry *faces, MeshGeometry *edges, const QVariantList &ids) const;
    bool rolledBack() const { return !m_future.empty(); }
    // The steps behind the marker, oldest first, as `history` gives them, each with `future` true
    // and `step` (its place in the whole history: the present ones' count and on).
    QVariantList future() const;
    int presentCount() const { return int(m_actions.size()); }
    // A step was made while the marker was back: the steps behind it can't be put back until
    // they are built again on top of it (not there yet: history/2026-11-07_timeline-rewind.md phase 5).
    bool futureStale() const { return !m_future.empty() && m_staleAt && int(m_actions.size()) >= m_staleAt; }
    // The action Ctrl+Y would put back (an empty map: none), so the window can get ready for it.
    Q_INVOKABLE QVariantMap redoNext() const { return m_redo.empty() ? QVariantMap{} : m_redo.back().action.toObject().toVariantMap(); }

    // Fill a view's shaded geometry and its outline geometry with the model.
    Q_INVOKABLE void fill(MeshGeometry *faces, MeshGeometry *edges) const;

    // What's under a ray (engine coordinates): {kind: "point" | "edge" | "face" | "", index}.
    // Points win over edges, edges over faces, like Fusion. A point or edge counts when the
    // ray passes within (distance along the ray × pointReach / edgeReach) of it and nothing
    // solid is in front of it.
    // through: Select Through, edges and corners behind the model count too (owner, 2026-11-02).
    Q_INVOKABLE QVariantMap pick(QVector3D origin, QVector3D direction, float pointReach, float edgeReach, bool through = false) const;
    Q_INVOKABLE int faceTotal() const { return int(m_mesh.faceCount); }

    // Shapes that show parts of the model (hover or selection): faces as copies of their
    // triangles, edges as thin tubes, points as small balls. parts: [{kind, index}, ...].
    // With perMm (perspective views, body parts) thickness is perMm × the part's own distance from
    // eye, so a highlight close to the camera stays thin however far away the pivot is.
    Q_INVOKABLE void fillParts(MeshGeometry *target, const QVariantList &parts, float edgeRadius,
                               QVector3D eye = {}, float perMm = 0, bool lean = false) const;

    // The plane a sketch on this face sits on (engine coordinates):
    // {flat, origin, normal, xAxis, yAxis, center}. flat is false for a curved face.
    // The normal points out of the body. The sketch's own Y axis is "up" (world Z, or
    // world Y on a face that lies flat), and the origin is the world origin dropped onto
    // the plane, as in Fusion, so sketches on parallel faces line up.
    Q_INVOKABLE QVariantMap facePlane(int face) const;

    // EXTRUDE (owner, 2026-11-02). `spec` is a map:
    //   areas: [{kind:"sketch", sketch, profile} or {kind:"face", face}]   (all on one plane)
    //   mode: "one" | "symmetric" | "two";  d1, d2: distances in mm (d2 only for "two");
    //   perArea: [mm per area for side 1] (optional; the per-area distance toggle);
    //   op1, op2: "auto" | "join" | "cut" | "new" | "component" (side 1, side 2);
    //   skip1, skip2: body numbers whose cutting is switched off; wholeLength: symmetric distance is the whole length.
    // extrudePlan says what each side would do and which bodies it touches or cuts; extrude does it as ONE history item.
    Q_INVOKABLE QVariantMap extrudePlan(const QVariantMap &spec);
    Q_INVOKABLE QVariantMap extrude(const QVariantMap &spec);
    // The same off the main thread; the answer comes as extrudeFinished. False and why if it can't start.
    Q_INVOKABLE QVariantMap extrudeInBackground(const QVariantMap &spec);
    // Where the arrows and per-area boxes sit: {ok, normal, anchors: [point per area]} (each area's middle on its plane).
    Q_INVOKABLE QVariantMap extrudeFrame(const QVariantMap &spec);
    Q_INVOKABLE void fillExtrudePreview(MeshGeometry *target, MeshGeometry *wire, const QVariantMap &spec, int side);
    Q_INVOKABLE void fillExtrudeCutaway(MeshGeometry *target, const QVariantMap &spec, int side);
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
    // Would opening this sketch (or, with 0, finishing the open one) take things out of sight?
    Q_INVOKABLE bool rewinds(int sketch) const;

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
    // Tests: a sketch's points as the background walk knows them at history length `count` (empty: not known yet),
    // and as they are drawn now (a glide or a past state on show) — {id: [u, v]}.
    Q_INVOKABLE QVariantMap walkedSketchPoints(int sketch, int count) const;
    Q_INVOKABLE QVariantMap shownSketchPoints(int sketch) const;

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
    // Text laid out in a w by h mm box (SketchGeometry::textInBox); the families installed on this computer.
    Q_INVOKABLE QVariantList sketchTextInBox(const QString &text, const QString &font, int flags, double height, double w, double h) const;
    Q_INVOKABLE QStringList fontFamilies() const;
    // Sliding a text along its path by the mouse: begin at the grab, slide with the mouse, end (one history step).
    // Test runs only (OMAMAKE_TEST_INPUT set): a real mouse event sent through the window, so a
    // drag can be tried the way the pointer does it. kind: press, move, release.
    Q_INVOKABLE void testMouse(QObject *window, const QString &kind, double x, double y);
    Q_INVOKABLE void testKey(QObject *window, int key, const QString &text = QString(), int modifiers = 0);
    Q_INVOKABLE bool beginSketchTextSlide(int sketch, int curve, double u, double v);
    Q_INVOKABLE bool slideSketchText(int sketch, int curve, double u, double v);
    Q_INVOKABLE bool endSketchTextSlide(int sketch);
    // Text laid along the parts in `codes` (kind * 1000000 + id): {shapes: [[{u, v}]], frame: [{u, v}]}.
    Q_INVOKABLE QVariantMap sketchTextOnPath(int sketch, const QString &text, const QString &font, int flags, double height,
                                             const QVariantList &codes) const;
    // Change a text's words, font, flags or height (props: text, font, flags, height); one undoable step.
    Q_INVOKABLE QString editSketchText(int sketch, int curve, const QVariantMap &props);
    Q_INVOKABLE void closeTextEdit();
    void recordTextEdit(int sketch, int curve, const QJsonObject &was, const QString &first, const QString &merged);   // the text panel left this text: its next edit is a new history item

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
                                           bool otherSide = false, const QString &expr = QString());
    // Where a dimension's value shows, dragged by hand: (u, v) mm from the middle of what it
    // measures. Kept with the dimension and saved; not a history step (owner, 2026-10-30).
    Q_INVOKABLE void placeDimensionLabel(int sketch, int constraint, double u, double v, bool placed = true);
    // A new value for a dimension already there (constraint id). An action.
    Q_INVOKABLE QString setSketchDimension(int sketch, int constraint, double value,
                                           const QString &unit, const QString &label,
                                           const QString &expr = QString(), bool exprFlip = false);
    // Parameters (owner, 2026-11-02, t91u). Rows are {id, name, unit, expr, comment}; unit is one of
    // in, ft, mm, cm, m, deg, rad, none. parameters(): what is saved, with each one's value shown.
    Q_INVOKABLE QVariantList parameters() const;
    // Checks a draft list as typed (nothing is changed): one map per row, in order:
    // {id, nameError, exprError, exprAt, shown, ok}.
    // Parameters to / from a spreadsheet file: .ods (default) or .csv (Fusion 360's columns); import also reads .xlsx.
    // exportParameters gives "" or why not; importParameters gives {error, rows:[{name,unit,expr,comment}], skipped:[text]}.
    Q_INVOKABLE QString exportParameters(const QString &path, const QVariantList &rows) const;
    Q_INVOKABLE QVariantMap importParameters(const QString &path) const;
    Q_INVOKABLE QVariantList checkParameters(const QVariantList &rows) const;
    // Makes the draft list the parameters (renames carry into dimensions; a dimension that follows a
    // formula follows the new values). One action. Returns "" or why not (nothing changed).
    Q_INVOKABLE QString applyParameters(const QVariantList &rows);
    Q_INVOKABLE void beginParameterSession();   // the Parameters window opened: its edits are one history item
    Q_INVOKABLE void endParameterSession();     // ... and it closed
    // A formula typed in a dimension's box: kind "length" or "angle". {ok, error, value (mm / rad),
    // unit, names}.
    Q_INVOKABLE QVariantMap evaluateExpression(const QString &text, const QString &kind) const;
    // `text` with the parameter name `from` written as `to` (whole names only).
    Q_INVOKABLE QString renameInExpression(const QString &text, const QString &from, const QString &to) const;
    // Why a name can't be used ("" if it can). Used for the live check beside the Name cell.
    Q_INVOKABLE QString parameterNameProblem(const QString &name) const;
    // What a dimension of that kind would read now: {value (mm), a, b} (engine coordinates of
    // the two places it spans), or {} if those parts can't be dimensioned.
    Q_INVOKABLE QVariantMap sketchMeasure(int sketch, const QString &type, int first, int second,
                                          bool otherSide = false) const;
    // Every dimension in a sketch: [{id, type, value, unit, a, b}].
    Q_INVOKABLE QVariantList sketchDimensions(int sketch) const;
    Q_INVOKABLE bool dimensionIsDriven(int sketch, const QString &type, int first, int second, bool otherSide = false) const;
    // The sketch's profiles and gaps (profiles.h), from its normal lines, arcs and curves:
    // {profiles: [{outer: [{u, v}], holes: [[{u, v}]], area}], gaps: [{a, b: {u, v, at},
    // size}]} (u, v on the sketch in mm; at: the point in the model, for the screen).
    Q_INVOKABLE QVariantMap sketchProfiles(int sketch, double gapMax = 1.0) const;

    // The open sketch's point or line under a ray (engine coordinates):
    // {kind: "sketch_point" | "sketch_line" | "", sketch, index, depth}. Points win over lines.
    Q_INVOKABLE void setSketchShows(bool points, bool projected);
    // `allow`: which kinds of sketch part count, as the bits of sketchPartClass (1 lines, 2 points,
    // 4 construction, 8 projected). All of them unless the Selectable menu switched some off.
    Q_INVOKABLE QVariantList sketchOutlines(int sketch, int allow = 15) const;
    // The model's edges and corner points as screen-testable paths, for a box drag while Project
    // runs: [{kind: "edge" | "point", index, paths: [[QVector3D...]]}], edges and/or points.
    Q_INVOKABLE QVariantList bodyOutlines(bool edges, bool points) const;
    // Each face of the model by the lines round its edge (where its triangles stop), for a box drag.
    Q_INVOKABLE QVariantList faceOutlines() const;
    // Which of these points (in space) the camera cannot see because the model stands in the way:
    // one true (hidden) or false per point. `eye` is the camera's place, or, when `ortho`, the
    // direction it looks (then every ray runs parallel). Points ON the surface count as seen. A box
    // drag takes only what is seen unless Select Through is on (owner, 2026-11-02).
    Q_INVOKABLE QVariantList hiddenFrom(const QVariantList &points, QVector3D eye, bool ortho) const;
    // Up to `most` points spread over a face (the middles of its triangles), to test it for being seen.
    Q_INVOKABLE QVariantList faceSamples(int face, int most) const;
    // The Selectable menu's kind of one sketch part: 1 a normal line, arc or curve, 2 a normal
    // point, 4 a construction line, arc or curve, 8 anything projected from the model; 0 if gone.
    Q_INVOKABLE int sketchPartClass(int sketch, const QString &kind, int index) const;
    Q_INVOKABLE QVariantMap pickSketch(int sketch, QVector3D origin, QVector3D direction,
                                       float pointReach, float edgeReach, int allow = 15) const;

    // Every line of these sketches as a thin tube, every point as a small ball: what can
    // still move into `target`, what's fully constrained into `fullTarget`.
    // Construction lines go dashed into `construction` / `constructionFull` the same way.
    // Fusion's Project: the shadow of the whole model (face -1) or one face, straight down onto
    // the sketch, as lines, arcs and circles held where the model put them.
    Q_INVOKABLE QVariantMap projectIntoSketch(int sketch, int face);
    // Project's third choice (owner, 2026-11-01): one edge ("edge", an index into the model's edges)
    // or one corner ("point") of the model, straight down onto the sketch. A straight edge is a
    // line, a round one an arc or circle, any other curve a spline through its points.
    // Another sketch, made earlier, projected with a live link: `parts` empty is the whole sketch,
    // else [{kind: "line" | "arc" | "point", index}] of it. Breaking the link is a step of its own.
    Q_INVOKABLE QVariantMap projectSketchIntoSketch(int sketch, int source, const QVariantList &parts);
    Q_INVOKABLE QVariantMap breakLink(int sketch, int link);
    // A Project run (the tool from its start to Done): the projections made in between become ONE
    // step in the history, so one Ctrl+Z (or Break Link) deals with all of them.
    Q_INVOKABLE void beginProjectRun(int sketch);
    Q_INVOKABLE QVariantMap endProjectRun(int sketch, int items);
    Q_INVOKABLE QVariantMap foldProjectRun(int sketch, int items);
    Q_INVOKABLE void undoProjectRun();
    // The sketches before this one, which can be projected into it (the rest are its future).
    Q_INVOKABLE QVariantList earlierSketches(int sketch) const;
    Q_INVOKABLE QVariantMap projectPartIntoSketch(int sketch, const QString &kind, int index);
    Q_INVOKABLE QVariantList projectPartsIntoSketch(int sketch, const QVariantList &parts);
    Q_INVOKABLE int currentBody() const;
    // The body a picked face, edge or corner belongs to (0: none).
    Q_INVOKABLE int bodyOfPart(const QString &kind, int index) const;
    // Which bodies are drawn and pickable (the Browser's eyes): their numbers. Not told yet: all.
    Q_INVOKABLE void showBodies(const QVariantList &ids);
    Q_INVOKABLE QStringList lineageOf(const QString &key) const;
    Q_INVOKABLE QVariantList bodiesUnder(int componentId) const;
    Q_INVOKABLE QVariantList projectFacesIntoSketch(int sketch, const QVariantList &faces);
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
    // One point inside each closed profile of a sketch (the cached ones, no gap search).
    Q_INVOKABLE QVariantList profileSpots(int sketch) const;
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
    bool working() const { return m_working; }
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
    void parametersChanged();
    void fileChanged();
    void importChanged();
    void workingChanged();
    void extrudeFinished(const QVariantMap &result);
    void importFinished(const QString &why, double seconds, int faces, int triangles);
    void fitFinished(const QString &why, double seconds, int cylinders, int cones, int spheres, int trianglesLeft, double worstMm);

private:
    // Add an action to the history; summary is the one line a person reads.
    void record(const QJsonObject &action, const QString &summary);
    struct Named;
    QString addCurvatureConstraint(Named &named, int sketch, int first, int second, int third);
    QString addOtherConstraint(Named &named, int sketch, const QString &type, int first, int second, int third, int keep);
    void refreshMesh();
    void placeImport(omamake::BodyId body, const QString &name);
    void swapBody(int was, omamake::BodyId body);

    std::unique_ptr<omamake::GeometryEngine> m_engine;
    bool m_importing = false;
    bool m_working = false;           // an extrude's solid work is running off the main thread
    QString m_importStage, m_importName;
    QByteArray m_stepFile;     // a design saved before body_file: the imported file, packed
    QByteArray m_bodyFile;     // the imported body's exact solid, packed, saved with the design
    struct Fitted { omamake::BodyId body; QByteArray file; };
    std::vector<Fitted> m_beforeFit;   // what each Fit Curved Surfaces replaced, for undo
    omamake::BodyId m_body = 0;   // the newest body (what Fit and Import act on); every body is in m_bodies
    // Bodies being several (owner, 2026-11-02): the model is drawn and picked as ONE mesh of every
    // shown body joined end to end; these say which body each face, edge and corner came from.
    std::vector<int> m_faceBody, m_edgeBody, m_pointBody;
    // The bodies' exact edges and corners seen on a sketch plane (one face's, or all for face -1), for Project.
    // Project, exactly, from the engine (whole model: face -1; else a mesh face number): 1 done, 0 the
    // engine has no exact edges (the Lite engine: the mesh is all there is), -1 failed (why says why).
    int exactShadow(const QVariantMap &plane, int face, std::vector<std::vector<omamake::FlatPiece>> &loops, QString &why) const;
    void exactEdges(const QVariantMap &plane, int face, std::vector<omamake::FlatCurve> &curves,
                    std::vector<omamake::Point2> &corners) const;
    std::set<int> m_shown;
    mutable std::map<int, omamake::Mesh> m_meshCache;   // each body's own mesh, made once per change of that body
    bool m_shownSet = false;
    int m_nextBody = 1;           // the number in the next new body's name (Body1, Body2, ...)
    // A body made by an operation: in `component`'s Bodies, named BodyN. Returns its number.
    int addBody(omamake::BodyId solid, int component, const QString &name = QString());
    // A body gone (its solid released).
    void dropBody(int id);
    struct ExtrudeSide {
        QString op = QStringLiteral("auto");
        std::vector<double> from, to;          // per area
        std::set<int> skip;
        double taper = 0;                      // degrees, positive widens (Fusion's way round)
        QString resolved;                      // what auto came to
        std::vector<int> hits, touches;        // bodies it cuts into / sits against
    };
    struct ExtrudeJob {
        std::vector<omamake::Placement> places;
        std::vector<omamake::Profile> profiles;
        std::vector<std::pair<int, int>> faces;   // per area: (body, face number in it) for a body's face, else (0, -1)
        std::vector<ExtrudeSide> sides;
        bool fuzzy = false;   // Fuzzy profile: set only for the automatic second try (computeExtrudeTrying); see kFuzzyProfile
    };
    bool readExtrude(const QVariantMap &spec, ExtrudeJob &job, QString &why) const;
    void arcCentres(const SketchGeometry &g, omamake::Profile &profile) const;
    void exactCurves(const SketchGeometry &g, omamake::Profile &profile) const;
    // How far along `normal` from `start` (the areas' middle, on their plane) a picked part lies, for
    // To Object. False and why if it can't be reached that way.
    bool extrudeReach(const QVariantMap &part, const double start[3], const double normal[3], double &mm, QString &why) const;
    // `areas`, if given, gets the area each tool was made from.
    // The side's tools, one per area (two for a tapered symmetric one). Areas that couldn't be made are
    // left out, the engine's reason for each added to `problems`.
    std::vector<omamake::BodyId> extrudeTools(const ExtrudeJob &job, int side, std::vector<std::size_t> *areas = nullptr,
                                              QStringList *problems = nullptr) const;
    // One area pushed: a body's face exactly as it is (extrudeFace), else its profile.
    omamake::BodyId extrudeArea(const ExtrudeJob &job, std::size_t area, double from, double to, double taper) const;
    void planExtrude(ExtrudeJob &job);
    struct ExtrudeResult {
        std::vector<std::pair<int, int>> swaps;                          // (body as it was, solid it is now)
        std::vector<std::pair<bool, std::vector<omamake::BodyId>>> made;  // (in a new component?, new solids)
        std::map<int, omamake::Mesh> meshes;
        QStringList notes, summary;
        QStringList trace;     // every step, for a failure's log
        bool failed = false;   // a body's operation couldn't be built, or a result wasn't sound: all put back
        bool booleanFailed = false;   // a cut, join or intersect couldn't be built (the only kind Fuzzy profile can help)
        bool fuzzyTried = false;      // made on the automatic second try, with Fuzzy profile
        double modelLoose = 0;        // with Fuzzy profile: the loosest edge (mm) of the bodies it worked on
    };
    ExtrudeResult computeExtrude(const ExtrudeJob &job) const;
    // An extrude worked out exactly; if a cut, join or intersect couldn't be built that way, the whole of it
    // again with Fuzzy profile (kFuzzyProfile). Both tries go in the trace.
    ExtrudeResult computeExtrudeTrying(ExtrudeJob job) const;
    QString writeExtrudeLog(const ExtrudeResult &r, const QVariantMap &spec, const QString &said) const;
    QVariantMap applyExtrude(ExtrudeResult &&r, const QVariantMap &spec);
    const omamake::Mesh &bodyMesh(int id) const;
    std::map<int, std::vector<std::pair<int, int>>> m_extrudeKeep;   // per extrude: (old solid, solid it became), for undo
    int m_extrudeToken = 1;
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
    void forgetSketchLines(SketchGeometry &g, const QJsonObject &action);
    void removeSketch(int id);
    void removeComponent(int id);
    QString componentName(int id) const;
    QVariantMap markVisible(QVariantMap node, bool parentVisible, const std::set<QString> &future) const;
    // While a sketch from the past is open, the rows made after it (their keys): the document is
    // taken back to when that sketch was made, so they are out of sight and out of reach.
    std::set<QString> futureKeys(int sketch = -1) const;
    // Is the model made after the open sketch (so it can't be projected into it)?
    bool bodyIsFuture() const;
    // Links (see SketchGeometry::Link).
    int addBodyLink(SketchGeometry &g, const SketchGeometry &before, bool whole) const;
    bool syncLinks();
    void syncLink(const Named &target, SketchGeometry &g, SketchGeometry::Link &link, const Named &source,
                  QString &issue, QString &level) const;
    int sketchOrder(int id) const;
    static QJsonArray linksJson(const SketchGeometry &g);
    QJsonObject geometryJson(const SketchGeometry &g, const Named *s) const;
    static void readGeometry(SketchGeometry &g, const QJsonObject &o);
    QJsonObject sketchDiff(const SketchGeometry &before, const SketchGeometry &after) const;
    static void unDiff(SketchGeometry &g, const QJsonObject &d);
    void keepBefore(QJsonObject &entry) const;
    int unstepSketch(const QJsonObject &last, const std::function<SketchGeometry *(int)> &sketchOf, std::map<int, SketchGeometry> *snapshots);
    static void readLinks(SketchGeometry &g, const QJsonArray &list);
    static Component rootComponent() {
        Component root;
        root.name = QStringLiteral("(Unsaved)");   // the document's name, until it's saved
        return root;
    }
    Component *component(int id);
    QVariantMap componentNode(const Component &c) const;

    struct Parameter {
        int id = 0;
        QString name, unit = QStringLiteral("in"), expr, comment;
    };
    std::vector<Parameter> m_params;
    int m_nextParameter = 1;
    bool m_paramSession = false;     // the Parameters window is open
    int m_paramItem = -1;            // its history item (index in m_actions), once it made one
    // Puts every formula-following dimension at the value its formula gives under `params`; on a
    // failure (a formula that doesn't work, or a sketch that can't hold) changes nothing and says why.
    QString followParameters(const std::vector<Parameter> &params, QJsonArray &dimsWas, QJsonArray &movedWas,
                             const std::map<QString, QString> &renames);
    std::vector<Component> m_components = {rootComponent()};
    std::map<int, Named> m_bodies, m_sketches;   // by id
    std::set<QString> m_hidden;   // keys of hidden rows (the design's own origin starts shown)
    int m_active = 0;
    std::map<int, SketchGeometry::Point> m_dragBefore;
    struct TextSlide {
        bool on = false;
        double from = 0, anchor = 0;       // where the mouse grabbed, and the anchor then
        QJsonObject was;
    } m_slide;
    QVariantMap projectPart(int sketch, const QString &kind, int index, bool notify);
    std::map<int, SketchGeometry> m_snapshots;   // sketches as they were before each "shape" action (Ctrl+Z)
    int m_nextSnapshot = 1;   // where the dragged sketch's points were
    int m_openSketch = 0;   // the sketch being edited (0: none); what's done meanwhile is its history
    int m_nextComponent = 1;
    int m_nextSketch = 1;
    QJsonArray m_actions;   // every change, in order (saved with the file)
    // Redo: what each undo took back, newest last. Each is the document as it was just before that
    // undo; the solids and meshes the undo let go of are kept here (parked) instead of released, so
    // putting it back is instant. Released when what can be redone ends (clearRedo).
    struct Redo {
        QJsonValue action;
        std::vector<Component> components;
        std::map<int, Named> bodies, sketches;
        std::set<QString> hidden;
        int active = 0, openSketch = 0, nextComponent = 1, nextBody = 1;
        std::map<int, SketchGeometry> snapshots;
        std::vector<Parameter> params;
        std::map<int, std::vector<std::pair<int, int>>> extrudeKeep;
        std::vector<Fitted> beforeFit;
        omamake::BodyId body = 0;
        QByteArray stepFile, bodyFile;
        std::vector<omamake::BodyId> parked;     // solids the undo let go of (their meshes stay cached)
    };
    std::vector<Redo> m_redo;
    std::vector<Redo> m_future;     // the steps behind the Timeline's marker, the next one last
    int m_staleAt = 0;              // history length at which a step made while the marker was back sits (0: none)
    // A sketch drawn in between two states of itself (setSketchTween, liveTween).
    struct SketchTween {
        int sketch = 0;
        int actions = 0;              // the marker's: that many steps ahead (0: a live glide)
        bool grow = false;            // what's new grows from where it was started (else it's simply there)
        double t = 1;
        SketchGeometry from, to, shown;
        std::set<int> heldFrom, heldTo;
        bool sameShape = false;       // nothing added or taken away: the shading can follow too
    };
    std::optional<SketchTween> m_tween;
    // The background walk (sketchReadyFrom). m_before[i]: the sketches step i changed, as they were just before it
    // (nullopt: it didn't exist yet), with the step's own record to know it's still the same step.
    struct Before {
        QString at;                        // the step's own time stamp: still the same step
        std::map<int, std::optional<SketchGeometry>> sketches;
    };
    std::vector<Before> m_before;          // [m_walkAt, m_beforeTop): worked out
    int m_walkAt = 0, m_beforeTop = 0;
    bool m_walkStuck = false;              // a step it can't take back (an older file's): nothing before it
    std::map<int, SketchGeometry> m_walkCur;   // the sketches when the history was m_walkAt long
    QTimer *m_walkTimer = nullptr;
    int m_lowSeen = INT_MAX, m_dirtyFrom = INT_MAX;   // the history went down to / was changed in place from
    void syncWalk();
    void walkSome();
    bool stepBack(int i, std::map<int, SketchGeometry> &cur);
    void restartWalk();
    int m_pastSketch = 0;
    std::vector<SketchGeometry> m_past;
    int m_pastFrom = 0;                // the first of m_past known
    bool m_quietSteps = false;        // undo / redo not logged (prepareSketchPast)
    // The background rebuild (modelBuiltTo). m_built[c]: the design (bodies, components) after the first c steps,
    // kept for c 0 and after each step that changes bodies; solids in this Document's engine.
    struct Built {
        QString at;                        // step c - 1's time stamp: still the same step
        std::vector<Component> components;
        std::map<int, Named> bodies;
        std::set<QString> hiddenBodies;    // "b<id>" keys
        int nextBody = 1, nextComponent = 1;
        omamake::BodyId body = 0;
    };
    std::map<int, Built> m_built;
    std::map<QString, QByteArray> m_importSolids;   // each import's solid, packed, by its step's time stamp
    std::set<int> m_builtOwned;            // solids loaded for m_built: never let go of by undo
    int m_loaded = 0;                      // the steps that came from the file
    int m_builtTo = -1;
    bool m_buildDone = true, m_buildStarted = false;
    QString m_buildWhy;
    int m_buildGen = 0;                    // which opening a result from the thread belongs to
    std::shared_ptr<std::atomic_bool> m_buildCancel;
    struct Arrived;                        // what the thread sends back (document.cpp)
    std::shared_ptr<Arrived> m_arrived;    // solids still to load here, a few at a time
    QTimer *m_buildTimer = nullptr;
    bool m_background = false;
    void startBuild();
    void cancelBuild();
    void loadSome();
    static bool changesBodies(const QJsonObject &a);
    const Built *builtAt(int count) const;
    void applyBuilt(const Built &b);
    int m_liveSketch = 0;             // the open sketch as last drawn, for a live glide's start
    SketchGeometry m_liveLast;
    bool m_liveHeld = false, m_liveWaiting = false;
    void blendTween();
    void noteLiveMove();
    const SketchGeometry &shownGeometry(int id, const SketchGeometry &real) const {
        return m_tween && m_tween->sketch == id ? m_tween->shown : real;
    }
    bool undoInto(std::vector<Redo> &into, QJsonObject &undone);
    bool redoFrom(std::vector<Redo> &from, QJsonObject &redone);
    void announceAll();
    Redo *m_parking = nullptr;    // set while an undo runs: what it lets go of goes here
    void releaseBody(omamake::BodyId body);   // released, or parked while an undo runs
    void clearRedo();
    int m_textEditItem = -1, m_textEditSketch = 0, m_textEditCurve = 0;   // newest text-edit history item (-1: none)
    int m_runStart = -1;    // where the open Project run began in m_actions (-1: none)
    int m_runSketch = 0;
    QString m_filePath;     // where it was last saved ("" until then)
    QJsonObject toJson() const;
    struct BvhNode { float lo[3]; float hi[3]; int left; int right; int first; int count; };
    mutable std::vector<BvhNode> m_bvh;      // the model's triangles in a tree, built when first needed
    mutable std::vector<int> m_bvhTriangles;
    omamake::Mesh m_mesh;   // the model as last built, with its faces, edges and points
    QVector3D m_boundsMin, m_boundsMax;
};
