// SPDX-License-Identifier: MIT
// 1.7 (plan I): HUD interpolation on a WARP device. Two real frames are
// captured (back buffer + HUD-less); a generated frame (the interpolated,
// HUD-free scene) then receives the HUD at phase t:
//   HUD in both frames  -> (1-t)*back0 + t*back1, fully covered;
//   HUD only in frame 0 -> fades out over the scene: (1-t)*back0 + t*scene;
//   HUD that moves     -> drawn along its own motion, halfway at t = 0.5;
//   HUD only in frame 1 -> fades in: t*back1 + (1-t)*scene;
//   no HUD              -> the scene, untouched.
// A frame id that was never captured leaves the target untouched.
#include "../src/addons/mfgunlock/xemfg/native_hud_interpolation.hpp"
#include <dxgi1_6.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <vector>
using Microsoft::WRL::ComPtr;
using namespace mfgunlock::xemfg::native;
namespace {
void Check(HRESULT hr,const char* what) { if(FAILED(hr)) throw std::runtime_error(what); }
void Require(bool ok,const char* what) { if(!ok) throw std::runtime_error(what); }
struct Gpu {
  ComPtr<ID3D12Device> device; ComPtr<ID3D12CommandQueue> queue;
  ComPtr<ID3D12CommandAllocator> allocator; ComPtr<ID3D12GraphicsCommandList> list;
  ComPtr<ID3D12Fence> fence; uint64_t value{};
  Gpu() {
    ComPtr<IDXGIFactory4> factory; Check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)),"factory");
    ComPtr<IDXGIAdapter> warp; Check(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp)),"warp adapter");
    Check(D3D12CreateDevice(warp.Get(),D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&device)),"warp device");
    D3D12_COMMAND_QUEUE_DESC qd{D3D12_COMMAND_LIST_TYPE_DIRECT}; Check(device->CreateCommandQueue(&qd,IID_PPV_ARGS(&queue)),"queue");
    Check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocator)),"allocator");
    Check(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator.Get(),nullptr,IID_PPV_ARGS(&list)),"list");
    Check(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence)),"fence");
  }
  void Run() {
    Check(list->Close(),"close"); ID3D12CommandList* raw=list.Get(); queue->ExecuteCommandLists(1,&raw);
    Check(queue->Signal(fence.Get(),++value),"signal");
    while(fence->GetCompletedValue()<value) Sleep(0);
    Check(allocator->Reset(),"allocator reset"); Check(list->Reset(allocator.Get(),nullptr),"list reset");
  }
  void Flush() { Check(queue->Signal(fence.Get(),++value),"signal"); while(fence->GetCompletedValue()<value) Sleep(0); }
  ComPtr<ID3D12Resource> Buffer(D3D12_HEAP_TYPE type,uint64_t size) {
    D3D12_HEAP_PROPERTIES heap{type}; D3D12_RESOURCE_DESC d{};
    d.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER; d.Width=size; d.Height=1; d.DepthOrArraySize=1; d.MipLevels=1;
    d.SampleDesc.Count=1; d.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> r;
    Check(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&d,
        type==D3D12_HEAP_TYPE_UPLOAD?D3D12_RESOURCE_STATE_GENERIC_READ:D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&r)),"buffer");
    return r;
  }
  void Barrier(ID3D12Resource* r,D3D12_RESOURCE_STATES from,D3D12_RESOURCE_STATES to) {
    D3D12_RESOURCE_BARRIER b{}; b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition={r,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,from,to}; list->ResourceBarrier(1,&b);
  }
  // An RGBA8 texture left in PRESENT (COMMON); `target` adds render-target use.
  ComPtr<ID3D12Resource> Texture(const std::vector<uint32_t>& pixels,unsigned width,unsigned height,bool target=false) {
    D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT}; D3D12_RESOURCE_DESC d{};
    d.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D; d.Width=width; d.Height=height; d.DepthOrArraySize=1; d.MipLevels=1;
    d.Format=DXGI_FORMAT_R8G8B8A8_UNORM; d.SampleDesc.Count=1;
    if(target) d.Flags=D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    ComPtr<ID3D12Resource> texture;
    Check(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&d,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&texture)),"texture");
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT layout{}; uint64_t total{};
    device->GetCopyableFootprints(&d,0,1,0,&layout,nullptr,nullptr,&total);
    auto upload=Buffer(D3D12_HEAP_TYPE_UPLOAD,total);
    uint8_t* mapped{}; Check(upload->Map(0,nullptr,reinterpret_cast<void**>(&mapped)),"map upload");
    for(unsigned y=0;y<height;++y) memcpy(mapped+layout.Offset+size_t(y)*layout.Footprint.RowPitch,&pixels[size_t(y)*width],width*4);
    upload->Unmap(0,nullptr);
    D3D12_TEXTURE_COPY_LOCATION dst{texture.Get(),D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX,{}};
    D3D12_TEXTURE_COPY_LOCATION src{upload.Get(),D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT,{}}; src.PlacedFootprint=layout;
    list->CopyTextureRegion(&dst,0,0,0,&src,nullptr);
    Barrier(texture.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_PRESENT);
    Run();
    return texture;
  }
  // Reads a PRESENT (COMMON) RGBA8 texture.
  std::vector<uint32_t> Read(ID3D12Resource* texture) {
    const auto d=texture->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT layout{}; uint64_t total{};
    device->GetCopyableFootprints(&d,0,1,0,&layout,nullptr,nullptr,&total);
    auto readback=Buffer(D3D12_HEAP_TYPE_READBACK,total);
    Barrier(texture,D3D12_RESOURCE_STATE_PRESENT,D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION dst{readback.Get(),D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT,{}}; dst.PlacedFootprint=layout;
    D3D12_TEXTURE_COPY_LOCATION src{texture,D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX,{}};
    list->CopyTextureRegion(&dst,0,0,0,&src,nullptr);
    Barrier(texture,D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_PRESENT);
    Run();
    std::vector<uint32_t> out(size_t(d.Width)*d.Height);
    uint8_t* mapped{}; Check(readback->Map(0,nullptr,reinterpret_cast<void**>(&mapped)),"map readback");
    for(unsigned y=0;y<d.Height;++y) memcpy(&out[size_t(y)*d.Width],mapped+layout.Offset+size_t(y)*layout.Footprint.RowPitch,size_t(d.Width)*4);
    readback->Unmap(0,nullptr);
    return out;
  }
};
constexpr unsigned kWidth=96, kHeight=24;
uint32_t Pack(float r,float g,float b) {
  const auto c=[](float v){ return uint32_t(std::lround(std::fmin(std::fmax(v,0.f),1.f)*255.f)); };
  return 0xFF000000u|c(r)|(c(g)<<8)|(c(b)<<16);
}
float Channel(uint32_t v,int i) { return float((v>>(8*i))&0xFF)/255.f; }
// Static regions, farther apart than the motion search radius:
// x in [24,32) HUD in both frames; [0,8) only frame 0; [64,72) only frame 1.
int Region(unsigned x) { return x>=24 && x<32 ? 3 : x<8 ? 1 : x>=64 && x<72 ? 2 : 0; }
uint32_t Clean(unsigned x,unsigned y,unsigned frame) { return Pack(.1f+.02f*float(x%7),.2f+.03f*float(y%5),.3f+.1f*float(frame)); }
uint32_t Hud(unsigned frame) { return frame?Pack(.9f,.8f,.1f):Pack(.7f,.95f,.2f); }
struct Captured { std::vector<uint32_t> back, clean; };
// Captures one real frame with id `id`; `hud(x,y)` returns true where HUD is.
template<class F> Captured CaptureFrame(Gpu& gpu,HudInterpolator& hud,uint64_t id,unsigned frame,F covered,uint32_t color) {
  Captured c;
  c.back.resize(size_t(kWidth)*kHeight); c.clean.resize(c.back.size());
  for(unsigned y=0;y<kHeight;++y) for(unsigned x=0;x<kWidth;++x) {
    const size_t i=size_t(y)*kWidth+x;
    c.clean[i]=Clean(x,y,frame);
    c.back[i]=covered(x,y)?color:c.clean[i];
  }
  auto b=gpu.Texture(c.back,kWidth,kHeight), cl=gpu.Texture(c.clean,kWidth,kHeight);
  ID3D12Resource* hudless{}; ID3D12Resource* ui{};
  Require(hud.Capture(id,b.Get(),D3D12_RESOURCE_STATE_PRESENT,cl.Get(),D3D12_RESOURCE_STATE_PRESENT,{0,0,kWidth,kHeight},
      &hudless,&ui)!=nullptr && hudless && ui,"capture");
  Require(hud.SubmitCapture(gpu.queue.Get()),"submit capture");
  gpu.Flush();
  Require(gpu.Read(ui)==std::vector<uint32_t>(c.back.size(),0u),"UI input is not empty");
  return c;
}
}
int main() {
  try {
    Gpu gpu; HudInterpolator hud;
    Require(HudInterpolator::SupportedFormat(DXGI_FORMAT_R8G8B8A8_UNORM)
        && !HudInterpolator::SupportedFormat(DXGI_FORMAT_R16G16B16A16_FLOAT),"format filter");
    Check(hud.Initialize(gpu.device.Get(),DXGI_FORMAT_R8G8B8A8_UNORM)?S_OK:hud.LastError(),"initialization");
    Captured frames[2];
    for(unsigned f=0;f<2;++f)
      frames[f]=CaptureFrame(gpu,hud,10+f,f,[f](unsigned x,unsigned) { const int r=Region(x); return r==3 || (r==1 && f==0) || (r==2 && f==1); },Hud(f));
    // The generated frame: the interpolated scene, HUD-free.
    const std::vector<uint32_t> scene(frames[0].back.size(),Pack(.5f,.4f,.35f));
    float worst=0;
    for(const float t:{0.25f,0.5f,0.75f}) {
      auto target=gpu.Texture(scene,kWidth,kHeight,true);
      Require(hud.Compose(gpu.queue.Get(),target.Get(),11,t),"compose");
      gpu.Flush();
      const auto out=gpu.Read(target.Get());
      for(unsigned y=0;y<kHeight;++y) for(unsigned x=0;x<kWidth;++x) {
        const size_t i=size_t(y)*kWidth+x;
        const int r=Region(x);
        for(int ch=0;ch<3;++ch) {
          const float s=Channel(scene[i],ch), b0=Channel(frames[0].back[i],ch), b1=Channel(frames[1].back[i],ch);
          const float expected=r==3?(1-t)*b0+t*b1:r==1?(1-t)*b0+t*s:r==2?t*b1+(1-t)*s:s;
          worst=std::fmax(worst,std::fabs(Channel(out[i],ch)-expected));
        }
      }
    }
    Require(worst<=2.5f/255.f,"static HUD composition differs from the cross-fade");

    // Moving HUD: an 8x8 block moves 8 px to the right between two real
    // frames. At t = 0.5 it is drawn halfway, x in [44,52), fully opaque; the
    // old and new positions outside that are left to the scene.
    const auto block=[](unsigned x0) { return [x0](unsigned x,unsigned y) { return x>=x0 && x<x0+8 && y>=8 && y<16; }; };
    const uint32_t color=Pack(.95f,.9f,.2f);
    CaptureFrame(gpu,hud,20,0,block(40),color);
    CaptureFrame(gpu,hud,21,1,block(48),color);
    Require(hud.Snapshot().motionFields>=2,"no motion field for consecutive frames");
    auto moving=gpu.Texture(scene,kWidth,kHeight,true);
    Require(hud.Compose(gpu.queue.Get(),moving.Get(),21,0.5f),"compose moving");
    gpu.Flush();
    const auto out=gpu.Read(moving.Get());
    for(unsigned y=9;y<15;++y) {
      for(unsigned x=45;x<51;++x) Require(out[size_t(y)*kWidth+x]==color,"moving HUD not drawn halfway");
      for(unsigned x:{40u,41u,42u,54u,55u}) Require(out[size_t(y)*kWidth+x]==scene[0],"moving HUD left a trail");
    }

    // An id that was never captured: the generated frame is left as is.
    auto untouched=gpu.Texture(scene,kWidth,kHeight,true);
    Require(!hud.Compose(gpu.queue.Get(),untouched.Get(),99,0.5f),"unknown frame composed");
    Require(gpu.Read(untouched.Get())==scene,"unknown frame changed the output");
    std::printf("PASS: static HUD cross-faded (max error %.4f), moving HUD drawn halfway without a trail, unknown frame untouched\n",worst);
    return 0;
  } catch(const std::exception& e) { std::fprintf(stderr,"%s\n",e.what()); return 1; }
}
