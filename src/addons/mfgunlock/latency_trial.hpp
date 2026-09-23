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

// Tests a lower addon-forced multiplier before reducing real/source FPS. The
// saved setting is never overwritten; returning zero restores it on the next
// normal game SetOptions call. No Streamline function is called from Present.
struct MultiplierTrial {
  uint64_t epoch = 0, started = 0, cooldown_until = 0;
  uint32_t override_multiplier = 0, candidate = 0, stable = 0;
  uint32_t configured = 0, baseline_interval = 0, baseline_queue = 0;
  uint32_t baseline_pipeline = 0, baseline_ai = 0;
  bool accepted = false, attempted = false;

  void Stop(uint64_t now) {
    override_multiplier = candidate = stable = 0;
    accepted = false;
    cooldown_until = now + 30000;
  }

  uint32_t Update(uint64_t now, uint64_t new_epoch, uint32_t configured_value,
                  uint32_t live_multiplier, bool safe, uint32_t recommended,
                  uint32_t interval, uint32_t queue, uint32_t pipeline,
                  uint32_t ai) {
    if (new_epoch != epoch || configured_value != configured) {
      Stop(now);
      epoch = new_epoch;
      configured = configured_value;
      attempted = false;
    }
    if (!safe) {
      if (override_multiplier) Stop(now);
      stable = 0;
      return 0;
    }
    if (override_multiplier) {
      // The game must naturally resubmit SetOptions. If the live provider
      // multiplier never follows, the trial is not actionable and is undone.
      if (!accepted && now - started >= 3000 &&
          live_multiplier != override_multiplier) {
        Stop(now);
        return 0;
      }
      if (interval == 0 ||
          uint64_t(interval) * 100 > uint64_t(baseline_interval) * 108 ||
          uint64_t(interval) * 100 < uint64_t(baseline_interval) * 85 ||
          pipeline > baseline_pipeline + 1000 || ai > baseline_ai + 1000) {
        Stop(now);
        return 0;
      }
      if (!accepted && now - started >= 4000) {
        const bool queue_better =
            uint64_t(queue) * 100 <= uint64_t(baseline_queue) * 80;
        const bool latency_better = pipeline + 250 < baseline_pipeline;
        accepted = queue_better && latency_better;
        if (!accepted) {
          Stop(now);
          return 0;
        }
      }
      if (now - started >= 30000) {
        Stop(now);
        return 0;
      }
      return override_multiplier;
    }
    if (now < cooldown_until || recommended < 2 ||
        recommended >= configured) {
      stable = 0;
      return 0;
    }
    stable = candidate == recommended ? stable + 1 : 1;
    candidate = recommended;
    if (stable >= 4) {
      override_multiplier = candidate;
      attempted = true;
      started = now;
      accepted = false;
      baseline_interval = interval;
      baseline_queue = queue;
      baseline_pipeline = pipeline;
      baseline_ai = ai;
    }
    return override_multiplier;
  }
};
} // namespace mfgunlock::latency
