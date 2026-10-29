#pragma once

// Follows OmaSkins' transparency step for the 3D space's background, the way
// OmaShow does. Models are never made see-through by this.
//
// ~/.config/omaskins/transparency holds "<focused> <unfocused> [ignored…]",
// e.g. "0.75 0.68 blur". No file → fully opaque.

#include <QFileSystemWatcher>
#include <QObject>

class Transparency : public QObject {
    Q_OBJECT
    Q_PROPERTY(qreal focused READ focused NOTIFY changed)
    Q_PROPERTY(qreal unfocused READ unfocused NOTIFY changed)

public:
    explicit Transparency(QObject *parent = nullptr);

    qreal focused() const { return m_focused; }
    qreal unfocused() const { return m_unfocused; }

signals:
    void changed();

private:
    static QString path();
    void reload();
    void markOwnTransparency() const;

    QFileSystemWatcher m_watcher;
    qreal m_focused = 1.0;
    qreal m_unfocused = 1.0;
};
