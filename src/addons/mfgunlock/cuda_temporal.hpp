/* SPDX-License-Identifier: MIT
 * Phase-validated CUDA confidence history for Adaptive Quality V3.2.
 *
 * This deliberately uses a minimal dynamically-resolved Driver API surface.
 * No CUDA SDK library is linked into the addon. The patched kernel keeps its
 * original 144-byte ABI and exposes only a private module-global control block.
 */
#pragma once

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include <include/reshade.hpp>

#include "./framecount.hpp"
#include "./ngx_hook.hpp"

namespace mfgunlock::cudatemporal {

enum class StabilityMode : uint32_t {
  kLocal = 1,
  kTemporal = 2,
};

inline constexpr StabilityMode NormalizeMode(uint32_t value) {
  return value == static_cast<uint32_t>(StabilityMode::kTemporal)
             ? StabilityMode::kTemporal
             : StabilityMode::kLocal;
}

struct Phase {
  uint32_t index = 0;
  uint32_t bucket = 0;
  bool swap = false;
  bool valid = false;
};

inline Phase ResolvePhase(float t, uint32_t multiplier) {
  Phase result{};
  if (!std::isfinite(t) || multiplier < 2 || multiplier > 6) return result;
  const float scaled = t * static_cast<float>(multiplier);
  const long rounded = std::lround(scaled);
  if (rounded <= 0 || rounded >= static_cast<long>(multiplier)) return result;
  const float exact = static_cast<float>(rounded) /
                      static_cast<float>(multiplier);
  if (std::abs(t - exact) > 1.0f / 2048.0f) return result;
  result.index = static_cast<uint32_t>(rounded);
  result.bucket = (std::min)(result.index, multiplier - result.index) - 1u;
  result.swap = result.index > multiplier - result.index;
  result.valid = true;
  return result;
}

inline uint64_t RequiredHistoryBytes(uint32_t width, uint32_t height,
                                     uint32_t multiplier) {
  if (width == 0 || height == 0 || multiplier < 2 || multiplier > 6) return 0;
  return uint64_t{width} * height * 2u * (multiplier / 2u);
}

inline constexpr uint64_t kHistoryLimit = 64ull * 1024ull * 1024ull;
inline constexpr uint32_t kHistoryMagic = 0x56333148u;

inline uint8_t UpdateHistoryReference(float current, uint8_t previous,
                                      bool symmetric_second) {
  current = std::clamp(current, 0.0f, 1.0f);
  if (previous != 255u) {
    const float old = static_cast<float>(previous) / 254.0f;
    current = (std::min)(current,
                         symmetric_second ? old : old + 0.20f);
  }
  return static_cast<uint8_t>(
      (std::min)(std::lround(current * 254.0f), 254l));
}

inline std::atomic<StabilityMode> g_mode{StabilityMode::kTemporal};
inline std::atomic_bool g_provider_authorized{false};
inline std::atomic_bool g_hooked{false};
inline std::atomic_bool g_installing{false};
inline std::atomic_bool g_install_failed{false};
// Non-null only when the addon had to acquire its own system32 reference.
// Keeping it until hook teardown guarantees that every trampoline remains
// backed by the same CUDA Driver module for the lifetime of the hooks.
inline std::atomic<HMODULE> g_cuda_reference{nullptr};
inline std::atomic_bool g_temporal_active{false};
inline std::atomic<uint64_t> g_history_bytes{0};
inline std::atomic<uint32_t> g_probe_launches{0};
inline std::atomic<uint32_t> g_probe_phase_mask{0};
inline std::atomic<uint32_t> g_history_width{0};
inline std::atomic<uint32_t> g_history_height{0};
inline std::atomic<uint32_t> g_history_multiplier{0};
inline std::atomic_bool g_fallback{false};
inline std::atomic_bool g_fast_path_ready{false};
inline SRWLOCK g_detail_lock = SRWLOCK_INIT;
inline std::string g_detail{"local stability; CUDA temporal probe not started"};

inline void SetDetail(std::string detail, bool fallback = false) {
  AcquireSRWLockExclusive(&g_detail_lock);
  g_detail = std::move(detail);
  ReleaseSRWLockExclusive(&g_detail_lock);
  g_fallback.store(fallback, std::memory_order_release);
}

inline std::string Detail() {
  AcquireSRWLockShared(&g_detail_lock);
  std::string result = g_detail;
  ReleaseSRWLockShared(&g_detail_lock);
  return result;
}

inline bool FastPathReady() {
  return g_fast_path_ready.load(std::memory_order_acquire);
}

inline void Configure(StabilityMode mode) {
  g_mode.store(mode, std::memory_order_release);
  if (mode == StabilityMode::kLocal) {
    g_temporal_active.store(false, std::memory_order_release);
    SetDetail("Local Stable selected; CUDA history disabled");
  }
}

inline void AuthorizeExactProvider(bool authorized) {
  if (authorized) g_provider_authorized.store(true, std::memory_order_release);
}

namespace internal {

using CUresult = int;
using CUdeviceptr = uint64_t;
using CUfunction = void*;
using CUmodule = void*;
using CUcontext = void*;
using CUstream = void*;
constexpr CUresult kSuccess = 0;

struct CUlaunchConfig {
  unsigned int grid_dim_x;
  unsigned int grid_dim_y;
  unsigned int grid_dim_z;
  unsigned int block_dim_x;
  unsigned int block_dim_y;
  unsigned int block_dim_z;
  unsigned int shared_mem_bytes;
  CUstream stream;
  void* attrs;
  unsigned int num_attrs;
};

using LaunchFn = CUresult(WINAPI*)(
    CUfunction, unsigned int, unsigned int, unsigned int, unsigned int,
    unsigned int, unsigned int, unsigned int, CUstream, void**, void**);
using LaunchExFn = CUresult(WINAPI*)(const CUlaunchConfig*, CUfunction, void**,
                                     void**);
using FuncGetNameFn = CUresult(WINAPI*)(const char**, CUfunction);
using FuncGetModuleFn = CUresult(WINAPI*)(CUmodule*, CUfunction);
using FuncGetParamCountFn = CUresult(WINAPI*)(CUfunction, size_t*);
using FuncGetParamInfoFn = CUresult(WINAPI*)(CUfunction, size_t, size_t*,
                                             size_t*);
using ModuleGetGlobalFn = CUresult(WINAPI*)(CUdeviceptr*, size_t*, CUmodule,
                                            const char*);
using CtxGetCurrentFn = CUresult(WINAPI*)(CUcontext*);
using MemAllocFn = CUresult(WINAPI*)(CUdeviceptr*, size_t);
using MemcpyHtoDAsyncFn = CUresult(WINAPI*)(CUdeviceptr, const void*, size_t,
                                            CUstream);
using MemcpyHtoDFn = CUresult(WINAPI*)(CUdeviceptr, const void*, size_t);
using MemcpyDtoHFn = CUresult(WINAPI*)(void*, CUdeviceptr, size_t);
using MemsetD8AsyncFn = CUresult(WINAPI*)(CUdeviceptr, unsigned char, size_t,
                                          CUstream);

inline LaunchFn g_real_launch = nullptr;
inline LaunchFn g_real_launch_ptsz = nullptr;
inline LaunchExFn g_real_launch_ex = nullptr;
inline LaunchExFn g_real_launch_ex_ptsz = nullptr;
inline FuncGetNameFn g_func_get_name = nullptr;
inline FuncGetModuleFn g_func_get_module = nullptr;
inline FuncGetParamCountFn g_func_get_param_count = nullptr;
inline FuncGetParamInfoFn g_func_get_param_info = nullptr;
inline ModuleGetGlobalFn g_module_get_global = nullptr;
inline CtxGetCurrentFn g_ctx_get_current = nullptr;
inline MemAllocFn g_mem_alloc = nullptr;
inline MemcpyHtoDAsyncFn g_memcpy_htod_async = nullptr;
inline MemcpyHtoDFn g_memcpy_htod = nullptr;
inline MemcpyDtoHFn g_memcpy_dtoh = nullptr;
inline MemsetD8AsyncFn g_memset_d8_async = nullptr;
inline std::vector<hook::HookItem> g_hooks;

#pragma pack(push, 1)
struct KernelParameters {
  uint8_t prefix[32];
  float t;
  uint8_t middle[76];
  uint32_t width;
  uint32_t height;
  uint8_t suffix[24];
};
#pragma pack(pop)
static_assert(sizeof(KernelParameters) == 144);
static_assert(offsetof(KernelParameters, t) == 32);
static_assert(offsetof(KernelParameters, width) == 112);
static_assert(offsetof(KernelParameters, height) == 116);

enum class LaunchApi : uint32_t {
  kKernel = 1,
  kKernelPtsz = 2,
  kKernelEx = 3,
  kKernelExPtsz = 4,
};

struct alignas(16) Control {
  uint64_t history = 0;
  uint32_t enabled = 0;
  uint32_t reserved = 0;
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t multiplier = 0;
  uint32_t pixel_count = 0;
};
static_assert(sizeof(Control) == 32);
static_assert(offsetof(Control, history) == 0);
static_assert(offsetof(Control, enabled) == 8);
static_assert(offsetof(Control, width) == 16);
static_assert(offsetof(Control, pixel_count) == 28);

struct State {
  SRWLOCK lock = SRWLOCK_INIT;
  CUfunction function = nullptr;
  CUmodule module = nullptr;
  CUcontext context = nullptr;
  CUstream stream = nullptr;
  CUdeviceptr control_device = 0;
  CUdeviceptr history = 0;
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t multiplier = 0;
  uint32_t phase_mask = 0;
  uint32_t valid_launches = 0;
  uint32_t last_phase = 0;
  uint32_t completed_cycles = 0;
  int32_t phase_direction = 0;
  LaunchApi launch_api = LaunchApi::kKernel;
  bool launch_api_seen = false;
  bool active = false;
  bool permanently_local = false;
  bool waiting_multiplier_reported = false;
  std::vector<std::unique_ptr<Control>> control_snapshots;
};
inline State g_state;
inline std::atomic<CUfunction> g_target_function{nullptr};
#if defined(MFGUNLOCK_CUDA_TEMPORAL_TESTING)
inline std::atomic<uint64_t> g_test_lock_acquisitions{0};
inline void ResetStateForTests() {
  AcquireSRWLockExclusive(&g_state.lock);
  g_state.function = nullptr;
  g_state.module = nullptr;
  g_state.context = nullptr;
  g_state.stream = nullptr;
  g_state.control_device = 0;
  g_state.history = 0;
  g_state.width = 0;
  g_state.height = 0;
  g_state.multiplier = 0;
  g_state.phase_mask = 0;
  g_state.valid_launches = 0;
  g_state.last_phase = 0;
  g_state.completed_cycles = 0;
  g_state.phase_direction = 0;
  g_state.launch_api = LaunchApi::kKernel;
  g_state.launch_api_seen = false;
  g_state.active = false;
  g_state.permanently_local = false;
  g_state.waiting_multiplier_reported = false;
  g_state.control_snapshots.clear();
  ReleaseSRWLockExclusive(&g_state.lock);
  g_target_function.store(nullptr, std::memory_order_release);
  g_fast_path_ready.store(false, std::memory_order_release);
}
#endif

inline bool ResolveDriverFunctions(HMODULE cuda) {
  const auto proc = [cuda](const char* name) {
    return reinterpret_cast<void*>(GetProcAddress(cuda, name));
  };
  g_func_get_name = reinterpret_cast<FuncGetNameFn>(proc("cuFuncGetName"));
  g_func_get_module =
      reinterpret_cast<FuncGetModuleFn>(proc("cuFuncGetModule"));
  g_func_get_param_count =
      reinterpret_cast<FuncGetParamCountFn>(proc("cuFuncGetParamCount"));
  g_func_get_param_info =
      reinterpret_cast<FuncGetParamInfoFn>(proc("cuFuncGetParamInfo"));
  g_module_get_global =
      reinterpret_cast<ModuleGetGlobalFn>(proc("cuModuleGetGlobal_v2"));
  g_ctx_get_current =
      reinterpret_cast<CtxGetCurrentFn>(proc("cuCtxGetCurrent"));
  g_mem_alloc = reinterpret_cast<MemAllocFn>(proc("cuMemAlloc_v2"));
  g_memcpy_htod_async = reinterpret_cast<MemcpyHtoDAsyncFn>(
      proc("cuMemcpyHtoDAsync_v2"));
  g_memcpy_htod =
      reinterpret_cast<MemcpyHtoDFn>(proc("cuMemcpyHtoD_v2"));
  g_memcpy_dtoh =
      reinterpret_cast<MemcpyDtoHFn>(proc("cuMemcpyDtoH_v2"));
  g_memset_d8_async =
      reinterpret_cast<MemsetD8AsyncFn>(proc("cuMemsetD8Async"));
  return g_func_get_name && g_func_get_module && g_func_get_param_count &&
         g_func_get_param_info && g_module_get_global && g_ctx_get_current &&
         g_mem_alloc && g_memcpy_htod_async && g_memcpy_htod &&
         g_memcpy_dtoh && g_memset_d8_async;
}

inline const uint8_t* ParameterBuffer(void** kernel_params, void** extra) {
  if (kernel_params != nullptr && kernel_params[0] != nullptr)
    return static_cast<const uint8_t*>(kernel_params[0]);
  if (extra == nullptr) return nullptr;
  const void* buffer = nullptr;
  size_t size = 0;
  for (size_t index = 0; index < 16; index += 2) {
    const uintptr_t token = reinterpret_cast<uintptr_t>(extra[index]);
    if (token == 0) break;
    if (extra[index + 1] == nullptr) return nullptr;
    if (token == 1) {
      buffer = extra[index + 1];
    } else if (token == 2) {
      std::memcpy(&size, extra[index + 1], sizeof(size));
    } else {
      return nullptr;
    }
  }
  return buffer != nullptr && size == sizeof(KernelParameters)
             ? static_cast<const uint8_t*>(buffer)
             : nullptr;
}

inline uint32_t EffectiveMultiplier() {
  if (!framecount::g_effective_request_seen.load(std::memory_order_acquire))
    return 0;
  const uint32_t generated =
      framecount::g_last_effective_generated.load(std::memory_order_relaxed);
  return generated >= 1 && generated <= 5 ? generated + 1 : 0;
}

inline void DisableDeviceHistory(State& state, const char* reason,
                                 bool write_control = true) {
  state.active = false;
  g_temporal_active.store(false, std::memory_order_release);
  g_history_bytes.store(0, std::memory_order_relaxed);
  if (write_control && state.control_device != 0 && g_memcpy_htod != nullptr) {
    Control disabled{};
    g_memcpy_htod(state.control_device, &disabled, sizeof(disabled));
  }
  SetDetail(reason, true);
}

inline bool IsTargetFunction(CUfunction function) {
  const char* name = nullptr;
  return
      g_func_get_name(&name, function) == kSuccess && name != nullptr &&
      std::strcmp(name, "Kernel_EstimateIntermMvecsScatter") == 0;
}

inline bool BindTargetFirstTime(State& state, CUfunction function,
                                CUstream stream) {
  CUmodule module = nullptr;
  CUcontext context = nullptr;
  if (g_func_get_module(&module, function) != kSuccess || module == nullptr ||
      g_ctx_get_current(&context) != kSuccess || context == nullptr) {
    state.permanently_local = true;
    SetDetail("Temporal Stable fallback: CUDA function context/module unavailable",
              true);
    return false;
  }
  if ((state.function != nullptr && state.function != function) ||
      (state.context != nullptr && state.context != context) ||
      (state.stream != nullptr && state.stream != stream)) {
    const bool context_changed =
        state.context != nullptr && state.context != context;
    state.permanently_local = true;
    DisableDeviceHistory(
        state,
        "Temporal Stable fallback: multiple CUDA functions, contexts, or streams observed",
        !context_changed);
    return false;
  }
  size_t count = 0, offset = 0, parameter_size = 0;
  if (g_func_get_param_count(function, &count) != kSuccess || count != 1 ||
      g_func_get_param_info(function, 0, &offset, &parameter_size) != kSuccess ||
      offset != 0 || parameter_size != sizeof(KernelParameters)) {
    state.permanently_local = true;
    SetDetail("Temporal Stable fallback: CUDA kernel parameter ABI is not 144 bytes",
              true);
    return false;
  }
  CUdeviceptr magic = 0, control = 0;
  size_t magic_size = 0, control_size = 0;
  if (g_module_get_global(&magic, &magic_size, module,
                          "mfgunlock_v31_history_magic") != kSuccess ||
      magic == 0 || magic_size != sizeof(uint32_t) ||
      g_module_get_global(&control, &control_size, module,
                          "mfgunlock_v31_history_control") != kSuccess ||
      control == 0 || control_size != sizeof(Control)) {
    state.permanently_local = true;
    SetDetail("Temporal Stable fallback: patched CUDA module symbols not found",
              true);
    return false;
  }
  uint32_t magic_value = 0;
  if (g_memcpy_dtoh(&magic_value, magic, sizeof(magic_value)) != kSuccess ||
      magic_value != kHistoryMagic) {
    state.permanently_local = true;
    SetDetail("Temporal Stable fallback: patched CUDA module magic mismatch",
              true);
    return false;
  }
  state.function = function;
  state.module = module;
  state.context = context;
  state.stream = stream;
  state.control_device = control;
  g_target_function.store(function, std::memory_order_release);
  g_fast_path_ready.store(true, std::memory_order_release);
  return true;
}

inline bool ValidateBoundTarget(State& state, CUfunction function,
                                CUstream stream) {
  CUcontext context = nullptr;
  if (state.function != function ||
      g_ctx_get_current(&context) != kSuccess || context == nullptr ||
      context != state.context || stream != state.stream) {
    const bool context_changed = state.context != nullptr &&
                                 context != nullptr && context != state.context;
    state.permanently_local = true;
    DisableDeviceHistory(
        state,
        "Temporal Stable fallback: CUDA function, context, or stream changed",
        !context_changed);
    return false;
  }
  return true;
}

inline void ResetProbe(State& state, uint32_t width, uint32_t height,
                       uint32_t multiplier) {
  if (state.active) DisableDeviceHistory(state, "Temporal Stable probe reset");
  state.width = width;
  state.height = height;
  state.multiplier = multiplier;
  state.phase_mask = 0;
  state.valid_launches = 0;
  state.last_phase = 0;
  state.completed_cycles = 0;
  state.phase_direction = 0;
  g_probe_launches.store(0, std::memory_order_relaxed);
  g_probe_phase_mask.store(0, std::memory_order_relaxed);
  SetDetail("Temporal Stable read-only phase probe in progress");
}

inline bool ObservePhaseSequence(State& state, const Phase& phase) {
  const uint32_t phase_count = state.multiplier - 1u;
  if (phase_count == 1u) {
    if (state.last_phase != 0) ++state.completed_cycles;
    state.last_phase = phase.index;
    return true;
  }
  if (state.last_phase == 0) {
    state.last_phase = phase.index;
    return true;
  }
  const uint32_t ascending = state.last_phase == phase_count
                                 ? 1u
                                 : state.last_phase + 1u;
  const uint32_t descending = state.last_phase == 1u
                                  ? phase_count
                                  : state.last_phase - 1u;
  if (state.phase_direction == 0) {
    if (phase.index == ascending)
      state.phase_direction = 1;
    else if (phase.index == descending)
      state.phase_direction = -1;
    else
      return false;
  } else {
    const uint32_t expected = state.phase_direction > 0 ? ascending : descending;
    if (phase.index != expected) return false;
  }
  const bool wrapped = state.phase_direction > 0
                           ? state.last_phase == phase_count && phase.index == 1u
                           : state.last_phase == 1u && phase.index == phase_count;
  if (wrapped) ++state.completed_cycles;
  state.last_phase = phase.index;
  return true;
}

inline bool Activate(State& state) {
  const uint64_t required =
      RequiredHistoryBytes(state.width, state.height, state.multiplier);
  if (required == 0 || required > kHistoryLimit) {
    state.permanently_local = true;
    DisableDeviceHistory(state,
                         "Temporal Stable fallback: 64 MiB history limit exceeded");
    return false;
  }
  if (state.history == 0 &&
      g_mem_alloc(&state.history, static_cast<size_t>(kHistoryLimit)) !=
          kSuccess) {
    state.permanently_local = true;
    DisableDeviceHistory(state,
                         "Temporal Stable fallback: CUDA history allocation failed");
    return false;
  }
  if (g_memset_d8_async(state.history, 255, static_cast<size_t>(required),
                        state.stream) != kSuccess) {
    state.permanently_local = true;
    DisableDeviceHistory(state,
                         "Temporal Stable fallback: CUDA history clear failed");
    return false;
  }
  auto control = std::make_unique<Control>();
  control->history = state.history;
  control->pixel_count = state.width * state.height;
  control->width = state.width;
  control->height = state.height;
  control->multiplier = state.multiplier;
  control->enabled = 1;
  if (g_memcpy_htod_async(state.control_device, control.get(), sizeof(Control),
                          state.stream) != kSuccess) {
    state.permanently_local = true;
    DisableDeviceHistory(state,
                         "Temporal Stable fallback: CUDA control upload failed");
    return false;
  }
  state.control_snapshots.push_back(std::move(control));
  state.active = true;
  g_temporal_active.store(true, std::memory_order_release);
  g_history_bytes.store(required, std::memory_order_relaxed);
  g_history_width.store(state.width, std::memory_order_relaxed);
  g_history_height.store(state.height, std::memory_order_relaxed);
  g_history_multiplier.store(state.multiplier, std::memory_order_relaxed);
  std::ostringstream detail;
  detail << "Temporal Stable active: " << state.width << "x" << state.height
         << ", " << state.multiplier << "x, " << required
         << " history bytes, single validated CUDA stream";
  SetDetail(detail.str());
  reshade::log::message(reshade::log::level::info, detail.str().c_str());
  return true;
}

inline void PrepareLaunch(LaunchApi launch_api, CUfunction function,
                          CUstream stream,
                          void** kernel_params, void** extra) {
  const CUfunction target =
      g_target_function.load(std::memory_order_acquire);
  if (target != nullptr && function != target) return;
  if (g_mode.load(std::memory_order_acquire) != StabilityMode::kTemporal ||
      !g_provider_authorized.load(std::memory_order_acquire))
    return;
  State& state = g_state;
#if defined(MFGUNLOCK_CUDA_TEMPORAL_TESTING)
  g_test_lock_acquisitions.fetch_add(1, std::memory_order_relaxed);
#endif
  AcquireSRWLockExclusive(&state.lock);
  if (state.permanently_local) {
    ReleaseSRWLockExclusive(&state.lock);
    return;
  }
  if (target == nullptr) {
    if (!IsTargetFunction(function) ||
        !BindTargetFirstTime(state, function, stream)) {
      ReleaseSRWLockExclusive(&state.lock);
      return;
    }
  } else if (!ValidateBoundTarget(state, function, stream)) {
    ReleaseSRWLockExclusive(&state.lock);
    return;
  }
  if (state.launch_api_seen && state.launch_api != launch_api) {
    state.permanently_local = true;
    DisableDeviceHistory(
        state, "Temporal Stable fallback: multiple CUDA launch APIs observed");
    ReleaseSRWLockExclusive(&state.lock);
    return;
  }
  state.launch_api = launch_api;
  state.launch_api_seen = true;
  const uint8_t* buffer = ParameterBuffer(kernel_params, extra);
  if (buffer == nullptr) {
    state.permanently_local = true;
    DisableDeviceHistory(
        state, "Temporal Stable fallback: unsupported CUDA launch argument layout");
    ReleaseSRWLockExclusive(&state.lock);
    return;
  }
  float t = 0.0f;
  uint32_t width = 0;
  uint32_t height = 0;
  std::memcpy(&t, buffer + offsetof(KernelParameters, t), sizeof(t));
  std::memcpy(&width, buffer + offsetof(KernelParameters, width),
              sizeof(width));
  std::memcpy(&height, buffer + offsetof(KernelParameters, height),
              sizeof(height));
  const uint32_t multiplier = EffectiveMultiplier();
  if (multiplier == 0) {
    if (!state.waiting_multiplier_reported) {
      SetDetail(
          "Temporal Stable read-only probe waiting for effective multiplier");
      state.waiting_multiplier_reported = true;
    }
    ReleaseSRWLockExclusive(&state.lock);
    return;
  }
  state.waiting_multiplier_reported = false;
  const Phase phase = ResolvePhase(t, multiplier);
  if (!phase.valid || width == 0 || height == 0 ||
      width > 16384 || height > 16384) {
    state.permanently_local = true;
    DisableDeviceHistory(
        state, "Temporal Stable fallback: invalid phase or dimensions observed");
    ReleaseSRWLockExclusive(&state.lock);
    return;
  }
  if (state.width != width || state.height != height ||
      state.multiplier != multiplier) {
    ResetProbe(state, width, height, multiplier);
  }
  if (!ObservePhaseSequence(state, phase)) {
    state.permanently_local = true;
    DisableDeviceHistory(
        state, "Temporal Stable fallback: CUDA phase sequence is ambiguous");
    ReleaseSRWLockExclusive(&state.lock);
    return;
  }
  state.phase_mask |= 1u << phase.index;
  ++state.valid_launches;
  g_probe_launches.store(state.valid_launches, std::memory_order_relaxed);
  g_probe_phase_mask.store(state.phase_mask, std::memory_order_relaxed);
  const uint32_t expected_mask = ((1u << multiplier) - 1u) & ~1u;
  if (!state.active && state.phase_mask == expected_mask &&
      state.completed_cycles >= 2u) {
    Activate(state);
  }
  ReleaseSRWLockExclusive(&state.lock);
}

inline CUresult WINAPI HookedLaunch(
    CUfunction function, unsigned int gx, unsigned int gy, unsigned int gz,
    unsigned int bx, unsigned int by, unsigned int bz, unsigned int shared,
    CUstream stream, void** params, void** extra) {
  PrepareLaunch(LaunchApi::kKernel, function, stream, params, extra);
  return g_real_launch(function, gx, gy, gz, bx, by, bz, shared, stream,
                       params, extra);
}

inline CUresult WINAPI HookedLaunchPtsz(
    CUfunction function, unsigned int gx, unsigned int gy, unsigned int gz,
    unsigned int bx, unsigned int by, unsigned int bz, unsigned int shared,
    CUstream stream, void** params, void** extra) {
  PrepareLaunch(LaunchApi::kKernelPtsz, function, stream, params, extra);
  return g_real_launch_ptsz(function, gx, gy, gz, bx, by, bz, shared, stream,
                            params, extra);
}

inline CUresult WINAPI HookedLaunchEx(const CUlaunchConfig* config,
                                      CUfunction function, void** params,
                                      void** extra) {
  PrepareLaunch(LaunchApi::kKernelEx, function,
                config != nullptr ? config->stream : nullptr, params, extra);
  return g_real_launch_ex(config, function, params, extra);
}

inline CUresult WINAPI HookedLaunchExPtsz(const CUlaunchConfig* config,
                                          CUfunction function, void** params,
                                          void** extra) {
  PrepareLaunch(LaunchApi::kKernelExPtsz, function,
                config != nullptr ? config->stream : nullptr, params, extra);
  return g_real_launch_ex_ptsz(config, function, params, extra);
}

}  // namespace internal

inline void TryInstall() {
  if (g_hooked.load(std::memory_order_acquire) ||
      g_install_failed.load(std::memory_order_acquire) ||
      !g_provider_authorized.load(std::memory_order_acquire) ||
      g_mode.load(std::memory_order_acquire) != StabilityMode::kTemporal)
    return;
  bool expected = false;
  if (!g_installing.compare_exchange_strong(expected, true,
                                             std::memory_order_acq_rel))
    return;
  const auto finish = []() {
    g_installing.store(false, std::memory_order_release);
  };
  HMODULE cuda = GetModuleHandleW(L"nvcuda.dll");
  if (cuda == nullptr) {
    // Some Streamline integrations delay-load the CUDA Driver until after the
    // provider cubin has already been installed. Waiting passively in that
    // case creates a deadlock: no launch can be observed until the hook is
    // installed, and no hook can be installed until the driver is loaded.
    // Exact-provider authorization and Temporal mode have both been checked
    // above, so acquire the genuine system driver here, outside loader lock.
    cuda = LoadLibraryExW(L"nvcuda.dll", nullptr,
                          LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (cuda == nullptr) {
      std::ostringstream detail;
      detail << "Temporal Stable fallback: system nvcuda.dll load failed (Win32 "
             << GetLastError() << ")";
      SetDetail(detail.str(), true);
      g_install_failed.store(true, std::memory_order_release);
      finish();
      return;
    }
    g_cuda_reference.store(cuda, std::memory_order_release);
  }
  if (!internal::ResolveDriverFunctions(cuda)) {
    SetDetail("Temporal Stable fallback: required CUDA Driver APIs are absent",
              true);
    g_install_failed.store(true, std::memory_order_release);
    finish();
    return;
  }
  internal::g_hooks.clear();
  const auto add = [&](const char* name, void** real, void* replacement) {
    if (GetProcAddress(cuda, name) != nullptr)
      internal::g_hooks.emplace_back(name, real, replacement);
  };
  add("cuLaunchKernel", reinterpret_cast<void**>(&internal::g_real_launch),
      reinterpret_cast<void*>(&internal::HookedLaunch));
  add("cuLaunchKernel_ptsz",
      reinterpret_cast<void**>(&internal::g_real_launch_ptsz),
      reinterpret_cast<void*>(&internal::HookedLaunchPtsz));
  add("cuLaunchKernelEx", reinterpret_cast<void**>(&internal::g_real_launch_ex),
      reinterpret_cast<void*>(&internal::HookedLaunchEx));
  add("cuLaunchKernelEx_ptsz",
      reinterpret_cast<void**>(&internal::g_real_launch_ex_ptsz),
      reinterpret_cast<void*>(&internal::HookedLaunchExPtsz));
  if (internal::g_hooks.size() != 4u ||
      !hook::Install(cuda, internal::g_hooks, "nvcuda.dll")) {
    SetDetail("Temporal Stable fallback: CUDA launch hooks were not installed",
              true);
    g_install_failed.store(true, std::memory_order_release);
    finish();
    return;
  }
  g_hooked.store(true, std::memory_order_release);
  SetDetail("Temporal Stable CUDA hooks installed; read-only phase probe pending");
  finish();
}

inline void Uninstall() {
  const bool hooked = g_hooked.exchange(false, std::memory_order_acq_rel);
  if (hooked) hook::Uninstall(internal::g_hooks);
  // Device allocations are context-owned and intentionally left for CUDA
  // context teardown. Freeing while provider worker streams may still execute
  // would be less safe than bounded process-lifetime retention.
  g_temporal_active.store(false, std::memory_order_release);
  g_fast_path_ready.store(false, std::memory_order_release);
  internal::g_target_function.store(nullptr, std::memory_order_release);
  if (HMODULE cuda = g_cuda_reference.exchange(nullptr,
                                                std::memory_order_acq_rel);
      cuda != nullptr)
    FreeLibrary(cuda);
}

}  // namespace mfgunlock::cudatemporal
