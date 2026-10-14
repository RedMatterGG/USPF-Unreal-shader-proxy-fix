// Startup-phase detection. While a game runs its startup shader precompile, every PSO is
// compiled normally (stock behaviour). The phase ends once that flood of PSO creations has
// died down, judged from the creations themselves rather than a fixed timer: at least
// StartupMinPsos creations seen, then StartupQuietSeconds consecutive seconds with no more
// than StartupQuietRate creations each.
#include "common.h"

#include <mutex>

namespace d3a {
namespace {
constexpr int kBuckets = 64;
std::atomic<bool> g_active{true};
std::atomic<uint64_t> g_total{0};
std::atomic<int64_t> g_sec[kBuckets];
std::atomic<uint32_t> g_count[kBuckets];
std::atomic<int64_t> g_lastEvalMs{0};
LARGE_INTEGER g_freq = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return f; }();

int64_t NowMs() {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart * 1000 / g_freq.QuadPart;
}
int64_t g_startMs = NowMs();  // DLL load
std::atomic<int64_t> g_endedMs{-1};
// End of the most recent "busy" second (more than StartupQuietRate creates/loads), or DLL load.
// The quiet window is measured from here, so time before the game started can never count as quiet.
std::atomic<int64_t> g_lastBusyMs{g_startMs};
std::atomic<int64_t> g_phaseStartMs{g_startMs};  // StartupMinSeconds counts from here (DLL load; tests reset it)

// The precompile itself must have been seen before the phase may end: busy seconds counted only
// after the game's first frame. Without this, a quiet stretch between engine start-up and the
// compile screen (Borderlands 4 plays ~20 s of intro there) ended the phase before the compile
// screen even began.
std::atomic<int64_t> g_firstFrameMs{0};
std::atomic<int64_t> g_lastCountedSec{-1};
std::atomic<int> g_floodSecs{0};
std::atomic<bool> g_manualEnd{false};

void MarkBusy(int64_t atMs) {
    int64_t cur = g_lastBusyMs.load(std::memory_order_relaxed);
    while (cur < atMs && !g_lastBusyMs.compare_exchange_weak(cur, atMs)) {}
    int64_t ff = g_firstFrameMs.load(std::memory_order_relaxed);
    if (!ff || atMs < ff) return;
    int64_t sec = atMs / 1000, prev = g_lastCountedSec.load(std::memory_order_relaxed);
    if (sec != prev && g_lastCountedSec.compare_exchange_strong(prev, sec)) g_floodSecs++;
}

// Game CPU usage, sampled about once a second from the phase check. A precompile keeps the
// CPU near 100%; while the game's CPU use stays above StartupCpuBusyPercent the phase counts
// as busy even if no pipeline calls are visible (some games compile through other routes).
std::mutex g_cpuMu;
int64_t g_cpuSampleMs = 0;
uint64_t g_cpuPrev = 0;
double g_cpuLast = -1;
int g_ncpu = [] { SYSTEM_INFO si; GetSystemInfo(&si); return (int)si.dwNumberOfProcessors; }();

uint64_t ProcessCpu100ns() {
    FILETIME a, b, k, u;
    GetProcessTimes(GetCurrentProcess(), &a, &b, &k, &u);
    return (((uint64_t)k.dwHighDateTime << 32) | k.dwLowDateTime) + (((uint64_t)u.dwHighDateTime << 32) | u.dwLowDateTime);
}

void SampleCpu(int64_t now) {
    std::lock_guard<std::mutex> lk(g_cpuMu);
    if (g_cpuSampleMs && now - g_cpuSampleMs < 1000) return;
    uint64_t t = ProcessCpu100ns();
    if (g_cpuSampleMs) {
        g_cpuLast = (t - g_cpuPrev) / ((now - g_cpuSampleMs) * 10000.0 * g_ncpu) * 100.0;
        if (g_cfg.startupCpuBusy > 0 && g_cpuLast > g_cfg.startupCpuBusy) MarkBusy(now);
    }
    g_cpuSampleMs = now;
    g_cpuPrev = t;
}
}  // namespace

// Detection always runs (also with Enabled=0, for the run logs); behaviour only changes when enabled.
bool StartupPhase() { return g_active.load(std::memory_order_relaxed); }
bool StartupActive() { return g_cfg.enabled && g_cfg.startupSync && StartupPhase(); }
bool StartupBusyNow() { return NowMs() - g_lastBusyMs.load(std::memory_order_relaxed) < 2000; }
double StartupCpuPercent() { std::lock_guard<std::mutex> lk(g_cpuMu); return g_cpuLast; }
double StartupEndedAt() { return g_endedMs.load() < 0 ? -1.0 : (g_endedMs.load() - g_startMs) / 1000.0; }
uint64_t StartupPsos() { return g_total.load(); }

// The phase is one-way: it starts at launch and, once over, nothing turns it back on for the rest
// of the session (no CPU spike, loading screen or pipeline burst). Only the test suite resets it.
void StartupResetForTest() {
    g_total = 0;
    for (int i = 0; i < kBuckets; ++i) { g_sec[i] = 0; g_count[i] = 0; }
    g_endedMs = -1;
    g_lastBusyMs = NowMs();
    g_firstFrameMs = 0;
    g_lastCountedSec = -1;
    g_floodSecs = 0;
    g_manualEnd = false;
    g_phaseStartMs = NowMs();
    { std::lock_guard<std::mutex> lk(g_cpuMu); g_cpuSampleMs = 0; g_cpuLast = -1; }
    g_active = true;
}

CallTimer::CallTimer() {
    SetCallCause(CallCause::Other);
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    t0 = t.QuadPart;
}
CallTimer::~CallTimer() {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    uint64_t us = (uint64_t)((t.QuadPart - t0) * 1000000 / g_freq.QuadPart);
    g_stats.callUs.fetch_add(us, std::memory_order_relaxed);
    TrackCall(us, CurrentCallCause(), OnRecordingThread());
}

void StartupFirstFrame() {
    int64_t z = 0;
    g_firstFrameMs.compare_exchange_strong(z, NowMs());
}

void StartupNote() {
    if (!StartupPhase()) return;
    int64_t s = NowMs() / 1000;
    int i = (int)(s % kBuckets);
    int64_t prev = g_sec[i].load(std::memory_order_relaxed);
    if (prev != s && g_sec[i].compare_exchange_strong(prev, s)) g_count[i].store(0);
    uint32_t c = ++g_count[i];
    g_total++;
    if (c > (uint32_t)g_cfg.startupQuietRate) MarkBusy((s + 1) * 1000);
}

void StartupTick() {
    if (!StartupPhase()) return;
    int64_t now = NowMs(), last = g_lastEvalMs.load(std::memory_order_relaxed);
    if (now - last < 250 || !g_lastEvalMs.compare_exchange_strong(last, now)) return;  // cheap: at most 4x a second
    SampleCpu(now);
    if (g_cfg.startupHotkey && (GetAsyncKeyState(VK_CONTROL) & 0x8000) && (GetAsyncKeyState(VK_SHIFT) & 0x8000) &&
        (GetAsyncKeyState(VK_END) & 0x8000))
        g_manualEnd = true;
    bool manual = g_manualEnd.load();
    // StartupMinSeconds: for this long after launch nothing but the hotkey ends the phase
    // (Borderlands 4 can go quiet for 17 s between its intro and the compile screen).
    if (!manual && g_cfg.startupMinSeconds > 0 && now - g_phaseStartMs.load() < (int64_t)g_cfg.startupMinSeconds * 1000) return;
    bool forced = manual || (g_cfg.startupMaxSeconds > 0 && now - g_startMs >= (int64_t)g_cfg.startupMaxSeconds * 1000);
    if (!forced && g_total.load() < (uint64_t)g_cfg.startupMinPsos) return;
    if (!forced && g_cfg.startupFloodSeconds > 0 && g_floodSecs.load() < g_cfg.startupFloodSeconds) {
        // No precompile seen yet. A game whose caches are already warm may never have one:
        // after StartupFloodWaitSeconds past the first frame, the normal quiet rule applies.
        int64_t ff = g_firstFrameMs.load();
        bool waitedEnough = ff && g_cfg.startupFloodWait > 0 && now - ff >= (int64_t)g_cfg.startupFloodWait * 1000;
        if (!waitedEnough) return;
    }
    int64_t busyEnd = g_lastBusyMs.load();
    if (!forced && now - busyEnd < (int64_t)g_cfg.startupQuietSeconds * 1000) return;
    if (!forced && g_cfg.startupCpuBusy > 0 && StartupCpuPercent() < 0) return;  // no CPU sample yet
    if (forced) busyEnd = now;
    bool expected = true;
    if (g_active.compare_exchange_strong(expected, false)) {
        g_endedMs = busyEnd;  // when it actually went quiet
        if (manual)
            Log(LogLevel::Info, "startup phase ended by the Ctrl+Shift+End hotkey at %.1f s (%llu pipeline creates/loads)%s",
                (now - g_startMs) / 1000.0, (unsigned long long)g_total.load(), g_cfg.enabled ? ": background compiling is now on" : "");
        else if (forced)
            Log(LogLevel::Info, "startup phase ended by StartupMaxSeconds=%d (%llu pipeline creates/loads, game CPU %.0f%%)%s",
                g_cfg.startupMaxSeconds, (unsigned long long)g_total.load(), g_cpuLast,
                g_cfg.enabled ? ": background compiling is now on" : "");
        else
            Log(LogLevel::Info, "startup precompile finished at %.1f s (%llu pipeline creates/loads, then %d quiet seconds with "
                "game CPU <= %d%%, now %.0f%%; %d busy seconds after the first frame)%s",
                StartupEndedAt(), (unsigned long long)g_total.load(), g_cfg.startupQuietSeconds, g_cfg.startupCpuBusy, g_cpuLast,
                g_floodSecs.load(),
                g_cfg.enabled ? ": background compiling is now on" : "");
    }
}

StartupBoost::StartupBoost(bool on) {
    if (!on || !g_cfg.enabled || !g_cfg.startupBoost) return;
    HANDLE t = GetCurrentThread();
    int p = GetThreadPriority(t);
    if (p != THREAD_PRIORITY_ERROR_RETURN && p < THREAD_PRIORITY_HIGHEST && SetThreadPriority(t, THREAD_PRIORITY_HIGHEST)) prev = p;
}
StartupBoost::~StartupBoost() {
    if (prev != THREAD_PRIORITY_ERROR_RETURN) SetThreadPriority(GetCurrentThread(), prev);
}

// Pipelines that only draw to 8-bit colour targets without depth are what UI, fonts and
// cached widget textures use. A skipped draw there can stick (a widget rendered once into a
// texture and reused), so these always compile normally; they are few and cheap.
bool IsUiLike(UINT numRt, const DXGI_FORMAT* rt, DXGI_FORMAT ds) {
    if (!g_cfg.syncUi || numRt == 0 || ds != DXGI_FORMAT_UNKNOWN) return false;
    for (UINT i = 0; i < numRt && i < 8; ++i) {
        switch (rt[i]) {
            case DXGI_FORMAT_R8G8B8A8_UNORM:
            case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
            case DXGI_FORMAT_B8G8R8A8_UNORM:
            case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
            case DXGI_FORMAT_B8G8R8X8_UNORM:
            case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
            case DXGI_FORMAT_R10G10B10A2_UNORM:
            case DXGI_FORMAT_R8_UNORM:
            case DXGI_FORMAT_R8G8_UNORM:
            case DXGI_FORMAT_A8_UNORM:
                break;
            default:
                return false;
        }
    }
    return true;
}

}  // namespace d3a
