#include "modifierkeys.h"

#include <QCoreApplication>
#include <QDebug>
#include <QGuiApplication>
#include <QInputEvent>
#include <QKeyEvent>

ModifierKeys::ModifierKeys(QObject *parent) : QObject(parent) {
    QCoreApplication::instance()->installEventFilter(this);
    // While one is held, the window system is asked now and then whether it still is (owner, 2026-11-09,
    // GB-050: "there are times I have to hit esc or something else" to get the preview back: held had stuck,
    // and only another key, carrying the real state, put it right). Two answers in a row, so one asked in the
    // instant between a key and the window system's own update can't let go of a key still down.
    m_poll.setInterval(80);
    connect(&m_poll, &QTimer::timeout, this, [this] { check("poll"); });
}

// What the window system says is down now (QGuiApplication::queryKeyboardModifiers: on Wayland the
// compositor's own modifier state, on X the server's), not the last event's. Under OMAMAKE_TEST_INPUT the
// test keys' (Document::testKey keeps them on the app), since the real keyboard's say nothing of those.
Qt::KeyboardModifiers ModifierKeys::platform() const {
    static const bool test = qEnvironmentVariableIsSet("OMAMAKE_TEST_INPUT");
    if (test)
        return Qt::KeyboardModifiers(QCoreApplication::instance()->property("omamakeTestModifiers").toInt());
    return QGuiApplication::queryKeyboardModifiers();
}

void ModifierKeys::check(const char *why) {
    const Qt::KeyboardModifiers real = platform();
    const Qt::KeyboardModifiers up = m_down & ~real & (Qt::ControlModifier | Qt::ShiftModifier);
    if (!up) {
        m_upFor = 0;
        return;
    }
    if (++m_upFor < 2)
        return;
    m_upFor = 0;
    qInfo().noquote() << "modifierKeys: let go by the window system's state (" << why << "): its release never came";
    if (up & Qt::ControlModifier)
        set(Qt::ControlModifier, false);
    if (up & Qt::ShiftModifier)
        set(Qt::ShiftModifier, false);
}

// Held is what the Ctrl and Shift keys themselves said, going down and up (owner, 2026-11-09: GB-047 flashed
// back and forth while the key was held). Other events' modifiers can't take it away: Qt Quick's own hover
// events (sent as the scene under a still pointer changes) carry QGuiApplication::keyboardModifiers(), which on
// Linux is the last key event's modifiers WITHOUT the key itself, so Ctrl just pressed reads as no modifier.
// Any other key from the window system does carry the real state (the quirk is only a modifier key's own
// event), so it puts both right; a real pointer event may only add one (the window got focus with it already
// down). Leaving the window or the app lets go of both, so a release that went elsewhere can't stick.
bool ModifierKeys::eventFilter(QObject *watched, QEvent *event) {
    switch (event->type()) {
    case QEvent::KeyPress:
    case QEvent::KeyRelease: {
        const auto *key = static_cast<QKeyEvent *>(event);
        if (key->isAutoRepeat())
            break;
        const bool down = event->type() == QEvent::KeyPress;
        if (key->key() == Qt::Key_Control)
            set(Qt::ControlModifier, down);
        else if (key->key() == Qt::Key_Shift)
            set(Qt::ShiftModifier, down);
        else if (event->spontaneous()) {
            set(Qt::ControlModifier, key->modifiers() & Qt::ControlModifier);
            set(Qt::ShiftModifier, key->modifiers() & Qt::ShiftModifier);
        }
        break;
    }
    case QEvent::MouseMove:
    case QEvent::MouseButtonPress:
    case QEvent::MouseButtonRelease:
    case QEvent::Wheel:
        if (event->spontaneous())
            add(static_cast<QInputEvent *>(event)->modifiers());
        break;
    case QEvent::WindowDeactivate:
    case QEvent::ApplicationDeactivate:
        set(Qt::ControlModifier, false);
        set(Qt::ShiftModifier, false);
        break;
    default:
        break;
    }
    return QObject::eventFilter(watched, event);
}

void ModifierKeys::add(Qt::KeyboardModifiers modifiers) {
    if (modifiers & Qt::ControlModifier)
        set(Qt::ControlModifier, true);
    if (modifiers & Qt::ShiftModifier)
        set(Qt::ShiftModifier, true);
}

void ModifierKeys::set(Qt::KeyboardModifier which, bool down) {
    const bool was = ctrlOrShift();
    m_down.setFlag(which, down);
    if (ctrlOrShift() != was) {
        m_upFor = 0;
        if (ctrlOrShift())
            m_poll.start();
        else
            m_poll.stop();
        emit changed();
    }
}
