#include "theme/transparency.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>

Transparency::Transparency(QObject *parent) : QObject(parent) {
    // OmaSkins rewrites the file (or creates its folder) on every change, so watch both.
    connect(&m_watcher, &QFileSystemWatcher::fileChanged, this, &Transparency::reload);
    connect(&m_watcher, &QFileSystemWatcher::directoryChanged, this, &Transparency::reload);
    reload();
}

QString Transparency::path() {
    QString config = qEnvironmentVariable("XDG_CONFIG_HOME");
    if (config.isEmpty())
        config = QDir::homePath() + QStringLiteral("/.config");
    return config + QStringLiteral("/omaskins/transparency");
}

void Transparency::reload() {
    qreal focused = 1.0, unfocused = 1.0;
    QFile file(path());
    if (file.open(QIODevice::ReadOnly)) {
        const QStringList parts = QString::fromUtf8(file.readAll()).simplified().split(QLatin1Char(' '));
        bool okFocused = false, okUnfocused = false;
        const qreal f = parts.value(0).toDouble(&okFocused);
        const qreal u = parts.value(1).toDouble(&okUnfocused);
        if (okFocused && okUnfocused) {
            focused = qBound(0.0, f, 1.0);
            unfocused = qBound(0.0, u, 1.0);
        }
    }

    const QString dir = QFileInfo(path()).absolutePath();
    if (QFileInfo::exists(dir) && !m_watcher.directories().contains(dir))
        m_watcher.addPath(dir);
    if (QFileInfo::exists(path()) && !m_watcher.files().contains(path()))
        m_watcher.addPath(path());

    if (focused != m_focused || unfocused != m_unfocused) {
        m_focused = focused;
        m_unfocused = unfocused;
        emit changed();
    }
    markOwnTransparency();
}

// While the background is drawn see-through, the compositor must not fade the whole
// window too (the models would fade with it). OmaSkins keeps its fade off any app
// that leaves this marker, named after the process, in the runtime directory.
void Transparency::markOwnTransparency() const {
    if (!QFileInfo::exists(path()) || (m_focused >= 0.999 && m_unfocused >= 0.999))
        return;
    const QString runtime = qEnvironmentVariable("XDG_RUNTIME_DIR");
    if (runtime.isEmpty())
        return;
    const QString dir = runtime + QStringLiteral("/omaskins-qt");
    if (!QDir().mkpath(dir))
        return;
    QFile mark(dir + QLatin1Char('/') + QString::number(QCoreApplication::applicationPid()));
    if (!mark.open(QIODevice::WriteOnly))
        qWarning("Could not mark the window as drawing its own transparency");
}
