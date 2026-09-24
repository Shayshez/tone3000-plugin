// addInsertSlot tests (the chain-map chip menu's Add Slot Left/Right)
//
//   - the new empty slot lands exactly at the requested lane index, even
//     above the insert invariant's minimum count (4 tones => normally one
//     "+"),
//   - a later structural edit's back-to-front trim removes trailing surplus
//     first, so the slot the user positioned survives,
//   - it's one undo step.
#include "Processor.h"
#include "chain_test_helpers.h"

#include <gtest/gtest.h>

namespace {

std::vector<juce::String> kinds(ChainTestProcessor& proc) {
  std::vector<juce::String> out;
  const juce::var state = proc.getChainState(-1);  // keep alive while iterating
  if (const auto* lane = state["chain"].getArray())
    for (const auto& item : *lane)
      out.push_back(item["kind"] == "insert" ? juce::String("+")
                                             : item["blockId"].toString());
  return out;
}

using V = std::vector<juce::String>;

TEST(InsertSlotTest, AddsAtIndexSurvivesTrimAndUndoes) {
  ChainTestProcessor proc;
  seedChain(proc, {"a", "b", "c", "d"});
  ASSERT_TRUE(waitForChainLoaded(proc));
  ASSERT_EQ(kinds(proc), (V{"a", "b", "c", "d", "+"}));

  // Add Slot Right of "a".
  ASSERT_FALSE(proc.addInsertSlot(1).empty());
  EXPECT_EQ(kinds(proc), (V{"a", "+", "b", "c", "d", "+"}));

  // A structural edit normalizes: removing "d" leaves 3 tones (min 2
  // inserts), so nothing is trimmed and the positioned slot stays.
  letAudioGoIdle();
  ASSERT_TRUE(proc.removeChainBlock("d"));
  EXPECT_EQ(kinds(proc), (V{"a", "+", "b", "c", "+"}));

  // Undo the removal, then undo the add.
  ASSERT_TRUE(proc.undoChain());
  ASSERT_TRUE(proc.undoChain());
  EXPECT_EQ(kinds(proc), (V{"a", "b", "c", "d", "+"}));
}

TEST(InsertSlotTest, ClampsOutOfRangeIndex) {
  ChainTestProcessor proc;
  seedChain(proc, {"a", "b", "c", "d"});
  ASSERT_TRUE(waitForChainLoaded(proc));
  ASSERT_FALSE(proc.addInsertSlot(-5).empty());
  ASSERT_FALSE(proc.addInsertSlot(999).empty());
  EXPECT_EQ(kinds(proc), (V{"+", "a", "b", "c", "d", "+", "+"}));
}

TEST(InsertSlotTest, RemoveDeletesMiddleSlotButNeverTheRightmost) {
  ChainTestProcessor proc;
  seedChain(proc, {"a", "b", "c", "d"});
  ASSERT_TRUE(waitForChainLoaded(proc));
  ASSERT_FALSE(proc.addInsertSlot(1).empty());
  ASSERT_EQ(kinds(proc), (V{"a", "+", "b", "c", "d", "+"}));

  const juce::var state = proc.getChainState(-1);
  const std::string middle = state["chain"][1]["blockId"].toString().toStdString();
  const std::string last = state["chain"][5]["blockId"].toString().toStdString();

  EXPECT_FALSE(proc.removeInsertSlot(last)) << "rightmost + must stay";
  EXPECT_FALSE(proc.removeInsertSlot("a")) << "not an insert slot";
  EXPECT_FALSE(proc.removeInsertSlot("nope"));

  ASSERT_TRUE(proc.removeInsertSlot(middle));
  EXPECT_EQ(kinds(proc), (V{"a", "b", "c", "d", "+"}));

  ASSERT_TRUE(proc.undoChain());
  EXPECT_EQ(kinds(proc), (V{"a", "+", "b", "c", "d", "+"}));
}

TEST(InsertSlotTest, RemoveBelowMinimumRegrowsAtTheEnd) {
  // 2 tones => 3 inserts minimum: deleting a middle "+" can't shrink the
  // count, so the lane re-grows one at the end (net effect: it moved).
  ChainTestProcessor proc;
  seedChain(proc, {"a", "b"});
  ASSERT_TRUE(waitForChainLoaded(proc));
  ASSERT_FALSE(proc.addInsertSlot(1).empty());  // a + b + + +
  ASSERT_TRUE(proc.removeChainBlock("b"));        // a + + + (3 inserts, fine)
  const juce::var state = proc.getChainState(-1);
  const auto before = kinds(proc);
  ASSERT_EQ(before, (V{"a", "+", "+", "+", "+"}));
  ASSERT_TRUE(proc.removeInsertSlot(state["chain"][1]["blockId"].toString().toStdString()));
  EXPECT_EQ(kinds(proc), (V{"a", "+", "+", "+", "+"}));
}

}  // namespace
