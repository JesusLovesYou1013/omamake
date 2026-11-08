#pragma once

// What's drawn in one sketch, in the sketch's own flat coordinates (u along its X axis,
// v along its Y axis, mm): points, lines between them, and the constraints that hold
// lines in relation to each other. Lines that meet share their point, so moving one end
// moves both. solve() moves points as little as possible until every constraint holds.

#include <QString>

#include <utility>

#include <functional>
#include <map>
#include <set>
#include <vector>

struct SketchGeometry {
    struct Point {
        double u = 0, v = 0;
    };
    struct Line {
        int a = 0, b = 0;             // point ids
    };
    // An arc (owner, 2026-10-30: the curves behind Fusion's other drawing tools): centre c,
    // from end a round to end b counter-clockwise (seen with the sketch's u to the right and v
    // up); the solver keeps both ends the same distance from c. A full circle has a == b: that
    // one point on its rim (never shown or picked) is kept level with the centre, to the
    // right, so the only thing it adds is the radius.
    struct Arc {
        int c = 0, a = 0, b = 0;
        bool circle() const { return a == b; }
    };
    // The other curves (owner, 2026-10-30: every Fusion drawing tool), each through its points:
    //   "fitspline"      Fit Point Spline: a smooth curve through every point, in order
    //   "controlspline"  Control Point Spline: pulled toward its points (a cubic B-spline,
    //                    starting at the first and ending at the last); also Blend Curve
    //   "ellipse"        centre, the end of its long axis, the end of its short axis (the
    //                    solver keeps the two axes square to each other)
    //   "conic"          start, end, and the corner its two end directions meet at; `value` is
    //                    Fusion's Rho (0 to 1: flat to sharp; 0.5 is a parabola)
    //   "text"           `text` written from point 0 (its bottom left), `value` mm tall (an old,
    //                    one-point text); or, with two points, written in the box those two
    //                    corners make (owner, 2026-11-02): `font` the family ("" = the default),
    //                    `flags` below, `value` the capitals' height in mm unless the Fit flag
    //                    sizes the text to the box. The words wrap at the box's width.
    struct Curve {
        QString type;
        std::vector<int> points;
        double value = 0;
        QString text;
        QString font;
        int flags = 0;
        // Text on a path (owner, 2026-11-02): the lines, arcs and curves the words follow, each as
        // kind * 1000000 + id (1 line, 2 arc or circle, 3 other curve), the first picked first.
        // `points` is then just the first part's start. Empty: not on a path.
        std::vector<int> path;
        // Where on that path the text is anchored (mm along it, after Flip), once the owner has
        // dragged it there (flag TextAnchored). The justify then sits about it: left starts there,
        // centre is centred on it, right ends there. Unset: left at the start, centre in the
        // middle of the path, right at its end.
        double anchor = 0;
    };
    // Text flags: bit 0 bold, bit 1 italic, bit 2 fit to the box, bits 4-5 across (0 left,
    // 1 centre, 2 right), bits 6-7 down (0 top, 1 middle, 2 bottom).
    // On a path, bits 4-5 are the justify (left, centre, right along it) and bit 8 runs it backwards.
    enum TextFlag { TextBold = 1, TextItalic = 2, TextFit = 4, TextFlip = 256, TextAnchored = 512 };
    // type: "parallel" or "perpendicular", between two lines. first stays put where it
    // can; second turns to meet it (the order they were selected, as in Fusion).
    // "fix" (Fusion's Fix/UnFix): point `first` held at (u, v); second unused.
    // "horizontal" / "vertical": line `first` along the sketch's X / Y axis; second unused.
    // "equal": lines first and second the same length.
    // Added 2026-10-29 (owner: the rest of Fusion's constraints, two at a time):
    //   "horizontalpoints" / "verticalpoints"   points first and second level / one above
    //                                           the other (Fusion's Horizontal/Vertical on
    //                                           two points; on a line it's "horizontal" etc.)
    //   "coincident"     points first and second in the same place
    //   "pointonline"    point first on line second (or where it would go, extended)
    //   "midpoint"       point first at the middle of line second
    //   "collinear"      line second on the same straight line as line first
    //   "symmetry"       points first and second mirror each other across line third
    //   "symmetrylines"  lines first and second mirror each other across line third, ends
    //                    paired a↔a, b↔b (u = 1: a↔b, b↔a, whichever was nearer when made)
    // Dimensions (Fusion's Sketch Dimension), `value` in mm, `unit` as typed ("in", "mm"…,
    // shown as the dimension's main unit):
    //   "length"     line `first` is `value` long
    //   "distance"   points first and second are `value` apart
    //   "pointline"  point `first` is `value` from line `second` (measured square to it)
    //   "lineline"   line `second` is `value` from line `first`: its middle, measured square
    //                to `first` (owner, 2026-10-29: only offered for parallel lines, where
    //                the distance is the same all along)
    // Curves (owner, 2026-10-30; kind 'A' is an arc or circle, by its id in `arcs`):
    //   "tangent"       line first touches arc second (its centre one radius from the line)
    //   "tangentarcs"   arcs first and second touch: outside each other (u = 1) or one inside
    //                   the other (u = 0)
    //   "concentric"    arcs first and second share a centre
    //   "equalradius"   arcs first and second the same size (Fusion's Equal on curves)
    //   "pointonarc"    point first on arc second's circle (Fusion's Coincident on a curve)
    //   "radius" / "diameter"   dimensions of arc `first`
    //   "angle"         dimension: the angle (radians) between lines first and second, 0 to π,
    //                   each line taken from its a end to its b end (u = -1: the second
    //                   from b to a, the corner's other side)
    //   "midpoints"     point first halfway between points second and third (Center Rectangle,
    //                   Center Point Slot)
    //   "rotation"      point second is point first turned `value` radians (counter-clockwise)
    //                   about point third (Circular Pattern)
    //   "translation"   point second is point first moved by (u, v) mm (Rectangular Pattern)
    //   "offset"        dimension: line second is `value` × u mm to the left of line first
    //                   (seen from its a end; u = ±1, the side), square to it all along
    //   "offsetarcs"    dimension: arc second shares arc first's centre, its radius `value` × u
    //                   mm bigger
    // A dimension of one or two parts with `third` set takes its value from dimension `third`
    // (its leader): Fillet's corners all the radius of the first, every piece of an Offset the
    // same distance, a chamfer's two equal sides. Followers aren't shown; changing the leader
    // changes them all.
    struct Constraint {
        QString type;
        int first = 0, second = 0, third = 0;
        double u = 0, v = 0;
        double value = 0;
        QString unit;
        // A dimension's value where it was dragged to: mm from the middle of what it measures.
        bool labelPlaced = false;
        double labelU = 0, labelV = 0;
        // A driven dimension (owner, t88u): the other constraints already decide this measurement, so it
        // only shows what it is now; the solver never holds it and nothing can type its value.
        bool driven = false;
        // A dimension that follows a formula or parameter (owner, t91u): the text typed, and whether an angle's
        // shown side is the other one (stored value is then the rest of pi).
        QString expr;
        bool exprFlip = false;

        Constraint() = default;
        Constraint(QString type, int first, int second = 0, double u = 0, double v = 0)
            : type(std::move(type)), first(first), second(second), u(u), v(v) {}
        bool isDimension() const {
            return type == QLatin1String("length") || type == QLatin1String("distance") || type == QLatin1String("pointline")
                || type == QLatin1String("lineline") || type == QLatin1String("radius") || type == QLatin1String("diameter")
                || type == QLatin1String("angle") || type == QLatin1String("offset") || type == QLatin1String("offsetarcs");
        }
        // A dimension that follows another's value (see above): never shown on its own.
        bool follows() const { return third && isDimension(); }
    };

    // The model's origin and the sketch's two axes through it (owner, 2026-10-30: constraints
    // can tie the sketch to them, which is what locks it in place, fully constrained). They
    // have ids below 0, are never in `points` / `lines` and never move: point Origin, lines
    // XAxis (along u) and YAxis (along v). Ends -2 and -3 (1 mm out along each axis) only
    // give the axes a direction; they can't be picked.
    static constexpr int Origin = -1, XAxis = -1, YAxis = -2;
    static bool isReference(int id) { return id < 0; }
    Point origin;                     // where the model's origin falls in the sketch (u, v)
    bool hasPoint(int id) const { return points.count(id) || id == Origin; }
    bool hasLine(int id) const { return lines.count(id) || id == XAxis || id == YAxis; }
    Point point(int id) const;        // any point, the origin included
    Line line(int id) const;          // any line, the axes included

    std::map<int, Point> points;
    std::map<int, Line> lines;
    std::map<int, Arc> arcs;
    std::map<int, Curve> curves;
    std::map<int, Constraint> constraints;
    int nextPoint = 1, nextLine = 1, nextConstraint = 1, nextArc = 1, nextCurve = 1;
    bool directOnly = false;          // solve without the short-step retry, which can bend a shape
    // Helpers: what a tool draws only to hold its shape (Fusion draws them as construction
    // lines): a polygon's circle, a chamfer's lost corner. Never drawn, picked or snapped to.
    std::set<int> helperPoints, helperLines, helperArcs;
    // Construction lines, arcs and curves (Fusion's Normal/Construction, owner 2026-10-30):
    // drawn dashed, picked, snapped to, constrained and dimensioned like any other, but never
    // part of a profile.
    std::set<int> constructionLines, constructionArcs, constructionCurves;
    // Projected from the model (Fusion's Project, owner 2026-10-31): these points are held where
    // the model put them, like the origin, so nothing solves for them or drags them.
    std::set<int> projectedPoints;

    // A link (owner, 2026-11-01): geometry projected onto this sketch that stays tied to where it
    // came from. `source` is the sketch it came from (0: the model, whose link only records
    // that, as the model can't change yet). `whole`: all of that sketch was projected, so what
    // is added to it later comes in too. Otherwise only the `lines`, `arcs` and `points` picked.
    // The maps go from the source's id to ours: 0 not made yet, -1 taken off by the user (never
    // put back). `broken`: the user broke the link; the geometry stays, as plain geometry.
    // `level` is "" (fine), "yellow" (still follows its source, but the constraints on it can't
    // hold) or "red" (something it follows is gone); `issue` says why.
    struct Link {
        int id = 0;
        int source = 0;
        bool whole = false;
        bool broken = false;
        std::map<int, int> points, lines, arcs;
        std::set<int> wantPoints;      // source points picked by themselves
        std::set<int> owned;           // every point of ours the link made
        QString level, issue;
    };
    std::vector<Link> links;
    int nextLink = 1;

    // Whether a point is a circle's hidden rim point.
    bool isRim(int point) const;
    // Rim points and helper points: never drawn or picked.
    bool isHidden(int point) const { return isRim(point) || helperPoints.count(point); }
    double radius(int arc) const;
    // A dimension's value, its leader's when it follows one.
    double valueOf(const Constraint &c) const;
    // Whether anything holds the points at all (constraints, arcs, ellipses).
    bool holdsNothing() const;
    // Every curve, as lines short enough to draw and pick: arcs from a counter-clockwise to b
    // (all the way round for a circle), the other curves along their points, text glyph by
    // glyph (one list per outline).
    std::vector<std::vector<Point>> arcPath(int arc) const;
    std::vector<std::vector<Point>> curvePath(int curve) const;
    // Text's outlines, `height` mm tall, its bottom left at (0, 0).
    static std::vector<std::vector<Point>> textOutline(const QString &text, double height);
    // Text laid out in a w by h mm box (its bottom left at (0, 0)): wrapped, aligned, and, with
    // the Fit flag, as large as fits. `height` is the capitals' height when not fitting.
    static std::vector<std::vector<Point>> textInBox(const QString &text, const QString &family, int flags, double height,
                                                     double w, double h);
    // The parts of a text's path joined end to end, in the order the words run (empty if none):
    // starts at the first part picked, going from a line's A to B and clockwise round an arc.
    std::vector<Point> pathChain(const Curve &k, bool &closed) const;
    // Words laid along a chain (shapes: the letters; frame: a bracket round where they sit).
    struct Along {
        std::vector<std::vector<Point>> shapes;
        std::vector<Point> frame;
    };
    static Along textAlong(const QString &text, const QString &family, int flags, double height,
                           const std::vector<Point> &chain, bool closed, double anchor = 0);
    // How far along the path's chain the point nearest `at` is (and the chain's length, whether it closes).
    double textPathPosition(const Curve &k, const Point &at, double &total, bool &closed) const;
    // The box a two-point text sits in, as a closed outline (empty for any other curve).
    std::vector<Point> textFrame(int curve) const;

    // How far off the constraints are, all together (0: all hold).
    double error() const;

    // Move points so every constraint holds, each as little as possible; points in
    // `steady` move far less than the rest. Returns false (and leaves every point where it
    // was) when the constraints can't all hold at once.
    bool solve(const std::set<int> &steady = {});

    // Dragging: put these points where the mouse wants them, then let everything else
    // follow as little as it must so the constraints still hold (the dragged points
    // themselves give way only if the constraints leave no other choice). Points held by
    // Fix don't move. Returns false (nothing moved) when it can't be done.
    bool drag(const std::map<int, Point> &targets);
    // Slides arc `moving` over whole (its size kept) until tangent constraint c holds: against
    // the other arc of a "tangentarcs", or against the line of a "tangent".
    void touch(const Constraint &c, int moving);
    // Solves with only the points in `ring` free to move (`steady` ones hardly); false and
    // nothing moved when that can't work.
    bool solveOnly(const std::set<int> &ring, const std::set<int> &steady);
    // A circle or arc dragged by its edge takes radius r (its centre stays where it can);
    // false and nothing moved when its size is held.
    bool resize(int arc, double r);

    // Solve after a constraint was added or changed, moving as few points as possible: first
    // only `seeds` may move, then their neighbours, ring by ring, until it works.
    bool solveNear(const std::set<int> &seeds);

    // Curvature (G2, owner 2026-11-02): a line, arc or spline leaving a join: the point, the unit
    // direction heading away from the join, the curvature vector (towards the bend's centre, 1/radius long).
    struct Leave {
        double px = 0, py = 0, tx = 0, ty = 0, kx = 0, ky = 0;
        bool ok = false;
    };
    bool curvatureLeaves(const Constraint &c, const std::function<void(int, double &, double &)> &at, Leave &s1, Leave &s2) const;
    double leaveDot(const Constraint &c) const;   // -1: they run straight on, 1: they fold back

    // The points a constraint ties together.
    std::vector<int> pointsOf(const Constraint &c) const;

    // What a constraint type is made of, in order: 'P' a point, 'L' a line ("PL": a point,
    // then a line). Empty for a type that isn't known.
    static QString kinds(const QString &type);

    // Solve after a new constraint, moving as few points as possible, with `steady` (the
    // thing picked first, as in Fusion) staying put where it can.
    bool solveNear(const std::set<int> &seeds, const std::set<int> &steady);
    // Slides the whole shape `moving` belongs to so that point lands on `to`. False if it isn't free to.
    bool slideTogether(int moving, int to);

    // What a dimension would measure now (mm), with the two places it spans (u, v);
    // for "pointline" the second place is the foot of the square on the line; for "lineline"
    // the places are the second line's middle and the foot of the square from it on the first.
    bool measure(const QString &type, int first, int second, double &value, Point &from, Point &to) const;

    // Would this constraint add nothing to what the others (not driven ones) already hold? Then a
    // dimension like it is driven: its value is a result, not a wish.
    bool redundant(const Constraint &c) const;
    // How many independent ways of moving the points the constraints take away.
    std::size_t jacobianRank() const;

    // The points that can't move at all any more: every way of moving them would break a
    // constraint (Fusion's "fully constrained"). A line is fully constrained when both its
    // ends are.
    std::set<int> fullyConstrained() const;

    bool isFixed(int point) const;

    // Whether one connected piece reaches both the origin or an axis and projected geometry.
    // When the model moves the two would fight (owner's rule, 2026-10-29).
    bool tiedToBoth() const;

    // Whether two lines point the same way (either direction), to within a tenth of a degree.
    bool parallel(int lineA, int lineB) const;

private:
    bool solveWeighted(const std::map<int, double> &freedom);
    bool solveRings(std::set<int> ring, const std::set<int> &steady, bool growFirst);
    std::vector<double> residuals(const std::vector<double> &x, const std::vector<int> &ids) const;
};
