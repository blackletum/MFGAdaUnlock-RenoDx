// SPDX-License-Identifier: MIT
#pragma once
#include <d3d12.h>
#include <wrl/client.h>
#include <cstdint>

namespace mfgunlock::xemfg::native {
// 1.7 (plan E.2): HUD stabilization for games that provide no HUD-less color.
// Per real frame, on its own list (own descriptor heap; the game's bound heaps
// are never touched) on the presentation queue before Present:
//   hudless = a copy of the back buffer,
//   ui      = back-buffer pixels unchanged for `threshold` real frames, alpha 1;
//             transparent elsewhere (native_ui_mask.hlsl).
// XeFG then interpolates the scene and composes the unchanged HUD from the
// newest real frame. Both outputs are left in NON_PIXEL_SHADER_RESOURCE; tags
// may be recorded on the returned list before Submit.
class UiMask {
 public:
  static constexpr uint32_t kSlots=3;
  static constexpr uint32_t kThreshold=4;
  static constexpr float kTolerance=2.0f/255.0f;
  // Formats XeFG accepts for both inputs that this pass can also write.
  static bool SupportedFormat(DXGI_FORMAT format);
  bool Initialize(ID3D12Device* device,DXGI_FORMAT format);
  ID3D12GraphicsCommandList* Record(ID3D12Resource* backbuffer,D3D12_RESOURCE_STATES state,
      ID3D12Resource** hudless,ID3D12Resource** ui);
  bool Submit(ID3D12CommandQueue* queue);
  // Waits for every submitted pass (retirement).
  bool Drain(uint32_t timeoutMs);
  // The next Record starts a new history (resize, pause, history reset).
  void Reset() { history=false; }
  HRESULT LastError() const { return error; }
  ~UiMask();
 private:
  bool Fail(HRESULT hr) { error=hr; return false; }
  bool Ensure(Microsoft::WRL::ComPtr<ID3D12Resource>& texture,DXGI_FORMAT format,uint32_t width,uint32_t height,
      D3D12_RESOURCE_FLAGS flags,D3D12_RESOURCE_STATES initial);
  Microsoft::WRL::ComPtr<ID3D12Device> device;
  Microsoft::WRL::ComPtr<ID3D12RootSignature> root;
  Microsoft::WRL::ComPtr<ID3D12PipelineState> pipeline;
  Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> heap;
  Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocators[kSlots];
  Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> list;
  Microsoft::WRL::ComPtr<ID3D12Fence> fence;
  Microsoft::WRL::ComPtr<ID3D12Resource> hudless[kSlots], ui[kSlots], persist[2];
  HANDLE event{};
  uint64_t fenceValues[kSlots]{}, nextFence{1};
  uint32_t slot{}, descriptorSize{}, persistIndex{};
  DXGI_FORMAT format{DXGI_FORMAT_UNKNOWN};
  bool open{}, history{};
  HRESULT error{S_OK};
};
}
