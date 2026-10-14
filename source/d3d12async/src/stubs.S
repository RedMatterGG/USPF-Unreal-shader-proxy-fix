# Pass-through export stubs (x64, Windows ABI). Each jumps to the real d3d12.dll export.
# If the real DLL isn't resolved yet, the stub resolves it first, preserving the
# integer and float argument registers; stack arguments are never touched.
    .intel_syntax noprefix
    .text

.macro STUB name
    .globl stub_\name
    .def stub_\name; .scl 2; .type 32; .endef
stub_\name:
    mov rax, qword ptr [rip + real_\name]
    test rax, rax
    jz 1f
    jmp rax
1:
    push rcx
    push rdx
    push r8
    push r9
    sub rsp, 104
    movdqu xmmword ptr [rsp + 32], xmm0
    movdqu xmmword ptr [rsp + 48], xmm1
    movdqu xmmword ptr [rsp + 64], xmm2
    movdqu xmmword ptr [rsp + 80], xmm3
    call d3a_resolve_exports
    movdqu xmm0, xmmword ptr [rsp + 32]
    movdqu xmm1, xmmword ptr [rsp + 48]
    movdqu xmm2, xmmword ptr [rsp + 64]
    movdqu xmm3, xmmword ptr [rsp + 80]
    add rsp, 104
    pop r9
    pop r8
    pop rdx
    pop rcx
    mov rax, qword ptr [rip + real_\name]
    test rax, rax
    jz 2f
    jmp rax
2:
    mov eax, 0x80004001     # E_NOTIMPL: the system d3d12.dll lacks this export
    ret
.endm

    STUB Ordinal99
    STUB SetAppCompatStringPointer
    STUB D3D12GetDebugInterface
    STUB D3D12CoreCreateLayeredDevice
    STUB D3D12CoreGetLayeredDeviceSize
    STUB D3D12CoreRegisterLayers
    STUB D3D12CreateRootSignatureDeserializer
    STUB D3D12CreateVersionedRootSignatureDeserializer
    STUB D3D12EnableExperimentalFeatures
    STUB D3D12PIXEventsReplaceBlock
    STUB D3D12PIXGetThreadInfo
    STUB D3D12PIXNotifyWakeFromFenceSignal
    STUB D3D12PIXReportCounter
    STUB D3D12SerializeRootSignature
    STUB D3D12SerializeVersionedRootSignature
    STUB GetBehaviorValue
