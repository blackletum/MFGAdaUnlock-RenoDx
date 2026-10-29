// SPDX-License-Identifier: MIT
#pragma once
#include <d3d12.h>
#include <wrl/client.h>
#include <cstdint>
#include <mutex>
#include "native_dejitter.hpp"

namespace mfgunlock::xemfg::native {
// 1.7 (plan I): HUD interpolation. XeFG interpolates only the game's HUD-less
// scene (with an empty UI); this class draws the HUD on every generated frame.
//
// Capture, per real frame, on the source queue before its Present: copies of
// the back buffer and of the game's HUD-less image into a ring slot keyed by
// the frame id; the HUD layer (premultiplied, native_hud_motion.hlsl LayerCS);
// and, when the previous frame id was captured, the HUD's own motion between
// the two (MotionCS, one offset per 8x8 block). Then a fence signal. The
// HUD-less copy and an empty UI texture are what XeFG receives.
//
// Compose, per generated output, on XeFG's presentation queue inside the
// final swap chain's Present: waits for the captures, then draws the HUD at
// the output's phase, moved along its motion (native_hud_interpolation.hlsl).
// Each slot remembers the presentation fence value of its last use; capture
// waits for it before overwriting.
class HudInterpolator {
 public:
  static constexpr uint32_t kSlots=6;
  static constexpr float kTolerance=3.0f/255.0f;
  // Full coverage just above the tolerance: every HUD pixel, also thin and
  // semi-transparent ones, is drawn as opaque as in the real frame.
  static constexpr float kRamp=128.0f;
  static constexpr int kSearchRadius=64;
  static bool SupportedFormat(DXGI_FORMAT format);
  bool Initialize(ID3D12Device* device,DXGI_FORMAT format);
  // Source side. `clean` is the game's HUD-less color (region `area`), equal
  // in size to the back buffer. Returns the open list for XeFG's tags, or
  // nullptr; *hudless and *ui receive the textures to tag.
  ID3D12GraphicsCommandList* Capture(uint64_t id,ID3D12Resource* backbuffer,D3D12_RESOURCE_STATES backState,
      ID3D12Resource* clean,D3D12_RESOURCE_STATES cleanState,const DejitterRegion& area,
      ID3D12Resource** hudless,ID3D12Resource** ui);
  bool SubmitCapture(ID3D12CommandQueue* queue);
  // Presentation side: draws the HUD between real frames id-1 and id at
  // `phase` into `target` (a swap-chain buffer in PRESENT state) on `queue`.
  // False when the frame's capture is unknown; the output is then left as is.
  bool Compose(ID3D12CommandQueue* queue,ID3D12Resource* target,uint64_t id,float phase);
  bool Drain(uint32_t timeoutMs);
  HRESULT LastError() const { return error; }
  struct Counters { uint64_t captures{}, composed{}, missing{}, previousMissing{}, motionFields{}; };
  Counters Snapshot() const { std::lock_guard guard(lock); return counters; }
  ~HudInterpolator();
 private:
  struct Slot {
    Microsoft::WRL::ComPtr<ID3D12Resource> back, clean, layer, motion;
    uint64_t id{UINT64_MAX}, captured{}, lastUse{};
    uint32_t width{}, height{};
    bool moving{};
  };
  bool Fail(HRESULT hr) { error=hr; return false; }
  bool Ensure(Microsoft::WRL::ComPtr<ID3D12Resource>& texture,DXGI_FORMAT textureFormat,uint32_t width,uint32_t height,
      D3D12_RESOURCE_FLAGS flags,D3D12_RESOURCE_STATES initial);
  mutable std::mutex lock;
  Microsoft::WRL::ComPtr<ID3D12Device> device;
  Microsoft::WRL::ComPtr<ID3D12RootSignature> root, computeRoot;
  Microsoft::WRL::ComPtr<ID3D12PipelineState> pipeline, layerPipeline, motionPipeline;
  Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> srvHeap, rtvHeap, computeHeap;
  // Capture: own allocators/list on the source queue.
  Microsoft::WRL::ComPtr<ID3D12CommandAllocator> captureAllocators[kSlots];
  Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> captureList;
  Microsoft::WRL::ComPtr<ID3D12Fence> captureFence;
  uint64_t nextCapture{1};
  // Compose: own allocators/list on the presentation queue.
  static constexpr uint32_t kComposeSlots=8;
  Microsoft::WRL::ComPtr<ID3D12CommandAllocator> composeAllocators[kComposeSlots];
  Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> composeList;
  Microsoft::WRL::ComPtr<ID3D12Fence> composeFence;
  uint64_t composeValues[kComposeSlots]{}, nextCompose{1};
  uint32_t composeSlot{};
  Microsoft::WRL::ComPtr<ID3D12Resource> emptyUi;
  bool emptyCleared{};
  Slot slots[kSlots];
  uint32_t captureSlot{}, emptyWidth{}, emptyHeight{};
  uint64_t pendingId{};
  HANDLE event{};
  uint32_t srvSize{}, rtvSize{};
  DXGI_FORMAT format{DXGI_FORMAT_UNKNOWN};
  bool captureOpen{};
  Counters counters;
  HRESULT error{S_OK};
};
}
