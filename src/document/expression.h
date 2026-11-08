#pragma once
// Formulas for parameters and dimensions (owner, 2026-11-02, t91u): numbers with units, other
// parameters' names, + - * / ^, brackets, functions. Values are kept in millimetres and radians
// with how many times length and angle go into them, so "a length plus an angle" is caught the
// way Fusion catches it.
#include <QString>
#include <QStringList>
#include <functional>

namespace omamake::expr {

struct Quantity {
    double v = 0;     // base units: mm^len * rad^ang
    int len = 0;
    int ang = 0;
};

// What a bare number means where the context wants a length or an angle (mm per unit, rad per unit).
struct Context {
    double lenScale = 25.4;
    double angScale = 3.14159265358979323846 / 180;
};

// Looks a name up. Returns false with `error` set when it can't give a value.
using Resolver = std::function<bool(const QString &name, Quantity &q, QString &unit, QString &error)>;

struct Result {
    bool ok = false;
    Quantity q;
    QString error;
    int errorAt = -1;        // character position of the problem, when known
    QStringList names;       // every parameter name the text uses, in order of first use
    QString firstUnit;       // the first unit written, or the first used parameter's unit
};

Result evaluate(const QString &text, const Context &ctx, const Resolver &resolve);

// Names that can't be a parameter: units, functions, constants.
bool reserved(const QString &name);
// "" when `name` can be a parameter's name, else the reason.
QString nameProblem(const QString &name);
// Unit words: the canonical key ("in", "ft", "mm", "cm", "m", "deg", "rad"), "" if not a unit.
QString unitKey(const QString &word);
double unitScale(const QString &key);       // mm or rad per one of it
bool isAngleUnit(const QString &key);
// "a length", "an angle", "a number", "an area (length × length)"...
QString describe(const Quantity &q);

} // namespace omamake::expr
