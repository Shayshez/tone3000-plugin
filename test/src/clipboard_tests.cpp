// Block clipboard tests
//
// copyChainBlock / pasteChainBlock (the UI's Copy on a tone tile and Paste
// on an insert slot): copy stores a self-contained snapshot (settings tree +
// model bytes) on the processor, paste rebuilds a fresh block from it. These
// pin the contracts that distinguish it from duplicateChainBlock's
// clone-by-live-id:
//
//   - paste carries every persisted setting plus the tone/model identity,
//     and loads cache-first from the copied model bytes (fake URLs would
//     fail any network fetch),
//   - the clipboard survives the source block disappearing: paste still
//     works after the copied block is deleted, and after the whole chain is
//     replaced by a state restore (the preset-switch path),
//   - the clipboard is a snapshot, not a reference: edits to the source
//     after the copy don't leak into a later paste,
//   - `canPasteBlock` (getChainState) tracks the clipboard, not the chain,
//   - an empty clipboard and bad sources are rejected,
//   - a paste is one undo step.
#include "Processor.h"
#include "chain_test_helpers.h"

#include <gtest/gtest.h>

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

// A mono rig with one IR block carrying non-default settings, so "the paste
// matches" can't pass vacuously.
void seedMonoChain(ChainTestProcessor& proc, const juce::String& blockId) {
  auto block = makeIrBlockTree(blockId, 1, 100);
  block.setProperty("normalize", false, nullptr);
  block.setProperty("inputGain", 0.3f, nullptr);
  block.setProperty("outputGain", 0.6f, nullptr);
  block.setProperty("mix", 0.7f, nullptr);
  juce::ValueTree state("ChainSnapshot");
  juce::ValueTree left("ChainBlocks");
  left.appendChild(block, nullptr);
  state.appendChild(left, nullptr);
  proc.restoreFromTree(state);
}

TEST(BlockClipboardTest, PasteFillsInsertSlotAndCarriesEverySetting) {
  ChainTestProcessor proc;
  seedMonoChain(proc, "blk-a");
  ASSERT_TRUE(waitForChainLoaded(proc)) << "source never finished loading from cache";

  EXPECT_FALSE(static_cast<bool>(proc.getChainState(-1)["canPasteBlock"]));
  ASSERT_TRUE(proc.copyChainBlock("blk-a"));
  EXPECT_TRUE(static_cast<bool>(proc.getChainState(-1)["canPasteBlock"]));

  // Paste into the first insert slot (lane index 1): the slot is consumed.
  const std::string newId = proc.pasteChainBlock(1);
  ASSERT_FALSE(newId.empty());
  EXPECT_NE(newId, "blk-a");

  const juce::var after = proc.getChainState(-1);
  const auto layout = laneLayout(after, "chain");
  ASSERT_EQ(layout.size(), 5u);  // 2 tones + 3 inserts (kMinLaneSlots)
  EXPECT_EQ(layout[0], std::make_pair(juce::String("tone"), juce::String("blk-a")));
  EXPECT_EQ(layout[1], std::make_pair(juce::String("tone"), juce::String(newId)));

  // Every setting rides along: same tone/model, identical params.
  const juce::var src = after["chain"][0];
  const juce::var paste = after["chain"][1];
  EXPECT_EQ(static_cast<int>(paste["tone"]["id"]), static_cast<int>(src["tone"]["id"]));
  EXPECT_EQ(static_cast<int>(paste["activeModelId"]), static_cast<int>(src["activeModelId"]));
  EXPECT_EQ(juce::JSON::toString(paste["params"]), juce::JSON::toString(src["params"]));
  EXPECT_FLOAT_EQ(static_cast<float>(paste["params"]["inputGain"]), 0.3f);
  EXPECT_FLOAT_EQ(static_cast<float>(paste["params"]["mix"]), 0.7f);
  EXPECT_FALSE(static_cast<bool>(paste["params"]["normalize"]));

  // Cache-first load: the tone's URL is fake, so a loaded paste proves the
  // model bytes came from the clipboard rather than a download.
  EXPECT_TRUE(waitForChainLoaded(proc)) << "paste should load from the copied model cache";

  // One undo step removes the paste and only the paste.
  ASSERT_TRUE(proc.undoChain());
  const auto undone = laneLayout(proc.getChainState(-1), "chain");
  ASSERT_GE(undone.size(), 1u);
  EXPECT_EQ(undone[0].second, "blk-a");
  for (size_t i = 1; i < undone.size(); ++i)
    EXPECT_EQ(undone[i].first, "insert") << "unexpected tone at " << i;
}

TEST(BlockClipboardTest, PasteSurvivesSourceDeletionAndChainReplacement) {
  ChainTestProcessor proc;
  seedMonoChain(proc, "blk-a");
  ASSERT_TRUE(waitForChainLoaded(proc));

  ASSERT_TRUE(proc.copyChainBlock("blk-a"));

  // Deleting the copied block doesn't invalidate the clipboard...
  letAudioGoIdle();
  ASSERT_TRUE(proc.removeChainBlock("blk-a"));
  EXPECT_TRUE(static_cast<bool>(proc.getChainState(-1)["canPasteBlock"]));

  // ...and neither does replacing the whole chain (the preset-switch path:
  // presets restore through the same snapshot machinery as state loads).
  juce::ValueTree other("ChainSnapshot");
  juce::ValueTree left("ChainBlocks");
  left.appendChild(makeIrBlockTree("blk-other", 2, 200), nullptr);
  other.appendChild(left, nullptr);
  proc.restoreFromTree(other);
  ASSERT_TRUE(waitForChainLoaded(proc));
  EXPECT_TRUE(static_cast<bool>(proc.getChainState(-1)["canPasteBlock"]));

  const std::string newId = proc.pasteChainBlock(1);
  ASSERT_FALSE(newId.empty());

  const juce::var after = proc.getChainState(-1);
  const auto layout = laneLayout(after, "chain");
  EXPECT_EQ(layout[0].second, "blk-other");
  EXPECT_EQ(layout[1], std::make_pair(juce::String("tone"), juce::String(newId)));
  // The paste is the copied tone (id 1), not the new chain's (id 2), with
  // the copied settings intact.
  EXPECT_EQ(static_cast<int>(after["chain"][1]["tone"]["id"]), 1);
  EXPECT_FLOAT_EQ(static_cast<float>(after["chain"][1]["params"]["mix"]), 0.7f);
  EXPECT_TRUE(waitForChainLoaded(proc)) << "paste should load from the copied model cache";
}

TEST(BlockClipboardTest, ClipboardIsASnapshotNotAReference) {
  ChainTestProcessor proc;
  seedMonoChain(proc, "blk-a");
  ASSERT_TRUE(waitForChainLoaded(proc));

  ASSERT_TRUE(proc.copyChainBlock("blk-a"));

  // Edit the source after the copy; the paste must carry the copy-time value.
  ASSERT_TRUE(proc.setBlockParam("blk-a", "mix", 0.2));

  const std::string newId = proc.pasteChainBlock(1);
  ASSERT_FALSE(newId.empty());
  const juce::var after = proc.getChainState(-1);
  EXPECT_FLOAT_EQ(static_cast<float>(after["chain"][0]["params"]["mix"]), 0.2f);
  EXPECT_FLOAT_EQ(static_cast<float>(after["chain"][1]["params"]["mix"]), 0.7f);
}

TEST(BlockClipboardTest, RejectsEmptyClipboardAndBadSources) {
  ChainTestProcessor proc;
  seedMonoChain(proc, "blk-a");
  ASSERT_TRUE(waitForChainLoaded(proc));

  // Nothing copied yet.
  EXPECT_TRUE(proc.pasteChainBlock(1).empty());

  // Insert slots and unknown ids are not copyable.
  const juce::String insertId = proc.getChainState(-1)["chain"][1]["blockId"].toString();
  EXPECT_FALSE(proc.copyChainBlock(insertId.toStdString()));
  EXPECT_FALSE(proc.copyChainBlock("not-a-block"));
  EXPECT_FALSE(static_cast<bool>(proc.getChainState(-1)["canPasteBlock"]));
}

}  // namespace
