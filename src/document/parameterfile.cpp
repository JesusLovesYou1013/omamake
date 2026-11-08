#include "document/parameterfile.h"

#include <QBuffer>
#include <QFile>
#include <QMap>
#include <QRegularExpression>
#include <QXmlStreamReader>
#include <QXmlStreamWriter>
#include <private/qzipreader_p.h>
#include <private/qzipwriter_p.h>

namespace omamake::parameterfile {

namespace {

const QStringList kHeader = {QStringLiteral("Name"), QStringLiteral("Unit"), QStringLiteral("Expression"),
                             QStringLiteral("Value"), QStringLiteral("Comments"), QStringLiteral("Favorite")};

QString clean(QString s) {
    s.remove(QStringLiteral("_x000D_"));              // Excel's escaped carriage return
    s.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    s.replace(QLatin1Char('\r'), QLatin1Char('\n'));
    return s;
}

// ---- CSV ---------------------------------------------------------------------------------

QString csvField(const QString &s) {
    if (s.contains(QLatin1Char(',')) || s.contains(QLatin1Char('"')) || s.contains(QLatin1Char('\n')) || s.contains(QLatin1Char('\r'))) {
        QString q = s;
        q.replace(QLatin1Char('"'), QStringLiteral("\"\""));
        return QLatin1Char('"') + q + QLatin1Char('"');
    }
    return s;
}

QString writeCsv(const QString &path, const std::vector<Cells> &rows) {
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return QStringLiteral("Can't write %1: %2").arg(path, f.errorString());
    QString text = kHeader.join(QLatin1Char(',')) + QStringLiteral("\r\n");
    for (const Cells &c : rows)
        text += csvField(c.name) + QLatin1Char(',') + csvField(c.unit) + QLatin1Char(',') + csvField(c.expr) + QLatin1Char(',')
              + csvField(c.value) + QLatin1Char(',') + csvField(c.comment) + QStringLiteral(",FALSE\r\n");
    f.write(text.toUtf8());
    return {};
}

// RFC 4180: quoted fields can hold commas, quotes ("") and line breaks.
std::vector<QStringList> parseCsv(const QString &text) {
    std::vector<QStringList> out;
    QStringList row;
    QString field;
    bool quoted = false, any = false;
    for (int i = 0; i < text.size(); ++i) {
        const QChar ch = text[i];
        if (quoted) {
            if (ch == QLatin1Char('"')) {
                if (i + 1 < text.size() && text[i + 1] == QLatin1Char('"')) { field += QLatin1Char('"'); ++i; }
                else quoted = false;
            } else {
                field += ch;
            }
        } else if (ch == QLatin1Char('"')) {
            quoted = true; any = true;
        } else if (ch == QLatin1Char(',')) {
            row << field; field.clear(); any = true;
        } else if (ch == QLatin1Char('\n') || ch == QLatin1Char('\r')) {
            if (ch == QLatin1Char('\r') && i + 1 < text.size() && text[i + 1] == QLatin1Char('\n')) ++i;
            if (any || !field.isEmpty() || !row.isEmpty()) { row << field; out.push_back(row); }
            row.clear(); field.clear(); any = false;
        } else {
            field += ch; any = true;
        }
    }
    if (any || !field.isEmpty() || !row.isEmpty()) { row << field; out.push_back(row); }
    return out;
}

// ---- ODS ---------------------------------------------------------------------------------

QString writeOds(const QString &path, const std::vector<Cells> &rows) {
    const QString office = QStringLiteral("urn:oasis:names:tc:opendocument:xmlns:office:1.0");
    QByteArray content;
    {
        QBuffer buf(&content);
        buf.open(QIODevice::WriteOnly);
        QXmlStreamWriter w(&buf);
        w.setAutoFormatting(false);
        w.writeStartDocument();
        w.writeStartElement(QStringLiteral("office:document-content"));
        w.writeAttribute(QStringLiteral("xmlns:office"), office);
        w.writeAttribute(QStringLiteral("xmlns:style"), QStringLiteral("urn:oasis:names:tc:opendocument:xmlns:style:1.0"));
        w.writeAttribute(QStringLiteral("xmlns:text"), QStringLiteral("urn:oasis:names:tc:opendocument:xmlns:text:1.0"));
        w.writeAttribute(QStringLiteral("xmlns:table"), QStringLiteral("urn:oasis:names:tc:opendocument:xmlns:table:1.0"));
        w.writeAttribute(QStringLiteral("xmlns:fo"), QStringLiteral("urn:oasis:names:tc:opendocument:xmlns:xsl-fo-compatible:1.0"));
        w.writeAttribute(QStringLiteral("office:version"), QStringLiteral("1.2"));

        w.writeStartElement(QStringLiteral("office:automatic-styles"));
        const char *widths[] = {"3.6cm", "2.2cm", "5.2cm", "2.6cm", "8cm", "2.6cm"};
        for (int i = 0; i < 6; ++i) {
            w.writeStartElement(QStringLiteral("style:style"));
            w.writeAttribute(QStringLiteral("style:name"), QStringLiteral("co%1").arg(i + 1));
            w.writeAttribute(QStringLiteral("style:family"), QStringLiteral("table-column"));
            w.writeEmptyElement(QStringLiteral("style:table-column-properties"));
            w.writeAttribute(QStringLiteral("style:column-width"), QString::fromLatin1(widths[i]));
            w.writeEndElement();
        }
        w.writeStartElement(QStringLiteral("style:style"));         // comments: long text wraps onto more lines
        w.writeAttribute(QStringLiteral("style:name"), QStringLiteral("wrap"));
        w.writeAttribute(QStringLiteral("style:family"), QStringLiteral("table-cell"));
        w.writeEmptyElement(QStringLiteral("style:table-cell-properties"));
        w.writeAttribute(QStringLiteral("fo:wrap-option"), QStringLiteral("wrap"));
        w.writeEndElement();
        w.writeEndElement();                                         // automatic-styles

        w.writeStartElement(QStringLiteral("office:body"));
        w.writeStartElement(QStringLiteral("office:spreadsheet"));
        w.writeStartElement(QStringLiteral("table:table"));
        w.writeAttribute(QStringLiteral("table:name"), QStringLiteral("ExportedParameters"));
        for (int i = 0; i < 6; ++i) {
            w.writeEmptyElement(QStringLiteral("table:table-column"));
            w.writeAttribute(QStringLiteral("table:style-name"), QStringLiteral("co%1").arg(i + 1));
        }
        auto text = [&](const QString &s, bool wrap) {
            w.writeStartElement(QStringLiteral("table:table-cell"));
            w.writeAttribute(QStringLiteral("office:value-type"), QStringLiteral("string"));
            if (wrap)
                w.writeAttribute(QStringLiteral("table:style-name"), QStringLiteral("wrap"));
            const QStringList lines = s.split(QLatin1Char('\n'));
            for (const QString &l : lines)
                w.writeTextElement(QStringLiteral("text:p"), l);
            w.writeEndElement();
        };
        w.writeStartElement(QStringLiteral("table:table-row"));
        for (const QString &h : kHeader)
            text(h, false);
        w.writeEndElement();
        for (const Cells &c : rows) {
            w.writeStartElement(QStringLiteral("table:table-row"));
            text(c.name, false);
            text(c.unit, false);
            text(c.expr, false);
            bool isNumber = false;
            const double v = c.value.toDouble(&isNumber);
            if (isNumber) {
                w.writeStartElement(QStringLiteral("table:table-cell"));
                w.writeAttribute(QStringLiteral("office:value-type"), QStringLiteral("float"));
                w.writeAttribute(QStringLiteral("office:value"), QString::number(v, 'g', 12));
                w.writeTextElement(QStringLiteral("text:p"), QString::number(v, 'g', 12));
                w.writeEndElement();
            } else {
                text(QString(), false);
            }
            text(c.comment, true);
            w.writeStartElement(QStringLiteral("table:table-cell"));
            w.writeAttribute(QStringLiteral("office:value-type"), QStringLiteral("boolean"));
            w.writeAttribute(QStringLiteral("office:boolean-value"), QStringLiteral("false"));
            w.writeTextElement(QStringLiteral("text:p"), QStringLiteral("FALSE"));
            w.writeEndElement();
            w.writeEndElement();
        }
        w.writeEndElement(); w.writeEndElement(); w.writeEndElement(); w.writeEndElement();
        w.writeEndDocument();
    }
    const QByteArray manifest =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
        "<manifest:manifest xmlns:manifest=\"urn:oasis:names:tc:opendocument:xmlns:manifest:1.0\" manifest:version=\"1.2\">"
        "<manifest:file-entry manifest:full-path=\"/\" manifest:media-type=\"application/vnd.oasis.opendocument.spreadsheet\"/>"
        "<manifest:file-entry manifest:full-path=\"content.xml\" manifest:media-type=\"text/xml\"/>"
        "</manifest:manifest>";
    QFile out(path);
    if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return QStringLiteral("Can't write %1: %2").arg(path, out.errorString());
    QZipWriter zip(&out);
    // The mimetype comes first and uncompressed, so programs can tell what the file is without opening it.
    zip.setCompressionPolicy(QZipWriter::NeverCompress);
    zip.addFile(QStringLiteral("mimetype"), QByteArray("application/vnd.oasis.opendocument.spreadsheet"));
    zip.setCompressionPolicy(QZipWriter::AutoCompress);
    zip.addFile(QStringLiteral("META-INF/manifest.xml"), manifest);
    zip.addFile(QStringLiteral("content.xml"), content);
    zip.close();
    return out.error() == QFile::NoError ? QString() : QStringLiteral("Can't write %1.").arg(path);
}

// Rows of cells as text, from an ODS table (the first sheet): repeats expanded, paragraphs joined by line breaks.
std::vector<QStringList> readOdsRows(const QByteArray &xml) {
    std::vector<QStringList> out;
    QXmlStreamReader r(xml);
    bool inTable = false, done = false;
    QStringList row;
    while (!r.atEnd() && !done) {
        r.readNext();
        if (r.isStartElement() && r.qualifiedName() == QLatin1String("table:table") && !inTable) {
            inTable = true;
        } else if (inTable && r.isStartElement() && r.qualifiedName() == QLatin1String("table:table-row")) {
            row.clear();
            const int repeat = qMax(1, r.attributes().value(QStringLiteral("table:number-rows-repeated")).toInt());
            bool blankRow = true;
            while (!r.atEnd()) {
                r.readNext();
                if (r.isEndElement() && r.qualifiedName() == QLatin1String("table:table-row"))
                    break;
                if (r.isStartElement() && (r.qualifiedName() == QLatin1String("table:table-cell")
                                           || r.qualifiedName() == QLatin1String("table:covered-table-cell"))) {
                    const auto a = r.attributes();
                    const int cols = qMin(64, qMax(1, a.value(QStringLiteral("table:number-columns-repeated")).toInt()));
                    const QString type = a.value(QStringLiteral("office:value-type")).toString();
                    QString value = a.value(QStringLiteral("office:value")).toString();
                    QString bool_ = a.value(QStringLiteral("office:boolean-value")).toString();
                    QStringList paragraphs;
                    QString para;
                    bool inPara = false;
                    while (!r.atEnd()) {
                        r.readNext();
                        if (r.isEndElement() && (r.qualifiedName() == QLatin1String("table:table-cell")
                                                 || r.qualifiedName() == QLatin1String("table:covered-table-cell")))
                            break;
                        if (r.isStartElement() && r.qualifiedName() == QLatin1String("text:p")) { inPara = true; para.clear(); }
                        else if (r.isStartElement() && r.qualifiedName() == QLatin1String("text:s"))
                            para += QString(qMax(1, r.attributes().value(QStringLiteral("text:c")).toInt()), QLatin1Char(' '));
                        else if (r.isStartElement() && r.qualifiedName() == QLatin1String("text:tab")) para += QLatin1Char('\t');
                        else if (r.isStartElement() && r.qualifiedName() == QLatin1String("text:line-break")) para += QLatin1Char('\n');
                        else if (r.isCharacters() && inPara) para += r.text();
                        else if (r.isEndElement() && r.qualifiedName() == QLatin1String("text:p")) { paragraphs << para; inPara = false; }
                    }
                    QString text = paragraphs.join(QLatin1Char('\n'));
                    if (type == QLatin1String("float") || type == QLatin1String("percentage") || type == QLatin1String("currency"))
                        text = value;
                    else if (type == QLatin1String("boolean"))
                        text = bool_ == QLatin1String("true") ? QStringLiteral("TRUE") : QStringLiteral("FALSE");
                    if (!text.isEmpty()) blankRow = false;
                    for (int k = 0; k < cols; ++k) row << text;
                }
            }
            if (!blankRow)
                for (int k = 0; k < qMin(repeat, 1000); ++k) out.push_back(row);
        } else if (inTable && r.isEndElement() && r.qualifiedName() == QLatin1String("table:table")) {
            done = true;
        }
    }
    return out;
}

// ---- XLSX (Excel) ------------------------------------------------------------------------

int columnOf(const QString &ref) {
    int c = 0;
    for (const QChar ch : ref) {
        if (ch.isLetter()) c = c * 26 + (ch.toUpper().unicode() - 'A' + 1);
        else break;
    }
    return c - 1;
}

std::vector<QStringList> readXlsxRows(QZipReader &zip) {
    QStringList shared;
    {
        QXmlStreamReader r(zip.fileData(QStringLiteral("xl/sharedStrings.xml")));
        QString cur;
        bool inSi = false, inT = false;
        while (!r.atEnd()) {
            r.readNext();
            if (r.isStartElement() && r.name() == QLatin1String("si")) { inSi = true; cur.clear(); }
            else if (r.isStartElement() && r.name() == QLatin1String("t") && inSi) inT = true;
            else if (r.isCharacters() && inT) cur += r.text();
            else if (r.isEndElement() && r.name() == QLatin1String("t")) inT = false;
            else if (r.isEndElement() && r.name() == QLatin1String("si")) { shared << cur; inSi = false; }
        }
    }
    QString sheetPath = QStringLiteral("xl/worksheets/sheet1.xml");
    if (zip.fileData(sheetPath).isEmpty()) {
        for (const auto &info : zip.fileInfoList())
            if (info.filePath.startsWith(QLatin1String("xl/worksheets/")) && info.filePath.endsWith(QLatin1String(".xml"))) {
                sheetPath = info.filePath;
                break;
            }
    }
    std::vector<QStringList> out;
    QXmlStreamReader r(zip.fileData(sheetPath));
    QStringList row;
    while (!r.atEnd()) {
        r.readNext();
        if (r.isStartElement() && r.name() == QLatin1String("row")) {
            row.clear();
        } else if (r.isStartElement() && r.name() == QLatin1String("c")) {
            const int col = columnOf(r.attributes().value(QStringLiteral("r")).toString());
            const QString type = r.attributes().value(QStringLiteral("t")).toString();
            QString v, inlineText;
            while (!r.atEnd()) {
                r.readNext();
                if (r.isEndElement() && r.name() == QLatin1String("c"))
                    break;
                if (r.isStartElement() && r.name() == QLatin1String("v")) v = r.readElementText();
                else if (r.isStartElement() && r.name() == QLatin1String("t")) inlineText += r.readElementText();
            }
            QString text = v;
            if (type == QLatin1String("s")) text = shared.value(v.toInt());
            else if (type == QLatin1String("inlineStr")) text = inlineText;
            else if (type == QLatin1String("b")) text = v == QLatin1String("1") ? QStringLiteral("TRUE") : QStringLiteral("FALSE");
            while (row.size() <= col) row << QString();
            if (col >= 0) row[col] = text;
        } else if (r.isEndElement() && r.name() == QLatin1String("row")) {
            bool blank = true;
            for (const QString &c : row) if (!c.isEmpty()) blank = false;
            if (!blank) out.push_back(row);
        }
    }
    return out;
}

}   // namespace

QString write(const QString &path, const std::vector<Cells> &rows) {
    if (path.endsWith(QLatin1String(".csv"), Qt::CaseInsensitive))
        return writeCsv(path, rows);
    return writeOds(path, rows);
}

QString read(const QString &path, std::vector<Cells> &rows) {
    rows.clear();
    QFile f(path);
    if (!f.exists())
        return QStringLiteral("There's no file at %1.").arg(path);
    std::vector<QStringList> table;
    // The contents decide the format, not the ending: a spreadsheet saved or renamed as
    // ".csv" (owner's Fusion-style file was really an Excel file) must still be read right.
    if (!f.open(QIODevice::ReadOnly))
        return QStringLiteral("Can't open %1: %2").arg(path, f.errorString());
    const QByteArray raw = f.readAll();
    f.close();
    if (raw.isEmpty())
        return QStringLiteral("The file is empty.");
    if (raw.startsWith("PK\x03\x04")) {
        QZipReader zip(path);
        if (!zip.isReadable())
            return QStringLiteral("That isn't a spreadsheet file I can open.");
        const QByteArray xml = zip.fileData(QStringLiteral("content.xml"));
        if (!xml.isEmpty()) {
            table = readOdsRows(xml);
        } else if (!zip.fileData(QStringLiteral("xl/worksheets/sheet1.xml")).isEmpty()) {
            table = readXlsxRows(zip);
        } else {
            return QStringLiteral("That is a zip file, but not an .ods or .xlsx spreadsheet.");
        }
    } else {
        if (raw.contains('\0') || raw.startsWith("\xD0\xCF\x11\xE0"))
            return QStringLiteral("That isn't a spreadsheet I can read. Use .ods, .csv or .xlsx.");
        QString text = QString::fromUtf8(raw);
        if (text.contains(QChar::ReplacementCharacter))
            text = QString::fromLatin1(raw);  // an older CSV that isn't UTF-8
        if (text.startsWith(QChar(0xFEFF)))
            text.remove(0, 1);
        table = parseCsv(text);
    }
    if (table.empty())
        return QStringLiteral("The file has no rows.");
    // Columns by their header names (any order); without a header, Fusion's order.
    int cName = 0, cUnit = 1, cExpr = 2, cComment = 4;
    size_t first = 0;
    const QStringList &head = table[0];
    auto find = [&](const char *name) {
        for (int i = 0; i < head.size(); ++i)
            if (head[i].trimmed().compare(QLatin1String(name), Qt::CaseInsensitive) == 0)
                return i;
        return -1;
    };
    if (find("Name") >= 0 && find("Expression") >= 0) {
        cName = find("Name"); cExpr = find("Expression");
        cUnit = find("Unit"); cComment = find("Comments");
        first = 1;
    }
    for (size_t i = first; i < table.size(); ++i) {
        const QStringList &r = table[i];
        auto at = [&](int c) { return c >= 0 && c < r.size() ? clean(r[c]) : QString(); };
        Cells c;
        c.name = at(cName).trimmed();
        c.unit = at(cUnit).trimmed();
        c.expr = at(cExpr).trimmed();
        c.comment = at(cComment);
        if (c.name.isEmpty() && c.expr.isEmpty())
            continue;
        rows.push_back(c);
    }
    if (rows.empty())
        return QStringLiteral("The file has no parameters in it.");
    return {};
}

}
