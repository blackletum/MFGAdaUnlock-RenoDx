// SPDX-License-Identifier: MIT
#include <cstdlib>
#include <iostream>
#include "../src/addons/mfgunlock/nvapi_status.hpp"
#include "../src/addons/mfgunlock/latency_trace.hpp"
#define CHECK(x) do { if (!(x)) { std::cerr << "failed line " << __LINE__ << ": " #x "\n"; return 1; } } while(0)
using namespace mfgunlock;
void Fill(nvapistatus::LatencyResult& r, uint64_t first, uint64_t scale = 1) {
  r = {};
  for (size_t i=0; i<64; ++i) {
    auto& f = r.frames[i]; f.frame_id = first + i;
    const uint64_t t = (1000000 + f.frame_id * 10000) * scale;
    f.input_sample_time = f.simulation_start_time = t;
    f.simulation_end_time = t + 1000*scale;
    f.render_submit_start_time = t+2000*scale; f.render_submit_end_time=t+2500*scale;
    f.present_start_time=t+3000*scale; f.present_end_time=t+3100*scale;
    f.os_render_queue_start_time=t+4000*scale;
    f.gpu_render_start_time=t+4000*scale; f.gpu_render_end_time=t+8000*scale;
    f.gpu_frame_time_us=10000; f.ai_frame_time_us=1000;
  }
}
int main() {
  CHECK(!nvapistatus::NgxFeatureActive(0));
  CHECK(nvapistatus::NgxFeatureActive(
      nvapistatus::kNgxOverrideCreated));
  CHECK(nvapistatus::NgxFeatureActive(
      nvapistatus::kNgxOverrideEvaluate));
  CHECK(nvapistatus::NgxFeatureConfigured(
      nvapistatus::kNgxOverridePreset));
  CHECK(nvapistatus::NgxFeatureConfigured(
      nvapistatus::kNgxOverrideScalingRatio));
  CHECK(!nvapistatus::NgxFeatureConfigured(
      nvapistatus::kNgxOverrideInitialized));
  nvapistatus::LatencyResult r{}; latency::History h{};
  Fill(r,0);
  auto a = latency::Analyze(r.frames,10000000,1000,1,h);
  CHECK(a.timestamp_units == latency::Units::kMicroseconds && !a.source_timing_confident);
  Fill(r,50); a=latency::Analyze(r.frames,10000000,1500,1,h);
  CHECK(a.source_timing_confident && a.source_interval_us==10000 && a.median_queue_wait_us==0);
  CHECK(a.median_pipeline_latency_us==8000 && a.new_frames==50);
  a=latency::Analyze(r.frames,10000000,2000,1,h); CHECK(!a.fresh && !a.source_timing_confident);
  Fill(r,100,10); h={}; a=latency::Analyze(r.frames,10000000,1000,2,h);
  Fill(r,150,10); a=latency::Analyze(r.frames,10000000,1500,2,h);
  CHECK(a.timestamp_units==latency::Units::kQpc && a.source_timing_confident);
  CHECK(a.median_pipeline_latency_us==8000);
  Fill(r,200,7); a=latency::Analyze(r.frames,10000000,2000,2,h);
  CHECK(a.timestamp_units==latency::Units::kUnknown && !a.source_timing_confident);
  Fill(r,250); for(auto& f:r.frames) f.gpu_frame_time_us=2500;
  a=latency::Analyze(r.frames,10000000,2500,2,h); CHECK(!a.source_timing_confident);
  Fill(r,300); r.frames[40].frame_id=1;
  a=latency::Analyze(r.frames,10000000,3000,2,h); CHECK(!a.source_timing_confident);
  Fill(r,350); for(size_t i=0;i<4;++i) r.frames[i].os_render_queue_start_time-=3000;
  a=latency::Analyze(r.frames,10000000,3500,2,h); CHECK(a.median_queue_wait_us==0);
  Fill(r,400); a=latency::Analyze(r.frames,10000000,7000,2,h); CHECK(!a.fresh);
  Fill(r,450); a=latency::Analyze(r.frames,10000000,7500,3,h); CHECK(!a.fresh);
  Fill(r,500); for(auto& f:r.frames) f.os_render_queue_start_time=0;
  a=latency::Analyze(r.frames,10000000,8000,3,h); CHECK(!a.source_timing_confident);

  latency::QueueTrial trial;
  CHECK(trial.Update(0,1,4,true,97,10000,3000,12000)==0);
  for(uint64_t t=30000;t<31500;t+=500) CHECK(trial.Update(t,1,4,true,97,10000,3000,12000)==0);
  CHECK(trial.Update(31500,1,4,true,97,10000,3000,12000)==97);
  CHECK(trial.Update(35500,1,4,true,0,10300,1000,11000)==97);
  CHECK(trial.accepted); // beneficial low queue must not immediately remove its own cap
  CHECK(trial.Update(36000,1,4,true,0,14000,1000,11000)==0); // base-FPS loss rolls back
  CHECK(trial.Update(36500,1,4,true,97,10000,3000,12000)==0); // cooldown
  CHECK(trial.Update(67000,1,4,false,97,10000,3000,12000)==0);
  h = {}; // one hour of synthetic 100-FPS sampling; no real time sleeps
  for (uint64_t i=0; i<7200; ++i) {
    Fill(r,i*50); a=latency::Analyze(r.frames,10000000,500*i,11,h);
    CHECK(a.source_timing_confident == (i != 0));
    CHECK(a.median_queue_wait_us == 0);
  }
  latencytrace::Clear(); latencytrace::recording.store(true);
  for(uint64_t i=0;i<7200;++i) latencytrace::Record({i});
  latencytrace::recording.store(false);
  const auto csv=latencytrace::Csv();
  CHECK(csv.find("\n3600,")!=std::string::npos && csv.find("\n3599,")==std::string::npos);
  CHECK(csv.find("\n7199,")!=std::string::npos && latencytrace::dropped.load()==0);
  std::cout << "latency measurement and trial tests passed\n";
}
