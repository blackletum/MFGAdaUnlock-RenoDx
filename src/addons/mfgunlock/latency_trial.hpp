/* SPDX-License-Identifier: MIT */
#pragma once
#include <cstdint>
namespace mfgunlock::latency {
// Conservative bounded trial, not a replacement for NVIDIA's frame scheduler.
struct QueueTrial {
  uint64_t epoch = 0, started = 0, cooldown_until = 0;
  uint32_t cap = 0, candidate = 0, stable = 0, multiplier = 0;
  uint32_t baseline_interval = 0, baseline_queue = 0, baseline_latency = 0;
  bool accepted = false;
  void Stop(uint64_t now) {
    cap = candidate = stable = 0; accepted = false;
    cooldown_until = now + 30000;
  }
  uint32_t Update(uint64_t now, uint64_t new_epoch, uint32_t multi,
                  bool safe, uint32_t recommended, uint32_t interval,
                  uint32_t queue, uint32_t pipeline) {
    if (new_epoch != epoch || multi != multiplier) {
      Stop(now); epoch = new_epoch; multiplier = multi;
    }
    if (!safe) { if (cap) Stop(now); stable = 0; return 0; }
    if (cap) {
      // Reject throughput collapse, stale workload assumptions, or increased
      // pipeline latency. A controller never stacks trims on its own output.
      if (uint64_t(interval) * 100 > uint64_t(baseline_interval) * 108 ||
          uint64_t(interval) * 100 < uint64_t(baseline_interval) * 85 ||
          pipeline > baseline_latency + 1000) { Stop(now); return 0; }
      if (!accepted && now - started >= 4000) {
        accepted = uint64_t(queue) * 100 <= uint64_t(baseline_queue) * 80 &&
                   pipeline + 250 < baseline_latency;
        if (!accepted) { Stop(now); return 0; }
      }
      // A successful trim is not immediately released just because it worked.
      // Periodic native-baseline probes avoid pinning a cap indefinitely.
      if (now - started >= 30000) { Stop(now); return 0; }
      return cap;
    }
    if (now < cooldown_until || recommended == 0) { stable = 0; return 0; }
    const auto difference = recommended > candidate ? recommended - candidate : candidate - recommended;
    stable = candidate && difference <= 1 ? stable + 1 : 1;
    candidate = recommended;
    if (stable >= 4) {
      cap = candidate; started = now; accepted = false;
      baseline_interval = interval; baseline_queue = queue; baseline_latency = pipeline;
    }
    return cap;
  }
};
} // namespace mfgunlock::latency
