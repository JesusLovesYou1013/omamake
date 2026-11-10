#pragma once

#include <QObject>
#include <QTimer>

// Whether Ctrl or Shift is held, anywhere in the app (owner, 2026-11-09: Chamfer shows what's picked only
// while one is held, as Fusion does). Kept from the Ctrl and Shift keys going down and up (see eventFilter
// for why no event's own modifiers may let go of one), and let go when the window or the app loses focus, or
// when the window system itself says they're up (a release that never came, GB-050).
class ModifierKeys : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool ctrlOrShift READ ctrlOrShift NOTIFY changed)
public:
    explicit ModifierKeys(QObject *parent = nullptr);
    bool ctrlOrShift() const { return m_down & (Qt::ControlModifier | Qt::ShiftModifier); }

signals:
    void changed();

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void set(Qt::KeyboardModifier which, bool down);
    void add(Qt::KeyboardModifiers modifiers);
    void check(const char *why);
    Qt::KeyboardModifiers platform() const;
    Qt::KeyboardModifiers m_down;
    QTimer m_poll;
    int m_upFor = 0;           // polls in a row the window system has said a held one is up
};
