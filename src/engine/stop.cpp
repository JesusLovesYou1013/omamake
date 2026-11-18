#include "engine/stop.h"

namespace omamake {

namespace {
std::atomic_bool g_memoryStop{false};
std::atomic_int g_heavy{0};
thread_local std::chrono::steady_clock::time_point t_deadline = std::chrono::steady_clock::time_point::max();
} // namespace

bool stopAsked() {
    return g_memoryStop.load(std::memory_order_relaxed) || std::chrono::steady_clock::now() > t_deadline;
}

void requestMemoryStop() { g_memoryStop = true; }
bool memoryStopRequested() { return g_memoryStop.load(); }
std::chrono::steady_clock::time_point stopDeadline() { return t_deadline; }

StopScope::StopScope(double seconds)
    : m_was(t_deadline),
      m_deadline(std::chrono::steady_clock::now() + std::chrono::milliseconds(static_cast<long long>(seconds * 1000))) {
    if (m_deadline < t_deadline)
        t_deadline = m_deadline;
}

StopScope::~StopScope() { t_deadline = m_was; }

bool StopScope::timedOut() const { return std::chrono::steady_clock::now() > m_deadline; }

HeavyScope::HeavyScope() { ++g_heavy; }
HeavyScope::~HeavyScope() { --g_heavy; }
int heavyCount() { return g_heavy.load(); }

} // namespace omamake
