// d3d12async - background PSO compilation layer for D3D12 games.
// Copyright (c) 2026 Meower. MIT License.
#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <directx/d3d12.h>
#include <dxguids/dxguids.h>
#include <atomic>
#include <cstdint>
#include <string>

namespace d3a {

enum class LogLevel : int { Error = 0, Warn = 1, Info = 2, Perf = 3, Debug = 4 };

struct Config {
    bool enabled = true;          // master switch; 0 = pure pass-through
    double budgetMs = 1.0;        // wait this long for a compile before handing out a placeholder
    int workers = 2;              // 0 = auto
    bool allThreads = true;       // false: only threads that record command lists get placeholders
    int maxQueue = 256;           // past this many queued compiles, non-recording threads compile synchronously
    int burstWorkers = -1;        // extra compile threads while the queue is long; -1 = auto (CPUs allowed by WorkerCpus, max 8)
    int burstQueue = 16;          // ...used once more than this many compiles are waiting
    int maxPlaceholders = 512;    // QueueFullMode=placeholder: at most this many placeholders outstanding (not yet compiled)
    std::atomic<bool> queueFullPlaceholder{true};  // past maxQueue, still hand out placeholders (queue grows) instead of compiling
                                                    // on the game thread; can be toggled live from the settings panel
    bool workerEfficiency = true;   // EXPERIMENTAL: compile threads in Windows efficiency mode (EcoQoS) with low memory priority
    double maxCompilesPerSec = 0;   // EXPERIMENTAL: start at most this many background compiles per second (0 = no limit)
    int computeMode = 2;          // compute pipelines: 0 = compile normally; 1 = background, a dispatch that needs one waits;
                                  // 2 = EXPERIMENTAL background, dispatches skipped until ready
    bool lowPriority = true;      // background compile threads at below-normal priority
    std::wstring workerCpus = L"upper-half";  // logical CPUs the compile threads may run on
    double slowLogMs = 50.0;      // log individual background compiles slower than this
    double debugDelayMs = 0;      // testing: extra delay before each background compile
    bool overlay = true;          // small on-screen label showing the layer is loaded
    bool overlayStats = true;     // ...with live counters
    bool overlayFollowFocus = false; // hide it while the game isn't the foreground window
    int overlayOpacity = 200;     // 0..255
    bool startupSync = true;      // compile normally during the game's startup precompile flood
    int startupMinPsos = 64;      // ...which counts as over after at least this many PSOs
    int startupQuietSeconds = 10;  // ...followed by this many seconds
    int startupQuietRate = 10;    // ...with at most this many PSO creations each
    bool startupBoost = true;     // raise the compiling thread's priority during the startup phase
    int startupCpuBusy = 80;      // game CPU % (of all CPUs) above which a second counts as busy; 0 = ignore CPU
    int startupFloodSeconds = 10; // busy seconds after the first frame needed before the phase may end
    int startupFloodWait = 45;    // ...unless no precompile starts within this many seconds of the first frame
    bool startupHotkey = true;    // Ctrl+Shift+End ends the startup phase by hand
    int startupMinSeconds = 40;   // the startup phase can't end automatically before this many seconds after launch
    bool startupWarmLaunch = true;  // a launch whose startup pipelines are nearly all driver-cache hits ends the phase early
    double startupSlowMs = 5;     // a create taking at least this long is a real compile (not a cache hit)
    int startupMaxSeconds = 0;    // optional hard limit on the startup phase; 0 = none
    int startupFpsCap = 30;       // frame-rate cap during the startup phase only (0 = off)
    bool measureWhenDisabled = true;  // Enabled=0 still hooks (pass-through) to measure, for OFF run logs
    bool runLogs = true;          // per-run frame-time logs in d3d12async-runs
    int runLogsKeepOn = 1;        // how many runs to keep with the layer on / off
    int runLogsKeepOff = 1;
    int hitchMs = 50;             // gameplay frames at least this slow are recorded as hitches
    bool overlayGraph = true;     // frame-time graph in the on-screen label
    bool settingsPanel = false;   // small settings panel (bottom left) with live hotkey toggles
    int uiMode = 1;               // UI-type pipelines: 0 = like any other, 1 = wait (background, draws wait), 2 = sync
    double uiWaitMaxMs = 2;
    bool loadingSync = false;     // compile normally while the game shows no frames (a loading screen); off by default
    int loadingGapMs = 500;       // ...i.e. while no frame has been presented for this long
    int loadingHoldMs = 1500;     // ...and this long after frames resume, if the gap lasted 2x loadingGapMs       // uiMode 1: a draw waits at most this long, then is skipped (0 = wait for the whole compile)
    bool syncUi = true;           // UI-type pipelines (8-bit targets, no depth) always compile normally
    LogLevel logLevel = LogLevel::Info;
};
extern Config g_cfg;

void LoadConfig(const std::wstring& dir);

void LogInit(const std::wstring& dir);
void Log(LogLevel lvl, const char* fmt, ...);
std::wstring ModuleDir(HMODULE m);

// Statistics, reported periodically and at exit.
struct Stats {
    std::atomic<uint64_t> psoRequests{0};      // graphics PSO creations seen
    std::atomic<uint64_t> psoSync{0};          // passed straight through (not eligible)
    std::atomic<uint64_t> psoInBudget{0};      // compiled within the budget (no placeholder)
    std::atomic<uint64_t> psoPlaceholders{0};  // handed out as placeholders
    std::atomic<uint64_t> psoFailed{0};        // background compile failed
    std::atomic<uint64_t> drawsSkipped{0};     // draws dropped while a PSO was compiling
    std::atomic<uint64_t> forcedWaits{0};      // placeholder had to be waited on (bundle, residency, library...)
    std::atomic<uint64_t> bgCompileUs{0};      // total background compile time
    std::atomic<uint64_t> psoStartupSync{0};
    std::atomic<uint64_t> psoComputeAsync{0};
    std::atomic<uint64_t> dispatchesSkipped{0};  // ComputePipelineMode=skip  // compute pipelines compiled in background with wait-on-use
    std::atomic<uint64_t> uiWaits{0};          // draws that waited for a UI-type pipeline
    std::atomic<uint64_t> uiWaitTimeouts{0};
    std::atomic<uint64_t> psoLoadingSync{0};   // compiled normally while a loading screen was detected   // ...and were skipped after UiWaitMaxMs
    std::atomic<uint64_t> psoUiAsync{0};       // UI-type pipelines compiled in background with wait-on-use
    std::atomic<uint64_t> psoCreates{0};       // every graphics/stream PSO creation the hooks saw, any mode
    std::atomic<uint64_t> computeCreates{0};   // CreateComputePipelineState calls
    std::atomic<uint64_t> libraryLoads{0};     // ID3D12PipelineLibrary Load*Pipeline calls
    std::atomic<uint64_t> callUs{0};           // time game threads spent inside PSO create/load calls
    std::atomic<uint64_t> callUsByCause[12]{};  // ...split by CallCause   // compiled normally during the startup phase
    std::atomic<uint64_t> psoUiSync{0};        // UI-type pipelines compiled normally
    std::atomic<uint64_t> psoOverflow{0};      // QueueFullMode=placeholder: placeholders handed out past MaxQueuedCompiles
    std::atomic<uint64_t> rateLimitedUs{0};    // MaxCompilesPerSecond: time workers waited before starting a compile
    std::atomic<uint64_t> queuePeak{0};        // longest the compile queue got after the startup phase
};
extern Stats g_stats;

void LogStats(const char* why);
void InstallCrashLog();
void DescribeAddress(void* addr, char* out, size_t n);  // "module.dll+0x1234"

// Why a placeholder had to be waited on (each one blocks the calling game thread).
enum WaitReason { WaitQueryInterface, WaitCachedBlob, WaitBundle, WaitResidency, WaitLibrary, WaitOther, WaitReasonCount };
void NoteForcedWait(WaitReason r, void* caller, const char* detail);
void OverlayStart();

// Present hook (present.cpp): frame pacing during the startup phase and frame timing.
void HookPresent(ID3D12Device* device);
bool StartupCapping();
uint64_t PresentCount();  // the startup frame cap is limiting right now
void RunLogFrame(double frameMs, bool loading = false);   // runlog.cpp
void RunLogSetDir(const std::wstring& dllDir);
size_t PoolPending();
bool ApplyEfficiencyMode(bool on);  // WorkerEfficiencyMode on the calling thread (async_pso.cpp)

// Startup phase (phase.cpp)
bool StartupPhase();    // the game's startup precompile is still running (detected in every mode)
bool StartupActive();   // ...and the layer is enabled and acting on it
// Loading-screen detection (present.cpp): the game hasn't presented a frame for LoadingGapMs
// (or did so only shortly ago, after a long gap). Pipelines then compile normally, as at startup.
bool LoadingActive();
bool LoadingFrame(double frameMs);  // this frame was a loading screen (long, with normal compiles in it)
struct LoadingStats { uint64_t periods; double seconds; };
LoadingStats GetLoadingStats();
int LoadingGapForTest();  // ms since the last frame (test aid)
double StartupEndedAt();
double StartupCpuPercent();
bool StartupBusyNow();
void StartupFirstFrame();  // the game presented its first frame  // the last busy second (pipeline calls or CPU) was within 2 s  // last sampled game CPU %, -1 before the first sample  // seconds after DLL load the precompile went quiet; -1 while running
uint64_t StartupPsos();
uint64_t StartupSlowCompiles();  // creates of at least StartupSlowCompileMs during the phase
bool StartupWarm();             // the phase was ended by the warm-launch rule
void StartupNoteSlow();
void StartupResetForTest();
void StartupNote();   // pipeline activity happened (create or library load)
// Times a create/load call on the game's thread (adds to g_stats.callUs).
struct CallTimer {
    int64_t t0;
    CallTimer();
    ~CallTimer();
};

// Live compile tracker (tracker.cpp). Why a pipeline call on a game thread ran the way it did.
enum class CallCause : uint8_t { Other, Async, Startup, Ui, QueueFull, Compute, NotEligible, CopyFailed, LibraryLoad, ForcedWait, UiWait, Loading };
constexpr int kCallCauses = 12;
enum class HitchKind : uint8_t { NotPipeline, Compile, ForcedWait, SeveralCalls };
struct Hitch {
    double atSeconds, frameMs, longestCallMs, callMs;
    bool longestOnRenderThread;
    CallCause longestCause;
    HitchKind kind;
    uint32_t calls;
    uint64_t placeholders, skipped, forcedWaits, queue;
};
void SetCallCause(CallCause c);       // tag the current call (read when its CallTimer ends)
CallCause CurrentCallCause();
const char* CauseName(CallCause c);
void TrackCall(uint64_t us, CallCause cause, bool recording);
void TrackFrame(double ms, double atSeconds, bool loading = false);  // called once per Present
std::string DescribeHitch(const Hitch& h);
uint64_t HitchCount(bool compileOnly);
bool LastHitch(Hitch* out);
int RecentFrames(float* ms, bool* compile, int max);
bool OnRecordingThread();               // hooks.cpp
int64_t OutstandingPlaceholders();      // hooks.cpp: placeholders whose compile hasn't finished
void RunLogHitch(const Hitch& h);       // runlog.cpp
double RunSeconds();                    // runlog.cpp: seconds since DLL load
void StartupTick();   // called often (every command list Close); evaluates the end of the phase
bool IsUiLike(UINT numRt, const DXGI_FORMAT* rt, DXGI_FORMAT ds);
// While alive, the current thread (one of the game's compile threads) runs above everything
// else in the game, so during the startup precompile the CPU goes to compiling first.
struct StartupBoost {
    int prev = THREAD_PRIORITY_ERROR_RETURN;
    explicit StartupBoost(bool on = true);
    ~StartupBoost();
};

// Hook installation (hooks.cpp)
void HookDevice(IUnknown* anyDeviceInterface);
void HookCommandList(IUnknown* anyCommandList);
void HookPipelineLibrary(IUnknown* lib);
void HookDeviceFactory(IUnknown* factory);
void HookSdkConfiguration(IUnknown* cfg);

}  // namespace d3a
