// Pins Reverse (setBlockIrReverse/ChainBlock::reverseEnabled): flips the
// fully-shaped kernel end-for-end, applied last in prepareIrShapeRebuild
// (after Trim Init's copy offset and the Attack/Decay envelope + its
// fade-out), so it reverses whatever's actually audible rather than the raw
// source. The reversal itself (std::reverse over a plain float buffer) is
// trivially correct by construction; what's worth pinning here is the
// plumbing - the toggle actually reaching the rebuilt engine, leaving
// duration/metadata untouched, and surviving undo/redo and state restore -
// same shape as ir_trim_init_tests.cpp, and for the same reason its own
// audio-level check settled for "measurably different", not exact
// onset-timing: a chain-wide artifact unrelated to the loaded IR dominates
// naive raw-sample assertions in this rig.
#include "chain_test_helpers.h"

#include <algorithm>
#include <cstdio>

namespace {
constexpr int kBlock = 512;

void seedMonoIrChain(ChainTestProcessor& proc, const juce::String& blockId,
                     const char* fileName = "reverb-ir-mono-test.wav") {
  juce::ValueTree state("ChainSnapshot");
  state.setProperty("stereoEnabled", false, nullptr);
  juce::ValueTree lane("ChainBlocks");
  lane.appendChild(makeIrBlockTree(blockId, 1, 100, fileName), nullptr);
  state.appendChild(lane, nullptr);
  state.appendChild(juce::ValueTree("RightChainBlocks"), nullptr);
  proc.restoreFromTree(state);
}

void pumpAudio(ChainTestProcessor& proc, int totalMs, bool alreadyWarm = false) {
  const std::vector<float> silence(static_cast<size_t>(kBlock), 0.0f);
  constexpr int kPumpIntervalMs = 50;
  if (!alreadyWarm)
    processStereo(proc, silence);
  for (int elapsed = 0; elapsed < totalMs; elapsed += kPumpIntervalMs) {
    juce::Thread::sleep(kPumpIntervalMs);
    processStereo(proc, silence);
  }
}

bool setReverseAndWaitForRebuild(ChainTestProcessor& proc, const char* blockId, bool enabled) {
  const std::vector<float> silence(static_cast<size_t>(kBlock), 0.0f);
  processStereo(proc, silence);  // prime
  const bool ok = proc.setBlockIrReverse(blockId, enabled);
  pumpAudio(proc, 1200, /*alreadyWarm=*/true);
  return ok;
}

constexpr int kImpulseBlock = 15;
constexpr size_t kImpulseOnset = static_cast<size_t>(kImpulseBlock) * kBlock;

std::vector<float> makeDelayedImpulse(int totalBlocks) {
  std::vector<float> impulse(static_cast<size_t>(totalBlocks * kBlock), 0.0f);
  impulse[kImpulseOnset] = 1.0f;
  return impulse;
}

double irContentLengthMs(ChainTestProcessor& proc, const char* blockId) {
  const juce::var state = proc.getChainState(-1);
  if (const auto* lane = state["chain"].getArray())
    for (const auto& item : *lane)
      if (item["blockId"].toString() == blockId)
        return static_cast<double>(item["irContentLengthMs"]);
  return -1.0;
}

bool reverseOf(ChainTestProcessor& proc, const char* blockId) {
  const juce::var state = proc.getChainState(-1);
  if (const auto* lane = state["chain"].getArray())
    for (const auto& item : *lane)
      if (item["blockId"].toString() == blockId)
        return static_cast<bool>(item["params"]["reverse"]);
  return false;
}
}  // namespace

TEST(ReverseTest, DefaultsOff) {
  ChainTestProcessor proc;
  proc.setPlayConfigDetails(2, 2, kFs, kBlock);
  proc.prepareToPlay(kFs, kBlock);
  seedMonoIrChain(proc, "blk-a");
  ASSERT_TRUE(waitForChainLoaded(proc));

  EXPECT_FALSE(reverseOf(proc, "blk-a"));
}

// Reverse only reorders samples - it must never change the reported
// duration (unlike Size, which deliberately does).
TEST(ReverseTest, DoesNotChangeReportedLength) {
  ChainTestProcessor proc;
  proc.setPlayConfigDetails(2, 2, kFs, kBlock);
  proc.prepareToPlay(kFs, kBlock);
  seedMonoIrChain(proc, "blk-a");
  ASSERT_TRUE(waitForChainLoaded(proc));
  const double beforeMs = irContentLengthMs(proc, "blk-a");

  ASSERT_TRUE(setReverseAndWaitForRebuild(proc, "blk-a", true));
  const double afterMs = irContentLengthMs(proc, "blk-a");

  EXPECT_NEAR(afterMs, beforeMs, beforeMs * 0.01)
      << "Reverse changed the reported content length - it should only reorder samples";
}

// The reported toggle must reflect a real engine change, not just
// getChainState arithmetic - same reasoning and same simplified check as
// TrimInitTest.ActuallyChangesTheConvolvedOutput (see that test's own
// comment for why an exact-onset-timing assertion is unreliable here).
TEST(ReverseTest, ActuallyChangesTheConvolvedOutput) {
  constexpr int kTotalBlocks = 120;
  const auto impulse = makeDelayedImpulse(kTotalBlocks);

  auto runWithReverse = [&](bool enabled) {
    ChainTestProcessor proc;
    proc.setPlayConfigDetails(2, 2, kFs, kBlock);
    proc.prepareToPlay(kFs, kBlock);
    seedMonoIrChain(proc, "blk-a");
    EXPECT_TRUE(waitForChainLoaded(proc)) << "IR block never finished loading from cache";
    EXPECT_TRUE(proc.setBlockParam("blk-a", "mix", 1.0));
    // EXPECT not ASSERT: this lambda has an inferred non-void return type,
    // and ASSERT_TRUE expands to a bare `return;` on failure, which won't
    // typecheck against that.
    if (enabled) EXPECT_TRUE(setReverseAndWaitForRebuild(proc, "blk-a", true));
    letAudioGoIdle();
    const auto [outL, outR] = processStereo(proc, impulse);
    juce::ignoreUnused(outR);
    return outL;
  };

  const auto outOff = runWithReverse(false);
  const auto outOn = runWithReverse(true);

  ASSERT_EQ(outOff.size(), outOn.size());
  float maxAbsDiff = 0.0f;
  for (size_t i = 0; i < outOff.size(); ++i)
    maxAbsDiff = std::max(maxAbsDiff, std::abs(outOff[i] - outOn[i]));

  std::printf("[ReverseTest] max |diff| Reverse off vs on: %.6f\n",
              static_cast<double>(maxAbsDiff));
  EXPECT_GT(maxAbsDiff, 0.01f)
      << "toggling Reverse produced no measurable change in the actual convolved output";
}

TEST(ReverseTest, SurvivesUndoRedo) {
  ChainTestProcessor proc;
  proc.setPlayConfigDetails(2, 2, kFs, kBlock);
  proc.prepareToPlay(kFs, kBlock);
  seedMonoIrChain(proc, "blk-a");
  ASSERT_TRUE(waitForChainLoaded(proc));

  ASSERT_TRUE(setReverseAndWaitForRebuild(proc, "blk-a", true));
  ASSERT_TRUE(reverseOf(proc, "blk-a"));

  ASSERT_TRUE(proc.undoChain());
  pumpAudio(proc, 1200, /*alreadyWarm=*/false);
  EXPECT_FALSE(reverseOf(proc, "blk-a"))
      << "undo didn't restore the pre-toggle state (Reverse wasn't pushed to chain history?)";

  ASSERT_TRUE(proc.redoChain());
  pumpAudio(proc, 1200, /*alreadyWarm=*/false);
  EXPECT_TRUE(reverseOf(proc, "blk-a"));
}

// Same reapply-after-restore shape as TrimInitTest.ReappliesAfterStateRestore.
TEST(ReverseTest, ReappliesAfterStateRestore) {
  constexpr int kTotalBlocks = 120;
  const auto impulse = makeDelayedImpulse(kTotalBlocks);

  ChainTestProcessor proc;
  proc.setPlayConfigDetails(2, 2, kFs, kBlock);
  proc.prepareToPlay(kFs, kBlock);
  seedMonoIrChain(proc, "blk-a");
  ASSERT_TRUE(waitForChainLoaded(proc));
  ASSERT_TRUE(proc.setBlockParam("blk-a", "mix", 1.0));
  ASSERT_TRUE(setReverseAndWaitForRebuild(proc, "blk-a", true));
  letAudioGoIdle();

  const auto [beforeL, beforeR] = processStereo(proc, impulse);

  juce::MemoryBlock savedState;
  proc.getStateInformation(savedState);

  ChainTestProcessor restored;
  restored.setPlayConfigDetails(2, 2, kFs, kBlock);
  restored.prepareToPlay(kFs, kBlock);
  restored.setStateInformation(savedState.getData(), static_cast<int>(savedState.getSize()));

  bool loaded = false;
  {
    const std::vector<float> silence(static_cast<size_t>(kBlock), 0.0f);
    const auto deadline = juce::Time::getMillisecondCounter() + 5000u;
    while (juce::Time::getMillisecondCounter() < deadline) {
      processStereo(restored, silence);
      const juce::var state = restored.getChainState(-1);
      bool allLoaded = true;
      for (const auto* lane : {state["chain"].getArray(), state["chainRight"].getArray()}) {
        if (lane == nullptr) continue;
        for (const auto& item : *lane)
          if (item["kind"].toString() == "tone" && !static_cast<bool>(item["loaded"]))
            allLoaded = false;
      }
      if (allLoaded && !restored.isChainEditFadeHeld()) {
        loaded = true;
        break;
      }
      juce::Thread::sleep(20);
    }
  }
  ASSERT_TRUE(loaded) << "restored IR block never finished reloading";
  ASSERT_TRUE(reverseOf(restored, "blk-a")) << "Reverse didn't persist across state restore";
  ASSERT_TRUE(restored.setBlockParam("blk-a", "mix", 1.0));
  pumpAudio(restored, 1200, /*alreadyWarm=*/true);
  letAudioGoIdle();

  const auto [afterL, afterR] = processStereo(restored, impulse);

  ASSERT_EQ(beforeL.size(), afterL.size());
  float maxAbsDiff = 0.0f;
  for (size_t i = 0; i < beforeL.size(); ++i) {
    maxAbsDiff = std::max(maxAbsDiff, std::abs(beforeL[i] - afterL[i]));
    maxAbsDiff = std::max(maxAbsDiff, std::abs(beforeR[i] - afterR[i]));
  }
  std::printf("[ReverseTest] max |diff| before save vs after restore: %.9f\n",
              static_cast<double>(maxAbsDiff));
  // Same non-bit-exactness caveat as TrimInitTest/IrSizeWidthTest's own
  // reapply tests - JUCE's convolution engine isn't run-to-run bit-exact
  // even for two independently-built engines given the identical kernel.
  EXPECT_LT(maxAbsDiff, 0.03f)
      << "restore didn't reapply the persisted Reverse shape to the reloaded engine";
}
