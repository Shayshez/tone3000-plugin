// Multi-core processing tests
//
// Multi-core mode spreads an oversampled NAM engine's phase instances across
// the RtWorkerPool realtime workers (see RtWorkerPool.h). Parallelism is
// pure scheduling (no arithmetic or ordering changes anywhere), so its one
// testable contract is strong:
//
//   - the parallel schedule's output is BIT-IDENTICAL to the serial one,
//     across oversampling factors and across mid-stream toggles of the
//     setting,
//   - the pool survives the host lifecycle (re-prepare, release, restart)
//     with audio flowing throughout.
//
// Chains are seeded through setStateInformation with model bytes embedded
// (ModelCache), so loads are cache-first and never touch the network.
#include "Processor.h"
#include "chain_test_helpers.h"

#include <gtest/gtest.h>

#include <utility>
#include <vector>

namespace {

constexpr int kBlock = 512;

// Skips the first second like the rest of the suite (wet fades, smoothers,
// convolver engagement), but expects *zero* difference after it.
float settledDiff(const std::vector<float>& a, const std::vector<float>& b) {
  return settledMaxChannelDiff(a, b, 48000);
}

// Mono + oversampling isolates the NAM phase fork: the engine's phase
// instances run on pool workers when multi-core is on and sequentially when
// it's off, and the two schedules must null exactly at every factor.
TEST(MultiCoreTest, MonoPhaseParallelMatchesSerialBitExact) {
  auto runMonoRig = [](bool multiCore, float osFactorNormalized, const std::vector<float>& in) {
    ChainTestProcessor proc;
    proc.setMultiCoreEnabled(multiCore, /*persist=*/false);
    proc.setPlayConfigDetails(2, 2, kFs, kBlock);
    proc.parameters.getParameter("osEnabled")->setValueNotifyingHost(1.0f);
    proc.parameters.getParameter("osFactor")->setValueNotifyingHost(osFactorNormalized);
    proc.prepareToPlay(kFs, kBlock);

    juce::ValueTree state("ChainSnapshot");
    juce::ValueTree lane("ChainBlocks");
    lane.appendChild(makeNamBlockTree("blk-amp", 1, 100), nullptr);
    auto cab = makeIrBlockTree("blk-cab", 2, 200);
    cab.setProperty("mix", 0.7f, nullptr);
    lane.appendChild(cab, nullptr);
    state.appendChild(lane, nullptr);
    proc.restoreFromTree(state);
    EXPECT_TRUE(waitForChainLoaded(proc)) << "blocks never finished loading from cache";

    return processStereo(proc, in);
  };

  struct Factor {
    float normalized;
    const char* label;
  };
  const auto in = makeNoise(240 * kBlock, 27182, 0.1f);

  for (const Factor f : {Factor{0.0f, "2x"}, Factor{0.5f, "4x"}, Factor{1.0f, "8x"}}) {
    SCOPED_TRACE(juce::String("oversampling ") + f.label);

    const auto [sl, sr] = runMonoRig(false, f.normalized, in);
    const auto [pl, pr] = runMonoRig(true, f.normalized, in);

    EXPECT_EQ(settledDiff(sl, pl), 0.0f) << "phase fork diverged from the serial phase loop";
    EXPECT_EQ(settledDiff(sr, pr), 0.0f) << "fan-out channel diverged";
  }
}

// The toggle is pure scheduling and applies per callback, so flipping it
// repeatedly mid-stream (as a user would from Settings) must leave the
// output bit-identical to a run that never toggled: the hardest version of
// the contract, on a mono NAM chain at 8x (phase forks appearing and
// disappearing between blocks).
TEST(MultiCoreTest, MidStreamTogglesStayBitExact) {
  auto makeMonoOversampledRig = [](ChainTestProcessor& proc) {
    proc.setPlayConfigDetails(2, 2, 48000.0, kBlock);
    proc.parameters.getParameter("osEnabled")->setValueNotifyingHost(1.0f);
    proc.parameters.getParameter("osFactor")->setValueNotifyingHost(1.0f);  // 8x
    proc.prepareToPlay(48000.0, kBlock);

    juce::ValueTree state("ChainSnapshot");
    juce::ValueTree lane("ChainBlocks");
    lane.appendChild(makeNamBlockTree("blk-amp", 1, 100), nullptr);
    state.appendChild(lane, nullptr);
    proc.restoreFromTree(state);
  };

  const auto in = makeNoise(240 * kBlock, 16180, 0.1f);

  ChainTestProcessor serialProc;
  serialProc.setMultiCoreEnabled(false, /*persist=*/false);
  makeMonoOversampledRig(serialProc);
  ASSERT_TRUE(waitForChainLoaded(serialProc)) << "blocks never finished loading from cache";
  const auto [sl, sr] = processStereo(serialProc, in);

  ChainTestProcessor proc;
  proc.setMultiCoreEnabled(true, /*persist=*/false);
  makeMonoOversampledRig(proc);
  ASSERT_TRUE(waitForChainLoaded(proc)) << "blocks never finished loading from cache";

  // Feed the same signal in 30-block chunks, flipping the setting between
  // chunks (processor state carries across processStereo calls).
  std::vector<float> tl, tr;
  bool multiCore = true;
  constexpr size_t kChunk = 30 * kBlock;
  for (size_t off = 0; off < in.size(); off += kChunk) {
    const std::vector<float> chunk(in.begin() + static_cast<long>(off),
                                   in.begin() + static_cast<long>(off + kChunk));
    const auto [cl, cr] = processStereo(proc, chunk);
    tl.insert(tl.end(), cl.begin(), cl.end());
    tr.insert(tr.end(), cr.begin(), cr.end());
    multiCore = !multiCore;
    proc.setMultiCoreEnabled(multiCore, /*persist=*/false);
  }

  EXPECT_EQ(settledDiff(sl, tl), 0.0f) << "left output diverged across mid-stream toggles";
  EXPECT_EQ(settledDiff(sr, tr), 0.0f) << "right output diverged across mid-stream toggles";
}

// Informational, the headline case for the phase fork: a mono NAM at 8x
// runs 8 phase instances per block, the worst single-thread load in the
// plugin when serial and near-ideal parallelism when forked (8 independent
// equal-size jobs). Expect the largest speedup of the suite here.
TEST(MultiCoreTest, ReportsOversampledSpeedup) {
  auto measure = [](bool multiCore) {
    ChainTestProcessor proc;
    proc.setMultiCoreEnabled(multiCore, /*persist=*/false);
    proc.setPlayConfigDetails(2, 2, kFs, kBlock);
    proc.parameters.getParameter("osEnabled")->setValueNotifyingHost(1.0f);
    proc.parameters.getParameter("osFactor")->setValueNotifyingHost(1.0f);  // 8x
    proc.prepareToPlay(kFs, kBlock);

    juce::ValueTree state("ChainSnapshot");
    juce::ValueTree lane("ChainBlocks");
    lane.appendChild(makeNamBlockTree("blk-amp", 1, 100), nullptr);
    state.appendChild(lane, nullptr);
    proc.restoreFromTree(state);
    EXPECT_TRUE(waitForChainLoaded(proc)) << "blocks never finished loading from cache";

    const auto in = makeNoise(60 * kBlock, 1618, 0.1f);
    processStereo(proc, in);  // warm-up: fades settle, caches warm

    const auto t0 = juce::Time::getHighResolutionTicks();
    processStereo(proc, in);
    const auto t1 = juce::Time::getHighResolutionTicks();
    return juce::Time::highResolutionTicksToSeconds(t1 - t0);
  };

  const double serial = measure(false);
  const double parallel = measure(true);
  std::printf("  mono NAM at 8x, %.2f s audio: serial %.1f ms, parallel %.1f ms (%.2fx)\n",
              60.0 * kBlock / kFs, serial * 1000.0, parallel * 1000.0, serial / parallel);
}

// Host lifecycle: the pool is restarted by every prepareToPlay and stopped
// by releaseResources. Audio must flow correctly through stop/start cycles,
// including a processBlock after releaseResources (the fork gates fall
// back to serial when the pool is down, they must not deadlock or crash).
// Oversampling stays on so the NAM phase fork rides through every cycle.
TEST(MultiCoreTest, WorkerSurvivesHostLifecycle) {
  ChainTestProcessor proc;
  proc.setMultiCoreEnabled(true, /*persist=*/false);
  proc.setPlayConfigDetails(2, 2, kFs, kBlock);
  proc.parameters.getParameter("osEnabled")->setValueNotifyingHost(1.0f);
  proc.parameters.getParameter("osFactor")->setValueNotifyingHost(1.0f);  // 8x
  proc.prepareToPlay(kFs, kBlock);

  juce::ValueTree state("ChainSnapshot");
  juce::ValueTree lane("ChainBlocks");
  lane.appendChild(makeNamBlockTree("blk-amp", 1, 100), nullptr);
  state.appendChild(lane, nullptr);
  proc.restoreFromTree(state);
  ASSERT_TRUE(waitForChainLoaded(proc)) << "blocks never finished loading from cache";

  const auto in = makeNoise(120 * kBlock, 999, 0.1f);
  auto rms = [](const std::vector<float>& v) {
    double sum = 0.0;
    for (float s : v)
      sum += static_cast<double>(s) * s;
    return std::sqrt(sum / static_cast<double>(v.size()));
  };

  for (int cycle = 0; cycle < 3; ++cycle) {
    SCOPED_TRACE("cycle " + juce::String(cycle));
    const auto [l, r] = processStereo(proc, in);
    EXPECT_GT(rms(l), 1e-4) << "left channel went silent";
    EXPECT_GT(rms(r), 1e-4) << "right channel went silent";

    proc.releaseResources();

    // A host should not process after releaseResources, but a defensive
    // block through the downed worker must degrade to serial, not hang.
    juce::AudioBuffer<float> stray(2, kBlock);
    stray.clear();
    juce::MidiBuffer midi;
    proc.processBlock(stray, midi);

    proc.prepareToPlay(kFs, kBlock);
  }
}

}  // namespace
