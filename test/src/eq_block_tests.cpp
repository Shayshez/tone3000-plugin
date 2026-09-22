// addEqBlock (the tile menu's standalone "EQ" row, ChainBlockType::EQ): a
// block that is nothing but this block's own eq member (every block already
// carries one, for the PRE/POST EQ around its model) - no model, no tone to
// download, mixNormalized pinned at its class default of 1.0 (no Mix control
// in the UI, so nothing ever moves it), loaded the instant it exists rather
// than through the async tone-loading pipeline every other type goes
// through. These pin:
//
//   - it lands at the requested insert slot (or the lane's first insert when
//     none is given), reports blockType "eq", and is loaded immediately -
//     no waitForChainLoaded polling needed, unlike every other block type,
//   - it actually shapes the *entire* signal passing through it (there is no
//     dry path to leak into or out of - mix is permanently 100%),
//   - it survives undo/redo and a full state round trip through the same
//     generic per-block machinery every other type uses,
//   - duplicate/copy/paste come up loaded immediately too (proving
//     queueActiveModelLoad's EQ short-circuit covers those paths, not just
//     fresh creation).
#include "chain_test_helpers.h"

#include <cmath>
#include <cstdio>

namespace {
constexpr int kBlock = 512;

juce::var shapedBand() {
  auto* band = new juce::DynamicObject();
  band->setProperty("type", "bell");
  band->setProperty("freqHz", 1500.0);
  band->setProperty("gainDb", 12.0);
  band->setProperty("q", 1.2);
  return juce::var(band);
}

// Lane items as (kind, blockId) pairs, mirroring duplicate_tests.cpp's own.
std::vector<std::pair<juce::String, juce::String>> laneLayout(const juce::var& state,
                                                              const char* laneKey) {
  std::vector<std::pair<juce::String, juce::String>> layout;
  if (const auto* lane = state[laneKey].getArray())
    for (const auto& item : *lane)
      layout.emplace_back(item["kind"].toString(), item["blockId"].toString());
  return layout;
}

juce::var blockById(TONE3000Processor& proc, const juce::String& blockId,
                    const char* laneKey = "chain") {
  const juce::var state = proc.getChainState(-1);
  if (const auto* lane = state[laneKey].getArray())
    for (const auto& item : *lane)
      if (item["blockId"].toString() == blockId) return item;
  return {};
}

std::pair<std::vector<float>, std::vector<float>> runWithEq(bool shapeEq) {
  ChainTestProcessor proc;
  proc.setPlayConfigDetails(2, 2, kFs, kBlock);
  proc.prepareToPlay(kFs, kBlock);

  const std::string blockId = proc.addEqBlock();
  EXPECT_FALSE(blockId.empty());
  if (shapeEq) EXPECT_TRUE(proc.setBlockEqBand(blockId, 2, shapedBand()));

  constexpr int kWarmupBlocks = 15;
  processStereo(proc, makeNoise(kWarmupBlocks * kBlock, 1111, 0.25f));
  return processStereo(proc, makeNoise(20 * kBlock, 4242, 0.25f));
}

float maxAbsDiff(const std::vector<float>& a, const std::vector<float>& b) {
  float maxDiff = 0.0f;
  for (size_t i = 0; i < a.size(); ++i) maxDiff = std::max(maxDiff, std::abs(a[i] - b[i]));
  return maxDiff;
}
}  // namespace

TEST(EqBlockTest, LandsAtRequestedSlotLoadedAsEqWithFullWetMix) {
  ChainTestProcessor proc;
  proc.setPlayConfigDetails(2, 2, kFs, kBlock);
  proc.prepareToPlay(kFs, kBlock);

  auto ir = makeIrBlockTree("blk-ir", 1, 100);
  juce::ValueTree state("ChainSnapshot");
  juce::ValueTree left("ChainBlocks");
  left.appendChild(ir, nullptr);
  state.appendChild(left, nullptr);
  proc.restoreFromTree(state);
  ASSERT_TRUE(waitForChainLoaded(proc));

  // Target the *last* insert slot, not the first, to prove targeting really
  // resolves the given id rather than always falling back to the lane's
  // first one.
  const auto before = laneLayout(proc.getChainState(-1), "chain");
  ASSERT_GT(before.size(), 1u) << "fixture needs at least 2 slots to prove real targeting";
  const juce::String targetInsertId = before.back().second;
  ASSERT_EQ(before.back().first, juce::String("insert"));

  const std::string newId = proc.addEqBlock(targetInsertId.toStdString());
  ASSERT_FALSE(newId.empty());

  const auto after = laneLayout(proc.getChainState(-1), "chain");
  EXPECT_EQ(after.back().second, juce::String(newId))
      << "addEqBlock landed somewhere other than the requested insert slot";
  EXPECT_EQ(after.size(), before.size())
      << "landing on an insert slot should consume it, not grow the lane";

  const juce::var block = blockById(proc, newId);
  ASSERT_FALSE(block.isVoid());
  EXPECT_EQ(block["blockType"].toString(), juce::String("eq"));
  EXPECT_TRUE(static_cast<bool>(block["loaded"]))
      << "an EQ block has nothing to load - it must be loaded the instant it exists";
  EXPECT_FLOAT_EQ(static_cast<float>(block["params"]["mix"]), 1.0f)
      << "EQ block mix must stay pinned at fully wet - there is no dry path for it";
}

TEST(EqBlockTest, DefaultsToFirstInsertWhenNoTargetGiven) {
  ChainTestProcessor proc;
  proc.setPlayConfigDetails(2, 2, kFs, kBlock);
  proc.prepareToPlay(kFs, kBlock);

  const std::string newId = proc.addEqBlock();
  ASSERT_FALSE(newId.empty());

  const auto layout = laneLayout(proc.getChainState(-1), "chain");
  ASSERT_FALSE(layout.empty());
  EXPECT_EQ(layout.front().second, juce::String(newId));
  EXPECT_EQ(layout.front().first, juce::String("tone"));
}

// The explicit ask this block was built for: it processes the *entire*
// signal passing through it, unlike every other block type where Mix can
// blend the effect away. There is no mix knob to defeat this with; the only
// way to prove it is a real audio-level comparison.
TEST(EqBlockTest, ShapesTheFullSignal) {
  const auto [flatL, flatR] = runWithEq(/*shapeEq=*/false);
  const auto [shapedL, shapedR] = runWithEq(/*shapeEq=*/true);

  ASSERT_EQ(flatL.size(), shapedL.size());
  const float diff = std::max(maxAbsDiff(flatL, shapedL), maxAbsDiff(flatR, shapedR));
  std::printf("[EqBlockTest] max |diff| flat vs. shaped EQ block: %.6f\n",
             static_cast<double>(diff));
  EXPECT_GT(diff, 0.01f) << "shaping the EQ block's own band had no measurable effect on the "
                            "signal passing through it";
}

TEST(EqBlockTest, SurvivesUndoRedo) {
  ChainTestProcessor proc;
  proc.setPlayConfigDetails(2, 2, kFs, kBlock);
  proc.prepareToPlay(kFs, kBlock);

  const std::string newId = proc.addEqBlock();
  ASSERT_FALSE(newId.empty());
  ASSERT_FALSE(blockById(proc, newId).isVoid());

  ASSERT_TRUE(proc.undoChain());
  EXPECT_TRUE(blockById(proc, newId).isVoid()) << "undo didn't remove the newly-added EQ block";

  ASSERT_TRUE(proc.redoChain());
  const juce::var restored = blockById(proc, newId);
  ASSERT_FALSE(restored.isVoid()) << "redo didn't restore the EQ block";
  EXPECT_EQ(restored["blockType"].toString(), juce::String("eq"));
  EXPECT_TRUE(static_cast<bool>(restored["loaded"]));
}

TEST(EqBlockTest, PersistsThroughStateRestoreWithItsBandsIntact) {
  ChainTestProcessor proc;
  proc.setPlayConfigDetails(2, 2, kFs, kBlock);
  proc.prepareToPlay(kFs, kBlock);

  const std::string blockId = proc.addEqBlock();
  ASSERT_FALSE(blockId.empty());
  ASSERT_TRUE(proc.setBlockEqBand(blockId, 2, shapedBand()));

  juce::MemoryBlock savedState;
  proc.getStateInformation(savedState);

  ChainTestProcessor restored;
  restored.setPlayConfigDetails(2, 2, kFs, kBlock);
  restored.prepareToPlay(kFs, kBlock);
  restored.setStateInformation(savedState.getData(), static_cast<int>(savedState.getSize()));

  const juce::var block = blockById(restored, blockId);
  ASSERT_FALSE(block.isVoid()) << "EQ block didn't survive a state round trip";
  EXPECT_EQ(block["blockType"].toString(), juce::String("eq"));
  EXPECT_TRUE(static_cast<bool>(block["loaded"]))
      << "a restored EQ block has nothing to (re)load - it must come back loaded immediately";
  const juce::var band = block["params"]["eq"]["bands"][2];
  EXPECT_NEAR(static_cast<double>(band["gainDb"]), 12.0, 1e-6)
      << "EQ band shaping didn't survive the state round trip";
}

TEST(EqBlockTest, DuplicateAndPasteComeUpLoadedImmediately) {
  ChainTestProcessor proc;
  proc.setPlayConfigDetails(2, 2, kFs, kBlock);
  proc.prepareToPlay(kFs, kBlock);

  const std::string sourceId = proc.addEqBlock();
  ASSERT_FALSE(sourceId.empty());
  ASSERT_TRUE(proc.setBlockEqBand(sourceId, 2, shapedBand()));

  const std::string dupId = proc.duplicateChainBlock(sourceId, 1);
  ASSERT_FALSE(dupId.empty());
  const juce::var dup = blockById(proc, dupId);
  ASSERT_FALSE(dup.isVoid());
  EXPECT_EQ(dup["blockType"].toString(), juce::String("eq"));
  EXPECT_TRUE(static_cast<bool>(dup["loaded"]))
      << "a duplicated EQ block has nothing to reload - it must come up loaded immediately";
  EXPECT_NEAR(static_cast<double>(dup["params"]["eq"]["bands"][2]["gainDb"]), 12.0, 1e-6);

  ASSERT_TRUE(proc.copyChainBlock(sourceId));
  const std::string pasteId = proc.pasteChainBlock(2);
  ASSERT_FALSE(pasteId.empty());
  const juce::var pasted = blockById(proc, pasteId);
  ASSERT_FALSE(pasted.isVoid());
  EXPECT_EQ(pasted["blockType"].toString(), juce::String("eq"));
  EXPECT_TRUE(static_cast<bool>(pasted["loaded"]))
      << "a pasted EQ block has nothing to reload - it must come up loaded immediately";
}
