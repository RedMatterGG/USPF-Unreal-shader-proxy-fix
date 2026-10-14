#include "desc_copy.h"

#include <cstring>
#include <new>

namespace d3a {

OwnedStorage::~OwnedStorage() {
    for (IUnknown* u : refs_) u->Release();
}

const void* OwnedStorage::CopyBytes(const void* src, size_t n) {
    if (!src || !n) return nullptr;
    if (n > (256u << 20)) throw std::bad_alloc();  // a corrupt length: let the runtime reject it synchronously
    auto p = std::make_unique<uint8_t[]>(n);
    std::memcpy(p.get(), src, n);
    blobs_.push_back(std::move(p));
    return blobs_.back().get();
}

const char* OwnedStorage::CopyString(const char* s) {
    if (!s) return nullptr;
    strings_.emplace_back(s);
    return strings_.back().c_str();
}

void OwnedStorage::HoldRef(IUnknown* u) {
    if (!u) return;
    u->AddRef();
    refs_.push_back(u);
}

namespace {

void CopyShader(OwnedStorage& s, D3D12_SHADER_BYTECODE& bc) {
    bc.pShaderBytecode = s.CopyBytes(bc.pShaderBytecode, bc.BytecodeLength);
    if (!bc.pShaderBytecode) bc.BytecodeLength = 0;
}

void CopyInputLayout(OwnedStorage& s, D3D12_INPUT_LAYOUT_DESC& il) {
    if (!il.pInputElementDescs || !il.NumElements) {
        il.pInputElementDescs = nullptr;
        il.NumElements = 0;
        return;
    }
    auto* e = (D3D12_INPUT_ELEMENT_DESC*)s.CopyBytes(il.pInputElementDescs, sizeof(D3D12_INPUT_ELEMENT_DESC) * il.NumElements);
    for (UINT i = 0; i < il.NumElements; ++i) e[i].SemanticName = s.CopyString(e[i].SemanticName);
    il.pInputElementDescs = e;
}

void CopyStreamOutput(OwnedStorage& s, D3D12_STREAM_OUTPUT_DESC& so) {
    if (so.pSODeclaration && so.NumEntries) {
        auto* e = (D3D12_SO_DECLARATION_ENTRY*)s.CopyBytes(so.pSODeclaration, sizeof(D3D12_SO_DECLARATION_ENTRY) * so.NumEntries);
        for (UINT i = 0; i < so.NumEntries; ++i) e[i].SemanticName = s.CopyString(e[i].SemanticName);
        so.pSODeclaration = e;
    } else {
        so.pSODeclaration = nullptr;
        so.NumEntries = 0;
    }
    so.pBufferStrides = (const UINT*)s.CopyBytes(so.pBufferStrides, sizeof(UINT) * so.NumStrides);
    if (!so.pBufferStrides) so.NumStrides = 0;
}

void CopyCached(OwnedStorage& s, D3D12_CACHED_PIPELINE_STATE& c) {
    c.pCachedBlob = s.CopyBytes(c.pCachedBlob, c.CachedBlobSizeInBytes);
    if (!c.pCachedBlob) c.CachedBlobSizeInBytes = 0;
}

// Layout of one stream subobject, exactly as the runtime (and d3dx12.h) lays it out:
// a pointer-aligned struct holding the type enum followed by the inner struct.
template <typename T>
struct alignas(void*) Sub {
    D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type;
    T inner;
};

struct SubLayout {
    size_t innerOffset = 0, size = 0;
    bool known = false;
};

template <typename T>
constexpr SubLayout L() {
    return {offsetof(Sub<T>, inner), sizeof(Sub<T>), true};
}

SubLayout LayoutOf(uint32_t t) {
    switch (t) {
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE: return L<ID3D12RootSignature*>();
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS:
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS:
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DS:
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_HS:
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_GS:
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CS:
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_AS:
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS: return L<D3D12_SHADER_BYTECODE>();
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_STREAM_OUTPUT: return L<D3D12_STREAM_OUTPUT_DESC>();
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_BLEND: return L<D3D12_BLEND_DESC>();
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_MASK: return L<UINT>();
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER: return L<D3D12_RASTERIZER_DESC>();
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL: return L<D3D12_DEPTH_STENCIL_DESC>();
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_INPUT_LAYOUT: return L<D3D12_INPUT_LAYOUT_DESC>();
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_IB_STRIP_CUT_VALUE: return L<D3D12_INDEX_BUFFER_STRIP_CUT_VALUE>();
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PRIMITIVE_TOPOLOGY: return L<D3D12_PRIMITIVE_TOPOLOGY_TYPE>();
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS: return L<D3D12_RT_FORMAT_ARRAY>();
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL_FORMAT: return L<DXGI_FORMAT>();
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_DESC: return L<DXGI_SAMPLE_DESC>();
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_NODE_MASK: return L<UINT>();
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CACHED_PSO: return L<D3D12_CACHED_PIPELINE_STATE>();
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_FLAGS: return L<D3D12_PIPELINE_STATE_FLAGS>();
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL1: return L<D3D12_DEPTH_STENCIL_DESC1>();
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VIEW_INSTANCING: return L<D3D12_VIEW_INSTANCING_DESC>();
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL2: return L<D3D12_DEPTH_STENCIL_DESC2>();
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER1: return L<D3D12_RASTERIZER_DESC1>();
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER2: return L<D3D12_RASTERIZER_DESC2>();
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SERIALIZED_ROOT_SIGNATURE: return L<D3D12_SERIALIZED_ROOT_SIGNATURE_DESC>();
        default: return {};  // a subobject type this build doesn't know: never guess its size
    }
}

}  // namespace

std::unique_ptr<OwnedComputeDesc> CopyComputeDesc(const D3D12_COMPUTE_PIPELINE_STATE_DESC* d) {
    if (!d) return nullptr;
    auto o = std::make_unique<OwnedComputeDesc>();
    o->desc = *d;
    o->store.HoldRef(d->pRootSignature);
    CopyShader(o->store, o->desc.CS);
    CopyCached(o->store, o->desc.CachedPSO);
    return o;
}

std::unique_ptr<OwnedGraphicsDesc> CopyGraphicsDesc(const D3D12_GRAPHICS_PIPELINE_STATE_DESC* d) {
    if (!d) return nullptr;
    auto o = std::make_unique<OwnedGraphicsDesc>();
    o->desc = *d;
    auto& s = o->store;
    s.HoldRef(d->pRootSignature);
    CopyShader(s, o->desc.VS);
    CopyShader(s, o->desc.PS);
    CopyShader(s, o->desc.DS);
    CopyShader(s, o->desc.HS);
    CopyShader(s, o->desc.GS);
    CopyStreamOutput(s, o->desc.StreamOutput);
    CopyInputLayout(s, o->desc.InputLayout);
    CopyCached(s, o->desc.CachedPSO);
    return o;
}

std::unique_ptr<OwnedStream> CopyStream(const D3D12_PIPELINE_STATE_STREAM_DESC* d, const char** whyNot) {
    auto fail = [&](const char* w) { if (whyNot) *whyNot = w; return nullptr; };
    if (!d || !d->pPipelineStateSubobjectStream || !d->SizeInBytes) return fail("empty stream");
    if (d->SizeInBytes % sizeof(void*)) return fail("stream size not pointer aligned");
    if (d->SizeInBytes > (64u << 10)) return fail("stream larger than 64 KiB");

    auto o = std::make_unique<OwnedStream>();
    o->buffer.resize((d->SizeInBytes + 7) / 8);
    auto* dst = reinterpret_cast<uint8_t*>(o->buffer.data());
    std::memcpy(dst, d->pPipelineStateSubobjectStream, d->SizeInBytes);
    auto& s = o->store;

    size_t off = 0;
    while (off < d->SizeInBytes) {
        if (d->SizeInBytes - off < sizeof(uint32_t)) return fail("truncated subobject");
        uint32_t type;
        std::memcpy(&type, dst + off, sizeof type);
        SubLayout lay = LayoutOf(type);
        if (!lay.known) return fail("unknown subobject type");
        if (off + lay.size > d->SizeInBytes) return fail("subobject overruns stream");
        void* inner = dst + off + lay.innerOffset;
        switch (type) {
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE:
                s.HoldRef(*reinterpret_cast<ID3D12RootSignature**>(inner));
                break;
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CS:
                o->isCompute = true;
                [[fallthrough]];
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS:
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS:
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DS:
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_HS:
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_GS:
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_AS:
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS:
                CopyShader(s, *reinterpret_cast<D3D12_SHADER_BYTECODE*>(inner));
                break;
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_STREAM_OUTPUT:
                CopyStreamOutput(s, *reinterpret_cast<D3D12_STREAM_OUTPUT_DESC*>(inner));
                break;
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_INPUT_LAYOUT:
                CopyInputLayout(s, *reinterpret_cast<D3D12_INPUT_LAYOUT_DESC*>(inner));
                break;
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CACHED_PSO:
                CopyCached(s, *reinterpret_cast<D3D12_CACHED_PIPELINE_STATE*>(inner));
                break;
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VIEW_INSTANCING: {
                auto& vi = *reinterpret_cast<D3D12_VIEW_INSTANCING_DESC*>(inner);
                vi.pViewInstanceLocations = (const D3D12_VIEW_INSTANCE_LOCATION*)s.CopyBytes(
                    vi.pViewInstanceLocations, sizeof(D3D12_VIEW_INSTANCE_LOCATION) * vi.ViewInstanceCount);
                break;
            }
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SERIALIZED_ROOT_SIGNATURE: {
                auto& rs = *reinterpret_cast<D3D12_SERIALIZED_ROOT_SIGNATURE_DESC*>(inner);
                rs.pSerializedBlob = s.CopyBytes(rs.pSerializedBlob, rs.SerializedBlobSizeInBytes);
                break;
            }
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS: {
                auto& f = *reinterpret_cast<D3D12_RT_FORMAT_ARRAY*>(inner);
                o->numRt = f.NumRenderTargets > 8 ? 8 : f.NumRenderTargets;
                for (UINT k = 0; k < o->numRt; ++k) o->rt[k] = f.RTFormats[k];
                break;
            }
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL_FORMAT:
                o->ds = *reinterpret_cast<DXGI_FORMAT*>(inner);
                break;
            default: break;  // plain values, already copied
        }
        off += lay.size;
    }
    o->desc.SizeInBytes = d->SizeInBytes;
    o->desc.pPipelineStateSubobjectStream = dst;
    return o;
}

}  // namespace d3a
