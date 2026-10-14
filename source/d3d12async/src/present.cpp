// Present hook: frame timing for the run logs, and a frame-rate cap that applies only while
// the game's startup precompile is running (fewer frames to render = more CPU for compiling).
//
// The swap chain's vtable lives in dxgi.dll and is shared by every swap chain of the
// process, so it is found once by creating a tiny hidden swap chain and patched in memory.
#include "common.h"

#include <dxgi1_4.h>
#include <mutex>

namespace d3a {
namespace {
using PFN_Present = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
using PFN_Present1 = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);
constexpr unsigned kPresent = 8, kPresent1 = 22;  // IDXGISwapChain::Present, IDXGISwapChain1::Present1 (fixed ABI)

PFN_Present g_origPresent = nullptr;
PFN_Present1 g_origPresent1 = nullptr;
std::atomic<int> g_attempts{0};
std::atomic<bool> g_hooked{false};
std::atomic<bool> g_capping{false};
std::atomic<uint64_t> g_frames{0};
std::mutex g_paceMu;
LARGE_INTEGER g_freq = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return f; }();
int64_t g_last = 0;       // previous present (after pacing), QPC
int64_t g_nextDue = 0;    // cap: earliest time for the next present
HANDLE g_timer = nullptr;

int64_t Qpc() { LARGE_INTEGER t; QueryPerformanceCounter(&t); return t.QuadPart; }

// Loading-screen detection. In gameplay a frame is presented every few ms, and with background
// compiling a pipeline create never holds a frame up. If the game hasn't presented anything for
// LoadingGapMs while it creates pipelines, nothing is on screen to stutter: it's loading (or
// already frozen), so those pipelines compile normally and the loading screen waits for them,
// as without the DLL. After a long gap (2x LoadingGapMs) the same goes for LoadingHoldMs after
// frames resume, for the level fading in.
std::atomic<int64_t> g_lastPresentQpc{0};
std::atomic<int64_t> g_loadingSeenQpc{0};   // last time a create saw a long gap
std::atomic<int64_t> g_holdUntilQpc{0};
std::atomic<bool> g_holdArmed{false};       // a long gap was seen: the hold starts at the next frame
std::atomic<uint64_t> g_loadingSyncAtFrame{0};
std::atomic<uint64_t> g_loadingPeriods{0};
std::atomic<uint64_t> g_loadingUs{0};

void WaitUntil(int64_t due) {
    for (;;) {
        int64_t now = Qpc();
        int64_t left = due - now;
        if (left <= 0) return;
        double ms = left * 1000.0 / g_freq.QuadPart;
        if (ms > 1.5 && g_timer) {  // sleep most of it, spin the last bit for accuracy
            LARGE_INTEGER rel;
            rel.QuadPart = -(LONGLONG)((ms - 1.0) * 10000.0);
            if (SetWaitableTimer(g_timer, &rel, 0, nullptr, nullptr, FALSE)) WaitForSingleObject(g_timer, 100);
            else Sleep(1);
        } else if (ms > 1.5) {
            Sleep(1);
        } else {
            YieldProcessor();
        }
    }
}

// Called before every real present: applies the startup cap, then records the frame time.
void OnPresent(UINT flags) {
    if (flags & DXGI_PRESENT_TEST) return;  // a status query, not a frame
    if (g_frames++ == 0) StartupFirstFrame();
    StartupTick();
    std::lock_guard<std::mutex> lk(g_paceMu);
    int cap = g_cfg.startupFpsCap;
    bool capNow = cap > 0 && StartupActive() && StartupBusyNow();  // no cap while counting down quiet seconds
    g_capping.store(capNow, std::memory_order_relaxed);
    if (capNow) {
        int64_t period = g_freq.QuadPart / cap;
        if (g_nextDue) WaitUntil(g_nextDue);
        int64_t now = Qpc();
        // Next slot one period after this one; resync if we fell far behind (a long frame).
        g_nextDue = (g_nextDue && now - g_nextDue < period) ? g_nextDue + period : now + period;
    } else {
        g_nextDue = 0;
    }
    int64_t now = Qpc();
    if (g_last) {
        double ms = (now - g_last) * 1000.0 / g_freq.QuadPart;
        bool loading = LoadingFrame(ms);
        RunLogFrame(ms, loading);
        TrackFrame(ms, RunSeconds(), loading);
    }
    g_last = now;
    int64_t done = Qpc();
    // LoadingHoldMs counts from the first frame after the loading screen (its normal compiles
    // can take long, so counting from the last create would end the hold too early).
    if (g_holdArmed.exchange(false)) g_holdUntilQpc.store(done + g_freq.QuadPart * g_cfg.loadingHoldMs / 1000, std::memory_order_relaxed);
    g_lastPresentQpc.store(done, std::memory_order_relaxed);
}

HRESULT STDMETHODCALLTYPE Hk_Present(IDXGISwapChain* self, UINT sync, UINT flags) {
    OnPresent(flags);
    return g_origPresent(self, sync, flags);
}
HRESULT STDMETHODCALLTYPE Hk_Present1(IDXGISwapChain1* self, UINT sync, UINT flags, const DXGI_PRESENT_PARAMETERS* p) {
    OnPresent(flags);
    return g_origPresent1(self, sync, flags, p);
}

bool PatchSlot(void** vt, unsigned idx, void* hook, void** orig) {
    if (vt[idx] == hook) return true;
    DWORD old;
    if (!VirtualProtect(&vt[idx], sizeof(void*), PAGE_READWRITE, &old)) return false;
    *orig = vt[idx];
    vt[idx] = hook;
    VirtualProtect(&vt[idx], sizeof(void*), old, &old);
    return true;
}

LRESULT CALLBACK DummyProc(HWND h, UINT m, WPARAM w, LPARAM l) { return DefWindowProcW(h, m, w, l); }
}  // namespace

uint64_t PresentCount() { return g_frames.load(); }

bool LoadingActive() {
    if (!g_cfg.enabled || !g_cfg.loadingSync || g_cfg.loadingGapMs <= 0 || StartupPhase()) return false;
    int64_t last = g_lastPresentQpc.load(std::memory_order_relaxed);
    if (!last) return false;  // no frame yet: the startup phase covers that
    int64_t now = Qpc();
    int64_t gap = now - last, gapTicks = g_freq.QuadPart * g_cfg.loadingGapMs / 1000;
    // No frame for over a minute: rather than a loading screen, the Present hook has probably lost
    // track of the game's frames (another layer replaced the swap chain). Background compiling stays on.
    if (gap > g_freq.QuadPart * 60) {
        static std::atomic<bool> warned{false};
        if (!warned.exchange(true)) Log(LogLevel::Warn, "loading detection: no frame seen for over 60 s; treating it as gameplay");
        return false;
    }
    if (gap >= gapTicks) {
        g_loadingSeenQpc.store(now, std::memory_order_relaxed);
        if (gap >= 2 * gapTicks)  // a real loading screen, not a hitch: keep compiling normally a bit after it
            g_holdArmed.store(true, std::memory_order_relaxed);
        return true;
    }
    return now < g_holdUntilQpc.load(std::memory_order_relaxed);
}

// A frame that took at least LoadingGapMs and had normal (loading) compiles in it is a loading
// screen: left out of the gameplay frame stats and hitches, counted separately.
bool LoadingFrame(double frameMs) {
    uint64_t n = g_stats.psoLoadingSync.load(std::memory_order_relaxed);
    uint64_t prev = g_loadingSyncAtFrame.exchange(n);
    bool compiledInHold = n != prev;
    bool loading = compiledInHold && (frameMs >= g_cfg.loadingGapMs || Qpc() < g_holdUntilQpc.load(std::memory_order_relaxed));
    if (loading && frameMs >= g_cfg.loadingGapMs) {
        g_loadingPeriods++;
        g_loadingUs += (uint64_t)(frameMs * 1000.0);
    }
    return loading;
}

int LoadingGapForTest() {
    int64_t last = g_lastPresentQpc.load();
    return last ? (int)((Qpc() - last) * 1000 / g_freq.QuadPart) : -1;
}
LoadingStats GetLoadingStats() { return {g_loadingPeriods.load(), g_loadingUs.load() / 1e6}; }
bool StartupCapping() { return g_capping.load(std::memory_order_relaxed); }

void HookPresent(ID3D12Device* device) {
    if (g_hooked.load() || g_attempts.fetch_add(1) >= 3) return;
    HMODULE dxgi = LoadLibraryW(L"dxgi.dll");
    auto create = dxgi ? (HRESULT(WINAPI*)(UINT, REFIID, void**))(void*)GetProcAddress(dxgi, "CreateDXGIFactory2") : nullptr;
    if (!create) { Log(LogLevel::Warn, "present hook: dxgi.dll / CreateDXGIFactory2 not available"); return; }

    WNDCLASSW wc{};
    wc.lpfnWndProc = DummyProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"d3d12async_probe";
    RegisterClassW(&wc);
    HWND wnd = CreateWindowExW(0, wc.lpszClassName, L"", WS_OVERLAPPED, 0, 0, 64, 64, nullptr, nullptr, wc.hInstance, nullptr);
    ID3D12CommandQueue* queue = nullptr;
    IDXGIFactory2* factory = nullptr;
    IDXGISwapChain1* sc = nullptr;
    HRESULT hr = E_FAIL;
    D3D12_COMMAND_QUEUE_DESC qd{};
    if (wnd && SUCCEEDED(device->CreateCommandQueue(&qd, __uuidof(ID3D12CommandQueue), (void**)&queue)) &&
        SUCCEEDED(create(0, __uuidof(IDXGIFactory2), (void**)&factory))) {
        DXGI_SWAP_CHAIN_DESC1 d{};
        d.Width = 64;
        d.Height = 64;
        d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        d.SampleDesc.Count = 1;
        d.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        d.BufferCount = 2;
        d.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        hr = factory->CreateSwapChainForHwnd(queue, wnd, &d, nullptr, nullptr, &sc);
    }
    if (sc) {
        void** vt = *reinterpret_cast<void***>(sc);
        std::lock_guard<std::mutex> lk(g_paceMu);
        bool ok = PatchSlot(vt, kPresent, (void*)Hk_Present, (void**)&g_origPresent) &&
                  PatchSlot(vt, kPresent1, (void*)Hk_Present1, (void**)&g_origPresent1);
        g_hooked = ok;
        Log(ok ? LogLevel::Info : LogLevel::Warn, "present hook %s (swap chain vtable %p)", ok ? "installed" : "failed", (void*)vt);
        sc->Release();
    } else {
        Log(LogLevel::Warn, "present hook: probe swap chain failed (hr=0x%08lx); no frame timing or startup cap", (unsigned long)hr);
    }
    if (factory) factory->Release();
    if (queue) queue->Release();
    if (wnd) DestroyWindow(wnd);
    if (!g_timer) g_timer = CreateWaitableTimerExW(nullptr, nullptr, 0x00000002 /*HIGH_RESOLUTION*/, TIMER_ALL_ACCESS);
    if (!g_timer) g_timer = CreateWaitableTimerW(nullptr, TRUE, nullptr);
}

}  // namespace d3a
