// SPDX-License-Identifier: MIT
// Exercise production wrappers with deterministic providers, without a GPU.
#include <cstdlib>
#include <iostream>
#include <vector>

#include "../src/addons/mfgunlock/framecount.hpp"
#include "../src/addons/mfgunlock/midpoint.hpp"

#define CHECK(condition)                                                        \
  do {                                                                          \
    if (!(condition)) {                                                         \
      std::cerr << "FAILED: " #condition " at " << __LINE__ << '\n';          \
      std::abort();                                                             \
    }                                                                           \
  } while (false)

namespace fc = mfgunlock::framecount;
std::vector<unsigned int> requests;
unsigned int accepted_count = 1;
bool transient = false;
sl::Result Provider(const sl::ViewportHandle&, const sl::DLSSGOptions& options) {
  requests.push_back(options.numFramesToGenerate);
  if (options.mode == sl::DLSSGMode::eOff) return sl::Result::eOk;
  if (transient && requests.size() == 1) return sl::Result::eErrorInvalidState;
  return options.numFramesToGenerate == accepted_count
      ? sl::Result::eOk : sl::Result::eErrorFeatureNotSupported;
}

int main() {
  fc::internal::g_real_set_options.store(&Provider);
  // Standalone tests have no ReShade logger. Mark one-shot runtime messages as
  // already emitted while still exercising every state and fallback branch.
  fc::g_intercepted.store(true);
  fc::g_force_failed_for.store(3);
  fc::g_force_multiplier.store(4);
  sl::DLSSGOptions options{};
  options.mode = sl::DLSSGMode::eOn;
  options.numFramesToGenerate = 1;
  const sl::ViewportHandle viewport(0);
  CHECK(fc::internal::HookedSetOptions(viewport, options) == sl::Result::eOk);
  if (requests != std::vector<unsigned int>{3, 3, 1}) {
    std::cerr << "first sequence:";
    for (const auto value : requests) std::cerr << ' ' << value;
    std::cerr << '\n';
  }
  CHECK((requests == std::vector<unsigned int>{3, 3, 1}));
  CHECK(fc::g_last_effective_generated.load() == 1);
  CHECK(fc::g_fixed_override_status.load() == static_cast<unsigned int>(
      mfgunlock::forcepolicy::FixedOverrideStatus::kRejected));
  CHECK(options.numFramesToGenerate == 1);

  accepted_count = 3;
  transient = true;
  requests.clear();
  CHECK(fc::internal::HookedSetOptions(viewport, options) == sl::Result::eOk);
  CHECK((requests == std::vector<unsigned int>{3, 3}));
  CHECK(fc::g_last_effective_generated.load() == 3);
  CHECK(fc::g_fixed_override_status.load() == static_cast<unsigned int>(
      mfgunlock::forcepolicy::FixedOverrideStatus::kApplied));

  // A forced lower multiplier must replace a game's higher multiplier too.
  transient = false;
  accepted_count = 1;
  options.numFramesToGenerate = 3;
  fc::g_force_multiplier.store(2);
  requests.clear();
  CHECK(fc::internal::HookedSetOptions(viewport, options) == sl::Result::eOk);
  CHECK((requests == std::vector<unsigned int>{1}));
  CHECK(fc::g_last_effective_generated.load() == 1);
  CHECK(options.numFramesToGenerate == 3);

  options.mode = sl::DLSSGMode::eOff;
  requests.clear();
  CHECK(fc::internal::HookedSetOptions(viewport, options) == sl::Result::eOk);
  CHECK((requests == std::vector<unsigned int>{3}));
  CHECK(!fc::g_effective_request_seen.load());

  // Preflight failure must not poison storage used by future hook retries.
  void* first = nullptr;
  void* second = reinterpret_cast<void*>(1);
  const std::vector<mfgunlock::hook::HookItem> hooks = {
      {"GetTickCount", &first, reinterpret_cast<void*>(&Provider)},
      {"GetCurrentProcessId", &second, reinterpret_cast<void*>(&Provider)}};
  CHECK(!mfgunlock::hook::Install(GetModuleHandleW(L"kernel32.dll"), hooks, "test"));
  CHECK(first == nullptr && second == reinterpret_cast<void*>(1));

  // A failed descriptor restore must retain its backing allocation rather than
  // leave a live descriptor pointing at freed memory.
  void* temporal_allocation =
      VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
  CHECK(temporal_allocation != nullptr);
  std::vector<mfgunlock::midpoint::Patch> failed_restore = {
      {reinterpret_cast<uint64_t*>(1), 0}};
  mfgunlock::midpoint::Restore(failed_restore, temporal_allocation);
  CHECK(temporal_allocation != nullptr);
  CHECK(VirtualFree(temporal_allocation, 0, MEM_RELEASE) != 0);

  std::cout << "runtime hook tests passed\n";
}
