// SPDX-License-Identifier: MIT
#include "native_hud_interpolation.hpp"
#include "native_hud_interpolation_vs.h" // fxc /Fh output: g_native_hud_interpolation_vs
#include "native_hud_interpolation_ps.h" // fxc /Fh output: g_native_hud_interpolation_ps
#include "native_hud_layer_cs.h"         // fxc /Fh output: g_native_hud_layer_cs
#include "native_hud_motion_cs.h"        // fxc /Fh output: g_native_hud_motion_cs
#include <algorithm>
#include <cstring>
#include <iterator>
#include <utility>

using Microsoft::WRL::ComPtr;
namespace mfgunlock::xemfg::native {
namespace {
void Transition(ID3D12GraphicsCommandList* list,ID3D12Resource* resource,D3D12_RESOURCE_STATES from,D3D12_RESOURCE_STATES to) {
  if(from==to) return;
  D3D12_RESOURCE_BARRIER barrier{};
  barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  barrier.Transition={resource,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,from,to};
  list->ResourceBarrier(1,&barrier);
}
constexpr auto kReadable=D3D12_RESOURCE_STATES(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE|D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
constexpr DXGI_FORMAT kLayerFormat=DXGI_FORMAT_R16G16B16A16_FLOAT;
constexpr DXGI_FORMAT kMotionFormat=DXGI_FORMAT_R16G16_SINT;
bool Wait(ID3D12Fence* fence,uint64_t value,HANDLE event,DWORD ms) {
  if(fence->GetCompletedValue()>=value) return true;
  return SUCCEEDED(fence->SetEventOnCompletion(value,event)) && WaitForSingleObject(event,ms)==WAIT_OBJECT_0;
}
HRESULT RootSignature(ID3D12Device* device,const D3D12_ROOT_SIGNATURE_DESC& desc,ComPtr<ID3D12RootSignature>& out) {
  ComPtr<ID3DBlob> blob,errors;
  HRESULT hr=D3D12SerializeRootSignature(&desc,D3D_ROOT_SIGNATURE_VERSION_1,&blob,&errors);
  if(SUCCEEDED(hr)) hr=device->CreateRootSignature(0,blob->GetBufferPointer(),blob->GetBufferSize(),IID_PPV_ARGS(&out));
  return hr;
}
}
bool HudInterpolator::SupportedFormat(DXGI_FORMAT value) {
  return value==DXGI_FORMAT_R8G8B8A8_UNORM || value==DXGI_FORMAT_B8G8R8A8_UNORM
      || value==DXGI_FORMAT_R10G10B10A2_UNORM;
}
HudInterpolator::~HudInterpolator() { Drain(1000); if(event) CloseHandle(event); }
bool HudInterpolator::Initialize(ID3D12Device* target,DXGI_FORMAT output) {
  device=target; format=output;
  if(!SupportedFormat(format)) return Fail(DXGI_ERROR_UNSUPPORTED);
  HRESULT hr;
  {
    D3D12_DESCRIPTOR_RANGE range{D3D12_DESCRIPTOR_RANGE_TYPE_SRV,3,0,0,0};
    D3D12_ROOT_PARAMETER parameters[2]{};
    parameters[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameters[0].Constants={0,0,6}; parameters[0].ShaderVisibility=D3D12_SHADER_VISIBILITY_PIXEL;
    parameters[1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[1].DescriptorTable={1,&range}; parameters[1].ShaderVisibility=D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_STATIC_SAMPLER_DESC sampler{};
    sampler.Filter=D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU=sampler.AddressV=sampler.AddressW=D3D12_TEXTURE_ADDRESS_MODE_BORDER;
    sampler.BorderColor=D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK;
    sampler.MaxLOD=D3D12_FLOAT32_MAX; sampler.ShaderVisibility=D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_ROOT_SIGNATURE_DESC desc{2,parameters,1,&sampler,D3D12_ROOT_SIGNATURE_FLAG_NONE};
    if(FAILED(hr=RootSignature(device.Get(),desc,root))) return Fail(hr);
  }
  {
    D3D12_DESCRIPTOR_RANGE ranges[2]{};
    ranges[0]={D3D12_DESCRIPTOR_RANGE_TYPE_SRV,2,0,0,0};
    ranges[1]={D3D12_DESCRIPTOR_RANGE_TYPE_UAV,2,0,0,2};
    D3D12_ROOT_PARAMETER parameters[2]{};
    parameters[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameters[0].Constants={0,0,5};
    parameters[1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[1].DescriptorTable={2,ranges};
    D3D12_ROOT_SIGNATURE_DESC desc{2,parameters,0,nullptr,D3D12_ROOT_SIGNATURE_FLAG_NONE};
    if(FAILED(hr=RootSignature(device.Get(),desc,computeRoot))) return Fail(hr);
  }
  D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};
  pso.pRootSignature=root.Get();
  pso.VS={g_native_hud_interpolation_vs,sizeof(g_native_hud_interpolation_vs)};
  pso.PS={g_native_hud_interpolation_ps,sizeof(g_native_hud_interpolation_ps)};
  // Premultiplied: the HUD replaces the interpolated scene by its coverage.
  auto& blend=pso.BlendState.RenderTarget[0];
  blend.BlendEnable=TRUE;
  blend.SrcBlend=D3D12_BLEND_ONE; blend.DestBlend=D3D12_BLEND_INV_SRC_ALPHA; blend.BlendOp=D3D12_BLEND_OP_ADD;
  blend.SrcBlendAlpha=D3D12_BLEND_ZERO; blend.DestBlendAlpha=D3D12_BLEND_ONE; blend.BlendOpAlpha=D3D12_BLEND_OP_ADD;
  blend.RenderTargetWriteMask=D3D12_COLOR_WRITE_ENABLE_RED|D3D12_COLOR_WRITE_ENABLE_GREEN|D3D12_COLOR_WRITE_ENABLE_BLUE;
  pso.SampleMask=UINT_MAX;
  pso.RasterizerState.FillMode=D3D12_FILL_MODE_SOLID; pso.RasterizerState.CullMode=D3D12_CULL_MODE_NONE;
  pso.RasterizerState.DepthClipEnable=TRUE;
  pso.PrimitiveTopologyType=D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  pso.NumRenderTargets=1; pso.RTVFormats[0]=format; pso.SampleDesc.Count=1;
  if(FAILED(hr=device->CreateGraphicsPipelineState(&pso,IID_PPV_ARGS(&pipeline)))) return Fail(hr);
  D3D12_COMPUTE_PIPELINE_STATE_DESC compute{};
  compute.pRootSignature=computeRoot.Get();
  compute.CS={g_native_hud_layer_cs,sizeof(g_native_hud_layer_cs)};
  if(FAILED(hr=device->CreateComputePipelineState(&compute,IID_PPV_ARGS(&layerPipeline)))) return Fail(hr);
  compute.CS={g_native_hud_motion_cs,sizeof(g_native_hud_motion_cs)};
  if(FAILED(hr=device->CreateComputePipelineState(&compute,IID_PPV_ARGS(&motionPipeline)))) return Fail(hr);
  D3D12_DESCRIPTOR_HEAP_DESC srv{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,kComposeSlots*3,D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE,0};
  D3D12_DESCRIPTOR_HEAP_DESC computeDesc{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,kSlots*8,D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE,0};
  D3D12_DESCRIPTOR_HEAP_DESC rtv{D3D12_DESCRIPTOR_HEAP_TYPE_RTV,kComposeSlots+1,D3D12_DESCRIPTOR_HEAP_FLAG_NONE,0};
  if(FAILED(hr=device->CreateDescriptorHeap(&srv,IID_PPV_ARGS(&srvHeap)))
      || FAILED(hr=device->CreateDescriptorHeap(&computeDesc,IID_PPV_ARGS(&computeHeap)))
      || FAILED(hr=device->CreateDescriptorHeap(&rtv,IID_PPV_ARGS(&rtvHeap)))) return Fail(hr);
  srvSize=device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  rtvSize=device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
  for(auto& allocator:captureAllocators)
    if(FAILED(hr=device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocator)))) return Fail(hr);
  for(auto& allocator:composeAllocators)
    if(FAILED(hr=device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocator)))) return Fail(hr);
  if(FAILED(hr=device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,captureAllocators[0].Get(),nullptr,IID_PPV_ARGS(&captureList)))
      || FAILED(hr=device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,composeAllocators[0].Get(),nullptr,IID_PPV_ARGS(&composeList))))
    return Fail(hr);
  captureList->Close(); composeList->Close();
  if(FAILED(hr=device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&captureFence)))
      || FAILED(hr=device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&composeFence)))) return Fail(hr);
  event=CreateEventW(nullptr,FALSE,FALSE,nullptr);
  return event?true:Fail(HRESULT_FROM_WIN32(GetLastError()));
}
bool HudInterpolator::Ensure(ComPtr<ID3D12Resource>& texture,DXGI_FORMAT textureFormat,uint32_t width,uint32_t height,
    D3D12_RESOURCE_FLAGS flags,D3D12_RESOURCE_STATES initial) {
  if(texture) {
    const auto d=texture->GetDesc();
    if(d.Width==width && d.Height==height) return true;
    texture.Reset();
  }
  D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
  D3D12_RESOURCE_DESC d{};
  d.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D; d.Width=width; d.Height=height; d.DepthOrArraySize=1; d.MipLevels=1;
  d.Format=textureFormat; d.SampleDesc.Count=1; d.Flags=flags;
  const HRESULT hr=device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&d,initial,nullptr,IID_PPV_ARGS(&texture));
  return SUCCEEDED(hr)?true:Fail(hr);
}
ID3D12GraphicsCommandList* HudInterpolator::Capture(uint64_t id,ID3D12Resource* backbuffer,D3D12_RESOURCE_STATES backState,
    ID3D12Resource* clean,D3D12_RESOURCE_STATES cleanState,const DejitterRegion& area,
    ID3D12Resource** hudless,ID3D12Resource** ui) {
  std::lock_guard guard(lock);
  const auto bd=backbuffer?backbuffer->GetDesc():D3D12_RESOURCE_DESC{};
  if(captureOpen || !backbuffer || !clean || bd.Format!=format || area.width!=bd.Width || area.height!=bd.Height) {
    Fail(E_INVALIDARG); return nullptr;
  }
  Slot* previous=&slots[captureSlot];
  captureSlot=(captureSlot+1)%kSlots;
  auto& slot=slots[captureSlot];
  // The allocator was last used by this slot's previous capture.
  if(slot.captured && !Wait(captureFence.Get(),slot.captured,event,250)) { Fail(HRESULT_FROM_WIN32(WAIT_TIMEOUT)); return nullptr; }
  slot.id=UINT64_MAX; // not composable until this capture is submitted
  const uint32_t width=uint32_t(bd.Width), height=bd.Height;
  const uint32_t blocksX=(width+7)/8, blocksY=(height+7)/8;
  const auto uav=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
  if(!Ensure(slot.back,format,width,height,D3D12_RESOURCE_FLAG_NONE,kReadable)
      || !Ensure(slot.clean,format,width,height,D3D12_RESOURCE_FLAG_NONE,kReadable)
      || !Ensure(slot.layer,kLayerFormat,width,height,uav,kReadable)
      || !Ensure(slot.motion,kMotionFormat,blocksX,blocksY,uav,kReadable)
      || !Ensure(emptyUi,format,width,height,D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,kReadable)) return nullptr;
  if(emptyWidth!=width || emptyHeight!=height) { emptyCleared=false; emptyWidth=width; emptyHeight=height; }
  // The HUD's motion needs the previous real frame, captured at this size.
  if(previous==&slot || previous->id!=id-1 || previous->width!=width || previous->height!=height) previous=nullptr;
  HRESULT hr=captureAllocators[captureSlot]->Reset();
  if(SUCCEEDED(hr)) hr=captureList->Reset(captureAllocators[captureSlot].Get(),nullptr);
  if(FAILED(hr)) { Fail(hr); return nullptr; }
  captureOpen=true;
  if(!emptyCleared) {
    auto rtv=rtvHeap->GetCPUDescriptorHandleForHeapStart(); rtv.ptr+=SIZE_T(kComposeSlots)*rtvSize;
    device->CreateRenderTargetView(emptyUi.Get(),nullptr,rtv);
    const float zero[4]{};
    Transition(captureList.Get(),emptyUi.Get(),kReadable,D3D12_RESOURCE_STATE_RENDER_TARGET);
    captureList->ClearRenderTargetView(rtv,zero,0,nullptr);
    Transition(captureList.Get(),emptyUi.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET,kReadable);
    emptyCleared=true;
  }
  Transition(captureList.Get(),backbuffer,backState,D3D12_RESOURCE_STATE_COPY_SOURCE);
  Transition(captureList.Get(),slot.back.Get(),kReadable,D3D12_RESOURCE_STATE_COPY_DEST);
  captureList->CopyResource(slot.back.Get(),backbuffer);
  Transition(captureList.Get(),slot.back.Get(),D3D12_RESOURCE_STATE_COPY_DEST,kReadable);
  Transition(captureList.Get(),backbuffer,D3D12_RESOURCE_STATE_COPY_SOURCE,backState);
  Transition(captureList.Get(),clean,cleanState,D3D12_RESOURCE_STATE_COPY_SOURCE);
  Transition(captureList.Get(),slot.clean.Get(),kReadable,D3D12_RESOURCE_STATE_COPY_DEST);
  D3D12_TEXTURE_COPY_LOCATION dst{slot.clean.Get(),D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX,{}};
  D3D12_TEXTURE_COPY_LOCATION src{clean,D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX,{}};
  const D3D12_BOX box{area.x,area.y,0,area.x+area.width,area.y+area.height,1};
  captureList->CopyTextureRegion(&dst,0,0,0,&src,&box);
  Transition(captureList.Get(),slot.clean.Get(),D3D12_RESOURCE_STATE_COPY_DEST,kReadable);
  Transition(captureList.Get(),clean,D3D12_RESOURCE_STATE_COPY_SOURCE,cleanState);

  // Two descriptor tables per slot: the layer pass and the motion pass. The
  // unused UAV of each pass is a null descriptor.
  const auto cpuBase=[&](uint32_t table) {
    auto cpu=computeHeap->GetCPUDescriptorHandleForHeapStart(); cpu.ptr+=SIZE_T(captureSlot*8+table*4)*srvSize; return cpu;
  };
  const auto gpuBase=[&](uint32_t table) {
    auto gpu=computeHeap->GetGPUDescriptorHandleForHeapStart(); gpu.ptr+=UINT64(captureSlot*8+table*4)*srvSize; return gpu;
  };
  const auto srvAt=[&](D3D12_CPU_DESCRIPTOR_HANDLE cpu,ID3D12Resource* resource,DXGI_FORMAT viewFormat) {
    D3D12_SHADER_RESOURCE_VIEW_DESC view{}; view.Format=viewFormat; view.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;
    view.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; view.Texture2D.MipLevels=1;
    device->CreateShaderResourceView(resource,&view,cpu);
  };
  const auto uavAt=[&](D3D12_CPU_DESCRIPTOR_HANDLE cpu,ID3D12Resource* resource,DXGI_FORMAT viewFormat) {
    D3D12_UNORDERED_ACCESS_VIEW_DESC view{}; view.Format=viewFormat; view.ViewDimension=D3D12_UAV_DIMENSION_TEXTURE2D;
    device->CreateUnorderedAccessView(resource,nullptr,&view,cpu);
  };
  const auto step=[&](D3D12_CPU_DESCRIPTOR_HANDLE cpu,uint32_t n) { cpu.ptr+=SIZE_T(n)*srvSize; return cpu; };
  {
    const auto cpu=cpuBase(0);
    srvAt(cpu,slot.back.Get(),format);
    srvAt(step(cpu,1),slot.clean.Get(),format);
    uavAt(step(cpu,2),slot.layer.Get(),kLayerFormat);
    uavAt(step(cpu,3),nullptr,kMotionFormat);
  }
  if(previous) {
    const auto cpu=cpuBase(1);
    srvAt(cpu,slot.layer.Get(),kLayerFormat);
    srvAt(step(cpu,1),previous->layer.Get(),kLayerFormat);
    uavAt(step(cpu,2),nullptr,kLayerFormat);
    uavAt(step(cpu,3),slot.motion.Get(),kMotionFormat);
  }
  ID3D12DescriptorHeap* heaps[]={computeHeap.Get()};
  captureList->SetDescriptorHeaps(1,heaps);
  captureList->SetComputeRootSignature(computeRoot.Get());
  const float tolerance=kTolerance, ramp=kRamp;
  uint32_t constants[5]={width,height,0,0,uint32_t(kSearchRadius)};
  std::memcpy(&constants[2],&tolerance,4); std::memcpy(&constants[3],&ramp,4);
  captureList->SetComputeRoot32BitConstants(0,5,constants,0);
  Transition(captureList.Get(),slot.layer.Get(),kReadable,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  captureList->SetPipelineState(layerPipeline.Get());
  captureList->SetComputeRootDescriptorTable(1,gpuBase(0));
  captureList->Dispatch((width+7)/8,(height+7)/8,1);
  Transition(captureList.Get(),slot.layer.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,kReadable);
  slot.moving=previous!=nullptr;
  if(previous) {
    Transition(captureList.Get(),slot.motion.Get(),kReadable,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    captureList->SetPipelineState(motionPipeline.Get());
    captureList->SetComputeRootDescriptorTable(1,gpuBase(1));
    captureList->Dispatch((blocksX+7)/8,(blocksY+7)/8,1);
    Transition(captureList.Get(),slot.motion.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,kReadable);
  }
  slot.width=width; slot.height=height;
  pendingId=id; // published in SubmitCapture
  *hudless=slot.clean.Get(); *ui=emptyUi.Get();
  return captureList.Get();
}
bool HudInterpolator::SubmitCapture(ID3D12CommandQueue* queue) {
  std::lock_guard guard(lock);
  if(!captureOpen) return Fail(E_UNEXPECTED);
  captureOpen=false;
  auto& slot=slots[captureSlot];
  const uint64_t id=pendingId;
  HRESULT hr=captureList->Close();
  if(FAILED(hr)) return Fail(hr);
  // Never overwrite a slot that a queued composition still reads.
  if(slot.lastUse && FAILED(hr=queue->Wait(composeFence.Get(),slot.lastUse))) return Fail(hr);
  ID3D12CommandList* lists[]={captureList.Get()};
  queue->ExecuteCommandLists(1,lists);
  const uint64_t value=nextCapture++;
  if(FAILED(hr=queue->Signal(captureFence.Get(),value))) return Fail(hr);
  slot.captured=value; slot.id=id; slot.lastUse=0;
  ++counters.captures;
  if(slot.moving) ++counters.motionFields;
  return true;
}
bool HudInterpolator::Compose(ID3D12CommandQueue* queue,ID3D12Resource* target,uint64_t id,float phase) {
  std::lock_guard guard(lock);
  Slot* current=nullptr; Slot* previous=nullptr;
  for(auto& slot:slots) {
    if(slot.id==id) current=&slot;
    else if(id && slot.id==id-1) previous=&slot;
  }
  if(!current) { ++counters.missing; return false; }
  if(!previous) ++counters.previousMissing;
  const auto td=target?target->GetDesc():D3D12_RESOURCE_DESC{};
  if(!target || td.Width!=current->width || td.Height!=current->height
      || (previous && (previous->width!=current->width || previous->height!=current->height))) { ++counters.missing; return false; }
  composeSlot=(composeSlot+1)%kComposeSlots;
  if(!Wait(composeFence.Get(),composeValues[composeSlot],event,250)) return Fail(HRESULT_FROM_WIN32(WAIT_TIMEOUT));
  HRESULT hr=composeAllocators[composeSlot]->Reset();
  if(SUCCEEDED(hr)) hr=composeList->Reset(composeAllocators[composeSlot].Get(),pipeline.Get());
  if(FAILED(hr)) return Fail(hr);
  const Slot& zero=previous?*previous:*current;
  auto cpu=srvHeap->GetCPUDescriptorHandleForHeapStart(); cpu.ptr+=SIZE_T(composeSlot)*3*srvSize;
  auto gpu=srvHeap->GetGPUDescriptorHandleForHeapStart(); gpu.ptr+=UINT64(composeSlot)*3*srvSize;
  const std::pair<ID3D12Resource*,DXGI_FORMAT> views[3]={{zero.layer.Get(),kLayerFormat},{current->layer.Get(),kLayerFormat},
      {current->motion.Get(),kMotionFormat}};
  for(const auto& [resource,viewFormat]:views) {
    D3D12_SHADER_RESOURCE_VIEW_DESC view{}; view.Format=viewFormat; view.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;
    view.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; view.Texture2D.MipLevels=1;
    device->CreateShaderResourceView(resource,&view,cpu); cpu.ptr+=srvSize;
  }
  auto rtv=rtvHeap->GetCPUDescriptorHandleForHeapStart(); rtv.ptr+=SIZE_T(composeSlot)*rtvSize;
  D3D12_RENDER_TARGET_VIEW_DESC rtvDesc{}; rtvDesc.Format=format; rtvDesc.ViewDimension=D3D12_RTV_DIMENSION_TEXTURE2D;
  device->CreateRenderTargetView(target,&rtvDesc,rtv);
  Transition(composeList.Get(),target,D3D12_RESOURCE_STATE_PRESENT,D3D12_RESOURCE_STATE_RENDER_TARGET);
  const D3D12_VIEWPORT viewport{0,0,float(td.Width),float(td.Height),0,1};
  const D3D12_RECT scissor{0,0,LONG(td.Width),LONG(td.Height)};
  composeList->RSSetViewports(1,&viewport); composeList->RSSetScissorRects(1,&scissor);
  composeList->OMSetRenderTargets(1,&rtv,FALSE,nullptr);
  ID3D12DescriptorHeap* heaps[]={srvHeap.Get()};
  composeList->SetDescriptorHeaps(1,heaps);
  composeList->SetGraphicsRootSignature(root.Get());
  const float constants[6]={std::clamp(phase,0.0f,1.0f),previous?1.0f:0.0f,previous && current->moving?1.0f:0.0f,0.0f,
      1.0f/float(td.Width),1.0f/float(td.Height)};
  composeList->SetGraphicsRoot32BitConstants(0,6,constants,0);
  composeList->SetGraphicsRootDescriptorTable(1,gpu);
  composeList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  composeList->DrawInstanced(3,1,0,0);
  Transition(composeList.Get(),target,D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_PRESENT);
  if(FAILED(hr=composeList->Close())) return Fail(hr);
  // The captures ran on the source queue; this queue may be another one.
  const uint64_t ready=(std::max)(current->captured,previous?previous->captured:uint64_t{0});
  if(FAILED(hr=queue->Wait(captureFence.Get(),ready))) return Fail(hr);
  ID3D12CommandList* lists[]={composeList.Get()};
  queue->ExecuteCommandLists(1,lists);
  const uint64_t value=nextCompose++;
  if(FAILED(hr=queue->Signal(composeFence.Get(),value))) return Fail(hr);
  composeValues[composeSlot]=value;
  current->lastUse=value; if(previous) previous->lastUse=value;
  ++counters.composed;
  return true;
}
bool HudInterpolator::Drain(uint32_t timeoutMs) {
  bool ok=true;
  if(captureFence && nextCapture>1) ok=Wait(captureFence.Get(),nextCapture-1,event,timeoutMs) && ok;
  if(composeFence && nextCompose>1) ok=Wait(composeFence.Get(),nextCompose-1,event,timeoutMs) && ok;
  return ok;
}
}
