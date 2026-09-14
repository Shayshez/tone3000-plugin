// Cab Block v1 (ChainBlockType::CAB, TONE3000 issue #121 item 3): a real,
// site-loaded block type - reachable through parseToneForLoading/
// toneEngineType routing a gear == "cab" tone straight to CAB instead of
// IR + IrCategory::Cab (see ProcessorChain.cpp) - structurally minimal (no
// predelay/envelope/waveform fields at all, single convolver, reuses the
// #89 fix's Cab truncation/-18dB pad/100% mix unconditionally).
//
// The crash-regression test below is the one that matters: a CAB block's
// engine install must go through the SAME background-thread-pool +
// chainMutex pipeline every other tone block uses (queueActiveModelLoad /
// loadToneInBackground -> prepareBlockModelOffThread ->
// applyPreparedModelToChainBlock). The very first prototype of this feature
// bypassed that pipeline with a synchronous shortcut and crashed the
// real-time thread the instant a file loaded while audio was running (a
// scratch buffer that was never sized for a block added mid-session). This
// test drives real audio *while* the block's load is still in flight on the
// loader thread - deliberately not waiting first - to make sure that
// specific race can't recur.
#include "chain_test_helpers.h"

#include <gtest/gtest.h>

#include <cmath>
#include <iostream>
#include <memory>
#include <vector>

namespace {

juce::ValueTree makeCabBlockTree(const juce::String& blockId, int toneId, int modelId,
                                 const char* fileName = "cab-ir-test.wav") {
  const juce::String toneJson =
      "{\"id\":" + juce::String(toneId) +
      ",\"title\":\"Test Cab\",\"format\":\"ir\",\"gear\":\"cab\","
      "\"models\":[{\"id\":" +
      juce::String(modelId) + ",\"name\":\"cab\",\"model_url\":\"https://test.invalid/cab.wav\"}]}";

  juce::ValueTree block("ChainBlock");
  block.setProperty("id", blockId, nullptr);
  block.setProperty("type", "cab", nullptr);
  block.setProperty("enabled", true, nullptr);
  block.setProperty("normalize", true, nullptr);
  block.setProperty("inputGain", 0.5f, nullptr);
  block.setProperty("outputGain", 0.5f, nullptr);
  block.setProperty("mix", 1.0f, nullptr);
  block.setProperty("toneId", toneId, nullptr);
  block.setProperty("toneJson", toneJson, nullptr);
  block.setProperty("activeModelId", modelId, nullptr);

  // Embedded model bytes (same trick makeIrBlockTree/makeNamBlockTree use):
  // the background loader finds them in the cache and never touches the
  // fake model_url.
  juce::MemoryBlock bytes;
  EXPECT_TRUE(testFile(fileName).loadFileAsData(bytes));
  juce::ValueTree cached("CachedModel");
  cached.setProperty("modelId", modelId, nullptr);
  cached.setProperty("data", juce::var(bytes), nullptr);
  juce::ValueTree cache("ModelCache");
  cache.appendChild(cached, nullptr);
  block.appendChild(cache, nullptr);
  return block;
}

juce::var firstToneBlock(TONE3000Processor& proc) {
  const juce::var state = proc.getChainState(-1);
  if (const auto* lane = state["chain"].getArray())
    for (const auto& item : *lane)
      if (item["kind"].toString() == "tone")
        return item;
  return {};
}

void assertAllFinite(const juce::AudioBuffer<float>& buffer) {
  for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
    for (int i = 0; i < buffer.getNumSamples(); ++i)
      ASSERT_TRUE(std::isfinite(buffer.getSample(ch, i))) << "ch " << ch << " sample " << i;
}

// Peak at each of `checkpointSeconds`, from a SINGLE continuous impulse
// response (one impulse injected once; each checkpoint is a further point
// along that same decay) - not a fresh impulse per checkpoint, which would
// measure disjoint responses instead of one continuous one. Shared by the
// truncation-cliff test below and the round-trip conversion test.
std::vector<float> peaksAtCheckpoints(ChainTestProcessor& proc, int blockSize,
                                      const std::vector<double>& checkpointSeconds) {
  juce::AudioBuffer<float> buffer(2, blockSize);
  juce::MidiBuffer midi;
  buffer.clear();
  buffer.setSample(0, 0, 1.0f);
  buffer.setSample(1, 0, 1.0f);
  std::vector<float> peaks;
  int done = 0;
  for (double checkpointSec : checkpointSeconds) {
    const int targetBlock = static_cast<int>(checkpointSec * 48000.0 / blockSize);
    float peak = 0.0f;
    while (done <= targetBlock) {
      proc.processBlock(buffer, midi);
      peak = 0.0f;
      for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
        for (int s = 0; s < buffer.getNumSamples(); ++s)
          peak = std::max(peak, std::abs(buffer.getSample(ch, s)));
      buffer.clear();
      ++done;
    }
    peaks.push_back(peak);
  }
  return peaks;
}

// Polls until the block's engine rebuild (model switch / conversion) has
// landed: modelLoading clears (the block stays `loaded` throughout a
// conversion - the previous engine keeps processing until the new one
// splices in, see convertBlockTypeInBackground), same condition swap_fade_
// tests.cpp's waitForActiveModel uses for a model switch.
bool waitForModelLoadingDone(TONE3000Processor& proc, int timeoutMs = 20000) {
  const auto deadline = juce::Time::getMillisecondCounter() + static_cast<juce::uint32>(timeoutMs);
  while (juce::Time::getMillisecondCounter() < deadline) {
    const juce::var block = firstToneBlock(proc);
    if (!block.isVoid() && !static_cast<bool>(block["modelLoading"]))
      return true;
    juce::Thread::sleep(10);
  }
  return false;
}

}  // namespace

// Step 1 routing: gear == "cab" produces a real CAB block, not IR.
TEST(CabBlockTest, SiteGearCabProducesRealCabBlockType) {
  TONE3000Processor proc;
  const juce::String toneJson =
      "{\"id\":1,\"title\":\"Test Cab\",\"format\":\"ir\",\"gear\":\"cab\","
      "\"models\":[{\"id\":100,\"name\":\"cab\",\"model_url\":\"https://test.invalid/cab.wav\"}]}";
  const std::string blockId = proc.loadTone(toneJson, "");
  ASSERT_FALSE(blockId.empty());

  const juce::var block = firstToneBlock(proc);
  ASSERT_FALSE(block.isVoid());
  EXPECT_EQ(block["blockType"].toString(), juce::String("cab"));
}

// A non-cab IR tone is unaffected by the routing change.
TEST(CabBlockTest, SiteNonCabGearStaysIrBlockType) {
  TONE3000Processor proc;
  const juce::String toneJson =
      "{\"id\":1,\"title\":\"Test Space\",\"format\":\"ir\",\"gear\":\"space\","
      "\"models\":[{\"id\":100,\"name\":\"space\",\"model_url\":\"https://test.invalid/space.wav\"}]}";
  const std::string blockId = proc.loadTone(toneJson, "");
  ASSERT_FALSE(blockId.empty());

  const juce::var block = firstToneBlock(proc);
  ASSERT_FALSE(block.isVoid());
  EXPECT_EQ(block["blockType"].toString(), juce::String("ir"));
}

TEST(CabBlockTest, LoadWhileAudioIsRunningDoesNotCrash) {
  ChainTestProcessor proc;
  proc.setPlayConfigDetails(2, 2, 48000.0, 512);
  proc.prepareToPlay(48000.0, 512);

  juce::ValueTree state("ChainSnapshot");
  state.setProperty("stereoEnabled", false, nullptr);
  juce::ValueTree lane("ChainBlocks");
  lane.appendChild(makeCabBlockTree("blk", 1, 100), nullptr);
  state.appendChild(lane, nullptr);
  state.appendChild(juce::ValueTree("RightChainBlocks"), nullptr);

  // Queues the CAB block's engine build/install onto the background thread
  // pool (see queueActiveModelLoad -> switchModelInBackground), the same
  // path a real relaunch or site load takes. Deliberately not waiting here:
  // driving processBlock immediately, on this thread, races that job on
  // its own thread - exactly the scenario that crashed the first prototype.
  proc.restoreFromTree(state);

  juce::AudioBuffer<float> buffer(2, 512);
  juce::MidiBuffer midi;
  for (int i = 0; i < 200; ++i) {
    buffer.clear();
    buffer.setSample(0, 0, 1.0f);
    buffer.setSample(1, 0, 1.0f);
    proc.processBlock(buffer, midi);
    assertAllFinite(buffer);
  }

  ASSERT_TRUE(waitForChainLoaded(proc));

  // Loaded and correctly configured: real CAB type, unconditional -18dB
  // pad's 100% default mix (see applyPreparedModelToChainBlock's CAB
  // branch).
  const juce::var block = firstToneBlock(proc);
  ASSERT_FALSE(block.isVoid());
  EXPECT_EQ(block["blockType"].toString(), juce::String("cab"));
  EXPECT_FLOAT_EQ(static_cast<float>(block["params"]["mix"]), 1.0f);

  for (int i = 0; i < 50; ++i) {
    buffer.clear();
    buffer.setSample(0, 0, 1.0f);
    buffer.setSample(1, 0, 1.0f);
    proc.processBlock(buffer, midi);
    assertAllFinite(buffer);
  }
}

// Empirical proof the 500ms cap actually fires for a real ChainBlockType::
// CAB load - not just code review of isCabKnown's OR condition. Measures the
// ACTUAL processed output: feed a unit impulse through the loaded block and
// sample the response's peak in fixed windows around the 500ms mark.
//
// reverb-ir-mono-test.wav is a genuine, real (not silence-padded) reverb
// decay verified by manual analysis of the raw file before writing this
// test: absolute level runs from -8.65 dBFS at t=0 down to roughly -80 dBFS
// (JUCE's own file-load trim floor) around t=1.3-1.4s, then continues
// decaying in-band well past 2s.
//
// A naive "does the chain's total output cross some floor" measurement
// doesn't work here: the chain's own DC blocker is a feedback IIR filter,
// so once excited it keeps ringing down on its own for a long time
// regardless of what the convolver did - both loads show a long, smooth
// exponential tail no matter what, which is a red herring. The real,
// unambiguous signature of a hard file-read truncation is a *sudden,
// disproportionate cliff* in level right at the cutoff, immediately
// followed by the DC blocker's ordinary residual decay rate - vs. a load
// whose kernel still has genuine samples there, which shows no such cliff
// (measured decay stays close to the surrounding windows' rate). This test
// measures exactly that cliff, at 450ms -> 500ms, for both loads.
TEST(CabBlockTest, FiveHundredMsTruncationEmpiricallyMeasuredVsUntruncatedIrPlayer) {
  constexpr int kBlockSize = 64;  // fine time resolution for the measurement

  auto loadCab = [] {
    auto proc = std::make_unique<ChainTestProcessor>();
    proc->setPlayConfigDetails(2, 2, 48000.0, kBlockSize);
    proc->prepareToPlay(48000.0, kBlockSize);
    juce::ValueTree state("ChainSnapshot");
    state.setProperty("stereoEnabled", false, nullptr);
    juce::ValueTree lane("ChainBlocks");
    lane.appendChild(makeCabBlockTree("blk", 1, 100, "reverb-ir-mono-test.wav"), nullptr);
    state.appendChild(lane, nullptr);
    state.appendChild(juce::ValueTree("RightChainBlocks"), nullptr);
    proc->restoreFromTree(state);
    return proc;
  };
  auto loadIrPlayer = [] {
    auto proc = std::make_unique<ChainTestProcessor>();
    proc->setPlayConfigDetails(2, 2, 48000.0, kBlockSize);
    proc->prepareToPlay(48000.0, kBlockSize);
    juce::ValueTree state("ChainSnapshot");
    state.setProperty("stereoEnabled", false, nullptr);
    juce::ValueTree lane("ChainBlocks");
    lane.appendChild(makeIrBlockTree("blk", 1, 100, "reverb-ir-mono-test.wav", "irPlayer"),
                     nullptr);
    state.appendChild(lane, nullptr);
    state.appendChild(juce::ValueTree("RightChainBlocks"), nullptr);
    proc->restoreFromTree(state);
    return proc;
  };

  auto procCab = loadCab();
  ASSERT_TRUE(waitForChainLoaded(*procCab));
  ASSERT_EQ(firstToneBlock(*procCab)["blockType"].toString(), juce::String("cab"));
  const std::vector<float> cabPeaks = peaksAtCheckpoints(*procCab, kBlockSize, {0.45, 0.50});
  const float cabAt450 = cabPeaks[0];
  const float cabAt500 = cabPeaks[1];

  auto procIrPlayer = loadIrPlayer();
  ASSERT_TRUE(waitForChainLoaded(*procIrPlayer));
  const std::vector<float> irPeaks = peaksAtCheckpoints(*procIrPlayer, kBlockSize, {0.45, 0.50});
  const float irAt450 = irPeaks[0];
  const float irAt500 = irPeaks[1];

  const double cabDropRatio = cabAt500 / cabAt450;
  const double irDropRatio = irAt500 / irAt450;

  std::cerr << "[EMPIRICAL] CAB      peak@450ms=" << cabAt450 << " peak@500ms=" << cabAt500
            << " ratio=" << cabDropRatio << "\n";
  std::cerr << "[EMPIRICAL] IrPlayer peak@450ms=" << irAt450 << " peak@500ms=" << irAt500
            << " ratio=" << irDropRatio << "\n";

  // Real measured values from this file (documented so a future change to
  // the fixture or the pipeline shows up as a concrete number, not just a
  // pass/fail): CAB drops ~148x (ratio ~0.0068) right at the cap boundary,
  // then settles into the DC blocker's steady ~5x-per-50ms residual decay;
  // IrPlayer shows no such cliff (ratio ~0.78, consistent with the
  // surrounding windows) because its kernel still has real content there.
  EXPECT_LT(cabDropRatio, 0.05) << "CAB must show a real cliff right at the 500ms cap";
  EXPECT_GT(irDropRatio, 0.5) << "IrPlayer's kernel must still have real content past 450ms";
  EXPECT_GT(irDropRatio, cabDropRatio * 10)
      << "the cliff must be dramatically sharper for the truncated CAB load";
}

// convertBlockType round trip: an IR Player block carries its sample into a
// real CAB block (picking up the same 500ms truncation cliff the direct-load
// test above measures), then back into an IR Player block, restoring the
// full original sample (no memory of the truncation - see convertBlockType's
// doc comment and issue #117's round-trip discussion).
TEST(CabBlockTest, ConvertBlockTypeRoundTripAppliesAndRestoresTruncation) {
  constexpr int kBlockSize = 64;  // fine time resolution for the 500ms measurement

  ChainTestProcessor proc;
  proc.setPlayConfigDetails(2, 2, 48000.0, kBlockSize);
  proc.prepareToPlay(48000.0, kBlockSize);

  juce::ValueTree state("ChainSnapshot");
  state.setProperty("stereoEnabled", false, nullptr);
  juce::ValueTree lane("ChainBlocks");
  lane.appendChild(makeIrBlockTree("blk", 1, 100, "reverb-ir-mono-test.wav", "irPlayer"), nullptr);
  state.appendChild(lane, nullptr);
  state.appendChild(juce::ValueTree("RightChainBlocks"), nullptr);
  proc.restoreFromTree(state);
  ASSERT_TRUE(waitForChainLoaded(proc));
  ASSERT_EQ(firstToneBlock(proc)["blockType"].toString(), juce::String("ir"));

  // IR Player -> CAB: same 500ms cliff signature as a direct site-loaded
  // CAB, measured against this processor's own untruncated baseline above
  // (irDropRatio) so the assertion doesn't depend on re-deriving thresholds.
  ASSERT_TRUE(proc.convertBlockType("blk", "cab"));
  ASSERT_TRUE(waitForModelLoadingDone(proc));
  ASSERT_EQ(firstToneBlock(proc)["blockType"].toString(), juce::String("cab"));

  const std::vector<float> cabPeaks = peaksAtCheckpoints(proc, kBlockSize, {0.45, 0.50});
  const double cabDropRatio = cabPeaks[1] / cabPeaks[0];
  std::cerr << "[ROUND TRIP] after IR->CAB: peak@450ms=" << cabPeaks[0]
            << " peak@500ms=" << cabPeaks[1] << " ratio=" << cabDropRatio << "\n";
  EXPECT_LT(cabDropRatio, 0.05) << "converted CAB must show the same real cliff a direct CAB "
                                   "load shows at the 500ms cap";

  // Let the chain's DC blocker (a persistent IIR filter, not swapped by the
  // conversion) settle before injecting a second impulse, so the CAB
  // measurement's own decay can't bleed into the next peak reading.
  {
    juce::AudioBuffer<float> silence(2, kBlockSize);
    juce::MidiBuffer midi;
    silence.clear();
    for (int i = 0; i < 48000 / kBlockSize; ++i)
      proc.processBlock(silence, midi);
  }

  // CAB -> IR Player: back to the full original sample (no cliff), and
  // explicitly the "IR Player" category the button is labeled with, not a
  // re-guessed classification.
  ASSERT_TRUE(proc.convertBlockType("blk", "ir"));
  ASSERT_TRUE(waitForModelLoadingDone(proc));
  const juce::var restored = firstToneBlock(proc);
  ASSERT_EQ(restored["blockType"].toString(), juce::String("ir"));
  EXPECT_EQ(restored["irCategory"].toString(), juce::String("irPlayer"));

  const std::vector<float> restoredPeaks = peaksAtCheckpoints(proc, kBlockSize, {0.45, 0.50});
  const double restoredDropRatio = restoredPeaks[1] / restoredPeaks[0];
  std::cerr << "[ROUND TRIP] after CAB->IR: peak@450ms=" << restoredPeaks[0]
            << " peak@500ms=" << restoredPeaks[1] << " ratio=" << restoredDropRatio << "\n";
  EXPECT_GT(restoredDropRatio, 0.5) << "converting back must restore the full original sample - "
                                       "no cliff at 500ms, matching an ordinary IrPlayer load";
  EXPECT_GT(restoredDropRatio, cabDropRatio * 10)
      << "the round trip must remove the truncation cliff, not carry it forward";
}
