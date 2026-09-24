// Scenes (see Processor.h's SCENES section). Pins the model's contracts:
//
//   - bypass is always per scene; plain params are shared (editing one in
//     any scene changes every scene),
//   - a param marked Per Scene is seeded with the current value in every
//     scene and then diverges per scene,
//   - the scene level moves the output, and follows the active scene,
//   - scenes survive a session save/restore (active scene included), prune
//     removed blocks, and undo restores them.
#include "Processor.h"
#include "chain_test_helpers.h"

#include <gtest/gtest.h>

#include <cmath>

namespace {

juce::var blockParams(ChainTestProcessor& proc, const juce::String& id) {
  const juce::var state = proc.getChainState(-1);
  if (const auto* lane = state["chain"].getArray())
    for (const auto& item : *lane)
      if (item["blockId"].toString() == id) return item["params"];
  return {};
}

bool enabled(ChainTestProcessor& proc, const juce::String& id) {
  return static_cast<bool>(blockParams(proc, id)["enabled"]);
}

float param(ChainTestProcessor& proc, const juce::String& id, const char* name) {
  return static_cast<float>(blockParams(proc, id)[name]);
}

TEST(ScenesTest, BypassIsPerSceneAndPlainParamsAreShared) {
  ChainTestProcessor proc;
  seedChain(proc, {"a", "b"});
  ASSERT_TRUE(waitForChainLoaded(proc));

  ASSERT_TRUE(proc.selectScene(1));
  ASSERT_TRUE(proc.setBlockParam("b", "enabled", 0.0));   // bypass b in scene 2
  ASSERT_TRUE(proc.setBlockParam("a", "outputGain", 0.8)); // shared edit

  ASSERT_TRUE(proc.selectScene(0));
  EXPECT_TRUE(enabled(proc, "b")) << "scene 1 never bypassed b";
  EXPECT_FLOAT_EQ(param(proc, "a", "outputGain"), 0.8f) << "plain params are shared";

  ASSERT_TRUE(proc.selectScene(1));
  EXPECT_FALSE(enabled(proc, "b")) << "scene 2 keeps its bypass";
  EXPECT_EQ(proc.getActiveScene(), 1);
  EXPECT_FALSE(proc.selectScene(8));
  EXPECT_FALSE(proc.selectScene(-1));
}

TEST(ScenesTest, PerSceneParamIsSeededThenDiverges) {
  ChainTestProcessor proc;
  seedChain(proc, {"a"});
  ASSERT_TRUE(waitForChainLoaded(proc));
  ASSERT_TRUE(proc.setBlockParam("a", "mix", 0.3));
  ASSERT_TRUE(proc.setBlockParamPerScene("a", "mix", true));

  ASSERT_TRUE(proc.selectScene(3));
  EXPECT_FLOAT_EQ(param(proc, "a", "mix"), 0.3f) << "seeded with the value at marking time";
  ASSERT_TRUE(proc.setBlockParam("a", "mix", 0.9));

  ASSERT_TRUE(proc.selectScene(0));
  EXPECT_FLOAT_EQ(param(proc, "a", "mix"), 0.3f);
  ASSERT_TRUE(proc.selectScene(3));
  EXPECT_FLOAT_EQ(param(proc, "a", "mix"), 0.9f);

  // Back to shared: the current value stays and is now common to all.
  ASSERT_TRUE(proc.setBlockParamPerScene("a", "mix", false));
  ASSERT_TRUE(proc.selectScene(0));
  EXPECT_FLOAT_EQ(param(proc, "a", "mix"), 0.9f);
  EXPECT_FALSE(proc.setBlockParamPerScene("a", "bogus", true));
}

TEST(ScenesTest, SceneLevelMovesTheOutput) {
  ChainTestProcessor proc;
  proc.setPlayConfigDetails(2, 2, kFs, 512);
  proc.prepareToPlay(kFs, 512);
  seedChain(proc, {"a"});
  ASSERT_TRUE(waitForChainLoaded(proc));
  auto rmsDb = [](const std::vector<float>& v) {
    double sum = 0.0;
    for (size_t i = v.size() / 2; i < v.size(); ++i) sum += static_cast<double>(v[i]) * v[i];
    return 10.0 * std::log10(sum / static_cast<double>(v.size() / 2) + 1e-20);
  };
  const auto noise = makeNoise(40 * 512, 99, 0.2f);
  processStereo(proc, noise);
  const double base = rmsDb(processStereo(proc, noise).first);

  ASSERT_TRUE(proc.setSceneLevel(2, -6.0));
  const double unchanged = rmsDb(processStereo(proc, noise).first);
  EXPECT_NEAR(unchanged, base, 0.1) << "another scene's level must not touch the active one";

  ASSERT_TRUE(proc.selectScene(2));
  processStereo(proc, noise);
  const double quieter = rmsDb(processStereo(proc, noise).first);
  EXPECT_NEAR(quieter - base, -6.0, 0.2);
}

TEST(ScenesTest, SurviveSessionRoundTripAndPruneRemovedBlocks) {
  ChainTestProcessor proc;
  seedChain(proc, {"a", "b"});
  ASSERT_TRUE(waitForChainLoaded(proc));
  ASSERT_TRUE(proc.setBlockParamPerScene("a", "inputGain", true));
  ASSERT_TRUE(proc.renameScene(4, "Lead"));
  ASSERT_TRUE(proc.selectScene(4));
  ASSERT_TRUE(proc.setBlockParam("a", "inputGain", 0.75));
  ASSERT_TRUE(proc.setBlockParam("b", "enabled", 0.0));

  juce::MemoryBlock data;
  proc.getStateInformation(data);
  ChainTestProcessor restored;
  restored.setStateInformation(data.getData(), static_cast<int>(data.getSize()));
  ASSERT_TRUE(waitForChainLoaded(restored));

  EXPECT_EQ(restored.getActiveScene(), 4);
  EXPECT_FALSE(enabled(restored, "b"));
  EXPECT_FLOAT_EQ(param(restored, "a", "inputGain"), 0.75f);
  ASSERT_TRUE(restored.selectScene(0));
  EXPECT_TRUE(enabled(restored, "b"));
  EXPECT_FLOAT_EQ(param(restored, "a", "inputGain"), 0.5f);
  ASSERT_TRUE(restored.selectScene(4));
  EXPECT_FALSE(enabled(restored, "b"));

  // Removing a block drops it from every scene; switching stays safe.
  letAudioGoIdle();
  ASSERT_TRUE(restored.removeChainBlock("b"));
  ASSERT_TRUE(restored.selectScene(0));
  ASSERT_TRUE(restored.selectScene(4));
  EXPECT_FLOAT_EQ(param(restored, "a", "inputGain"), 0.75f);
}

TEST(ScenesTest, UndoRestoresScenes) {
  ChainTestProcessor proc;
  seedChain(proc, {"a"});
  ASSERT_TRUE(waitForChainLoaded(proc));
  ASSERT_TRUE(proc.selectScene(1));
  ASSERT_TRUE(proc.setBlockParam("a", "enabled", 0.0));  // undoable edit in scene 2
  ASSERT_TRUE(proc.undoChain());
  EXPECT_TRUE(enabled(proc, "a"));
  EXPECT_EQ(proc.getActiveScene(), 1);
}

}  // namespace
