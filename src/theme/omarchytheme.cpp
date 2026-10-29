#include "theme/omarchytheme.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>

OmarchyTheme::OmarchyTheme(QObject *parent) : QObject(parent) {
    // Omarchy installs itself here (with a version file); its theme state lives elsewhere.
    QString data = qEnvironmentVariable("XDG_DATA_HOME");
    if (data.isEmpty())
        data = QDir::homePath() + QStringLiteral("/.local/share");
    m_onOmarchy = QFileInfo::exists(data + QStringLiteral("/omarchy/version"))
               || QFileInfo::exists(QDir::homePath() + QStringLiteral("/.local/share/omarchy/version"));

    connect(&m_watcher, &QFileSystemWatcher::fileChanged, this, &OmarchyTheme::reload);
    connect(&m_watcher, &QFileSystemWatcher::directoryChanged, this, &OmarchyTheme::reload);
    reload();
}

// $XDG_STATE_HOME first, then ~/.local/state (where Omarchy itself writes).
QString OmarchyTheme::currentDir() {
    QStringList roots;
    const QString state = qEnvironmentVariable("XDG_STATE_HOME");
    if (!state.isEmpty())
        roots << state;
    roots << QDir::homePath() + QStringLiteral("/.local/state");
    for (const QString &root : roots) {
        const QString dir = root + QStringLiteral("/omarchy/current");
        if (QFileInfo::exists(dir + QStringLiteral("/theme/colors.toml")))
            return dir;
    }
    return roots.last() + QStringLiteral("/omarchy/current");
}

void OmarchyTheme::reload() {
    const QString dir = currentDir();
    QVariantMap colors;
    QFile file(dir + QStringLiteral("/theme/colors.toml"));
    if (file.open(QIODevice::ReadOnly)) {
        static const QRegularExpression line(QStringLiteral(R"(^\s*(\w+)\s*=\s*"([^"]*)\")"));
        for (const QString &text : QString::fromUtf8(file.readAll()).split(QLatin1Char('\n'))) {
            const auto m = line.match(text);
            if (m.hasMatch())
                colors.insert(m.captured(1), m.captured(2));
        }
    }
    QString name;
    QFile nameFile(dir + QStringLiteral("/theme.name"));
    if (nameFile.open(QIODevice::ReadOnly))
        name = QString::fromUtf8(nameFile.readAll()).trimmed();

    // Watch the file and every folder a theme switch might replace.
    for (const QString &path : {dir, dir + QStringLiteral("/theme"), file.fileName(), nameFile.fileName()}) {
        if (QFileInfo::exists(path) && !m_watcher.files().contains(path) && !m_watcher.directories().contains(path))
            m_watcher.addPath(path);
    }

    if (colors != m_colors || name != m_name) {
        m_colors = colors;
        m_name = name;
        emit changed();
    }
}
