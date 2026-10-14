// Live shader-compile tracker.
//
// Every pipeline call on a game thread (create, cache-library load, forced wait for a
// placeholder) is timed and tagged with why it ran the way it did. Each frame (one Present)
// collects the calls that finished during it. A gameplay frame slower than HitchMs is
// recorded as a hitch together with its most likely cause, so a hitch can be told apart:
// "a pipeline compiled on the render thread for 380 ms" vs "no pipeline work at all this
// frame" (streaming, CPU or GPU work, not a shader compile).
#include "common.h"

#include <cstdio>
#include <mutex>

namespace d3a {
namespace {
thread_local CallCause t_cause = CallCause::Other;

// Calls that finished since the last Present.
std::atomic<uint32_t> g_winCalls{0};
std::atomic<uint64_t> g_winUs{0};
std::atomic<uint64_t> g_winMax{0};  // packed: us << 16 | recording << 8 | cause

constexpr int kRing = 256;
std::atomic<uint32_t> g_ring[kRing];  // frame ms * 10 | 0x80000000 if a compile caused it
std::atomic<uint32_t> g_ringPos{0};

std::mutex g_lastMu;
Hitch g_last{};
std::atomic<uint64_t> g_hitches{0}, g_compileHitches{0};
std::atomic<uint64_t> g_lastPlace{0}, g_lastSkip{0}, g_lastForced{0};
}  // namespace

const char* CauseName(CallCause c) {
    switch (c) {
        case CallCause::Async: return "background compile, waited BudgetMs";
        case CallCause::Startup: return "startup phase, compiled normally";
        case CallCause::Loading: return "loading screen, compiled normally";
        case CallCause::Ui: return "UI-type pipeline, compiled normally";
        case CallCause::QueueFull: return "compile queue full, compiled normally";
        case CallCause::Compute: return "compute pipeline, compiled normally";
        case CallCause::NotEligible: return "not eligible for background compile";
        case CallCause::CopyFailed: return "description could not be copied";
        case CallCause::LibraryLoad: return "pipeline-cache load";
        case CallCause::ForcedWait: return "forced wait for a background compile";
        case CallCause::UiWait: return "a draw or dispatch waited for its pipeline's background compile";
        default: return "other pipeline call";
    }
}

void SetCallCause(CallCause c) { t_cause = c; }
CallCause CurrentCallCause() { return t_cause; }

void TrackCall(uint64_t us, CallCause cause, bool recording) {
    if (!StartupPhase()) g_stats.callUsByCause[(int)cause].fetch_add(us, std::memory_order_relaxed);
    g_winCalls.fetch_add(1, std::memory_order_relaxed);
    g_winUs.fetch_add(us, std::memory_order_relaxed);
    uint64_t packed = (us << 16) | ((uint64_t)recording << 8) | (uint64_t)cause;
    uint64_t cur = g_winMax.load(std::memory_order_relaxed);
    while ((cur >> 16) < us && !g_winMax.compare_exchange_weak(cur, packed)) {}
}

void TrackFrame(double ms, double atSeconds, bool loading) {
    uint32_t calls = g_winCalls.exchange(0);
    uint64_t sumUs = g_winUs.exchange(0);
    uint64_t mx = g_winMax.exchange(0);
    uint64_t place = g_stats.psoPlaceholders.load(), skip = g_stats.drawsSkipped.load(), forced = g_stats.forcedWaits.load();
    uint64_t dPlace = place - g_lastPlace.exchange(place), dSkip = skip - g_lastSkip.exchange(skip),
             dForced = forced - g_lastForced.exchange(forced);

    double maxMs = (mx >> 16) / 1000.0;
    bool rec = (mx >> 8) & 1;
    auto cause = (CallCause)(mx & 0xff);
    bool startup = StartupPhase() || loading;  // loading screens aren't gameplay hitches
    bool hitch = !startup && g_cfg.hitchMs > 0 && ms >= g_cfg.hitchMs;

    Hitch h{};
    h.atSeconds = atSeconds;
    h.frameMs = ms;
    h.longestCallMs = maxMs;
    h.longestOnRenderThread = rec;
    h.longestCause = cause;
    h.calls = calls;
    h.callMs = sumUs / 1000.0;
    h.placeholders = dPlace;
    h.skipped = dSkip;
    h.forcedWaits = dForced;
    h.queue = PoolPending();
    // Attribution: a single call covering at least half the frame is the cause; else several.
    if (calls && maxMs >= ms * 0.5) h.kind = cause == CallCause::ForcedWait ? HitchKind::ForcedWait : HitchKind::Compile;
    else if (dForced) h.kind = HitchKind::ForcedWait;
    else if (calls && h.callMs >= ms * 0.5) h.kind = HitchKind::SeveralCalls;
    else h.kind = HitchKind::NotPipeline;

    bool compile = hitch && h.kind != HitchKind::NotPipeline;
    uint32_t v = (uint32_t)std::min(ms * 10.0, 2000000000.0) | (compile ? 0x80000000u : 0);
    g_ring[g_ringPos.fetch_add(1) % kRing].store(v, std::memory_order_relaxed);
    if (!hitch) return;
    g_hitches++;
    if (compile) g_compileHitches++;
    { std::lock_guard<std::mutex> lk(g_lastMu); g_last = h; }
    RunLogHitch(h);
    static std::atomic<int> logged{0};
    if ((h.kind == HitchKind::Compile || h.kind == HitchKind::ForcedWait) && logged.fetch_add(1) < 500)
        Log(LogLevel::Info, "hitch %.0f ms at %.1f s: %s (%.0f ms, %s thread)", ms, atSeconds,
            h.kind == HitchKind::ForcedWait ? "forced wait for a background compile" : CauseName(cause), maxMs,
            rec ? "command-recording" : "game task");
}

std::string DescribeHitch(const Hitch& h) {
    char b[256];
    switch (h.kind) {
        case HitchKind::Compile:
            snprintf(b, sizeof b, "%s: %.0f ms on a %s thread", CauseName(h.longestCause), h.longestCallMs,
                     h.longestOnRenderThread ? "command-recording" : "game task");
            break;
        case HitchKind::ForcedWait:
            if (h.longestCause == CallCause::ForcedWait && h.longestCallMs > 0)
                snprintf(b, sizeof b, "forced wait for a background compile: %.0f ms on a %s thread", h.longestCallMs,
                         h.longestOnRenderThread ? "command-recording" : "game task");
            else
                snprintf(b, sizeof b, "forced wait for a background compile");
            break;
        case HitchKind::SeveralCalls: snprintf(b, sizeof b, "%u pipeline calls, %.0f ms in total", h.calls, h.callMs); break;
        default: snprintf(b, sizeof b, "no pipeline compile this frame (streaming / CPU / GPU)"); break;
    }
    return b;
}

uint64_t HitchCount(bool compileOnly) { return compileOnly ? g_compileHitches.load() : g_hitches.load(); }
bool LastHitch(Hitch* out) {
    std::lock_guard<std::mutex> lk(g_lastMu);
    if (g_last.frameMs <= 0) return false;
    *out = g_last;
    return true;
}
int RecentFrames(float* ms, bool* compile, int max) {
    uint32_t end = g_ringPos.load();
    int n = (int)std::min<uint32_t>(end, (uint32_t)std::min(max, kRing));
    for (int i = 0; i < n; ++i) {
        uint32_t v = g_ring[(end - n + i) % kRing].load(std::memory_order_relaxed);
        ms[i] = (v & 0x7fffffff) / 10.0f;
        compile[i] = v & 0x80000000u;
    }
    return n;
}

}  // namespace d3a
