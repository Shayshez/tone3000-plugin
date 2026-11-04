// Scenes & channels (see Processor.h's SCENES & CHANNELS section). Pins the
// model's contracts:
//
//   - a scene stores each block's bypass and channel; params live in the
//     channel, so an edit reaches every scene using that channel,
//   - a new channel starts at the block's defaults (same model) and then diverges;
//     channels can be copied over each other,
//   - each channel holds its own tone, of the block's kind: a load touches the
//     active channel only, and a block never changes kind,
//   - the scene level moves the output, and follows the active scene,
//   - scenes survive a session save/restore (active scene included), prune
//     removed blocks, and undo restores them.
#include "Processor.h"
#include "chain_test_helpers.h"

#include <gtest/gtest.h>

#include <cmath>
#include <functional>

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
  EXPECT_FLOAT_EQ(param(proc, "a", "mix"), 1.0f) << "a new channel starts at defaults";
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

// Each channel holds its own tone: replacing it touches the active channel
// alone.
TEST(ScenesTest, ReplacingTheToneTouchesOnlyTheActiveChannel) {
  ChainTestProcessor proc;
  seedChain(proc, {"a"});
  ASSERT_TRUE(waitForChainLoaded(proc));
  ASSERT_TRUE(proc.setBlockParam("a", "mix", 0.3));
  ASSERT_TRUE(proc.selectBlockChannel("a", 1));
  ASSERT_TRUE(proc.setBlockParam("a", "mix", 0.7));
  ASSERT_TRUE(proc.swapTone(
      "a",
      "{\"id\":9,\"title\":\"Other\",\"format\":\"ir\",\"models\":[{\"id\":900,"
      "\"name\":\"other\",\"model_url\":\"https://test.invalid/other.wav\"}]}"));
  {
    const juce::var row = proc.getChainState(-1)["chain"][0];
    EXPECT_EQ(static_cast<int>(row["activeModelId"]), 900);
    EXPECT_EQ(static_cast<int>(row["tone"]["id"]), 9);
    EXPECT_FLOAT_EQ(static_cast<float>(row["params"]["mix"]), 0.7f) << "a swap keeps the params";
  }
  ASSERT_TRUE(proc.selectBlockChannel("a", 0));
  const juce::var row = proc.getChainState(-1)["chain"][0];
  EXPECT_EQ(static_cast<int>(row["activeModelId"]), 100) << "channel A keeps its own model";
  EXPECT_EQ(static_cast<int>(row["tone"]["id"]), 1) << "channel A keeps its own tone";
  EXPECT_FLOAT_EQ(static_cast<float>(row["params"]["mix"]), 0.3f);
  EXPECT_TRUE(row["params"]["channelNames"][1].toString().contains("Other"));
  ASSERT_TRUE(proc.selectBlockChannel("a", 1));
  EXPECT_EQ(static_cast<int>(proc.getChainState(-1)["chain"][0]["tone"]["id"]), 9);
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
  // The switch runs under chainMutex, which the audio thread waits on: it
  // must stay far below a callback (no engine reset/prewarm in there).
  const double t0 = juce::Time::getMillisecondCounterHiRes();
  ASSERT_TRUE(proc.selectScene(0));                 // switch while "playing"
  const double switchMs = juce::Time::getMillisecondCounterHiRes() - t0;
  std::printf("[ScenesTest] warm NAM switch held the chain for %.2f ms\n", switchMs);
  EXPECT_LT(switchMs, 0.5) << "the switch stalls the audio thread (a reset/prewarm crept back in?)";
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

// IR/Cab channels: every other channel's kernel (model + shape) is kept warm,
// and a switch crossfades between the two convolvers - no wet-mute dip.
// Covers both a shape-only difference (same file) and a different file.
TEST(ScenesTest, IrChannelSwitchIsGaplessOnceWarm) {
  ChainTestProcessor proc;
  proc.setPlayConfigDetails(2, 2, kFs, 512);
  proc.prepareToPlay(kFs, 512);

  juce::ValueTree block = makeIrBlockTree("cab", 1, 100);
  {
    juce::MemoryBlock bytes;
    ASSERT_TRUE(testFile("cab-ir-test-2.wav").loadFileAsData(bytes));
    juce::ValueTree cached("CachedModel");
    cached.setProperty("modelId", 101, nullptr);
    cached.setProperty("data", juce::var(bytes), nullptr);
    block.getChildWithName("ModelCache").appendChild(cached, nullptr);
    block.setProperty(
        "toneJson",
        "{\"id\":1,\"title\":\"Test IR\",\"format\":\"ir\",\"models\":["
        "{\"id\":100,\"name\":\"cab\",\"model_url\":\"https://test.invalid/cab.wav\"},"
        "{\"id\":101,\"name\":\"cab-b\",\"model_url\":\"https://test.invalid/cab-b.wav\"}]}",
        nullptr);
  }
  juce::ValueTree state("ChainSnapshot");
  juce::ValueTree lane("ChainBlocks");
  lane.appendChild(block, nullptr);
  state.appendChild(lane, nullptr);
  proc.restoreFromTree(state);
  ASSERT_TRUE(waitForChainLoaded(proc));

  auto waitFor = [](const std::function<bool()>& cond) {
    const auto until = juce::Time::getMillisecondCounter() + 10000;
    while (!cond() && juce::Time::getMillisecondCounter() < until)
      juce::Thread::sleep(20);
    return cond();
  };
  auto modelLoading = [&] {
    const juce::var st = proc.getChainState(-1);
    return static_cast<bool>(st["chain"][0]["modelLoading"]);
  };

  // Channel B: same file, another shape (tail pulled down).
  ASSERT_TRUE(proc.selectBlockChannel("cab", 1));
  ASSERT_TRUE(proc.setBlockIrDecay("cab", 1.0, 0.0, 0.5, 0.8, 0.3, 0.5));
  juce::Thread::sleep(300);  // the live shape rebuild lands
  // Channel C: another file.
  ASSERT_TRUE(proc.selectBlockChannel("cab", 2));
  auto* model101 = new juce::DynamicObject();
  model101->setProperty("id", 101);
  model101->setProperty("name", "cab-b");
  model101->setProperty("model_url", "https://test.invalid/cab-b.wav");
  ASSERT_TRUE(proc.switchModel("cab", 101, juce::var(model101)));
  ASSERT_TRUE(waitFor([&] { return !modelLoading(); }));
  ASSERT_TRUE(waitForChainLoaded(proc));

  const auto sine = makeSine(120 * 512, 220.0, 0.3f);
  auto rms = [](const std::vector<float>& v, size_t start, size_t len) {
    double sum = 0.0;
    for (size_t i = start; i < start + len; ++i) sum += static_cast<double>(v[i]) * v[i];
    return std::sqrt(sum / static_cast<double>(len));
  };
  // Switch while "playing"; no 5 ms window may fall far below both the
  // level before and the level after (a wet-mute swap drops to ~silence).
  auto checkSwitch = [&](int channel, const char* what) {
    ASSERT_TRUE(waitFor([&] { return proc.isChannelWarm("cab", channel); }))
        << what << ": channel never warmed up";
    const std::vector<float> a(sine.begin(), sine.begin() + 40 * 512);
    const std::vector<float> b(sine.begin() + 40 * 512, sine.begin() + 80 * 512);
    const auto before = processStereo(proc, a).first;
    const double t0 = juce::Time::getMillisecondCounterHiRes();
    ASSERT_TRUE(proc.selectBlockChannel("cab", channel));
    const double switchMs = juce::Time::getMillisecondCounterHiRes() - t0;
    EXPECT_LT(switchMs, 1.0) << what << ": the switch stalls the audio thread";
    EXPECT_FALSE(modelLoading()) << what << ": warm switch must not load";
    const auto after = processStereo(proc, b).first;
    const size_t win = 240;
    const double steadyBefore = rms(before, before.size() - 8 * win, 8 * win);
    const double steadyAfter = rms(after, after.size() - 8 * win, 8 * win);
    double worst = 1e9;
    for (size_t w = 0; w < 20; ++w) worst = std::min(worst, rms(after, w * win, win));
    std::printf("[ScenesTest] %s: before %.4f after %.4f worst %.4f (%.2f ms)\n", what,
                steadyBefore, steadyAfter, worst, switchMs);
    EXPECT_GT(worst, 0.6 * std::min(steadyBefore, steadyAfter)) << what << ": switch dipped";
  };
  checkSwitch(1, "other file -> shape");  // C (file 101) -> B (file 100, shaped)
  checkSwitch(0, "shape -> default");     // B -> A (same file, default shape)
  checkSwitch(2, "default -> other file");  // A -> C
  const juce::var row = proc.getChainState(-1)["chain"][0];
  EXPECT_EQ(static_cast<int>(row["activeModelId"]), 101);
}

// Spillover: a reverb's tail survives a channel switch or a bypass. A noise
// burst goes in, then silence; mid-tail the block switches channel (or is
// bypassed). The tail that follows must keep its energy compared with an
// untouched reference run - a crossfaded/faded engine would cut it off.
namespace {
struct ReverbRig {
  ChainTestProcessor proc;
  explicit ReverbRig(bool twoChannels) {
    proc.setPlayConfigDetails(2, 2, kFs, 512);
    proc.prepareToPlay(kFs, 512);
    juce::ValueTree block = makeIrBlockTree("verb", 1, 100, "reverb-ir-6s-test.wav", "irPlayer");
    juce::ValueTree state("ChainSnapshot");
    juce::ValueTree lane("ChainBlocks");
    lane.appendChild(block, nullptr);
    state.appendChild(lane, nullptr);
    proc.restoreFromTree(state);
    EXPECT_TRUE(waitForChainLoaded(proc));
    if (twoChannels) {
      // Channel B: same file, darker/shorter shape; back on A, wait for B warm.
      EXPECT_TRUE(proc.selectBlockChannel("verb", 1));
      EXPECT_TRUE(proc.setBlockIrDecay("verb", 1.0, 0.0, 0.5, 0.6, 0.5, 0.5));
      juce::Thread::sleep(400);
      EXPECT_TRUE(proc.selectBlockChannel("verb", 0));
      // Back on A for the first time goes through the regular shape rebuild
      // (A was never warmed): let it land - it mutes the wet path briefly,
      // and must not overlap the measured burst.
      juce::Thread::sleep(400);
      processStereo(proc, std::vector<float>(40 * 512, 0.0f));
      const auto until = juce::Time::getMillisecondCounter() + 10000;
      while (!proc.isChannelWarm("verb", 1) && juce::Time::getMillisecondCounter() < until)
        juce::Thread::sleep(20);
      EXPECT_TRUE(proc.isChannelWarm("verb", 1));
    }
    processStereo(proc, std::vector<float>(40 * 512, 0.0f));  // settle
  }
};

double energy(const std::vector<float>& v, size_t start, size_t len) {
  double sum = 0.0;
  for (size_t i = start; i < std::min(v.size(), start + len); ++i)
    sum += static_cast<double>(v[i]) * v[i];
  return sum;
}

std::vector<float> burst() {
  auto v = makeNoise(24 * 512, 7, 0.3f);  // ~0.25 s of noise
  return v;
}
}  // namespace

TEST(ScenesTest, ReverbTailSurvivesAChannelSwitch) {
  ReverbRig ref(true), test(true);
  const auto in = burst();
  const std::vector<float> silence(96 * 512, 0.0f);  // ~1 s
  processStereo(ref.proc, in);
  processStereo(test.proc, in);
  ASSERT_TRUE(test.proc.selectBlockChannel("verb", 1));  // switch at the start of the tail
  const auto refTail = processStereo(ref.proc, silence).first;
  const auto testTail = processStereo(test.proc, silence).first;
  const size_t from = 4800, len = 19200;  // 100-500 ms into the tail
  const double r = energy(refTail, from, len), t = energy(testTail, from, len);
  std::printf("[ScenesTest] tail energy after switch: %.4f of reference\n", t / r);
  EXPECT_GT(t, 0.7 * r) << "the switch cut the reverb tail";
}

TEST(ScenesTest, ReverbTailSurvivesAChannelSwitchWithOddHostBlocks) {
  ReverbRig ref(true), test(true);
  const auto in = burst();
  const std::vector<float> silence(96 * 512, 0.0f);
  processStereo(ref.proc, in, 300);
  processStereo(test.proc, in, 300);
  ASSERT_TRUE(test.proc.selectBlockChannel("verb", 1));
  const auto refTail = processStereo(ref.proc, silence, 300).first;
  const auto testTail = processStereo(test.proc, silence, 300).first;
  const double r = energy(refTail, 4800, 19200), t = energy(testTail, 4800, 19200);
  EXPECT_GT(t, 0.7 * r) << "odd host blocks: the switch cut the reverb tail";
  EXPECT_LT(t, 1.3 * r);
}

TEST(ScenesTest, ReverbTailSurvivesABypass) {
  ReverbRig ref(false), test(false);
  const auto in = burst();
  const std::vector<float> silence(96 * 512, 0.0f);
  processStereo(ref.proc, in);
  processStereo(test.proc, in);
  ASSERT_TRUE(test.proc.setBlockParam("verb", "enabled", 0.0));  // bypass mid-tail
  const auto refTail = processStereo(ref.proc, silence).first;
  const auto testTail = processStereo(test.proc, silence).first;
  const size_t from = 4800, len = 19200;
  const double r = energy(refTail, from, len), t = energy(testTail, from, len);
  std::printf("[ScenesTest] tail energy after bypass: %.4f of reference\n", t / r);
  EXPECT_GT(t, 0.7 * r) << "bypass cut the reverb tail";

  // New input no longer reaches the reverb while bypassed: dry only.
  processStereo(test.proc, std::vector<float>(10 * 48000, 0.0f));  // let the tail finish
  const auto sine = makeSine(40 * 512, 440.0, 0.25f);
  const auto out = processStereo(test.proc, sine).first;
  const double inE = energy(sine, 20 * 512, 20 * 512), outE = energy(out, 20 * 512, 20 * 512);
  EXPECT_NEAR(outE / inE, 1.0, 0.05) << "a bypassed block passes dry at unity";
}

// A channel switch crossfades the block EQ instead of stepping it: right
// after switching from a flat channel to one with a big bell boost, the
// output still sits near the old level and glides up over the fade.
TEST(ScenesTest, ChannelSwitchCrossfadesTheEq) {
  ChainTestProcessor proc;
  proc.setPlayConfigDetails(2, 2, kFs, 512);
  proc.prepareToPlay(kFs, 512);
  seedChain(proc, {"a"});
  ASSERT_TRUE(waitForChainLoaded(proc));
  ASSERT_TRUE(proc.selectBlockChannel("a", 1));
  auto* band = new juce::DynamicObject();
  band->setProperty("freqHz", 1000.0);
  band->setProperty("gainDb", 18.0);
  band->setProperty("q", 1.0);
  band->setProperty("poles", 4);
  band->setProperty("on", true);
  ASSERT_TRUE(proc.setBlockEqBand("a", 2, juce::var(band)));
  ASSERT_TRUE(proc.setBlockEqEnabled("a", true));
  ASSERT_TRUE(proc.selectBlockChannel("a", 0));

  const auto sine = makeSine(80 * 512, 1000.0, 0.05f);
  const std::vector<float> first(sine.begin(), sine.begin() + 40 * 512);
  const std::vector<float> second(sine.begin() + 40 * 512, sine.end());
  const auto before = processStereo(proc, first).first;
  ASSERT_TRUE(proc.selectBlockChannel("a", 1));
  const auto after = processStereo(proc, second).first;
  auto rms = [](const std::vector<float>& v, size_t start, size_t len) {
    double sum = 0.0;
    for (size_t i = start; i < start + len; ++i) sum += static_cast<double>(v[i]) * v[i];
    return std::sqrt(sum / static_cast<double>(len));
  };
  const double levelA = rms(before, before.size() - 4800, 4800);
  const double levelB = rms(after, after.size() - 4800, 4800);
  const double justAfter = rms(after, 0, 96);  // first 2 ms of a 30 ms fade
  std::printf("[ScenesTest] EQ switch: A %.4f, first 2 ms %.4f, B %.4f\n", levelA, justAfter,
              levelB);
  ASSERT_GT(levelB, 4.0 * levelA) << "fixture: channel B should be much louder at 1 kHz";
  EXPECT_LT(justAfter, levelA + 0.25 * (levelB - levelA)) << "the EQ stepped instead of fading";
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

// Channels holding different tones. The rig is a state tree: channel A on one
// tone, channel B on another with its bytes embedded, so nothing can come off
// the (fake) URLs.
juce::String channelToneJson(int toneId, int modelId, const char* title, const char* format,
                             const char* gear = "") {
  return "{\"id\":" + juce::String(toneId) + ",\"title\":\"" + title + "\",\"format\":\"" +
         format + "\",\"gear\":\"" + gear + "\",\"models\":[{\"id\":" + juce::String(modelId) +
         ",\"name\":\"" + title + "\",\"model_url\":\"https://test.invalid/" +
         juce::String(modelId) + "\"}]}";
}

void addToneChannel(juce::ValueTree& block, int index, int toneId, int modelId, const char* title,
                    const char* format, const char* fileName,
                    const char* irCategory = nullptr) {
  const juce::String json = channelToneJson(toneId, modelId, title, format);
  juce::ValueTree slot("ChainBlock");
  slot.setProperty("index", index, nullptr);
  slot.setProperty("toneId", toneId, nullptr);
  slot.setProperty("toneJson", json, nullptr);
  slot.setProperty("activeModelId", modelId, nullptr);
  slot.setProperty("mix", 1.0f, nullptr);
  if (irCategory != nullptr)
    slot.setProperty("irCategory", juce::String(irCategory), nullptr);
  slot.setProperty("channelModel",
                   juce::JSON::toString(juce::JSON::parse(json)["models"][0], true), nullptr);
  juce::ValueTree channels = block.getOrCreateChildWithName("Channels", nullptr);
  channels.appendChild(slot, nullptr);

  juce::MemoryBlock bytes;
  EXPECT_TRUE(testFile(fileName).loadFileAsData(bytes));
  juce::ValueTree cached("CachedModel");
  cached.setProperty("modelId", modelId, nullptr);
  cached.setProperty("data", juce::var(bytes), nullptr);
  block.getChildWithName("ModelCache").appendChild(cached, nullptr);
}

void restoreBlock(ChainTestProcessor& proc, const juce::ValueTree& block) {
  juce::ValueTree state("ChainSnapshot");
  juce::ValueTree lane("ChainBlocks");
  lane.appendChild(block, nullptr);
  state.appendChild(lane, nullptr);
  proc.restoreFromTree(state);
}

bool waitForChannelWarm(ChainTestProcessor& proc, const std::string& id, int channel) {
  const auto until = juce::Time::getMillisecondCounter() + 15000;
  while (!proc.isChannelWarm(id, channel) && juce::Time::getMillisecondCounter() < until)
    juce::Thread::sleep(20);
  return proc.isChannelWarm(id, channel);
}

double rmsOf(const std::vector<float>& v, size_t start, size_t len) {
  return std::sqrt(energy(v, start, len) / static_cast<double>(len));
}

// The whole point: two scenes on two different captures, switching instantly.
TEST(ScenesTest, ChannelsOnDifferentNamTonesSwitchGaplessAcrossScenes) {
  ChainTestProcessor proc;
  proc.setPlayConfigDetails(2, 2, kFs, 512);
  proc.prepareToPlay(kFs, 512);
  juce::ValueTree block = makeNamBlockTree("amp", 1, 100);
  addToneChannel(block, 1, 2, 200, "Other Amp", "nam", "a2-am-test-2.nam");
  restoreBlock(proc, block);
  ASSERT_TRUE(waitForChainLoaded(proc));
  ASSERT_TRUE(waitForChannelWarm(proc, "amp", 1));

  // Scene 2 = channel B; hop there and back so both directions are warm.
  ASSERT_TRUE(proc.selectScene(1));
  ASSERT_TRUE(proc.selectBlockChannel("amp", 1));
  ASSERT_TRUE(proc.selectScene(0));
  ASSERT_TRUE(waitForChainLoaded(proc));
  // Noise, not a sine: a single tone through two different amps can land in
  // antiphase and partly cancel mid-fade, which is the crossfade doing its
  // job, not a gap. Broadband signal shows a real dip for what it is.
  const auto noise = makeNoise(120 * 512, 11, 0.3f);
  processStereo(proc, noise);
  ASSERT_TRUE(waitForChannelWarm(proc, "amp", 1));

  auto hop = [&](int scene, int modelId, int toneId) {
    // Steady state, outgoing engine settled and pooled.
    const auto before = processStereo(proc, noise).first;
    ASSERT_TRUE(proc.isChannelWarm("amp", scene == 1 ? 1 : 0));
    const double t0 = juce::Time::getMillisecondCounterHiRes();
    ASSERT_TRUE(proc.selectScene(scene));
    const double ms = juce::Time::getMillisecondCounterHiRes() - t0;
    const auto after = processStereo(proc, noise).first;
    const juce::var row = proc.getChainState(-1)["chain"][0];
    EXPECT_EQ(static_cast<int>(row["activeModelId"]), modelId);
    EXPECT_EQ(static_cast<int>(row["tone"]["id"]), toneId);
    EXPECT_FALSE(static_cast<bool>(row["modelLoading"])) << "a warm switch never loads";
    const size_t win = 240;
    // Two amps differ in level: the floor is the quieter one's steady state.
    const double steady = std::min(rmsOf(before, before.size() - 8 * win, 8 * win),
                                   rmsOf(after, after.size() - 8 * win, 8 * win));
    double worst = 1e9;
    for (size_t w = 0; w < 20; ++w)
      worst = std::min(worst, rmsOf(after, w * win, win));
    std::printf("[ScenesTest] tone switch to scene %d: %.3f ms, worst vs quieter amp %.2f dB\n",
                scene + 1, ms, 20.0 * std::log10(worst / steady));
    EXPECT_LT(ms, 0.5) << "the switch stalls the audio thread";
    EXPECT_GT(worst, steady * 0.6) << "the switch dipped - not gapless";
  };
  hop(1, 200, 2);
  hop(0, 100, 1);
  hop(1, 200, 2);

  // A session keeps both tones, and comes back warm from the embedded bytes.
  juce::MemoryBlock data;
  proc.getStateInformation(data);
  ChainTestProcessor restored;
  restored.setPlayConfigDetails(2, 2, kFs, 512);
  restored.prepareToPlay(kFs, 512);
  restored.setStateInformation(data.getData(), static_cast<int>(data.getSize()));
  ASSERT_TRUE(waitForChainLoaded(restored));
  EXPECT_EQ(static_cast<int>(restored.getChainState(-1)["chain"][0]["tone"]["id"]), 2);
  ASSERT_TRUE(waitForChannelWarm(restored, "amp", 0));
  ASSERT_TRUE(restored.selectScene(0));
  EXPECT_EQ(static_cast<int>(restored.getChainState(-1)["chain"][0]["tone"]["id"]), 1);
  EXPECT_EQ(static_cast<int>(restored.getChainState(-1)["chain"][0]["activeModelId"]), 100);
  EXPECT_FALSE(static_cast<bool>(restored.getChainState(-1)["chain"][0]["modelLoading"]));
}

// Spillover between two different reverbs: the first one's tail plays on after
// the switch, while the second one is already live.
TEST(ScenesTest, ReverbTailSpillsAcrossChannelsOnDifferentIrTones) {
  auto rig = [](ChainTestProcessor& proc) {
    proc.setPlayConfigDetails(2, 2, kFs, 512);
    proc.prepareToPlay(kFs, 512);
    juce::ValueTree block = makeIrBlockTree("verb", 1, 100, "reverb-ir-6s-test.wav", "irPlayer");
    addToneChannel(block, 1, 2, 200, "Short", "ir", "cab-ir-test.wav", "cab");
    restoreBlock(proc, block);
    ASSERT_TRUE(waitForChainLoaded(proc));
    ASSERT_TRUE(waitForChannelWarm(proc, "verb", 1));
    processStereo(proc, std::vector<float>(40 * 512, 0.0f));
  };
  ChainTestProcessor ref, test;
  rig(ref);
  rig(test);
  const auto in = burst();
  const std::vector<float> silence(96 * 512, 0.0f);
  processStereo(ref, in);
  processStereo(test, in);
  const double t0 = juce::Time::getMillisecondCounterHiRes();
  ASSERT_TRUE(test.selectBlockChannel("verb", 1));
  const double ms = juce::Time::getMillisecondCounterHiRes() - t0;
  const juce::var row = test.getChainState(-1)["chain"][0];
  EXPECT_EQ(static_cast<int>(row["tone"]["id"]), 2);
  EXPECT_FALSE(static_cast<bool>(row["modelLoading"]));
  const auto refTail = processStereo(ref, silence).first;
  const auto testTail = processStereo(test, silence).first;
  const double r = energy(refTail, 4800, 19200), t = energy(testTail, 4800, 19200);
  std::printf("[ScenesTest] IR tone switch %.3f ms (history included), tail %.3f of reference\n",
              ms, t / r);
  EXPECT_GT(t, 0.7 * r) << "the switch cut the other reverb's tail";
}

// A channel's kernel warms up as what that channel is: a long reverb behind a
// block currently playing a short cab must not be truncated to Cab length.
TEST(ScenesTest, WarmIrKeepsItsChannelsCategory) {
  ChainTestProcessor proc;
  proc.setPlayConfigDetails(2, 2, kFs, 512);
  proc.prepareToPlay(kFs, 512);
  juce::ValueTree block = makeIrBlockTree("ir", 1, 100, "cab-ir-test.wav", "cab");
  addToneChannel(block, 1, 2, 200, "Hall", "ir", "reverb-ir-6s-test.wav", "irPlayer");
  restoreBlock(proc, block);
  ASSERT_TRUE(waitForChainLoaded(proc));
  ASSERT_TRUE(waitForChannelWarm(proc, "ir", 1));
  ASSERT_TRUE(proc.selectBlockChannel("ir", 1));
  processStereo(proc, std::vector<float>(40 * 512, 0.0f));
  processStereo(proc, burst());
  const auto tail = processStereo(proc, std::vector<float>(192 * 512, 0.0f)).first;
  EXPECT_GT(rmsOf(tail, 72000, 12000), 1.0e-5) << "the reverb was cut at the Cab limit";
}

TEST(ScenesTest, ABlockKeepsItsKind) {
  const juce::String ir = channelToneJson(9, 900, "An IR", "ir");
  const juce::String cab = channelToneJson(10, 901, "A Cab", "ir", "cab");
  const juce::String nam = channelToneJson(11, 902, "An Amp", "nam");

  ChainTestProcessor proc;
  juce::ValueTree amp = makeNamBlockTree("amp", 1, 100);
  addToneChannel(amp, 1, 2, 200, "Other Amp", "nam", "a2-am-test-2.nam");
  restoreBlock(proc, amp);
  ASSERT_TRUE(waitForChainLoaded(proc));
  EXPECT_FALSE(proc.swapTone("amp", ir));
  EXPECT_FALSE(proc.swapTone("amp", cab));
  EXPECT_TRUE(proc.toneKindConflict("amp", ir).contains("NAM"));
  EXPECT_TRUE(proc.toneKindConflict("amp", nam).isEmpty());
  const juce::var row = proc.getChainState(-1)["chain"][0];
  EXPECT_EQ(static_cast<int>(row["activeModelId"]), 100);
  EXPECT_TRUE(static_cast<bool>(row["params"]["channelsUsed"][1])) << "channels stay";

  ChainTestProcessor irProc;
  restoreBlock(irProc, makeIrBlockTree("ir", 1, 100));
  ASSERT_TRUE(waitForChainLoaded(irProc));
  EXPECT_TRUE(irProc.toneKindConflict("ir", nam).isNotEmpty());
  EXPECT_TRUE(irProc.toneKindConflict("ir", cab).isNotEmpty());
  EXPECT_TRUE(irProc.toneKindConflict("ir", ir).isEmpty());
}

TEST(ScenesTest, CopyChannelCarriesItsTone) {
  ChainTestProcessor proc;
  juce::ValueTree amp = makeNamBlockTree("amp", 1, 100);
  addToneChannel(amp, 1, 2, 200, "Other Amp", "nam", "a2-am-test-2.nam");
  restoreBlock(proc, amp);
  ASSERT_TRUE(waitForChainLoaded(proc));
  ASSERT_TRUE(proc.copyBlockChannel("amp", 1, 2));
  ASSERT_TRUE(waitForChannelWarm(proc, "amp", 2));
  ASSERT_TRUE(proc.selectBlockChannel("amp", 2));
  const juce::var row = proc.getChainState(-1)["chain"][0];
  EXPECT_EQ(static_cast<int>(row["tone"]["id"]), 2);
  EXPECT_EQ(static_cast<int>(row["activeModelId"]), 200);
}

// Undo across a channel switch crosses tones: the block survives with its
// cached bytes (undo snapshots carry none, and the URLs here are fake).
TEST(ScenesTest, UndoAcrossChannelsOnDifferentTonesReloadsFromCache) {
  ChainTestProcessor proc;
  juce::ValueTree amp = makeNamBlockTree("amp", 1, 100);
  addToneChannel(amp, 1, 2, 200, "Other Amp", "nam", "a2-am-test-2.nam");
  restoreBlock(proc, amp);
  ASSERT_TRUE(waitForChainLoaded(proc));
  ASSERT_TRUE(waitForChannelWarm(proc, "amp", 1));
  ASSERT_TRUE(proc.selectBlockChannel("amp", 1));
  ASSERT_TRUE(proc.undoChain());
  ASSERT_TRUE(waitForChainLoaded(proc));
  const auto until = juce::Time::getMillisecondCounter() + 10000;
  while (static_cast<bool>(proc.getChainState(-1)["chain"][0]["modelLoading"]) &&
         juce::Time::getMillisecondCounter() < until)
    juce::Thread::sleep(20);
  const juce::var row = proc.getChainState(-1)["chain"][0];
  EXPECT_EQ(static_cast<int>(row["tone"]["id"]), 1);
  EXPECT_EQ(static_cast<int>(row["activeModelId"]), 100);
  EXPECT_TRUE(static_cast<bool>(row["loaded"]));
  EXPECT_FALSE(static_cast<bool>(row["loadFailed"]));
  EXPECT_TRUE(waitForChannelWarm(proc, "amp", 1));
}

// Leaving a channel mid-load: the engine then playing is still the previous
// model, and must not come back under the new model's name.
TEST(ScenesTest, LeavingAChannelMidLoadDoesNotMislabelItsEngine) {
  auto modelBytes = [](const char* file) {
    juce::MemoryBlock bytes;
    EXPECT_TRUE(testFile(file).loadFileAsData(bytes));
    return juce::Base64::toBase64(bytes.getData(), bytes.getSize());
  };
  ChainTestProcessor proc;
  proc.setPlayConfigDetails(2, 2, kFs, 512);
  proc.prepareToPlay(kFs, 512);
  juce::ValueTree amp = makeNamBlockTree("amp", 1, 100);
  addToneChannel(amp, 1, 2, 200, "Other Amp", "nam", "a2-am-test-2.nam");
  restoreBlock(proc, amp);
  ASSERT_TRUE(waitForChainLoaded(proc));
  ASSERT_TRUE(waitForChannelWarm(proc, "amp", 1));

  auto* entry = new juce::DynamicObject();
  entry->setProperty("name", "a2-amp-cab-test.nam");
  entry->setProperty("data", modelBytes("a2-amp-cab-test.nam"));
  juce::Array<juce::var> files;
  files.add(juce::var(entry));
  const juce::var swapped = proc.loadLocalTone("a2-amp-cab-test", files, "amp");
  ASSERT_TRUE(swapped["error"].isVoid()) << swapped["error"].toString().toStdString();
  const int newModel = static_cast<int>(proc.getChainState(-1)["chain"][0]["activeModelId"]);
  ASSERT_NE(newModel, 100);
  ASSERT_TRUE(proc.selectBlockChannel("amp", 1));  // leaves before the load lands

  processStereo(proc, makeSine(60 * 512, 220.0, 0.3f));
  ASSERT_TRUE(waitForChannelWarm(proc, "amp", 0));
  // Same process, same bytes: what comes back must be the file just dropped.
  ASSERT_TRUE(proc.selectBlockChannel("amp", 0));
  const juce::var row = proc.getChainState(-1)["chain"][0];
  EXPECT_EQ(static_cast<int>(row["activeModelId"]), newModel);
  EXPECT_EQ(row["tone"]["title"].toString(), juce::String("a2-amp-cab-test"));
  EXPECT_FALSE(static_cast<bool>(row["modelLoading"]));

  ChainTestProcessor ref;
  ref.setPlayConfigDetails(2, 2, kFs, 512);
  ref.prepareToPlay(kFs, 512);
  ASSERT_TRUE(ref.loadLocalTone("a2-amp-cab-test", files)["error"].isVoid());
  ASSERT_TRUE(waitForChainLoaded(ref));
  const auto sine = makeSine(200 * 512, 220.0, 0.3f);
  const auto got = processStereo(proc, sine).first;
  const auto want = processStereo(ref, sine).first;
  const size_t from = 120 * 512, len = 60 * 512;
  EXPECT_NEAR(rmsOf(got, from, len), rmsOf(want, from, len), 0.1 * rmsOf(want, from, len))
      << "the engine now playing isn't the model the block names";
}

}  // namespace
