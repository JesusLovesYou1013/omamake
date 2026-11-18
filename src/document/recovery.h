#pragma once
// GB-089 (owner, 2026-11-16): the memory guard and recovery.
//
// The memory guard: while heavy work runs (omamake::HeavyScope, src/engine/stop.h), a thread of its own reads the
// system's MemAvailable (/proc/meminfo) every 200 ms. Under 15 % of MemTotal it stops every job (omamake::
// requestMemoryStop: OpenCascade's Booleans break), writes a small "memory stop" note beside the recovery file, and
// tells the window, which shows "System memory running low, closing to protect system" and closes OmaMake itself
// before the system's out-of-memory killer has to (it once took the whole desktop session with it).
//
// The recovery file: the design as an .omamake file holds it (Document::toJson: solids, imports, history), written
// whenever the history changes (a second after, off the window thread, never while the engine is busy), so nothing
// big has to be written when memory is short. In $XDG_STATE_HOME/omamake/recovery (~/.local/state/omamake/recovery),
// one per window: window-<pid>.omamake and window-<pid>.json (what it is). A window that closes normally removes its
// own. At the next start a memory stop's file (or one left by a window that ended without closing, e.g. killed) is
// offered: Recover opens it, the Timeline's marker 2 items back (the owner: "saving progress 2 history items prior to
// this issue"), the 2 newer items behind the marker; No sets it aside (recovery/set-aside, the newest 5 kept).
#include <QObject>
#include <QVariantMap>
#include <QString>

#include <atomic>
#include <map>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>

class Document;
class QTimer;
class QSocketNotifier;

class Recovery : public QObject {
    Q_OBJECT
    // A recovery waiting to be asked about at start ({} when none): reason ("memory" or "ended"), name, filePath
    // ("" never saved), when (local time text), steps, rewindTo, snapshot (its path).
    Q_PROPERTY(QVariantMap pending READ pending NOTIFY pendingChanged)
public:
    explicit Recovery(Document *doc, QObject *parent = nullptr);
    ~Recovery() override;

    QVariantMap pending() const { return m_pending; }
    // Yes: the pending design opened in this window. "" when opened (the window then moves the marker back), or why not.
    Q_INVOKABLE QVariantMap recover();
    // No: set aside (recovery/set-aside), not offered again.
    Q_INVOKABLE void decline();
    // The memory popup has been shown: OmaMake closes now.
    Q_INVOKABLE void closeNow();
    // The window has the memory stop (it shows the popup); the guard's own fallback waits for closeNow.
    Q_INVOKABLE void acknowledge() { m_acked = true; }

    // Tests (OMAMAKE_TEST_INPUT only): pretend MemAvailable is this many MB (-1: the real one); write the recovery
    // file now; what the recovery has done ({dir, writes, lastMs, lastBytes, steps, rewindTo, file}).
    Q_INVOKABLE void testSqueeze(int availMb);
    Q_INVOKABLE void testSnapshotNow();
    Q_INVOKABLE QVariantMap testState() const;

    static QString folder();

signals:
    void memoryLow(const QString &message, const QString &detail);
    void pendingChanged();

private:
    void watch();
    void stopNow(long availKb, long totalKb);
    void snapshotSoon();
    void snapshot();
    void writeDone(qint64 readMs, qint64 ms, qint64 bytes, int steps, int rewindTo, const QString &filePath, const QString &name);
    void findPending();
    void removeOwn();
    void setAside(const QString &stem, const QString &into);

    Document *m_doc;
    QString m_dir, m_stem;          // the folder, and this window's "window-<pid>"
    QTimer *m_soon = nullptr;
    bool m_writing = false, m_dirty = false;
    std::thread m_writer;
    int m_writes = 0;
    qint64 m_lastMs = 0, m_lastBytes = 0, m_lastReadMs = 0;
    int m_snapSteps = -1, m_snapRewind = -1;
    std::map<int, int> m_rowsAt;    // history length -> Timeline items then, for the states written
    QVariantMap m_pending;

    // The guard's thread and what it may touch.
    std::thread m_watcher;
    std::mutex m_wake;
    std::condition_variable m_wakeUp;
    bool m_quit = false;
    std::atomic_bool m_stopped{false}, m_acked{false};
    std::atomic_long m_fakeAvailKb{-1};
    double m_percent = 15;
    // The memory stop's note, made ready beforehand (no work to do when memory is short): the path, and what the last
    // recovery file is (filled in by writeDone, under m_noteLock).
    char m_notePath[1024] = {0};
    std::mutex m_noteLock;
    std::string m_noteBody;
    std::atomic_int m_stepsNow{0};

    // SIGTERM closes the window as its close button does (its recovery file removed, not offered as left behind).
    QSocketNotifier *m_termNote = nullptr;
};
