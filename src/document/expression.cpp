#include "expression.h"

#include <QRegularExpression>
#include <cmath>

namespace omamake::expr {

namespace {

constexpr double kPi = 3.14159265358979323846;

struct UnitInfo { const char *word; const char *key; };
const UnitInfo kUnits[] = {
    {"in", "in"}, {"inch", "in"}, {"inches", "in"}, {"\"", "in"}, {"\u2033", "in"},
    {"ft", "ft"}, {"foot", "ft"}, {"feet", "ft"}, {"'", "ft"}, {"\u2032", "ft"},
    {"mm", "mm"}, {"millimeter", "mm"}, {"millimeters", "mm"}, {"millimetre", "mm"}, {"millimetres", "mm"},
    {"cm", "cm"}, {"centimeter", "cm"}, {"centimeters", "cm"}, {"centimetre", "cm"}, {"centimetres", "cm"},
    {"m", "m"}, {"meter", "m"}, {"meters", "m"}, {"metre", "m"}, {"metres", "m"},
    {"deg", "deg"}, {"degs", "deg"}, {"degree", "deg"}, {"degrees", "deg"}, {"\u00b0", "deg"},
    {"rad", "rad"}, {"radian", "rad"}, {"radians", "rad"},
};

const char *kFunctions[] = {"sqrt", "abs", "round", "floor", "ceil", "sign", "min", "max", "sin", "cos", "tan",
                            "asin", "acos", "atan", "atan2", "ln", "log", "exp", "pow"};
const char *kConstants[] = {"pi", "e"};

QString unitWord(const QString &w) { return w.toLower(); }

bool bare(const Quantity &q) { return q.len == 0 && q.ang == 0; }

class Parser {
public:
    Parser(const QString &text, const Context &ctx, const Resolver &resolve) : s(text), ctx(ctx), resolve(resolve) {}

    Result run() {
        Result r;
        skip();
        if (i >= s.size()) {
            r.error = QStringLiteral("Type a number or a formula.");
            return r;
        }
        Quantity q;
        if (parseSum(q) && !failed) {
            skip();
            if (i < s.size()) {
                fail(QStringLiteral("Didn't expect \"%1\" here.").arg(s.mid(i, 1)), i);
                if (s[i] == QLatin1Char(')'))
                    error = QStringLiteral("There's a closing bracket with no opening one.");
            }
        }
        r.names = names;
        r.firstUnit = firstUnit;
        if (failed) {
            r.error = error;
            r.errorAt = errorAt;
            return r;
        }
        if (!std::isfinite(q.v)) {
            r.error = QStringLiteral("That doesn't work out to a number.");
            return r;
        }
        r.ok = true;
        r.q = q;
        return r;
    }

private:
    const QString s;
    const Context ctx;
    const Resolver resolve;
    int i = 0;
    bool failed = false;
    QString error;
    int errorAt = -1;
    QStringList names;
    QString firstUnit;

    bool fail(const QString &why, int at = -1) {
        if (!failed) {
            failed = true;
            error = why;
            errorAt = at;
        }
        return false;
    }
    void skip() {
        while (i < s.size() && s[i].isSpace())
            ++i;
    }
    bool more() { skip(); return i < s.size(); }
    bool at(QChar c) { return more() && s[i] == c; }

    // a bare number met a length or an angle: it takes that one's unit as the context says
    bool match(Quantity &a, Quantity &b, const QString &what) {
        if (a.len == b.len && a.ang == b.ang)
            return true;
        if (bare(a) && (b.len == 1 && b.ang == 0)) { a.v *= ctx.lenScale; a.len = 1; return true; }
        if (bare(a) && (b.ang == 1 && b.len == 0)) { a.v *= ctx.angScale; a.ang = 1; return true; }
        if (bare(b) && (a.len == 1 && a.ang == 0)) { b.v *= ctx.lenScale; b.len = 1; return true; }
        if (bare(b) && (a.ang == 1 && a.len == 0)) { b.v *= ctx.angScale; b.ang = 1; return true; }
        return fail(QStringLiteral("Units don't match: can't %1 %2 and %3.").arg(what, describe(a), describe(b)), i);
    }

    bool parseSum(Quantity &out) {
        if (!parseTerm(out))
            return false;
        while (more() && (s[i] == QLatin1Char('+') || s[i] == QLatin1Char('-'))) {
            const bool plus = s[i] == QLatin1Char('+');
            ++i;
            Quantity rhs;
            if (!parseTerm(rhs))
                return false;
            if (!match(out, rhs, plus ? QStringLiteral("add") : QStringLiteral("subtract")))
                return false;
            out.v = plus ? out.v + rhs.v : out.v - rhs.v;
        }
        return true;
    }
    bool parseTerm(Quantity &out) {
        if (!parseUnary(out))
            return false;
        while (more() && (s[i] == QLatin1Char('*') || s[i] == QLatin1Char('/'))) {
            const bool times = s[i] == QLatin1Char('*');
            ++i;
            Quantity rhs;
            if (!parseUnary(rhs))
                return false;
            if (times) {
                out.v *= rhs.v;
                out.len += rhs.len;
                out.ang += rhs.ang;
            } else {
                if (rhs.v == 0)
                    return fail(QStringLiteral("Can't divide by zero."), i);
                out.v /= rhs.v;
                out.len -= rhs.len;
                out.ang -= rhs.ang;
            }
        }
        return true;
    }
    bool parseUnary(Quantity &out) {
        if (at(QLatin1Char('-'))) {
            ++i;
            if (!parseUnary(out))
                return false;
            out.v = -out.v;
            return true;
        }
        if (at(QLatin1Char('+'))) {
            ++i;
            return parseUnary(out);
        }
        return parsePower(out);
    }
    bool parsePower(Quantity &out) {
        if (!parsePrimary(out))
            return false;
        if (at(QLatin1Char('^'))) {
            ++i;
            Quantity e;
            if (!parseUnary(e))                // right to left: 2^3^2, and 2^-1
                return false;
            if (!bare(e))
                return fail(QStringLiteral("A power has to be a plain number, not %1.").arg(describe(e)), i);
            if (!bare(out)) {
                if (std::fabs(e.v - std::round(e.v)) > 1e-9)
                    return fail(QStringLiteral("A length or angle can only be raised to a whole number."), i);
                out.len *= int(std::lround(e.v));
                out.ang *= int(std::lround(e.v));
            }
            out.v = std::pow(out.v, e.v);
            if (!std::isfinite(out.v))
                return fail(QStringLiteral("That doesn't work out to a number."), i);
        }
        return true;
    }

    bool applyUnit(Quantity &q, const QString &key, int atPos) {
        if (!bare(q))
            return fail(QStringLiteral("A unit goes right after a plain number, not after %1.").arg(describe(q)), atPos);
        if (firstUnit.isEmpty())
            firstUnit = key;
        if (isAngleUnit(key)) {
            q.v *= unitScale(key);
            q.ang = 1;
        } else {
            q.v *= unitScale(key);
            q.len = 1;
        }
        return true;
    }

    // a unit word (or ' " mark) straight after a value, if there is one; consumed when found
    bool takeUnit(QString &key, int &pos) {
        skip();
        pos = i;
        if (i >= s.size())
            return false;
        const QChar c = s[i];
        if (c == QLatin1Char('"') || c == QChar(0x2033) || c == QLatin1Char('\'') || c == QChar(0x2032) || c == QChar(0x00b0)) {
            key = unitKey(QString(c));
            ++i;
            return !key.isEmpty();
        }
        if (c.isLetter()) {
            int j = i;
            while (j < s.size() && (s[j].isLetter()))
                ++j;
            // a letter run glued to digits or underscores is a name, not a unit
            if (j < s.size() && (s[j].isDigit() || s[j] == QLatin1Char('_')))
                return false;
            const QString word = s.mid(i, j - i);
            key = unitKey(word);
            if (key.isEmpty())
                return false;
            i = j;
            return true;
        }
        return false;
    }

    bool number(double &v, bool &integer) {
        const QRegularExpression re(QStringLiteral("^(\\d+\\.?\\d*|\\.\\d+)([eE][+-]?\\d+)?"));
        const auto m = re.match(s.mid(i));
        if (!m.hasMatch())
            return false;
        v = m.captured(0).toDouble();
        integer = !m.captured(0).contains(QLatin1Char('.')) && m.captured(2).isEmpty();
        i += m.capturedLength();
        return true;
    }

    bool parsePrimary(Quantity &out) {
        skip();
        if (i >= s.size())
            return fail(QStringLiteral("The formula stops too soon: something has to come next."), i);
        const QChar c = s[i];
        // a name may start with a digit (3D_Width): a run of letters, digits and _ that is more than a
        // number with a unit after it (10mm, 2e3) and has no decimal point is a name
        bool digitName = false;
        if (c.isDigit()) {
            const QRegularExpression run(QStringLiteral("^[\\p{L}\\p{N}_]+"));
            const QRegularExpression num(QStringLiteral("^(\\d+\\.?\\d*|\\.\\d+)([eE][+-]?\\d+)?"));
            const auto r = run.match(s.mid(i)), m = num.match(s.mid(i));
            if (r.hasMatch() && m.hasMatch() && r.capturedLength() > m.capturedLength() && !m.captured(0).contains(QLatin1Char('.'))
                && unitKey(r.captured(0).mid(m.capturedLength())).isEmpty())
                digitName = true;
        }
        if (!digitName && (c.isDigit() || c == QLatin1Char('.'))) {
            double v = 0;
            bool integer = false;
            const int start = i;
            if (!number(v, integer))
                return fail(QStringLiteral("\"%1\" isn't a number.").arg(s.mid(i, 1)), i);
            // 1 1/2 : a whole number then a fraction
            if (integer) {
                const QRegularExpression frac(QStringLiteral("^\\s+(\\d+)\\s*/\\s*(\\d+)(?![\\d.])"));
                const auto m = frac.match(s.mid(i));
                if (m.hasMatch() && m.captured(2).toDouble() > 0) {
                    v += m.captured(1).toDouble() / m.captured(2).toDouble();
                    i += m.capturedLength();
                }
            }
            out = {v, 0, 0};
            QString key;
            int pos = 0;
            if (takeUnit(key, pos))
                return applyUnit(out, key, pos);
            // 3/4 in : the unit belongs to the whole fraction
            const QRegularExpression slash(QStringLiteral("^\\s*/\\s*(\\d+\\.?\\d*|\\.\\d+)"));
            const auto m = slash.match(s.mid(i));
            if (m.hasMatch()) {
                const int save = i;
                i += m.capturedLength();
                if (takeUnit(key, pos)) {
                    const double d = m.captured(1).toDouble();
                    if (d == 0)
                        return fail(QStringLiteral("Can't divide by zero."), save);
                    out.v = v / d;
                    return applyUnit(out, key, pos);
                }
                i = save;
            }
            Q_UNUSED(start)
            if (takeUnit(key, pos))
                return applyUnit(out, key, pos);
            // a letter run that isn't a unit, right after a number
            skip();
            if (i < s.size() && (s[i].isLetter() || s[i] == QLatin1Char('_'))) {
                int j = i;
                while (j < s.size() && (s[j].isLetterOrNumber() || s[j] == QLatin1Char('_')))
                    ++j;
                return fail(QStringLiteral("\"%1\" isn't a unit I know. To multiply by a name, put * before it.").arg(s.mid(i, j - i)), i);
            }
            return true;
        }
        if (c == QLatin1Char('(')) {
            ++i;
            if (!parseSum(out))
                return false;
            if (!at(QLatin1Char(')')))
                return fail(QStringLiteral("There's an opening bracket with no closing one."), i);
            ++i;
            QString key;
            int pos = 0;
            const int save = i;
            if (takeUnit(key, pos))
                return applyUnit(out, key, pos);
            i = save;
            return true;
        }
        if (digitName || c.isLetter() || c == QLatin1Char('_')) {
            int j = i;
            while (j < s.size() && (s[j].isLetterOrNumber() || s[j] == QLatin1Char('_')))
                ++j;
            const QString word = s.mid(i, j - i);
            const int wordAt = i;
            i = j;
            const QString lower = word.toLower();
            if (at(QLatin1Char('('))) {
                for (const char *f : kFunctions)
                    if (lower == QLatin1String(f))
                        return parseFunction(lower, out, wordAt);
                // a name followed by brackets: say so plainly
                return fail(QStringLiteral("\"%1\" isn't a function I know.").arg(word), wordAt);
            }
            if (word == QLatin1String("pi")) {
                out = {kPi, 0, 0};
                return true;
            }
            if (word == QLatin1String("e")) {
                out = {std::exp(1.0), 0, 0};
                return true;
            }
            if (!unitKey(word).isEmpty())
                return fail(QStringLiteral("\"%1\" is a unit: it goes after a number, like 2 %1.").arg(word), wordAt);
            if (!names.contains(word))
                names << word;
            QString unit, why;
            if (!resolve || !resolve(word, out, unit, why))
                return fail(why.isEmpty() ? QStringLiteral("Don't know \"%1\".").arg(word) : why, wordAt);
            if (firstUnit.isEmpty() && !unit.isEmpty() && unit != QLatin1String("none"))
                firstUnit = unit;
            return true;
        }
        return fail(QStringLiteral("Didn't expect \"%1\" here.").arg(QString(c)), i);
    }

    bool parseFunction(const QString &f, Quantity &out, int wordAt) {
        ++i;                                             // the bracket
        std::vector<Quantity> args;
        if (!at(QLatin1Char(')'))) {
            for (;;) {
                Quantity a;
                if (!parseSum(a))
                    return false;
                args.push_back(a);
                if (at(QLatin1Char(','))) {
                    ++i;
                    continue;
                }
                break;
            }
        }
        if (!at(QLatin1Char(')')))
            return fail(QStringLiteral("There's an opening bracket with no closing one."), i);
        ++i;
        const auto need = [&](size_t n) {
            if (args.size() == n)
                return true;
            return fail(QStringLiteral("%1 needs %2 value%3.").arg(f).arg(n).arg(n == 1 ? "" : "s"), wordAt);
        };
        const auto asAngle = [&](Quantity a, double &rad) {
            if (bare(a)) { rad = a.v * ctx.angScale; return true; }
            if (a.ang == 1 && a.len == 0) { rad = a.v; return true; }
            return fail(QStringLiteral("%1 needs an angle or a plain number, not %2.").arg(f, describe(a)), wordAt);
        };
        const auto plain = [&](const Quantity &a) {
            if (bare(a))
                return true;
            return fail(QStringLiteral("%1 needs a plain number, not %2.").arg(f, describe(a)), wordAt);
        };
        double r = 0;
        if (f == QLatin1String("sqrt")) {
            if (!need(1))
                return false;
            if (args[0].v < 0)
                return fail(QStringLiteral("Can't take the square root of a negative number."), wordAt);
            if (args[0].len % 2 || args[0].ang % 2)
                return fail(QStringLiteral("Can't take a square root of %1.").arg(describe(args[0])), wordAt);
            out = {std::sqrt(args[0].v), args[0].len / 2, args[0].ang / 2};
        } else if (f == QLatin1String("abs") || f == QLatin1String("round") || f == QLatin1String("floor")
                   || f == QLatin1String("ceil")) {
            if (!need(1))
                return false;
            out = args[0];
            out.v = f == "abs" ? std::fabs(out.v) : f == "round" ? std::round(out.v) : f == "floor" ? std::floor(out.v) : std::ceil(out.v);
        } else if (f == QLatin1String("sign")) {
            if (!need(1))
                return false;
            out = {args[0].v > 0 ? 1.0 : args[0].v < 0 ? -1.0 : 0.0, 0, 0};
        } else if (f == QLatin1String("min") || f == QLatin1String("max")) {
            if (args.size() < 2)
                return fail(QStringLiteral("%1 needs at least two values.").arg(f), wordAt);
            out = args[0];
            for (size_t k = 1; k < args.size(); ++k) {
                Quantity b = args[k];
                if (!match(out, b, QStringLiteral("compare")))
                    return false;
                out.v = f == "min" ? std::min(out.v, b.v) : std::max(out.v, b.v);
            }
        } else if (f == QLatin1String("sin") || f == QLatin1String("cos") || f == QLatin1String("tan")) {
            if (!need(1) || !asAngle(args[0], r))
                return false;
            out = {f == "sin" ? std::sin(r) : f == "cos" ? std::cos(r) : std::tan(r), 0, 0};
        } else if (f == QLatin1String("asin") || f == QLatin1String("acos") || f == QLatin1String("atan")) {
            if (!need(1) || !plain(args[0]))
                return false;
            if ((f == "asin" || f == "acos") && std::fabs(args[0].v) > 1)
                return fail(QStringLiteral("%1 needs a number from -1 to 1.").arg(f), wordAt);
            out = {f == "asin" ? std::asin(args[0].v) : f == "acos" ? std::acos(args[0].v) : std::atan(args[0].v), 0, 1};
        } else if (f == QLatin1String("atan2")) {
            if (!need(2))
                return false;
            Quantity y = args[0], x = args[1];
            if (!match(y, x, QStringLiteral("compare")))
                return false;
            out = {std::atan2(y.v, x.v), 0, 1};
        } else if (f == QLatin1String("ln") || f == QLatin1String("log") || f == QLatin1String("exp")) {
            if (!need(1) || !plain(args[0]))
                return false;
            if ((f == "ln" || f == "log") && !(args[0].v > 0))
                return fail(QStringLiteral("%1 needs a number above zero.").arg(f), wordAt);
            out = {f == "ln" ? std::log(args[0].v) : f == "log" ? std::log10(args[0].v) : std::exp(args[0].v), 0, 0};
        } else if (f == QLatin1String("pow")) {
            if (!need(2) || !plain(args[1]))
                return false;
            if (!bare(args[0]) && std::fabs(args[1].v - std::round(args[1].v)) > 1e-9)
                return fail(QStringLiteral("A length or angle can only be raised to a whole number."), wordAt);
            out = {std::pow(args[0].v, args[1].v), args[0].len * int(std::lround(args[1].v)), args[0].ang * int(std::lround(args[1].v))};
        } else {
            return fail(QStringLiteral("\"%1\" isn't a function I know.").arg(f), wordAt);
        }
        if (!std::isfinite(out.v))
            return fail(QStringLiteral("That doesn't work out to a number."), wordAt);
        return true;
    }
};

} // namespace

QString unitKey(const QString &word) {
    const QString w = unitWord(word);
    for (const UnitInfo &u : kUnits)
        if (w == QLatin1String(u.word))
            return QString::fromLatin1(u.key);
    return {};
}

double unitScale(const QString &key) {
    if (key == QLatin1String("in")) return 25.4;
    if (key == QLatin1String("ft")) return 304.8;
    if (key == QLatin1String("mm")) return 1;
    if (key == QLatin1String("cm")) return 10;
    if (key == QLatin1String("m")) return 1000;
    if (key == QLatin1String("deg")) return kPi / 180;
    if (key == QLatin1String("rad")) return 1;
    return 1;
}

bool isAngleUnit(const QString &key) { return key == QLatin1String("deg") || key == QLatin1String("rad"); }

bool reserved(const QString &name) {
    if (!unitKey(name).isEmpty())
        return true;
    const QString l = name.toLower();
    for (const char *f : kFunctions)
        if (l == QLatin1String(f))
            return true;
    for (const char *c : kConstants)
        if (name == QLatin1String(c))
            return true;
    return false;
}

QString nameProblem(const QString &name) {
    if (name.isEmpty())
        return QStringLiteral("A parameter needs a name.");
    // a value typed into the name box by mistake: 10, 2.5in, 3/4 mm, 90deg
    {
        const auto m = QRegularExpression(QStringLiteral("^[\\d.]+(?:\\s*/\\s*[\\d.]+)?\\s*(.*)$")).match(name);
        if (QRegularExpression(QStringLiteral("^(\\d+\\.?\\d*|\\.\\d+)[eE][+-]?\\d+$")).match(name).hasMatch())
            return QStringLiteral("\"%1\" reads as a number, not a name: add a letter or _ to it.").arg(name);
        if (m.hasMatch() && (name.at(0).isDigit() || name.at(0) == QLatin1Char('.'))
            && (m.captured(1).trimmed().isEmpty() || !unitKey(m.captured(1).trimmed()).isEmpty()))
            return QStringLiteral("\"%1\" looks like a value, not a name. Type the value in the Expression box and give the parameter a name here.").arg(name);
    }
    if (name.contains(QRegularExpression(QStringLiteral("\\s"))))
        return QStringLiteral("A name can't have spaces: use letters, digits and _ (like Wall_Width).");
    if (!QRegularExpression(QStringLiteral("^[\\p{L}\\p{N}_]+$")).match(name).hasMatch())
        return QStringLiteral("A name can only use letters, digits and _ .");
    if (reserved(name))
        return QStringLiteral("\"%1\" is already a unit, function or constant: pick another name.").arg(name);
    return {};
}

QString describe(const Quantity &q) {
    if (q.len == 0 && q.ang == 0)
        return QStringLiteral("a number");
    if (q.len == 1 && q.ang == 0)
        return QStringLiteral("a length");
    if (q.len == 0 && q.ang == 1)
        return QStringLiteral("an angle");
    if (q.len == 2 && q.ang == 0)
        return QStringLiteral("an area (length \u00d7 length)");
    if (q.len == 3 && q.ang == 0)
        return QStringLiteral("a volume (length \u00d7 length \u00d7 length)");
    QStringList parts;
    if (q.len)
        parts << QStringLiteral("length^%1").arg(q.len);
    if (q.ang)
        parts << QStringLiteral("angle^%1").arg(q.ang);
    return parts.join(QStringLiteral(" \u00d7 "));
}

Result evaluate(const QString &text, const Context &ctx, const Resolver &resolve) {
    return Parser(text, ctx, resolve).run();
}

} // namespace omamake::expr
