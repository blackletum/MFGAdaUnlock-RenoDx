// SPDX-License-Identifier: MIT
#include <iostream>
#include <vector>
#include "../src/addons/mfgunlock/framecount.hpp"
#define CHECK(x) do { if (!(x)) { std::cerr << "failed " << __LINE__ << ": " #x "\n"; return 1; } } while(0)
namespace fc = mfgunlock::framecount;
extern "C" __declspec(dllexport) void ReShadeLogMessage(void*, int, const char*) {}
std::vector<uint32_t> calls;
bool reject_override = false;
sl::Result Reflex(const sl::ReflexOptions& o) {
  calls.push_back(o.frameLimitUs);
  return reject_override && o.frameLimitUs != 0 ? sl::Result::eErrorInvalidState : sl::Result::eOk;
}
sl::Result Reentrant(const sl::ReflexOptions& o) { return fc::internal::HookedReflexSetOptions(o); }
struct Frame : sl::FrameToken {
  uint32_t value;
  explicit Frame(uint32_t v):value(v) {}
  operator uint32_t() const override { return value; }
};
uint32_t nonnull = 0;
sl::Result Tags(const sl::FrameToken&, const sl::ViewportHandle&, const sl::ResourceTag* t,
                 uint32_t n, sl::CommandBuffer*) {
  nonnull=0; for(uint32_t i=0;i<n;++i) nonnull += t[i].resource != nullptr;
  return sl::Result::eOk;
}
int main() {
  reshade::internal::get_reshade_module_handle(GetModuleHandleW(nullptr));
  fc::internal::g_real_reflex_set_options.store(&Reflex);
  fc::g_latency_guard_mode.store(1);
  sl::ReflexOptions native;
  CHECK(fc::internal::HookedReflexSetOptions(native)==sl::Result::eOk);
  CHECK(calls.size()==1);
  fc::internal::RefreshReflexTarget(); fc::internal::RefreshReflexTarget(); CHECK(calls.size()==1);
  CHECK(fc::internal::HookedReflexSetOptions(native)==sl::Result::eOk); CHECK(calls.size()==2);
  fc::internal::g_reflex_owner_thread.store(GetCurrentThreadId()+1);
  fc::internal::RefreshReflexTarget(); CHECK(calls.size()==2 && fc::g_latency_guard_refresh_pending.load());
  fc::internal::g_reflex_owner_thread.store(GetCurrentThreadId());
  fc::g_latency_guard_mode.store(2); fc::g_latency_guard_auto_cap_ready.store(true);
  fc::g_latency_guard_active_source_cap_fps.store(97);
  reject_override=true; calls.clear();
  CHECK(fc::internal::HookedReflexSetOptions(native)==sl::Result::eOk);
  CHECK(calls.size()==2 && calls[0]>0 && calls[1]==0);
  CHECK(!fc::g_reflex_limit_applied.load() && !fc::g_latency_guard_auto_cap_ready.load());
  // Unknown extension must reach native exactly once, never be retained/replayed.
  sl::ReflexOptions extension; native.next=&extension; calls.clear();
  CHECK(fc::internal::HookedReflexSetOptions(native)==sl::Result::eOk);
  fc::internal::RefreshReflexTarget(); CHECK(calls.size()==1 && !fc::g_reflex_options_seen.load());
  native.next=nullptr; fc::internal::g_real_reflex_set_options.store(&Reentrant);
  CHECK(fc::internal::HookedReflexSetOptions(native)==sl::Result::eErrorInvalidState);

  fc::g_hdr_compatibility_mode.store(unsigned(fc::HdrCompatibilityMode::kAutomaticHybrid));
  fc::g_format_api.store(mfgunlock::qualityguard::FormatApi::kDxgi);
  fc::g_hdr_active.store(false);
  fc::internal::g_real_set_tag_for_frame=&Tags;
  sl::Resource color(sl::ResourceType::eTex2d, reinterpret_cast<void*>(1), 0);
  color.width=1920; color.height=1080; color.nativeFormat=28;
  sl::ResourceTag pair[] = {
    {&color,sl::kBufferTypeHUDLessColor,sl::ResourceLifecycle::eValidUntilPresent},
    {&color,sl::kBufferTypeUIColorAndAlpha,sl::ResourceLifecycle::eValidUntilPresent}};
  sl::ViewportHandle viewport(3); Frame f1(1), f2(2), f3(3), f4(4);
  CHECK(fc::internal::HookedSetTagForFrame(f1,viewport,pair,2,nullptr)==sl::Result::eOk && nonnull==2);
  const auto resets=fc::g_quality_resets_requested.load();
  CHECK(fc::internal::HookedSetTagForFrame(f2,viewport,pair,2,nullptr)==sl::Result::eOk && nonnull==2);
  CHECK(fc::g_quality_resets_requested.load()==resets); // no per-frame history reset
  CHECK(fc::internal::HookedSetTagForFrame(f3,viewport,pair,1,nullptr)==sl::Result::eOk && nonnull==0);
  CHECK(fc::internal::HookedSetTagForFrame(f4,viewport,pair+1,1,nullptr)==sl::Result::eOk && nonnull==0);
  std::cout << "Reflex replay and HUD frame-lifetime tests passed\n";
}
