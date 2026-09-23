#include <cstdlib>
#include <iostream>
#include <string>

#include "../src/addons/mfgunlock/thin_geometry.hpp"

#define CHECK(condition)                                                        \
  do {                                                                          \
    if (!(condition)) {                                                         \
      std::cerr << "CHECK failed at line " << __LINE__ << ": " #condition      \
                << '\n';                                                        \
      return EXIT_FAILURE;                                                      \
    }                                                                           \
  } while (false)

int main() {
  using namespace mfgunlock::thingeometry;

  std::string why;
  std::string previous =
      ".entry Kernel_EstimatePrev2CurrScatter(\n"
      "ld.param.f32 %f24, [%rd6+60];\n";
  CHECK(internal::RewritePreviousScatter(previous, why));
  CHECK(previous.find("MFGUNLOCK_PREVIOUS_SCATTER_V1") != std::string::npos);
  CHECK(previous.find("mul.ftz.f32 %f24, %f24, 0f3F000000") != std::string::npos);
  CHECK(!internal::RewritePreviousScatter(previous, why));
  CHECK(why == "previous-scatter PTX is already modified");

  std::string blend =
      ".entry Kernel_BlendCandidatesFused(\n"
      ".reg .pred %p<260>;\n"
      "ld.param.u8 %rs8, [%rd6+220];\n";
  why.clear();
  CHECK(internal::RewriteValidatedWarpBlend(blend, why));
  CHECK(blend.find("MFGUNLOCK_VALIDATED_WARP_BLEND_V1") != std::string::npos);
  CHECK(blend.find(".reg .pred %qv<7>;") != std::string::npos);
  CHECK(blend.find("0f3F59999A") != std::string::npos);
  CHECK(blend.find("ld.param.u8 %rs8, [%rd6+220];") != std::string::npos);
  g_refinement_enabled = true;
  std::string refined = ".entry Kernel_BlendCandidatesFused(\n.reg .pred %p<260>;\nld.param.u8 %rs8, [%rd6+220];\n";
  CHECK(internal::RewriteValidatedWarpBlend(refined, why));
  CHECK(refined.find(mfgunlock::qualityrefinement::kBlendWeights) != std::string::npos);
  g_border_confidence_enabled = true;
  std::string border = ".entry Kernel_BlendCandidatesFused(\n.reg .pred %p<260>;\nld.param.u8 %rs8, [%rd6+220];\n";
  CHECK(internal::RewriteValidatedWarpBlend(border, why));
  CHECK(border.find(mfgunlock::qualityrefinement::kBlendWeights) != std::string::npos);
  CHECK(border.find(mfgunlock::qualityborder::kBorderWeights) != std::string::npos);
  CHECK(border.find("MFGUNLOCK_BORDER_CONFIDENCE_V1") != std::string::npos);
  CHECK(border.find("MFGUNLOCK_BORDER_INTERIOR_FAST_PATH_V1") !=
        std::string::npos);
  CHECK(border.find("sub.f32 %qf6, %qf0, %qf8;") != std::string::npos);
  CHECK(border.find("sub.f32 %qf6, %qf1, %qf10;") != std::string::npos);
  g_border_confidence_enabled = false;
  g_refinement_enabled = false;
  g_adaptive_quality_enabled = true;
  std::string adaptive = ".entry Kernel_BlendCandidatesFused(\n.reg .pred %p<260>;\nld.param.u8 %rs8, [%rd6+220];\n";
  CHECK(internal::RewriteValidatedWarpBlend(adaptive, why));
  CHECK(adaptive.find(mfgunlock::qualityrefinement::kBlendWeights) != std::string::npos);
  CHECK(adaptive.find(mfgunlock::qualityborder::kBorderWeights) != std::string::npos);
  CHECK(adaptive.find(mfgunlock::adaptivequality::kCandidateArbitration) != std::string::npos);
  CHECK(adaptive.find("MFGUNLOCK_CANDIDATE_ARBITRATION_V2") != std::string::npos);
  CHECK(adaptive.find("MFGUNLOCK_AGREEMENT_FAST_PATH_V1") !=
        std::string::npos);
  CHECK(adaptive.find("selp.f32 %qf2, %qf1, %qf0, %qv2;") != std::string::npos);
  CHECK(adaptive.find("selp.f32 %qf3, %qf10, %qf8, %qv2;") != std::string::npos);
  g_adaptive_quality_enabled = false;

  const float unchanged = mfgunlock::adaptivequality::ArbitrateExtraWeight(
      0.2f, 0.85f, 0.04f, 1.0f);
  CHECK(unchanged > 0.849f && unchanged <= 0.85f);
  const float attenuated = mfgunlock::adaptivequality::ArbitrateExtraWeight(
      0.2f, 0.85f, 0.15f, 1.0f);
  CHECK(attenuated >= 0.2f && attenuated < unchanged);

  IMAGE_NT_HEADERS64 headers{};
  headers.FileHeader.TimeDateStamp = 0x6A986031u;
  headers.OptionalHeader.SizeOfImage = 7565312u;
  CHECK(internal::MatchProvider(&headers) != nullptr);
  CHECK(std::string(internal::MatchProvider(&headers)->version) == "310.9.1");
  headers.FileHeader.TimeDateStamp = 0;
  CHECK(internal::MatchProvider(&headers) == nullptr);

  const uint8_t fnv_sample[] = {'h', 'e', 'l', 'l', 'o'};
  CHECK(internal::Fnv1a64(fnv_sample, sizeof(fnv_sample)) ==
        0xa430d84680aabd0bull);

  std::cout << "thin geometry tests passed\n";
  return EXIT_SUCCESS;
}
