// End-to-end test for d3d12async. Renders with real D3D12 and reads pixels back, so it
// checks what the GPU actually drew, not just that calls succeed.
// Put next to the built d3d12.dll and run. Exit code 0 = all passed.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <dxgi1_4.h>
#include <directx/d3d12.h>
#include <dxguids/dxguids.h>
#include <d3dcompiler.h>

#include <atomic>
#include <mutex>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

static int g_failed = 0, g_passed = 0;
#define CHECK(cond, ...)                                     \
    do {                                                     \
        if (cond) { ++g_passed; printf("  PASS  "); }        \
        else { ++g_failed; printf("  FAIL  "); }             \
        printf(__VA_ARGS__);                                 \
        printf("\n");                                        \
        fflush(stdout);                                      \
    } while (0)

using PFN_State = int(WINAPI*)(void*);
using PFN_Configure = void(WINAPI*)(double, int, double, int);
using PFN_Stats = void(WINAPI*)(uint64_t*);
static PFN_State PlaceholderState;
static PFN_Configure Configure;
static PFN_Stats GetStats;

static uint64_t Stat(int i) { uint64_t s[8]; GetStats(s); return s[i]; }
enum { S_REQ, S_SYNC, S_INBUDGET, S_PLACEHOLDER, S_FAILED, S_SKIPPED, S_FORCED, S_US };

struct Gpu {
    ID3D12Device2* dev = nullptr;
    ID3D12CommandQueue* queue = nullptr;
    ID3D12RootSignature* rs = nullptr;
    ID3D12Resource *rt = nullptr, *readback = nullptr, *vb = nullptr;
    ID3D12DescriptorHeap* rtvHeap = nullptr;
    ID3D12Fence* fence = nullptr;
    UINT64 fenceValue = 0;
    HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);

    void Wait() {
        queue->Signal(fence, ++fenceValue);
        fence->SetEventOnCompletion(fenceValue, ev);
        WaitForSingleObject(ev, INFINITE);
    }
};
static Gpu g;

static ID3DBlob* Compile(const std::string& src, const char* entry, const char* target) {
    ID3DBlob *code = nullptr, *err = nullptr;
    HRESULT hr = D3DCompile(src.data(), src.size(), nullptr, nullptr, nullptr, entry, target, 0, 0, &code, &err);
    if (FAILED(hr)) {
        printf("shader compile failed: %s\n", err ? (const char*)err->GetBufferPointer() : "?");
        exit(2);
    }
    if (err) err->Release();
    return code;
}

// Fullscreen triangle; the pixel shader outputs a constant colour (unique per PSO).
static const char* kVS =
    "struct O { float4 p : SV_Position; };\n"
    "O main(uint id : SV_VertexID, float4 unused : POSITION) { O o;\n"
    "  float2 uv = float2((id << 1) & 2, id & 2);\n"
    "  o.p = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1) + unused * 0.0; return o; }\n";
static std::string PS(float r, float gg, float b) {
    char s[256];
    snprintf(s, sizeof s, "float4 main() : SV_Target { return float4(%f, %f, %f, 1); }\n", r, gg, b);
    return s;
}

static uint32_t Rgba(float r, float gg, float b) {
    auto c = [](float v) { return (uint32_t)(v * 255.0f + 0.5f); };
    return c(r) | (c(gg) << 8) | (c(b) << 16) | (0xFFu << 24);
}
static const uint32_t kBlack = 0xFF000000u;
static bool Near(uint32_t a, uint32_t b) {  // UNORM conversion may round either way
    for (int i = 0; i < 32; i += 8) {
        int x = (a >> i) & 255, y = (b >> i) & 255;
        if (x - y > 1 || y - x > 1) return false;
    }
    return true;
}

struct OwnedGfx {  // a description whose memory the test can scribble over afterwards
    std::vector<uint8_t> vs, ps;
    char semantic[16] = "POSITION";
    D3D12_INPUT_ELEMENT_DESC elem{};
    D3D12_GRAPHICS_PIPELINE_STATE_DESC d{};
};

static OwnedGfx MakeGfx(float r, float gg, float b, bool garbageVS = false) {
    OwnedGfx o;
    ID3DBlob* vs = Compile(kVS, "main", "vs_5_0");
    ID3DBlob* ps = Compile(PS(r, gg, b), "main", "ps_5_0");
    o.vs.assign((uint8_t*)vs->GetBufferPointer(), (uint8_t*)vs->GetBufferPointer() + vs->GetBufferSize());
    o.ps.assign((uint8_t*)ps->GetBufferPointer(), (uint8_t*)ps->GetBufferPointer() + ps->GetBufferSize());
    vs->Release();
    ps->Release();
    if (garbageVS) for (size_t i = 0; i < o.vs.size(); ++i) o.vs[i] = (uint8_t)(i * 37 + 11);
    o.elem = {o.semantic, 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0};
    auto& d = o.d;
    d.pRootSignature = g.rs;
    d.VS = {o.vs.data(), o.vs.size()};
    d.PS = {o.ps.data(), o.ps.size()};
    d.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    d.SampleMask = 0xFFFFFFFF;
    d.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    d.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    d.RasterizerState.DepthClipEnable = TRUE;
    d.InputLayout = {&o.elem, 1};
    d.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    d.NumRenderTargets = 1;
    d.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    d.SampleDesc.Count = 1;
    return o;
}

static void Scribble(OwnedGfx& o) {  // the app frees/reuses its memory right after the call
    memset(o.vs.data(), 0xCC, o.vs.size());
    memset(o.ps.data(), 0xCC, o.ps.size());
    memset(o.semantic, 'X', sizeof o.semantic - 1);
}

// Per-thread command recording (the layer treats recording threads as frame-critical).
struct Recorder {
    ID3D12CommandAllocator* alloc = nullptr;
    ID3D12GraphicsCommandList* list = nullptr;
    Recorder() {
        g.dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc));
        g.dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, nullptr, IID_PPV_ARGS(&list));
        list->Close();
    }
    ~Recorder() { list->Release(); alloc->Release(); }
};

static D3D12_CPU_DESCRIPTOR_HANDLE HeapStart(ID3D12DescriptorHeap* h) {
#if defined(_MSC_VER)
    return h->GetCPUDescriptorHandleForHeapStart();
#else
    D3D12_CPU_DESCRIPTOR_HANDLE r;
    h->GetCPUDescriptorHandleForHeapStart(&r);  // MinGW ABI form of the same call
    return r;
#endif
}

static D3D12_GPU_DESCRIPTOR_HANDLE GpuHeapStart(ID3D12DescriptorHeap* h) {
#if defined(_MSC_VER)
    return h->GetGPUDescriptorHandleForHeapStart();
#else
    D3D12_GPU_DESCRIPTOR_HANDLE r;
    h->GetGPUDescriptorHandleForHeapStart(&r);
    return r;
#endif
}

static std::atomic<int> g_renderLock{0};
static void (*g_beforeExecute)() = nullptr;

// Stand-ins for driver functions that a per-object vtable copy points to directly.
static std::atomic<int> g_fakeSet{0}, g_fakeDraw{0}, g_fakeSawPlaceholder{0};
static void STDMETHODCALLTYPE FakeDriverSetPSO(ID3D12GraphicsCommandList*, ID3D12PipelineState* p) {
    ++g_fakeSet;
    if (PlaceholderState(p) != -1) ++g_fakeSawPlaceholder;
}
// Minimal pipeline library: records what was stored.
struct FakeLibrary : ID3D12PipelineLibrary {
    std::atomic<ULONG> refs{1};
    std::mutex mu;
    std::vector<std::pair<std::wstring, ID3D12PipelineState*>> stored;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (riid == __uuidof(IUnknown) || riid == __uuidof(ID3D12Object) || riid == __uuidof(ID3D12DeviceChild) ||
            riid == __uuidof(ID3D12PipelineLibrary)) { AddRef(); *ppv = this; return S_OK; }
        *ppv = nullptr; return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs; }
    ULONG STDMETHODCALLTYPE Release() override { ULONG n = --refs; return n; }  // stack object
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID, UINT*, void*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID, UINT, const void*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID, const IUnknown*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetName(LPCWSTR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE GetDevice(REFIID, void**) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE StorePipeline(LPCWSTR name, ID3D12PipelineState* p) override {
        std::lock_guard<std::mutex> lk(mu);
        for (auto& e : stored) if (e.first == name) return E_INVALIDARG;
        if (p) p->AddRef();
        stored.push_back({name, p});
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE LoadGraphicsPipeline(LPCWSTR, const D3D12_GRAPHICS_PIPELINE_STATE_DESC*, REFIID, void**) override { return E_INVALIDARG; }
    HRESULT STDMETHODCALLTYPE LoadComputePipeline(LPCWSTR, const D3D12_COMPUTE_PIPELINE_STATE_DESC*, REFIID, void**) override { return E_INVALIDARG; }
    SIZE_T STDMETHODCALLTYPE GetSerializedSize() override { return 0; }
    HRESULT STDMETHODCALLTYPE Serialize(void*, SIZE_T) override { return S_OK; }
};

static void STDMETHODCALLTYPE FakeDriverDraw(ID3D12GraphicsCommandList*, UINT, UINT, UINT, UINT) { ++g_fakeDraw; }


// Draws with `pso` (or with the list's initial PSO when bindPso is false) and returns pixel (1,1).
static uint32_t Render(Recorder& r, ID3D12PipelineState* pso, bool bindPso = true, ID3D12GraphicsCommandList* useList = nullptr) {
    while (g_renderLock.exchange(1)) Sleep(0);  // one shared render target
    ID3D12GraphicsCommandList* l = useList ? useList : r.list;
    if (!useList) {
        r.alloc->Reset();
        l->Reset(r.alloc, nullptr);
    }
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = HeapStart(g.rtvHeap);
    float black[4] = {0, 0, 0, 1};
    l->ClearRenderTargetView(rtv, black, 0, nullptr);
    l->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    D3D12_VIEWPORT vp = {0, 0, 4, 4, 0, 1};
    D3D12_RECT sc = {0, 0, 4, 4};
    l->RSSetViewports(1, &vp);
    l->RSSetScissorRects(1, &sc);
    l->SetGraphicsRootSignature(g.rs);
    l->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    D3D12_VERTEX_BUFFER_VIEW vbv = {g.vb->GetGPUVirtualAddress(), 256, 16};
    l->IASetVertexBuffers(0, 1, &vbv);
    if (bindPso) l->SetPipelineState(pso);
    l->DrawInstanced(3, 1, 0, 0);
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition = {g.rt, 0, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE};
    l->ResourceBarrier(1, &b);
    D3D12_TEXTURE_COPY_LOCATION dst{}, src{};
    dst.pResource = g.readback;
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint.Footprint = {DXGI_FORMAT_R8G8B8A8_UNORM, 4, 4, 1, 256};
    src.pResource = g.rt;
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    l->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
    l->ResourceBarrier(1, &b);
    l->Close();
    if (g_beforeExecute) g_beforeExecute();
    ID3D12CommandList* lists[] = {l};
    g.queue->ExecuteCommandLists(1, lists);
    g.Wait();
    uint32_t* px = nullptr;
    g.readback->Map(0, nullptr, (void**)&px);
    uint32_t v = px[256 / 4 + 1];
    g.readback->Unmap(0, nullptr);
    g_renderLock = 0;
    return v;
}

static bool WaitState(void* pso, int want, int ms) {
    for (int t = 0; t < ms; t += 5) {
        if (PlaceholderState(pso) == want) return true;
        Sleep(5);
    }
    return PlaceholderState(pso) == want;
}

static void Setup() {
    HRESULT hr = D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&g.dev));
    if (FAILED(hr)) { printf("D3D12CreateDevice failed 0x%08lx\n", hr); exit(2); }
    HMODULE m = GetModuleHandleW(L"d3d12.dll");
    PlaceholderState = (PFN_State)(void*)GetProcAddress(m, "D3D12Async_PlaceholderState");
    Configure = (PFN_Configure)(void*)GetProcAddress(m, "D3D12Async_Configure");
    GetStats = (PFN_Stats)(void*)GetProcAddress(m, "D3D12Async_GetStats");
    if (!PlaceholderState || !Configure || !GetStats) { printf("d3d12async proxy not loaded\n"); exit(2); }

    D3D12_COMMAND_QUEUE_DESC qd{};
    g.dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&g.queue));
    g.dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g.fence));

    D3D12_ROOT_SIGNATURE_DESC rsd{};
    rsd.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    ID3DBlob *blob = nullptr, *err = nullptr;
    hr = D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err);  // exercises a pass-through stub
    if (FAILED(hr)) { printf("serialize root sig failed\n"); exit(2); }
    g.dev->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&g.rs));
    blob->Release();

    D3D12_HEAP_PROPERTIES def{D3D12_HEAP_TYPE_DEFAULT}, rb{D3D12_HEAP_TYPE_READBACK}, up{D3D12_HEAP_TYPE_UPLOAD};
    D3D12_RESOURCE_DESC td{};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = 4; td.Height = 4; td.DepthOrArraySize = 1; td.MipLevels = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    g.dev->CreateCommittedResource(&def, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, IID_PPV_ARGS(&g.rt));
    D3D12_RESOURCE_DESC bd{};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = 256 * 4; bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1;
    bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    g.dev->CreateCommittedResource(&rb, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&g.readback));
    bd.Width = 256;
    g.dev->CreateCommittedResource(&up, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&g.vb));
    void* p; g.vb->Map(0, nullptr, &p); memset(p, 0, 256); g.vb->Unmap(0, nullptr);
    D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1};
    g.dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g.rtvHeap));
    g.dev->CreateRenderTargetView(g.rt, nullptr, HeapStart(g.rtvHeap));
}

// Pipeline stream with the same contents as a graphics desc.
struct Stream {
    struct alignas(void*) RS { D3D12_PIPELINE_STATE_SUBOBJECT_TYPE t = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE; ID3D12RootSignature* v; } rs;
    struct alignas(void*) VS { D3D12_PIPELINE_STATE_SUBOBJECT_TYPE t = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS; D3D12_SHADER_BYTECODE v; } vs;
    struct alignas(void*) PSs { D3D12_PIPELINE_STATE_SUBOBJECT_TYPE t = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS; D3D12_SHADER_BYTECODE v; } ps;
    struct alignas(void*) IL { D3D12_PIPELINE_STATE_SUBOBJECT_TYPE t = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_INPUT_LAYOUT; D3D12_INPUT_LAYOUT_DESC v; } il;
    struct alignas(void*) RA { D3D12_PIPELINE_STATE_SUBOBJECT_TYPE t = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER; D3D12_RASTERIZER_DESC v; } ra;
    struct alignas(void*) TO { D3D12_PIPELINE_STATE_SUBOBJECT_TYPE t = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PRIMITIVE_TOPOLOGY; D3D12_PRIMITIVE_TOPOLOGY_TYPE v; } to;
    struct alignas(void*) RT { D3D12_PIPELINE_STATE_SUBOBJECT_TYPE t = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS; D3D12_RT_FORMAT_ARRAY v; } rt;
};

static Stream MakeStream(OwnedGfx& o) {
    Stream s{};
    s.rs.v = g.rs;
    s.vs.v = o.d.VS;
    s.ps.v = o.d.PS;
    s.il.v = o.d.InputLayout;
    s.ra.v = o.d.RasterizerState;
    s.to.v = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    s.rt.v.NumRenderTargets = 1;
    s.rt.v.RTFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    return s;
}

int main() {
    Setup();
    printf("d3d12async end-to-end test\n");
    Recorder rec;  // main thread now counts as a recording thread

    printf("[0] built-in defaults match the shipped d3d12async.ini (best Borderlands 4 run)\n");
    {
        HMODULE m = GetModuleHandleW(L"d3d12.dll");
        using PFN_Def = void(WINAPI*)(int*, int*, int*, int*, double*, int*, int*);
        auto def = (PFN_Def)(void*)GetProcAddress(m, "D3D12Async_Defaults");
        CHECK(def != nullptr, "D3D12Async_Defaults exported");
        int qf = -1, eff = -1, comp = -1, minS = -1, panel = -1, focus = -1;
        double ui = -1;
        if (def) def(&qf, &eff, &comp, &minS, &ui, &panel, &focus);
        CHECK(qf == 1 && eff == 1 && comp == 2, "QueueFullMode=placeholder, WorkerEfficiencyMode=1, ComputePipelineMode=skip (%d %d %d)", qf, eff, comp);
        CHECK(minS == 150 && ui == 2 && panel == 0 && focus == 0,
              "StartupMinSeconds=150, UiWaitMaxMs=2, SettingsPanel=0, OverlayOnlyWhenFocused=0 (%d %.0f %d %d)", minS, ui, panel, focus);
        // The checks below were written against the older baseline; set it explicitly.
        using PFN_Qm = int(WINAPI*)(int, double, int);
        using PFN_D = void(WINAPI*)(double);
        using PFN_I = void(WINAPI*)(int);
        if (auto q = (PFN_Qm)(void*)GetProcAddress(m, "D3D12Async_TestQueueMode")) q(0, 0, 0);
        if (auto u = (PFN_D)(void*)GetProcAddress(m, "D3D12Async_TestUiWait")) u(0);
        if (auto c = (PFN_I)(void*)GetProcAddress(m, "D3D12Async_TestComputeMode")) c(0);
    }

    printf("[1] baseline: real PSO renders\n");
    {
        Configure(1e9, 0, 0, 100000);  // huge budget: always synchronous
        OwnedGfx o = MakeGfx(1, 0, 0);
        ID3D12PipelineState* pso = nullptr;
        HRESULT hr = g.dev->CreateGraphicsPipelineState(&o.d, IID_PPV_ARGS(&pso));
        CHECK(SUCCEEDED(hr) && PlaceholderState(pso) == -1, "in-budget create returns the real PSO (hr=0x%08lx)", hr);
        CHECK(Render(rec, pso) == Rgba(1, 0, 0), "red triangle drawn");
        pso->Release();
    }

    printf("[2] placeholder: draws skipped while compiling, drawn once ready\n");
    {
        Configure(0, 0, 400, 100000);
        OwnedGfx o = MakeGfx(0, 1, 0);
        ID3D12PipelineState* pso = nullptr;
        uint64_t skipped0 = Stat(S_SKIPPED);
        HRESULT hr = g.dev->CreateGraphicsPipelineState(&o.d, IID_PPV_ARGS(&pso));
        Scribble(o);  // the game is allowed to free its desc memory now
        CHECK(SUCCEEDED(hr) && PlaceholderState(pso) == 0, "create returns immediately with a pending placeholder");
        CHECK(Render(rec, pso) == kBlack, "draw skipped while compiling (pixel stays black)");
        CHECK(Stat(S_SKIPPED) == skipped0 + 1, "skip counted");
        CHECK(WaitState(pso, 1, 5000), "background compile completes");
        CHECK(Render(rec, pso) == Rgba(0, 1, 0), "green triangle drawn after compile (desc was deep-copied)");
        pso->Release();
    }

    printf("[3] pipeline-stream path with deep copy\n");
    {
        Configure(0, 0, 400, 100000);
        OwnedGfx o = MakeGfx(0, 0, 1);
        Stream s = MakeStream(o);
        D3D12_PIPELINE_STATE_STREAM_DESC sd{sizeof s, &s};
        ID3D12PipelineState* pso = nullptr;
        HRESULT hr = g.dev->CreatePipelineState(&sd, IID_PPV_ARGS(&pso));
        Scribble(o);
        memset((void*)&s, 0xCD, sizeof s);
        CHECK(SUCCEEDED(hr) && PlaceholderState(pso) == 0, "stream create returns a pending placeholder");
        CHECK(Render(rec, pso) == kBlack, "draw skipped while compiling");
        CHECK(WaitState(pso, 1, 5000), "stream compile completes");
        CHECK(Render(rec, pso) == Rgba(0, 0, 1), "blue triangle drawn");
        pso->Release();
    }

    printf("[4] non-recording threads keep blocking semantics\n");
    {
        Configure(0, 0, 200, 100000);
        int state = -2;
        uint32_t px = 0;
        std::thread t([&] {
            OwnedGfx o = MakeGfx(1, 1, 0);
            ID3D12PipelineState* pso = nullptr;
            g.dev->CreateGraphicsPipelineState(&o.d, IID_PPV_ARGS(&pso));
            state = PlaceholderState(pso);
            px = Render(rec, pso);  // rendered via the main thread's list, after creation
            pso->Release();
        });
        t.join();
        CHECK(state == -1, "precache-style thread got the real PSO, no placeholder");
        CHECK(px == Rgba(1, 1, 0), "yellow triangle drawn");
    }

    printf("[5] compute PSOs are never skipped (sync by default; wait mode: background, dispatch waits)\n");
    {
        typedef void(WINAPI * PFN_Cm)(int);
        auto computeMode = (PFN_Cm)(void*)GetProcAddress(GetModuleHandleW(L"d3d12.dll"), "D3D12Async_TestComputeMode");
        Configure(0, 0, 300, 100000);  // every background compile takes 300 ms
        computeMode(1);                // ComputePipelineMode=wait
        ID3DBlob* cs = Compile("RWBuffer<uint> b : register(u0); [numthreads(1,1,1)] void main() { b[0] = 7; }", "main", "cs_5_0");
        D3D12_DESCRIPTOR_RANGE range{D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, 0};
        D3D12_ROOT_PARAMETER param{};
        param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        param.DescriptorTable = {1, &range};
        D3D12_ROOT_SIGNATURE_DESC rsd{1, &param};
        ID3DBlob* rsb = nullptr;
        D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &rsb, nullptr);
        ID3D12RootSignature* crs = nullptr;
        g.dev->CreateRootSignature(0, rsb->GetBufferPointer(), rsb->GetBufferSize(), IID_PPV_ARGS(&crs));
        struct alignas(void*) { D3D12_PIPELINE_STATE_SUBOBJECT_TYPE t = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE; ID3D12RootSignature* v; } srs;
        struct alignas(void*) { D3D12_PIPELINE_STATE_SUBOBJECT_TYPE t = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CS; D3D12_SHADER_BYTECODE v; } scs;
        struct { decltype(srs) a; decltype(scs) b; } stream{};
        stream.a.t = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE; stream.a.v = crs;
        stream.b.t = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CS; stream.b.v = {cs->GetBufferPointer(), cs->GetBufferSize()};
        D3D12_PIPELINE_STATE_STREAM_DESC sd{sizeof stream, &stream};
        ID3D12PipelineState* pso = nullptr;
        HRESULT hr = g.dev->CreatePipelineState(&sd, IID_PPV_ARGS(&pso));
        CHECK(SUCCEEDED(hr) && PlaceholderState(pso) == 0, "compute stream: create returns at once, compile runs in the background");

        D3D12_HEAP_PROPERTIES def{D3D12_HEAP_TYPE_DEFAULT}, rb{D3D12_HEAP_TYPE_READBACK};
        D3D12_RESOURCE_DESC bd{};
        bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; bd.Width = 256; bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1;
        bd.SampleDesc.Count = 1; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ID3D12Resource *buf = nullptr, *back = nullptr;
        bd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        g.dev->CreateCommittedResource(&def, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&buf));
        bd.Flags = D3D12_RESOURCE_FLAG_NONE;
        g.dev->CreateCommittedResource(&rb, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&back));
        D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
        ID3D12DescriptorHeap* heap = nullptr;
        g.dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&heap));
        D3D12_UNORDERED_ACCESS_VIEW_DESC ud{};
        ud.Format = DXGI_FORMAT_R32_UINT;
        ud.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        ud.Buffer.NumElements = 64;
        g.dev->CreateUnorderedAccessView(buf, nullptr, &ud, HeapStart(heap));
        rec.alloc->Reset();
        ID3D12GraphicsCommandList* l = rec.list;
        l->Reset(rec.alloc, nullptr);
        l->SetDescriptorHeaps(1, &heap);
        l->SetComputeRootSignature(crs);
        l->SetComputeRootDescriptorTable(0, GpuHeapStart(heap));
        l->SetPipelineState(pso);
        int stateAfterBind = PlaceholderState(pso);
        l->Dispatch(1, 1, 1);
        D3D12_RESOURCE_BARRIER br{};
        br.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        br.Transition = {buf, 0, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE};
        l->ResourceBarrier(1, &br);
        l->CopyBufferRegion(back, 0, buf, 0, 4);
        l->Close();
        ID3D12CommandList* lists[] = {l};
        g.queue->ExecuteCommandLists(1, lists);
        g.Wait();
        uint32_t* v = nullptr;
        D3D12_RANGE rr{0, 4};
        back->Map(0, &rr, (void**)&v);
        uint32_t got = v ? v[0] : 0;
        back->Unmap(0, nullptr);
        CHECK(stateAfterBind == 1 && got == 7, "binding it waited for the compile and the dispatch ran (wrote %u)", got);
        {
            // EXPERIMENTAL ComputePipelineMode=skip: dispatches with a still-compiling PSO are dropped.
            typedef void(WINAPI * PFN_Act2)(uint64_t*);
            auto act = (PFN_Act2)(void*)GetProcAddress(GetModuleHandleW(L"d3d12.dll"), "D3D12Async_GetActivity");
            computeMode(2);
            Configure(0, 0, 300, 100000);
            computeMode(2);
            ID3D12PipelineState* sk = nullptr;
            g.dev->CreatePipelineState(&sd, IID_PPV_ARGS(&sk));
            uint64_t b0[4], b1[4];
            act(b0);
            rec.alloc->Reset();
            l->Reset(rec.alloc, nullptr);
            l->SetDescriptorHeaps(1, &heap);
            l->SetComputeRootSignature(crs);
            l->SetComputeRootDescriptorTable(0, GpuHeapStart(heap));
            l->SetPipelineState(sk);
            l->Dispatch(1, 1, 1);
            l->Close();
            g.queue->ExecuteCommandLists(1, lists);
            g.Wait();
            act(b1);
            CHECK(sk && PlaceholderState(sk) == 0 && b1[3] == b0[3] + 1,
                  "skip mode: create returns at once, the dispatch is dropped while compiling (no wait)");
            WaitState(sk, 1, 5000);
            act(b0);
            rec.alloc->Reset();
            l->Reset(rec.alloc, nullptr);
            l->SetDescriptorHeaps(1, &heap);
            l->SetComputeRootSignature(crs);
            l->SetComputeRootDescriptorTable(0, GpuHeapStart(heap));
            l->SetPipelineState(sk);
            l->Dispatch(1, 1, 1);
            l->Close();
            g.queue->ExecuteCommandLists(1, lists);
            g.Wait();
            act(b1);
            CHECK(b1[3] == b0[3], "once compiled, the dispatch runs");
            if (sk) sk->Release();
        }
        computeMode(0);  // default: compute compiles on the calling thread
        ID3D12PipelineState* syncPso = nullptr;
        g.dev->CreatePipelineState(&sd, IID_PPV_ARGS(&syncPso));
        CHECK(syncPso && PlaceholderState(syncPso) == -1, "default ComputePipelineMode=sync: compiled on the calling thread");
        if (syncPso) syncPso->Release();
        buf->Release(); back->Release(); heap->Release();
        if (pso) pso->Release();
        crs->Release(); rsb->Release(); cs->Release();
    }

    printf("[6] failures: in-budget errors are returned, late errors keep draws skipped\n");
    {
        Configure(1e9, 0, 0, 100000);
        OwnedGfx bad = MakeGfx(1, 0, 1, true);
        ID3D12PipelineState* pso = nullptr;
        HRESULT hr = g.dev->CreateGraphicsPipelineState(&bad.d, IID_PPV_ARGS(&pso));
        CHECK(FAILED(hr) && !pso, "garbage shader: create fails with hr=0x%08lx, like the real runtime", hr);

        Configure(0, 0, 200, 100000);
        OwnedGfx bad2 = MakeGfx(1, 0, 1, true);
        uint64_t failed0 = Stat(S_FAILED);
        hr = g.dev->CreateGraphicsPipelineState(&bad2.d, IID_PPV_ARGS(&pso));
        CHECK(SUCCEEDED(hr) && PlaceholderState(pso) == 0, "late failure: placeholder handed out first");
        CHECK(WaitState(pso, 2, 5000) && Stat(S_FAILED) == failed0 + 1, "placeholder marked failed");
        CHECK(Render(rec, pso) == kBlack, "failed PSO: draw skipped, no crash");
        pso->Release();
    }

    printf("[7] placeholder object behaviour\n");
    {
        Configure(0, 0, 300, 100000);
        OwnedGfx o = MakeGfx(0.5f, 0.5f, 0.5f);
        ID3D12PipelineState* pso = nullptr;
        g.dev->CreateGraphicsPipelineState(&o.d, IID_PPV_ARGS(&pso));
        CHECK(PlaceholderState(pso) == 0, "pending");
        static const GUID kTag = {0x1234, 0x5678, 0x9abc, {1, 2, 3, 4, 5, 6, 7, 8}};
        uint32_t tag = 0xC0FFEE;
        pso->SetPrivateData(kTag, sizeof tag, &tag);
        pso->SetName(L"async-test");
        ID3D12Device* d2 = nullptr;
        pso->GetDevice(IID_PPV_ARGS(&d2));
        CHECK(d2 != nullptr, "GetDevice works while pending");
        if (d2) d2->Release();
        ID3D12Pageable* pg = pso;
        HRESULT hr = g.dev->MakeResident(1, &pg);
        CHECK(SUCCEEDED(hr) && PlaceholderState(pso) == 1, "MakeResident waits for and passes the real PSO (hr=0x%08lx)", hr);
        uint32_t got = 0;
        UINT size = sizeof got;
        hr = pso->GetPrivateData(kTag, &size, &got);
        CHECK(SUCCEEDED(hr) && got == tag, "private data set while pending survives onto the real PSO");
        pso->Release();

        Configure(0, 0, 300, 100000);
        OwnedGfx o2 = MakeGfx(0.25f, 0.5f, 0.75f);
        g.dev->CreateGraphicsPipelineState(&o2.d, IID_PPV_ARGS(&pso));
        ID3DBlob* blob = nullptr;
        hr = pso->GetCachedBlob(&blob);
        CHECK(PlaceholderState(pso) == 1, "GetCachedBlob waits for the real PSO (hr=0x%08lx)", hr);
        if (blob) blob->Release();
        pso->Release();

        Configure(0, 0, 200, 100000);
        OwnedGfx o3 = MakeGfx(0.1f, 0.2f, 0.3f);
        g.dev->CreateGraphicsPipelineState(&o3.d, IID_PPV_ARGS(&pso));
        pso->Release();  // released while still compiling
        Sleep(500);
        CHECK(true, "releasing a placeholder mid-compile is safe");
    }

    printf("[8] CreateCommandList with a pending initial PSO\n");
    {
        Configure(0, 0, 400, 100000);
        OwnedGfx o = MakeGfx(1, 0, 0.5f);
        ID3D12PipelineState* pso = nullptr;
        g.dev->CreateGraphicsPipelineState(&o.d, IID_PPV_ARGS(&pso));
        ID3D12CommandAllocator* a = nullptr;
        ID3D12GraphicsCommandList* l = nullptr;
        g.dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&a));
        HRESULT hr = g.dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, a, pso, IID_PPV_ARGS(&l));
        CHECK(SUCCEEDED(hr), "list created with a pending initial PSO");
        CHECK(Render(rec, nullptr, false, l) == kBlack, "draw with the pending initial PSO skipped");
        WaitState(pso, 1, 5000);
        a->Reset();
        l->Reset(a, pso);
        CHECK(Render(rec, nullptr, false, l) == Rgba(1, 0, 0.5f), "after compile, Reset with the placeholder draws normally");
        l->Release(); a->Release(); pso->Release();
    }

    printf("[9] concurrency: 4 recording threads x 24 PSOs\n");
    {
        Configure(0, 0, 0, 100000);
        std::atomic<int> ok{0}, total{0}, placeholders{0};
        std::vector<std::thread> ts;
        for (int t = 0; t < 4; ++t)
            ts.emplace_back([&, t] {
                Recorder r;
                for (int i = 0; i < 24; ++i) {
                    float c = (t * 24 + i + 1) / 100.0f;
                    OwnedGfx o = MakeGfx(c, 1 - c, 0.5f);
                    ID3D12PipelineState* pso = nullptr;
                    if (FAILED(g.dev->CreateGraphicsPipelineState(&o.d, IID_PPV_ARGS(&pso)))) continue;
                    Scribble(o);
                    if (PlaceholderState(pso) == 0) placeholders++;
                    uint32_t px = Render(r, pso);
                    bool skippedOrRight = px == kBlack || Near(px, Rgba(c, 1 - c, 0.5f));
                    WaitState(pso, 1, 10000);
                    uint32_t px2 = Render(r, pso);
                    if (skippedOrRight && Near(px2, Rgba(c, 1 - c, 0.5f))) ok++;
                    else printf("    mismatch t%d i%d first=%08x after=%08x want=%08x state=%d\n", t, i, px, px2, Rgba(c, 1 - c, 0.5f), PlaceholderState(pso));
                    total++;
                    pso->Release();
                }
            });
        for (auto& t : ts) t.join();
        CHECK(ok == 96 && total == 96, "%d/%d PSOs drew the right colour (%d were placeholders)", ok.load(), total.load(), placeholders.load());
    }

    printf("[10] AsyncThreads=all: non-recording threads get placeholders too\n");
    {
        Configure(0, 1, 300, 100000);
        int state = -2;
        uint32_t px = 1;
        std::thread t([&] {
            OwnedGfx o = MakeGfx(0, 1, 1);
            ID3D12PipelineState* pso = nullptr;
            g.dev->CreateGraphicsPipelineState(&o.d, IID_PPV_ARGS(&pso));
            state = PlaceholderState(pso);
            px = Render(rec, pso);
            WaitState(pso, 1, 5000);
            if (Render(rec, pso) != Rgba(0, 1, 1)) px = 1;
            pso->Release();
        });
        t.join();
        CHECK(state == 0 && px == kBlack, "worker-thread create returned a placeholder; draw skipped, then drawn");
    }

    printf("[11] backpressure: a full queue makes background threads compile synchronously\n");
    {
        Sleep(1500);  // let earlier work drain
        Configure(0, 1, 400, 2);
        int sync = 0, async = 0;
        std::thread t([&] {
            std::vector<ID3D12PipelineState*> v;
            for (int i = 0; i < 6; ++i) {
                OwnedGfx o = MakeGfx(0.9f, 0.1f * i, 0.2f);
                ID3D12PipelineState* pso = nullptr;
                g.dev->CreateGraphicsPipelineState(&o.d, IID_PPV_ARGS(&pso));
                (PlaceholderState(pso) == -1 ? sync : async)++;
                v.push_back(pso);
            }
            for (auto* p : v) { WaitState(p, 1, 10000); p->Release(); }
        });
        t.join();
        CHECK(sync >= 1 && async >= 2, "%d deferred, %d compiled synchronously once the queue held 2", async, sync);
    }

    printf("[12] bind boost: a placeholder the game draws with jumps the queue\n");
    {
        Sleep(1500);
        Configure(0, 0, 250, 100000);
        std::vector<ID3D12PipelineState*> v;
        std::vector<OwnedGfx> keep;
        for (int i = 0; i < 8; ++i) {
            keep.push_back(MakeGfx(0.1f * i, 0.3f, 0.6f));
            ID3D12PipelineState* pso = nullptr;
            g.dev->CreateGraphicsPipelineState(&keep.back().d, IID_PPV_ARGS(&pso));
            v.push_back(pso);
        }
        Render(rec, v[7]);  // the game draws with the last one: skipped, and boosted
        bool lastReady = WaitState(v[7], 1, 5000);
        int stillPending = 0;
        for (int i = 2; i < 7; ++i) stillPending += PlaceholderState(v[i]) == 0;
        CHECK(lastReady && stillPending >= 2, "last-queued PSO finished first; %d earlier ones were still compiling", stillPending);
        for (auto* p : v) { WaitState(p, 1, 10000); p->Release(); }
    }

    printf("[13] residency priority and bundles\n");
    {
        Sleep(1000);
        Configure(0, 0, 300, 100000);
        OwnedGfx o = MakeGfx(0.2f, 0.4f, 0.6f);
        ID3D12PipelineState* pso = nullptr;
        g.dev->CreateGraphicsPipelineState(&o.d, IID_PPV_ARGS(&pso));
        ID3D12Device1* d1 = nullptr;
        g.dev->QueryInterface(IID_PPV_ARGS(&d1));
        ID3D12Pageable* pg = pso;
        D3D12_RESIDENCY_PRIORITY pr = D3D12_RESIDENCY_PRIORITY_HIGH;
        HRESULT hr = d1->SetResidencyPriority(1, &pg, &pr);
        CHECK(PlaceholderState(pso) == 1, "SetResidencyPriority passes the real PSO, never the placeholder (hr=0x%08lx)", hr);
        d1->Release();
        pso->Release();

        Configure(0, 0, 300, 100000);
        OwnedGfx ob = MakeGfx(0.6f, 0.2f, 0.9f);
        g.dev->CreateGraphicsPipelineState(&ob.d, IID_PPV_ARGS(&pso));
        ID3D12CommandAllocator* ba = nullptr;
        ID3D12GraphicsCommandList* bundle = nullptr;
        g.dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_BUNDLE, IID_PPV_ARGS(&ba));
        hr = ba ? g.dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_BUNDLE, ba, nullptr, IID_PPV_ARGS(&bundle)) : E_FAIL;
        if (FAILED(hr) || !bundle) {
            printf("  SKIP  bundles not supported by this driver (hr=0x%08lx)\n", hr);
        } else {
            bundle->Close();
            ba->Reset();
            bundle->Reset(ba, pso);  // pending placeholder as a bundle's initial state
            CHECK(PlaceholderState(pso) == 1, "bundle Reset waited for the real PSO instead of skipping");
            bundle->Close();
            bundle->Release();
        }
        if (ba) ba->Release();
        pso->Release();
    }

    printf("[14] another layer swaps in a copied vtable (overlay / Streamline style)\n");
    {
        Configure(1e9, 0, 0, 100000);
        Recorder r2;
        static void** origVt;
        static ID3D12GraphicsCommandList* swapped;
        origVt = *reinterpret_cast<void***>(r2.list);
        swapped = r2.list;
        static void* copyVt[160];
        memcpy(copyVt, origVt, sizeof copyVt);
        *reinterpret_cast<void***>(r2.list) = copyVt;  // the swap a vtable-hooking layer does
        void** devVt = *reinterpret_cast<void***>(g.dev);
        static void* devCopy[120];
        memcpy(devCopy, devVt, sizeof devCopy);
        *reinterpret_cast<void***>(g.dev) = devCopy;
        OwnedGfx o = MakeGfx(0.8f, 0.4f, 0.2f);
        ID3D12PipelineState* pso = nullptr;
        HRESULT hr = g.dev->CreateGraphicsPipelineState(&o.d, IID_PPV_ARGS(&pso));
        // Restore the driver's own vtable before submission, as a real layer's wrapper would.
        g_beforeExecute = [] { *reinterpret_cast<void***>(swapped) = origVt; };
        uint32_t px = SUCCEEDED(hr) ? Render(r2, pso) : 0;
        g_beforeExecute = nullptr;
        *reinterpret_cast<void***>(r2.list) = origVt;
        *reinterpret_cast<void***>(g.dev) = devVt;
        CHECK(SUCCEEDED(hr) && Near(px, Rgba(0.8f, 0.4f, 0.2f)), "hooks reached through copied vtables still call the originals");
        if (pso) pso->Release();
    }

    printf("[15] driver-style private vtable with entries straight into the driver\n");
    {
        Configure(0, 0, 300, 100000);
        Recorder r3;
        OwnedGfx o = MakeGfx(0.3f, 0.6f, 0.9f);
        ID3D12PipelineState* pso = nullptr;
        g.dev->CreateGraphicsPipelineState(&o.d, IID_PPV_ARGS(&pso));
        void** origVt = *reinterpret_cast<void***>(r3.list);
        static void* copyVt[160];
        memcpy(copyVt, origVt, sizeof copyVt);
        copyVt[25] = (void*)&FakeDriverSetPSO;  // SetPipelineState -> "driver"
        copyVt[12] = (void*)&FakeDriverDraw;    // DrawInstanced    -> "driver"
        *reinterpret_cast<void***>(r3.list) = copyVt;
        r3.alloc->Reset();
        r3.list->Reset(r3.alloc, nullptr);  // first hook call through the copy: discovers and hooks it
        CHECK(copyVt[25] != (void*)&FakeDriverSetPSO && copyVt[12] != (void*)&FakeDriverDraw, "copy's driver entries were hooked on discovery");
        g_fakeSet = 0; g_fakeDraw = 0; g_fakeSawPlaceholder = 0;  // the runtime's Reset calls SetPipelineState(null) itself
        r3.list->SetPipelineState(pso);
        r3.list->DrawInstanced(3, 1, 0, 0);
        CHECK(PlaceholderState(pso) == 0 && g_fakeSet == 0 && g_fakeDraw == 0, "while compiling: neither SetPipelineState nor the draw reached the driver");
        WaitState(pso, 1, 5000);
        r3.list->SetPipelineState(pso);
        r3.list->DrawInstanced(3, 1, 0, 0);
        CHECK(g_fakeSet == 1 && g_fakeDraw == 1 && g_fakeSawPlaceholder == 0, "once ready: the driver got the real PSO and the draw");
        r3.list->Close();
        *reinterpret_cast<void***>(r3.list) = origVt;
        pso->Release();
    }

    printf("[16] pipeline library: StorePipeline of a compiling PSO doesn't block\n");
    {
        using PFN_Hook = void(WINAPI*)(IUnknown*);
        auto hookLib = (PFN_Hook)(void*)GetProcAddress(GetModuleHandleW(L"d3d12.dll"), "D3D12Async_TestHookLibrary");
        static FakeLibrary lib;  // Wine's D3D12 has no pipeline libraries: a minimal one stands in
        hookLib(&lib);
        ID3D12PipelineLibrary* volatile L = &lib;  // call through the vtable, as a game does
        Configure(0, 0, 400, 100000);
        OwnedGfx o = MakeGfx(0.7f, 0.1f, 0.4f);
        ID3D12PipelineState* pso = nullptr;
        g.dev->CreateGraphicsPipelineState(&o.d, IID_PPV_ARGS(&pso));
        uint64_t waits0 = Stat(S_FORCED);
        LARGE_INTEGER f, t0, t1;
        QueryPerformanceFrequency(&f);
        QueryPerformanceCounter(&t0);
        HRESULT hr = L->StorePipeline(L"async-store", pso);
        QueryPerformanceCounter(&t1);
        double ms = (t1.QuadPart - t0.QuadPart) * 1000.0 / f.QuadPart;
        size_t storedNow;
        { std::lock_guard<std::mutex> lk(lib.mu); storedNow = lib.stored.size(); }
        CHECK(SUCCEEDED(hr) && PlaceholderState(pso) == 0 && Stat(S_FORCED) == waits0 && storedNow == 0 && ms < 50,
              "StorePipeline returned in %.2f ms while compiling: no forced wait, store deferred", ms);
        WaitState(pso, 1, 5000);
        Sleep(50);
        ID3D12PipelineState* got = nullptr;
        { std::lock_guard<std::mutex> lk(lib.mu); if (lib.stored.size() == 1) got = lib.stored[0].second; }
        CHECK(got && PlaceholderState(got) == -1, "after the compile the real PSO (not the placeholder) was stored");
        CHECK(L->StorePipeline(L"async-store", pso) == E_INVALIDARG, "a later store of a ready PSO passes straight through");
        Configure(0, 0, 0, 100000);
        OwnedGfx bad = MakeGfx(0.2f, 0.2f, 0.2f, true);
        ID3D12PipelineState* badPso = nullptr;
        Configure(0, 0, 200, 100000);
        g.dev->CreateGraphicsPipelineState(&bad.d, IID_PPV_ARGS(&badPso));
        L->StorePipeline(L"failed-one", badPso);
        WaitState(badPso, 2, 5000);
        Sleep(50);
        bool storedFailed = false;
        { std::lock_guard<std::mutex> lk(lib.mu); for (auto& e : lib.stored) if (e.first == L"failed-one") storedFailed = true; }
        CHECK(!storedFailed, "a PSO whose compile failed is never stored");
        {
            typedef void(WINAPI * PFN_Act)(uint64_t*);
            auto act = (PFN_Act)(void*)GetProcAddress(GetModuleHandleW(L"d3d12.dll"), "D3D12Async_GetActivity");
            uint64_t a0[4], a1[4];
            act(a0);
            ID3D12PipelineState* none = nullptr;
            HRESULT lh = L->LoadGraphicsPipeline(L"missing", &bad.d, IID_PPV_ARGS(&none));
            act(a1);
            CHECK(lh == E_INVALIDARG && a1[2] == a0[2] + 1, "library loads pass through unchanged and are counted");
        }
        if (badPso) badPso->Release();
        pso->Release();
    }

    printf("[17] startup phase: normal compiles during the precompile flood, async after it goes quiet\n");
    {
        typedef void(WINAPI * PFN_Phase)(int, int, int, int, int);
        typedef int(WINAPI * PFN_Active)();
        HMODULE m = GetModuleHandleW(L"d3d12.dll");
        auto phase = (PFN_Phase)(void*)GetProcAddress(m, "D3D12Async_TestPhase");
        auto active = (PFN_Active)(void*)GetProcAddress(m, "D3D12Async_StartupActive");
        Configure(0, 1, 200, 100000);
        phase(1, 0, 6, 2, 0);  // phase over after 6 PSOs and then 2 seconds with none
        CHECK(active() == 1, "startup phase is active from launch");
        bool allReal = true;
        auto tick = [&] {  // a frame: Close drives the phase check
            ID3D12GraphicsCommandList* cl = nullptr;
            rec.alloc->Reset();
            g.dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, rec.alloc, nullptr, IID_PPV_ARGS(&cl));
            cl->Close();
            cl->Release();
        };
        for (int i = 0; i < 8; ++i) {
            OwnedGfx o = MakeGfx(0.1f * i, 0.5f, 0.5f);
            o.d.RTVFormats[0] = DXGI_FORMAT_R16G16B16A16_FLOAT;
            ID3D12PipelineState* pso = nullptr;
            g.dev->CreateGraphicsPipelineState(&o.d, IID_PPV_ARGS(&pso));
            if (!pso || PlaceholderState(pso) != -1) allReal = false;
            if (pso) pso->Release();
            tick();
        }
        CHECK(allReal && active() == 1, "PSOs created during the flood compile normally (no placeholders)");
        CHECK(active() == 1, "a burst right after launch doesn't end the phase: time before it never counts as quiet");
        for (int i = 0; i < 20 && active(); ++i) { Sleep(300); tick(); }
        CHECK(active() == 0, "phase ends by itself once creations go quiet");
        {
            typedef void(WINAPI * PFN_Cpu)(int);
            auto cpu = (PFN_Cpu)(void*)GetProcAddress(m, "D3D12Async_TestCpu");
            phase(1, 0, 0, 1, 0);  // no PSOs needed, 1 quiet second
            cpu(1);                // any CPU use above 1% counts as busy
            std::atomic<bool> stop{false};
            std::thread burn([&] { while (!stop) {} });  // the "precompile": one core flat out
            for (int i = 0; i < 12; ++i) { Sleep(250); tick(); }
            bool heldByCpu = active() == 1;
            stop = true;
            burn.join();
            cpu(1000);  // threshold out of reach: CPU never busy
            for (int i = 0; i < 12 && active(); ++i) { Sleep(250); tick(); }
            CHECK(heldByCpu && active() == 0, "high game CPU keeps the phase running; it ends once CPU drops");
            // Once over, it never comes back: CPU spikes and a pipeline burst in gameplay don't re-enter it.
            auto cpuNoReset = (PFN_Cpu)(void*)GetProcAddress(m, "D3D12Async_TestCpuNoReset");
            cpuNoReset(1);
            stop = false;
            std::thread burn2([&] { while (!stop) {} });
            for (int i = 0; i < 40; ++i) {
                OwnedGfx o = MakeGfx(0.01f * i, 0.2f, 0.3f);
                o.d.RTVFormats[0] = DXGI_FORMAT_R16G16B16A16_FLOAT;
                ID3D12PipelineState* p = nullptr;
                g.dev->CreateGraphicsPipelineState(&o.d, IID_PPV_ARGS(&p));
                if (p) { WaitState(p, 1, 5000); p->Release(); }
            }
            for (int i = 0; i < 12; ++i) { Sleep(250); tick(); }
            stop = true;
            burn2.join();
            CHECK(active() == 0, "the startup phase never re-enters during gameplay (CPU spike + 40-PSO burst)");
        }
        {
            // The phase can't end before the precompile itself has been seen after the first frame
            // (Borderlands 4: ~20 quiet seconds of intro between engine start-up and its compile screen).
            typedef void(WINAPI * PFN_Flood)(int, int);
            typedef void(WINAPI * PFN_V)();
            typedef void(WINAPI * PFN_Cpu2)(int);
            auto flood = (PFN_Flood)(void*)GetProcAddress(m, "D3D12Async_TestFlood");
            auto firstFrame = (PFN_V)(void*)GetProcAddress(m, "D3D12Async_TestFirstFrame");
            auto cpuNoReset2 = (PFN_Cpu2)(void*)GetProcAddress(m, "D3D12Async_TestCpuNoReset");
            phase(1, 0, 0, 1, 0);
            flood(3, 0);  // 3 busy seconds after the first frame needed; no fallback
            firstFrame();
            for (int i = 0; i < 10; ++i) { Sleep(250); tick(); }
            bool heldBeforeFlood = active() == 1;
            cpuNoReset2(1);
            std::atomic<bool> stop2{false};
            std::thread burn3([&] { while (!stop2) {} });
            for (int i = 0; i < 20; ++i) { Sleep(250); tick(); }  // ~5 s of "precompile"
            stop2 = true;
            burn3.join();
            cpuNoReset2(1000);
            for (int i = 0; i < 12 && active(); ++i) { Sleep(250); tick(); }
            CHECK(heldBeforeFlood && active() == 0, "quiet intro before the precompile doesn't end the phase; quiet after it does");
            phase(1, 0, 0, 1, 0);
            flood(3, 1);  // warm cache: no precompile at all, fallback after 1 s
            firstFrame();
            for (int i = 0; i < 12 && active(); ++i) { Sleep(250); tick(); }
            CHECK(active() == 0, "no precompile ever starts (warm cache): the phase still ends after StartupFloodWaitSeconds");
        }
        OwnedGfx o = MakeGfx(0.3f, 0.3f, 0.9f);
        o.d.RTVFormats[0] = DXGI_FORMAT_R16G16B16A16_FLOAT;
        ID3D12PipelineState* pso = nullptr;
        g.dev->CreateGraphicsPipelineState(&o.d, IID_PPV_ARGS(&pso));
        CHECK(pso && PlaceholderState(pso) == 0, "after the phase, slow compiles get placeholders again (no restart)");
        if (pso) { WaitState(pso, 1, 5000); pso->Release(); }
    }

    printf("[18] UI-type pipelines (8-bit targets, no depth): never skipped\n");
    {
        typedef void(WINAPI * PFN_Phase)(int, int, int, int, int);
        auto phase = (PFN_Phase)(void*)GetProcAddress(GetModuleHandleW(L"d3d12.dll"), "D3D12Async_TestPhase");
        Configure(0, 1, 200, 100000);
        phase(0, 1, 64, 5, 3);
        OwnedGfx ui = MakeGfx(0.9f, 0.9f, 0.1f);
        ID3D12PipelineState* p1 = nullptr;
        g.dev->CreateGraphicsPipelineState(&ui.d, IID_PPV_ARGS(&p1));
        CHECK(p1 && PlaceholderState(p1) == -1, "8-bit target without depth: real PSO");
        CHECK(p1 && Near(Render(rec, p1), Rgba(0.9f, 0.9f, 0.1f)), "and it draws on first use");
        OwnedGfx ui2 = MakeGfx(0.2f, 0.9f, 0.9f);
        Stream st = MakeStream(ui2);
        D3D12_PIPELINE_STATE_STREAM_DESC sd{sizeof st, &st};
        ID3D12Device2* d2 = nullptr;
        ID3D12PipelineState* p2 = nullptr;
        if (SUCCEEDED(g.dev->QueryInterface(IID_PPV_ARGS(&d2)))) { d2->CreatePipelineState(&sd, IID_PPV_ARGS(&p2)); d2->Release(); }
        CHECK(p2 && PlaceholderState(p2) == -1, "same through the pipeline-stream path");
        OwnedGfx scene = MakeGfx(0.5f, 0.1f, 0.5f);
        scene.d.DSVFormat = DXGI_FORMAT_D32_FLOAT;
        ID3D12PipelineState* p3 = nullptr;
        g.dev->CreateGraphicsPipelineState(&scene.d, IID_PPV_ARGS(&p3));
        CHECK(p3 && PlaceholderState(p3) == 0, "with a depth target it is a scene pipeline: placeholder");
        OwnedGfx hdr = MakeGfx(0.5f, 0.5f, 0.1f);
        hdr.d.RTVFormats[0] = DXGI_FORMAT_R11G11B10_FLOAT;
        ID3D12PipelineState* p4 = nullptr;
        g.dev->CreateGraphicsPipelineState(&hdr.d, IID_PPV_ARGS(&p4));
        CHECK(p4 && PlaceholderState(p4) == 0, "HDR target: placeholder");
        if (p3) WaitState(p3, 1, 5000);
        if (p4) WaitState(p4, 1, 5000);
        for (auto* p : {p1, p2, p3, p4}) if (p) p->Release();

        // Default mode "wait": a burst of UI pipelines compiles in the background in parallel, and a
        // draw that needs one waits (compiling it on the drawing thread if no worker has started it).
        typedef void(WINAPI * PFN_Ui)(int);
        auto uiMode = (PFN_Ui)(void*)GetProcAddress(GetModuleHandleW(L"d3d12.dll"), "D3D12Async_TestUiMode");
        uiMode(1);
        Configure(0, 1, 200, 100000);
        uiMode(1);
        ID3D12PipelineState* burst[12] = {};
        LARGE_INTEGER f, t0, t1;
        QueryPerformanceFrequency(&f);
        QueryPerformanceCounter(&t0);
        for (int i = 0; i < 12; ++i) {
            OwnedGfx u = MakeGfx(0.05f * i, 0.8f, 0.3f);
            g.dev->CreateGraphicsPipelineState(&u.d, IID_PPV_ARGS(&burst[i]));
        }
        QueryPerformanceCounter(&t1);
        double createMs = (t1.QuadPart - t0.QuadPart) * 1000.0 / f.QuadPart;
        CHECK(burst[11] && PlaceholderState(burst[11]) == 0 && createMs < 1000,
              "wait mode: 12 UI creates return at once (%.0f ms) instead of compiling one by one", createMs);
        QueryPerformanceCounter(&t0);
        uint32_t px = burst[11] ? Render(rec, burst[11]) : 0;
        QueryPerformanceCounter(&t1);
        double drawMs = (t1.QuadPart - t0.QuadPart) * 1000.0 / f.QuadPart;
        CHECK(Near(px, Rgba(0.55f, 0.8f, 0.3f)), "the last one's first draw waits and draws (never skipped)");
        CHECK(drawMs < 1200, "it didn't wait behind the whole queue: compiled on the drawing thread (%.0f ms)", drawMs);
        for (auto* p : burst) if (p) { WaitState(p, 1, 5000); p->Release(); }
        Configure(0, 1, 0, 100000);
    }

    printf("[19] present hook: startup frame cap and per-run logs\n");
    {
        typedef void(WINAPI * PFN_Phase)(int, int, int, int, int);
        typedef void(WINAPI * PFN_Startup)(int);
        typedef uint64_t(WINAPI * PFN_Count)();
        HMODULE m = GetModuleHandleW(L"d3d12.dll");
        auto phase = (PFN_Phase)(void*)GetProcAddress(m, "D3D12Async_TestPhase");
        auto startup = (PFN_Startup)(void*)GetProcAddress(m, "D3D12Async_TestStartup");
        auto count = (PFN_Count)(void*)GetProcAddress(m, "D3D12Async_PresentCount");

        // Old runs that a new run must prune (RunLogsKeepOn=1 keeps only the new one); OFF runs stay.
        wchar_t dll[MAX_PATH];
        GetModuleFileNameW(m, dll, MAX_PATH);
        std::wstring dir(dll);
        dir = dir.substr(0, dir.find_last_of(L"\\/")) + L"\\d3d12async-runs";
        CreateDirectoryW(dir.c_str(), nullptr);
        for (const wchar_t* n : {L"2000-01-01_00-00-00_dll-ON.csv", L"2000-01-01_00-00-00_dll-ON.txt", L"2000-01-02_00-00-00_dll-ON.csv",
                                 L"2000-01-02_00-00-00_dll-ON.txt", L"2000-01-03_00-00-00_dll-OFF.csv", L"2000-01-03_00-00-00_dll-OFF.txt"}) {
            FILE* f = _wfopen((dir + L"\\" + n).c_str(), L"wb");
            if (f) { fputs("old", f); fclose(f); }
        }
        WNDCLASSW wc{};
        wc.lpfnWndProc = DefWindowProcW;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"pso_test_wnd";
        RegisterClassW(&wc);
        HWND wnd = CreateWindowExW(0, wc.lpszClassName, L"t", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 0, 0, 900, 560, nullptr, nullptr, wc.hInstance, nullptr);
        IDXGIFactory2* fac = nullptr;
        IDXGISwapChain1* sc = nullptr;
        CreateDXGIFactory2(0, IID_PPV_ARGS(&fac));
        DXGI_SWAP_CHAIN_DESC1 d{};
        d.Width = 128; d.Height = 128; d.Format = DXGI_FORMAT_R8G8B8A8_UNORM; d.SampleDesc.Count = 1;
        d.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; d.BufferCount = 2; d.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        HRESULT hr = fac ? fac->CreateSwapChainForHwnd(g.queue, wnd, &d, nullptr, nullptr, &sc) : E_FAIL;
        CHECK(SUCCEEDED(hr) && sc, "swap chain created (hr=0x%08lx)", (unsigned long)hr);
        if (sc) {
            auto timed = [&](int n) {
                LARGE_INTEGER f, t0, t1;
                QueryPerformanceFrequency(&f);
                QueryPerformanceCounter(&t0);
                for (int i = 0; i < n; ++i) sc->Present(0, 0);
                QueryPerformanceCounter(&t1);
                return (t1.QuadPart - t0.QuadPart) * 1000.0 / f.QuadPart;
            };
            uint64_t c0 = count();
            phase(0, 0, 1000000, 60, 0);  // startup phase off: uncapped
            double free = timed(10);
            CHECK(count() - c0 == 10, "every Present reached the hook (%llu)", (unsigned long long)(count() - c0));
            phase(1, 0, 1000000, 60, 0);  // startup phase on, can't end during the test
            startup(20);                  // 20 fps cap
            double capped = timed(11);    // 10 intervals of >= 50 ms
            CHECK(capped >= 480, "startup phase: 20 fps cap holds frames (%.0f ms for 10 frames; uncapped %.0f ms)", capped, free);
            phase(0, 0, 0, 1, 1000000);  // detection ends after 1 quiet second: later frames count as gameplay
            double after = timed(10);
            CHECK(after < capped / 2, "cap lifts when the startup phase is over (%.0f ms)", after);
            for (int i = 0; i < 25; ++i) { sc->Present(0, 0); Sleep(100); }  // 2.5 s of frames for the writer
            Sleep(1500);
            auto exists = [&](const std::wstring& pat) {
                WIN32_FIND_DATAW fd;
                HANDLE h = FindFirstFileW((dir + L"\\" + pat).c_str(), &fd);
                int n = 0;
                if (h != INVALID_HANDLE_VALUE) { do ++n; while (FindNextFileW(h, &fd)); FindClose(h); }
                return n;
            };
            CHECK(exists(L"*_dll-ON.csv") == 1 && exists(L"*_dll-ON.txt") == 1, "this run logged as dll-ON; older ON runs pruned");
            CHECK(exists(L"2000-01-03_00-00-00_dll-OFF.csv") == 1, "OFF runs are kept separately");
            std::string idx;
            FILE* f = _wfopen((dir + L"\\runs.txt").c_str(), L"rb");
            if (f) { char b[8192]; size_t r = fread(b, 1, sizeof b - 1, f); b[r] = 0; idx = b; fclose(f); }
            CHECK(idx.find("#1  LATEST") != std::string::npos && idx.find("dll-ON") < idx.find("dll-OFF.txt") &&
                  idx.find("frames: gameplay") != std::string::npos && idx.find("avg") != std::string::npos &&
                  idx.find("frames: startup") == std::string::npos,
                  "runs.txt: newest run first, marked LATEST, gameplay frame stats only (startup excluded)");
            {
                auto readFirst = [&](const wchar_t* pat) {
                    std::string out;
                    WIN32_FIND_DATAW fd;
                    HANDLE h = FindFirstFileW((dir + L"\\" + pat).c_str(), &fd);
                    if (h == INVALID_HANDLE_VALUE) return out;
                    FILE* ff = _wfopen((dir + L"\\" + fd.cFileName).c_str(), L"rb");
                    FindClose(h);
                    if (!ff) return out;
                    std::vector<char> b(1 << 20);
                    size_t r = fread(b.data(), 1, b.size() - 1, ff);
                    fclose(ff);
                    return std::string(b.data(), r);
                };
                std::string fr = readFirst(L"*_dll-ON_frames.csv");
                int gameplayRows = 0, startupRows = 0;
                for (size_t pos = fr.find('\n'); pos != std::string::npos && pos + 1 < fr.size(); pos = fr.find('\n', pos + 1)) {
                    size_t eol = fr.find('\r', pos + 1);
                    if (eol == std::string::npos) break;
                    char last = fr[eol - 1];
                    (last == '1' ? gameplayRows : startupRows)++;
                }
                CHECK(fr.rfind("time_s,frame_ms,gameplay", 0) == 0 && gameplayRows >= 8 && startupRows >= 10,
                      "_frames.csv has every frame, gameplay and startup marked (%d gameplay, %d startup)", gameplayRows, startupRows);
                std::string pc = readFirst(L"*_dll-ON.csv");
                size_t hdrEnd = pc.find('\r');
                bool cols = pc.find("min_ms,p999_ms,low1_fps,low01_fps") < hdrEnd;
                bool filled = false;  // a gameplay row with all four new columns filled
                for (size_t pos = pc.find(",game,"); pos != std::string::npos; pos = pc.find(",game,", pos + 1)) {
                    size_t eol = pc.find('\r', pos);
                    std::string row = pc.substr(pos, eol - pos);
                    if (row.size() > 4 && row.find(",,,", row.size() - 4) == std::string::npos && row.back() != ',') { filled = true; break; }
                }
                CHECK(cols && filled, "per-second CSV has min_ms, p999_ms, 1%% and 0.1%% low columns, filled in gameplay");
                CHECK(idx.find("fps       max ") != std::string::npos, "run summary has the max fps line");
            }
            printf("[20] live tracker: hitches are attributed to their cause\n");
            {
                typedef int(WINAPI * PFN_Hitch)(double*, int*, int*, double*);
                auto last = (PFN_Hitch)(void*)GetProcAddress(m, "D3D12Async_LastHitch");
                phase(0, 0, 0, 1, 1000000);
                for (int i = 0; i < 15; ++i) { sc->Present(0, 0); Sleep(100); }  // startup detection ends; gameplay
                double fms = 0, lms = 0;
                int kind = -1, cause = -1;
                sc->Present(0, 0);
                Sleep(150);  // a slow frame with no pipeline work
                sc->Present(0, 0);
                int n1 = last(&fms, &kind, &cause, &lms);
                CHECK(n1 >= 1 && fms >= 140 && kind == 0, "slow frame without pipeline work: hitch, 'not pipeline' (%.0f ms, kind %d)", fms, kind);
                Configure(0, 1, 300, 100000);  // background compiles take 300 ms
                OwnedGfx o = MakeGfx(0.4f, 0.1f, 0.7f);
                o.d.RTVFormats[0] = DXGI_FORMAT_R16G16B16A16_FLOAT;
                ID3D12PipelineState* p = nullptr;
                g.dev->CreateGraphicsPipelineState(&o.d, IID_PPV_ARGS(&p));
                sc->Present(0, 0);
                ID3DBlob* blob = nullptr;
                if (p) p->GetCachedBlob(&blob);  // needs the real object: forced wait ~300 ms
                if (blob) blob->Release();
                sc->Present(0, 0);
                int n2 = last(&fms, &kind, &cause, &lms);
                CHECK(n2 == n1 + 1 && kind == 2 && cause == 9 && lms >= 200,
                      "frame blocked by a forced wait: attributed to it (%.0f ms frame, longest call %.0f ms, cause %d)", fms, lms, cause);
                if (p) p->Release();
                Sleep(2500);
                std::string hc;
                WIN32_FIND_DATAW fd;
                HANDLE fh = FindFirstFileW((dir + L"\\*_dll-ON_hitches.csv").c_str(), &fd);
                if (fh != INVALID_HANDLE_VALUE) {
                    FILE* f = _wfopen((dir + L"\\" + fd.cFileName).c_str(), L"rb");
                    if (f) { char b[8192]; size_t r = fread(b, 1, sizeof b - 1, f); b[r] = 0; hc = b; fclose(f); }
                    FindClose(fh);
                }
                CHECK(hc.find("not-pipeline") != std::string::npos && hc.find("forced wait") != std::string::npos,
                      "both hitches are in the run's _hitches.csv with their cause");
            }
            sc->Release();
        }
        if (fac) fac->Release();
        DestroyWindow(wnd);
    }

    printf("[21] a burst of creates doesn't pay BudgetMs each once compiles are queued\n");
    {
        Configure(20, 1, 100, 100000);  // budget 20 ms, every compile takes 100 ms
        ID3D12PipelineState* ps[10] = {};
        LARGE_INTEGER f, t0, t1;
        QueryPerformanceFrequency(&f);
        QueryPerformanceCounter(&t0);
        for (int i = 0; i < 10; ++i) {
            OwnedGfx o = MakeGfx(0.07f * i, 0.4f, 0.6f);
            o.d.RTVFormats[0] = DXGI_FORMAT_R16G16B16A16_FLOAT;
            g.dev->CreateGraphicsPipelineState(&o.d, IID_PPV_ARGS(&ps[i]));
        }
        QueryPerformanceCounter(&t1);
        double ms = (t1.QuadPart - t0.QuadPart) * 1000.0 / f.QuadPart;
        CHECK(ms < 100, "10 creates took %.0f ms (paying the 20 ms budget each would be 200 ms)", ms);
        for (auto* p : ps) if (p) { WaitState(p, 1, 5000); p->Release(); }
        Configure(0, 1, 0, 100000);
    }

    printf("[22] queue options: placeholders when full (up to MaxPlaceholders), compile rate limit, efficiency mode\n");
    {
        using PFN_Qm = int(WINAPI*)(int, double, int);
        auto queueMode = (PFN_Qm)(void*)GetProcAddress(GetModuleHandleW(L"d3d12.dll"), "D3D12Async_TestQueueMode");
        CHECK(queueMode != nullptr, "D3D12Async_TestQueueMode exported");
        if (queueMode) {
            Sleep(1500);  // let earlier work drain
            // QueueFullMode=placeholder: past MaxQueuedCompiles (2) a game-task thread still gets placeholders,
            // but only up to MaxPlaceholders outstanding; after that it compiles normally.
            using PFN_Max = void(WINAPI*)(int);
            auto maxPh = (PFN_Max)(void*)GetProcAddress(GetModuleHandleW(L"d3d12.dll"), "D3D12Async_TestMaxPlaceholders");
            CHECK(maxPh != nullptr, "D3D12Async_TestMaxPlaceholders exported");
            auto burst = [&](int n, int* sync, int* async) {
                *sync = *async = 0;
                int ready = 0;
                std::thread t([&] {
                    std::vector<ID3D12PipelineState*> v;
                    for (int i = 0; i < n; ++i) {
                        OwnedGfx o = MakeGfx(0.3f, 0.07f * i, 0.9f);
                        o.d.RTVFormats[0] = DXGI_FORMAT_R16G16B16A16_FLOAT;
                        ID3D12PipelineState* pso = nullptr;
                        g.dev->CreateGraphicsPipelineState(&o.d, IID_PPV_ARGS(&pso));
                        (PlaceholderState(pso) == -1 ? *sync : *async)++;
                        v.push_back(pso);
                    }
                    for (auto* p : v) { if (WaitState(p, 1, 15000) || PlaceholderState(p) == -1) ++ready; p->Release(); }
                });
                t.join();
                return ready;
            };
            Configure(0, 1, 400, 2);
            queueMode(1, 0, 0);
            if (maxPh) maxPh(1000);
            int sync = 0, async = 0;
            int ready = burst(6, &sync, &async);
            CHECK(sync == 0 && async == 6 && ready == 6,
                  "queue limit 2, QueueFullMode=placeholder, MaxPlaceholders=1000: %d placeholders, %d compiled normally, %d finished", async, sync, ready);
            Sleep(500);
            Configure(0, 1, 400, 2);
            queueMode(1, 0, 0);
            if (maxPh) maxPh(4);
            ready = burst(10, &sync, &async);
            CHECK(async == 4 && sync == 6 && ready == 10,
                  "MaxPlaceholders=4: a 10-pipeline flood gets %d placeholders, the other %d compile normally (%d finished)", async, sync, ready);
            if (maxPh) maxPh(512);

            // MaxCompilesPerSecond=5: six queued compiles (no compile delay) take about a second to all start.
            Sleep(500);
            Configure(0, 1, 0, 100000);
            queueMode(0, 5, 0);
            LARGE_INTEGER f, t0, t1;
            QueryPerformanceFrequency(&f);
            std::vector<ID3D12PipelineState*> v;
            QueryPerformanceCounter(&t0);
            for (int i = 0; i < 6; ++i) {
                OwnedGfx o = MakeGfx(0.5f, 0.1f * i, 0.05f);
                o.d.RTVFormats[0] = DXGI_FORMAT_R16G16B16A16_FLOAT;
                ID3D12PipelineState* pso = nullptr;
                g.dev->CreateGraphicsPipelineState(&o.d, IID_PPV_ARGS(&pso));
                v.push_back(pso);
            }
            int done = 0;
            for (auto* p : v) if (PlaceholderState(p) == -1 || WaitState(p, 1, 10000)) ++done;
            QueryPerformanceCounter(&t1);
            double ms = (t1.QuadPart - t0.QuadPart) * 1000.0 / f.QuadPart;
            CHECK(done == 6 && ms >= 800 && ms < 5000, "6 compiles at MaxCompilesPerSecond=5 finished in %.0f ms (no limit: a few ms)", ms);
            for (auto* p : v) p->Release();
            queueMode(0, 0, 0);

            int eff = queueMode(0, 0, 1);
            CHECK(eff == 0 || eff == 1, "WorkerEfficiencyMode applies without error (Windows accepted it: %s)", eff ? "yes" : "no, older Windows/Wine");
        }
        Configure(0, 1, 0, 100000);
    }

    printf("[23] StartupMinSeconds: the phase can't end before it, then normal detection resumes\n");
    {
        typedef void(WINAPI * PFN_Phase)(int, int, int, int, int);
        typedef void(WINAPI * PFN_Min)(int);
        typedef int(WINAPI * PFN_Active)();
        HMODULE m = GetModuleHandleW(L"d3d12.dll");
        auto phase = (PFN_Phase)(void*)GetProcAddress(m, "D3D12Async_TestPhase");
        auto minSecs = (PFN_Min)(void*)GetProcAddress(m, "D3D12Async_TestMinSeconds");
        auto active = (PFN_Active)(void*)GetProcAddress(m, "D3D12Async_StartupActive");
        CHECK(minSecs != nullptr, "D3D12Async_TestMinSeconds exported");
        if (minSecs) {
            auto tick = [&] {
                ID3D12GraphicsCommandList* cl = nullptr;
                rec.alloc->Reset();
                g.dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, rec.alloc, nullptr, IID_PPV_ARGS(&cl));
                cl->Close();
                cl->Release();
            };
            phase(1, 0, 0, 1, 0);  // no PSOs needed, 1 quiet second: would end after ~1 s
            minSecs(4);            // ...but not before 4 s
            for (int i = 0; i < 12; ++i) { Sleep(250); tick(); }  // 3 s, all quiet
            bool held = active() == 1;
            for (int i = 0; i < 16 && active(); ++i) { Sleep(250); tick(); }
            CHECK(held && active() == 0, "quiet for 3 s: still in the startup phase under StartupMinSeconds=4; ended after it");
            phase(0, 0, 0, 1, 0);
            Configure(0, 1, 0, 100000);
        }
    }

    printf("[24] UI pipelines: UiWaitMaxMs caps the wait, then the draw is skipped\n");
    {
        HMODULE m = GetModuleHandleW(L"d3d12.dll");
        using PFN_UiWait = void(WINAPI*)(double);
        using PFN_UiMode = void(WINAPI*)(int);
        auto uiWait = (PFN_UiWait)(void*)GetProcAddress(m, "D3D12Async_TestUiWait");
        auto uiMode = (PFN_UiMode)(void*)GetProcAddress(m, "D3D12Async_TestUiMode");
        CHECK(uiWait && uiMode, "D3D12Async_TestUiWait exported");
        if (uiWait && uiMode) {
            Sleep(500);
            Configure(0, 0, 300, 100000);  // every background compile takes 300 ms
            uiMode(1);                     // UiPipelineMode=wait
            uiWait(10);                    // UiWaitMaxMs=10
            OwnedGfx o = MakeGfx(0.2f, 0.9f, 0.6f);  // 8-bit target, no depth: UI-type
            ID3D12PipelineState* pso = nullptr;
            g.dev->CreateGraphicsPipelineState(&o.d, IID_PPV_ARGS(&pso));
            LARGE_INTEGER f, t0, t1;
            QueryPerformanceFrequency(&f);
            QueryPerformanceCounter(&t0);
            uint32_t px = Render(rec, pso);
            QueryPerformanceCounter(&t1);
            double ms = (t1.QuadPart - t0.QuadPart) * 1000.0 / f.QuadPart;
            CHECK(pso && PlaceholderState(pso) == 0 && px == kBlack && ms < 150,
                  "UiWaitMaxMs=10: the draw waited %.0f ms (not the 300 ms compile), then was skipped", ms);
            WaitState(pso, 1, 5000);
            px = Render(rec, pso);
            CHECK(Near(px, Rgba(0.2f, 0.9f, 0.6f)), "drawn once the compile finished");
            if (pso) pso->Release();

            uiWait(0);
        }
        Configure(0, 1, 0, 100000);
    }

    printf("[26] loading screens: while no frame is presented, pipelines compile normally\n");
    {
        HMODULE m = GetModuleHandleW(L"d3d12.dll");
        typedef void(WINAPI * PFN_Phase)(int, int, int, int, int);
        using PFN_Loading = int(WINAPI*)(int, int, int);
        auto phase = (PFN_Phase)(void*)GetProcAddress(m, "D3D12Async_TestPhase");
        auto loading = (PFN_Loading)(void*)GetProcAddress(m, "D3D12Async_TestLoading");
        CHECK(loading != nullptr, "D3D12Async_TestLoading exported");
        WNDCLASSW wc{};
        wc.lpfnWndProc = DefWindowProcW;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"pso_test_wnd_loading";
        RegisterClassW(&wc);
        HWND wnd = CreateWindowExW(0, wc.lpszClassName, L"t", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 0, 0, 300, 200, nullptr, nullptr, wc.hInstance, nullptr);
        IDXGIFactory2* fac = nullptr;
        IDXGISwapChain1* sc = nullptr;
        CreateDXGIFactory2(0, IID_PPV_ARGS(&fac));
        DXGI_SWAP_CHAIN_DESC1 d{};
        d.Width = 64; d.Height = 64; d.Format = DXGI_FORMAT_R8G8B8A8_UNORM; d.SampleDesc.Count = 1;
        d.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; d.BufferCount = 2; d.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        if (fac) fac->CreateSwapChainForHwnd(g.queue, wnd, &d, nullptr, nullptr, &sc);
        if (loading && sc) {
            Configure(0, 1, 300, 100000);   // every background compile takes 300 ms
            phase(0, 0, 0, 1, 1000000);     // startup detection ends after 1 quiet second of frames
            for (int i = 0; i < 20; ++i) { sc->Present(0, 0); Sleep(100); }
            loading(1, 200, 1000);          // LoadingGapMs=200, LoadingHoldMs=1000
            float col = 0.11f;
            auto create = [&] {
                OwnedGfx o = MakeGfx(col, 0.2f, 0.7f);
                col += 0.03f;
                o.d.RTVFormats[0] = DXGI_FORMAT_R16G16B16A16_FLOAT;
                ID3D12PipelineState* p = nullptr;
                g.dev->CreateGraphicsPipelineState(&o.d, IID_PPV_ARGS(&p));
                int st = p ? PlaceholderState(p) : -2;
                if (p) { WaitState(p, 1, 5000); p->Release(); }
                return st;  // 0 = placeholder (background), -1 = compiled normally
            };
            sc->Present(0, 0);
            int gameplay = create();
            sc->Present(0, 0);
            Sleep(300);  // 300 ms without a frame (>= 200 ms gap, < 400 ms: a hitch, no hold)
            int gapCreate = create();
            sc->Present(0, 0);
            int afterShortGap = create();
            sc->Present(0, 0);
            int periods0 = loading(1, 200, 1000);
            Sleep(600);  // a real loading screen: >= 2x the gap
            int loadCreate = create();
            sc->Present(0, 0);
            int inHold = create();
            Sleep(1100);
            sc->Present(0, 0);
            int afterHold = create();
            int periods1 = loading(1, 200, 1000);
            CHECK(gameplay == 0, "frames flowing: background compile with a placeholder (%d)", gameplay);
            CHECK(gapCreate == -1 && afterShortGap == 0,
                  "no frame for 300 ms: compiled normally (%d); right after a short gap, background again (%d)", gapCreate, afterShortGap);
            CHECK(loadCreate == -1 && inHold == -1 && afterHold == 0,
                  "loading screen: compiled normally (%d), still for LoadingHoldMs after frames resume (%d), then background (%d)",
                  loadCreate, inHold, afterHold);
            CHECK(periods1 > periods0, "the long frame was counted as a loading screen, not a gameplay hitch (%d -> %d)", periods0, periods1);
            loading(0, 500, 1500);
            Configure(0, 1, 0, 100000);
        }
        if (sc) sc->Release();
        if (fac) fac->Release();
        if (wnd) DestroyWindow(wnd);
    }

    printf("[27] settings panel: Ctrl+Shift+P toggles queue-full placeholders live while the game has focus\n");
    {
        using PFN_Get = int(WINAPI*)();
        auto get = (PFN_Get)(void*)GetProcAddress(GetModuleHandleW(L"d3d12.dll"), "D3D12Async_QueueFullPlaceholder");
        CHECK(get != nullptr, "D3D12Async_QueueFullPlaceholder exported");
        WNDCLASSW wc{};
        wc.lpfnWndProc = DefWindowProcW;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"pso_test_wnd_panel";
        RegisterClassW(&wc);
        HWND wnd = CreateWindowExW(0, wc.lpszClassName, L"game", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 40, 40, 1000, 640, nullptr, nullptr,
                                   wc.hInstance, nullptr);
        SetForegroundWindow(wnd);
        auto pump = [&](int ms) {
            for (int t = 0; t < ms; t += 20) {
                MSG m;
                while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&m);
                Sleep(20);
            }
        };
        auto chord = [&] {
            keybd_event(VK_CONTROL, 0, 0, 0); keybd_event(VK_SHIFT, 0, 0, 0); keybd_event('P', 0, 0, 0);
            pump(200);
            keybd_event('P', 0, KEYEVENTF_KEYUP, 0); keybd_event(VK_SHIFT, 0, KEYEVENTF_KEYUP, 0); keybd_event(VK_CONTROL, 0, KEYEVENTF_KEYUP, 0);
            pump(300);
        };
        pump(1200);  // the overlay finds the game window and shows the panel
        int before = get ? get() : -1;
        chord();
        int after = get ? get() : -1;
        printf("    (panel visible for a screenshot)\n");
        fflush(stdout);
        pump(4000);
        chord();
        int back = get ? get() : -1;
        CHECK(before == 0 && after == 1 && back == 0, "hotkey toggled it on (%d -> %d) and off again (%d)", before, after, back);
        DestroyWindow(wnd);
    }

    uint64_t s[8];
    GetStats(s);
    printf("stats: requests=%llu sync=%llu in-budget=%llu placeholders=%llu failed=%llu skipped=%llu forced-waits=%llu\n",
           (unsigned long long)s[0], (unsigned long long)s[1], (unsigned long long)s[2], (unsigned long long)s[3],
           (unsigned long long)s[4], (unsigned long long)s[5], (unsigned long long)s[6]);
    printf("%d passed, %d failed\n", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
