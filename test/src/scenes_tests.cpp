// Scenes & channels (see Processor.h's SCENES & CHANNELS section). Pins the
// model's contracts:
//
//   - a scene stores each block's bypass and channel; params live in the
//     channel, so an edit reaches every scene using that channel,
//   - a new channel starts as a copy of the current one and then diverges;
//     channels can be copied over each other,
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
  EXPECT_FALSE(proc.selectScene(4));
  EXPECT_FALSE(proc.selectScene(-1));
}

int channel(ChainTestProcessor& proc, const juce::String& id) {
  return static_cast<int>(blockParams(proc, id)["channel"]);
}

TEST(ScenesTest, ChannelsAreSeededThenDivergeAndScenesPickThem) {
  ChainTestProcessor proc;
  seedChain(proc, {"a"});
  ASSERT_TRUE(waitForChainLoaded(proc));
  ASSERT_TRUE(proc.setBlockParam("a", "mix", 0.3));

  ASSERT_TRUE(proc.selectBlockChannel("a", 1));
  EXPECT_EQ(channel(proc, "a"), 1);
  EXPECT_FLOAT_EQ(param(proc, "a", "mix"), 0.3f) << "a new channel starts as a copy";
  ASSERT_TRUE(proc.setBlockParam("a", "mix", 0.9));
  ASSERT_TRUE(proc.selectBlockChannel("a", 0));
  EXPECT_FLOAT_EQ(param(proc, "a", "mix"), 0.3f);
  ASSERT_TRUE(proc.selectBlockChannel("a", 1));
  EXPECT_FLOAT_EQ(param(proc, "a", "mix"), 0.9f);
  {
    const juce::var used = blockParams(proc, "a")["channelsUsed"];
    EXPECT_TRUE(static_cast<bool>(used[0]) && static_cast<bool>(used[1]));
    EXPECT_FALSE(static_cast<bool>(used[2]) || static_cast<bool>(used[3]));
  }

  // Scene 3 uses channel A, bypassed (set from the manager, without visiting).
  ASSERT_TRUE(proc.setSceneBlockChannel(2, "a", 0));
  ASSERT_TRUE(proc.setSceneBlockEnabled(2, "a", false));
  EXPECT_FLOAT_EQ(param(proc, "a", "mix"), 0.9f) << "another scene's edit leaves the live one";
  EXPECT_TRUE(enabled(proc, "a"));
  ASSERT_TRUE(proc.selectScene(2));
  EXPECT_EQ(channel(proc, "a"), 0);
  EXPECT_FLOAT_EQ(param(proc, "a", "mix"), 0.3f);
  EXPECT_FALSE(enabled(proc, "a"));
  ASSERT_TRUE(proc.selectScene(0));
  EXPECT_EQ(channel(proc, "a"), 1);
  EXPECT_TRUE(enabled(proc, "a"));

  // Copy B over A: A now matches, scene 3 hears it.
  ASSERT_TRUE(proc.copyBlockChannel("a", 1, 0));
  ASSERT_TRUE(proc.selectScene(2));
  EXPECT_FLOAT_EQ(param(proc, "a", "mix"), 0.9f);

  EXPECT_FALSE(proc.selectBlockChannel("a", 4));
  EXPECT_FALSE(proc.copyBlockChannel("a", 1, 1));
  EXPECT_FALSE(proc.selectBlockChannel("nope", 1));
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
  ASSERT_TRUE(proc.renameScene(3, "Lead"));
  ASSERT_TRUE(proc.selectScene(3));
  ASSERT_TRUE(proc.selectBlockChannel("a", 1));
  ASSERT_TRUE(proc.setBlockParam("a", "inputGain", 0.75));
  ASSERT_TRUE(proc.setBlockParam("b", "enabled", 0.0));

  juce::MemoryBlock data;
  proc.getStateInformation(data);
  ChainTestProcessor restored;
  restored.setStateInformation(data.getData(), static_cast<int>(data.getSize()));
  ASSERT_TRUE(waitForChainLoaded(restored));

  EXPECT_EQ(restored.getActiveScene(), 3);
  EXPECT_FALSE(enabled(restored, "b"));
  EXPECT_EQ(channel(restored, "a"), 1);
  EXPECT_FLOAT_EQ(param(restored, "a", "inputGain"), 0.75f);
  ASSERT_TRUE(restored.selectScene(0));
  EXPECT_TRUE(enabled(restored, "b"));
  EXPECT_EQ(channel(restored, "a"), 0);
  EXPECT_FLOAT_EQ(param(restored, "a", "inputGain"), 0.5f);
  ASSERT_TRUE(restored.selectScene(3));
  EXPECT_FALSE(enabled(restored, "b"));
  {
    const juce::var state = restored.getChainState(-1);
    EXPECT_EQ(state["scenes"]["names"][3].toString(), "Lead");
  }

  // Removing a block drops it from every scene; switching stays safe.
  letAudioGoIdle();
  ASSERT_TRUE(restored.removeChainBlock("b"));
  ASSERT_TRUE(restored.selectScene(0));
  ASSERT_TRUE(restored.selectScene(3));
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

// Gapless model switching: the other channel's model is kept warm, and the
// switch crossfades between two running engines - no dip, no load state.
// Both "models" are the same capture bytes under two ids, so a gapless
// switch leaves the output level untouched; the old wet-mute swap would
// have dropped it to near silence for ~50 ms.
TEST(ScenesTest, NamModelSwitchIsGaplessOnceWarm) {
  ChainTestProcessor proc;
  proc.setPlayConfigDetails(2, 2, kFs, 512);
  proc.prepareToPlay(kFs, 512);

  juce::ValueTree block = makeNamBlockTree("amp", 1, 100);
  {
    juce::MemoryBlock bytes;
    ASSERT_TRUE(testFile("a2-amp-test.nam").loadFileAsData(bytes));
    juce::ValueTree cached("CachedModel");
    cached.setProperty("modelId", 101, nullptr);
    cached.setProperty("data", juce::var(bytes), nullptr);
    block.getChildWithName("ModelCache").appendChild(cached, nullptr);
    // List both models on the tone, so the restore keeps 101's cached bytes
    // (unreferenced cache entries are dropped on restore).
    block.setProperty(
        "toneJson",
        "{\"id\":1,\"title\":\"Test Amp\",\"format\":\"nam\",\"models\":["
        "{\"id\":100,\"name\":\"amp\",\"model_url\":\"https://test.invalid/amp.nam\"},"
        "{\"id\":101,\"name\":\"amp-b\",\"model_url\":\"https://test.invalid/amp-b.nam\"}]}",
        nullptr);
  }
  juce::ValueTree state("ChainSnapshot");
  juce::ValueTree lane("ChainBlocks");
  lane.appendChild(block, nullptr);
  state.appendChild(lane, nullptr);
  proc.restoreFromTree(state);
  ASSERT_TRUE(waitForChainLoaded(proc));

  // Scene 2 puts the amp on channel B with model 101 (a regular load the
  // first time).
  ASSERT_TRUE(proc.selectScene(1));
  ASSERT_TRUE(proc.selectBlockChannel("amp", 1));
  auto* model101 = new juce::DynamicObject();
  model101->setProperty("id", 101);
  model101->setProperty("name", "amp-b");
  model101->setProperty("model_url", "https://test.invalid/amp-b.nam");
  ASSERT_TRUE(proc.switchModel("amp", 101, juce::var(model101)));
  // loaded stays true while a switch loads (the old engine keeps playing),
  // so wait for modelLoading to clear instead.
  {
    const auto until = juce::Time::getMillisecondCounter() + 10000;
    while (static_cast<bool>(proc.getChainState(-1)["chain"][0]["modelLoading"]) &&
           juce::Time::getMillisecondCounter() < until)
      juce::Thread::sleep(20);
    ASSERT_FALSE(static_cast<bool>(proc.getChainState(-1)["chain"][0]["modelLoading"]));
  }
  ASSERT_TRUE(waitForChainLoaded(proc));

  // Scene 1's model (100) must warm up in the background.
  const auto deadline = juce::Time::getMillisecondCounter() + 10000;
  while (!proc.isSceneModelWarm("amp", 100) && juce::Time::getMillisecondCounter() < deadline)
    juce::Thread::sleep(20);
  ASSERT_TRUE(proc.isSceneModelWarm("amp", 100)) << "scene 1's model never warmed up";

  const auto sine = makeSine(80 * 512, 220.0, 0.3f);
  const std::vector<float> first(sine.begin(), sine.begin() + 40 * 512);
  const std::vector<float> second(sine.begin() + 40 * 512, sine.end());
  processStereo(proc, first);                       // steady state on model 101
  ASSERT_TRUE(proc.selectScene(0));                 // switch while "playing"
  const auto after = processStereo(proc, second).first;

  const juce::var row = proc.getChainState(-1)["chain"][0];
  EXPECT_EQ(static_cast<int>(row["activeModelId"]), 100);
  EXPECT_TRUE(static_cast<bool>(row["loaded"])) << "warm swap must not enter a loading state";

  // 5 ms windows across the switch: none may dip (a wet-mute swap would).
  auto rms = [&](size_t start, size_t len) {
    double sum = 0.0;
    for (size_t i = start; i < start + len; ++i) sum += static_cast<double>(after[i]) * after[i];
    return std::sqrt(sum / static_cast<double>(len));
  };
  const size_t win = 240;  // 5 ms at 48 kHz
  const double steady = rms(after.size() - 8 * win, 8 * win);
  double worst = 1e9;
  for (size_t w = 0; w < 20; ++w) worst = std::min(worst, rms(w * win, win));
  std::printf("[ScenesTest] steady rms %.4f, worst 5ms window after switch %.4f (%.2f dB)\n",
              steady, worst, 20.0 * std::log10(worst / steady));
  EXPECT_GT(worst, steady * 0.7) << "the switch dipped - not gapless";

  // Session round trip: scene 2's model bytes must ride the state (the URL
  // is fake, so a warm engine can only come from the embedded bytes) and
  // warm up again on load, ready for a gapless switch.
  juce::MemoryBlock data;
  proc.getStateInformation(data);
  ChainTestProcessor restored;
  restored.setPlayConfigDetails(2, 2, kFs, 512);
  restored.prepareToPlay(kFs, 512);
  restored.setStateInformation(data.getData(), static_cast<int>(data.getSize()));
  ASSERT_TRUE(waitForChainLoaded(restored));
  EXPECT_EQ(restored.getActiveScene(), 0);
  const auto until = juce::Time::getMillisecondCounter() + 10000;
  while (!restored.isSceneModelWarm("amp", 101) && juce::Time::getMillisecondCounter() < until)
    juce::Thread::sleep(20);
  EXPECT_TRUE(restored.isSceneModelWarm("amp", 101))
      << "scene 2's model was not restored from the saved bytes";
}

TEST(ScenesTest, HostSceneParameterFollowsAndDrivesTheActiveScene) {
  ChainTestProcessor proc;
  seedChain(proc, {"a"});
  ASSERT_TRUE(waitForChainLoaded(proc));
  auto* param = proc.parameters.getParameter("scene");
  ASSERT_NE(param, nullptr);

  // Host automation -> switch (deferred to the message thread).
  param->setValueNotifyingHost(param->convertTo0to1(3.0f));
  juce::MessageManager::getInstance()->runDispatchLoopUntil(50);
  EXPECT_EQ(proc.getActiveScene(), 3);

  // UI/MIDI switch -> the parameter follows, so the host can record it.
  ASSERT_TRUE(proc.selectScene(2));
  juce::MessageManager::getInstance()->runDispatchLoopUntil(50);
  EXPECT_EQ(juce::roundToInt(param->convertFrom0to1(param->getValue())), 2);
}

}  // namespace
