#include "document/document.h"
#include "engine/engine.h"
#include "theme/omarchytheme.h"
#include "theme/transparency.h"
#include "view/meshgeometry.h"

#include <QDir>
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickWindow>
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

    qmlRegisterType<MeshGeometry>("OmaMake", 1, 0, "MeshGeometry");

    Transparency transparency;
    OmarchyTheme omarchyTheme;
    Document document(omamake::makeLiteEngine());
    document.addCube(12 * kInch);

    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("doc"), &document);
    engine.rootContext()->setContextProperty(QStringLiteral("transparency"), &transparency);
    engine.rootContext()->setContextProperty(QStringLiteral("omarchyTheme"), &omarchyTheme);
    engine.load(QUrl::fromLocalFile(QDir(qmlDir()).filePath(QStringLiteral("Main.qml"))));
    if (engine.rootObjects().isEmpty())
        return 1;
    return app.exec();
}
