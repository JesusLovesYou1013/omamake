#pragma once

#include <QString>
#include <QStringList>
#include <vector>

// Parameters as a spreadsheet file (owner, 2026-11-02, t95u/t96u): the same columns Fusion 360's
// parameter CSV has, in this order: Name, Unit, Expression, Value, Comments, Favorite. Written as an
// OpenDocument spreadsheet (.ods, the default) or a CSV Fusion can import; read from .ods, .csv or
// .xlsx (Excel). The sheet is called ExportedParameters, as in Fusion's.
namespace omamake::parameterfile {

struct Cells {
    QString name, unit, expr, comment;
    QString value;            // a plain number as text, or empty (worked out again on import, so unused there)
};

// "" when written, else why not. The format comes from the file's ending: .csv, else .ods.
QString write(const QString &path, const std::vector<Cells> &rows);

// "" when read (rows filled), else why not. .csv, .ods or .xlsx by the file's ending.
QString read(const QString &path, std::vector<Cells> &rows);

}
