// Auto-align probe tests
//
// The probe measurement (AutoOffset.h): the state machine must fade, sweep,
// capture and unmute on its fixed schedule, and the soft-PHAT estimator must
// recover integer and fractional inter-chain lags through gain/voicing
// differences and polarity inversion, rejecting a lag beyond what the
// Offset knob can express. The last test closes the loop through the real
// StereoOffset engine. AutoOffset/StereoOffset are shared with Dual Mono's
// own per-block probe (armDualAutoAlign/pollDualAutoAlign) - see
// dual_mono_block_tests.cpp for that call site's coverage.
#include "AutoOffset.h"
#include "StereoOffset.h"
#include "test_helpers.h"

#include <gtest/gtest.h>
#include <juce_audio_basics/juce_audio_basics.h>

#include <cmath>
#include <vector>

namespace {

constexpr int kBlock = 512;
constexpr float kMsPerSample = 1000.0f / static_cast<float>(kFs);

// Unit impulse at `delay` samples, scaled by `gain`: a pure-delay "chain".
std::vector<float> impulseFir(int delay, float gain = 1.0f) {
  std::vector<float> h(static_cast<size_t>(delay) + 1, 0.0f);
  h.back() = gain;
  return h;
}

// Fractional delay: Blackman-windowed sinc centered at `delay`. The kernel
// is symmetric about `delay`, so its phase is exactly linear and the group
// delay exact at every frequency; only the magnitude ripples near Nyquist,
// which cannot move a correlation peak.
std::vector<float> fractionalDelayFir(double delay, int halfWidth = 32) {
  const int lo = static_cast<int>(std::floor(delay)) - halfWidth;
  const int hi = static_cast<int>(std::ceil(delay)) + halfWidth;
  EXPECT_GE(lo, 0) << "kernel would need negative taps";
  std::vector<float> h(static_cast<size_t>(hi) + 1, 0.0f);
  for (int k = lo; k <= hi; ++k) {
    const double x = k - delay;
    const double sinc = x == 0.0 ? 1.0 : std::sin(kPi * x) / (kPi * x);
    const double t = x / (halfWidth + 1);
    const double w = 0.42 + 0.5 * std::cos(kPi * t) + 0.08 * std::cos(2.0 * kPi * t);
    h[static_cast<size_t>(k)] = static_cast<float>(sinc * w);
  }
  return h;
}

// One-pole lowpass as a truncated FIR, a stand-in for "differently voiced
// chain" (decayed far below float noise by 256 taps at fc = 1 kHz).
std::vector<float> onePoleLpFir(double fc, int taps = 256) {
  const double a = std::exp(-2.0 * kPi * fc / kFs);
  std::vector<float> h(static_cast<size_t>(taps));
  double c = 1.0 - a;
  for (auto& tap : h) {
    tap = static_cast<float>(c);
    c *= a;
  }
  return h;
}

// h shifted right by `delay` samples (h convolved with a delayed impulse).
std::vector<float> delayedFir(const std::vector<float>& h, int delay) {
  std::vector<float> out(static_cast<size_t>(delay), 0.0f);
  out.insert(out.end(), h.begin(), h.end());
  return out;
}

std::vector<float> scaledFir(std::vector<float> h, float gain) {
  for (auto& tap : h)
    tap *= gain;
  return h;
}

// Runs the full probe schedule against a synthetic chain pair, each an FIR
// applied to the probe stream (renderProbeInput / captureChainOutputs /
// applyOutputGain per block, like processBlock does), and returns the
// analysis.
AutoOffset::Result measure(const std::vector<float>& firL, const std::vector<float>& firR) {
  AutoOffset ao;
  ao.prepare(kFs);
  ao.arm();

  std::vector<float> probe;
  std::vector<float> block(kBlock), outL(kBlock), outR(kBlock);
  juce::AudioBuffer<float> host(2, kBlock);

  for (int guard = 0; guard < 200 && ao.state() != AutoOffset::State::Captured; ++guard) {
    if (ao.renderProbeInput(block.data(), kBlock)) {
      const size_t base = probe.size();
      probe.insert(probe.end(), block.begin(), block.end());
      auto fir = [&](const std::vector<float>& h, size_t n) {
        double acc = 0.0;
        const size_t kMax = std::min(h.size() - 1, n);
        for (size_t k = 0; k <= kMax; ++k)
          acc += static_cast<double>(h[k]) * probe[n - k];
        return static_cast<float>(acc);
      };
      for (int i = 0; i < kBlock; ++i) {
        outL[static_cast<size_t>(i)] = fir(firL, base + static_cast<size_t>(i));
        outR[static_cast<size_t>(i)] = fir(firR, base + static_cast<size_t>(i));
      }
      ao.captureChainOutputs(outL.data(), outR.data(), kBlock);
    }
    host.clear();
    ao.applyOutputGain(host);  // advances FadeOut -> Probing and the mute
  }

  EXPECT_EQ(ao.state(), AutoOffset::State::Captured) << "schedule never completed";
  return ao.analyze();
}

TEST(AutoOffsetTest, MeasuresIntegerDelaysBothDirections) {
  // Left chain lags by 150 samples: delay the RIGHT chain, positive ms in
  // the StereoOffset convention. Sub-sample estimator, so the tolerance is
  // a twentieth of a sample.
  {
    const auto result = measure(impulseFir(150), impulseFir(0));
    EXPECT_NEAR(result.offsetMs, 150.0f * kMsPerSample, 0.05f * kMsPerSample);
    EXPECT_FALSE(result.inverted);
    EXPECT_GT(result.confidence, 0.9f);
    EXPECT_GT(result.peakSharpness, 3.0f);
  }
  // Right chain lags by 112: delay the LEFT chain, negative ms.
  {
    const auto result = measure(impulseFir(0), impulseFir(112));
    EXPECT_NEAR(result.offsetMs, -112.0f * kMsPerSample, 0.05f * kMsPerSample);
    EXPECT_FALSE(result.inverted);
    EXPECT_GT(result.confidence, 0.9f);
  }
}

TEST(AutoOffsetTest, RecoversFractionalDelayAndRepeatsExactly) {
  // A 37.36-sample lag: the band-limited sub-sample refinement must land
  // within a twentieth of a sample, and the deterministic schedule must
  // produce the identical answer on a second run.
  const auto firL = fractionalDelayFir(37.36);
  const auto firR = impulseFir(0);

  const auto first = measure(firL, firR);
  EXPECT_NEAR(first.offsetMs, 37.36f * kMsPerSample, 0.05f * kMsPerSample);
  EXPECT_GT(first.confidence, 0.9f);

  const auto second = measure(firL, firR);
  EXPECT_EQ(first.offsetMs, second.offsetMs);
}

TEST(AutoOffsetTest, GainAndVoicingMismatchDoNotMoveTheEstimate) {
  // One chain bright, the other 1 kHz-lowpassed and lagging 200 samples,
  // then the same pair with a 48 dB relative level mismatch. PHAT weighting
  // normalizes each bin's magnitude, so the level must not move the
  // estimate at all; the lowpass may contribute a couple of samples of its
  // own group delay (that is real, measurable delay).
  const auto voiced = delayedFir(onePoleLpFir(1000.0), 200);
  const auto flat = impulseFir(0);

  const auto even = measure(voiced, flat);
  EXPECT_NEAR(even.offsetMs, 200.0f * kMsPerSample, 0.2f);
  EXPECT_GT(even.peakSharpness, 3.0f);

  const auto mismatched = measure(scaledFir(voiced, 16.0f), scaledFir(flat, 1.0f / 16.0f));
  EXPECT_NEAR(mismatched.offsetMs, even.offsetMs, 1.0e-4f);
}

TEST(AutoOffsetTest, MeasuresInvertedChains) {
  // Captures don't share a polarity convention, so one chain can arrive
  // 180° out. The magnitude peak search must still land on the true lag and
  // report the inversion at full confidence.
  {
    const auto result = measure(impulseFir(0), impulseFir(150, -1.0f));
    EXPECT_NEAR(result.offsetMs, -150.0f * kMsPerSample, 0.05f * kMsPerSample);
    EXPECT_TRUE(result.inverted);
    EXPECT_GT(result.confidence, 0.9f);
  }
  // Perfectly aligned but inverted: zero offset, inversion reported.
  {
    const auto result = measure(impulseFir(0), impulseFir(0, -1.0f));
    EXPECT_NEAR(result.offsetMs, 0.0f, 0.05f * kMsPerSample);
    EXPECT_TRUE(result.inverted);
    EXPECT_GT(result.confidence, 0.9f);
  }
}

TEST(AutoOffsetTest, RejectsWhenTheLagIsBeyondTheKnobRange) {
  // 40 ms of true lag sits outside the ±24 ms search window, so the best
  // in-window peak is junk; the sharpness gate the processor checks must
  // reject it.
  const int lag = static_cast<int>(std::round(0.040 * kFs));
  const auto result = measure(impulseFir(lag), impulseFir(0));

  EXPECT_LT(result.peakSharpness, 2.0f) << "confidence " << result.confidence;
}

TEST(AutoOffsetTest, OutputMutesOnScheduleAndRampsBackAfterResume) {
  AutoOffset ao;
  ao.prepare(kFs);

  juce::AudioBuffer<float> ones(2, kBlock);
  auto refill = [&] {
    for (int ch = 0; ch < 2; ++ch)
      juce::FloatVectorOperations::fill(ones.getWritePointer(ch), 1.0f, kBlock);
  };

  // Idle: untouched. No probe before the fade lands either.
  refill();
  ao.applyOutputGain(ones);
  EXPECT_EQ(ones.getSample(0, kBlock - 1), 1.0f);

  ao.arm();
  std::vector<float> probeBlock(kBlock);
  EXPECT_FALSE(ao.renderProbeInput(probeBlock.data(), kBlock)) << "probe must wait for the fade";

  // FadeOut: 5 ms at 48 kHz lands inside one 512-sample block; the block
  // ends silent and the probe owns the next one.
  refill();
  ao.applyOutputGain(ones);
  EXPECT_EQ(ones.getSample(0, kBlock - 1), 0.0f);
  EXPECT_EQ(ao.state(), AutoOffset::State::Probing);

  // Probing/Tail: hard mute, deterministic progress to Captured.
  while (ao.state() == AutoOffset::State::Probing || ao.state() == AutoOffset::State::Tail) {
    ASSERT_TRUE(ao.renderProbeInput(probeBlock.data(), kBlock));
    ao.captureChainOutputs(probeBlock.data(), probeBlock.data(), kBlock);
    refill();
    ao.applyOutputGain(ones);
    EXPECT_EQ(ones.getSample(1, kBlock - 1), 0.0f);
  }
  EXPECT_EQ(ao.state(), AutoOffset::State::Captured);
  EXPECT_EQ(ao.progress(), 1.0f);

  // Captured/Analyzing: live signal may run the chains again (probe is
  // done) but the output stays muted until resume().
  EXPECT_FALSE(ao.renderProbeInput(probeBlock.data(), kBlock));
  const auto result = ao.analyze();
  EXPECT_GT(result.confidence, 0.99f);  // captured itself on both channels
  refill();
  ao.applyOutputGain(ones);
  EXPECT_EQ(ones.getSample(0, kBlock - 1), 0.0f);
  EXPECT_EQ(ao.state(), AutoOffset::State::Analyzing);

  // RampBack: 10 ms lands inside one block; back to Idle, gain restored.
  ao.resume();
  refill();
  ao.applyOutputGain(ones);
  EXPECT_EQ(ones.getSample(0, kBlock - 1), 1.0f);
  EXPECT_EQ(ao.state(), AutoOffset::State::Idle);
}

TEST(AutoOffsetTest, EndToEndMeasureThenAlignThroughStereoOffset) {
  // The full loop, exactly as pollAutoOffset applies it: measure a
  // 300-sample left-chain lag, map the result onto the knob's normalized
  // value, run a misaligned noise pair through the real StereoOffset; the
  // residual lag between the outputs must be zero.
  constexpr int kLag = 300;
  const auto result = measure(impulseFir(kLag), impulseFir(0));
  ASSERT_GT(result.confidence, 0.9f);

  const float norm = juce::jlimit(
      0.0f, 1.0f, 0.5f + result.offsetMs / (2.0f * StereoOffsetParams::kMaxOffsetMs));

  const auto x = makeNoise(440 * kBlock, 47, 0.5f);
  StereoOffset offset;
  offset.prepare(kFs, kBlock);
  juce::AudioBuffer<float> buf(2, kBlock);
  std::vector<float> outL, outR;
  for (size_t off = 0; off + kBlock <= x.size(); off += kBlock) {
    for (int i = 0; i < kBlock; ++i) {
      const size_t n = off + static_cast<size_t>(i);
      buf.setSample(0, i, n >= kLag ? x[n - kLag] : 0.0f);
      buf.setSample(1, i, x[n]);
    }
    offset.setTarget(StereoOffsetParams::fromNormalized(norm), true);
    offset.process(buf);
    for (int i = 0; i < kBlock; ++i) {
      outL.push_back(buf.getSample(0, i));
      outR.push_back(buf.getSample(1, i));
    }
  }

  // Well past the 40 ms delay glide-in; residual lag must be zero in both
  // directions (bestCorrelationLag only searches one way).
  const int start = static_cast<int>(kFs);
  EXPECT_EQ(bestCorrelationLag(outL, outR, start, 8 * kBlock, kLag), 0);
  EXPECT_EQ(bestCorrelationLag(outR, outL, start, 8 * kBlock, kLag), 0);
}

// The global two-lane auto-align probe (startAutoOffset/pollAutoOffset,
// measuring Left lane against Right lane) was removed along with the
// stereo-lane chain mode. AutoOffset itself (the measurement engine
// exercised by every other test in this file) is unchanged: Dual Mono's own
// per-block probe (armDualAutoAlign/pollDualAutoAlign, see
// dual_mono_block_tests.cpp) drives the same engine instance at its own
// call site instead of this one.

}  // namespace
