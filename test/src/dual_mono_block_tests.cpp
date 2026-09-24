// addDualMonoBlock (the tile menu's "Dual Mono" row, ChainBlockType::DUAL_MONO):
// a block with no tone/model of its own - just two fixed child slots
// (dualLeft/dualRight, at most one block each) that seed independently from
// the incoming buffer, run through the ordinary per-block path, and
// recombine (see runDualMono, Processor.cpp). These pin:
//
//   - it lands at the requested insert slot (or the lane's first insert when
//     none is given), reports blockType "dualMono", and is loaded
//     immediately (it has nothing of its own to fetch) - the same shape as
//     ChainBlockType::EQ,
//   - with a spare physical channel (a 2-ch buffer) it *widens*: each side's
//     processed signal lands in its own output channel, proven by swapping
//     which content sits on which side and confirming the output channels
//     swap with it,
//   - pinned to one physical channel (a genuinely mono host output) it
//     *folds*: identical content on both sides reduces to exactly what that
//     content would produce as a plain, unwrapped block; different content
//     on both sides measurably differs from either side alone,
//   - an empty side is a true no-op (seeded straight through), and seeding
//     keeps the two channels distinct rather than duplicating one into both,
//   - a live Pan/Width change (setDualImage) glides, never steps,
//   - it survives undo/redo (both of its own creation and of a live
//     pan/width drag, which coalesces into one undo step) and a full state
//     round trip with its per-side content and image intact,
//   - removing the block tears down both children atomically,
//   - a dual child's id resolves through the same generic per-block API
//     every top-level block uses (findBlockById's recursion into
//     dualLeft/dualRight).
#include "chain_test_helpers.h"

#include <cmath>
#include <cstdio>
#include <functional>

namespace {
constexpr int kBlock = 512;
constexpr int kWarmupBlocks = 15;  // settles the structural-edit ChainEditFade

// Lane items as (kind, blockId) pairs, mirroring eq_block_tests.cpp's own.
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

float maxAbsDiff(const std::vector<float>& a, const std::vector<float>& b) {
  float maxDiff = 0.0f;
  for (size_t i = 0; i < a.size(); ++i) maxDiff = std::max(maxDiff, std::abs(a[i] - b[i]));
  return maxDiff;
}

// Recurses into "dualLeft"/"dualRight" as well as the two top-level lanes -
// waitForChainLoaded (chain_test_helpers.h) only walks "chain"/"chainRight",
// blind to a Dual Mono block's nested child slots.
bool allToneItemsLoaded(const juce::Array<juce::var>* items) {
  if (items == nullptr) return true;
  for (const auto& item : *items) {
    if (item["kind"].toString() == "tone" && !static_cast<bool>(item["loaded"]))
      return false;
    if (!allToneItemsLoaded(item["dualLeft"].getArray())) return false;
    if (!allToneItemsLoaded(item["dualRight"].getArray())) return false;
  }
  return true;
}

bool waitForDualMonoLoaded(TONE3000Processor& proc, int timeoutMs = 20000) {
  const auto deadline = juce::Time::getMillisecondCounter() + static_cast<juce::uint32>(timeoutMs);
  while (juce::Time::getMillisecondCounter() < deadline) {
    const juce::var state = proc.getChainState(-1);
    const bool allLoaded = allToneItemsLoaded(state["chain"].getArray()) &&
                           allToneItemsLoaded(state["chainRight"].getArray());
    if (allLoaded && !proc.isChainEditFadeHeld())
      return true;
    juce::Thread::sleep(20);
  }
  return false;
}

// A Dual Mono block tree in plugin-state shape (mirrors makeIrBlockTree/
// makeNamBlockTree's own shape), nesting `leftChild`/`rightChild` (each 0 or
// 1 block, e.g. from makeIrBlockTree/makeNamBlockTree) under
// DualLeftBlocks/DualRightBlocks exactly like serializeChainToTree emits -
// see ProcessorState.cpp/ProcessorHistory.cpp's own DUAL_MONO nesting.
juce::ValueTree makeDualMonoBlockTree(const juce::String& blockId,
                                      juce::ValueTree leftChild = {},
                                      juce::ValueTree rightChild = {}, float leftPan = 0.0f,
                                      float rightPan = 1.0f, float width = 1.0f) {
  juce::ValueTree block("ChainBlock");
  block.setProperty("id", blockId, nullptr);
  block.setProperty("type", "dualMono", nullptr);
  block.setProperty("enabled", true, nullptr);
  block.setProperty("normalize", true, nullptr);
  block.setProperty("inputGain", 0.5f, nullptr);
  block.setProperty("outputGain", 0.5f, nullptr);
  block.setProperty("mix", 1.0f, nullptr);
  block.setProperty("toneId", 0, nullptr);
  block.setProperty(
      "toneJson",
      juce::String("{\"id\":0,\"local\":true,\"title\":\"Dual Mono\",\"format\":\"dualMono\"}"),
      nullptr);
  block.setProperty("activeModelId", 0, nullptr);
  block.setProperty("dualLeftPan", leftPan, nullptr);
  block.setProperty("dualRightPan", rightPan, nullptr);
  block.setProperty("dualWidth", width, nullptr);

  juce::ValueTree dualLeftState("DualLeftBlocks");
  if (leftChild.isValid()) dualLeftState.appendChild(leftChild, nullptr);
  block.appendChild(dualLeftState, nullptr);

  juce::ValueTree dualRightState("DualRightBlocks");
  if (rightChild.isValid()) dualRightState.appendChild(rightChild, nullptr);
  block.appendChild(dualRightState, nullptr);

  return block;
}

// Wraps `blockTree` as the sole content of the chain, restores it, and
// drives `in` through. `monoRig` picks which pipeline shape the block runs
// under: false = the chain sees the full (up to 2-channel) buffer directly;
// true = a genuinely mono host output (a 1-channel buffer), the only way to
// reach Dual Mono's numChannels==1 fold branch where Pan/Width go inert and
// the two sides average via a true 50/50 sum. Returns both output channels
// (a mono rig duplicates its one channel into both).
std::pair<std::vector<float>, std::vector<float>> runLane(const juce::ValueTree& blockTree,
                                                           const std::vector<float>& in,
                                                           bool monoRig) {
  ChainTestProcessor proc;
  proc.setPlayConfigDetails(2, monoRig ? 1 : 2, kFs, kBlock);
  proc.prepareToPlay(kFs, kBlock);

  juce::ValueTree state("ChainSnapshot");
  juce::ValueTree lane("ChainBlocks");
  lane.appendChild(blockTree.createCopy(), nullptr);
  state.appendChild(lane, nullptr);
  proc.restoreFromTree(state);
  EXPECT_TRUE(waitForDualMonoLoaded(proc));

  if (!monoRig) {
    processStereo(proc, makeNoise(kWarmupBlocks * kBlock, 1111, 0.25f));
    return processStereo(proc, in);
  }

  auto driveMono = [&](const std::vector<float>& signal) {
    const int total = static_cast<int>(signal.size());
    std::vector<float> out(signal.size(), 0.0f);
    juce::AudioBuffer<float> buffer(1, kBlock);
    juce::MidiBuffer midi;
    for (int off = 0; off + kBlock <= total; off += kBlock) {
      buffer.copyFrom(0, 0, signal.data() + off, kBlock);
      proc.processBlock(buffer, midi);
      std::copy(buffer.getReadPointer(0), buffer.getReadPointer(0) + kBlock, out.begin() + off);
    }
    return out;
  };

  driveMono(makeNoise(kWarmupBlocks * kBlock, 1111, 0.25f));
  const auto out = driveMono(in);
  return {out, out};
}
}  // namespace

TEST(DualMonoBlockTest, LandsAtRequestedSlotLoadedAsDualMonoWithFullWetMix) {
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

  const std::string newId = proc.addDualMonoBlock(targetInsertId.toStdString());
  ASSERT_FALSE(newId.empty());

  const auto after = laneLayout(proc.getChainState(-1), "chain");
  EXPECT_EQ(after.back().second, juce::String(newId))
      << "addDualMonoBlock landed somewhere other than the requested insert slot";
  EXPECT_EQ(after.size(), before.size())
      << "landing on an insert slot should consume it, not grow the lane";

  const juce::var block = blockById(proc, newId);
  ASSERT_FALSE(block.isVoid());
  EXPECT_EQ(block["blockType"].toString(), juce::String("dualMono"));
  EXPECT_TRUE(static_cast<bool>(block["loaded"]))
      << "a Dual Mono block has no tone of its own - it must be loaded the instant it exists";
  EXPECT_FLOAT_EQ(static_cast<float>(block["params"]["mix"]), 1.0f);
  EXPECT_TRUE(block["dualLeft"].getArray() != nullptr && block["dualLeft"].getArray()->isEmpty())
      << "a fresh Dual Mono block starts with both sides empty";
  EXPECT_TRUE(block["dualRight"].getArray() != nullptr && block["dualRight"].getArray()->isEmpty());
}

TEST(DualMonoBlockTest, DefaultsToFirstInsertWhenNoTargetGiven) {
  ChainTestProcessor proc;
  proc.setPlayConfigDetails(2, 2, kFs, kBlock);
  proc.prepareToPlay(kFs, kBlock);

  const std::string newId = proc.addDualMonoBlock();
  ASSERT_FALSE(newId.empty());

  const auto layout = laneLayout(proc.getChainState(-1), "chain");
  ASSERT_FALSE(layout.empty());
  EXPECT_EQ(layout.front().second, juce::String(newId));
  EXPECT_EQ(layout.front().first, juce::String("tone"));
}

TEST(DualMonoBlockTest, ConvertBlockToDualMonoWrapsSourceAsLeftChild) {
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

  const auto before = laneLayout(proc.getChainState(-1), "chain");
  size_t sourceIndex = before.size();
  for (size_t i = 0; i < before.size(); ++i)
    if (before[i].second == juce::String("blk-ir")) sourceIndex = i;
  ASSERT_LT(sourceIndex, before.size()) << "fixture setup didn't land the source block";

  const std::string wrapperId = proc.convertBlockToDualMono("blk-ir");
  ASSERT_FALSE(wrapperId.empty());
  ASSERT_TRUE(waitForDualMonoLoaded(proc));

  const auto after = laneLayout(proc.getChainState(-1), "chain");
  EXPECT_EQ(after.size(), before.size())
      << "converting in place should not change the lane's slot count";
  EXPECT_EQ(after[sourceIndex].second, juce::String(wrapperId))
      << "the wrapper should land at the exact slot the original block occupied";
  EXPECT_TRUE(blockById(proc, "blk-ir").isVoid())
      << "the original block's own id should no longer resolve as a top-level block";

  const juce::var wrapper = blockById(proc, wrapperId);
  ASSERT_FALSE(wrapper.isVoid());
  EXPECT_EQ(wrapper["blockType"].toString(), juce::String("dualMono"));
  const auto* leftArr = wrapper["dualLeft"].getArray();
  ASSERT_TRUE(leftArr != nullptr && leftArr->size() == 1)
      << "the source's own content should land as the wrapper's sole Left child";
  const juce::var leftChild = (*leftArr)[0];
  EXPECT_EQ(leftChild["blockType"].toString(), juce::String("ir"));
  EXPECT_EQ(static_cast<int>(leftChild["tone"]["id"]), 1);
  EXPECT_FLOAT_EQ(static_cast<float>(leftChild["params"]["mix"]), 1.0f);
  const auto* rightArr = wrapper["dualRight"].getArray();
  EXPECT_TRUE(rightArr != nullptr && rightArr->isEmpty())
      << "conversion should leave the Right side empty for the user to fill in";
}

TEST(DualMonoBlockTest, ConvertBlockToDualMonoRejectsInsertAndAlreadyDualBlocks) {
  ChainTestProcessor proc;
  proc.setPlayConfigDetails(2, 2, kFs, kBlock);
  proc.prepareToPlay(kFs, kBlock);

  const auto layout = laneLayout(proc.getChainState(-1), "chain");
  ASSERT_FALSE(layout.empty());
  EXPECT_EQ(layout.front().first, juce::String("insert"));
  EXPECT_TRUE(proc.convertBlockToDualMono(layout.front().second.toStdString()).empty())
      << "an insert slot has no content to convert";

  const std::string dualId = proc.addDualMonoBlock();
  ASSERT_FALSE(dualId.empty());
  EXPECT_TRUE(proc.convertBlockToDualMono(dualId).empty())
      << "a Dual Mono block is already the target shape";
}

TEST(DualMonoBlockTest, ConvertBlockToDualMonoSurvivesUndoRedo) {
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

  const std::string wrapperId = proc.convertBlockToDualMono("blk-ir");
  ASSERT_FALSE(wrapperId.empty());
  ASSERT_TRUE(waitForDualMonoLoaded(proc));

  ASSERT_TRUE(proc.undoChain());
  EXPECT_TRUE(blockById(proc, wrapperId).isVoid()) << "undo didn't remove the wrapper";
  const juce::var restoredSource = blockById(proc, "blk-ir");
  ASSERT_FALSE(restoredSource.isVoid()) << "undo didn't restore the original block";
  EXPECT_EQ(restoredSource["blockType"].toString(), juce::String("ir"));

  ASSERT_TRUE(proc.redoChain());
  EXPECT_TRUE(blockById(proc, "blk-ir").isVoid());
  const juce::var restoredWrapper = blockById(proc, wrapperId);
  ASSERT_FALSE(restoredWrapper.isVoid()) << "redo didn't restore the wrapper";
  EXPECT_EQ(restoredWrapper["blockType"].toString(), juce::String("dualMono"));
}

TEST(DualMonoBlockTest, CollapseDualMonoToSingleRestoresTheLoneLoadedSide) {
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

  const auto before = laneLayout(proc.getChainState(-1), "chain");
  size_t sourceIndex = before.size();
  for (size_t i = 0; i < before.size(); ++i)
    if (before[i].second == juce::String("blk-ir")) sourceIndex = i;
  ASSERT_LT(sourceIndex, before.size());

  const std::string wrapperId = proc.convertBlockToDualMono("blk-ir");
  ASSERT_FALSE(wrapperId.empty());
  ASSERT_TRUE(waitForDualMonoLoaded(proc));

  const std::string singleId = proc.collapseDualMonoToSingle(wrapperId);
  ASSERT_FALSE(singleId.empty());
  ASSERT_TRUE(waitForChainLoaded(proc));

  const auto after = laneLayout(proc.getChainState(-1), "chain");
  EXPECT_EQ(after.size(), before.size())
      << "collapsing in place should not change the lane's slot count";
  EXPECT_EQ(after[sourceIndex].second, juce::String(singleId))
      << "the collapsed block should land at the exact slot the wrapper occupied";
  EXPECT_TRUE(blockById(proc, wrapperId).isVoid())
      << "the wrapper's own id should no longer resolve as a top-level block";

  const juce::var single = blockById(proc, singleId);
  ASSERT_FALSE(single.isVoid());
  EXPECT_EQ(single["blockType"].toString(), juce::String("ir"));
  EXPECT_EQ(static_cast<int>(single["tone"]["id"]), 1);
  EXPECT_FLOAT_EQ(static_cast<float>(single["params"]["mix"]), 1.0f);
}

TEST(DualMonoBlockTest, CollapseDualMonoToSingleRejectsBothEmptyOrBothLoaded) {
  ChainTestProcessor proc;
  proc.setPlayConfigDetails(2, 2, kFs, kBlock);
  proc.prepareToPlay(kFs, kBlock);

  const std::string emptyWrapperId = proc.addDualMonoBlock();
  ASSERT_FALSE(emptyWrapperId.empty());
  EXPECT_TRUE(proc.collapseDualMonoToSingle(emptyWrapperId).empty())
      << "both sides empty has nothing to collapse to";

  const std::string leftId = proc.loadToneIntoDualSlot(
      emptyWrapperId, /*isLeftSide=*/true,
      juce::String(
          "{\"id\":1,\"format\":\"nam\",\"models\":[{\"id\":10,\"name\":\"amp\",\"model_url\":"
          "\"https://test.invalid/amp.nam\"}]}"));
  ASSERT_FALSE(leftId.empty());
  const std::string rightId = proc.loadToneIntoDualSlot(
      emptyWrapperId, /*isLeftSide=*/false,
      juce::String(
          "{\"id\":2,\"format\":\"ir\",\"models\":[{\"id\":20,\"name\":\"cab\",\"model_url\":"
          "\"https://test.invalid/cab.wav\"}]}"));
  ASSERT_FALSE(rightId.empty());
  EXPECT_TRUE(proc.collapseDualMonoToSingle(emptyWrapperId).empty())
      << "both sides loaded has no clear single side to keep";
}

TEST(DualMonoBlockTest, CollapseDualMonoToSingleSurvivesUndoRedo) {
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

  const std::string wrapperId = proc.convertBlockToDualMono("blk-ir");
  ASSERT_FALSE(wrapperId.empty());
  ASSERT_TRUE(waitForDualMonoLoaded(proc));

  const std::string singleId = proc.collapseDualMonoToSingle(wrapperId);
  ASSERT_FALSE(singleId.empty());
  ASSERT_TRUE(waitForChainLoaded(proc));

  ASSERT_TRUE(proc.undoChain());
  EXPECT_TRUE(blockById(proc, singleId).isVoid()) << "undo didn't remove the collapsed block";
  const juce::var restoredWrapper = blockById(proc, wrapperId);
  ASSERT_FALSE(restoredWrapper.isVoid()) << "undo didn't restore the wrapper";
  EXPECT_EQ(restoredWrapper["blockType"].toString(), juce::String("dualMono"));

  ASSERT_TRUE(proc.redoChain());
  EXPECT_TRUE(blockById(proc, wrapperId).isVoid());
  const juce::var restoredSingle = blockById(proc, singleId);
  ASSERT_FALSE(restoredSingle.isVoid()) << "redo didn't restore the collapsed block";
  EXPECT_EQ(restoredSingle["blockType"].toString(), juce::String("ir"));
}

TEST(DualMonoBlockTest, CopyDualSlotFromSiblingFillsEmptySideWithClone) {
  ChainTestProcessor proc;
  proc.setPlayConfigDetails(2, 2, kFs, kBlock);
  proc.prepareToPlay(kFs, kBlock);

  const std::string wrapperId = proc.addDualMonoBlock();
  ASSERT_FALSE(wrapperId.empty());

  const std::string leftId = proc.loadToneIntoDualSlot(
      wrapperId, /*isLeftSide=*/true,
      juce::String("{\"id\":1,\"format\":\"nam\",\"models\":[{\"id\":10,\"name\":\"amp\","
                  "\"model_url\":\"https://test.invalid/amp.nam\"}]}"));
  ASSERT_FALSE(leftId.empty());

  const std::string copiedId = proc.copyDualSlotFromSibling(wrapperId, /*toLeftSide=*/false);
  ASSERT_FALSE(copiedId.empty());

  const juce::var wrapper = blockById(proc, wrapperId);
  ASSERT_FALSE(wrapper.isVoid());
  const auto* rightArr = wrapper["dualRight"].getArray();
  ASSERT_TRUE(rightArr != nullptr && rightArr->size() == 1);
  const juce::var rightChild = (*rightArr)[0];
  EXPECT_EQ(rightChild["blockType"].toString(), juce::String("nam"));
  EXPECT_EQ(static_cast<int>(rightChild["tone"]["id"]), 1);
  EXPECT_NE(rightChild["blockId"].toString(), juce::String(leftId))
      << "the copy must be its own block, not a shared reference";
}

TEST(DualMonoBlockTest, CopyDualSlotFromSiblingRejectsWhenTargetLoadedOrSiblingEmpty) {
  ChainTestProcessor proc;
  proc.setPlayConfigDetails(2, 2, kFs, kBlock);
  proc.prepareToPlay(kFs, kBlock);

  const std::string wrapperId = proc.addDualMonoBlock();
  ASSERT_FALSE(wrapperId.empty());
  EXPECT_TRUE(proc.copyDualSlotFromSibling(wrapperId, true).empty())
      << "both sides empty - nothing to copy";

  const std::string leftId = proc.loadToneIntoDualSlot(
      wrapperId, /*isLeftSide=*/true,
      juce::String("{\"id\":1,\"format\":\"nam\",\"models\":[{\"id\":10,\"name\":\"amp\","
                  "\"model_url\":\"https://test.invalid/amp.nam\"}]}"));
  ASSERT_FALSE(leftId.empty());
  EXPECT_TRUE(proc.copyDualSlotFromSibling(wrapperId, true).empty())
      << "target already loaded - would silently discard its own content";

  const std::string rightId = proc.loadToneIntoDualSlot(
      wrapperId, /*isLeftSide=*/false,
      juce::String("{\"id\":2,\"format\":\"ir\",\"models\":[{\"id\":20,\"name\":\"cab\","
                  "\"model_url\":\"https://test.invalid/cab.wav\"}]}"));
  ASSERT_FALSE(rightId.empty());
  EXPECT_TRUE(proc.copyDualSlotFromSibling(wrapperId, true).empty())
      << "both sides loaded - still nothing valid to target";
}

TEST(DualMonoBlockTest, CopyDualSlotFromSiblingSurvivesUndoRedo) {
  ChainTestProcessor proc;
  proc.setPlayConfigDetails(2, 2, kFs, kBlock);
  proc.prepareToPlay(kFs, kBlock);

  const std::string wrapperId = proc.addDualMonoBlock();
  ASSERT_FALSE(wrapperId.empty());
  const std::string leftId = proc.loadToneIntoDualSlot(
      wrapperId, /*isLeftSide=*/true,
      juce::String("{\"id\":1,\"format\":\"nam\",\"models\":[{\"id\":10,\"name\":\"amp\","
                  "\"model_url\":\"https://test.invalid/amp.nam\"}]}"));
  ASSERT_FALSE(leftId.empty());

  const std::string copiedId = proc.copyDualSlotFromSibling(wrapperId, /*toLeftSide=*/false);
  ASSERT_FALSE(copiedId.empty());

  ASSERT_TRUE(proc.undoChain());
  const juce::var afterUndo = blockById(proc, wrapperId);
  ASSERT_FALSE(afterUndo.isVoid());
  const auto* rightAfterUndo = afterUndo["dualRight"].getArray();
  EXPECT_TRUE(rightAfterUndo != nullptr && rightAfterUndo->isEmpty())
      << "undo didn't clear the copied side back to empty";

  ASSERT_TRUE(proc.redoChain());
  const juce::var afterRedo = blockById(proc, wrapperId);
  ASSERT_FALSE(afterRedo.isVoid());
  const auto* rightAfterRedo = afterRedo["dualRight"].getArray();
  ASSERT_TRUE(rightAfterRedo != nullptr && rightAfterRedo->size() == 1);
  EXPECT_EQ((*rightAfterRedo)[0]["blockType"].toString(), juce::String("nam"));
}

TEST(DualMonoBlockTest, SurvivesUndoRedoOfCreation) {
  ChainTestProcessor proc;
  proc.setPlayConfigDetails(2, 2, kFs, kBlock);
  proc.prepareToPlay(kFs, kBlock);

  const std::string newId = proc.addDualMonoBlock();
  ASSERT_FALSE(newId.empty());
  ASSERT_FALSE(blockById(proc, newId).isVoid());

  ASSERT_TRUE(proc.undoChain());
  EXPECT_TRUE(blockById(proc, newId).isVoid())
      << "undo didn't remove the newly-added Dual Mono block";

  ASSERT_TRUE(proc.redoChain());
  const juce::var restored = blockById(proc, newId);
  ASSERT_FALSE(restored.isVoid()) << "redo didn't restore the Dual Mono block";
  EXPECT_EQ(restored["blockType"].toString(), juce::String("dualMono"));
  EXPECT_TRUE(static_cast<bool>(restored["loaded"]));
}

TEST(DualMonoBlockTest, RemovalTearsDownBothChildrenAtomically) {
  ChainTestProcessor proc;
  proc.setPlayConfigDetails(2, 2, kFs, kBlock);
  proc.prepareToPlay(kFs, kBlock);

  const std::string blockId = proc.addDualMonoBlock();
  ASSERT_FALSE(blockId.empty());

  const std::string leftId =
      proc.loadToneIntoDualSlot(blockId, /*isLeftSide=*/true,
                                juce::String("{\"id\":1,\"format\":\"nam\",\"models\":[{\"id\":10,"
                                             "\"name\":\"amp\",\"model_url\":\"https://test.invalid/"
                                             "amp.nam\"}]}"));
  ASSERT_FALSE(leftId.empty());
  const std::string rightId =
      proc.loadToneIntoDualSlot(blockId, /*isLeftSide=*/false,
                                juce::String("{\"id\":2,\"format\":\"ir\",\"models\":[{\"id\":20,"
                                             "\"name\":\"cab\",\"model_url\":\"https://test.invalid/"
                                             "cab.wav\"}]}"));
  ASSERT_FALSE(rightId.empty());

  const auto before = laneLayout(proc.getChainState(-1), "chain");
  const size_t beforeSize = before.size();

  ASSERT_TRUE(proc.removeChainBlock(blockId));

  EXPECT_TRUE(blockById(proc, blockId).isVoid());
  const auto after = laneLayout(proc.getChainState(-1), "chain");
  // Removing the wrapper drops one lane slot; a padded insert takes its
  // place, same shape as removing any other single block.
  EXPECT_EQ(after.size(), beforeSize);

  // The children were never top-level lane items (only reachable while
  // nested under the wrapper), so prove they're gone, not just detached and
  // leaked, via the same generic per-block lookup every setter routes
  // through (findBlockById): it must no longer resolve either id.
  auto* band = new juce::DynamicObject();
  band->setProperty("freqHz", 1000.0);
  band->setProperty("gainDb", 3.0);
  band->setProperty("q", 1.0);
  EXPECT_FALSE(proc.setBlockEqBand(leftId, 0, juce::var(band)))
      << "removing the wrapper left the left child still resolvable";
  EXPECT_FALSE(proc.setBlockEqBand(rightId, 0, juce::var(band)))
      << "removing the wrapper left the right child still resolvable";
}

TEST(DualMonoBlockTest, EmptySidesPassThroughDistinctChannelsUnchanged) {
  ChainTestProcessor proc;
  proc.setPlayConfigDetails(2, 2, kFs, kBlock);
  proc.prepareToPlay(kFs, kBlock);

  ASSERT_FALSE(proc.addDualMonoBlock().empty());
  processStereo(proc, makeNoise(kWarmupBlocks * kBlock, 1111, 0.25f));

  // Genuinely different content per channel (not processStereo's own
  // identical-both-channels convention) - proves seeding keeps dl/dr
  // distinct rather than accidentally duplicating one channel into both.
  const auto inL = makeNoise(20 * kBlock, 4242, 0.25f);
  const auto inR = makeNoise(20 * kBlock, 9999, 0.25f);

  juce::AudioBuffer<float> buffer(2, kBlock);
  juce::MidiBuffer midi;
  std::vector<float> outL(inL.size()), outR(inR.size());
  for (int off = 0; off + kBlock <= static_cast<int>(inL.size()); off += kBlock) {
    buffer.copyFrom(0, 0, inL.data() + off, kBlock);
    buffer.copyFrom(1, 0, inR.data() + off, kBlock);
    proc.processBlock(buffer, midi);
    std::copy(buffer.getReadPointer(0), buffer.getReadPointer(0) + kBlock, outL.begin() + off);
    std::copy(buffer.getReadPointer(1), buffer.getReadPointer(1) + kBlock, outR.begin() + off);
  }

  const float diffL = maxAbsDiff(outL, inL);
  const float diffR = maxAbsDiff(outR, inR);
  std::printf("[DualMonoBlockTest] empty pass-through max |diff|: L=%.6f R=%.6f\n",
             static_cast<double>(diffL), static_cast<double>(diffR));
  EXPECT_LT(diffL, 0.05f) << "an empty Dual Mono block should pass its left channel through "
                             "essentially unchanged";
  EXPECT_LT(diffR, 0.05f) << "an empty Dual Mono block should pass its right channel through "
                             "essentially unchanged, distinct from the left";
}

// Widen (mono mode, 2-channel buffer): each side's processed content should
// land in its own output channel. Proven by symmetry rather than an
// external reference chain (the full processBlock pipeline - DC blocker,
// balance/pan, output level - makes bit-exact comparison against an
// isolated single-block run fragile): swap which content sits on which
// side and confirm the output channels swap correspondingly, within the
// *same* run's pipeline both times.
TEST(DualMonoBlockTest, WidenCaseGivesEachSideItsOwnOutputChannel) {
  const auto in = makeNoise(20 * kBlock, 777, 0.25f);

  auto namTree = [] { return makeNamBlockTree("blk-nam", 1, 100); };
  auto irTree = [] { return makeIrBlockTree("blk-ir", 2, 200); };

  const auto runA =
      runLane(makeDualMonoBlockTree("blk-dual", namTree(), irTree()), in, /*monoRig=*/false);
  const auto runB =
      runLane(makeDualMonoBlockTree("blk-dual", irTree(), namTree()), in, /*monoRig=*/false);

  const float crossDiffNam = maxAbsDiff(runA.first, runB.second);   // NAM: A's L vs B's R
  const float crossDiffIr = maxAbsDiff(runA.second, runB.first);    // IR: A's R vs B's L
  const float withinRunDiff = maxAbsDiff(runA.first, runA.second);  // A's own L vs R

  std::printf(
      "[DualMonoBlockTest] widen cross-run diff (same content, swapped side): NAM=%.6f IR=%.6f; "
      "within-run L-vs-R diff: %.6f\n",
      static_cast<double>(crossDiffNam), static_cast<double>(crossDiffIr),
      static_cast<double>(withinRunDiff));

  EXPECT_LT(crossDiffNam, 1e-4f)
      << "the NAM side's content should land on whichever output channel it's assigned to";
  EXPECT_LT(crossDiffIr, 1e-4f)
      << "the IR side's content should land on whichever output channel it's assigned to";
  EXPECT_GT(withinRunDiff, 0.01f)
      << "left and right should carry genuinely different content, not the same signal twice";
}

// Fold (a genuinely mono host output, one physical channel): identical
// content on both sides must reduce to exactly what that content produces
// as a plain, unwrapped block - proving fold is a true average (averaging
// two identical signals is the identity), not some other blend.
TEST(DualMonoBlockTest, FoldWithIdenticalContentBothSidesEqualsPlainSingleBlock) {
  const auto in = makeNoise(20 * kBlock, 555, 0.25f);

  const auto dualRun = runLane(
      makeDualMonoBlockTree("blk-dual", makeNamBlockTree("blk-nam-l", 1, 100),
                            makeNamBlockTree("blk-nam-r", 1, 100)),
      in, /*monoRig=*/true);
  const auto plainRun = runLane(makeNamBlockTree("blk-nam", 1, 100), in, /*monoRig=*/true);

  const float diff = maxAbsDiff(dualRun.first, plainRun.first);
  std::printf("[DualMonoBlockTest] fold (identical both sides) vs plain block max |diff|: %.6f\n",
             static_cast<double>(diff));
  EXPECT_LT(diff, 1e-4f) << "folding identical content on both sides should reduce to exactly "
                            "the plain, unwrapped block's own output";
}

// Complements the identical-content case above: different content on each
// side must actually blend (fold is not silently favoring one side).
TEST(DualMonoBlockTest, FoldBlendsBothSidesNotJustOne) {
  const auto in = makeNoise(20 * kBlock, 555, 0.25f);

  const auto dualRun = runLane(
      makeDualMonoBlockTree("blk-dual", makeNamBlockTree("blk-nam", 1, 100),
                            makeIrBlockTree("blk-ir", 2, 200)),
      in, /*monoRig=*/true);
  const auto namOnlyRun = runLane(makeNamBlockTree("blk-nam", 1, 100), in, /*monoRig=*/true);
  const auto irOnlyRun = runLane(makeIrBlockTree("blk-ir", 2, 200), in, /*monoRig=*/true);

  const float diffFromNam = maxAbsDiff(dualRun.first, namOnlyRun.first);
  const float diffFromIr = maxAbsDiff(dualRun.first, irOnlyRun.first);
  std::printf(
      "[DualMonoBlockTest] fold (different sides) max |diff| from NAM-only=%.6f, from IR-only=%.6f\n",
      static_cast<double>(diffFromNam), static_cast<double>(diffFromIr));
  EXPECT_GT(diffFromNam, 0.01f) << "the right side's content had no measurable effect on the "
                                   "folded output";
  EXPECT_GT(diffFromIr, 0.01f) << "the left side's content had no measurable effect on the "
                                  "folded output";
}

// A live Pan L change must glide (JUCE's LinearSmoothedValue, reset to a
// fixed 50ms ramp at creation - see addDualMonoBlock/ChainBlock.h), never
// step. Constant, maximally-different content on the two input channels
// (via empty sides, which pass the raw input straight through as dl/dr)
// makes any instantaneous jump obvious: with the defaults (leftPan=0 hard
// left, rightPan=1 hard right, width=1), outL == dl exactly (see
// runDualMono's constant-power math at pan 0/1).
//
// A sustained constant value is DC, and the chain's DC blocker (~5 Hz HPF,
// downstream of the tone chain - see the signal-flow doc in CLAUDE.md)
// keeps decaying it toward 0 the whole time regardless of Pan, so the
// trial that changes Pan is compared against a `control` trial that runs
// the identical warmup/input but never calls setDualImage: both see
// exactly the same DC-blocker decay up to the moment of the change (same
// deterministic input, fresh instances), so any difference between them
// afterward isolates the pan change's own effect from that decay.
TEST(DualMonoBlockTest, PanChangeGlidesWithoutClicking) {
  auto runTrial = [](bool triggerPanChange) {
    ChainTestProcessor proc;
    proc.setPlayConfigDetails(2, 2, kFs, kBlock);
    proc.prepareToPlay(kFs, kBlock);
    const std::string blockId = proc.addDualMonoBlock();
    EXPECT_FALSE(blockId.empty());

    juce::AudioBuffer<float> buffer(2, kBlock);
    juce::MidiBuffer midi;
    auto runBlock = [&] {
      for (int i = 0; i < kBlock; ++i) {
        buffer.setSample(0, i, 1.0f);
        buffer.setSample(1, i, -1.0f);
      }
      proc.processBlock(buffer, midi);
      return std::vector<float>(buffer.getReadPointer(0), buffer.getReadPointer(0) + kBlock);
    };

    for (int i = 0; i < kWarmupBlocks; ++i) runBlock();
    if (triggerPanChange)
      EXPECT_TRUE(proc.setDualImage(blockId, /*left=*/1.0, /*right=*/1.0, /*width=*/1.0));
    return runBlock();  // the block right after the (possible) change
  };

  const auto withChange = runTrial(true);
  const auto control = runTrial(false);
  ASSERT_EQ(withChange.size(), control.size());

  // No single-sample jump should be anywhere near the full ±2 swing a step
  // would produce; a ~50ms/2400-sample linear ramp moves roughly 1/2400
  // (~0.0004) per sample on top of whatever the (smooth, continuous)
  // DC-blocker decay itself contributes.
  float maxStep = 0.0f;
  for (size_t i = 1; i < withChange.size(); ++i)
    maxStep = std::max(maxStep, std::abs(withChange[i] - withChange[i - 1]));
  std::printf("[DualMonoBlockTest] pan-change max single-sample step: %.6f\n",
             static_cast<double>(maxStep));
  EXPECT_LT(maxStep, 0.01f) << "Pan L stepped instead of gliding";

  // The change had a real, measurable effect once isolated from the shared
  // DC-blocker decay both trials otherwise experience identically.
  const float divergence = maxAbsDiff(withChange, control);
  std::printf("[DualMonoBlockTest] pan-change divergence from no-change control: %.6f\n",
             static_cast<double>(divergence));
  EXPECT_GT(divergence, 0.03f) << "setDualImage had no measurable effect";
}

TEST(DualMonoBlockTest, LiveDualImageDragCoalescesIntoOneUndoStep) {
  ChainTestProcessor proc;
  proc.setPlayConfigDetails(2, 2, kFs, kBlock);
  proc.prepareToPlay(kFs, kBlock);

  const std::string blockId = proc.addDualMonoBlock();
  ASSERT_FALSE(blockId.empty());

  // Simulates a drag: several ticks toward the same gesture.
  ASSERT_TRUE(proc.setDualImage(blockId, 0.3, 0.7, 0.5));
  ASSERT_TRUE(proc.setDualImage(blockId, 0.4, 0.8, 0.6));
  ASSERT_TRUE(proc.setDualImage(blockId, 0.5, 0.9, 0.8));

  const juce::var beforeUndo = blockById(proc, blockId);
  EXPECT_NEAR(static_cast<double>(beforeUndo["params"]["dualLeftPan"]), 0.5, 1e-6);

  ASSERT_TRUE(proc.undoChain());
  const juce::var afterUndo = blockById(proc, blockId);
  ASSERT_FALSE(afterUndo.isVoid()) << "one undo after a coalesced drag removed the block entirely";
  EXPECT_NEAR(static_cast<double>(afterUndo["params"]["dualLeftPan"]), 0.0, 1e-6)
      << "one undo after a coalesced 3-tick drag should revert all the way to the pre-drag value";
  EXPECT_NEAR(static_cast<double>(afterUndo["params"]["dualRightPan"]), 1.0, 1e-6);
  EXPECT_NEAR(static_cast<double>(afterUndo["params"]["dualWidth"]), 1.0, 1e-6);

  // One more undo should remove the block entirely (creation is its own,
  // separate history entry) - confirms the drag really was a single step on
  // top of creation, not three.
  ASSERT_TRUE(proc.undoChain());
  EXPECT_TRUE(blockById(proc, blockId).isVoid())
      << "the coalesced 3-tick drag should have consumed exactly one undo step past creation";
}

TEST(DualMonoBlockTest, LoadIntoSlotAndRemoveSlotContentAreStructurallyIndependentPerSide) {
  ChainTestProcessor proc;
  proc.setPlayConfigDetails(2, 2, kFs, kBlock);
  proc.prepareToPlay(kFs, kBlock);

  const std::string blockId = proc.addDualMonoBlock();
  ASSERT_FALSE(blockId.empty());

  const juce::String namJson(
      "{\"id\":1,\"format\":\"nam\",\"models\":[{\"id\":10,\"name\":\"amp\",\"model_url\":"
      "\"https://test.invalid/amp.nam\"}]}");
  const std::string leftId = proc.loadToneIntoDualSlot(blockId, /*isLeftSide=*/true, namJson);
  ASSERT_FALSE(leftId.empty());

  juce::var block = blockById(proc, blockId);
  ASSERT_FALSE(block.isVoid());
  {
    const auto* left = block["dualLeft"].getArray();
    ASSERT_NE(left, nullptr);
    ASSERT_EQ(left->size(), 1);
    EXPECT_EQ((*left)[0]["blockId"].toString(), juce::String(leftId));
    EXPECT_EQ((*left)[0]["blockType"].toString(), juce::String("nam"));
    const auto* right = block["dualRight"].getArray();
    ASSERT_NE(right, nullptr);
    EXPECT_TRUE(right->isEmpty()) << "loading into the left side must not touch the right";
  }

  EXPECT_TRUE(proc.removeDualSlotContent(blockId, /*isLeftSide=*/true));
  block = blockById(proc, blockId);
  EXPECT_TRUE(block["dualLeft"].getArray()->isEmpty());

  // A no-op remove on an already-empty side still reports success.
  EXPECT_TRUE(proc.removeDualSlotContent(blockId, /*isLeftSide=*/true));

  // Every entry point rejects a blockId that isn't a DUAL_MONO block.
  const std::string otherId = proc.addEqBlock();
  ASSERT_FALSE(otherId.empty());
  EXPECT_EQ(proc.loadToneIntoDualSlot(otherId, true, namJson), std::string());
  EXPECT_FALSE(proc.removeDualSlotContent(otherId, true));
  EXPECT_FALSE(proc.setDualImage(otherId, 0.2, 0.8, 0.5));

  EXPECT_EQ(proc.loadToneIntoDualSlot("not-a-real-id", true, namJson), std::string());
  EXPECT_FALSE(proc.removeDualSlotContent("not-a-real-id", true));
  EXPECT_FALSE(proc.setDualImage("not-a-real-id", 0.2, 0.8, 0.5));
}

// findBlockById's recursion into dualLeft/dualRight (needed for async load
// completion to re-locate a child by id) also means every other generic
// per-block setter transparently reaches a dual child - proven here with
// setBlockEqBand, the same generic per-block API eq_block_tests.cpp drives
// directly against a top-level block.
TEST(DualMonoBlockTest, SetBlockEqBandWorksOnADualChildViaRecursiveLookup) {
  ChainTestProcessor proc;
  proc.setPlayConfigDetails(2, 2, kFs, kBlock);
  proc.prepareToPlay(kFs, kBlock);

  juce::ValueTree state("ChainSnapshot");
  juce::ValueTree left("ChainBlocks");
  left.appendChild(
      makeDualMonoBlockTree("blk-dual", makeNamBlockTree("blk-nam", 1, 100), {}), nullptr);
  state.appendChild(left, nullptr);
  proc.restoreFromTree(state);
  ASSERT_TRUE(waitForDualMonoLoaded(proc));

  auto* band = new juce::DynamicObject();
  band->setProperty("freqHz", 1500.0);
  band->setProperty("gainDb", 12.0);
  band->setProperty("q", 1.2);
  EXPECT_TRUE(proc.setBlockEqBand("blk-nam", 2, juce::var(band)));

  const juce::var block = blockById(proc, "blk-dual");
  ASSERT_FALSE(block.isVoid());
  const auto* leftArr = block["dualLeft"].getArray();
  ASSERT_NE(leftArr, nullptr);
  ASSERT_EQ(leftArr->size(), 1);
  const juce::var band2 = (*leftArr)[0]["params"]["eq"]["bands"][2];
  EXPECT_NEAR(static_cast<double>(band2["gainDb"]), 12.0, 1e-6)
      << "setBlockEqBand didn't reach the dual child - findBlockById's recursion regressed";
}

TEST(DualMonoBlockTest, PersistsThroughStateRestoreWithSidesAndImageIntact) {
  ChainTestProcessor proc;
  proc.setPlayConfigDetails(2, 2, kFs, kBlock);
  proc.prepareToPlay(kFs, kBlock);

  juce::ValueTree state("ChainSnapshot");
  juce::ValueTree left("ChainBlocks");
  left.appendChild(makeDualMonoBlockTree("blk-dual", makeNamBlockTree("blk-nam", 1, 100),
                                         makeIrBlockTree("blk-ir", 2, 200), 0.25f, 0.75f, 0.6f),
                   nullptr);
  state.appendChild(left, nullptr);
  proc.restoreFromTree(state);
  ASSERT_TRUE(waitForDualMonoLoaded(proc));

  juce::MemoryBlock savedState;
  proc.getStateInformation(savedState);

  ChainTestProcessor restored;
  restored.setPlayConfigDetails(2, 2, kFs, kBlock);
  restored.prepareToPlay(kFs, kBlock);
  restored.setStateInformation(savedState.getData(), static_cast<int>(savedState.getSize()));
  ASSERT_TRUE(waitForDualMonoLoaded(restored));

  const juce::var block = blockById(restored, "blk-dual");
  ASSERT_FALSE(block.isVoid()) << "Dual Mono block didn't survive a state round trip";
  EXPECT_EQ(block["blockType"].toString(), juce::String("dualMono"));
  EXPECT_TRUE(static_cast<bool>(block["loaded"]));
  EXPECT_NEAR(static_cast<double>(block["params"]["dualLeftPan"]), 0.25, 1e-6);
  EXPECT_NEAR(static_cast<double>(block["params"]["dualRightPan"]), 0.75, 1e-6);
  EXPECT_NEAR(static_cast<double>(block["params"]["dualWidth"]), 0.6, 1e-6);

  const auto* leftArr = block["dualLeft"].getArray();
  ASSERT_NE(leftArr, nullptr);
  ASSERT_EQ(leftArr->size(), 1);
  EXPECT_EQ((*leftArr)[0]["blockId"].toString(), juce::String("blk-nam"));
  EXPECT_EQ((*leftArr)[0]["blockType"].toString(), juce::String("nam"));
  EXPECT_TRUE(static_cast<bool>((*leftArr)[0]["loaded"]));

  const auto* rightArr = block["dualRight"].getArray();
  ASSERT_NE(rightArr, nullptr);
  ASSERT_EQ(rightArr->size(), 1);
  EXPECT_EQ((*rightArr)[0]["blockId"].toString(), juce::String("blk-ir"));
  EXPECT_EQ((*rightArr)[0]["blockType"].toString(), juce::String("ir"));
  EXPECT_TRUE(static_cast<bool>((*rightArr)[0]["loaded"]));
}

TEST(DualMonoBlockTest, LinkPersistsThroughStateRestore) {
  ChainTestProcessor proc;
  proc.setPlayConfigDetails(2, 2, kFs, kBlock);
  proc.prepareToPlay(kFs, kBlock);

  const std::string blockId = proc.addDualMonoBlock();
  ASSERT_FALSE(blockId.empty());
  EXPECT_FALSE(static_cast<bool>(blockById(proc, blockId)["params"]["dualLinked"]))
      << "Link should default off";

  ASSERT_TRUE(proc.setDualLinked(blockId, true));
  EXPECT_TRUE(static_cast<bool>(blockById(proc, blockId)["params"]["dualLinked"]));

  juce::MemoryBlock savedState;
  proc.getStateInformation(savedState);

  ChainTestProcessor restored;
  restored.setPlayConfigDetails(2, 2, kFs, kBlock);
  restored.prepareToPlay(kFs, kBlock);
  restored.setStateInformation(savedState.getData(), static_cast<int>(savedState.getSize()));
  ASSERT_TRUE(waitForDualMonoLoaded(restored));

  EXPECT_TRUE(static_cast<bool>(blockById(restored, blockId)["params"]["dualLinked"]))
      << "Link didn't survive a state round trip";
}

TEST(DualMonoBlockTest, SoloIsExclusiveBetweenSides) {
  ChainTestProcessor proc;
  proc.setPlayConfigDetails(2, 2, kFs, kBlock);
  proc.prepareToPlay(kFs, kBlock);

  const std::string blockId = proc.addDualMonoBlock();
  ASSERT_FALSE(blockId.empty());

  ASSERT_TRUE(proc.setDualSolo(blockId, /*isLeftSide=*/true, true));
  {
    const juce::var block = blockById(proc, blockId);
    EXPECT_TRUE(static_cast<bool>(block["params"]["dualSoloLeft"]));
    EXPECT_FALSE(static_cast<bool>(block["params"]["dualSoloRight"]));
  }

  // Soloing the other side flips exclusively - the first side's solo clears.
  ASSERT_TRUE(proc.setDualSolo(blockId, /*isLeftSide=*/false, true));
  {
    const juce::var block = blockById(proc, blockId);
    EXPECT_FALSE(static_cast<bool>(block["params"]["dualSoloLeft"]))
        << "soloing Right didn't clear an existing Left solo";
    EXPECT_TRUE(static_cast<bool>(block["params"]["dualSoloRight"]));
  }

  ASSERT_TRUE(proc.setDualSolo(blockId, /*isLeftSide=*/false, false));
  {
    const juce::var block = blockById(proc, blockId);
    EXPECT_FALSE(static_cast<bool>(block["params"]["dualSoloLeft"]));
    EXPECT_FALSE(static_cast<bool>(block["params"]["dualSoloRight"]));
  }

  EXPECT_FALSE(proc.setDualSolo("not-a-real-id", true, true));
}

// Solo silences the *other* side in the recombined output: with Left
// soloed, the Right child's own content should no longer be audible in
// either output channel (widen case - each side otherwise gets its own
// channel, see WidenCaseGivesEachSideItsOwnOutputChannel).
TEST(DualMonoBlockTest, SoloedSideSilencesTheOtherInWidenOutput) {
  const auto in = makeNoise(20 * kBlock, 555, 0.25f);

  auto namTree = [] { return makeNamBlockTree("blk-nam", 1, 100); };
  auto irTree = [] { return makeIrBlockTree("blk-ir", 2, 200); };

  ChainTestProcessor proc;
  proc.setPlayConfigDetails(2, 2, kFs, kBlock);
  proc.prepareToPlay(kFs, kBlock);

  juce::ValueTree state("ChainSnapshot");
  juce::ValueTree left("ChainBlocks");
  left.appendChild(makeDualMonoBlockTree("blk-dual", namTree(), irTree()), nullptr);
  state.appendChild(left, nullptr);
  proc.restoreFromTree(state);
  ASSERT_TRUE(waitForDualMonoLoaded(proc));

  ASSERT_TRUE(proc.setDualSolo("blk-dual", /*isLeftSide=*/true, true));

  // Long warmup lets the solo gain smoother fully settle before measuring.
  processStereo(proc, makeNoise(kWarmupBlocks * 4 * kBlock, 1111, 0.25f));
  const auto out = processStereo(proc, in);

  float peakRight = 0.0f;
  for (float sample : out.second) peakRight = std::max(peakRight, std::abs(sample));
  std::printf("[DualMonoBlockTest] Right channel peak with Left soloed: %.6f\n",
             static_cast<double>(peakRight));
  EXPECT_LT(peakRight, 1e-4f) << "Left solo should silence the Right side's own output channel";
}

TEST(DualMonoBlockTest, DualMutePersistsThroughStateRestore) {
  ChainTestProcessor proc;
  proc.setPlayConfigDetails(2, 2, kFs, kBlock);
  proc.prepareToPlay(kFs, kBlock);

  const std::string blockId = proc.addDualMonoBlock();
  ASSERT_FALSE(blockId.empty());
  EXPECT_FALSE(static_cast<bool>(blockById(proc, blockId)["params"]["dualLeftMuted"]))
      << "mute should default off";

  ASSERT_TRUE(proc.setDualMuted(blockId, /*isLeftSide=*/true, true));
  EXPECT_TRUE(static_cast<bool>(blockById(proc, blockId)["params"]["dualLeftMuted"]));
  EXPECT_FALSE(static_cast<bool>(blockById(proc, blockId)["params"]["dualRightMuted"]))
      << "mute is independent per side, unlike Solo's exclusivity";

  juce::MemoryBlock savedState;
  proc.getStateInformation(savedState);

  ChainTestProcessor restored;
  restored.setPlayConfigDetails(2, 2, kFs, kBlock);
  restored.prepareToPlay(kFs, kBlock);
  restored.setStateInformation(savedState.getData(), static_cast<int>(savedState.getSize()));
  ASSERT_TRUE(waitForDualMonoLoaded(restored));

  EXPECT_TRUE(static_cast<bool>(blockById(restored, blockId)["params"]["dualLeftMuted"]))
      << "mute didn't survive a state round trip";

  EXPECT_FALSE(proc.setDualMuted("not-a-real-id", true, true));
}

// An empty side is normally a live pass-through (EmptySidesPassThroughDistinct
// ChannelsUnchanged) - muting it should silence that channel instead, and
// leave the *other*, still-unmuted empty side passing through as before.
TEST(DualMonoBlockTest, EmptySideMuteSilencesOnlyThatSidesPassThrough) {
  ChainTestProcessor proc;
  proc.setPlayConfigDetails(2, 2, kFs, kBlock);
  proc.prepareToPlay(kFs, kBlock);

  const std::string blockId = proc.addDualMonoBlock();
  ASSERT_FALSE(blockId.empty());
  ASSERT_TRUE(proc.setDualMuted(blockId, /*isLeftSide=*/true, true));

  processStereo(proc, makeNoise(kWarmupBlocks * kBlock, 1111, 0.25f));

  const auto inL = makeNoise(20 * kBlock, 4242, 0.25f);
  const auto inR = makeNoise(20 * kBlock, 9999, 0.25f);
  juce::AudioBuffer<float> buffer(2, kBlock);
  juce::MidiBuffer midi;
  std::vector<float> outL(inL.size()), outR(inR.size());
  for (int off = 0; off + kBlock <= static_cast<int>(inL.size()); off += kBlock) {
    buffer.copyFrom(0, 0, inL.data() + off, kBlock);
    buffer.copyFrom(1, 0, inR.data() + off, kBlock);
    proc.processBlock(buffer, midi);
    std::copy(buffer.getReadPointer(0), buffer.getReadPointer(0) + kBlock, outL.begin() + off);
    std::copy(buffer.getReadPointer(1), buffer.getReadPointer(1) + kBlock, outR.begin() + off);
  }

  float peakL = 0.0f;
  for (float sample : outL) peakL = std::max(peakL, std::abs(sample));
  const float diffR = maxAbsDiff(outR, inR);
  std::printf(
      "[DualMonoBlockTest] muted-empty-Left peak: %.6f; still-live-empty-Right pass-through max "
      "|diff|: %.6f\n",
      static_cast<double>(peakL), static_cast<double>(diffR));
  EXPECT_LT(peakL, 1e-3f) << "muted empty Left should no longer pass its input through";
  EXPECT_LT(diffR, 0.05f) << "the still-unmuted empty Right side should keep passing through";
}

namespace {
// A strongly shaped band, well clear of "inert" (~0 dB) - same shape as
// eq_post_routing_tests.cpp's own shapedBand().
juce::var shapedMasterBand() {
  auto* band = new juce::DynamicObject();
  band->setProperty("freqHz", 1500.0);
  band->setProperty("gainDb", 12.0);
  band->setProperty("q", 1.2);
  return juce::var(band);
}

// Both sides loaded with distinct content (widen case, mono mode) and run
// through the processor - `configure` runs after restore/load but before
// warmup, so callers can touch the wrapper's own master EQ (or leave it
// alone) before the comparison window starts.
std::pair<std::vector<float>, std::vector<float>> runDualWidenWithMasterEq(
    const std::vector<float>& in, const std::function<void(ChainTestProcessor&)>& configure) {
  ChainTestProcessor proc;
  proc.setPlayConfigDetails(2, 2, kFs, kBlock);
  proc.prepareToPlay(kFs, kBlock);

  juce::ValueTree state("ChainSnapshot");
  juce::ValueTree left("ChainBlocks");
  left.appendChild(
      makeDualMonoBlockTree("blk-dual", makeNamBlockTree("blk-nam", 1, 100),
                            makeIrBlockTree("blk-ir", 2, 200)),
      nullptr);
  state.appendChild(left, nullptr);
  proc.restoreFromTree(state);
  EXPECT_TRUE(waitForDualMonoLoaded(proc));

  configure(proc);

  processStereo(proc, makeNoise(kWarmupBlocks * kBlock, 1111, 0.25f));
  return processStereo(proc, in);
}
}  // namespace

// Proves the master EQ's own gate: leaving every band flat (the default)
// must be indistinguishable from explicitly disabling the EQ outright -
// isActive() (enabled && anyBandActive) should already treat flat bands as
// a no-op, same guarantee eq_post_routing_tests.cpp pins for an ordinary
// block's own EQ.
TEST(DualMonoBlockTest, MasterEqDefaultsToFlatAndIsInert) {
  const auto in = makeNoise(20 * kBlock, 777, 0.25f);

  const auto flatDefault = runDualWidenWithMasterEq(in, [](ChainTestProcessor&) {});
  const auto explicitlyDisabled = runDualWidenWithMasterEq(in, [](ChainTestProcessor& proc) {
    ASSERT_TRUE(proc.setBlockEqEnabled("blk-dual", false));
  });

  const float diff = std::max(maxAbsDiff(flatDefault.first, explicitlyDisabled.first),
                              maxAbsDiff(flatDefault.second, explicitlyDisabled.second));
  std::printf("[DualMonoBlockTest] master EQ flat-default vs explicitly-disabled max |diff|: %.9f\n",
             static_cast<double>(diff));
  EXPECT_LT(diff, 1e-4f) << "a flat (untouched) master EQ should already be a no-op, same as "
                            "explicitly disabling it";
}

// Companion to the test above: proves a shaped band actually does something,
// so a bug that made the master EQ a global no-op couldn't pass
// MasterEqDefaultsToFlatAndIsInert vacuously.
TEST(DualMonoBlockTest, MasterEqShapesTheRecombinedOutput) {
  const auto in = makeNoise(20 * kBlock, 777, 0.25f);

  const auto flat = runDualWidenWithMasterEq(in, [](ChainTestProcessor&) {});
  const auto shaped = runDualWidenWithMasterEq(in, [](ChainTestProcessor& proc) {
    ASSERT_TRUE(proc.setBlockEqBand("blk-dual", 2, shapedMasterBand()));
  });

  const float diff = std::max(maxAbsDiff(flat.first, shaped.first),
                              maxAbsDiff(flat.second, shaped.second));
  std::printf("[DualMonoBlockTest] master EQ flat vs shaped max |diff|: %.6f\n",
             static_cast<double>(diff));
  EXPECT_GT(diff, 1e-3f) << "a shaped master EQ band had no measurable effect on the recombined "
                            "output";
}

TEST(DualMonoBlockTest, MasterEqSurvivesStateRestore) {
  ChainTestProcessor proc;
  proc.setPlayConfigDetails(2, 2, kFs, kBlock);
  proc.prepareToPlay(kFs, kBlock);

  const std::string blockId = proc.addDualMonoBlock();
  ASSERT_FALSE(blockId.empty());
  ASSERT_TRUE(proc.setBlockEqBand(blockId, 2, shapedMasterBand()));
  ASSERT_TRUE(proc.setBlockEqEnabled(blockId, true));

  juce::MemoryBlock savedState;
  proc.getStateInformation(savedState);

  ChainTestProcessor restored;
  restored.setPlayConfigDetails(2, 2, kFs, kBlock);
  restored.prepareToPlay(kFs, kBlock);
  restored.setStateInformation(savedState.getData(), static_cast<int>(savedState.getSize()));
  ASSERT_TRUE(waitForDualMonoLoaded(restored));

  const juce::var block = blockById(restored, blockId);
  ASSERT_FALSE(block.isVoid());
  const juce::var band2 = block["params"]["eq"]["bands"][2];
  EXPECT_NEAR(static_cast<double>(band2["gainDb"]), 12.0, 1e-6)
      << "the master EQ's own band didn't survive a state round trip";
  EXPECT_TRUE(static_cast<bool>(block["params"]["eq"]["enabled"]));
}

// The master EQ's graph view backdrop (SpectrumBackdrop.tsx) reads
// getBlockSpectrum(blockId) generically - already wired for any blockId,
// wrapper included. The one piece that needed adding was runDualMono's own
// pushSamples call (mirroring processChainOnBuffer's end-of-loop feed);
// this proves it's actually happening now, not just that the plumbing
// exists on paper.
TEST(DualMonoBlockTest, MasterEqSpectrumAnalyzerReceivesSamplesWhenEnabled) {
  ChainTestProcessor proc;
  proc.setPlayConfigDetails(2, 2, kFs, kBlock);
  proc.prepareToPlay(kFs, kBlock);

  juce::ValueTree state("ChainSnapshot");
  juce::ValueTree left("ChainBlocks");
  left.appendChild(
      makeDualMonoBlockTree("blk-dual", makeNamBlockTree("blk-nam", 1, 100),
                            makeIrBlockTree("blk-ir", 2, 200)),
      nullptr);
  state.appendChild(left, nullptr);
  proc.restoreFromTree(state);
  ASSERT_TRUE(waitForDualMonoLoaded(proc));

  ASSERT_TRUE(proc.setBlockSpectrumEnabled("blk-dual", true));
  processStereo(proc, makeNoise(kWarmupBlocks * kBlock, 1111, 0.25f));
  processStereo(proc, makeNoise(4 * kBlock, 4242, 0.4f));

  const juce::var spectrum = proc.getBlockSpectrum("blk-dual");
  const auto* bins = spectrum.getArray();
  ASSERT_NE(bins, nullptr);
  EXPECT_EQ(bins->size(), 64);

  float maxDb = -1000.0f;
  for (const auto& bin : *bins) maxDb = std::max(maxDb, static_cast<float>(bin));
  std::printf("[DualMonoBlockTest] master EQ spectrum max bin: %.1f dB\n",
             static_cast<double>(maxDb));
  EXPECT_GT(maxDb, -100.0f)
      << "spectrum analyzer never rose above its silence floor - runDualMono isn't feeding it";
}

TEST(DualMonoBlockTest, MasterEqSpectrumAnalyzerStaysAtFloorWhenDisabled) {
  ChainTestProcessor proc;
  proc.setPlayConfigDetails(2, 2, kFs, kBlock);
  proc.prepareToPlay(kFs, kBlock);

  juce::ValueTree state("ChainSnapshot");
  juce::ValueTree left("ChainBlocks");
  left.appendChild(
      makeDualMonoBlockTree("blk-dual", makeNamBlockTree("blk-nam", 1, 100),
                            makeIrBlockTree("blk-ir", 2, 200)),
      nullptr);
  state.appendChild(left, nullptr);
  proc.restoreFromTree(state);
  ASSERT_TRUE(waitForDualMonoLoaded(proc));

  // Never calls setBlockSpectrumEnabled - matches the UI's own default
  // (only enabled while that EQ view is actually open).
  processStereo(proc, makeNoise(kWarmupBlocks * kBlock, 1111, 0.25f));
  processStereo(proc, makeNoise(4 * kBlock, 4242, 0.4f));

  const juce::var spectrum = proc.getBlockSpectrum("blk-dual");
  const auto* bins = spectrum.getArray();
  ASSERT_NE(bins, nullptr);
  for (const auto& bin : *bins)
    EXPECT_LE(static_cast<float>(bin), -99.9f)
        << "spectrum has real content despite never being enabled - pushSamples should be gated";
}

namespace {
// Same shape as runDualWidenWithMasterEq - both sides loaded with distinct
// content (widen case, mono mode), `configure` runs after load but before
// the comparison window starts.
std::pair<std::vector<float>, std::vector<float>> runDualWidenWithAlign(
    const std::vector<float>& in, const std::function<void(ChainTestProcessor&)>& configure) {
  ChainTestProcessor proc;
  proc.setPlayConfigDetails(2, 2, kFs, kBlock);
  proc.prepareToPlay(kFs, kBlock);

  juce::ValueTree state("ChainSnapshot");
  juce::ValueTree left("ChainBlocks");
  left.appendChild(
      makeDualMonoBlockTree("blk-dual", makeNamBlockTree("blk-nam", 1, 100),
                            makeIrBlockTree("blk-ir", 2, 200)),
      nullptr);
  state.appendChild(left, nullptr);
  proc.restoreFromTree(state);
  EXPECT_TRUE(waitForDualMonoLoaded(proc));

  configure(proc);

  processStereo(proc, makeNoise(kWarmupBlocks * kBlock, 1111, 0.25f));
  return processStereo(proc, in);
}
}  // namespace

// Proves Align's power switch: calling setDualAlign with engaged=false must
// be indistinguishable from never touching Align at all, even with the
// knobs dialed far from center/default - StereoOffset::setTarget's own
// "idle and staying idle" early-return (StereoOffset.cpp) should make this
// exact, not approximate.
TEST(DualMonoBlockTest, DualAlignDefaultsToOffAndIsInert) {
  const auto in = makeNoise(20 * kBlock, 777, 0.25f);

  const auto untouched = runDualWidenWithAlign(in, [](ChainTestProcessor&) {});
  const auto explicitlyDisabled = runDualWidenWithAlign(in, [](ChainTestProcessor& proc) {
    ASSERT_TRUE(proc.setDualAlign("blk-dual", false, 0.9, 0.5, true, 0.8, true, true));
  });

  const float diff = std::max(maxAbsDiff(untouched.first, explicitlyDisabled.first),
                              maxAbsDiff(untouched.second, explicitlyDisabled.second));
  std::printf(
      "[DualMonoBlockTest] align untouched vs explicitly-disabled-but-dialed-in max |diff|: "
      "%.9f\n",
      static_cast<double>(diff));
  EXPECT_LT(diff, 1e-4f) << "Align should be a no-op whenever engaged is false, regardless of "
                            "the dialed-in knob values";
}

// Companion to the test above: proves a real offset actually does something,
// so a bug that made Align a global no-op couldn't pass
// DualAlignDefaultsToOffAndIsInert vacuously.
TEST(DualMonoBlockTest, DualAlignDelaysOneSideWhenEnabled) {
  const auto in = makeNoise(20 * kBlock, 777, 0.25f);

  const auto off = runDualWidenWithAlign(in, [](ChainTestProcessor&) {});
  const auto on = runDualWidenWithAlign(in, [](ChainTestProcessor& proc) {
    // offsetNormalized 0.9 -> ~19.2 ms delay on one side (kMaxOffsetMs=24),
    // deck (wobble/crossover/diffuse) left off - a pure corrective delay.
    ASSERT_TRUE(proc.setDualAlign("blk-dual", true, 0.9, 0.0, false, 0.5, false, false));
  });

  const float diff = std::max(maxAbsDiff(off.first, on.first), maxAbsDiff(off.second, on.second));
  std::printf("[DualMonoBlockTest] align off vs enabled (~19ms offset) max |diff|: %.6f\n",
             static_cast<double>(diff));
  EXPECT_GT(diff, 1e-3f) << "enabling Align with a real offset had no measurable effect on the "
                            "recombined output";
}

TEST(DualMonoBlockTest, DualAlignSurvivesStateRestore) {
  ChainTestProcessor proc;
  proc.setPlayConfigDetails(2, 2, kFs, kBlock);
  proc.prepareToPlay(kFs, kBlock);

  const std::string blockId = proc.addDualMonoBlock();
  ASSERT_FALSE(blockId.empty());
  ASSERT_TRUE(proc.setDualAlign(blockId, true, 0.75, 0.4, true, 0.6, true, true));

  juce::MemoryBlock savedState;
  proc.getStateInformation(savedState);

  ChainTestProcessor restored;
  restored.setPlayConfigDetails(2, 2, kFs, kBlock);
  restored.prepareToPlay(kFs, kBlock);
  restored.setStateInformation(savedState.getData(), static_cast<int>(savedState.getSize()));
  ASSERT_TRUE(waitForDualMonoLoaded(restored));

  const juce::var block = blockById(restored, blockId);
  ASSERT_FALSE(block.isVoid());
  const juce::var params = block["params"];
  EXPECT_TRUE(static_cast<bool>(params["dualAlignEnabled"]))
      << "Align's power switch didn't survive a state round trip";
  EXPECT_NEAR(static_cast<double>(params["dualAlignOffset"]), 0.75, 1e-6);
  EXPECT_NEAR(static_cast<double>(params["dualAlignWobble"]), 0.4, 1e-6);
  EXPECT_TRUE(static_cast<bool>(params["dualAlignWobbleEnabled"]));
  EXPECT_NEAR(static_cast<double>(params["dualAlignCrossover"]), 0.6, 1e-6);
  EXPECT_TRUE(static_cast<bool>(params["dualAlignCrossoverEnabled"]));
  EXPECT_TRUE(static_cast<bool>(params["dualAlignDiffuseEnabled"]));
}

// Reuses FoldWithIdenticalContentBothSidesEqualsPlainSingleBlock's own
// "identical content on both sides" setup (same model both sides, a
// genuinely mono host output, so the block folds to mono): with no invert,
// l == r and the fold reduces to the plain block (already pinned by that
// test). Flipping just the right side's polarity makes it l + (-l) = 0 -
// proves Ø is real signal-domain inversion, not just a persisted flag, and
// (via the existing test above) that it doesn't fire without being asked.
TEST(DualMonoBlockTest, DualInvertOnIdenticalContentCancelsInFold) {
  const auto in = makeNoise(20 * kBlock, 555, 0.25f);

  ChainTestProcessor proc;
  proc.setPlayConfigDetails(2, 1, kFs, kBlock);
  proc.prepareToPlay(kFs, kBlock);

  juce::ValueTree state("ChainSnapshot");
  juce::ValueTree lane("ChainBlocks");
  lane.appendChild(
      makeDualMonoBlockTree("blk-dual", makeNamBlockTree("blk-nam-l", 1, 100),
                            makeNamBlockTree("blk-nam-r", 1, 100)),
      nullptr);
  state.appendChild(lane, nullptr);
  proc.restoreFromTree(state);
  ASSERT_TRUE(waitForDualMonoLoaded(proc));

  ASSERT_TRUE(proc.setDualInvert("blk-dual", false, true));  // invert the right side only

  auto driveMono = [&](const std::vector<float>& signal) {
    const int total = static_cast<int>(signal.size());
    std::vector<float> out(signal.size(), 0.0f);
    juce::AudioBuffer<float> buffer(1, kBlock);
    juce::MidiBuffer midi;
    for (int off = 0; off + kBlock <= total; off += kBlock) {
      buffer.copyFrom(0, 0, signal.data() + off, kBlock);
      proc.processBlock(buffer, midi);
      std::copy(buffer.getReadPointer(0), buffer.getReadPointer(0) + kBlock, out.begin() + off);
    }
    return out;
  };

  driveMono(makeNoise(kWarmupBlocks * kBlock, 1111, 0.25f));
  const auto out = driveMono(in);

  float peak = 0.0f;
  for (float sample : out) peak = std::max(peak, std::abs(sample));
  std::printf(
      "[DualMonoBlockTest] fold, identical content, right side inverted, peak: %.6f\n",
      static_cast<double>(peak));
  EXPECT_LT(peak, 1e-3f)
      << "identical content with one side inverted should nearly cancel in the fold";
}

TEST(DualMonoBlockTest, DualInvertSurvivesStateRestore) {
  ChainTestProcessor proc;
  proc.setPlayConfigDetails(2, 2, kFs, kBlock);
  proc.prepareToPlay(kFs, kBlock);

  const std::string blockId = proc.addDualMonoBlock();
  ASSERT_FALSE(blockId.empty());
  ASSERT_TRUE(proc.setDualInvert(blockId, true, true));

  juce::MemoryBlock savedState;
  proc.getStateInformation(savedState);

  ChainTestProcessor restored;
  restored.setPlayConfigDetails(2, 2, kFs, kBlock);
  restored.prepareToPlay(kFs, kBlock);
  restored.setStateInformation(savedState.getData(), static_cast<int>(savedState.getSize()));
  ASSERT_TRUE(waitForDualMonoLoaded(restored));

  const juce::var block = blockById(restored, blockId);
  ASSERT_FALSE(block.isVoid());
  const juce::var params = block["params"];
  EXPECT_TRUE(static_cast<bool>(params["dualLeftInvert"]))
      << "Left invert didn't survive a state round trip";
  EXPECT_FALSE(static_cast<bool>(params["dualRightInvert"]));
}

// The Stereo Processing screen's goniometer reads getDualGoniometer(blockId)
// generically by blockId, gated the same way the EQ spectrum analyzer is
// (setDualGoniometerEnabled, only while that view is actually open). Proves
// runDualMono's own pushSamples call actually happens, not just that the
// plumbing exists on paper - same shape as
// MasterEqSpectrumAnalyzerReceivesSamplesWhenEnabled.
TEST(DualMonoBlockTest, DualGoniometerReceivesPointsWhenEnabled) {
  ChainTestProcessor proc;
  proc.setPlayConfigDetails(2, 2, kFs, kBlock);
  proc.prepareToPlay(kFs, kBlock);

  juce::ValueTree state("ChainSnapshot");
  juce::ValueTree left("ChainBlocks");
  left.appendChild(
      makeDualMonoBlockTree("blk-dual", makeNamBlockTree("blk-nam", 1, 100),
                            makeIrBlockTree("blk-ir", 2, 200)),
      nullptr);
  state.appendChild(left, nullptr);
  proc.restoreFromTree(state);
  ASSERT_TRUE(waitForDualMonoLoaded(proc));

  ASSERT_TRUE(proc.setDualGoniometerEnabled("blk-dual", true));
  processStereo(proc, makeNoise(kWarmupBlocks * kBlock, 1111, 0.25f));
  processStereo(proc, makeNoise(4 * kBlock, 4242, 0.4f));

  const juce::var points = proc.getDualGoniometer("blk-dual");
  const auto* flat = points.getArray();
  ASSERT_NE(flat, nullptr);
  ASSERT_GT(flat->size(), 0);
  EXPECT_EQ(flat->size() % 2, 0) << "points should come back as flat [l, r] pairs";

  float peak = 0.0f;
  for (const auto& v : *flat) peak = std::max(peak, std::abs(static_cast<float>(v)));
  std::printf("[DualMonoBlockTest] goniometer point count: %d, peak |value|: %.4f\n",
             flat->size(), static_cast<double>(peak));
  EXPECT_GT(peak, 0.0f) << "goniometer never received any real signal - pushSamples isn't happening";
}

TEST(DualMonoBlockTest, DualGoniometerStaysEmptyWhenDisabled) {
  ChainTestProcessor proc;
  proc.setPlayConfigDetails(2, 2, kFs, kBlock);
  proc.prepareToPlay(kFs, kBlock);

  juce::ValueTree state("ChainSnapshot");
  juce::ValueTree left("ChainBlocks");
  left.appendChild(
      makeDualMonoBlockTree("blk-dual", makeNamBlockTree("blk-nam", 1, 100),
                            makeIrBlockTree("blk-ir", 2, 200)),
      nullptr);
  state.appendChild(left, nullptr);
  proc.restoreFromTree(state);
  ASSERT_TRUE(waitForDualMonoLoaded(proc));

  // Never calls setDualGoniometerEnabled - matches the UI's own default
  // (only enabled while the Stereo Processing view is actually open).
  processStereo(proc, makeNoise(kWarmupBlocks * kBlock, 1111, 0.25f));
  processStereo(proc, makeNoise(4 * kBlock, 4242, 0.4f));

  const juce::var points = proc.getDualGoniometer("blk-dual");
  const auto* flat = points.getArray();
  ASSERT_NE(flat, nullptr);
  EXPECT_EQ(flat->size(), 0)
      << "goniometer has real content despite never being enabled - pushSamples should be gated";
}

// Regression for two real bugs: (1) the correlation readout used to be
// dualAlign.correlation() (StereoOffset's own meter), which only updates
// while Align's engine is actively processing - so leaving Offset centered
// and Wobble/Crossover/Diffuse off (Align fully disengaged) froze the
// reading at whatever it last was; (2) it used to capture pre-Pan/Width,
// missing what the block's own knobs actually did to the output - now
// captured post-everything (mono chain mode -> widen, so the block's own
// default hard-left/hard-right Pan already gives the output two genuinely
// distinct channels to correlate, same as a listener would hear). Proves
// the goniometer's own correlation stays live with Align completely
// untouched, and reflects a live Ø flip in the block's real output.
TEST(DualMonoBlockTest, DualGoniometerCorrelationReflectsInvertEvenWithAlignOff) {
  ChainTestProcessor proc;
  proc.setPlayConfigDetails(2, 2, kFs, kBlock);
  proc.prepareToPlay(kFs, kBlock);

  // No stereoEnabled: mono chain mode, so the Dual Mono block widens (two
  // physical output channels) rather than folding to one.
  juce::ValueTree state("ChainSnapshot");
  juce::ValueTree left("ChainBlocks");
  left.appendChild(
      makeDualMonoBlockTree("blk-dual", makeNamBlockTree("blk-nam-l", 1, 100),
                            makeNamBlockTree("blk-nam-r", 1, 100)),
      nullptr);
  state.appendChild(left, nullptr);
  proc.restoreFromTree(state);
  ASSERT_TRUE(waitForDualMonoLoaded(proc));

  ASSERT_TRUE(proc.setDualGoniometerEnabled("blk-dual", true));
  // Align never touched - stays fully disengaged (offset centered, deck off).
  processStereo(proc, makeNoise(kWarmupBlocks * kBlock, 1111, 0.25f));
  processStereo(proc, makeNoise(4 * kBlock, 4242, 0.4f));

  auto readCorrelation = [&]() -> float {
    const juce::var levels = proc.getMeterLevels();
    return static_cast<float>(levels["blocks"]["blk-dual"]["alignCorrelation"]);
  };

  const float beforeInvert = readCorrelation();
  std::printf("[DualMonoBlockTest] goniometer correlation, identical sides, Align off: %.3f\n",
             static_cast<double>(beforeInvert));
  EXPECT_GT(beforeInvert, 0.9f)
      << "identical content on both sides should read as strongly correlated even with Align off";

  ASSERT_TRUE(proc.setDualInvert("blk-dual", false, true));
  // DeckCorrelation is a ~300 ms exponential running average (by design -
  // an instantaneous meter would jitter distractingly), so settling all
  // the way from +1 to near -1 genuinely takes real audio time, not just a
  // few blocks - process well past 3 time constants' worth.
  processStereo(proc, makeNoise(100 * kBlock, 9191, 0.4f));
  const float afterInvert = readCorrelation();
  std::printf("[DualMonoBlockTest] goniometer correlation, right side inverted, Align off: %.3f\n",
             static_cast<double>(afterInvert));
  EXPECT_LT(afterInvert, -0.9f)
      << "inverting one side should flip the correlation reading toward -1, live, without ever "
         "touching Align";
}

// Regression for a real, user-reported bug: Mute on a *loaded* side used to
// toggle that child's own `enabled` (bypass), which crossfades the block's
// output to its dry, unprocessed input - audible, not silent. A muted side
// must be true silence, exactly like an empty muted side already was.
TEST(DualMonoBlockTest, MutingALoadedSideIsTrueSilenceNotBypass) {
  const auto in = makeNoise(20 * kBlock, 4242, 0.25f);

  const auto runWithMuteState = [&](bool muted) {
    ChainTestProcessor proc;
    proc.setPlayConfigDetails(2, 2, kFs, kBlock);
    proc.prepareToPlay(kFs, kBlock);

    juce::ValueTree state("ChainSnapshot");
    juce::ValueTree left("ChainBlocks");
    left.appendChild(makeDualMonoBlockTree("blk-dual", makeNamBlockTree("blk-nam", 1, 100)),
                     nullptr);
    state.appendChild(left, nullptr);
    proc.restoreFromTree(state);
    EXPECT_TRUE(waitForDualMonoLoaded(proc));

    if (muted) EXPECT_TRUE(proc.setDualMuted("blk-dual", /*isLeftSide=*/true, true));

    processStereo(proc, makeNoise(kWarmupBlocks * kBlock, 1111, 0.25f));
    return processStereo(proc, in);
  };

  const auto unmuted = runWithMuteState(false);
  const auto muted = runWithMuteState(true);

  float peakUnmuted = 0.0f;
  for (float sample : unmuted.first) peakUnmuted = std::max(peakUnmuted, std::abs(sample));
  float peakMuted = 0.0f;
  for (float sample : muted.first) peakMuted = std::max(peakMuted, std::abs(sample));

  std::printf("[DualMonoBlockTest] loaded Left side peak - unmuted: %.4f, muted: %.6f\n",
             static_cast<double>(peakUnmuted), static_cast<double>(peakMuted));
  EXPECT_GT(peakUnmuted, 0.01f)
      << "sanity check: the unmuted run should have real signal to compare against";
  EXPECT_LT(peakMuted, 1e-3f)
      << "a muted loaded side must be true silence - it should not fall back to the dry, "
         "unprocessed input the way an ordinary block's own bypass (enabled=false) does";
}

// setDualStereoProcessingEnabled(false) must force both Align and Ø
// neutral WITHOUT clearing the stored dualLeftInvert/dualAlign* fields -
// turning it back on should restore exactly what was dialed in.
TEST(DualMonoBlockTest, StereoProcessingBypassOverridesWithoutClearingStoredSettings) {
  ChainTestProcessor proc;
  proc.setPlayConfigDetails(2, 2, kFs, kBlock);
  proc.prepareToPlay(kFs, kBlock);

  // No stereoEnabled: mono chain mode, so the block widens.
  juce::ValueTree state("ChainSnapshot");
  juce::ValueTree left("ChainBlocks");
  left.appendChild(makeDualMonoBlockTree("blk-dual", makeNamBlockTree("blk-nam-l", 1, 100),
                                         makeNamBlockTree("blk-nam-r", 1, 100)),
                   nullptr);
  state.appendChild(left, nullptr);
  proc.restoreFromTree(state);
  ASSERT_TRUE(waitForDualMonoLoaded(proc));

  ASSERT_TRUE(proc.setDualGoniometerEnabled("blk-dual", true));
  ASSERT_TRUE(proc.setDualInvert("blk-dual", false, true));
  processStereo(proc, makeNoise(kWarmupBlocks * kBlock, 1111, 0.25f));
  processStereo(proc, makeNoise(100 * kBlock, 4242, 0.4f));

  auto readCorrelation = [&]() -> float {
    const juce::var levels = proc.getMeterLevels();
    return static_cast<float>(levels["blocks"]["blk-dual"]["alignCorrelation"]);
  };

  const float invertedActive = readCorrelation();
  ASSERT_TRUE(proc.setDualStereoProcessingEnabled("blk-dual", false));
  processStereo(proc, makeNoise(100 * kBlock, 5151, 0.4f));
  const float bypassed = readCorrelation();
  ASSERT_TRUE(proc.setDualStereoProcessingEnabled("blk-dual", true));
  processStereo(proc, makeNoise(100 * kBlock, 6161, 0.4f));
  const float restored = readCorrelation();

  std::printf(
      "[DualMonoBlockTest] correlation - Ø active: %.3f, bypassed: %.3f, restored: %.3f\n",
      static_cast<double>(invertedActive), static_cast<double>(bypassed),
      static_cast<double>(restored));
  EXPECT_LT(invertedActive, -0.9f);
  EXPECT_GT(bypassed, 0.9f) << "bypass should force Ø neutral, reading as correlated again";
  EXPECT_LT(restored, -0.9f)
      << "re-enabling should restore the stored Ø flip, not have cleared it while bypassed";
}

namespace {
// Same shape as auto_offset_tests.cpp's own runProbe, rescoped to a Dual
// Mono block: drives the real audio-callback loop (probe injection, chain
// rendering, capture tap, mute stage, message-thread poll) and returns the
// terminal poll payload.
juce::var runDualAutoAlign(TONE3000Processor& proc, const juce::String& blockId) {
  if (!static_cast<bool>(proc.armDualAutoAlign(blockId.toStdString())))
    return {};
  juce::AudioBuffer<float> buffer(2, kBlock);
  juce::MidiBuffer midi;
  for (int block = 0; block < 400; ++block) {
    buffer.clear();
    proc.processBlock(buffer, midi);
    const juce::var poll = proc.pollDualAutoAlign(blockId.toStdString());
    const juce::String state = poll["state"].toString();
    if (state == "done" || state == "timeout")
      return poll;
  }
  return {};
}
}  // namespace

// The point of this whole feature: the global startAutoOffset requires
// stereo chain mode (two real lanes) - a Dual Mono block creates its own
// "two chains" out of a single lane, so armDualAutoAlign must work in
// ordinary mono chain mode, and must write the result to the block's own
// params rather than the global chain-level ones.
TEST(DualMonoBlockTest, AutoAlignMeasuresRealRigsInMonoChainModeAndWritesBlockParams) {
  ChainTestProcessor proc;
  proc.setPlayConfigDetails(2, 2, kFs, kBlock);
  proc.prepareToPlay(kFs, kBlock);

  // No stereoEnabled: mono chain mode. Two genuinely different real
  // captures (same fixture files auto_offset_tests.cpp's own
  // ProbeAlignsRealNamAndIrChains uses for the global case), so there's a
  // real, non-trivial lag/voicing difference to measure.
  juce::ValueTree state("ChainSnapshot");
  juce::ValueTree left("ChainBlocks");
  left.appendChild(
      makeDualMonoBlockTree("blk-dual", makeNamBlockTree("amp-l", 1, 100, "a2-amp-cab-test.nam"),
                            makeNamBlockTree("amp-r", 2, 101, "a2-am-test-2.nam")),
      nullptr);
  state.appendChild(left, nullptr);
  proc.restoreFromTree(state);
  ASSERT_TRUE(waitForDualMonoLoaded(proc)) << "dual mono block never finished loading";

  const juce::var result = runDualAutoAlign(proc, "blk-dual");
  ASSERT_EQ(result["state"].toString(), "done")
      << "probe did not complete: " << juce::JSON::toString(result).toStdString();
  const double matchedMs = static_cast<double>(result["matchedMs"]);
  std::printf("[DualMonoBlockTest] auto align matched %.3f ms\n", matchedMs);
  EXPECT_LT(std::abs(matchedMs), 24.0);

  const juce::var block = blockById(proc, "blk-dual");
  ASSERT_FALSE(block.isVoid());
  const juce::var params = block["params"];
  // ms -> knob position, the same StereoOffsetParams::fromNormalized
  // inverse the native side applies - confirms the result actually landed
  // on the block's own offset field, not left untouched or misapplied.
  const double expectedNorm =
      juce::jlimit(0.0, 1.0, 0.5 + matchedMs / (2.0 * StereoOffsetParams::kMaxOffsetMs));
  EXPECT_NEAR(static_cast<double>(params["dualAlignOffset"]), expectedNorm, 1e-4);
  if (std::abs(matchedMs) >= 0.05)
    EXPECT_TRUE(static_cast<bool>(params["dualAlignEnabled"]))
        << "a real measured offset should power Align on";
}

// Host-config sweep (Logic runs 44.1 kHz / 2048-sample blocks): the same
// Dual Mono rig must sound the same at every host rate and block size - the
// resampling boundary and block slicing are supposed to be transparent. A
// config-specific buffer bug shows up here as a level blow-up or garbage.
TEST(DualMonoBlockTest, HostRateAndBlockSizeSweepStaysSane) {
  struct Config {
    double rate;
    int block;
  };
  const Config configs[] = {{48000.0, 512}, {44100.0, 512}, {48000.0, 2048}, {44100.0, 2048}};
  double refRms = 0.0;
  float refPeak = 0.0f;
  for (const auto& cfg : configs) {
    ChainTestProcessor proc;
    proc.setPlayConfigDetails(2, 2, cfg.rate, cfg.block);
    proc.prepareToPlay(cfg.rate, cfg.block);
    juce::ValueTree state("ChainSnapshot");
    juce::ValueTree lane("ChainBlocks");
    lane.appendChild(makeDualMonoBlockTree("blk-dual", makeNamBlockTree("blk-nam", 1, 100),
                                           makeIrBlockTree("blk-ir", 2, 200)),
                     nullptr);
    state.appendChild(lane, nullptr);
    proc.restoreFromTree(state);
    ASSERT_TRUE(waitForDualMonoLoaded(proc));

    const int n = 40 * 2048;
    processStereo(proc, makeSine(n, 220.0, 0.25f, cfg.rate), cfg.block);  // warm-up
    const auto out = processStereo(proc, makeSine(n, 220.0, 0.25f, cfg.rate), cfg.block);
    float peak = 0.0f;
    double sum = 0.0;
    bool finite = true;
    for (size_t i = n / 2; i < out.first.size(); ++i) {
      for (float s : {out.first[i], out.second[i]}) {
        finite = finite && std::isfinite(s);
        peak = std::max(peak, std::abs(s));
        sum += static_cast<double>(s) * s;
      }
    }
    const double rms = std::sqrt(sum / static_cast<double>(out.first.size()));
    if (refRms == 0.0) refRms = rms;
    if (refPeak == 0.0f) refPeak = peak;
    std::printf("[DualMonoSweep] %.0f Hz / %d: peak %.3f rms %.4f (%.2f dB vs ref)\n", cfg.rate,
                cfg.block, static_cast<double>(peak), rms, 20.0 * std::log10(rms / refRms));
    EXPECT_TRUE(finite) << cfg.rate << "/" << cfg.block;
    EXPECT_NEAR(20.0 * std::log10(rms / refRms), 0.0, 1.5) << cfg.rate << "/" << cfg.block;
    // Peak too: the stale-sample bug left RMS untouched but spiked peaks
    // (0.344 vs 0.279 at 44.1k/2048).
    EXPECT_NEAR(20.0 * std::log10(peak / refPeak), 0.0, 0.5) << cfg.rate << "/" << cfg.block;
  }
}

// Block-size independence (the Logic noise bug): a host that varies its
// block sizes (Logic does; the 44.1k boundary also yields irregular chain
// blocks) must get sample-for-sample the same output as a fixed-size host.
// runDualMono used to hand each side its max-size scratch buffer, so every
// shorter block pushed stale samples through the sides' NAM/IR state.
TEST(DualMonoBlockTest, VariableHostBlockSizesMatchFixedOutput) {
  auto run = [](const juce::ValueTree& tree, bool variable) {
    ChainTestProcessor proc;
    proc.setPlayConfigDetails(2, 2, kFs, 512);
    proc.prepareToPlay(kFs, 512);
    juce::ValueTree state("ChainSnapshot");
    juce::ValueTree lane("ChainBlocks");
    lane.appendChild(tree.createCopy(), nullptr);
    state.appendChild(lane, nullptr);
    proc.restoreFromTree(state);
    EXPECT_TRUE(waitForDualMonoLoaded(proc));
    const int total = 48000 * 3;
    const auto in = makeNoise(total, 4321, 0.25f);
    std::vector<float> outL(static_cast<size_t>(total)), outR(static_cast<size_t>(total));
    juce::AudioBuffer<float> buf(2, 512);
    juce::MidiBuffer midi;
    const int pattern[] = {512, 256, 37, 512, 1, 511, 300};
    int off = 0, k = 0;
    while (off < total) {
      const int n = std::min(variable ? pattern[k++ % 7] : 512, total - off);
      buf.setSize(2, n, false, false, true);
      buf.copyFrom(0, 0, in.data() + off, n);
      buf.copyFrom(1, 0, in.data() + off, n);
      proc.processBlock(buf, midi);
      std::copy(buf.getReadPointer(0), buf.getReadPointer(0) + n, outL.begin() + off);
      std::copy(buf.getReadPointer(1), buf.getReadPointer(1) + n, outR.begin() + off);
      off += n;
    }
    return std::make_pair(outL, outR);
  };
  const auto dual = makeDualMonoBlockTree("blk-dual", makeNamBlockTree("blk-nam", 1, 100),
                                          makeIrBlockTree("blk-ir", 2, 200));
  const auto fixed = run(dual, false);
  const auto variable = run(dual, true);
  const float diffL = maxAbsDiff(fixed.first, variable.first);
  const float diffR = maxAbsDiff(fixed.second, variable.second);
  std::printf("[DualMonoBlockTest] fixed vs variable blocks max |diff|: L=%.8f R=%.8f\n",
              static_cast<double>(diffL), static_cast<double>(diffR));
  EXPECT_LT(diffL, 1e-4f) << "left side depends on host block size";
  EXPECT_LT(diffR, 1e-4f) << "right side depends on host block size";
}
