// Chain block duplicate tests
//
// duplicateChainBlock (the UI's copy/paste and alt-drag duplicate): clones a
// tone block into a chain index with a fresh id. These pin the contracts:
//
//   - the clone carries every persisted setting (gains, mix, enabled,
//     normalize, EQ) plus the tone/model identity, and loads cache-first
//     from the source's in-memory model bytes (the fake URLs would fail any
//     network fetch, so a loaded clone proves no download happened),
//   - a clone landing on an insert slot consumes it (paste); landing on a
//     tone block splices in before it (alt-drop between tiles),
//   - insert-slot and unknown sources are rejected,
//   - the duplicate is one undo step (undo removes the clone, redo re-adds).
#include "Processor.h"
#include "chain_test_helpers.h"

#include <gtest/gtest.h>

#include <vector>

namespace {

// Lane items as (kind, blockId) pairs for structural assertions.
std::vector<std::pair<juce::String, juce::String>> laneLayout(const juce::var& state,
                                                              const char* laneKey) {
  std::vector<std::pair<juce::String, juce::String>> layout;
  if (const auto* lane = state[laneKey].getArray())
    for (const auto& item : *lane)
      layout.emplace_back(item["kind"].toString(), item["blockId"].toString());
  return layout;
}

TEST(ChainDuplicateTest, PasteFillsInsertSlotAndCarriesEverySetting) {
  ChainTestProcessor proc;

  // A mono chain with one IR block carrying deliberately non-default
  // settings, so "the clone matches" can't pass vacuously.
  auto block = makeIrBlockTree("blk-a", 1, 100);
  block.setProperty("normalize", false, nullptr);
  block.setProperty("inputGain", 0.3f, nullptr);
  block.setProperty("outputGain", 0.6f, nullptr);
  block.setProperty("mix", 0.7f, nullptr);
  juce::ValueTree state("ChainSnapshot");
  juce::ValueTree left("ChainBlocks");
  left.appendChild(block, nullptr);
  state.appendChild(left, nullptr);
  proc.restoreFromTree(state);
  ASSERT_TRUE(waitForChainLoaded(proc)) << "source never finished loading from cache";

  // A shaped EQ band on the source (through the real setter).
  auto* band = new juce::DynamicObject();
  band->setProperty("type", "bell");
  band->setProperty("freqHz", 1500.0);
  band->setProperty("gainDb", 4.5);
  band->setProperty("q", 1.2);
  ASSERT_TRUE(proc.setBlockEqBand("blk-a", 2, juce::var(band)));

  // Paste into the first insert slot (lane index 1): the slot is consumed;
  // the clone takes its place, the lane stays at its minimum layout.
  const std::string newId = proc.duplicateChainBlock("blk-a", 1);
  ASSERT_FALSE(newId.empty());
  EXPECT_NE(newId, "blk-a");

  const juce::var after = proc.getChainState(-1);
  const auto layout = laneLayout(after, "chain");
  ASSERT_EQ(layout.size(), 5u);  // 2 tones + 3 inserts (kMinLaneSlots)
  EXPECT_EQ(layout[0], std::make_pair(juce::String("tone"), juce::String("blk-a")));
  EXPECT_EQ(layout[1], std::make_pair(juce::String("tone"), juce::String(newId)));
  for (size_t i = 2; i < layout.size(); ++i)
    EXPECT_EQ(layout[i].first, "insert") << "unexpected tone at " << i;

  // Every setting rides along: same tone/model, identical params + EQ.
  const juce::var src = after["chain"][0];
  const juce::var clone = after["chain"][1];
  EXPECT_EQ(static_cast<int>(clone["tone"]["id"]), static_cast<int>(src["tone"]["id"]));
  EXPECT_EQ(static_cast<int>(clone["activeModelId"]), static_cast<int>(src["activeModelId"]));
  EXPECT_EQ(juce::JSON::toString(clone["params"]), juce::JSON::toString(src["params"]));
  EXPECT_FLOAT_EQ(static_cast<float>(clone["params"]["inputGain"]), 0.3f);
  EXPECT_FLOAT_EQ(static_cast<float>(clone["params"]["mix"]), 0.7f);
  EXPECT_FALSE(static_cast<bool>(clone["params"]["normalize"]));
  EXPECT_DOUBLE_EQ(static_cast<double>(clone["params"]["eq"]["bands"][2]["gainDb"]), 4.5);

  // Cache-first load: the tone's URL is fake, so a loaded clone proves the
  // model bytes were copied rather than re-downloaded.
  EXPECT_TRUE(waitForChainLoaded(proc)) << "clone should load from the copied model cache";
}

TEST(ChainDuplicateTest, SplicesBeforeToneBlockAndUndoesAsOneStep) {
  ChainTestProcessor proc;
  seedChain(proc, {"blk-a", "blk-b"});
  ASSERT_TRUE(waitForChainLoaded(proc));

  // Alt-drop between the two tone blocks: index 1 holds a tone block, so the
  // clone splices in front of it instead of consuming anything.
  const std::string newId = proc.duplicateChainBlock("blk-a", 1);
  ASSERT_FALSE(newId.empty());
  {
    const juce::var state = proc.getChainState(-1);
    const auto layout = laneLayout(state, "chain");
    ASSERT_EQ(layout.size(), 5u);  // 3 tones + 2 inserts after normalization
    EXPECT_EQ(layout[0].second, "blk-a");
    EXPECT_EQ(layout[1].second, juce::String(newId));
    EXPECT_EQ(layout[2].second, "blk-b");
  }

  // One undo step removes the clone and only the clone…
  ASSERT_TRUE(proc.undoChain());
  {
    const auto layout = laneLayout(proc.getChainState(-1), "chain");
    ASSERT_GE(layout.size(), 2u);
    EXPECT_EQ(layout[0].second, "blk-a");
    EXPECT_EQ(layout[1].second, "blk-b");
  }

  // …and redo brings it back in place (with the same id).
  ASSERT_TRUE(proc.redoChain());
  EXPECT_EQ(laneLayout(proc.getChainState(-1), "chain")[1].second, juce::String(newId));
}

TEST(ChainDuplicateTest, RejectsInsertSlotAndUnknownSources) {
  ChainTestProcessor proc;
  seedChain(proc, {"blk-a"});
  ASSERT_TRUE(waitForChainLoaded(proc));

  const juce::String insertId = proc.getChainState(-1)["chain"][1]["blockId"].toString();
  EXPECT_TRUE(proc.duplicateChainBlock(insertId.toStdString(), 0).empty());
  EXPECT_TRUE(proc.duplicateChainBlock("not-a-block", 0).empty());
}

}  // namespace
