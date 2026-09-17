// Pins Trim Init (setBlockIrTrimInit/ChainBlock::trimInitEnabled): a manual
// toggle that shifts prepareIrShapeRebuild's copy-into-the-kernel start
// point from raw sample 0 to the block's detected onset
// (ChainBlock::irOnsetSamples, computeIrOnsetSamples - the forward-scanning
// mirror of computeIrContentLengthSamples). Off by default: a source with
// real leading silence would otherwise bake it into every convolution
// kernel, indistinguishable from an unwanted extra Predelay - this feature
// lets a user who notices that strip it out, without touching Predelay's
// own (real-time, genuinely-added) delay semantics.
//
// reverb-ir-silence-prefix-test.wav is reverb-ir-mono-test.wav with exactly
// 300ms of true digital silence prepended (sox `pad 0.3 0`); its own
// detected content should land ~300ms longer than the un-prefixed fixture
// with Trim Init off, and close to matching it with Trim Init on.
#include "chain_test_helpers.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace {
constexpr int kBlock = 512;

void seedMonoIrChain(ChainTestProcessor& proc, const juce::String& blockId,
                     const char* fileName) {
  juce::ValueTree state("ChainSnapshot");
  state.setProperty("stereoEnabled", false, nullptr);
  juce::ValueTree lane("ChainBlocks");
  lane.appendChild(makeIrBlockTree(blockId, 1, 100, fileName), nullptr);
  state.appendChild(lane, nullptr);
  state.appendChild(juce::ValueTree("RightChainBlocks"), nullptr);
  proc.restoreFromTree(state);
}

// Mirrors ir_size_width_tests.cpp's own pumpAudio: keeps the heartbeat alive
// through the background rebuild + wet-mute swap fade.
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

bool setTrimInitAndWaitForRebuild(ChainTestProcessor& proc, const char* blockId, bool enabled) {
  const std::vector<float> silence(static_cast<size_t>(kBlock), 0.0f);
  processStereo(proc, silence);  // prime
  const bool ok = proc.setBlockIrTrimInit(blockId, enabled);
  pumpAudio(proc, 1200, /*alreadyWarm=*/true);
  return ok;
}

constexpr int kImpulseBlock = 15;
constexpr size_t kImpulseOnset = static_cast<size_t>(kImpulseBlock) * kBlock;

std::vector<float> makeDelayedImpulse(int totalBlocks) {
  std::vector<float> impulse(static_cast<size_t>(totalBlocks * kBlock), 0.0f);
  impulse[kImpulseOnset] = 1.0f;
  return impulse;
}

double irContentLengthMs(ChainTestProcessor& proc, const char* blockId) {
  const juce::var state = proc.getChainState(-1);
  if (const auto* lane = state["chain"].getArray())
    for (const auto& item : *lane)
      if (item["blockId"].toString() == blockId)
        return static_cast<double>(item["irContentLengthMs"]);
  return -1.0;
}

bool trimInitOf(ChainTestProcessor& proc, const char* blockId) {
  const juce::var state = proc.getChainState(-1);
  if (const auto* lane = state["chain"].getArray())
    for (const auto& item : *lane)
      if (item["blockId"].toString() == blockId)
        return static_cast<bool>(item["params"]["trimInit"]);
  return false;
}

bool trimRelaxedOf(ChainTestProcessor& proc, const char* blockId) {
  const juce::var state = proc.getChainState(-1);
  if (const auto* lane = state["chain"].getArray())
    for (const auto& item : *lane)
      if (item["blockId"].toString() == blockId)
        return static_cast<bool>(item["params"]["trimRelaxed"]);
  return false;
}

double onsetFractionOf(ChainTestProcessor& proc, const char* blockId) {
  const juce::var state = proc.getChainState(-1);
  if (const auto* lane = state["chain"].getArray())
    for (const auto& item : *lane)
      if (item["blockId"].toString() == blockId)
        return static_cast<double>(item["irOnsetFraction"]);
  return -1.0;
}
}  // namespace

// Off by default (never applied automatically), and doesn't need a rebuild
// to report that - the persisted value round-trips through getChainState as
// soon as the block loads.
TEST(TrimInitTest, DefaultsOff) {
  ChainTestProcessor proc;
  proc.setPlayConfigDetails(2, 2, kFs, kBlock);
  proc.prepareToPlay(kFs, kBlock);
  seedMonoIrChain(proc, "blk-a", "reverb-ir-silence-prefix-test.wav");
  ASSERT_TRUE(waitForChainLoaded(proc));

  EXPECT_FALSE(trimInitOf(proc, "blk-a"));
  EXPECT_FALSE(trimRelaxedOf(proc, "blk-a"));
}

// The core behavior: enabling Trim Init on a source with real leading
// silence shrinks the reported content length by roughly the silence's own
// duration (300ms), and lands close to what the same content minus that
// prefix reports on its own (reverb-ir-mono-test.wav, the un-prefixed
// fixture) - proving this is genuine onset detection, not an arbitrary
// fixed shift.
TEST(TrimInitTest, EnablingShrinksReportedLengthBySilenceDuration) {
  ChainTestProcessor withPrefix;
  withPrefix.setPlayConfigDetails(2, 2, kFs, kBlock);
  withPrefix.prepareToPlay(kFs, kBlock);
  seedMonoIrChain(withPrefix, "blk-a", "reverb-ir-silence-prefix-test.wav");
  ASSERT_TRUE(waitForChainLoaded(withPrefix));
  const double beforeMs = irContentLengthMs(withPrefix, "blk-a");

  ASSERT_TRUE(setTrimInitAndWaitForRebuild(withPrefix, "blk-a", true));
  const double afterMs = irContentLengthMs(withPrefix, "blk-a");

  ChainTestProcessor reference;
  reference.setPlayConfigDetails(2, 2, kFs, kBlock);
  reference.prepareToPlay(kFs, kBlock);
  seedMonoIrChain(reference, "blk-a", "reverb-ir-mono-test.wav");
  ASSERT_TRUE(waitForChainLoaded(reference));
  const double referenceMs = irContentLengthMs(reference, "blk-a");

  std::printf(
      "[TrimInitTest] silence-prefixed content: off=%.1fms on=%.1fms | un-prefixed "
      "reference=%.1fms\n",
      beforeMs, afterMs, referenceMs);

  EXPECT_NEAR(beforeMs - afterMs, 300.0, 80.0)
      << "Trim Init didn't shrink the reported length by roughly the added silence";
  // Looser than the check above: the onset detector backs off from its raw
  // threshold-crossing by its OWN margin rule (10% of the detected onset),
  // while computeIrContentLengthSamples's end-margin uses a different rule
  // (10% of the detected end) - the two don't cancel to an exact match, just
  // a close one. Not a bug, just two independently-tuned heuristics.
  EXPECT_NEAR(afterMs, referenceMs, 100.0)
      << "Trim Init-on content length doesn't match the same content without the prefix";
}

// irOnsetFraction (the waveform display's crop signal, ChainBlock.tsx) is
// always shipped regardless of trimInit, and expressed against
// irContentLengthSamples specifically (the exact span irWaveformPeaks was
// downsampled over - the *detected content end*, e.g. ~1536ms for this
// fixture per EnablingShrinksReportedLengthBySilenceDuration above, NOT the
// full ~2.97s raw file) - not the already-trim-adjusted
// irRawContentLengthMs, which would make this circular (see that field's
// own comment). That same test measured the margin-adjusted onset at
// ~269ms, so 269/1536 =~ 0.175 is the expected ratio here. Must stay
// identical whether Trim Init is on or off - it's a pure detection result,
// not something toggling the feature changes.
TEST(TrimInitTest, OnsetFractionIsShippedRegardlessOfTrimInitState) {
  ChainTestProcessor proc;
  proc.setPlayConfigDetails(2, 2, kFs, kBlock);
  proc.prepareToPlay(kFs, kBlock);
  seedMonoIrChain(proc, "blk-a", "reverb-ir-silence-prefix-test.wav");
  ASSERT_TRUE(waitForChainLoaded(proc));

  const double fractionOff = onsetFractionOf(proc, "blk-a");
  std::printf("[TrimInitTest] irOnsetFraction (Trim Init off): %.4f\n", fractionOff);
  EXPECT_NEAR(fractionOff, 0.175, 0.03)
      << "onset fraction doesn't match the expected ~269ms/1536ms ratio";

  ASSERT_TRUE(setTrimInitAndWaitForRebuild(proc, "blk-a", true));
  const double fractionOn = onsetFractionOf(proc, "blk-a");
  EXPECT_NEAR(fractionOn, fractionOff, 1e-6)
      << "irOnsetFraction changed when Trim Init was toggled - it should be a pure detection "
         "result, unaffected by whether the feature is applied";
}

// The reported content-length change (see the test above) must reflect a
// real engine change, not just getChainState arithmetic decoupled from what
// actually got built - rebuildIrShapeInBackground (which builds the real
// convolver) and getChainState's copyLane (which reports the length) are
// two independent call sites that both derive trimStartSamples from
// trimInitEnabled/irOnsetSamples separately; nothing but this kind of
// audio-level check guards against them silently drifting apart in a future
// edit. Not attempting to pin down *where* in time the difference shows up
// (a chain-wide artifact dominates the first ~300ms regardless of Trim
// Init, empirically - the DC blocker/other always-on stages responding to
// the impulse itself, not the loaded IR - which makes exact onset-timing
// assertions unreliable in this rig); a genuinely different kernel just
// needs to produce a genuinely different output somewhere.
TEST(TrimInitTest, ActuallyChangesTheConvolvedOutput) {
  constexpr int kTotalBlocks = 120;
  const auto impulse = makeDelayedImpulse(kTotalBlocks);

  auto runWithTrim = [&](bool enabled) {
    ChainTestProcessor proc;
    proc.setPlayConfigDetails(2, 2, kFs, kBlock);
    proc.prepareToPlay(kFs, kBlock);
    seedMonoIrChain(proc, "blk-a", "reverb-ir-silence-prefix-test.wav");
    EXPECT_TRUE(waitForChainLoaded(proc)) << "IR block never finished loading from cache";
    EXPECT_TRUE(proc.setBlockParam("blk-a", "mix", 1.0));
    // EXPECT not ASSERT: this lambda has an inferred non-void return type,
    // and ASSERT_TRUE expands to a bare `return;` on failure, which won't
    // typecheck against that (same reason ir_size_width_tests.cpp's own
    // runWithSize lambda uses EXPECT here too).
    if (enabled) EXPECT_TRUE(setTrimInitAndWaitForRebuild(proc, "blk-a", true));
    letAudioGoIdle();
    const auto [outL, outR] = processStereo(proc, impulse);
    juce::ignoreUnused(outR);
    return outL;
  };

  const auto outOff = runWithTrim(false);
  const auto outOn = runWithTrim(true);

  ASSERT_EQ(outOff.size(), outOn.size());
  float maxAbsDiff = 0.0f;
  for (size_t i = 0; i < outOff.size(); ++i)
    maxAbsDiff = std::max(maxAbsDiff, std::abs(outOff[i] - outOn[i]));

  std::printf("[TrimInitTest] max |diff| Trim Init off vs on: %.6f\n",
              static_cast<double>(maxAbsDiff));
  EXPECT_GT(maxAbsDiff, 0.01f)
      << "toggling Trim Init produced no measurable change in the actual convolved output";
}

// Must never fire on a source that has no real leading silence to find -
// it's detection-based, not a blanket forced shift.
TEST(TrimInitTest, NoOpOnSourceWithoutLeadingSilence) {
  ChainTestProcessor proc;
  proc.setPlayConfigDetails(2, 2, kFs, kBlock);
  proc.prepareToPlay(kFs, kBlock);
  seedMonoIrChain(proc, "blk-a", "reverb-ir-mono-test.wav");
  ASSERT_TRUE(waitForChainLoaded(proc));
  const double beforeMs = irContentLengthMs(proc, "blk-a");

  ASSERT_TRUE(setTrimInitAndWaitForRebuild(proc, "blk-a", true));
  const double afterMs = irContentLengthMs(proc, "blk-a");

  EXPECT_NEAR(afterMs, beforeMs, beforeMs * 0.05)
      << "Trim Init changed the reported length on a source with no real leading silence";
}

TEST(TrimInitTest, SurvivesUndoRedo) {
  ChainTestProcessor proc;
  proc.setPlayConfigDetails(2, 2, kFs, kBlock);
  proc.prepareToPlay(kFs, kBlock);
  seedMonoIrChain(proc, "blk-a", "reverb-ir-silence-prefix-test.wav");
  ASSERT_TRUE(waitForChainLoaded(proc));

  ASSERT_TRUE(setTrimInitAndWaitForRebuild(proc, "blk-a", true));
  const double afterEnable = irContentLengthMs(proc, "blk-a");
  ASSERT_TRUE(trimInitOf(proc, "blk-a"));

  ASSERT_TRUE(proc.undoChain());
  pumpAudio(proc, 1200, /*alreadyWarm=*/false);
  EXPECT_FALSE(trimInitOf(proc, "blk-a"))
      << "undo didn't restore the pre-toggle state (Trim Init wasn't pushed to chain history?)";
  const double afterUndo = irContentLengthMs(proc, "blk-a");
  EXPECT_GT(afterUndo, afterEnable)
      << "undo didn't restore the pre-Trim-Init (longer) reported content length";

  ASSERT_TRUE(proc.redoChain());
  pumpAudio(proc, 1200, /*alreadyWarm=*/false);
  EXPECT_TRUE(trimInitOf(proc, "blk-a"));
  const double afterRedo = irContentLengthMs(proc, "blk-a");
  EXPECT_NEAR(afterRedo, afterEnable, afterEnable * 0.05)
      << "redo didn't restore the post-Trim-Init content length";
}

// Same reapply-after-restore shape as IrSizeWidthTest.BothReapplyAfterStateRestore.
TEST(TrimInitTest, ReappliesAfterStateRestore) {
  constexpr int kTotalBlocks = 120;
  const auto impulse = makeDelayedImpulse(kTotalBlocks);

  ChainTestProcessor proc;
  proc.setPlayConfigDetails(2, 2, kFs, kBlock);
  proc.prepareToPlay(kFs, kBlock);
  seedMonoIrChain(proc, "blk-a", "reverb-ir-silence-prefix-test.wav");
  ASSERT_TRUE(waitForChainLoaded(proc));
  ASSERT_TRUE(proc.setBlockParam("blk-a", "mix", 1.0));
  ASSERT_TRUE(setTrimInitAndWaitForRebuild(proc, "blk-a", true));
  letAudioGoIdle();

  const auto [beforeL, beforeR] = processStereo(proc, impulse);

  juce::MemoryBlock savedState;
  proc.getStateInformation(savedState);

  ChainTestProcessor restored;
  restored.setPlayConfigDetails(2, 2, kFs, kBlock);
  restored.prepareToPlay(kFs, kBlock);
  restored.setStateInformation(savedState.getData(), static_cast<int>(savedState.getSize()));

  bool loaded = false;
  {
    const std::vector<float> silence(static_cast<size_t>(kBlock), 0.0f);
    const auto deadline = juce::Time::getMillisecondCounter() + 5000u;
    while (juce::Time::getMillisecondCounter() < deadline) {
      processStereo(restored, silence);
      const juce::var state = restored.getChainState(-1);
      bool allLoaded = true;
      for (const auto* lane : {state["chain"].getArray(), state["chainRight"].getArray()}) {
        if (lane == nullptr) continue;
        for (const auto& item : *lane)
          if (item["kind"].toString() == "tone" && !static_cast<bool>(item["loaded"]))
            allLoaded = false;
      }
      if (allLoaded && !restored.isChainEditFadeHeld()) {
        loaded = true;
        break;
      }
      juce::Thread::sleep(20);
    }
  }
  ASSERT_TRUE(loaded) << "restored IR block never finished reloading";
  ASSERT_TRUE(trimInitOf(restored, "blk-a")) << "Trim Init didn't persist across state restore";
  ASSERT_TRUE(restored.setBlockParam("blk-a", "mix", 1.0));
  pumpAudio(restored, 1200, /*alreadyWarm=*/true);
  letAudioGoIdle();

  const auto [afterL, afterR] = processStereo(restored, impulse);

  ASSERT_EQ(beforeL.size(), afterL.size());
  float maxAbsDiff = 0.0f;
  for (size_t i = 0; i < beforeL.size(); ++i) {
    maxAbsDiff = std::max(maxAbsDiff, std::abs(beforeL[i] - afterL[i]));
    maxAbsDiff = std::max(maxAbsDiff, std::abs(beforeR[i] - afterR[i]));
  }
  std::printf("[TrimInitTest] max |diff| before save vs after restore: %.9f\n",
              static_cast<double>(maxAbsDiff));
  // Trim Init only shifts an integer sample offset (no resampling ratio
  // involved, unlike Size), so in principle this could be tighter than
  // IrSizeWidthTest.BothReapplyAfterStateRestore's 0.02 - but empirically
  // JUCE's convolution engine isn't run-to-run bit-exact even for two
  // independently-built engines given the identical kernel (same note as
  // that test), so a small divergence (~-32dB) is still expected here, not
  // a broken reapply.
  EXPECT_LT(maxAbsDiff, 0.03f)
      << "restore didn't reapply the persisted Trim Init shape to the reloaded engine";
}

// Trim's relaxed mode (see ChainBlock::trimRelaxed/irOnsetSamplesRelaxed):
// a second, less-sensitive onset detection computed at load time
// (computeIrOnsetSamples with a higher thresholdDb) for sources whose
// standard -60dB threshold clears well before their audible transient. A
// higher threshold requires a louder signal to trigger, so its detected
// onset can only land at the same point or later in time than the standard
// one - never earlier - regardless of which fixture it's measured on.
TEST(TrimInitTest, RelaxedThresholdNeverLandsEarlierThanStandard) {
  ChainTestProcessor proc;
  proc.setPlayConfigDetails(2, 2, kFs, kBlock);
  proc.prepareToPlay(kFs, kBlock);
  seedMonoIrChain(proc, "blk-a", "reverb-ir-silence-prefix-test.wav");
  ASSERT_TRUE(waitForChainLoaded(proc));

  const double fractionStandard = onsetFractionOf(proc, "blk-a");

  // relaxed=true alone (enabled stays false) is enough to switch which
  // onset irOnsetFraction reports - it's shipped unconditionally, same as
  // OnsetFractionIsShippedRegardlessOfTrimInitState above, just now keyed
  // off trimRelaxed too.
  ASSERT_TRUE(proc.setBlockIrTrimInit("blk-a", false, /*relaxed=*/true));
  pumpAudio(proc, 1200, /*alreadyWarm=*/false);
  const double fractionRelaxed = onsetFractionOf(proc, "blk-a");

  std::printf("[TrimInitTest] onset fraction standard=%.4f relaxed=%.4f\n", fractionStandard,
             fractionRelaxed);
  EXPECT_GE(fractionRelaxed, fractionStandard - 1e-9)
      << "relaxed threshold's onset landed earlier than the standard one's";
}

// The Off -> Std -> Lax -> Off cycle (ChainBlock.tsx's handleCycleTrimMode)
// commits both fields together in a single setBlockIrTrimInit call at every
// step - including Lax -> Off, which must also clear trimRelaxed (or the
// next Off -> on would silently resume on Lax instead of landing on Std) -
// so each step, including that last one, is exactly one undo step.
TEST(TrimInitTest, LaxToOffIsOneUndoStepAndClearsRelaxed) {
  ChainTestProcessor proc;
  proc.setPlayConfigDetails(2, 2, kFs, kBlock);
  proc.prepareToPlay(kFs, kBlock);
  seedMonoIrChain(proc, "blk-a", "reverb-ir-silence-prefix-test.wav");
  ASSERT_TRUE(waitForChainLoaded(proc));

  ASSERT_TRUE(setTrimInitAndWaitForRebuild(proc, "blk-a", true));  // Off -> Std
  ASSERT_TRUE(proc.setBlockIrTrimInit("blk-a", true, /*relaxed=*/true));  // Std -> Lax
  // Exceeds ChainHistory::kCoalesceWindowMs (1500ms) so the next push (Lax ->
  // Off) lands as its own undo step instead of silently merging into this
  // one - both share the same "param:<blockId>:trimInit" coalesce key, and a
  // real user's separate deliberate clicks are seconds apart, never this
  // close, but the test must be explicit about busting the window.
  pumpAudio(proc, 1700, /*alreadyWarm=*/false);
  ASSERT_TRUE(trimInitOf(proc, "blk-a"));
  ASSERT_TRUE(trimRelaxedOf(proc, "blk-a"));

  ASSERT_TRUE(proc.setBlockIrTrimInit("blk-a", false, /*relaxed=*/false));  // Lax -> Off
  pumpAudio(proc, 1200, /*alreadyWarm=*/false);
  EXPECT_FALSE(trimInitOf(proc, "blk-a"));
  EXPECT_FALSE(trimRelaxedOf(proc, "blk-a"));

  ASSERT_TRUE(proc.undoChain());
  pumpAudio(proc, 1200, /*alreadyWarm=*/false);
  EXPECT_TRUE(trimInitOf(proc, "blk-a"))
      << "one undo off Lax->Off should land back on Lax (enabled true)";
  EXPECT_TRUE(trimRelaxedOf(proc, "blk-a"))
      << "one undo off Lax->Off should restore relaxed too, not just enabled";
}
