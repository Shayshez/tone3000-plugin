// StereoOffset tests
//
// The mechanical guarantees of the post-chain stereo alignment engine
// (StereoOffset.h; design notes in plugin/docs/stereo-image.md), Dual Mono's
// own per-block Align:
//
//   StereoOffsetTest  the chosen side really is delayed by the dialed ms
//                     (bit-exact with the deck off), the deck sections
//                     (wobble / crossover / diffuse, all default off) hold
//                     the same mechanical guarantees as the shared ImageDeck
//                     primitives promise (no audio-rate wobble residue, the
//                     crossover keeps lows out of the deck, the diffuser
//                     preserves magnitude), and side swaps with the deck
//                     engaged never click.
//
// The by-ear items (mono fold-down comb motion, flange zone, "reads as a
// second take") can only be checked by listening.
#include "StereoOffset.h"
#include "test_helpers.h"

#include <gtest/gtest.h>
#include <juce_audio_basics/juce_audio_basics.h>

#include <utility>
#include <vector>

namespace {

constexpr int kBlock = 512;

struct StereoResult {
  std::vector<float> l, r;
};

double rms(const std::vector<float>& x, size_t from) {
  double sum = 0.0;
  for (size_t i = from; i < x.size(); ++i)
    sum += static_cast<double>(x[i]) * static_cast<double>(x[i]);
  return std::sqrt(sum / static_cast<double>(x.size() - from));
}

// L-R null depth in dB relative to L over [settled, end): deeply negative =
// the channels carry the same signal (dual-mono), near 0 = fully doubled.
double nullDepthDb(const StereoResult& out, size_t settled) {
  std::vector<float> diff(out.l.size());
  for (size_t i = 0; i < diff.size(); ++i)
    diff[i] = out.l[i] - out.r[i];
  return db(std::pow(rms(diff, settled), 2.0)) - db(std::pow(rms(out.l, settled), 2.0));
}

// Knob values held for a runStereoOffset pass; defaults mirror the APVTS
// defaults (the whole deck off: the plain corrective tool).
struct AlignSettings {
  float offsetNorm = 0.5f;
  float wobbleNorm = 0.25f;
  float crossoverNorm = 0.5f;
  bool wobbleOn = false;
  bool crossoverOn = false;
  bool diffuseOn = false;
};

// Streams mono input (seeded onto both channels) through a StereoOffset
// held at fixed knob values. Input length must be a multiple of kBlock.
StereoResult runStereoOffset(const std::vector<float>& in, const AlignSettings& s) {
  StereoOffset offset;
  offset.prepare(kFs, kBlock);
  juce::AudioBuffer<float> buf(2, kBlock);
  StereoResult out;
  out.l.reserve(in.size());
  out.r.reserve(in.size());
  for (size_t off = 0; off < in.size(); off += kBlock) {
    for (int i = 0; i < kBlock; ++i) {
      buf.setSample(0, i, in[off + static_cast<size_t>(i)]);
      buf.setSample(1, i, in[off + static_cast<size_t>(i)]);
    }
    offset.setTarget(StereoOffsetParams::fromNormalized(s.offsetNorm, s.wobbleNorm,
                                                        s.crossoverNorm, s.wobbleOn,
                                                        s.crossoverOn, s.diffuseOn),
                     true);
    offset.process(buf);
    for (int i = 0; i < kBlock; ++i) {
      out.l.push_back(buf.getSample(0, i));
      out.r.push_back(buf.getSample(1, i));
    }
  }
  return out;
}

TEST(StereoOffsetTest, DelaysTheChosenSideByTheDialedMs) {
  // Full-right knob = the right chain delayed by 24 ms; the left chain must
  // pass through untouched. Same mechanical check as a pure-delay test:
  // compare samples directly once the 40 ms glide has settled.
  // (A short-window correlation was flaky on MSVC — it once reported a
  // spurious peak at 879 samples against a correct 1152-sample delay.)
  const int frames = 96 * kBlock;
  const auto in = makeNoise(frames, 7, 0.5f);
  const auto out = runStereoOffset(in, {.offsetNorm = 1.0f});

  EXPECT_EQ(out.l, in);  // untouched side: not one bit may move

  // 24 ms = 1152 samples at 48 kHz. Skip 1 s so we are well past the ramp
  // and the delay line holds only post-settle audio.
  const int expectedLag = static_cast<int>(std::round(24.0e-3 * kFs));
  const size_t settled = static_cast<size_t>(kFs);
  ASSERT_EQ(expectedLag, 1152);
  for (size_t i = settled; i < in.size(); ++i)
    ASSERT_NEAR(out.r[i], in[i - static_cast<size_t>(expectedLag)], 1e-4f)
        << "delayed side is not a pure 24 ms delay at sample " << i;
}

TEST(StereoOffsetTest, WobblePowerOffIsTimeInvariant) {
  // Full wobble depth on the knob with the switch off (the align default)
  // must be exactly time-invariant. Proof by periodicity: feed a
  // bit-exactly periodic input (one 128-sample sine cycle tiled, 375 Hz at
  // 48 kHz) and require the settled output to repeat with the same period.
  // Any drift here would corrupt a corrective alignment.
  constexpr int kPeriod = 128;
  const auto cycle = makeSine(kPeriod, kFs / kPeriod, 0.5f);
  const int frames = 288 * kBlock;  // ~3 s
  std::vector<float> in(static_cast<size_t>(frames));
  for (int i = 0; i < frames; ++i)
    in[static_cast<size_t>(i)] = cycle[static_cast<size_t>(i % kPeriod)];

  const auto out =
      runStereoOffset(in, {.offsetNorm = 0.75f, .wobbleNorm = 1.0f, .wobbleOn = false});

  const size_t start = 2 * static_cast<size_t>(kFs);
  for (const auto* ch : {&out.l, &out.r})
    for (size_t i = start; i < ch->size(); ++i)
      ASSERT_NEAR((*ch)[i], (*ch)[i - kPeriod], 1e-6f)
          << "output not periodic at sample " << i << "; the wobble switch leaks";
}

TEST(StereoOffsetTest, WobbleAddsNoBroadbandFizz) {
  // The wobble must stay a sub-audio wander: any audio-rate residue in the
  // delay-time noise frequency-modulates the lag channel into broadband
  // noise skirts: audible fizz. Feed a pure sine at full wobble depth and
  // require the floor far from the carrier to stay down. (Regression: a
  // single 0.3 Hz one-pole on the noise leaves ~1% of its variance above
  // 20 Hz, which read as fizz once the depth normalization was corrected.)
  // Moved here from the now-deleted Spread's own copy of this test when
  // Spread was removed - DeckWobble is the shared ImageDeck.h primitive
  // underneath both engines, so the regression coverage belongs wherever a
  // deck-carrying engine still lives.
  const int frames = 288 * kBlock;  // ~3 s
  const auto in = makeSine(frames, 500.0, 0.5f);
  // +20 ms on the right side, wobble 100%, deck sections on (overriding
  // AlignSettings' corrective-tool-off defaults to match the conditions
  // this regression actually needs: the full deck engaged).
  const auto out = runStereoOffset(in, {.offsetNorm = 0.5f + 20.0f / 48.0f,
                                        .wobbleNorm = 1.0f,
                                        .wobbleOn = true,
                                        .crossoverOn = true,
                                        .diffuseOn = true});

  // Analyze the lag channel after the engage glide + walk settle (~1 s).
  const size_t start = static_cast<size_t>(kFs);
  const size_t n = out.r.size() - start;
  const double carrierDb = db(goertzelPower(out.r.data() + start, n, 500.0));

  // Probe bins far from the carrier and off its harmonics.
  for (const double freq : {2917.0, 4111.0, 6373.0, 9241.0, 13687.0}) {
    const double noiseDb = db(goertzelPower(out.r.data() + start, n, freq));
    EXPECT_LT(noiseDb - carrierDb, -90.0)
        << "fizz at " << freq << " Hz: " << (noiseDb - carrierDb) << " dB rel. carrier";
  }
}

TEST(StereoOffsetTest, CrossoverKeepsLowsOutOfTheDeck) {
  // With identical signal on both channels and a full-right offset, a 30 Hz
  // tone stays (near-)equal across the channels while the crossover is on
  // (lows skip the delay on both sides, and both sides get the identical
  // LR4 phase rotation), and diverges strongly with it off, where the full
  // band is delayed 24 ms (~260 degrees at 30 Hz).
  const int frames = 64 * kBlock;
  const auto in = makeSine(frames, 30.0, 0.5f);
  const size_t settled = static_cast<size_t>(kFs / 2);
  const double onDb =
      nullDepthDb(runStereoOffset(in, {.offsetNorm = 1.0f, .crossoverOn = true}), settled);
  const double offDb = nullDepthDb(runStereoOffset(in, {.offsetNorm = 1.0f}), settled);
  EXPECT_LT(onDb, -35.0) << "lows entered the deck with the crossover on: " << onDb << " dB";
  EXPECT_GT(offDb, -10.0) << "lows not delayed with the crossover off: " << offDb << " dB";
}

TEST(StereoOffsetTest, DiffusePreservesMagnitudeWithNoTrim) {
  // The diffuser is allpass (magnitude-flat) and align applies NO
  // precedence trim: a corrective tool must not color levels. Broadband RMS
  // of the diffused/delayed side must match the input's.
  const int frames = 96 * kBlock;
  const auto in = makeNoise(frames, 31337, 0.5f);
  const auto out = runStereoOffset(in, {.offsetNorm = 0.75f, .diffuseOn = true});
  const size_t settled = static_cast<size_t>(kFs);
  const double gainDb = db(std::pow(rms(out.r, settled), 2.0)) -
                        db(std::pow(rms(in, settled), 2.0));
  EXPECT_NEAR(gainDb, 0.0, 0.25) << "diffused side level moved by " << gainDb << " dB";
}

TEST(StereoOffsetTest, SideSwapWithDeckOnNeverClicks) {
  // Slam the knob between the extremes (±24 ms) every 40 blocks with the
  // whole deck engaged. A side swap must first glide the delay to zero AND
  // blend wobble/diffusion out (identity on the processed side at the
  // handover), so no channel may ever step.
  const int numBlocks = 240;
  const auto in = makeSine(numBlocks * kBlock, 440.0, 0.5f);

  StereoOffset offset;
  offset.prepare(kFs, kBlock);
  juce::AudioBuffer<float> buf(2, kBlock);
  std::vector<float> outL, outR;
  for (int b = 0; b < numBlocks; ++b) {
    for (int i = 0; i < kBlock; ++i) {
      buf.setSample(0, i, in[static_cast<size_t>(b * kBlock + i)]);
      buf.setSample(1, i, in[static_cast<size_t>(b * kBlock + i)]);
    }
    const float offsetNorm = (b / 40) % 2 == 0 ? 1.0f : 0.0f;
    offset.setTarget(
        StereoOffsetParams::fromNormalized(offsetNorm, 1.0f, 0.5f, true, true, true), true);
    offset.process(buf);
    for (int i = 0; i < kBlock; ++i) {
      outL.push_back(buf.getSample(0, i));
      outR.push_back(buf.getSample(1, i));
    }
  }

  // A 440 Hz sine at 0.5 moves ≤ ~0.029 per sample; glides, blends, and
  // filter transients stretch that, but a click is a near-step. Skip the
  // engage blend-in.
  const size_t settled = static_cast<size_t>(kFs / 10);
  for (const auto* ch : {&outL, &outR})
    for (size_t i = settled; i < ch->size(); ++i)
      ASSERT_LT(std::abs((*ch)[i] - (*ch)[i - 1]), 0.1f)
          << "discontinuity at sample " << i;
}

}  // namespace
