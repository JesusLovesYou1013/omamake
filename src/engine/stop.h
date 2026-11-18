#pragma once
// Stopping long engine work (GB-089, 2026-11-16).
//
// Two reasons a job is stopped part way:
//  - time: Extrude's automatic second try with Fuzzy profile has a limit (kFuzzySeconds in document.cpp). The worker
//    doing it opens a StopScope with that many seconds; OpenCascade's Boolean is asked through its progress range
//    (Message_ProgressIndicator::UserBreak, heavyengine.cpp BreakCheck) and between its own steps (stopAsked()), so
//    it gives up, the extrude puts everything back and says why.
//  - memory: the memory guard (src/document/recovery.cpp) found the system's free memory under its limit while heavy
//    work ran. Every job then stops (stopAsked() is true on every thread) and OmaMake closes itself, after saving.
// HeavyScope marks a worker's heavy work, so the guard watches only while some runs.
#include <atomic>
#include <chrono>

namespace omamake {

// The work on this thread should stop: the memory guard asked, or this thread's StopScope ran out of time.
bool stopAsked();
// Asked by the memory guard (every job stops; the window then closes).
void requestMemoryStop();
bool memoryStopRequested();
// The deadline of this thread's innermost StopScope (time_point::max(): none). A Boolean's break check takes a copy,
// as OpenCascade may ask it from its own threads.
std::chrono::steady_clock::time_point stopDeadline();

class StopScope {
public:
    explicit StopScope(double seconds);
    ~StopScope();
    StopScope(const StopScope &) = delete;
    StopScope &operator=(const StopScope &) = delete;
    // This scope's time ran out (checked when asked).
    bool timedOut() const;
private:
    std::chrono::steady_clock::time_point m_was, m_deadline;
};

// Heavy work running on this thread (an extrude, import, fit, rebuild, preview's solid): counted while it lasts.
class HeavyScope {
public:
    HeavyScope();
    ~HeavyScope();
    HeavyScope(const HeavyScope &) = delete;
    HeavyScope &operator=(const HeavyScope &) = delete;
};
int heavyCount();

} // namespace omamake
