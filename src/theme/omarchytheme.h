#pragma once

// Reads the colours of the current Omarchy theme, for "Follow Omarchy theme".
//
// Omarchy writes the active theme to ~/.local/state/omarchy/current/theme/colors.toml:
// lines like  accent = "#3CBF5C",  background = "#080C09",  mode = "dark".
// Theme switches replace that folder, so the parent folders are watched too.
// No file → available is false and the app keeps its own palette.

#include <QFileSystemWatcher>
#include <QObject>
#include <QVariantMap>

class OmarchyTheme : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool available READ available NOTIFY changed)
    Q_PROPERTY(QString name READ name NOTIFY changed)
    Q_PROPERTY(QVariantMap colors READ colors NOTIFY changed)

public:
    explicit OmarchyTheme(QObject *parent = nullptr);

    bool available() const { return !m_colors.isEmpty(); }
    QString name() const { return m_name; }
    QVariantMap colors() const { return m_colors; }

signals:
    void changed();

private:
    static QString currentDir();
    void reload();

    QFileSystemWatcher m_watcher;
    QString m_name;
    QVariantMap m_colors;
};
