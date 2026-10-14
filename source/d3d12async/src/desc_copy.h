// Deep copies of PSO descriptions. The application's description (and everything it
// points to) is only guaranteed valid during the create call, but the real compile
// runs later on a worker thread, so every pointer is copied into owned storage.
#pragma once
#include "common.h"

#include <deque>
#include <memory>
#include <string>
#include <vector>

namespace d3a {

class OwnedStorage {
public:
    ~OwnedStorage();
    const void* CopyBytes(const void* src, size_t n);
    const char* CopyString(const char* s);
    void HoldRef(IUnknown* u);  // AddRef now, Release in destructor
private:
    std::vector<std::unique_ptr<uint8_t[]>> blobs_;
    std::deque<std::string> strings_;
    std::vector<IUnknown*> refs_;
};

struct OwnedGraphicsDesc {
    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
    OwnedStorage store;
};

struct OwnedComputeDesc {
    D3D12_COMPUTE_PIPELINE_STATE_DESC desc{};
    OwnedStorage store;
};

struct OwnedStream {
    std::vector<uint64_t> buffer;  // 8-byte aligned storage for the stream
    D3D12_PIPELINE_STATE_STREAM_DESC desc{};
    OwnedStorage store;
    bool isCompute = false;
    UINT numRt = 0;                          // render target formats, for the UI check
    DXGI_FORMAT rt[8] = {};
    DXGI_FORMAT ds = DXGI_FORMAT_UNKNOWN;
};

// nullptr when something can't be copied safely: the caller then compiles synchronously.
std::unique_ptr<OwnedGraphicsDesc> CopyGraphicsDesc(const D3D12_GRAPHICS_PIPELINE_STATE_DESC* d);
std::unique_ptr<OwnedComputeDesc> CopyComputeDesc(const D3D12_COMPUTE_PIPELINE_STATE_DESC* d);
std::unique_ptr<OwnedStream> CopyStream(const D3D12_PIPELINE_STATE_STREAM_DESC* d, const char** whyNot);

}  // namespace d3a
