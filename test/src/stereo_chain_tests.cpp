// Stereo through the chain
//
// The contract: stereo holds from wherever it starts all the way to the
// output. A stereo source stays stereo through every block; a mono source
// turns stereo at the first widening block and stays that way. NAM models are
// single-channel, so NamEngine carries a second, independent set of instances
// for the right channel (see NamEngine.h); these tests pin both that engine
// and the audible result of whole chains.
#include "NAM/get_dsp.h"
#include "NamEngine.h"
#include "json.hpp"
#include "RtWorkerPool.h"
#include "chain_test_helpers.h"

#include <gtest/gtest.h>

#include <cmath>
#include <memory>
#include <vector>

namespace {

constexpr int kBlock = 512;
constexpr double kPi = 3.14159265358979323846;

std::vector<std::unique_ptr<nam::DSP>> makeInstances(const nlohmann::json& config, int count) {
  std::vector<std::unique_ptr<nam::DSP>> instances;
  for (int i = 0; i < count; ++i)
    instances.push_back(nam::get_dsp(config));
  return instances;
}

nlohmann::json loadConfig() {
  juce::MemoryBlock bytes;
  EXPECT_TRUE(testFile("a2-amp-test.nam").loadFileAsData(bytes));
  const auto* begin = static_cast<const char*>(bytes.getData());
  return nlohmann::json::parse(begin, begin + bytes.getSize());
}

std::unique_ptr<NamEngine> makeEngine(int factor, bool stereo) {
  const auto config = loadConfig();
  auto engine = std::make_unique<NamEngine>(
      makeInstances(config, factor), factor,
      stereo ? makeInstances(config, factor) : std::vector<std::unique_ptr<nam::DSP>>{});
  engine->prepare(kBlock);
  return engine;
}

using Stereo = std::pair<std::vector<float>, std::vector<float>>;

// Runs two channels through an engine in kBlock slices, like the chain does.
Stereo runEngine(NamEngine& engine, std::vector<float> l, std::vector<float> r,
                 RtWorkerPool* pool = nullptr) {
  for (size_t off = 0; off + kBlock <= l.size(); off += kBlock) {
    float* channels[] = {l.data() + off, r.data() + off};
    juce::AudioBuffer<float> view(channels, 2, kBlock);
    engine.process(view, pool);
  }
  return {std::move(l), std::move(r)};
}

// The mono-engine reference: what one amp does to one signal.
std::vector<float> runMono(int factor, const std::vector<float>& in) {
  auto engine = makeEngine(factor, false);
  return runEngine(*engine, in, in).first;
}

float maxDiff(const std::vector<float>& a, const std::vector<float>& b, size_t begin, size_t end) {
  float diff = 0.0f;
  for (size_t i = begin; i < end; ++i)
    diff = std::max(diff, std::abs(a[i] - b[i]));
  return diff;
}

std::vector<float> makeSine(double hz, size_t length, float amplitude = 0.2f) {
  std::vector<float> out(length);
  for (size_t i = 0; i < length; ++i)
    out[i] = amplitude * static_cast<float>(std::sin(2.0 * kPi * hz * i / kFs));
  return out;
}

// Power of one frequency over the settled region.
double toneLevel(const std::vector<float>& signal, double hz, size_t skip = 48000) {
  return goertzelPower(signal.data() + skip, signal.size() - skip, hz);
}

Stereo processTwoChannels(TONE3000Processor& proc, const std::vector<float>& l,
                          const std::vector<float>& r) {
  std::vector<float> outL(l.size(), 0.0f), outR(r.size(), 0.0f);
  juce::AudioBuffer<float> buffer(2, kBlock);
  juce::MidiBuffer midi;
  for (size_t off = 0; off + kBlock <= l.size(); off += kBlock) {
    buffer.copyFrom(0, 0, l.data() + off, kBlock);
    buffer.copyFrom(1, 0, r.data() + off, kBlock);
    proc.processBlock(buffer, midi);
    std::copy_n(buffer.getReadPointer(0), kBlock, outL.begin() + static_cast<long>(off));
    std::copy_n(buffer.getReadPointer(1), kBlock, outR.begin() + static_cast<long>(off));
  }
  return {std::move(outL), std::move(outR)};
}

void loadRig(ChainTestProcessor& proc, std::vector<juce::ValueTree> blocks) {
  proc.setPlayConfigDetails(2, 2, kFs, kBlock);
  proc.prepareToPlay(kFs, kBlock);
  juce::ValueTree state("ChainSnapshot");
  juce::ValueTree lane("ChainBlocks");
  for (auto& block : blocks)
    lane.appendChild(block, nullptr);
  state.appendChild(lane, nullptr);
  proc.restoreFromTree(state);
  ASSERT_TRUE(waitForChainLoaded(proc)) << "blocks never finished loading from cache";
}

}  // namespace

// Each side is exactly one amp: left from the first sample, right as soon as
// its fade-in is over.
TEST(NamEngineStereoTest, DifferentChannelsEachGetTheirOwnAmp) {
  for (const int factor : {1, 2, 4}) {
    SCOPED_TRACE(factor);
    const size_t length = 200 * kBlock;
    const auto inL = makeNoise(static_cast<int>(length), 1234, 0.1f);
    const auto inR = makeNoise(static_cast<int>(length), 5678, 0.1f);

    auto engine = makeEngine(factor, true);
    const auto [outL, outR] = runEngine(*engine, inL, inR);

    const size_t fadeEnd = static_cast<size_t>(0.06 * kFs * factor);
    EXPECT_EQ(maxDiff(outL, runMono(factor, inL), 0, length), 0.0f);
    EXPECT_EQ(maxDiff(outR, runMono(factor, inR), fadeEnd, length), 0.0f);
    EXPECT_GT(maxDiff(outL, outR, fadeEnd, length), 1.0e-3f);
  }
}

// The mono rig's guarantee: identical channels cost one amp and sound like
// one amp, bit for bit.
TEST(NamEngineStereoTest, IdenticalChannelsFanOutBitExact) {
  const size_t length = 100 * kBlock;
  const auto in = makeNoise(static_cast<int>(length), 42, 0.1f);

  auto engine = makeEngine(2, true);
  const auto [outL, outR] = runEngine(*engine, in, in);

  EXPECT_EQ(maxDiff(outL, outR, 0, length), 0.0f);
  EXPECT_EQ(maxDiff(outL, runMono(2, in), 0, length), 0.0f);
}

// An upstream mono convolver leaves its two channels a ULP apart: that is
// still one signal, not a reason to run a second amp.
TEST(NamEngineStereoTest, ChannelsAUlpApartCountAsIdentical) {
  const size_t length = 50 * kBlock;
  const auto inL = makeNoise(static_cast<int>(length), 42, 0.1f);
  auto inR = inL;
  for (auto& sample : inR)
    sample = std::nextafter(sample, 1.0f);

  auto engine = makeEngine(1, true);
  const auto [outL, outR] = runEngine(*engine, inL, inR);

  EXPECT_EQ(maxDiff(outL, outR, 0, length), 0.0f);
}

// Stereo ending (a reverb tail dying into a mono source, say) hands back to
// the fan-out after the hold; stereo starting again must fade in, not step.
TEST(NamEngineStereoTest, RightSetIdlesAndWakesWithoutAStep) {
  const size_t stereoPart = 40 * kBlock;
  const size_t monoPart = 160 * kBlock;  // > 1 s hold + fade at 48 kHz
  const size_t length = stereoPart + monoPart + stereoPart;

  auto inL = makeNoise(static_cast<int>(length), 1, 0.1f);
  auto inR = makeNoise(static_cast<int>(length), 2, 0.1f);
  std::copy(inL.begin() + static_cast<long>(stereoPart),
            inL.begin() + static_cast<long>(stereoPart + monoPart),
            inR.begin() + static_cast<long>(stereoPart));

  auto engine = makeEngine(1, true);
  const auto [outL, outR] = runEngine(*engine, inL, inR);

  const size_t wake = stereoPart + monoPart;
  EXPECT_EQ(maxDiff(outL, outR, wake - 4 * kBlock, wake), 0.0f) << "right set never idled";

  const float settledGap = maxDiff(outL, outR, wake + 8 * kBlock, length);
  EXPECT_GT(settledGap, 1.0e-3f) << "right set never woke";
  EXPECT_LT(maxDiff(outL, outR, wake, wake + 16), settledGap * 0.02f)
      << "right channel stepped instead of fading in";
}

// Multi-core is pure scheduling, stereo included.
TEST(NamEngineStereoTest, ParallelMatchesSerialBitExact) {
  const size_t length = 100 * kBlock;
  const auto inL = makeNoise(static_cast<int>(length), 3, 0.1f);
  const auto inR = makeNoise(static_cast<int>(length), 4, 0.1f);

  RtWorkerPool pool;
  pool.start(kFs, kBlock, 3);

  for (const int factor : {1, 4, 8}) {
    SCOPED_TRACE(factor);
    auto serial = makeEngine(factor, true);
    auto parallel = makeEngine(factor, true);
    const auto [sl, sr] = runEngine(*serial, inL, inR);
    const auto [pl, pr] = runEngine(*parallel, inL, inR, &pool);
    EXPECT_EQ(maxDiff(sl, pl, 0, length), 0.0f);
    EXPECT_EQ(maxDiff(sr, pr, 0, length), 0.0f);
  }
  pool.stop();
}

// A stereo source through an amp: each input stays on its own side.
TEST(StereoChainTest, StereoSourceSurvivesANamBlock) {
  ChainTestProcessor proc;
  loadRig(proc, {makeNamBlockTree("amp", 1, 100)});

  const size_t length = 240 * kBlock;
  const auto [outL, outR] =
      processTwoChannels(proc, makeSine(220.0, length), makeSine(330.0, length));

  EXPECT_GT(toneLevel(outL, 220.0), 100.0 * toneLevel(outL, 330.0));
  EXPECT_GT(toneLevel(outR, 330.0), 100.0 * toneLevel(outR, 220.0));
}

// Stereo through a whole rig, amp and cab, with the chain oversampled.
TEST(StereoChainTest, StereoSourceSurvivesAnOversampledAmpAndCab) {
  ChainTestProcessor proc;
  proc.parameters.getParameter("osEnabled")->setValueNotifyingHost(1.0f);
  proc.parameters.getParameter("osFactor")->setValueNotifyingHost(0.5f);  // 4x
  loadRig(proc, {makeNamBlockTree("amp", 1, 100), makeIrBlockTree("cab", 2, 200)});

  const size_t length = 240 * kBlock;
  const auto [outL, outR] =
      processTwoChannels(proc, makeSine(220.0, length), makeSine(330.0, length));

  EXPECT_GT(toneLevel(outL, 220.0), 100.0 * toneLevel(outL, 330.0));
  EXPECT_GT(toneLevel(outR, 330.0), 100.0 * toneLevel(outR, 220.0));
}

// The reported rig: a mono source, a stereo reverb IR, then the amp. The IR
// widens the signal and the amp must carry that to the output.
TEST(StereoChainTest, MonoSourceWidenedByStereoIrStaysStereoThroughNam) {
  ChainTestProcessor proc;
  loadRig(proc, {makeIrBlockTree("verb", 1, 100, "reverb-ir-stereo-test.wav", "irPlayer"),
                 makeNamBlockTree("amp", 2, 200)});

  const auto in = makeNoise(240 * kBlock, 99, 0.1f);
  const auto [outL, outR] = processStereo(proc, in);

  EXPECT_GT(settledMaxChannelDiff(outL, outR), 1.0e-3f) << "the amp collapsed the IR's stereo";
}

// Nothing in this rig widens, so the channels stay the same signal, however
// many blocks it passes through.
TEST(StereoChainTest, MonoSourceThroughMonoBlocksStaysIdentical) {
  ChainTestProcessor proc;
  loadRig(proc, {makeIrBlockTree("cab", 1, 100), makeNamBlockTree("amp", 2, 200)});

  const auto in = makeNoise(240 * kBlock, 7, 0.1f);
  const auto [outL, outR] = processStereo(proc, in);

  EXPECT_EQ(settledMaxChannelDiff(outL, outR), 0.0f);
}

// The input-mode button picks one side of a stereo source: that side is the
// mono signal the whole chain then sees.
TEST(StereoChainTest, SingleSideInputModeIsMonoAgain) {
  ChainTestProcessor proc;
  loadRig(proc, {makeNamBlockTree("amp", 1, 100)});
  proc.setInputMode(TONE3000Processor::InputMode::Left);

  const size_t length = 240 * kBlock;
  const auto [outL, outR] =
      processTwoChannels(proc, makeSine(220.0, length), makeSine(330.0, length));

  EXPECT_EQ(settledMaxChannelDiff(outL, outR), 0.0f);
  EXPECT_GT(toneLevel(outR, 220.0), 100.0 * toneLevel(outR, 330.0));
}
