/* SPDX-License-Identifier: MIT
 * Pure, bounded Reflex report analysis. No API calls or controller side effects.
 */
#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>

namespace mfgunlock::latency {
enum class Units : uint32_t { kUnknown, kMicroseconds, kQpc };
struct History {
  uint64_t epoch = 0, frame = 0, gpu_end = 0, polled_ms = 0;
  bool seen = false;
};
template <class Frame> struct Report {
  Frame latest{}, previous{};
  uint32_t valid_latency_frames = 0, consecutive_timing_samples = 0;
  uint32_t source_interval_us = 0, simulation_interval_us = 0;
  uint32_t median_queue_wait_us = 0, median_pipeline_latency_us = 0;
  uint32_t p95_queue_wait_us = 0, p95_pipeline_latency_us = 0;
  uint32_t median_input_to_gpu_end_us = 0, median_gpu_frame_time_us = 0;
  uint32_t p95_gpu_frame_time_us = 0, median_gpu_active_us = 0;
  uint32_t median_input_to_simulation_us = 0;
  uint32_t median_simulation_cpu_us = 0, median_submit_cpu_us = 0;
  uint32_t median_ai_frame_time_us = 0, new_frames = 0;
  Units timestamp_units = Units::kUnknown;
  uint64_t qpc_frequency = 0;
  bool fresh = false, source_timing_confident = false;
};
inline uint64_t ToUs(uint64_t value, Units units, uint64_t frequency) {
  if (units == Units::kMicroseconds) return value;
  if (units != Units::kQpc || frequency == 0 ||
      value > UINT64_MAX / 1000000ull) return 0;
  return value * 1000000ull / frequency;
}
inline bool Near(uint64_t a, uint64_t b, uint64_t percent = 2) {
  const auto delta = a > b ? a - b : b - a;
  return b != 0 && delta <= (std::max)(uint64_t{5}, b * percent / 100);
}
template <class Frame> bool Complete(const Frame& f) {
  return f.simulation_start_time && f.simulation_end_time >= f.simulation_start_time &&
      f.render_submit_start_time && f.render_submit_end_time >= f.render_submit_start_time &&
      f.present_start_time && f.present_end_time >= f.present_start_time &&
      f.gpu_render_start_time && f.gpu_render_end_time >= f.gpu_render_start_time &&
      f.gpu_render_end_time >= f.simulation_start_time;
}
template <size_t N> struct Samples {
  std::array<uint32_t, N> values{};
  size_t count = 0;
  void Add(uint64_t v) { if (v <= UINT32_MAX && count < N) values[count++] = uint32_t(v); }
  void Sort() { std::sort(values.begin(), values.begin() + count); }
  uint32_t P(size_t numerator, size_t denominator = 100) const {
    return count ? values[(count - 1) * numerator / denominator] : 0;
  }
};
template <class Frame, size_t N>
Report<Frame> Analyze(const Frame (&frames)[N], uint64_t frequency,
                      uint64_t now_ms, uint64_t epoch, History& history) {
  Report<Frame> out{};
  out.qpc_frequency = frequency;
  size_t pairs = 0, direct_matches = 0, qpc_matches = 0;
  for (size_t i = 1; i < N; ++i) {
    const auto& a = frames[i-1]; const auto& b = frames[i];
    if (!Complete(a) || !Complete(b) || a.frame_id == UINT64_MAX ||
        b.frame_id != a.frame_id + 1 || b.gpu_render_end_time <= a.gpu_render_end_time ||
        b.gpu_frame_time_us < 1000 || b.gpu_frame_time_us > 100000) continue;
    ++pairs;
    const auto delta = b.gpu_render_end_time - a.gpu_render_end_time;
    direct_matches += Near(delta, b.gpu_frame_time_us);
    qpc_matches += Near(ToUs(delta, Units::kQpc, frequency), b.gpu_frame_time_us);
  }
  const bool direct = pairs >= 48 && direct_matches * 100 >= pairs * 95;
  const bool qpc = pairs >= 48 && qpc_matches * 100 >= pairs * 95;
  if (direct && (!qpc || frequency == 1000000)) out.timestamp_units = Units::kMicroseconds;
  else if (qpc && !direct) out.timestamp_units = Units::kQpc;

  Samples<N> simulation, present, gpu, gpu_active, queue, pipeline, input,
      input_to_simulation, simulation_cpu, submit_cpu, ai;
  uint32_t run = 0;
  auto duration = [&](uint64_t a, uint64_t b, auto& samples) {
    // Zero duration is valid; missing endpoints are not.
    if (a && b && b >= a) {
      const auto us = ToUs(b - a, out.timestamp_units, frequency);
      if (out.timestamp_units != Units::kUnknown && us <= 1000000) samples.Add(us);
    }
  };
  for (size_t i = 0; i < N; ++i) {
    const auto& f = frames[i];
    if (!Complete(f)) { run = 0; continue; }
    ++out.valid_latency_frames;
    out.previous = out.latest; out.latest = f;
    if (i && Complete(frames[i-1]) && frames[i-1].frame_id != UINT64_MAX &&
        f.frame_id == frames[i-1].frame_id + 1 &&
        f.simulation_start_time > frames[i-1].simulation_start_time &&
        f.present_start_time > frames[i-1].present_start_time &&
        f.gpu_render_end_time > frames[i-1].gpu_render_end_time) {
      ++run;
      duration(frames[i-1].simulation_start_time, f.simulation_start_time, simulation);
      duration(frames[i-1].present_start_time, f.present_start_time, present);
    } else run = 0;
    if (f.gpu_frame_time_us >= 1000 && f.gpu_frame_time_us <= 100000) gpu.Add(f.gpu_frame_time_us);
    if (f.gpu_active_render_time_us && f.gpu_active_render_time_us <= 1000000)
      gpu_active.Add(f.gpu_active_render_time_us);
    duration(f.os_render_queue_start_time, f.gpu_render_start_time, queue);
    duration(f.simulation_start_time, f.gpu_render_end_time, pipeline);
    duration(f.input_sample_time, f.gpu_render_end_time, input);
    duration(f.input_sample_time, f.simulation_start_time, input_to_simulation);
    duration(f.simulation_start_time, f.simulation_end_time, simulation_cpu);
    duration(f.render_submit_start_time, f.render_submit_end_time, submit_cpu);
    if (f.ai_frame_time_us) ai.Add(f.ai_frame_time_us);
  }
  simulation.Sort(); present.Sort(); gpu.Sort(); gpu_active.Sort(); queue.Sort();
  pipeline.Sort(); input.Sort(); input_to_simulation.Sort();
  simulation_cpu.Sort(); submit_cpu.Sort(); ai.Sort();
  out.consecutive_timing_samples = run; // contiguous suffix, not sum across gaps
  out.simulation_interval_us = simulation.P(50);
  out.median_gpu_frame_time_us = gpu.P(50);
  out.p95_gpu_frame_time_us = gpu.P(95);
  out.median_gpu_active_us = gpu_active.P(50);
  out.median_queue_wait_us = queue.P(50);
  out.p95_queue_wait_us = queue.P(95);
  out.median_pipeline_latency_us = pipeline.P(50);
  out.p95_pipeline_latency_us = pipeline.P(95);
  out.median_input_to_gpu_end_us = input.P(50);
  out.median_input_to_simulation_us = input_to_simulation.P(50);
  out.median_simulation_cpu_us = simulation_cpu.P(50);
  out.median_submit_cpu_us = submit_cpu.P(50);
  out.median_ai_frame_time_us = ai.P(50);
  if (history.seen && history.epoch == epoch && now_ms >= history.polled_ms &&
      now_ms - history.polled_ms <= 2000 && out.latest.frame_id > history.frame &&
      out.latest.gpu_render_end_time > history.gpu_end) {
    out.new_frames = uint32_t((std::min)(out.latest.frame_id - history.frame, uint64_t{N}));
    out.fresh = out.new_frames >= 8;
  }
  history = {epoch, out.latest.frame_id, out.latest.gpu_render_end_time, now_ms, true};
  // GPU completion cadence alone is not a source-FPS estimate. Require two
  // application marker cadences to corroborate it before publishing a cap input.
  out.source_timing_confident = out.fresh && out.timestamp_units != Units::kUnknown &&
      run >= 48 && gpu.count >= 48 && gpu.count == out.valid_latency_frames &&
      queue.count == out.valid_latency_frames &&
      out.simulation_interval_us >= 1000 && out.simulation_interval_us <= 100000 &&
      Near(present.P(50), out.simulation_interval_us, 20) &&
      Near(gpu.P(50), out.simulation_interval_us, 20) &&
      gpu.P(90) <= uint64_t(gpu.P(50)) * 3 / 2;
  if (out.source_timing_confident) out.source_interval_us = out.simulation_interval_us;
  return out;
}
} // namespace mfgunlock::latency
