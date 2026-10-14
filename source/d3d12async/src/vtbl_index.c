/* Vtable slot indices taken from the official D3D12 C interface definitions,
   so no slot number is ever hand-counted. Compiled as C with CINTERFACE. */
#define CINTERFACE
#define COBJMACROS
#include <windows.h>
#include <directx/d3d12.h>
#include <stddef.h>

#define IDX(T, m) (unsigned)(offsetof(T##Vtbl, m) / sizeof(void *))

/* ID3D12Device family (one object, one vtable that grows with each version) */
const unsigned kDev_CreateGraphicsPipelineState = IDX(ID3D12Device, CreateGraphicsPipelineState);
const unsigned kDev_CreateCommandList           = IDX(ID3D12Device, CreateCommandList);
const unsigned kDev_MakeResident                = IDX(ID3D12Device, MakeResident);
const unsigned kDev_Evict                       = IDX(ID3D12Device, Evict);
const unsigned kDev1_CreatePipelineLibrary      = IDX(ID3D12Device1, CreatePipelineLibrary);
const unsigned kDev1_SetResidencyPriority       = IDX(ID3D12Device1, SetResidencyPriority);
const unsigned kDev2_CreatePipelineState        = IDX(ID3D12Device2, CreatePipelineState);
const unsigned kDev3_EnqueueMakeResident        = IDX(ID3D12Device3, EnqueueMakeResident);
const unsigned kDev4_CreateCommandList1         = IDX(ID3D12Device4, CreateCommandList1);

/* ID3D12GraphicsCommandList family */
const unsigned kCl_Close                = IDX(ID3D12GraphicsCommandList, Close);
const unsigned kCl_Reset                = IDX(ID3D12GraphicsCommandList, Reset);
const unsigned kCl_ClearState           = IDX(ID3D12GraphicsCommandList, ClearState);
const unsigned kCl_DrawInstanced        = IDX(ID3D12GraphicsCommandList, DrawInstanced);
const unsigned kCl_DrawIndexedInstanced = IDX(ID3D12GraphicsCommandList, DrawIndexedInstanced);
const unsigned kCl_SetPipelineState     = IDX(ID3D12GraphicsCommandList, SetPipelineState);
const unsigned kCl_ExecuteIndirect      = IDX(ID3D12GraphicsCommandList, ExecuteIndirect);
const unsigned kCl_ExecuteBundle        = IDX(ID3D12GraphicsCommandList, ExecuteBundle);
const unsigned kCl_Dispatch             = IDX(ID3D12GraphicsCommandList, Dispatch);
const unsigned kCl4_SetPipelineState1   = IDX(ID3D12GraphicsCommandList4, SetPipelineState1);
const unsigned kCl6_DispatchMesh        = IDX(ID3D12GraphicsCommandList6, DispatchMesh);

/* Others */
const unsigned kLib_StorePipeline              = IDX(ID3D12PipelineLibrary, StorePipeline);
const unsigned kLib_LoadGraphicsPipeline       = IDX(ID3D12PipelineLibrary, LoadGraphicsPipeline);
const unsigned kLib_LoadComputePipeline        = IDX(ID3D12PipelineLibrary, LoadComputePipeline);
const unsigned kLib1_LoadPipeline              = IDX(ID3D12PipelineLibrary1, LoadPipeline);
const unsigned kDev_CreateComputePipelineState = IDX(ID3D12Device, CreateComputePipelineState);
const unsigned kFactory_CreateDevice           = IDX(ID3D12DeviceFactory, CreateDevice);
const unsigned kSdkCfg1_CreateDeviceFactory    = IDX(ID3D12SDKConfiguration1, CreateDeviceFactory);
