// Placeholder pipeline state handed to the game while the real one compiles.
#pragma once
#include "common.h"

#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>

namespace d3a {

struct PoolJob;

class AsyncPSO final : public ID3D12PipelineState {
public:
    enum State { Pending = 0, Ready = 1, Failed = 2 };

    explicit AsyncPSO(ID3D12Device* device);

    static bool Is(const void* p);
    static AsyncPSO* From(void* p) { return Is(p) ? static_cast<AsyncPSO*>(reinterpret_cast<ID3D12PipelineState*>(p)) : nullptr; }

    // Called once by the worker that compiled the real PSO (takes ownership of `real`).
    void Complete(HRESULT hr, ID3D12PipelineState* real, double ms);

    State GetState() const { return (State)state_.load(std::memory_order_acquire); }
    ID3D12PipelineState* Real() const { return real_.load(std::memory_order_acquire); }  // non-null once Ready
    bool WaitFor(double ms);                // true once completed (ready or failed)
    ID3D12PipelineState* WaitReal(WaitReason why = WaitOther, void* caller = nullptr, const char* detail = nullptr);  // blocks; null if failed
    // UI-type pipelines: compiled in the background, but a draw that needs one waits for it
    // (helping by compiling it on the waiting thread) instead of being skipped.
    // ui: the wait is capped at UiWaitMaxMs (then the draw is skipped like any other).
    void SetWaitOnUse(bool ui = false) { waitOnUse_ = true; uiWait_ = ui; }
    bool WaitOnUse() const { return waitOnUse_; }
    ID3D12PipelineState* WaitForUse();  // null if not ready (failed, or a UI wait ran out of UiWaitMaxMs)
    HRESULT Result() const { return hr_; }
    bool SupportsIID(REFIID riid) const;
    void SetJob(std::shared_ptr<PoolJob> j) { std::lock_guard<std::mutex> lk(mu_); job_ = std::move(j); }
    void Boost();  // the game wants to draw with this now
    // Run `fn` with the real PSO (null if the compile failed) once it exists; runs right
    // away if it already does. Used to defer work the game wants done with the real PSO.
    void OnComplete(std::function<void(ID3D12PipelineState*)> fn);

    // IUnknown
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override;
    ULONG STDMETHODCALLTYPE AddRef() override;
    ULONG STDMETHODCALLTYPE Release() override;
    // ID3D12Object
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID guid, UINT* pDataSize, void* pData) override;
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID guid, UINT DataSize, const void* pData) override;
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID guid, const IUnknown* pData) override;
    HRESULT STDMETHODCALLTYPE SetName(LPCWSTR Name) override;
    // ID3D12DeviceChild
    HRESULT STDMETHODCALLTYPE GetDevice(REFIID riid, void** ppvDevice) override;
    // ID3D12PipelineState
    HRESULT STDMETHODCALLTYPE GetCachedBlob(ID3DBlob** ppBlob) override;

private:
    ~AsyncPSO();
    struct PrivateItem {
        GUID guid;
        std::vector<uint8_t> data;
        IUnknown* iface = nullptr;
    };
    PrivateItem* FindItem(REFGUID g);

    std::atomic<ULONG> refs_{1};
    ID3D12Device* device_;
    std::atomic<ID3D12PipelineState*> real_{nullptr};
    std::atomic<int> state_{Pending};
    HRESULT hr_ = S_OK;
    std::mutex mu_;
    std::condition_variable cv_;
    std::vector<PrivateItem> priv_;  // kept until the real PSO exists, then replayed onto it
    std::shared_ptr<PoolJob> job_;   // dropped once boosted or completed
    bool waitOnUse_ = false;
    bool uiWait_ = false;
    std::vector<std::function<void(ID3D12PipelineState*)>> onComplete_;
};

// Background compile pool. A job can be boosted (moved ahead of the queue) when the game
// tries to draw with its placeholder; it still runs exactly once.
struct PoolJob {
    std::function<void()> fn;
    std::atomic<bool> started{false};  // claimed under the pool lock, so a boosted job runs once
};
void PoolStart();
void PoolSubmit(const std::shared_ptr<PoolJob>& job);
void PoolBoost(const std::shared_ptr<PoolJob>& job);
bool PoolRunInline(const std::shared_ptr<PoolJob>& job);  // run on this thread if no worker has started it
size_t PoolPending();
int PoolBaseWorkers();

}  // namespace d3a
