// Vtable hooks on the game's device, command lists and pipeline libraries.
//
// Patching is done in memory only, on the vtables of objects the game created through
// this DLL. Each hooked slot remembers the original function per vtable, because the
// debug layer and other wrappers bring their own vtables with different originals.
#include "async_pso.h"
#include "desc_copy.h"

#include <algorithm>
#include <mutex>
#include <unordered_set>
#include <vector>

extern "C" {
extern const unsigned kDev_CreateGraphicsPipelineState, kDev_CreateCommandList, kDev_MakeResident, kDev_Evict,
    kDev1_CreatePipelineLibrary, kDev1_SetResidencyPriority, kDev2_CreatePipelineState, kDev3_EnqueueMakeResident, kDev4_CreateCommandList1;
extern const unsigned kCl_Close, kCl_Reset, kCl_ClearState, kCl_DrawInstanced, kCl_DrawIndexedInstanced,
    kCl_SetPipelineState, kCl_ExecuteIndirect, kCl_ExecuteBundle, kCl_Dispatch, kCl4_SetPipelineState1, kCl6_DispatchMesh;
extern const unsigned kLib_StorePipeline, kLib_LoadGraphicsPipeline, kLib_LoadComputePipeline, kLib1_LoadPipeline,
    kDev_CreateComputePipelineState, kFactory_CreateDevice, kSdkCfg1_CreateDeviceFactory;
}

namespace d3a {

// ---------------------------------------------------------------------------------
// Vtable patching

// One lock for all patching: hooks on the same vtable share a memory page, and
// VirtualProtect/restore sequences must not interleave.
std::mutex g_patchMu;

template <typename Fn>
class Slot {
public:
    static constexpr int kMax = 16;
    Fn Find(const void* obj) {
        void* vt = *reinterpret_cast<void* const*>(obj);
        int n = count_.load(std::memory_order_acquire);
        for (int i = 0; i < n; ++i)
            if (vt_[i] == vt) return orig_[i];
        return Adopt(reinterpret_cast<void**>(vt));
    }

    // A vtable we never patched reached this hook: another layer (overlay, Streamline, a
    // vtable-swapping hook) copied a patched vtable, our hook pointer included. Adopt it,
    // using the original from the registered vtable it resembles most.
    Fn Adopt(void** vt) {
        std::lock_guard<std::mutex> lk(g_patchMu);
        int n = count_.load();
        for (int i = 0; i < n; ++i)
            if (vt_[i] == vt) return orig_[i];
        if (n == 0) return nullptr;
        int best = 0, bestScore = -1;
        for (int i = 0; i < n; ++i) {
            void** reg = reinterpret_cast<void**>(vt_[i]);
            int score = 0;
            for (unsigned k = 3; k < 40; ++k)
                if (k != index_ && !IsBadReadPtr(&vt[k], sizeof(void*)) && vt[k] == reg[k]) ++score;
            if (score > bestScore) { bestScore = score; best = i; }
        }
        Fn o = orig_[best];
        if (n < kMax) {
            vt_[n] = vt;
            orig_[n] = o;
            count_.store(n + 1, std::memory_order_release);
        }
        Log(LogLevel::Warn, "%s: foreign vtable %p (a copy made by another layer) adopted; original taken from %p (%d matching slots)",
            name_ ? name_ : "?", (void*)vt, vt_[best], bestScore);
        return o;
    }

    void Patch(void* obj, unsigned index, Fn hook, const char* name) {
        std::lock_guard<std::mutex> lk(g_patchMu);
        void** vt = *reinterpret_cast<void***>(obj);
        int n = count_.load();
        for (int i = 0; i < n; ++i)
            if (vt_[i] == vt) return;  // already patched
        if (vt[index] == reinterpret_cast<void*>(hook)) return;
        if (n == kMax) {
            Log(LogLevel::Warn, "too many vtables for %s; new ones stay unhooked", name);
            return;
        }
        orig_[n] = reinterpret_cast<Fn>(vt[index]);
        vt_[n] = vt;
        index_ = index;
        name_ = name;
        count_.store(n + 1, std::memory_order_release);  // publish before the slot can be reached
        DWORD old;
        if (!VirtualProtect(&vt[index], sizeof(void*), PAGE_READWRITE, &old)) {
            Log(LogLevel::Error, "VirtualProtect failed for %s", name);
            return;
        }
        vt[index] = reinterpret_cast<void*>(hook);
        VirtualProtect(&vt[index], sizeof(void*), old, &old);
        Log(LogLevel::Debug, "hooked %s (vtable %p slot %u)", name, (void*)vt, index);
    }

private:
    void* vt_[kMax] = {};
    Fn orig_[kMax] = {};
    std::atomic<int> count_{0};
    unsigned index_ = 0;
    const char* name_ = nullptr;
};

#define ORIG(slot, self) slot.Find(self)

// ---------------------------------------------------------------------------------
// Hook groups: all hooked slots of one interface family, tracked per vtable.
//
// Some drivers and layers give objects private vtable copies (NVIDIA's driver does this
// for command lists, with entries pointing straight into the driver). Any vtable a hook
// sees for the first time is therefore hooked in full right away ("patch on discovery"),
// with originals taken from the vtable itself or, for slots that already hold our hook
// (a copy of a patched vtable), from the known vtable it resembles most.

std::atomic<bool> g_asyncAllowed{true};  // cleared if vtable tracking ever overflows

class HookGroup {
public:
    static constexpr int kMaxSlots = 16;
    static constexpr int kSnap = 48;   // slots compared when matching a copy to its source
    static constexpr int kMaxProtos = 32;

    HookGroup(const char* name, size_t capacity) : name_(name), cap_(capacity), table_(new Entry[capacity]) {}

    int Add(unsigned index, void* hook, const char* name) {
        defs_[n_] = {index, hook, name};
        return n_++;
    }

    // Original function for slot `k` of obj's vtable (discovering the vtable if new).
    void* Orig(const void* obj, int k) {
        void** vt = *reinterpret_cast<void** const*>(obj);
        if (Entry* e = Lookup(vt)) return e->orig[k];
        Entry* e = Discover(vt);
        return e ? e->orig[k] : fallback_[k];
    }

    // Make sure every slot of obj's current vtable is hooked (cheap when it already is).
    void Ensure(const void* obj) {
        void** vt = *reinterpret_cast<void** const*>(obj);
        Entry* e = Lookup(vt);
        if (e) {
            bool all = true;
            for (int k = 0; k < n_ && all; ++k) all = vt[defs_[k].index] == defs_[k].hook;
            if (all) return;
        }
        Discover(vt);  // new, or an address reused by a fresh copy: (re)hook it
    }

private:
    struct SlotDef { unsigned index; void* hook; const char* name; };
    struct Entry {
        std::atomic<void**> vt{nullptr};
        void* orig[kMaxSlots] = {};
    };
    struct Proto {
        void* snap[kSnap];
        void* orig[kMaxSlots];
    };

    size_t Hash(void** vt) const { return (reinterpret_cast<uintptr_t>(vt) >> 3) * 0x9E3779B97F4A7C15ull >> 20; }

    Entry* Lookup(void** vt) {
        size_t mask = cap_ - 1, i = Hash(vt) & mask;
        for (size_t probe = 0; probe < cap_; ++probe, i = (i + 1) & mask) {
            void** cur = table_[i].vt.load(std::memory_order_acquire);
            if (cur == vt) return &table_[i];
            if (!cur) return nullptr;
        }
        return nullptr;
    }

    static bool Readable(void* p) { return !IsBadReadPtr(p, sizeof(void*)); }

    Entry* Discover(void** vt) {
        std::lock_guard<std::mutex> lk(g_patchMu);
        Entry* e = Lookup(vt);
        void* orig[kMaxSlots];
        bool copied = false;
        for (int k = 0; k < n_; ++k) {
            void* cur = vt[defs_[k].index];
            if (cur == defs_[k].hook) {
                copied = true;
                orig[k] = e ? e->orig[k] : nullptr;
            } else {
                orig[k] = cur;  // whatever the game's call would have reached: runtime or driver
            }
        }
        if (copied && !e) {  // a copy of a patched vtable: borrow originals from the closest prototype
            int best = -1, bestScore = -1;
            for (int p = 0; p < nprotos_; ++p) {
                int score = 0;
                for (int j = 3; j < kSnap; ++j)
                    if (Readable(&vt[j]) && vt[j] == protos_[p].snap[j]) ++score;
                if (score > bestScore) { bestScore = score; best = p; }
            }
            for (int k = 0; k < n_; ++k)
                if (vt[defs_[k].index] == defs_[k].hook) orig[k] = best >= 0 ? protos_[best].orig[k] : fallback_[k];
            Log(LogLevel::Info, "%s: vtable %p is a copy of a hooked one; originals from prototype %d (%d/%d slots match)", name_,
                (void*)vt, best, bestScore, kSnap - 3);
        }
        for (int k = 0; k < n_; ++k)
            if (!orig[k]) {
                Log(LogLevel::Error, "%s: no original for %s in vtable %p; disabling placeholders", name_, defs_[k].name, (void*)vt);
                g_asyncAllowed = false;
                return nullptr;
            }
        if (!copied && nprotos_ < kMaxProtos) {  // a source vtable: remember it as a prototype
            bool dup = false;
            for (int p = 0; p < nprotos_ && !dup; ++p) dup = !memcmp(protos_[p].orig, orig, sizeof(void*) * n_);
            if (!dup) {
                Proto& pr = protos_[nprotos_++];
                for (int j = 0; j < kSnap; ++j) pr.snap[j] = Readable(&vt[j]) ? vt[j] : nullptr;
                memcpy(pr.orig, orig, sizeof(void*) * n_);
                for (int k = 0; k < n_; ++k)
                    if (!fallback_[k]) fallback_[k] = orig[k];
            }
        }
        if (!e) {
            if (used_ * 2 >= cap_) {
                Log(LogLevel::Error, "%s: too many vtables (%zu); disabling placeholders for safety", name_, used_);
                g_asyncAllowed = false;
            } else {
                size_t mask = cap_ - 1, i = Hash(vt) & mask;
                while (table_[i].vt.load()) i = (i + 1) & mask;
                e = &table_[i];
                ++used_;
            }
        }
        if (e) {
            memcpy(e->orig, orig, sizeof(void*) * n_);
            e->vt.store(vt, std::memory_order_release);  // publish before the slots point to us
        }
        // Hook every slot (a write per pointer; heap copies are already writable).
        for (int k = 0; k < n_; ++k) {
            void** slot = &vt[defs_[k].index];
            if (*slot == defs_[k].hook) continue;
            if (!e) break;  // untracked: leave it alone rather than hook without an original
            DWORD old;
            if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) {
                Log(LogLevel::Error, "%s: VirtualProtect failed on %p; disabling placeholders", name_, (void*)vt);
                g_asyncAllowed = false;
                continue;
            }
            *slot = defs_[k].hook;
            VirtualProtect(slot, sizeof(void*), old, &old);
        }
        Log(LogLevel::Debug, "%s: hooked vtable %p (%d slots%s)", name_, (void*)vt, n_, copied ? ", copy" : "");
        return e;
    }

    const char* name_;
    size_t cap_;
    std::unique_ptr<Entry[]> table_;
    size_t used_ = 0;
    SlotDef defs_[kMaxSlots];
    int n_ = 0;
    Proto protos_[kMaxProtos];
    int nprotos_ = 0;
    void* fallback_[kMaxSlots] = {};
};


template <typename Fn>
struct GSlot {
    HookGroup* g = nullptr;
    int k = -1;
    Fn Find(const void* obj) { return reinterpret_cast<Fn>(g->Orig(obj, k)); }
};

HookGroup g_clGroup("command list", 1 << 15);
HookGroup g_devGroup("device", 1 << 8);
std::once_flag g_clInit, g_devInit;

// ---------------------------------------------------------------------------------
// Per-command-list "skipping draws" state

namespace {

struct SkipSet {
    static constexpr int kShards = 32;
    struct Shard {
        SRWLOCK lock = SRWLOCK_INIT;
        std::unordered_set<const void*> lists;
    } shard[kShards];
    std::atomic<int> active{0};

    Shard& For(const void* p) { return shard[(reinterpret_cast<uintptr_t>(p) >> 6) % kShards]; }
    void Set(const void* cl, bool on) {
        if (!on && active.load(std::memory_order_relaxed) == 0) return;
        Shard& s = For(cl);
        AcquireSRWLockExclusive(&s.lock);
        if (on) {
            if (s.lists.insert(cl).second) active++;
        } else if (s.lists.erase(cl)) {
            active--;
        }
        ReleaseSRWLockExclusive(&s.lock);
    }
    bool Has(const void* cl) {
        if (active.load(std::memory_order_relaxed) == 0) return false;  // the common case: nothing compiling
        Shard& s = For(cl);
        AcquireSRWLockShared(&s.lock);
        bool r = s.lists.count(cl) != 0;
        ReleaseSRWLockShared(&s.lock);
        return r;
    }
} g_skip;

// Threads that record command lists are the ones a frame waits on. Placeholders are only
// handed out there (unless AsyncThreads=all), so engine precache threads keep their
// normal blocking behaviour and loading screens still wait for real compiles.
thread_local bool t_recorder = false;

// Resolve a PSO argument before it reaches the real runtime.
// Returns the PSO to pass on; `notReady` is set when a placeholder is still compiling.
ID3D12PipelineState* Resolve(ID3D12PipelineState* pso, bool wait, bool* notReady, WaitReason why = WaitOther,
                             void* caller = nullptr) {
    if (notReady) *notReady = false;
    AsyncPSO* a = AsyncPSO::From(pso);
    if (!a) return pso;
    if (auto* r = a->Real()) return r;
    if (wait) return a->WaitReal(why, caller);
    if (a->WaitOnUse()) {
        if (auto* r = a->WaitForUse()) return r;
        if (notReady) *notReady = true;  // a UI wait ran out of UiWaitMaxMs (or the compile failed)
        return nullptr;
    }
    a->Boost();
    if (notReady) *notReady = true;
    return nullptr;
}

// ---------------------------------------------------------------------------------
// Command list hooks

using PFN_Close = HRESULT(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*);
using PFN_Reset = HRESULT(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, ID3D12CommandAllocator*, ID3D12PipelineState*);
using PFN_ClearState = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, ID3D12PipelineState*);
using PFN_DrawInstanced = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, UINT, UINT, UINT);
using PFN_DrawIndexedInstanced = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, UINT, UINT, INT, UINT);
using PFN_SetPipelineState = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, ID3D12PipelineState*);
using PFN_ExecuteIndirect = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, ID3D12CommandSignature*, UINT,
                                                     ID3D12Resource*, UINT64, ID3D12Resource*, UINT64);
using PFN_ExecuteBundle = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, ID3D12GraphicsCommandList*);
using PFN_SetPipelineState1 = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList4*, ID3D12StateObject*);
using PFN_DispatchMesh = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList6*, UINT, UINT, UINT);
using PFN_Dispatch = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, UINT, UINT);

GSlot<PFN_Close> s_Close;
GSlot<PFN_Reset> s_Reset;
GSlot<PFN_ClearState> s_ClearState;
GSlot<PFN_DrawInstanced> s_DrawInstanced;
GSlot<PFN_DrawIndexedInstanced> s_DrawIndexedInstanced;
GSlot<PFN_SetPipelineState> s_SetPipelineState;
GSlot<PFN_ExecuteIndirect> s_ExecuteIndirect;
GSlot<PFN_ExecuteBundle> s_ExecuteBundle;
GSlot<PFN_SetPipelineState1> s_SetPipelineState1;
GSlot<PFN_DispatchMesh> s_DispatchMesh;
GSlot<PFN_Dispatch> s_Dispatch;

inline void CountSkip() { g_stats.drawsSkipped.fetch_add(1, std::memory_order_relaxed); }

HRESULT STDMETHODCALLTYPE Hk_Close(ID3D12GraphicsCommandList* self) {
    StartupTick();
    g_skip.Set(self, false);
    HRESULT hr = ORIG(s_Close, self)(self);
    g_clGroup.Ensure(self);
    return hr;
}

HRESULT STDMETHODCALLTYPE Hk_Reset(ID3D12GraphicsCommandList* self, ID3D12CommandAllocator* alloc, ID3D12PipelineState* pso) {
    t_recorder = true;
    bool notReady;
    bool bundle = AsyncPSO::Is(pso) && self->GetType() == D3D12_COMMAND_LIST_TYPE_BUNDLE;  // replayed later: never skip
    ID3D12PipelineState* use = Resolve(pso, bundle, &notReady, WaitBundle, __builtin_return_address(0));
    g_skip.Set(self, notReady);
    g_clGroup.Ensure(self);
    HRESULT hr = ORIG(s_Reset, self)(self, alloc, use);
    g_clGroup.Ensure(self);  // the driver may hand the list a fresh private vtable here
    return hr;
}

void STDMETHODCALLTYPE Hk_ClearState(ID3D12GraphicsCommandList* self, ID3D12PipelineState* pso) {
    bool notReady;
    ID3D12PipelineState* use = Resolve(pso, false, &notReady);
    g_skip.Set(self, notReady);
    ORIG(s_ClearState, self)(self, use);
}

void STDMETHODCALLTYPE Hk_SetPipelineState(ID3D12GraphicsCommandList* self, ID3D12PipelineState* pso) {
    AsyncPSO* a = AsyncPSO::From(pso);
    if (!a) {
        g_skip.Set(self, false);
        ORIG(s_SetPipelineState, self)(self, pso);
        return;
    }
    ID3D12PipelineState* real = a->Real();
    if (!real && a->GetState() == AsyncPSO::Pending && a->WaitOnUse()) real = a->WaitForUse();  // UI: waits (at most UiWaitMaxMs if set)
    if (!real && a->GetState() == AsyncPSO::Pending && self->GetType() == D3D12_COMMAND_LIST_TYPE_BUNDLE) {
        // A bundle is recorded once and replayed later: skipping here would lose the draws for good.
        real = a->WaitReal(WaitBundle, __builtin_return_address(0));
    }
    if (real) {
        g_skip.Set(self, false);
        ORIG(s_SetPipelineState, self)(self, real);
    } else {
        // Still compiling (or failed): keep the previous state bound and drop draws until
        // the game binds another pipeline. The compile moves to the front of the queue.
        a->Boost();
        g_skip.Set(self, true);
    }
}

void STDMETHODCALLTYPE Hk_DrawInstanced(ID3D12GraphicsCommandList* self, UINT a, UINT b, UINT c, UINT d) {
    if (g_skip.Has(self)) return CountSkip();
    ORIG(s_DrawInstanced, self)(self, a, b, c, d);
}

void STDMETHODCALLTYPE Hk_DrawIndexedInstanced(ID3D12GraphicsCommandList* self, UINT a, UINT b, UINT c, INT d, UINT e) {
    if (g_skip.Has(self)) return CountSkip();
    ORIG(s_DrawIndexedInstanced, self)(self, a, b, c, d, e);
}

void STDMETHODCALLTYPE Hk_ExecuteIndirect(ID3D12GraphicsCommandList* self, ID3D12CommandSignature* sig, UINT maxCount,
                                          ID3D12Resource* args, UINT64 argsOff, ID3D12Resource* count, UINT64 countOff) {
    if (g_skip.Has(self)) return CountSkip();
    ORIG(s_ExecuteIndirect, self)(self, sig, maxCount, args, argsOff, count, countOff);
}

// A bundle inherits the caller's pipeline; while that pipeline is still compiling, the
// previous one is what's actually bound, so the bundle must not run either.
void STDMETHODCALLTYPE Hk_ExecuteBundle(ID3D12GraphicsCommandList* self, ID3D12GraphicsCommandList* bundle) {
    if (g_skip.Has(self)) return CountSkip();
    ORIG(s_ExecuteBundle, self)(self, bundle);
}

void STDMETHODCALLTYPE Hk_SetPipelineState1(ID3D12GraphicsCommandList4* self, ID3D12StateObject* so) {
    g_skip.Set(self, false);
    ORIG(s_SetPipelineState1, self)(self, so);
}

// Only skips with ComputePipelineMode=skip (experimental): otherwise compute placeholders are
// never left bound unresolved, so the skip flag is never set when a Dispatch comes.
void STDMETHODCALLTYPE Hk_Dispatch(ID3D12GraphicsCommandList* self, UINT x, UINT y, UINT z) {
    if (g_skip.Has(self)) { g_stats.dispatchesSkipped.fetch_add(1, std::memory_order_relaxed); return; }
    ORIG(s_Dispatch, self)(self, x, y, z);
}

void STDMETHODCALLTYPE Hk_DispatchMesh(ID3D12GraphicsCommandList6* self, UINT x, UINT y, UINT z) {
    if (g_skip.Has(self)) return CountSkip();
    ORIG(s_DispatchMesh, self)(self, x, y, z);
}

// ---------------------------------------------------------------------------------
// Device hooks

using PFN_CreateGraphicsPipelineState =
    HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, const D3D12_GRAPHICS_PIPELINE_STATE_DESC*, REFIID, void**);
using PFN_CreatePipelineState = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device2*, const D3D12_PIPELINE_STATE_STREAM_DESC*, REFIID, void**);
using PFN_CreateCommandList = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, UINT, D3D12_COMMAND_LIST_TYPE, ID3D12CommandAllocator*,
                                                          ID3D12PipelineState*, REFIID, void**);
using PFN_CreateCommandList1 = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device4*, UINT, D3D12_COMMAND_LIST_TYPE, D3D12_COMMAND_LIST_FLAGS,
                                                           REFIID, void**);
using PFN_Residency = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, UINT, ID3D12Pageable* const*);
using PFN_EnqueueMakeResident = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device3*, D3D12_RESIDENCY_FLAGS, UINT, ID3D12Pageable* const*,
                                                            ID3D12Fence*, UINT64);
using PFN_SetResidencyPriority = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device1*, UINT, ID3D12Pageable* const*,
                                                              const D3D12_RESIDENCY_PRIORITY*);
using PFN_CreatePipelineLibrary = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device1*, const void*, SIZE_T, REFIID, void**);
using PFN_LoadGraphicsPipeline = HRESULT(STDMETHODCALLTYPE*)(ID3D12PipelineLibrary*, LPCWSTR, const D3D12_GRAPHICS_PIPELINE_STATE_DESC*,
                                                         REFIID, void**);
using PFN_LoadComputePipeline = HRESULT(STDMETHODCALLTYPE*)(ID3D12PipelineLibrary*, LPCWSTR, const D3D12_COMPUTE_PIPELINE_STATE_DESC*,
                                                        REFIID, void**);
using PFN_LoadPipeline = HRESULT(STDMETHODCALLTYPE*)(ID3D12PipelineLibrary1*, LPCWSTR, const D3D12_PIPELINE_STATE_STREAM_DESC*, REFIID,
                                                 void**);
using PFN_CreateComputePipelineState = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, const D3D12_COMPUTE_PIPELINE_STATE_DESC*, REFIID,
                                                               void**);
using PFN_StorePipeline = HRESULT(STDMETHODCALLTYPE*)(ID3D12PipelineLibrary*, LPCWSTR, ID3D12PipelineState*);
using PFN_FactoryCreateDevice = HRESULT(STDMETHODCALLTYPE*)(ID3D12DeviceFactory*, IUnknown*, D3D_FEATURE_LEVEL, REFIID, void**);
using PFN_CreateDeviceFactory = HRESULT(STDMETHODCALLTYPE*)(ID3D12SDKConfiguration1*, UINT, LPCSTR, REFIID, void**);

GSlot<PFN_CreateGraphicsPipelineState> s_CreateGraphicsPipelineState;
GSlot<PFN_CreatePipelineState> s_CreatePipelineState;
GSlot<PFN_CreateCommandList> s_CreateCommandList;
GSlot<PFN_CreateCommandList1> s_CreateCommandList1;
GSlot<PFN_Residency> s_MakeResident, s_Evict;
GSlot<PFN_EnqueueMakeResident> s_EnqueueMakeResident;
GSlot<PFN_CreatePipelineLibrary> s_CreatePipelineLibrary;
GSlot<PFN_SetResidencyPriority> s_SetResidencyPriority;
Slot<PFN_StorePipeline> s_StorePipeline;
Slot<PFN_LoadGraphicsPipeline> s_LoadGraphicsPipeline;
Slot<PFN_LoadComputePipeline> s_LoadComputePipeline;
Slot<PFN_LoadPipeline> s_LoadPipeline;
GSlot<PFN_CreateComputePipelineState> s_CreateComputePipelineState;
Slot<PFN_FactoryCreateDevice> s_FactoryCreateDevice;
Slot<PFN_CreateDeviceFactory> s_CreateDeviceFactory;

double NowMs() {
    static LARGE_INTEGER f = [] { LARGE_INTEGER x; QueryPerformanceFrequency(&x); return x; }();
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return double(t.QuadPart) * 1000.0 / double(f.QuadPart);
}

void MaybeLogStats() {
    static std::atomic<double> last{0};
    double now = NowMs(), prev = last.load();
    if (now - prev > 10000 && last.compare_exchange_strong(prev, now) && g_stats.psoPlaceholders.load()) LogStats("periodic");
}

// Placeholders handed out whose compile hasn't finished yet (queued or compiling).
std::atomic<int64_t> g_outstanding{0};

// The queue is too long to take another background compile, so this create (on a thread that
// doesn't record command lists) compiles on the calling thread:
//   QueueFullMode=compile:     at MaxQueuedCompiles waiting compiles.
//   QueueFullMode=placeholder: past MaxQueuedCompiles it still gets a placeholder, but only while
//                              fewer than MaxPlaceholders are outstanding. A loading screen or
//                              precache flood is far bigger than that, so most of it compiles
//                              normally and the screen waits for it.
constexpr int64_t kPlaceholderCap = 8192;  // hard upper bound for MaxPlaceholders
bool QueueTooFull(size_t pending) {
    if (g_cfg.maxQueue <= 0) return false;
    if (pending < (size_t)g_cfg.maxQueue) return false;
    if (!g_cfg.queueFullPlaceholder) return true;
    int64_t cap = std::min<int64_t>(std::max(g_cfg.maxPlaceholders, 0), kPlaceholderCap);
    return g_outstanding.load(std::memory_order_relaxed) >= cap;
}

// Why a create that isn't going to the background compiles on the calling thread.
CallCause SyncCause() {
    if (StartupActive()) return CallCause::Startup;
    if (LoadingActive()) return CallCause::Loading;
    if (g_cfg.enabled && g_cfg.allThreads && !t_recorder && QueueTooFull(PoolPending())) return CallCause::QueueFull;
    return CallCause::NotEligible;
}

bool Eligible(void** ppv, REFIID riid) {
    if (!g_cfg.enabled || !ppv || !g_asyncAllowed.load(std::memory_order_relaxed)) return false;  // ppv == null: validation only
    if (!g_cfg.allThreads && !t_recorder) return false;
    // Backpressure: under a precache flood, background threads block as they normally would,
    // so queued descriptions (with their shader bytecode) can't pile up without bound.
    if (!t_recorder && g_cfg.maxQueue > 0) {
        size_t pending = PoolPending();
        if (QueueTooFull(pending)) return false;
        // QueueFullMode=placeholder: past MaxQueuedCompiles this create still gets a placeholder
        // and waits its turn instead of compiling on the game thread.
        if (pending >= (size_t)g_cfg.maxQueue) g_stats.psoOverflow.fetch_add(1, std::memory_order_relaxed);
    }
    return riid == __uuidof(ID3D12PipelineState) || riid == __uuidof(IUnknown) || riid == __uuidof(ID3D12Pageable) ||
           riid == __uuidof(ID3D12DeviceChild) || riid == __uuidof(ID3D12Object);
}

// Shared tail of both create hooks: wait for the budget, then hand out the real PSO,
// the real error, or the placeholder.
// waitBudget: false when other compiles were already queued, so this one can't start (let alone
// finish) within BudgetMs. Waiting anyway costs ~BudgetMs per create; a game creating hundreds of
// pipelines in one frame (Wuchang: 648) would stall for hundreds of ms.
HRESULT Finish(AsyncPSO* pso, REFIID riid, void** ppv, bool waitBudget = true) {
    if (waitBudget && pso->WaitFor(g_cfg.budgetMs)) {
        HRESULT hr = pso->Result();
        ID3D12PipelineState* r = pso->Real();
        if (SUCCEEDED(hr) && r) {
            g_stats.psoInBudget++;
            hr = r->QueryInterface(riid, ppv);
        } else {
            *ppv = nullptr;
            if (SUCCEEDED(hr)) hr = E_FAIL;
        }
        Log(LogLevel::Debug, "PSO in budget hr=0x%08lx", (unsigned long)hr);
        pso->Release();
        return hr;
    }
    g_stats.psoPlaceholders++;
    Log(LogLevel::Debug, "PSO placeholder %p (recording thread=%d)", (void*)pso, (int)t_recorder);
    Log(LogLevel::Perf, "placeholder %p handed out (%zu compiles queued)", (void*)pso, PoolPending());
    MaybeLogStats();
    *ppv = static_cast<ID3D12PipelineState*>(pso);  // keeps the creation reference
    return S_OK;
}

template <typename Owned, typename CompileFn>
HRESULT LaunchAsync(ID3D12Device* device, std::unique_ptr<Owned> owned, CompileFn compile, REFIID riid, void** ppv,
                    bool waitOnUse = false, bool uiWait = false) {
    auto* pso = new (std::nothrow) AsyncPSO(device);
    if (pso && waitOnUse) pso->SetWaitOnUse(uiWait);
    if (!pso) {
        Owned* d = owned.get();
        ID3D12PipelineState* real = nullptr;
        HRESULT hr = compile(d, &real);
        if (SUCCEEDED(hr)) { hr = real->QueryInterface(riid, ppv); real->Release(); }
        return hr;
    }
    pso->AddRef();  // the job's reference
    std::shared_ptr<Owned> job(std::move(owned));
    double queued = NowMs();
    auto pj = std::make_shared<PoolJob>();
    pso->SetJob(pj);
    pj->fn = [pso, job, compile, queued] {
        if (g_cfg.debugDelayMs > 0) Sleep((DWORD)g_cfg.debugDelayMs);
        double t0 = NowMs();
        ID3D12PipelineState* real = nullptr;
        HRESULT hr = compile(job.get(), &real);
        double t1 = NowMs();
        g_stats.bgCompileUs += (uint64_t)((t1 - t0) * 1000.0);
        if (t1 - t0 >= g_cfg.slowLogMs)
            Log(LogLevel::Perf, "slow compile %.1f ms (queued %.1f ms) for %p", t1 - t0, t0 - queued, (void*)pso);
        pso->Complete(hr, real, t1 - t0);
        g_outstanding.fetch_sub(1, std::memory_order_relaxed);
        pso->Release();
    };
    bool idleWorker = PoolPending() < (size_t)std::max(1, PoolBaseWorkers());
    g_outstanding.fetch_add(1, std::memory_order_relaxed);
    PoolSubmit(pj);
    return Finish(pso, riid, ppv, idleWorker);
}

HRESULT STDMETHODCALLTYPE Hk_CreateGraphicsPipelineState(ID3D12Device* self, const D3D12_GRAPHICS_PIPELINE_STATE_DESC* desc,
                                                         REFIID riid, void** ppv) {
    auto orig = ORIG(s_CreateGraphicsPipelineState, self);
    CallTimer timer;
    g_stats.psoRequests++;
    g_stats.psoCreates++;
    StartupNote();
    std::unique_ptr<OwnedGraphicsDesc> copy;
    bool normal = false, startup = false;
    if (desc && StartupActive()) { normal = startup = true; g_stats.psoStartupSync++; SetCallCause(CallCause::Startup); }
    else if (desc && LoadingActive()) { normal = startup = true; g_stats.psoLoadingSync++; SetCallCause(CallCause::Loading); }
    bool ui = !startup && desc && g_cfg.uiMode != 0 && IsUiLike(desc->NumRenderTargets, desc->RTVFormats, desc->DSVFormat);
    if (ui && g_cfg.uiMode == 2) {
        normal = true;
        g_stats.psoUiSync++;
        SetCallCause(CallCause::Ui);
    }
    if (!normal) SetCallCause(SyncCause());
    if (!normal && Eligible(ppv, riid)) {
        try { copy = CopyGraphicsDesc(desc); } catch (...) { copy.reset(); }
        SetCallCause(copy ? CallCause::Async : CallCause::CopyFailed);
    }
    if (!copy) {
        g_stats.psoSync++;
        StartupBoost boost(startup);
        HRESULT hr = orig(self, desc, riid, ppv);
        Log(LogLevel::Debug, "PSO sync (graphics desc) hr=0x%08lx", (unsigned long)hr);
        return hr;
    }
    if (ui) g_stats.psoUiAsync++;
    return LaunchAsync(self, std::move(copy),
                       [self, orig](OwnedGraphicsDesc* d, ID3D12PipelineState** out) {
                           return orig(self, &d->desc, __uuidof(ID3D12PipelineState), reinterpret_cast<void**>(out));
                       },
                       riid, ppv, ui, ui);
}

HRESULT STDMETHODCALLTYPE Hk_CreatePipelineState(ID3D12Device2* self, const D3D12_PIPELINE_STATE_STREAM_DESC* desc, REFIID riid,
                                                 void** ppv) {
    auto orig = ORIG(s_CreatePipelineState, self);
    CallTimer timer;
    g_stats.psoCreates++;
    StartupNote();
    if (StartupActive()) {
        g_stats.psoStartupSync++;
        SetCallCause(CallCause::Startup);
        StartupBoost boost;
        return orig(self, desc, riid, ppv);
    }
    if (LoadingActive()) {
        g_stats.psoLoadingSync++;
        SetCallCause(CallCause::Loading);
        StartupBoost boost;
        return orig(self, desc, riid, ppv);
    }
    if (!Eligible(ppv, riid)) {
        SetCallCause(SyncCause());
        return orig(self, desc, riid, ppv);
    }
    const char* why = nullptr;
    std::unique_ptr<OwnedStream> copy;
    try { copy = CopyStream(desc, &why); } catch (...) { why = "out of memory while copying"; }
    // Compute work can't be skipped safely: either compile it here, or (default) in the background
    // with wait-on-use, so a dispatch that needs it waits for it instead of being dropped.
    if (copy && copy->isCompute && g_cfg.computeMode == 0) {
        SetCallCause(CallCause::Compute);
        return orig(self, desc, riid, ppv);
    }
    if (copy && copy->isCompute) {
        g_stats.psoComputeAsync++;
        SetCallCause(CallCause::Async);
        return LaunchAsync(self, std::move(copy),
                           [self, orig](OwnedStream* d, ID3D12PipelineState** out) {
                               return orig(self, &d->desc, __uuidof(ID3D12PipelineState), reinterpret_cast<void**>(out));
                           },
                           riid, ppv, g_cfg.computeMode == 1);  // wait: never skipped; skip: dispatches dropped until ready
    }
    g_stats.psoRequests++;
    bool ui = copy && g_cfg.uiMode != 0 && IsUiLike(copy->numRt, copy->rt, copy->ds);
    if (ui && g_cfg.uiMode == 2) {
        g_stats.psoUiSync++;
        SetCallCause(CallCause::Ui);
        return orig(self, desc, riid, ppv);
    }
    if (ui) g_stats.psoUiAsync++;
    SetCallCause(copy ? CallCause::Async : CallCause::CopyFailed);
    if (!copy) {
        g_stats.psoSync++;
        Log(LogLevel::Debug, "stream PSO compiled synchronously: %s", why ? why : "?");
        return orig(self, desc, riid, ppv);
    }
    return LaunchAsync(self, std::move(copy),
                       [self, orig](OwnedStream* d, ID3D12PipelineState** out) {
                           return orig(self, &d->desc, __uuidof(ID3D12PipelineState), reinterpret_cast<void**>(out));
                       },
                       riid, ppv, ui, ui);
}

HRESULT STDMETHODCALLTYPE Hk_CreateCommandList(ID3D12Device* self, UINT mask, D3D12_COMMAND_LIST_TYPE type,
                                               ID3D12CommandAllocator* alloc, ID3D12PipelineState* pso, REFIID riid, void** ppv) {
    t_recorder = true;
    bool notReady;
    ID3D12PipelineState* use = Resolve(pso, type == D3D12_COMMAND_LIST_TYPE_BUNDLE, &notReady, WaitBundle, __builtin_return_address(0));
    HRESULT hr = ORIG(s_CreateCommandList, self)(self, mask, type, alloc, use, riid, ppv);
    if (SUCCEEDED(hr) && ppv && *ppv) {
        HookCommandList(static_cast<IUnknown*>(*ppv));
        g_skip.Set(*ppv, notReady);  // also clears a stale entry left at a reused address
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE Hk_CreateCommandList1(ID3D12Device4* self, UINT mask, D3D12_COMMAND_LIST_TYPE type,
                                                D3D12_COMMAND_LIST_FLAGS flags, REFIID riid, void** ppv) {
    t_recorder = true;
    HRESULT hr = ORIG(s_CreateCommandList1, self)(self, mask, type, flags, riid, ppv);
    if (SUCCEEDED(hr) && ppv && *ppv) {
        HookCommandList(static_cast<IUnknown*>(*ppv));
        g_skip.Set(*ppv, false);
    }
    return hr;
}

// PSOs are pageable objects: residency calls must see the real ones. `keep` marks entries
// to pass on; only placeholders whose compile failed are dropped (the game's own entries,
// nulls included, go through untouched so the runtime validates them as usual).
bool ResolvePageables(UINT n, ID3D12Pageable* const* objs, std::vector<ID3D12Pageable*>& out, std::vector<bool>& keep) {
    bool changed = false;
    out.assign(objs, objs + n);
    keep.assign(n, true);
    for (UINT i = 0; i < n; ++i)
        if (AsyncPSO* a = AsyncPSO::From(out[i])) {
            changed = true;
            out[i] = a->WaitReal(WaitResidency, nullptr);
            if (!out[i]) keep[i] = false;
        }
    return changed;
}

template <typename T>
void Compact(std::vector<T>& v, const std::vector<bool>& keep) {
    size_t w = 0;
    for (size_t i = 0; i < v.size(); ++i)
        if (keep[i]) v[w++] = v[i];
    v.resize(w);
}

HRESULT ResidencyCall(PFN_Residency orig, ID3D12Device* self, UINT n, ID3D12Pageable* const* objs) {
    std::vector<ID3D12Pageable*> v;
    std::vector<bool> keep;
    if (!objs || !ResolvePageables(n, objs, v, keep)) return orig(self, n, objs);
    Compact(v, keep);
    if (v.empty()) return S_OK;
    return orig(self, (UINT)v.size(), v.data());
}

HRESULT STDMETHODCALLTYPE Hk_MakeResident(ID3D12Device* self, UINT n, ID3D12Pageable* const* objs) {
    return ResidencyCall(ORIG(s_MakeResident, self), self, n, objs);
}
HRESULT STDMETHODCALLTYPE Hk_Evict(ID3D12Device* self, UINT n, ID3D12Pageable* const* objs) {
    return ResidencyCall(ORIG(s_Evict, self), self, n, objs);
}
HRESULT STDMETHODCALLTYPE Hk_EnqueueMakeResident(ID3D12Device3* self, D3D12_RESIDENCY_FLAGS flags, UINT n, ID3D12Pageable* const* objs,
                                                 ID3D12Fence* fence, UINT64 value) {
    auto orig = ORIG(s_EnqueueMakeResident, self);
    std::vector<ID3D12Pageable*> v;
    std::vector<bool> keep;
    if (!objs || !ResolvePageables(n, objs, v, keep)) return orig(self, flags, n, objs, fence, value);
    Compact(v, keep);
    return orig(self, flags, (UINT)v.size(), v.data(), fence, value);  // still signals the fence when empty
}

HRESULT STDMETHODCALLTYPE Hk_SetResidencyPriority(ID3D12Device1* self, UINT n, ID3D12Pageable* const* objs,
                                                  const D3D12_RESIDENCY_PRIORITY* prio) {
    auto orig = ORIG(s_SetResidencyPriority, self);
    std::vector<ID3D12Pageable*> v;
    std::vector<bool> keep;
    if (!objs || !prio || !ResolvePageables(n, objs, v, keep)) return orig(self, n, objs, prio);
    std::vector<D3D12_RESIDENCY_PRIORITY> p(prio, prio + n);
    Compact(v, keep);
    Compact(p, keep);
    if (v.empty()) return S_OK;
    return orig(self, (UINT)v.size(), v.data(), p.data());
}

// Engines (Unreal's D3D12 PSO cache) store every new PSO into a pipeline library right
// after creating it. Waiting there would block on every compile, so a still-compiling
// PSO is stored once it's ready instead. Until then a LoadPipeline of that name simply
// misses, which the engine already handles by creating the PSO normally.
std::atomic<uint64_t> g_deferredStores{0};

HRESULT STDMETHODCALLTYPE Hk_StorePipeline(ID3D12PipelineLibrary* self, LPCWSTR name, ID3D12PipelineState* pso) {
    auto orig = ORIG(s_StorePipeline, self);
    AsyncPSO* a = AsyncPSO::From(pso);
    if (!a) return orig(self, name, pso);
    if (auto* r = a->Real()) return orig(self, name, r);
    if (a->GetState() == AsyncPSO::Failed) return E_INVALIDARG;
    if (!name) return E_INVALIDARG;
    self->AddRef();
    std::wstring key(name);
    a->OnComplete([self, key, orig](ID3D12PipelineState* real) {
        if (real) {
            HRESULT hr = orig(self, key.c_str(), real);
            if (FAILED(hr)) Log(LogLevel::Debug, "deferred StorePipeline failed hr=0x%08lx", (unsigned long)hr);
        }
        self->Release();
    });
    uint64_t n = ++g_deferredStores;
    if (n == 1 || (n & (n - 1)) == 0) Log(LogLevel::Info, "pipeline library: %llu stores deferred until their compile finishes", (unsigned long long)n);
    return S_OK;
}

// Pipeline-library loads and compute creates change nothing; they are counted because a
// startup precompile can run through them (a library load can compile inside the driver).
HRESULT STDMETHODCALLTYPE Hk_LoadGraphicsPipeline(ID3D12PipelineLibrary* self, LPCWSTR name,
                                                  const D3D12_GRAPHICS_PIPELINE_STATE_DESC* d, REFIID riid, void** ppv) {
    auto orig = ORIG(s_LoadGraphicsPipeline, self);
    CallTimer timer;
    SetCallCause(CallCause::LibraryLoad);
    g_stats.libraryLoads++;
    StartupNote();
    return orig(self, name, d, riid, ppv);
}
HRESULT STDMETHODCALLTYPE Hk_LoadComputePipeline(ID3D12PipelineLibrary* self, LPCWSTR name,
                                                 const D3D12_COMPUTE_PIPELINE_STATE_DESC* d, REFIID riid, void** ppv) {
    auto orig = ORIG(s_LoadComputePipeline, self);
    CallTimer timer;
    SetCallCause(CallCause::LibraryLoad);
    g_stats.libraryLoads++;
    StartupNote();
    return orig(self, name, d, riid, ppv);
}
HRESULT STDMETHODCALLTYPE Hk_LoadPipeline(ID3D12PipelineLibrary1* self, LPCWSTR name, const D3D12_PIPELINE_STATE_STREAM_DESC* d,
                                          REFIID riid, void** ppv) {
    auto orig = ORIG(s_LoadPipeline, self);
    CallTimer timer;
    SetCallCause(CallCause::LibraryLoad);
    g_stats.libraryLoads++;
    StartupNote();
    return orig(self, name, d, riid, ppv);
}
HRESULT STDMETHODCALLTYPE Hk_CreateComputePipelineState(ID3D12Device* self, const D3D12_COMPUTE_PIPELINE_STATE_DESC* d, REFIID riid,
                                                        void** ppv) {
    auto orig = ORIG(s_CreateComputePipelineState, self);
    CallTimer timer;
    SetCallCause(CallCause::Compute);
    g_stats.computeCreates++;
    StartupNote();
    if (g_cfg.computeMode != 0 && d && !StartupActive() && !LoadingActive() && Eligible(ppv, riid)) {
        std::unique_ptr<OwnedComputeDesc> copy;
        try { copy = CopyComputeDesc(d); } catch (...) { copy.reset(); }
        if (copy) {
            g_stats.psoComputeAsync++;
            SetCallCause(CallCause::Async);
            return LaunchAsync(self, std::move(copy),
                               [self, orig](OwnedComputeDesc* c, ID3D12PipelineState** out) {
                                   return orig(self, &c->desc, __uuidof(ID3D12PipelineState), reinterpret_cast<void**>(out));
                               },
                               riid, ppv, g_cfg.computeMode == 1);
        }
    }
    return orig(self, d, riid, ppv);
}

HRESULT STDMETHODCALLTYPE Hk_CreatePipelineLibrary(ID3D12Device1* self, const void* blob, SIZE_T size, REFIID riid, void** ppv) {
    HRESULT hr = ORIG(s_CreatePipelineLibrary, self)(self, blob, size, riid, ppv);
    if (SUCCEEDED(hr) && ppv && *ppv) HookPipelineLibrary(static_cast<IUnknown*>(*ppv));
    return hr;
}

HRESULT STDMETHODCALLTYPE Hk_FactoryCreateDevice(ID3D12DeviceFactory* self, IUnknown* adapter, D3D_FEATURE_LEVEL fl, REFIID riid,
                                                 void** ppv) {
    HRESULT hr = ORIG(s_FactoryCreateDevice, self)(self, adapter, fl, riid, ppv);
    if (SUCCEEDED(hr) && ppv && *ppv) {
        HookDevice(static_cast<IUnknown*>(*ppv));
        ID3D12Device* d = nullptr;
        if (SUCCEEDED(static_cast<IUnknown*>(*ppv)->QueryInterface(__uuidof(ID3D12Device), (void**)&d))) { HookPresent(d); d->Release(); }
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE Hk_CreateDeviceFactory(ID3D12SDKConfiguration1* self, UINT ver, LPCSTR path, REFIID riid, void** ppv) {
    HRESULT hr = ORIG(s_CreateDeviceFactory, self)(self, ver, path, riid, ppv);
    if (SUCCEEDED(hr) && ppv && *ppv) HookDeviceFactory(static_cast<IUnknown*>(*ppv));
    return hr;
}

template <typename I>
I* QI(IUnknown* u) {
    I* p = nullptr;
    if (u && SUCCEEDED(u->QueryInterface(__uuidof(I), reinterpret_cast<void**>(&p)))) return p;
    return nullptr;
}

}  // namespace

// ---------------------------------------------------------------------------------
// Installation

void HookDevice(IUnknown* any) {
    auto* d = QI<ID3D12Device>(any);
    if (!d) return;
    if (g_cfg.enabled) PoolStart();  // Enabled=0: hooks only measure, everything passes straight through
    std::call_once(g_devInit, [&] {
        auto add = [](auto& slot, unsigned idx, auto hook, const char* name) {
            slot.g = &g_devGroup;
            slot.k = g_devGroup.Add(idx, reinterpret_cast<void*>(hook), name);
        };
        add(s_CreateGraphicsPipelineState, kDev_CreateGraphicsPipelineState, Hk_CreateGraphicsPipelineState, "CreateGraphicsPipelineState");
        add(s_CreateCommandList, kDev_CreateCommandList, Hk_CreateCommandList, "CreateCommandList");
        add(s_CreateComputePipelineState, kDev_CreateComputePipelineState, Hk_CreateComputePipelineState, "CreateComputePipelineState");
        add(s_MakeResident, kDev_MakeResident, Hk_MakeResident, "MakeResident");
        add(s_Evict, kDev_Evict, Hk_Evict, "Evict");
        // Later interface versions live in the same vtable; only hook what this runtime has.
        if (auto* d1 = QI<ID3D12Device1>(any)) {
            add(s_CreatePipelineLibrary, kDev1_CreatePipelineLibrary, Hk_CreatePipelineLibrary, "CreatePipelineLibrary");
            add(s_SetResidencyPriority, kDev1_SetResidencyPriority, Hk_SetResidencyPriority, "SetResidencyPriority");
            d1->Release();
        }
        if (auto* d2 = QI<ID3D12Device2>(any)) {
            add(s_CreatePipelineState, kDev2_CreatePipelineState, Hk_CreatePipelineState, "CreatePipelineState");
            d2->Release();
        }
        if (auto* d3 = QI<ID3D12Device3>(any)) {
            add(s_EnqueueMakeResident, kDev3_EnqueueMakeResident, Hk_EnqueueMakeResident, "EnqueueMakeResident");
            d3->Release();
        }
        if (auto* d4 = QI<ID3D12Device4>(any)) {
            add(s_CreateCommandList1, kDev4_CreateCommandList1, Hk_CreateCommandList1, "CreateCommandList1");
            d4->Release();
        }
    });
    g_devGroup.Ensure(d);
    Log(LogLevel::Info, "device %p hooked", (void*)d);
    d->Release();
}

void HookCommandList(IUnknown* any) {
    auto* cl = QI<ID3D12GraphicsCommandList>(any);
    if (!cl) return;  // video command lists etc.
    std::call_once(g_clInit, [&] {
        auto add = [](auto& slot, unsigned idx, auto hook, const char* name) {
            slot.g = &g_clGroup;
            slot.k = g_clGroup.Add(idx, reinterpret_cast<void*>(hook), name);
        };
        add(s_Close, kCl_Close, Hk_Close, "Close");
        add(s_Reset, kCl_Reset, Hk_Reset, "Reset");
        add(s_ClearState, kCl_ClearState, Hk_ClearState, "ClearState");
        add(s_DrawInstanced, kCl_DrawInstanced, Hk_DrawInstanced, "DrawInstanced");
        add(s_DrawIndexedInstanced, kCl_DrawIndexedInstanced, Hk_DrawIndexedInstanced, "DrawIndexedInstanced");
        add(s_SetPipelineState, kCl_SetPipelineState, Hk_SetPipelineState, "SetPipelineState");
        add(s_ExecuteIndirect, kCl_ExecuteIndirect, Hk_ExecuteIndirect, "ExecuteIndirect");
        add(s_ExecuteBundle, kCl_ExecuteBundle, Hk_ExecuteBundle, "ExecuteBundle");
        add(s_Dispatch, kCl_Dispatch, Hk_Dispatch, "Dispatch");
        if (auto* c4 = QI<ID3D12GraphicsCommandList4>(any)) {
            add(s_SetPipelineState1, kCl4_SetPipelineState1, Hk_SetPipelineState1, "SetPipelineState1");
            c4->Release();
        }
        if (auto* c6 = QI<ID3D12GraphicsCommandList6>(any)) {
            add(s_DispatchMesh, kCl6_DispatchMesh, Hk_DispatchMesh, "DispatchMesh");
            c6->Release();
        }
    });
    g_clGroup.Ensure(cl);
    cl->Release();
}

void HookPipelineLibrary(IUnknown* any) {
    if (auto* lib = QI<ID3D12PipelineLibrary>(any)) {
        s_StorePipeline.Patch(lib, kLib_StorePipeline, Hk_StorePipeline, "StorePipeline");
        s_LoadGraphicsPipeline.Patch(lib, kLib_LoadGraphicsPipeline, Hk_LoadGraphicsPipeline, "LoadGraphicsPipeline");
        s_LoadComputePipeline.Patch(lib, kLib_LoadComputePipeline, Hk_LoadComputePipeline, "LoadComputePipeline");
        lib->Release();
        if (auto* l1 = QI<ID3D12PipelineLibrary1>(any)) {
            s_LoadPipeline.Patch(l1, kLib1_LoadPipeline, Hk_LoadPipeline, "LoadPipeline");
            l1->Release();
        }
    }
}

void HookDeviceFactory(IUnknown* any) {
    if (auto* f = QI<ID3D12DeviceFactory>(any)) {
        s_FactoryCreateDevice.Patch(f, kFactory_CreateDevice, Hk_FactoryCreateDevice, "DeviceFactory::CreateDevice");
        f->Release();
    }
}

void HookSdkConfiguration(IUnknown* any) {
    if (auto* c = QI<ID3D12SDKConfiguration1>(any)) {
        s_CreateDeviceFactory.Patch(c, kSdkCfg1_CreateDeviceFactory, Hk_CreateDeviceFactory, "SDKConfiguration1::CreateDeviceFactory");
        c->Release();
    }
}

bool OnRecordingThread() { return t_recorder; }
int64_t OutstandingPlaceholders() { return g_outstanding.load(std::memory_order_relaxed); }


}  // namespace d3a
