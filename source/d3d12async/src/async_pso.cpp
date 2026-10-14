#include <algorithm>
#include "async_pso.h"

#include <chrono>
#include <cstring>
#include <deque>
#include <thread>

namespace d3a {

namespace {
std::atomic<void*> g_vtbl{nullptr};
}

AsyncPSO::AsyncPSO(ID3D12Device* device) : device_(device) {
    device_->AddRef();
    void* v = *reinterpret_cast<void**>(this);
    void* expected = nullptr;
    g_vtbl.compare_exchange_strong(expected, v);
}

AsyncPSO::~AsyncPSO() {
    if (auto* r = real_.load()) r->Release();
    for (auto& it : priv_)
        if (it.iface) it.iface->Release();
    device_->Release();
}

bool AsyncPSO::Is(const void* p) {
    void* v = g_vtbl.load(std::memory_order_relaxed);
    return p && v && *reinterpret_cast<void* const*>(p) == v;
}

void AsyncPSO::Complete(HRESULT hr, ID3D12PipelineState* real, double ms) {
    std::vector<std::function<void(ID3D12PipelineState*)>> later;
    {
    std::lock_guard<std::mutex> lk(mu_);
    hr_ = hr;
    if (SUCCEEDED(hr) && real) {
        // Replay what the game attached to the placeholder before the real PSO existed.
        for (auto& it : priv_) {
            if (it.iface) real->SetPrivateDataInterface(it.guid, it.iface);
            else real->SetPrivateData(it.guid, (UINT)it.data.size(), it.data.data());
        }
        real_.store(real, std::memory_order_release);
        job_.reset();
        state_.store(Ready, std::memory_order_release);
    } else {
        if (real) real->Release();
        job_.reset();
        state_.store(Failed, std::memory_order_release);
        g_stats.psoFailed++;
        Log(LogLevel::Error, "background PSO compile failed hr=0x%08lx after %.1f ms; its draws stay skipped", (unsigned long)hr, ms);
    }
    later.swap(onComplete_);
    cv_.notify_all();
    }
    for (auto& fn : later) fn(Real());  // outside the lock: callbacks call into D3D12
}

void AsyncPSO::OnComplete(std::function<void(ID3D12PipelineState*)> fn) {
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (GetState() == Pending) {
            onComplete_.push_back(std::move(fn));
            return;
        }
    }
    fn(Real());
}

bool AsyncPSO::WaitFor(double ms) {
    if (GetState() != Pending) return true;
    std::unique_lock<std::mutex> lk(mu_);
    return cv_.wait_for(lk, std::chrono::duration<double, std::milli>(ms), [&] { return GetState() != Pending; });
}

ID3D12PipelineState* AsyncPSO::WaitForUse() {
    if (GetState() == Pending) {
        LARGE_INTEGER f, t0, t1;
        QueryPerformanceFrequency(&f);
        QueryPerformanceCounter(&t0);
        if (uiWait_ && g_cfg.uiWaitMaxMs > 0) {
            // UiWaitMaxMs: wait a little (at the front of the queue), then let the draw be skipped
            // rather than freeze the frame for the whole compile.
            Boost();
            if (!WaitFor(g_cfg.uiWaitMaxMs)) g_stats.uiWaitTimeouts++;
        } else {
            std::shared_ptr<PoolJob> j;
            {
                std::lock_guard<std::mutex> lk(mu_);
                j = job_;
            }
            if (!j || !PoolRunInline(j)) {  // a worker already has it: wait
                Boost();
                std::unique_lock<std::mutex> lk(mu_);
                cv_.wait(lk, [&] { return GetState() != Pending; });
            }
        }
        QueryPerformanceCounter(&t1);
        g_stats.uiWaits++;
        TrackCall((uint64_t)((t1.QuadPart - t0.QuadPart) * 1000000 / f.QuadPart), CallCause::UiWait, OnRecordingThread());
    }
    return Real();
}

ID3D12PipelineState* AsyncPSO::WaitReal(WaitReason why, void* caller, const char* detail) {
    if (GetState() == Pending) {
        g_stats.forcedWaits++;
        NoteForcedWait(why, caller, detail);
        Boost();  // don't wait behind the whole queue
        LARGE_INTEGER f, t0, t1;
        QueryPerformanceFrequency(&f);
        QueryPerformanceCounter(&t0);
        {
            std::unique_lock<std::mutex> lk(mu_);
            cv_.wait(lk, [&] { return GetState() != Pending; });
        }
        QueryPerformanceCounter(&t1);
        TrackCall((uint64_t)((t1.QuadPart - t0.QuadPart) * 1000000 / f.QuadPart), CallCause::ForcedWait, OnRecordingThread());
    }
    return Real();
}

void AsyncPSO::Boost() {
    std::shared_ptr<PoolJob> j;
    {
        std::lock_guard<std::mutex> lk(mu_);
        j = std::move(job_);  // boost once
    }
    if (j && !j->started.load()) PoolBoost(j);
}

bool AsyncPSO::SupportsIID(REFIID riid) const {
    return riid == __uuidof(IUnknown) || riid == __uuidof(ID3D12Object) || riid == __uuidof(ID3D12DeviceChild) ||
           riid == __uuidof(ID3D12Pageable) || riid == __uuidof(ID3D12PipelineState);
}

HRESULT AsyncPSO::QueryInterface(REFIID riid, void** ppv) {
    if (!ppv) return E_POINTER;
    if (SupportsIID(riid)) {
        AddRef();
        *ppv = static_cast<ID3D12PipelineState*>(this);
        return S_OK;
    }
    // Anything else (a newer interface, a debug interface): answer from the real object.
    char iid[64];
    snprintf(iid, sizeof iid, "{%08lX-%04X-%04X-%02X%02X-...}", (unsigned long)riid.Data1, riid.Data2, riid.Data3, riid.Data4[0], riid.Data4[1]);
    if (auto* r = WaitReal(WaitQueryInterface, __builtin_return_address(0), iid)) return r->QueryInterface(riid, ppv);
    *ppv = nullptr;
    return E_NOINTERFACE;
}

ULONG AsyncPSO::AddRef() { return ++refs_; }

ULONG AsyncPSO::Release() {
    ULONG n = --refs_;
    if (n == 0) delete this;
    return n;
}

AsyncPSO::PrivateItem* AsyncPSO::FindItem(REFGUID g) {
    for (auto& it : priv_)
        if (it.guid == g) return &it;
    return nullptr;
}

HRESULT AsyncPSO::GetPrivateData(REFGUID guid, UINT* pDataSize, void* pData) {
    std::lock_guard<std::mutex> lk(mu_);
    if (auto* r = Real()) return r->GetPrivateData(guid, pDataSize, pData);
    if (!pDataSize) return E_INVALIDARG;
    PrivateItem* it = FindItem(guid);
    if (!it) { *pDataSize = 0; return DXGI_ERROR_NOT_FOUND; }
    if (it->iface) {
        if (!pData) { *pDataSize = sizeof(IUnknown*); return S_OK; }
        if (*pDataSize < sizeof(IUnknown*)) return DXGI_ERROR_MORE_DATA;
        it->iface->AddRef();
        *reinterpret_cast<IUnknown**>(pData) = it->iface;
        *pDataSize = sizeof(IUnknown*);
        return S_OK;
    }
    UINT n = (UINT)it->data.size();
    if (!pData) { *pDataSize = n; return S_OK; }
    if (*pDataSize < n) return DXGI_ERROR_MORE_DATA;
    std::memcpy(pData, it->data.data(), n);
    *pDataSize = n;
    return S_OK;
}

// Interfaces displaced from the store are released after the lock is dropped: their
// destructors may call back into this object.
HRESULT AsyncPSO::SetPrivateData(REFGUID guid, UINT DataSize, const void* pData) {
    IUnknown* old = nullptr;
    HRESULT hr = S_OK;
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (auto* r = Real()) return r->SetPrivateData(guid, DataSize, pData);
        PrivateItem* it = FindItem(guid);
        if (!pData || !DataSize) {  // removal
            if (it) {
                old = it->iface;
                *it = priv_.back();
                priv_.pop_back();
            }
        } else {
            if (!it) { priv_.push_back({guid, {}, nullptr}); it = &priv_.back(); }
            old = it->iface;
            it->iface = nullptr;
            it->data.assign((const uint8_t*)pData, (const uint8_t*)pData + DataSize);
        }
    }
    if (old) old->Release();
    return hr;
}

HRESULT AsyncPSO::SetPrivateDataInterface(REFGUID guid, const IUnknown* pData) {
    if (!pData) return SetPrivateData(guid, 0, nullptr);  // null removes, as natively
    IUnknown* old = nullptr;
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (auto* r = Real()) return r->SetPrivateDataInterface(guid, pData);
        PrivateItem* it = FindItem(guid);
        if (!it) { priv_.push_back({guid, {}, nullptr}); it = &priv_.back(); }
        old = it->iface;
        it->data.clear();
        it->iface = const_cast<IUnknown*>(pData);
        it->iface->AddRef();
    }
    if (old) old->Release();
    return S_OK;
}

HRESULT AsyncPSO::SetName(LPCWSTR Name) {
    // D3D12 stores debug names as WKPDID_D3DDebugObjectNameW private data.
    static const GUID kName = {0x4cca5fd8, 0x921f, 0x42c8, {0x85, 0x66, 0x70, 0xca, 0xf2, 0xa9, 0xb7, 0x41}};
    if (auto* r = Real()) return r->SetName(Name);
    if (!Name) return SetPrivateData(kName, 0, nullptr);
    return SetPrivateData(kName, (UINT)((wcslen(Name) + 1) * sizeof(wchar_t)), Name);
}

HRESULT AsyncPSO::GetDevice(REFIID riid, void** ppvDevice) { return device_->QueryInterface(riid, ppvDevice); }

HRESULT AsyncPSO::GetCachedBlob(ID3DBlob** ppBlob) {
    if (auto* r = WaitReal(WaitCachedBlob, __builtin_return_address(0))) return r->GetCachedBlob(ppBlob);
    return E_FAIL;
}

// ---------------------------------------------------------------------------------
// Worker pool

namespace {
std::mutex g_poolMu;
std::condition_variable g_poolCv;
std::deque<std::shared_ptr<PoolJob>> g_jobs, g_urgent;
size_t g_queued = 0;  // jobs submitted and not yet started
std::atomic<bool> g_poolStarted{false};

DWORD_PTR g_workerMask = 0;  // 0 = no restriction

// "all" / "" -> no restriction; "upper-half" -> the last half of the logical CPUs;
// otherwise a list like "8-15" or "8,10,12-15" (logical CPU numbers, processor group 0).
DWORD_PTR ParseCpus(const std::wstring& spec, int* count) {
    DWORD_PTR proc = 0, sys = 0;
    GetProcessAffinityMask(GetCurrentProcess(), &proc, &sys);
    int n = 0;
    for (DWORD_PTR m = proc; m; m >>= 1) ++n;  // highest allowed CPU + 1
    DWORD_PTR mask = 0;
    if (spec.empty() || _wcsicmp(spec.c_str(), L"all") == 0) return 0;
    if (_wcsicmp(spec.c_str(), L"upper-half") == 0) {
        for (int i = n / 2; i < n; ++i) mask |= (DWORD_PTR)1 << i;
    } else {
        const wchar_t* p = spec.c_str();
        while (*p) {
            wchar_t* end;
            long a = wcstol(p, &end, 10);
            if (end == p) { ++p; continue; }
            long b = a;
            p = end;
            if (*p == L'-') { b = wcstol(p + 1, &end, 10); p = end; }
            for (long i = a; i <= b && i < (long)(sizeof(DWORD_PTR) * 8); ++i)
                if (i >= 0) mask |= (DWORD_PTR)1 << i;
            while (*p == L',' || *p == L' ') ++p;
        }
    }
    mask &= proc;  // never ask for CPUs the process can't use
    *count = 0;
    for (DWORD_PTR m = mask; m; m &= m - 1) ++*count;
    return mask;
}

int g_baseWorkers = 0;  // workers with index >= this only take jobs while the queue is long

// MaxCompilesPerSecond: the earliest time the next queued (non-urgent) compile may start.
std::chrono::steady_clock::time_point g_nextStart{};

}  // namespace

// WorkerEfficiencyMode: Windows 11 efficiency mode (EcoQoS) - the scheduler prefers efficiency
// cores and lower clocks for the thread - plus low memory priority. CPU priority is left as is
// (below-normal) so the workers can't be starved outright while the game is busy.
// Returns true if efficiency mode was applied.
bool ApplyEfficiencyMode(bool on) {
    using SetTI = BOOL(WINAPI*)(HANDLE, int, LPVOID, DWORD);
    auto f = (SetTI)(void*)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "SetThreadInformation");
    if (!f) return false;
    // THREAD_POWER_THROTTLING_EXECUTION_SPEED: on = EcoQoS, off = explicitly high QoS.
    struct { ULONG version, controlMask, stateMask; } power{1, 1, on ? 1u : 0u};
    bool ok = f(GetCurrentThread(), 3 /*ThreadPowerThrottling*/, &power, sizeof power) != 0;
    struct { ULONG priority; } mem{on ? 2u : 5u};  // MEMORY_PRIORITY_LOW / MEMORY_PRIORITY_NORMAL
    f(GetCurrentThread(), 0 /*ThreadMemoryPriority*/, &mem, sizeof mem);
    return ok;
}

namespace {

void WorkerMain(int index) {
    if (g_cfg.lowPriority) SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    if (g_workerMask) SetThreadAffinityMask(GetCurrentThread(), g_workerMask);
    using SetDesc = HRESULT(WINAPI*)(HANDLE, PCWSTR);
    if (auto f = (SetDesc)(void*)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "SetThreadDescription")) {
        wchar_t name[64];
        swprintf(name, 64, L"d3d12async compile %d", index);
        f(GetCurrentThread(), name);
    }
    // The experimental options (efficiency mode, rate limit) never apply during the startup
    // precompile: the startup phase keeps its stock, full-speed behaviour.
    bool eco = false;
    auto syncEco = [&] {
        bool wantEco = g_cfg.workerEfficiency && !StartupPhase();
        if (wantEco == eco) return;
        bool ok = ApplyEfficiencyMode(wantEco);
        eco = wantEco;
        static std::atomic<bool> logged{false};
        if (wantEco && !logged.exchange(true))
            Log(LogLevel::Info, "compile pool: efficiency mode (EcoQoS, low memory priority) %s after the startup phase",
                ok ? "on" : "not available on this Windows version");
    };
    for (;;) {
        std::shared_ptr<PoolJob> job;
        {
            std::unique_lock<std::mutex> lk(g_poolMu);
            bool burst = index >= g_baseWorkers;
            g_poolCv.wait(lk, [&] {
                if (!burst) return !g_jobs.empty() || !g_urgent.empty();
                return !g_urgent.empty() || g_queued > (size_t)std::max(0, g_cfg.burstQueue);
            });
            // Urgent jobs (a game thread is waiting on them) are never rate-limited.
            if (g_urgent.empty() && g_cfg.maxCompilesPerSec > 0 && !StartupPhase()) {
                auto now = std::chrono::steady_clock::now();
                if (now < g_nextStart) {
                    auto until = g_nextStart;
                    g_poolCv.wait_until(lk, until);
                    auto waited = std::chrono::steady_clock::now() - now;
                    g_stats.rateLimitedUs.fetch_add(
                        (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(waited).count(),
                        std::memory_order_relaxed);
                    continue;  // re-check: an urgent job may have arrived, or another worker took the slot
                }
                auto interval = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                    std::chrono::duration<double>(1.0 / g_cfg.maxCompilesPerSec));
                g_nextStart = std::max(now, g_nextStart) + interval;
            }
            auto& q = g_urgent.empty() ? g_jobs : g_urgent;
            job = std::move(q.front());
            q.pop_front();
            if (job->started.exchange(true)) continue;  // already taken from the other queue
            --g_queued;
        }
        syncEco();
        job->fn();
    }
}
}  // namespace

void PoolStart() {
    bool expected = false;
    if (!g_poolStarted.compare_exchange_strong(expected, true)) return;
    // Workers run code from this DLL for the rest of the process: never let it unload.
    HMODULE self;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                       reinterpret_cast<LPCWSTR>(&PoolStart), &self);
    int n = g_cfg.workers;
    if (n <= 0) {
        int hw = (int)std::thread::hardware_concurrency();
        n = hw / 4;  // leave most of the CPU to the game itself
        if (n < 2) n = 2;
        if (n > 8) n = 8;
    }
    int cpus = 0;
    g_workerMask = ParseCpus(g_cfg.workerCpus, &cpus);
    if (g_cfg.workers <= 0 && cpus > 0 && n > cpus) n = cpus;  // no more auto workers than allowed CPUs
    int total = n;
    if (g_cfg.burstWorkers < 0) {
        int allowed = cpus > 0 ? cpus : (int)std::thread::hardware_concurrency() / 2;
        total = std::max(n, std::min(allowed, 8));
    } else {
        total = n + g_cfg.burstWorkers;
    }
    g_baseWorkers = n;
    for (int i = 0; i < total; ++i) std::thread(WorkerMain, i).detach();
    if (total > n) Log(LogLevel::Info, "compile pool: %d more workers join while over %d compiles are waiting", total - n, g_cfg.burstQueue);
    if (g_workerMask)
        Log(LogLevel::Info, "compile pool: %d workers (%s priority) on CPUs mask 0x%llx (%ls)", n,
            g_cfg.lowPriority ? "below-normal" : "normal", (unsigned long long)g_workerMask, g_cfg.workerCpus.c_str());
    else
        Log(LogLevel::Info, "compile pool: %d workers (%s priority), any CPU", n, g_cfg.lowPriority ? "below-normal" : "normal");
}

void PoolSubmit(const std::shared_ptr<PoolJob>& job) {
    {
        std::lock_guard<std::mutex> lk(g_poolMu);
        g_jobs.push_back(job);
        ++g_queued;
        if (!StartupPhase()) {
            uint64_t peak = g_stats.queuePeak.load(std::memory_order_relaxed);
            if (g_queued > peak) g_stats.queuePeak.store(g_queued, std::memory_order_relaxed);
        }
    }
    if (g_queued > (size_t)std::max(0, g_cfg.burstQueue)) g_poolCv.notify_all();  // let burst workers join
    else g_poolCv.notify_one();
}

void PoolBoost(const std::shared_ptr<PoolJob>& job) {
    {
        std::lock_guard<std::mutex> lk(g_poolMu);
        g_urgent.push_back(job);
    }
    g_poolCv.notify_one();
}

bool PoolRunInline(const std::shared_ptr<PoolJob>& job) {
    if (job->started.exchange(true)) return false;
    {
        std::lock_guard<std::mutex> lk(g_poolMu);
        --g_queued;  // its queue entries are skipped by the workers (started is set)
    }
    job->fn();
    return true;
}

int PoolBaseWorkers() { return g_baseWorkers; }

size_t PoolPending() {
    std::lock_guard<std::mutex> lk(g_poolMu);
    return g_queued;
}

}  // namespace d3a
