// Pins resetBlockIrShape: a single button that resets every IR shaping
// parameter (Init/Attack/Decay Length/Level/Curve, Size, Width, Trim Init,
// Reverse - exactly blockHasNonDefaultIrShape's own field list,
// ProcessorChain.cpp) to default in one step. The one property worth a
// dedicated test file (the individual fields' own defaults are already
// pinned by ir_decay_tests.cpp/ir_size_width_tests.cpp/ir_trim_init_tests.cpp/
// ir_reverse_tests.cpp) is that Reset must land in undo history as exactly
// one entry, not one per field - explicitly requested, not incidental.
#include "chain_test_helpers.h"

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

bool resetShapeAndWaitForRebuild(ChainTestProcessor& proc, const char* blockId) {
  const std::vector<float> silence(static_cast<size_t>(kBlock), 0.0f);
  processStereo(proc, silence);  // prime
  const bool ok = proc.resetBlockIrShape(blockId);
  pumpAudio(proc, 1200, /*alreadyWarm=*/true);
  return ok;
}

struct ShapeSnapshot {
  double initLevel, attackLength, attackCurve, decayLength, decayLevel, decayCurve, size, width;
  bool trimInit, trimRelaxed, reverse;
};

ShapeSnapshot shapeOf(ChainTestProcessor& proc, const char* blockId) {
  const juce::var state = proc.getChainState(-1);
  if (const auto* lane = state["chain"].getArray()) {
    for (const auto& item : *lane) {
      if (item["blockId"].toString() != blockId) continue;
      const juce::var p = item["params"];
      return ShapeSnapshot{static_cast<double>(p["initLevel"]),
                           static_cast<double>(p["attackLength"]),
                           static_cast<double>(p["attackCurve"]),
                           static_cast<double>(p["decayLength"]),
                           static_cast<double>(p["decayLevel"]),
                           static_cast<double>(p["decayCurve"]),
                           static_cast<double>(p["size"]),
                           static_cast<double>(p["width"]),
                           static_cast<bool>(p["trimInit"]),
                           static_cast<bool>(p["trimRelaxed"]),
                           static_cast<bool>(p["reverse"])};
    }
  }
  ADD_FAILURE() << "block " << blockId << " not found in chain state";
  return {};
}

// Every field's own default, exactly matching ChainBlock.h's initializers
// (and blockHasNonDefaultIrShape's own comparisons).
void expectDefaultShape(const ShapeSnapshot& s) {
  EXPECT_NEAR(s.initLevel, 1.0, 1e-6);
  EXPECT_NEAR(s.attackLength, 0.0, 1e-6);
  EXPECT_NEAR(s.attackCurve, 0.5, 1e-6);
  EXPECT_NEAR(s.decayLength, 1.0, 1e-6);
  EXPECT_NEAR(s.decayLevel, 1.0, 1e-6);
  EXPECT_NEAR(s.decayCurve, 0.5, 1e-6);
  EXPECT_NEAR(s.size, 0.5, 1e-6);
  EXPECT_NEAR(s.width, 0.75, 1e-6);
  EXPECT_FALSE(s.trimInit);
  EXPECT_FALSE(s.trimRelaxed);
  EXPECT_FALSE(s.reverse);
}
}  // namespace

TEST(ResetShapeTest, NoOpAlreadyAtDefault) {
  ChainTestProcessor proc;
  proc.setPlayConfigDetails(2, 2, kFs, kBlock);
  proc.prepareToPlay(kFs, kBlock);
  seedMonoIrChain(proc, "blk-a");
  ASSERT_TRUE(waitForChainLoaded(proc));

  ASSERT_TRUE(proc.resetBlockIrShape("blk-a"));
  // A no-op reset must not push a history entry - nothing to undo.
  EXPECT_FALSE(proc.undoChain()) << "resetting an already-default block pushed a history entry";
}

TEST(ResetShapeTest, ResetsEveryFieldToDefault) {
  ChainTestProcessor proc;
  proc.setPlayConfigDetails(2, 2, kFs, kBlock);
  proc.prepareToPlay(kFs, kBlock);
  seedMonoIrChain(proc, "blk-a", "reverb-ir-stereo-test.wav");
  ASSERT_TRUE(waitForChainLoaded(proc));

  ASSERT_TRUE(proc.setBlockIrDecay("blk-a", 0.6, 0.3, 0.2, 0.7, 0.4, 0.8));
  ASSERT_TRUE(proc.setBlockIrSize("blk-a", 0.8));
  ASSERT_TRUE(proc.setBlockIrWidth("blk-a", 0.1));
  ASSERT_TRUE(proc.setBlockIrTrimInit("blk-a", true, /*relaxed=*/true));
  ASSERT_TRUE(proc.setBlockIrReverse("blk-a", true));
  pumpAudio(proc, 1200, /*alreadyWarm=*/false);

  const ShapeSnapshot before = shapeOf(proc, "blk-a");
  ASSERT_GT(before.attackLength, 0.0) << "setup didn't actually move the shape off default";

  ASSERT_TRUE(resetShapeAndWaitForRebuild(proc, "blk-a"));
  expectDefaultShape(shapeOf(proc, "blk-a"));
}

// The explicit ask this feature was built for: Reset must land as exactly
// one undo step, never one per field - set several fields as *separate*
// prior actions (each its own history entry, proven by needing several
// undos to fully unwind them), then Reset, then a *single* undo must
// restore every field Reset touched, and must not also revert any of the
// separate prior actions.
TEST(ResetShapeTest, UndoIsExactlyOneStep) {
  ChainTestProcessor proc;
  proc.setPlayConfigDetails(2, 2, kFs, kBlock);
  proc.prepareToPlay(kFs, kBlock);
  seedMonoIrChain(proc, "blk-a", "reverb-ir-stereo-test.wav");
  ASSERT_TRUE(waitForChainLoaded(proc));

  // Each of these is its own history entry (separate calls, not coalesced -
  // different coalesce keys and well outside any drag-coalescing window).
  ASSERT_TRUE(proc.setBlockIrSize("blk-a", 0.8));
  pumpAudio(proc, 300, /*alreadyWarm=*/false);
  ASSERT_TRUE(proc.setBlockIrWidth("blk-a", 0.1));
  pumpAudio(proc, 300, /*alreadyWarm=*/false);
  ASSERT_TRUE(proc.setBlockIrTrimInit("blk-a", true));
  pumpAudio(proc, 300, /*alreadyWarm=*/false);
  const ShapeSnapshot afterSetup = shapeOf(proc, "blk-a");

  ASSERT_TRUE(resetShapeAndWaitForRebuild(proc, "blk-a"));
  expectDefaultShape(shapeOf(proc, "blk-a"));

  ASSERT_TRUE(proc.undoChain());
  pumpAudio(proc, 1200, /*alreadyWarm=*/false);
  const ShapeSnapshot afterOneUndo = shapeOf(proc, "blk-a");

  // One undo must restore *every* field Reset touched, all at once - not
  // just Size, requiring three more undos to get Width/Trim Init back too.
  EXPECT_NEAR(afterOneUndo.size, afterSetup.size, 1e-6);
  EXPECT_NEAR(afterOneUndo.width, afterSetup.width, 1e-6);
  EXPECT_EQ(afterOneUndo.trimInit, afterSetup.trimInit);

  // And it must not have also unwound the three separate setup actions -
  // two more undos should still be available (Width's and Trim Init's own
  // entries), proving Reset didn't merge into or swallow them.
  ASSERT_TRUE(proc.undoChain());
  pumpAudio(proc, 1200, /*alreadyWarm=*/false);
  ASSERT_TRUE(proc.undoChain());
  pumpAudio(proc, 1200, /*alreadyWarm=*/false);
  const ShapeSnapshot afterThreeUndos = shapeOf(proc, "blk-a");
  EXPECT_NEAR(afterThreeUndos.size, 0.8, 1e-6)
      << "Size's own separate setup action should still be intact after unwinding Reset, "
         "Trim Init and Width individually";
}
