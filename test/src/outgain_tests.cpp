// Pins issue #99: Out Gain must apply to the combined dry+wet signal, after
// the mix blend, not to the wet term alone before it (see the tail loop in
// Processor.cpp's processChainOnBuffer). At mix < 100%, a wet-only gain
// leaves the dry share untouched, so pulling the knob down barely moves the
// block's actual output.
#include "chain_test_helpers.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace {

// Mono chain (not seedStereoChains's stereo + empty-right-lane setup): with
// stereo mode on and no blocks in the right lane, that lane is the raw,
// unprocessed input, and Balance/Pan blends it back into the final output -
// masking exactly the block-level gain change this test measures. Mono mode
// runs only the Left chain (no pan stage in the way), so the final output is
// this block's own tail loop, nothing else.
void seedMonoChain(ChainTestProcessor& proc, const juce::String& blockId) {
  juce::ValueTree state("ChainSnapshot");
  state.setProperty("stereoEnabled", false, nullptr);
  juce::ValueTree lane("ChainBlocks");
  lane.appendChild(makeIrBlockTree(blockId, 1, 100), nullptr);
  state.appendChild(lane, nullptr);
  state.appendChild(juce::ValueTree("RightChainBlocks"), nullptr);
  proc.restoreFromTree(state);
}

constexpr int kBlock = 512;
// mixSmoother/outputGainSmoother ramp over 50ms, wetFadeGain's load-in fade
// over 25ms (see ChainBlock.h's kWetFadeSeconds); 15 blocks @ 512/48k is the
// same margin predelay_tests.cpp already proved sufficient for both.
constexpr int kWarmupBlocks = 15;
constexpr int kRunBlocks = 20;

// A single IR block, driven with the given mix/outputGain (both normalized
// 0..1) already applied and settled, returns the following kRunBlocks worth
// of noise output. Everything upstream of the block's own mix/gain tail
// (input gate, convolution, IR normalization) is identical run to run since
// none of it reads mix or outputGain - only the noise seed distinguishes the
// discarded warmup from the measured window, so two calls with the same
// `mix`/`outputGain` reproduce the same dry/wet components bit-for-bit.
std::pair<std::vector<float>, std::vector<float>> runWithMixAndGain(double mix,
                                                                     double outputGainNormalized) {
  ChainTestProcessor proc;
  proc.setPlayConfigDetails(2, 2, kFs, kBlock);
  proc.prepareToPlay(kFs, kBlock);

  seedMonoChain(proc, "blk-a");
  EXPECT_TRUE(waitForChainLoaded(proc)) << "IR block never finished loading from cache";

  EXPECT_TRUE(proc.setBlockParam("blk-a", "mix", mix));
  EXPECT_TRUE(proc.setBlockParam("blk-a", "outputGain", outputGainNormalized));
  letAudioGoIdle();

  processStereo(proc, makeNoise(kWarmupBlocks * kBlock, 1111, 0.25f));  // discard: settle smoothers
  return processStereo(proc, makeNoise(kRunBlocks * kBlock, 4242, 0.25f));
}

float maxAbsDiff(const std::vector<float>& a, const std::vector<float>& b, size_t skip) {
  float m = 0.0f;
  for (size_t i = skip; i < a.size(); ++i) m = std::max(m, std::abs(a[i] - b[i]));
  return m;
}

constexpr size_t kSkip = 0;  // no cross-run smoother drift here; see comment above
constexpr double kUnityNorm = 0.5;    // 0 dB relative (gainDb formula's center)
constexpr double kReducedNorm = 0.0;  // -24 dB relative: (0.0 - 0.5) * 48
constexpr double kMix = 0.5;
// Ratio between the two outputGain settings' linear gain. Derived from the
// documented normalized->dB mapping alone (0.5 span == 48 dB), not from the
// short-IR cab pad offset, so it holds regardless of that constant.
const double kGainRatio = std::pow(10.0, ((kReducedNorm - kUnityNorm) * 48.0) / 20.0);
}  // namespace

// At mix=0.5, pulling Out Gain down by 24 dB must measurably attenuate the
// combined dry+wet output by (close to) the full 24 dB - not just the wet
// half, which a wet-only gain would leave at roughly half that drop since
// the untouched dry share still carries half the signal's energy.
TEST(OutGainTest, OutGainAffectsFullSignal) {
  const auto [unityL, unityR] = runWithMixAndGain(kMix, kUnityNorm);
  const auto [reducedL, reducedR] = runWithMixAndGain(kMix, kReducedNorm);

  auto rms = [](const std::vector<float>& l, const std::vector<float>& r) {
    double sumSq = 0.0;
    for (float v : l) sumSq += static_cast<double>(v) * v;
    for (float v : r) sumSq += static_cast<double>(v) * v;
    return std::sqrt(sumSq / (l.size() + r.size()));
  };

  const double rmsUnity = rms(unityL, unityR);
  const double rmsReduced = rms(reducedL, reducedR);
  ASSERT_GT(rmsUnity, 0.0) << "unity-gain run produced silence - fixture is broken";
  ASSERT_GT(rmsReduced, 0.0) << "reduced-gain run produced silence";

  const double measuredDropDb = 20.0 * std::log10(rmsUnity / rmsReduced);
  std::printf("[OutGainTest] measured drop at mix=0.5, outputGain -24dB: %.2f dB\n", measuredDropDb);

  // A wet-only gain would attenuate roughly half the signal's energy (the
  // wet share), landing well under half the requested 24 dB; the fixed,
  // post-mix gain scales the whole combined signal, landing close to it.
  EXPECT_NEAR(measuredDropDb, 24.0, 3.0)
      << "Out Gain isn't attenuating the full combined signal - looks wet-only again (issue #99)";
}

// Same two reference points, reconstructed sample-by-sample instead of via
// RMS: isolate the block's dry component (mix=0) and wet component (mix=1),
// each captured at the same "unity" outputGain so they combine cleanly, then
// verify the actual mix=0.5/reduced-gain run matches the post-mix formula
// and NOT the old wet-only one - i.e. this test would have failed against
// the previous (wet-only) implementation.
TEST(OutGainTest, OutGainMatchesPostMixFormulaNotWetOnlyFormula) {
  const auto [dryL, dryR] = runWithMixAndGain(0.0, kUnityNorm);  // mix=0 -> pure dry * unityGain
  const auto [wetL, wetR] = runWithMixAndGain(1.0, kUnityNorm);  // mix=1 -> pure wet * unityGain
  const auto [actualL, actualR] = runWithMixAndGain(kMix, kReducedNorm);

  ASSERT_EQ(dryL.size(), wetL.size());
  ASSERT_EQ(dryL.size(), actualL.size());

  auto buildExpected = [](const std::vector<float>& dry, const std::vector<float>& wet,
                          bool postMixGain) {
    std::vector<float> out(dry.size());
    for (size_t i = 0; i < dry.size(); ++i) {
      const float d = dry[i], w = wet[i];
      out[i] = postMixGain
                   // fixed: gain applies to the combined signal
                   ? static_cast<float>((d * (1.0 - kMix) + w * kMix) * kGainRatio)
                   // broken: gain applies to the wet term alone, before mixing
                   : static_cast<float>(d * (1.0 - kMix) + (w * kGainRatio) * kMix);
    }
    return out;
  };

  const auto expectedFixedL = buildExpected(dryL, wetL, true);
  const auto expectedFixedR = buildExpected(dryR, wetR, true);
  const auto expectedBrokenL = buildExpected(dryL, wetL, false);
  const auto expectedBrokenR = buildExpected(dryR, wetR, false);

  const float diffFromFixed =
      std::max(maxAbsDiff(actualL, expectedFixedL, kSkip), maxAbsDiff(actualR, expectedFixedR, kSkip));
  const float diffFromBroken = std::max(maxAbsDiff(actualL, expectedBrokenL, kSkip),
                                        maxAbsDiff(actualR, expectedBrokenR, kSkip));
  std::printf("[OutGainTest] actual vs post-mix formula: max|diff|=%.9f; vs wet-only formula: max|diff|=%.9f\n",
              static_cast<double>(diffFromFixed), static_cast<double>(diffFromBroken));

  // Everything upstream of the mix/gain tail is identical across these three
  // runs (same input, same gate/convolution/normalization state each time),
  // so matching the correct formula should be tight - floating-point
  // rounding noise, not a real divergence.
  EXPECT_LT(diffFromFixed, 1e-4f)
      << "current output doesn't match the post-mix Out Gain formula from issue #99";
  // The wet-only formula predicts a substantially different signal (it
  // leaves the dry share at unity while this run's dry share should be
  // attenuated too); a real gap here, not rounding noise, is what proves
  // this test would have caught the original bug.
  EXPECT_GT(diffFromBroken, 5e-3f)
      << "current output matches the old wet-only formula - Out Gain regressed to pre-mix (issue #99)";
}
