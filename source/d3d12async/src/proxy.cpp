// d3d12.dll proxy: forwards every export to the system d3d12.dll and hooks the objects
// the game creates. Nothing on disk is modified; remove the DLL to uninstall.
#include "common.h"

#include <cstring>

using namespace d3a;

// Export pointers filled from the real d3d12.dll; the assembly stubs jump through them.
extern "C" {
void* real_Ordinal99 = nullptr;
void* real_SetAppCompatStringPointer = nullptr;
void* real_D3D12GetDebugInterface = nullptr;
void* real_D3D12CoreCreateLayeredDevice = nullptr;
void* real_D3D12CoreGetLayeredDeviceSize = nullptr;
void* real_D3D12CoreRegisterLayers = nullptr;
void* real_D3D12CreateRootSignatureDeserializer = nullptr;
void* real_D3D12CreateVersionedRootSignatureDeserializer = nullptr;
// In the system DLL this export is a data block that debuggers read, not a function.
// Exporting it as data (zeroed) keeps anything that touches it from jumping into code.
alignas(16) unsigned char d3a_dred_placeholder[1024] = {};
void* real_D3D12EnableExperimentalFeatures = nullptr;
void* real_D3D12PIXEventsReplaceBlock = nullptr;
void* real_D3D12PIXGetThreadInfo = nullptr;
void* real_D3D12PIXNotifyWakeFromFenceSignal = nullptr;
void* real_D3D12PIXReportCounter = nullptr;
void* real_D3D12SerializeRootSignature = nullptr;
void* real_D3D12SerializeVersionedRootSignature = nullptr;
void* real_GetBehaviorValue = nullptr;
void* real_D3D12CreateDevice = nullptr;
void* real_D3D12GetInterface = nullptr;
void d3a_resolve_exports();
}

namespace {
HMODULE g_self = nullptr;
HMODULE g_real = nullptr;
INIT_ONCE g_once = INIT_ONCE_STATIC_INIT;

BOOL CALLBACK ResolveOnce(PINIT_ONCE, PVOID, PVOID*) {
    wchar_t sys[MAX_PATH];
    UINT n = GetSystemDirectoryW(sys, MAX_PATH);
    std::wstring path = std::wstring(sys, n) + L"\\d3d12.dll";
    g_real = LoadLibraryW(path.c_str());
    if (!g_real) {
        Log(LogLevel::Error, "could not load the system d3d12.dll (error %lu)", GetLastError());
        return TRUE;
    }
    if (g_real == g_self) {  // shouldn't happen, but never forward to ourselves
        Log(LogLevel::Error, "system d3d12.dll resolved to this proxy");
        g_real = nullptr;
        return TRUE;
    }
#define R(name) real_##name = (void*)GetProcAddress(g_real, #name)
    real_Ordinal99 = (void*)GetProcAddress(g_real, MAKEINTRESOURCEA(99));
    R(SetAppCompatStringPointer);
    R(D3D12GetDebugInterface);
    R(D3D12CoreCreateLayeredDevice);
    R(D3D12CoreGetLayeredDeviceSize);
    R(D3D12CoreRegisterLayers);
    R(D3D12CreateRootSignatureDeserializer);
    R(D3D12CreateVersionedRootSignatureDeserializer);
    R(D3D12EnableExperimentalFeatures);
    R(D3D12PIXEventsReplaceBlock);
    R(D3D12PIXGetThreadInfo);
    R(D3D12PIXNotifyWakeFromFenceSignal);
    R(D3D12PIXReportCounter);
    R(D3D12SerializeRootSignature);
    R(D3D12SerializeVersionedRootSignature);
    R(GetBehaviorValue);
    R(D3D12CreateDevice);
    R(D3D12GetInterface);
#undef R
    Log(LogLevel::Info, "forwarding to %ls", path.c_str());
    return TRUE;
}
}  // namespace

extern "C" void d3a_resolve_exports() { InitOnceExecuteOnce(&g_once, ResolveOnce, nullptr, nullptr); }

// Exports with real logic. Names are assigned in d3d12.def.
extern "C" HRESULT WINAPI Proxy_D3D12CreateDevice(IUnknown* adapter, D3D_FEATURE_LEVEL fl, REFIID riid, void** ppDevice) {
    d3a_resolve_exports();
    if (!real_D3D12CreateDevice) return E_NOTIMPL;
    HRESULT hr = ((PFN_D3D12_CREATE_DEVICE)real_D3D12CreateDevice)(adapter, fl, riid, ppDevice);
    if (SUCCEEDED(hr) && ppDevice && *ppDevice) OverlayStart();  // first real device: the game is starting up
    if (SUCCEEDED(hr) && ppDevice && *ppDevice && (g_cfg.enabled || g_cfg.measureWhenDisabled)) {
        HookDevice(static_cast<IUnknown*>(*ppDevice));
        ID3D12Device* d = nullptr;
        if (SUCCEEDED(static_cast<IUnknown*>(*ppDevice)->QueryInterface(__uuidof(ID3D12Device), (void**)&d))) {
            HookPresent(d);
            d->Release();
        }
    }
    return hr;
}

extern "C" HRESULT WINAPI Proxy_D3D12GetInterface(REFCLSID clsid, REFIID riid, void** ppv) {
    d3a_resolve_exports();
    if (!real_D3D12GetInterface) return E_NOINTERFACE;
    HRESULT hr = ((PFN_D3D12_GET_INTERFACE)real_D3D12GetInterface)(clsid, riid, ppv);
    if (SUCCEEDED(hr) && ppv && *ppv && (g_cfg.enabled || g_cfg.measureWhenDisabled)) {
        if (clsid == CLSID_D3D12DeviceFactory) HookDeviceFactory(static_cast<IUnknown*>(*ppv));
        else if (clsid == CLSID_D3D12SDKConfiguration) HookSdkConfiguration(static_cast<IUnknown*>(*ppv));
    }
    return hr;
}

// Diagnostics, used by the test suite and for troubleshooting. Not part of the D3D12 API.
#include "async_pso.h"
extern "C" __declspec(dllexport) int WINAPI D3D12Async_PlaceholderState(void* pso) {
    AsyncPSO* a = AsyncPSO::From(pso);
    return a ? (int)a->GetState() : -1;  // -1 = not a placeholder, 0 pending, 1 ready, 2 failed
}
extern "C" __declspec(dllexport) void WINAPI D3D12Async_Configure(double budgetMs, int allThreads, double debugDelayMs, int maxQueue) {
    g_cfg.startupSync = false;  // tests drive the async path directly
    g_cfg.syncUi = false;
    g_cfg.uiMode = 0;
    g_cfg.queueFullPlaceholder = false;
    g_cfg.maxCompilesPerSec = 0;
    g_cfg.loadingSync = false;  // the test suite pauses between frames all the time; [26] turns it on
    g_cfg.maxQueue = maxQueue;
    g_cfg.budgetMs = budgetMs;
    g_cfg.allThreads = allThreads != 0;
    g_cfg.debugDelayMs = debugDelayMs;
}
// Test aid: set the startup-phase and UI-pipeline options directly.
extern "C" __declspec(dllexport) void WINAPI D3D12Async_TestPhase(int startupSync, int syncUi, int minPsos, int quietSeconds, int quietRate) {
    g_cfg.startupSync = startupSync != 0;
    g_cfg.syncUi = syncUi != 0;
    g_cfg.uiMode = syncUi ? 2 : 0;
    g_cfg.startupMinPsos = minPsos;
    g_cfg.startupQuietSeconds = quietSeconds;
    g_cfg.startupQuietRate = quietRate;
    g_cfg.startupCpuBusy = 0;  // test machines' CPU load is not the game's
    g_cfg.startupFloodSeconds = 0;
    g_cfg.startupMinSeconds = 0;
    g_cfg.startupWarmLaunch = false;  // [17b] tests it on its own
    StartupResetForTest();
}
extern "C" __declspec(dllexport) void WINAPI D3D12Async_TestMinSeconds(int seconds) {
    g_cfg.startupMinSeconds = seconds;
    StartupResetForTest();
}
extern "C" __declspec(dllexport) void WINAPI D3D12Async_TestFlood(int floodSeconds, int waitSeconds) {
    g_cfg.startupFloodSeconds = floodSeconds;
    g_cfg.startupFloodWait = waitSeconds;
    StartupResetForTest();
}
extern "C" __declspec(dllexport) void WINAPI D3D12Async_TestFirstFrame() { StartupFirstFrame(); }
// Test aid: warm-launch rule on/off and the slow-compile threshold. Returns 1 if the last phase ended as warm.
extern "C" __declspec(dllexport) int WINAPI D3D12Async_TestWarm(int on, double slowMs) {
    if (on >= 0) { g_cfg.startupWarmLaunch = on != 0; g_cfg.startupSlowMs = slowMs; StartupResetForTest(); }
    return StartupWarm() ? 1 : 0;
}
extern "C" __declspec(dllexport) void WINAPI D3D12Async_TestCpuNoReset(int busyPercent) { g_cfg.startupCpuBusy = busyPercent; }
extern "C" __declspec(dllexport) void WINAPI D3D12Async_TestCpu(int busyPercent) {
    g_cfg.startupCpuBusy = busyPercent;
    StartupResetForTest();
}
extern "C" __declspec(dllexport) void WINAPI D3D12Async_TestStartup(int fpsCap) {
    g_cfg.startupFpsCap = fpsCap;
    StartupResetForTest();
}
extern "C" __declspec(dllexport) void WINAPI D3D12Async_GetActivity(uint64_t out[3]) {
    out[0] = g_stats.psoCreates; out[1] = g_stats.computeCreates; out[2] = g_stats.libraryLoads; out[3] = g_stats.dispatchesSkipped;
}
extern "C" __declspec(dllexport) int WINAPI D3D12Async_LastHitch(double* frameMs, int* kind, int* cause, double* longestMs) {
    Hitch h;
    if (!LastHitch(&h)) return 0;
    *frameMs = h.frameMs; *kind = (int)h.kind; *cause = (int)h.longestCause; *longestMs = h.longestCallMs;
    return (int)HitchCount(false);
}
extern "C" __declspec(dllexport) void WINAPI D3D12Async_TestComputeMode(int mode) { g_cfg.computeMode = mode; }
// Test aid: QueueFullMode / MaxCompilesPerSecond; efficiency != 0 applies WorkerEfficiencyMode to
// the calling thread and returns whether Windows accepted it (-1 if not asked).
extern "C" __declspec(dllexport) int WINAPI D3D12Async_TestQueueMode(int placeholder, double perSecond, int efficiency) {
    g_cfg.queueFullPlaceholder = placeholder != 0;
    g_cfg.maxCompilesPerSec = perSecond;
    return efficiency ? (ApplyEfficiencyMode(true) ? 1 : 0) : -1;
}
// Test aid: loading-screen detection. Returns loading screens counted so far.
extern "C" __declspec(dllexport) int WINAPI D3D12Async_TestLoading(int on, int gapMs, int holdMs) {
    if (on == -1) return LoadingActive() ? 1 : 0;
    if (on == -2) return LoadingGapForTest();
    g_cfg.loadingSync = on != 0;
    g_cfg.loadingGapMs = gapMs;
    g_cfg.loadingHoldMs = holdMs;
    return (int)GetLoadingStats().periods;
}
// Test aid: MaxPlaceholders.
extern "C" __declspec(dllexport) void WINAPI D3D12Async_TestMaxPlaceholders(int n) { g_cfg.maxPlaceholders = n; }
// Test aid: current queue-full placeholder setting (the settings panel's toggle).
extern "C" __declspec(dllexport) int WINAPI D3D12Async_QueueFullPlaceholder() { return g_cfg.queueFullPlaceholder ? 1 : 0; }
// Test aid: the built-in defaults (no ini next to the test).
extern "C" __declspec(dllexport) void WINAPI D3D12Async_Defaults(int* queueFull, int* efficiency, int* compute, int* minSeconds,
                                                                 double* uiWaitMs, int* panel, int* onlyFocused) {
    *queueFull = g_cfg.queueFullPlaceholder ? 1 : 0;
    *efficiency = g_cfg.workerEfficiency ? 1 : 0;
    *compute = g_cfg.computeMode;
    *minSeconds = g_cfg.startupMinSeconds;
    *uiWaitMs = g_cfg.uiWaitMaxMs;
    *panel = g_cfg.settingsPanel ? 1 : 0;
    *onlyFocused = g_cfg.overlayFollowFocus ? 1 : 0;
}
// Test aid: UiWaitMaxMs.
extern "C" __declspec(dllexport) void WINAPI D3D12Async_TestUiWait(double waitMaxMs) { g_cfg.uiWaitMaxMs = waitMaxMs; }
extern "C" __declspec(dllexport) void WINAPI D3D12Async_TestUiMode(int mode) { g_cfg.syncUi = mode != 0; g_cfg.uiMode = mode; }
extern "C" __declspec(dllexport) uint64_t WINAPI D3D12Async_PresentCount() { return PresentCount(); }
extern "C" __declspec(dllexport) int WINAPI D3D12Async_StartupActive() { return StartupActive() ? 1 : 0; }
// Test aid: hook a pipeline library object the test suite made (Wine's D3D12 has none).
extern "C" __declspec(dllexport) void WINAPI D3D12Async_TestHookLibrary(IUnknown* lib) { HookPipelineLibrary(lib); }
extern "C" __declspec(dllexport) void WINAPI D3D12Async_GetStats(uint64_t out[8]) {
    out[0] = g_stats.psoRequests; out[1] = g_stats.psoSync; out[2] = g_stats.psoInBudget; out[3] = g_stats.psoPlaceholders;
    out[4] = g_stats.psoFailed; out[5] = g_stats.drawsSkipped; out[6] = g_stats.forcedWaits; out[7] = g_stats.bgCompileUs;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_self = inst;
        DisableThreadLibraryCalls(inst);
        std::wstring dir = ModuleDir(inst);
        LoadConfig(dir);
        LogInit(dir);
        RunLogSetDir(dir);
        InstallCrashLog();
        wchar_t exe[MAX_PATH];
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        Log(LogLevel::Info, "d3d12async loaded into %ls; enabled=%d budget=%.2f ms threads=%s", exe, (int)g_cfg.enabled,
            g_cfg.budgetMs, g_cfg.allThreads ? "all" : "recording");
    } else if (reason == DLL_PROCESS_DETACH && reserved == nullptr) {
        LogStats("unload");
    }
    return TRUE;
}
