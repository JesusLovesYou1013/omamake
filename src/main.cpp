#include "document/document.h"
#include "engine/engine.h"
#include "theme/dialoglook.h"
#include "theme/omarchytheme.h"
#include "theme/transparency.h"
#include "view/meshgeometry.h"
#include "view/modifierkeys.h"

#include <QDir>
#include <QGuiApplication>
#include <QIcon>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QRegularExpression>
#include <QQuickWindow>
#include <QStandardPaths>
#include <QSurfaceFormat>
#include <QUrl>

namespace {

constexpr double kInch = 25.4;

// The interface is plain QML files read at startup, so UI changes need no compile:
// edit, relaunch. OMAMAKE_QML_DIR points elsewhere (an installed copy, a test tree).
QString qmlDir() {
    const QString fromEnv = qEnvironmentVariable("OMAMAKE_QML_DIR");
    return fromEnv.isEmpty() ? QStringLiteral(OMAMAKE_SOURCE_QML_DIR) : fromEnv;
}

} // namespace

int main(int argc, char *argv[]) {
    QQuickWindow::setGraphicsApi(QSGRendererInterface::OpenGL);
    QSurfaceFormat format = QSurfaceFormat::defaultFormat();
    format.setSamples(4);
    QSurfaceFormat::setDefaultFormat(format);
    // The 3D space's background can be see-through (OmaSkins), so windows get an alpha channel.
    QQuickWindow::setDefaultAlphaBuffer(true);

    QGuiApplication app(argc, argv);
    QGuiApplication::setApplicationName(QStringLiteral("OmaMake"));
    QGuiApplication::setDesktopFileName(QStringLiteral("omamake"));
    // The app's icon (owner, GB-064). On Wayland the bar and launcher take it from omamake.desktop (Icon=omamake).
    QGuiApplication::setWindowIcon(QIcon(QStringLiteral(":/icons/omamake.png")));

    qmlRegisterType<MeshGeometry>("OmaMake", 1, 0, "MeshGeometry");

    Transparency transparency;
    ModifierKeys modifierKeys;
    OmarchyTheme omarchyTheme;
    DialogLook dialogLook;
    // The engine (owner, 2026-10-30): Heavy (OpenCascade, exact surfaces) unless OMAMAKE_ENGINE=lite.
    Document document(qEnvironmentVariable("OMAMAKE_ENGINE") == QLatin1String("lite")
                          ? omamake::makeLiteEngine() : omamake::makeHeavyEngine());
    // A new design starts empty: sketches start on the origin planes (owner, 2026-10-29).
    // OMAMAKE_TEST_CUBE=1 places the old 12 in cube, for testing faces until bodies can be made.
    if (qEnvironmentVariableIntValue("OMAMAKE_TEST_CUBE"))
        document.addCube(12 * kInch);

    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("doc"), &document);
    engine.rootContext()->setContextProperty(QStringLiteral("transparency"), &transparency);
    engine.rootContext()->setContextProperty(QStringLiteral("modifierKeys"), &modifierKeys);
    engine.rootContext()->setContextProperty(QStringLiteral("omarchyTheme"), &omarchyTheme);
    engine.rootContext()->setContextProperty(QStringLiteral("dialogLook"), &dialogLook);
    // Where Save As starts: Documents/OmaMake, or OMAMAKE_SAVE_DIR (test windows run with a
    // made-up home, and their saves should still land in the real one).
    QString saveDir = qEnvironmentVariable("OMAMAKE_SAVE_DIR");
    if (saveDir.isEmpty())
        saveDir = QDir(QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation)).filePath(QStringLiteral("OmaMake"));
    QDir().mkpath(saveDir);
    engine.rootContext()->setContextProperty(QStringLiteral("saveFolder"), QUrl::fromLocalFile(saveDir));
    // A design named on the command line (Load while a window holds work, or a file opened
    // from the file manager once installed) opens straight away; what went wrong is said at start.
    QString openNote;
    const QStringList args = app.arguments();
    const bool stepFile = args.size() > 1 && QRegularExpression(QStringLiteral("\\.(ste?p|stl|obj)$"), QRegularExpression::CaseInsensitiveOption).match(args.at(1)).hasMatch();
    if (args.size() > 1 && !stepFile) {
        openNote = document.openFrom(args.at(1));
        if (!openNote.isEmpty())
            qWarning().noquote() << "open:" << openNote;
    }
    engine.rootContext()->setContextProperty(QStringLiteral("openNote"), openNote);
    engine.load(QUrl::fromLocalFile(QDir(qmlDir()).filePath(QStringLiteral("Main.qml"))));
    if (engine.rootObjects().isEmpty())
        return 1;
    if (stepFile)
        document.importFile(args.at(1));
    return app.exec();
}
