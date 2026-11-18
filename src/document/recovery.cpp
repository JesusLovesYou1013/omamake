#include "document/recovery.h"
#include "document/document.h"
#include "engine/stop.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QSocketNotifier>
#include <QTimer>

#include <algorithm>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <spawn.h>
#include <sys/socket.h>
#include <unistd.h>

extern char **environ;

namespace {

constexpr int kPollMs = 200;          // how often the guard reads the system's free memory
constexpr double kStopPercent = 15;   // the owner's limit: MemAvailable under 15 % of MemTotal (about 1.2 GB of 8 GB)
constexpr double kOwnPercent = 20;    // ...while OmaMake itself holds at least 20 % (in memory and swapped out)
constexpr double kHardPercent = 5;    // after the stop: under this, close at once (no popup wait)
constexpr int kFallbackMs = 1500;     // the window hasn't shown the popup by then (its thread busy): the guard closes it
constexpr int kCloseMs = 6000;        // the popup's been up this long and nothing closed it: the guard closes it
constexpr int kKeep = 5;              // set-aside and recovered files kept, newest first
constexpr const char *kMessage = "System memory running low, closing to protect system";

int g_termPipe[2] = {-1, -1};

void onTerm(int) {
    const char one = 1;
    if (g_termPipe[1] >= 0)
        (void)!::write(g_termPipe[1], &one, 1);
}

// A "Key:   1234 kB" line's number, from text read into a buffer (no allocation: this runs when memory is short).
long kbOf(const char *text, const char *key) {
    const char *at = std::strstr(text, key);
    return at ? std::strtol(at + std::strlen(key), nullptr, 10) : -1;
}

bool readSmall(const char *path, char *buffer, std::size_t size) {
    const int fd = ::open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return false;
    const ssize_t got = ::read(fd, buffer, size - 1);
    ::close(fd);
    if (got <= 0)
        return false;
    buffer[got] = 0;
    return true;
}

void say(const char *text) { (void)!::write(2, text, std::strlen(text)); }

QString escaped(const QString &text) {   // as a JSON string's inside
    QByteArray j = QJsonDocument(QJsonObject{{QStringLiteral("v"), text}}).toJson(QJsonDocument::Compact);
    j.chop(2);                            // "}" and the closing quote
    return QString::fromUtf8(j.mid(6));   // {"v":"
}

bool alive(int pid) {
    if (pid <= 0 || pid == int(::getpid()))
        return pid == int(::getpid());
    char path[64], comm[64];
    std::snprintf(path, sizeof path, "/proc/%d/comm", pid);
    return readSmall(path, comm, sizeof comm) && std::strncmp(comm, "omamake", 7) == 0;
}

} // namespace

QString Recovery::folder() {
    QString state = qEnvironmentVariable("XDG_STATE_HOME");
    if (state.isEmpty())
        state = QDir::home().filePath(QStringLiteral(".local/state"));
    return QDir(state).filePath(QStringLiteral("omamake/recovery"));
}

Recovery::Recovery(Document *doc, QObject *parent) : QObject(parent), m_doc(doc) {
    m_dir = folder();
    QDir().mkpath(m_dir);
    m_stem = QStringLiteral("window-%1").arg(::getpid());
    bool ok = false;
    const double percent = qEnvironmentVariable("OMAMAKE_MEM_STOP_PERCENT").toDouble(&ok);
    if (ok && percent > 0 && percent < 100)
        m_percent = percent;
    std::snprintf(m_notePath, sizeof m_notePath, "%s", QDir(m_dir).filePath(QStringLiteral("memory-stop-%1.json").arg(::getpid())).toLocal8Bit().constData());
    findPending();

    m_soon = new QTimer(this);
    m_soon->setSingleShot(true);
    m_soon->setInterval(1000);   // a second after the last change: a run of quick steps is written once
    connect(m_soon, &QTimer::timeout, this, &Recovery::snapshot);
    connect(doc, &Document::historyChanged, this, [this] {
        m_stepsNow = m_doc->historyLength();
        snapshotSoon();
    });
    connect(doc, &Document::fileChanged, this, &Recovery::snapshotSoon);
    connect(qApp, &QCoreApplication::aboutToQuit, this, &Recovery::removeOwn);

    if (::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, g_termPipe) == 0) {
        m_termNote = new QSocketNotifier(g_termPipe[0], QSocketNotifier::Read, this);
        connect(m_termNote, &QSocketNotifier::activated, this, [] {
            char one;
            (void)!::read(g_termPipe[0], &one, 1);
            QCoreApplication::quit();
        });
        struct sigaction term {};
        term.sa_handler = onTerm;
        sigemptyset(&term.sa_mask);
        term.sa_flags = SA_RESTART;
        ::sigaction(SIGTERM, &term, nullptr);
    }
    m_watcher = std::thread([this] { watch(); });
}

Recovery::~Recovery() {
    removeOwn();
}

// The guard (its own thread): every kPollMs, while heavy work runs, the system's MemAvailable against the limit.
void Recovery::watch() {
    char text[4096];
    std::chrono::steady_clock::time_point stoppedAt;
    for (;;) {
        {
            std::unique_lock<std::mutex> lock(m_wake);
            if (m_wakeUp.wait_for(lock, std::chrono::milliseconds(kPollMs), [this] { return m_quit; }))
                return;
        }
        if (!m_stopped && omamake::heavyCount() == 0)
            continue;
        if (!readSmall("/proc/meminfo", text, sizeof text))
            continue;
        const long total = kbOf(text, "MemTotal:");
        long avail = kbOf(text, "MemAvailable:");
        const long fake = m_fakeAvailKb.load();
        if (fake >= 0)
            avail = fake;
        if (total <= 0 || avail < 0)
            continue;
        if (m_stopped) {
            const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - stoppedAt).count();
            if (avail < total * kHardPercent / 100) {
                say("memory guard: memory still falling; closing OmaMake now\n");
                ::_exit(0);
            }
            if (!m_acked && ms > kFallbackMs) {
                // The window's thread didn't show the popup in time: the desktop's own notice, then close.
                say("memory guard: the window didn't answer; closing OmaMake (desktop notice instead of the popup)\n");
                char app[] = "notify-send", urgency[] = "-u", critical[] = "critical", title[] = "OmaMake",
                     body[] = "System memory running low, closing to protect system. Your design was saved for recovery.";
                char *argv[] = {app, urgency, critical, title, body, nullptr};
                pid_t child;
                if (::posix_spawnp(&child, app, nullptr, nullptr, argv, environ) == 0)
                    ::usleep(500 * 1000);
                ::_exit(0);
            }
            if (ms > kCloseMs) {
                say("memory guard: closing OmaMake\n");
                ::_exit(0);
            }
            continue;
        }
        if (avail >= total * m_percent / 100)
            continue;
        // Only when OmaMake itself holds a good part of it: something else filling memory isn't OmaMake's to stop.
        long own = 0;
        if (fake < 0) {
            char status[4096];
            if (readSmall("/proc/self/status", status, sizeof status))
                own = std::max(0L, kbOf(status, "VmRSS:")) + std::max(0L, kbOf(status, "VmSwap:"));
            if (own < total * kOwnPercent / 100)
                continue;
        }
        stoppedAt = std::chrono::steady_clock::now();
        stopNow(avail, total);
    }
}

// The memory stop, on the guard's thread: every job told to stop, the note written (small, from what was made ready),
// the window told. Nothing big is made here.
void Recovery::stopNow(long availKb, long totalKb) {
    m_stopped = true;
    omamake::requestMemoryStop();
    char line[256];
    std::snprintf(line, sizeof line, "memory guard: MemAvailable %ld MB of %ld MB (under %.0f %%): stopping and closing OmaMake\n",
                  availKb / 1024, totalKb / 1024, m_percent);
    say(line);
    char body[4096];
    int n;
    {
        std::lock_guard<std::mutex> lock(m_noteLock);
        n = std::snprintf(body, sizeof body,
                          "{\"reason\":\"memory\",\"pid\":%d,\"at\":%lld,\"avail_mb\":%ld,\"total_mb\":%ld,\"limit_percent\":%.1f,"
                          "\"steps_at_stop\":%d%s%s}\n",
                          int(::getpid()), static_cast<long long>(std::time(nullptr)), availKb / 1024, totalKb / 1024, m_percent,
                          m_stepsNow.load(), m_noteBody.empty() ? "" : ",", m_noteBody.c_str());
    }
    const int fd = ::open(m_notePath, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd >= 0) {
        (void)!::write(fd, body, std::size_t(std::max(0, std::min(n, int(sizeof body) - 1))));
        ::fsync(fd);
        ::close(fd);
    }
    QMetaObject::invokeMethod(this, [this] {
        emit memoryLow(QString::fromUtf8(kMessage),
                       m_snapSteps >= 0 ? QStringLiteral("Your design was saved for recovery. OmaMake will offer it the next time it opens.")
                                        : QStringLiteral("There was nothing to save yet."));
    }, Qt::QueuedConnection);
}

void Recovery::closeNow() {
    say("memory guard: closing OmaMake\n");
    ::_exit(0);
}

void Recovery::snapshotSoon() {
    if (!m_stopped)
        m_soon->start();
}

// Every file of a set: "<stem>.json" (what it is), "<stem>.s<steps>.omamake" (the design after that many history steps),
// "<stem>.stop.json" (a memory stop's note, once pending).
static QStringList filesOf(const QDir &dir, const QString &stem) {
    return dir.entryList({stem + QStringLiteral(".*")}, QDir::Files);
}

static int stepsOfFile(const QString &name) {   // "<stem>.s<steps>.omamake" -> steps (-1: not one)
    if (!name.endsWith(QLatin1String(".omamake")))
        return -1;
    const QString core = name.chopped(8);
    const int dot = core.lastIndexOf(QLatin1String(".s"));
    bool ok = false;
    const int steps = dot < 0 ? -1 : core.mid(dot + 2).toInt(&ok);
    return ok ? steps : -1;
}

// The recovery files, written off the window thread. The design is read here (Document::recoveryState: only when the
// engine is free, else again in half a second); a solid new since last time, turning it all into text and writing
// run on the writer. One file per state, by its history length, kept back to the state 2 Timeline items before
// the newest (what a memory stop's recovery opens); older and undone ones are removed.
void Recovery::snapshot() {
    if (m_stopped)
        return;
    if (m_writing) {
        m_dirty = true;
        return;
    }
    if (m_doc->historyLength() == 0 && m_doc->filePath().isEmpty()) {   // nothing to recover
        QDir dir(m_dir);
        for (const QString &name : filesOf(dir, m_stem))
            dir.remove(name);
        return;
    }
    QElapsedTimer clock;
    clock.start();
    std::vector<Document::LaterSolid> later;
    QJsonObject state = m_doc->recoveryState(later);
    if (state.isEmpty()) {
        m_soon->start(500);
        return;
    }
    const qint64 readMs = clock.elapsed();
    const int steps = m_doc->historyLength();
    const int rows = m_doc->recoveryRows(steps);
    const int rewindTo = m_doc->recoveryRewindTo();
    const QString filePath = m_doc->filePath();
    const QString name = state.value(QStringLiteral("name")).toString();
    m_rowsAt[steps] = rows;
    std::map<int, int> rowsAt = m_rowsAt;
    m_writing = true;
    if (m_writer.joinable())
        m_writer.join();
    const QString dir = m_dir, stem = m_stem;
    m_writer = std::thread([this, state, later = std::move(later), readMs, steps, rewindTo, rowsAt, filePath, name, dir, stem]() mutable {
        QElapsedTimer clock;
        clock.start();
        // The solids new since the last time (the step's new body), written here, kept by the document for next time.
        if (!later.empty()) {
            QJsonArray solids = state.value(QStringLiteral("solids")).toArray();
            QJsonArray sources = state.value(QStringLiteral("sources")).toArray();
            for (const Document::LaterSolid &one : later) {
                const std::string raw = one.write();
                if (raw.empty())
                    continue;
                const QString file = QString::fromLatin1(qCompress(QByteArray::fromRawData(raw.data(), qsizetype(raw.size()))).toBase64());
                const QString source = QString::fromStdString(m_doc->engineSourceOf(raw));
                QJsonObject entry{{QStringLiteral("id"), one.body}, {QStringLiteral("file"), file}};
                if (!source.isEmpty()) {
                    entry.insert(QStringLiteral("source"), source);
                    if (std::none_of(sources.begin(), sources.end(), [&](const QJsonValue &v) { return v.toObject().value(QStringLiteral("name")).toString() == source; })) {
                        const std::string mesh = m_doc->engineSourceBytes(source.toStdString());
                        sources.append(QJsonObject{{QStringLiteral("name"), source},
                                                   {QStringLiteral("mesh"), QString::fromLatin1(qCompress(QByteArray::fromRawData(mesh.data(), qsizetype(mesh.size()))).toBase64())}});
                    }
                }
                solids.append(entry);
                QMetaObject::invokeMethod(m_doc, [doc = m_doc, body = one.body, file, source] { doc->keepSolidText(body, file, source); }, Qt::QueuedConnection);
            }
            state.insert(QStringLiteral("solids"), solids);
            if (!sources.isEmpty())
                state.insert(QStringLiteral("sources"), sources);
        }
        const QByteArray bytes = QJsonDocument(state).toJson(QJsonDocument::Compact);
        QDir folder(dir);
        const QString mine = QStringLiteral("%1.s%2.omamake").arg(stem).arg(steps);
        QSaveFile file(folder.filePath(mine));
        bool ok = file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size() && file.commit();
        if (ok) {
            // Kept: this one, and back to the state 2 items before it (the newest at or before rewindTo); undone
            // states (more steps than now) and older ones go.
            int floor = -1;
            for (const QString &n : filesOf(folder, stem))
                if (const int s = stepsOfFile(n); s >= 0 && s <= rewindTo)
                    floor = std::max(floor, s);
            QJsonArray states;
            for (const QString &n : filesOf(folder, stem)) {
                const int s = stepsOfFile(n);
                if (s < 0)
                    continue;
                if (s > steps || (s < rewindTo && s != floor)) {
                    folder.remove(n);
                    continue;
                }
                states.append(QJsonObject{{QStringLiteral("steps"), s}, {QStringLiteral("file"), n},
                                          {QStringLiteral("rows"), rowsAt.count(s) ? rowsAt.at(s) : -1}});
            }
            QSaveFile meta(folder.filePath(stem + QStringLiteral(".json")));
            const QJsonObject about{{QStringLiteral("app"), QStringLiteral("omamake")},
                                    {QStringLiteral("pid"), int(::getpid())},
                                    {QStringLiteral("newest"), mine},
                                    {QStringLiteral("steps"), steps},
                                    {QStringLiteral("rows"), rowsAt.count(steps) ? rowsAt.at(steps) : -1},
                                    {QStringLiteral("rewind_to"), rewindTo},
                                    {QStringLiteral("states"), states},
                                    {QStringLiteral("file"), filePath},
                                    {QStringLiteral("name"), name},
                                    {QStringLiteral("written_at"), QDateTime::currentDateTime().toString(Qt::ISODate)}};
            ok = meta.open(QIODevice::WriteOnly) && meta.write(QJsonDocument(about).toJson(QJsonDocument::Indented)) > 0 && meta.commit();
        }
        const qint64 ms = readMs + clock.elapsed();
        const qint64 size = ok ? bytes.size() : -1;
        QMetaObject::invokeMethod(this, [this, readMs, ms, size, steps, rewindTo, filePath, name] {
            writeDone(readMs, ms, size, steps, rewindTo, filePath, name);
        }, Qt::QueuedConnection);
    });
}

void Recovery::writeDone(qint64 readMs, qint64 ms, qint64 bytes, int steps, int rewindTo, const QString &filePath, const QString &name) {
    m_writing = false;
    if (bytes >= 0) {
        ++m_writes;
        m_lastMs = ms;
        m_lastReadMs = readMs;
        m_lastBytes = bytes;
        m_snapSteps = steps;
        m_snapRewind = rewindTo;
        for (auto it = m_rowsAt.begin(); it != m_rowsAt.end();)   // undone states go (as the writer's files)
            it = it->first > steps ? m_rowsAt.erase(it) : std::next(it);
        const std::string body = QStringLiteral("\"steps\":%1,\"rewind_to\":%2,\"file\":\"%3\",\"name\":\"%4\"")
                                     .arg(steps).arg(rewindTo).arg(escaped(filePath), escaped(name)).toStdString();
        std::lock_guard<std::mutex> lock(m_noteLock);
        m_noteBody = body;
    } else {
        qWarning().noquote() << "recovery: couldn't write the recovery file in" << m_dir;
    }
    if (m_dirty) {
        m_dirty = false;
        snapshotSoon();
    }
}

// At start: a design left by a memory stop, or by a window that ended without closing (killed, crashed), is offered.
// Its files are renamed at once (pending-<pid>-<time>.*), so this window's own (window-<its pid>) can't overwrite them
// and another window opened meanwhile won't offer them too. The newest is offered; any older ones are set aside.
void Recovery::findPending() {
    QDir dir(m_dir);
    struct Found { QString stem; QString reason; QDateTime when; QJsonObject meta, note; };
    std::vector<Found> found;
    const auto readJson = [&](const QString &name) {
        QFile f(dir.filePath(name));
        return f.open(QIODevice::ReadOnly) ? QJsonDocument::fromJson(f.readAll()).object() : QJsonObject();
    };
    for (const QString &metaName : dir.entryList({QStringLiteral("window-*.json")}, QDir::Files)) {
        const QString stem = metaName.chopped(5);
        const int pid = stem.mid(7).toInt();
        const QString noteName = QStringLiteral("memory-stop-%1.json").arg(pid);
        const bool stopped = dir.exists(noteName);
        if (!stopped && pid != int(::getpid()) && alive(pid))
            continue;                                   // a window still open: its own
        Found f;
        f.reason = stopped ? QStringLiteral("memory") : QStringLiteral("ended");
        f.note = stopped ? readJson(noteName) : QJsonObject();
        f.when = stopped && f.note.contains(QStringLiteral("at")) ? QDateTime::fromSecsSinceEpoch(qint64(f.note.value(QStringLiteral("at")).toDouble()))
                                                                    : QFileInfo(dir.filePath(metaName)).lastModified();
        f.stem = QStringLiteral("pending-%1-%2").arg(pid).arg(f.when.toSecsSinceEpoch());
        for (const QString &n : filesOf(dir, stem))
            dir.rename(n, f.stem + n.mid(stem.size()));
        if (stopped)
            dir.rename(noteName, f.stem + QStringLiteral(".stop.json"));
        f.meta = readJson(f.stem + QStringLiteral(".json"));
        found.push_back(f);
    }
    for (const QString &noteName : dir.entryList({QStringLiteral("memory-stop-*.json")}, QDir::Files))
        if (!alive(noteName.mid(12, noteName.size() - 17).toInt()))
            dir.remove(noteName);                        // a stop with nothing saved yet: nothing to offer
    for (const QString &n : dir.entryList({QStringLiteral("window-*.omamake")}, QDir::Files))
        if (!alive(n.section(QLatin1Char('.'), 0, 0).mid(7).toInt()))
            dir.remove(n);                               // a design file whose note never got written
    // Pending ones an earlier start found and nobody answered (that window closed first) are offered again.
    for (const QString &metaName : dir.entryList({QStringLiteral("pending-*.json")}, QDir::Files)) {
        if (metaName.endsWith(QLatin1String(".stop.json")))
            continue;
        const QString stem = metaName.chopped(5);
        if (std::any_of(found.begin(), found.end(), [&](const Found &f) { return f.stem == stem; }))
            continue;
        Found f;
        f.stem = stem;
        f.meta = readJson(metaName);
        f.note = readJson(stem + QStringLiteral(".stop.json"));
        f.reason = f.note.isEmpty() ? QStringLiteral("ended") : QStringLiteral("memory");
        f.when = QDateTime::fromSecsSinceEpoch(stem.section(QLatin1Char('-'), 2, 2).toLongLong());
        found.push_back(f);
    }
    for (auto it = found.begin(); it != found.end();) {   // the newest state's file must be there
        const bool there = dir.exists(QStringLiteral("%1.s%2.omamake").arg(it->stem).arg(it->meta.value(QStringLiteral("steps")).toInt(-1)));
        if (!there)
            for (const QString &n : filesOf(dir, it->stem))
                dir.remove(n);
        it = there ? std::next(it) : found.erase(it);
    }
    if (found.empty())
        return;
    std::sort(found.begin(), found.end(), [](const Found &a, const Found &b) { return a.when > b.when; });
    for (std::size_t i = 1; i < found.size(); ++i)
        setAside(found[i].stem, QStringLiteral("set-aside"));
    const Found &f = found.front();
    const int steps = f.meta.value(QStringLiteral("steps")).toInt();
    m_pending = {{QStringLiteral("reason"), f.reason},
                 {QStringLiteral("stem"), f.stem},
                 {QStringLiteral("name"), f.meta.value(QStringLiteral("name")).toString()},
                 {QStringLiteral("filePath"), f.meta.value(QStringLiteral("file")).toString()},
                 {QStringLiteral("steps"), steps},
                 {QStringLiteral("stepsAtStop"), f.note.value(QStringLiteral("steps_at_stop")).toInt(steps)},
                 {QStringLiteral("rewindTo"), f.meta.value(QStringLiteral("rewind_to")).toInt(steps)},
                 {QStringLiteral("when"), f.when.date() == QDate::currentDate() ? f.when.toString(QStringLiteral("'at' h:mm AP 'today'"))
                                                                                  : f.when.toString(QStringLiteral("'at' h:mm AP 'on' ddd MMM d"))}};
    qInfo().noquote() << "recovery: offering" << f.stem << "(" << f.reason << ")";
}

// Yes. After a memory stop: the state 2 Timeline items before the newest, if its file is there (the newest is kept
// beside it in recovery/recovered, to Load); else the newest, and the window moves the marker back. After a window
// that ended without closing: the newest. {why: "" or why not, mode: "earlier" | "marker" | "newest", itemsBack,
// newest: the newest state's file once set aside, opened}.
QVariantMap Recovery::recover() {
    if (m_pending.isEmpty())
        return {{QStringLiteral("why"), QStringLiteral("Nothing to recover.")}};
    QDir dir(m_dir);
    const QString stem = m_pending.value(QStringLiteral("stem")).toString();
    QFile metaFile(dir.filePath(stem + QStringLiteral(".json")));
    const QJsonObject meta = metaFile.open(QIODevice::ReadOnly) ? QJsonDocument::fromJson(metaFile.readAll()).object() : QJsonObject();
    const int steps = meta.value(QStringLiteral("steps")).toInt(), rewindTo = meta.value(QStringLiteral("rewind_to")).toInt(steps);
    const int rowsNewest = meta.value(QStringLiteral("rows")).toInt(-1);
    const auto fileFor = [&](int s) { return QStringLiteral("%1.s%2.omamake").arg(stem).arg(s); };
    QString open = fileFor(steps), mode = QStringLiteral("newest");
    int itemsBack = 0;
    if (m_pending.value(QStringLiteral("reason")) == QLatin1String("memory") && rewindTo < steps) {
        mode = QStringLiteral("marker");
        int best = -1, bestRows = -1;
        for (const QJsonValue &v : meta.value(QStringLiteral("states")).toArray()) {
            const QJsonObject o = v.toObject();
            const int s = o.value(QStringLiteral("steps")).toInt(-1);
            if (s >= 0 && s <= rewindTo && s > best && dir.exists(fileFor(s))) {
                best = s;
                bestRows = o.value(QStringLiteral("rows")).toInt(-1);
            }
        }
        if (best >= 0) {
            open = fileFor(best);
            mode = QStringLiteral("earlier");
            itemsBack = rowsNewest >= 0 && bestRows >= 0 ? rowsNewest - bestRows : 2;
        }
    }
    const QString why = m_doc->openRecovered(dir.filePath(open), m_pending.value(QStringLiteral("filePath")).toString(),
                                             m_pending.value(QStringLiteral("name")).toString());
    if (!why.isEmpty())
        return {{QStringLiteral("why"), why}};
    setAside(stem, QStringLiteral("recovered"));
    const QString newest = QDir(dir.filePath(QStringLiteral("recovered"))).filePath(fileFor(steps));
    m_pending.clear();
    emit pendingChanged();
    return {{QStringLiteral("why"), QString()}, {QStringLiteral("mode"), mode}, {QStringLiteral("itemsBack"), itemsBack},
            {QStringLiteral("newest"), newest}, {QStringLiteral("opened"), open}};
}

void Recovery::decline() {
    if (m_pending.isEmpty())
        return;
    setAside(m_pending.value(QStringLiteral("stem")).toString(), QStringLiteral("set-aside"));
    m_pending.clear();
    emit pendingChanged();
}

void Recovery::setAside(const QString &stem, const QString &into) {
    QDir dir(m_dir);
    dir.mkpath(into);
    for (const QString &name : filesOf(dir, stem))
        dir.rename(name, into + QLatin1Char('/') + name);
    // The newest kKeep sets kept (by their .json).
    QDir kept(dir.filePath(into));
    QStringList sets = kept.entryList({QStringLiteral("*.json")}, QDir::Files, QDir::Time);
    sets.erase(std::remove_if(sets.begin(), sets.end(), [](const QString &n) { return n.endsWith(QLatin1String(".stop.json")); }), sets.end());
    for (int i = kKeep; i < sets.size(); ++i)
        for (const QString &name : filesOf(kept, sets[i].chopped(5)))
            kept.remove(name);
}

void Recovery::removeOwn() {
    {
        std::lock_guard<std::mutex> lock(m_wake);
        m_quit = true;
    }
    m_wakeUp.notify_all();
    if (m_watcher.joinable())
        m_watcher.join();
    if (m_writer.joinable())
        m_writer.join();
    QDir dir(m_dir);
    for (const QString &name : filesOf(dir, m_stem))
        dir.remove(name);
}

void Recovery::testSqueeze(int availMb) {
    if (qEnvironmentVariableIsSet("OMAMAKE_TEST_INPUT"))
        m_fakeAvailKb = availMb < 0 ? -1L : long(availMb) * 1024;
}

void Recovery::testSnapshotNow() {
    if (qEnvironmentVariableIsSet("OMAMAKE_TEST_INPUT"))
        snapshot();
}

QVariantMap Recovery::testState() const {
    return {{QStringLiteral("dir"), m_dir}, {QStringLiteral("stem"), m_stem}, {QStringLiteral("writes"), m_writes},
            {QStringLiteral("lastMs"), m_lastMs}, {QStringLiteral("readMs"), m_lastReadMs}, {QStringLiteral("lastBytes"), m_lastBytes},
            {QStringLiteral("steps"), m_snapSteps}, {QStringLiteral("rewindTo"), m_snapRewind},
            {QStringLiteral("writing"), m_writing}, {QStringLiteral("percent"), m_percent}};
}
