#pragma once

// Esc, once and twice (owner, GB-081): one Esc only steps out of the part of a tool being used (a box being
// typed in, a highlight, a pick waiting for its click); two Esc presses in quick succession, about as fast
// as a double-click, leave the tool (Cancel; in a sketch with no drawing tool on, Finish Sketch). This is the
// one place that says how quick "quick" is: the desktop's double-click time, a fifth longer (Esc is a
// bigger key than a mouse button). Main.qml's Esc and the system file dialogs both count presses here.
//
// The file dialogs (Import / Export STEP, Export DXF, Load, Save As, Parameters) are GTK's, drawn inside
// this process by Qt's gtk3 platform theme, and GTK's own Esc shuts them at once. watchGtkDialogs() puts a
// key watcher on GTK (found at run time, like DialogLook): in a dialog, one Esc only lets go of the text
// highlighted in its box (a search box still stops searching), and two quick ones cancel it. Without GTK
// (another platform theme, or a portal dialog in its own process) this does nothing and the dialog keeps
// its own Esc.

#include <QElapsedTimer>
#include <QObject>

class EscKeys : public QObject {
    Q_OBJECT
    Q_PROPERTY(int interval READ interval CONSTANT)

public:
    explicit EscKeys(QObject *parent = nullptr);

    // How quick two Esc presses must be, in ms.
    static int interval();
    // One Esc press: true when it's the second of two quick ones (the count then starts again, so a third
    // quick press is a first one again).
    Q_INVOKABLE bool press();
    // Forget the last press (a dialog opening, a test starting).
    Q_INVOKABLE void reset();
    // Watches GTK's keys for its dialogs; once is enough, later calls do nothing.
    Q_INVOKABLE void watchGtkDialogs();

    static EscKeys *instance();

    // Test-only (env OMAMAKE_TEST_INPUT): a GTK dialog that's never shown (made, not mapped), with a name box
    // holding "Part.step", "Part" highlighted. "make" makes it; "esc" sends it one Esc press through GTK's
    // own event path (so the watcher sees it as a keyboard's); "state" says {made, text, selected, response}
    // (response: the dialog's answer, -6 cancelled, 0 none yet); "free" throws it away.
    Q_INVOKABLE QString testGtkDialog(const QString &what);

private:
    QElapsedTimer clock;
    qint64 last = -1;
};
