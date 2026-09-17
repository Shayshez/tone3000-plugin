// Pins IR Size (vari-speed duration/pitch, setBlockIrSize) and IR Width
// (stereo-image crossfade/phase-inversion, setBlockIrWidth) - both feed the
// same off-thread rebuild path (rebuildIrShapeInBackground/
// prepareIrShapeRebuild) the envelope (see ir_decay_tests.cpp) already uses.
//
// Size never touches the trimmed buffer's sample count - it only scales the
// sample rate declared to the convolver's loadImpulseResponse, which is
// exactly what a vari-speed/sample-rate-change effect does (both duration
// and pitch move together, not a pitch-preserving time-stretch). Because the
// envelope's Attack/Decay fractions are expressed as fractions of the
// (sample-count-based, Size-independent) truncated content, Size composes
// with the envelope automatically - this file proves that holds rather than
// assuming it.
//
// Width reshapes a *separate* copy of the trimmed/enveloped buffer before it
// reaches convolverStereo only; convolverMono (the mono-fallback engine)
// always keeps the untouched image. Default is 0.75 (100%/full original
// stereo) rather than the knob's own 0.5 bipolar center (genuinely mono) -
// every stereo IR played its true recorded image unconditionally before
// this control existed, so the no-op starting point has to match that, not
// the knob's visual center; this file also pins that regression guard.
#include "chain_test_helpers.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace {
constexpr int kBlock = 512;

void seedMonoIrChain(ChainTestProcessor& proc, const juce::String& blockId,
                     const char* fileName = "reverb-ir-mono-test.wav") {
  juce::ValueTree state("ChainSnapshot");
  state.setProperty("stereoEnabled", false, nullptr);
  juce::ValueTree lane("ChainBlocks");
  lane.appendChild(makeIrBlockTree(blockId, 1, 100, fileName), nullptr);
  state.appendChild(lane, nullptr);
  state.appendChild(juce::ValueTree("RightChainBlocks"), nullptr);
  proc.restoreFromTree(state);
}

// Own local copies, not shared with swap_fade_tests.cpp's (private,
// anonymous-namespace) versions - same small-helper-duplication precedent
// as seedMonoIrChain/pumpAudio already being copied across this file and
// ir_trim_init_tests.cpp.
juce::var makeModelVar(int modelId, const juce::String& name) {
  auto* obj = new juce::DynamicObject();
  obj->setProperty("id", modelId);
  obj->setProperty("name", name);
  obj->setProperty("model_url", "https://test.invalid/" + name + ".wav");
  return juce::var(obj);
}

// Appends `fileName`'s bytes to the block tree's ModelCache under `modelId`,
// and lists that id in toneJson's own models array - restore only seeds
// modelCache for ids the tone actually references (block->referencesModel,
// see ProcessorHistory.cpp's restore path), so skipping this second part
// silently drops the cached bytes and switchModel falls through to a (here,
// unreachable) network fetch instead.
void cacheExtraModel(juce::ValueTree& block, int modelId, const char* fileName) {
  juce::MemoryBlock bytes;
  ASSERT_TRUE(testFile(fileName).loadFileAsData(bytes));
  juce::ValueTree cached("CachedModel");
  cached.setProperty("modelId", modelId, nullptr);
  cached.setProperty("data", juce::var(bytes), nullptr);
  block.getChildWithName("ModelCache").appendChild(cached, nullptr);

  juce::var tone = juce::JSON::parse(block.getProperty("toneJson").toString());
  auto* models = tone["models"].getArray();
  ASSERT_NE(models, nullptr);
  auto* model = new juce::DynamicObject();
  model->setProperty("id", modelId);
  model->setProperty("name", "cached-" + juce::String(modelId));
  model->setProperty("model_url", "https://test.invalid/cached-" + juce::String(modelId) + ".wav");
  models->add(juce::var(model));
  block.setProperty("toneJson", juce::JSON::toString(tone), nullptr);
}

bool switchModelSettled(ChainTestProcessor& proc, const char* blockId, int expectedModelId) {
  const juce::var state = proc.getChainState(-1);
  if (const auto* lane = state["chain"].getArray())
    for (const auto& item : *lane)
      if (item["blockId"].toString() == blockId)
        return !static_cast<bool>(item["modelLoading"]) && static_cast<bool>(item["loaded"]) &&
              static_cast<int>(item["activeModelId"]) == expectedModelId;
  return false;
}

// Mirrors ir_decay_tests.cpp's own pumpAudio: keeps the heartbeat alive
// through the background rebuild + wet-mute swap fade so requestSwapFadeAndWait
// runs its real mute/unmute path instead of short-circuiting on idle audio.
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

void setSizeAndWaitForRebuild(ChainTestProcessor& proc, const char* blockId, double sizeNorm) {
  const std::vector<float> silence(static_cast<size_t>(kBlock), 0.0f);
  processStereo(proc, silence);  // prime
  ASSERT_TRUE(proc.setBlockIrSize(blockId, sizeNorm));
  pumpAudio(proc, 1200, /*alreadyWarm=*/true);
}

bool setWidthAndWaitForRebuild(ChainTestProcessor& proc, const char* blockId, double widthNorm) {
  const std::vector<float> silence(static_cast<size_t>(kBlock), 0.0f);
  processStereo(proc, silence);  // prime
  const bool ok = proc.setBlockIrWidth(blockId, widthNorm);
  pumpAudio(proc, 1200, /*alreadyWarm=*/true);
  return ok;
}

// Same rationale as ir_decay_tests.cpp's own copy: placed well into the
// buffer so the input noise gate can't suppress a sample-0 spike.
constexpr int kImpulseBlock = 15;
constexpr size_t kImpulseOnset = static_cast<size_t>(kImpulseBlock) * kBlock;

std::vector<float> makeDelayedImpulse(int totalBlocks) {
  std::vector<float> impulse(static_cast<size_t>(totalBlocks * kBlock), 0.0f);
  impulse[kImpulseOnset] = 1.0f;
  return impulse;
}

size_t lastAboveFloor(const std::vector<float>& v, float floor) {
  for (size_t i = v.size(); i-- > kImpulseOnset;)
    if (std::abs(v[i]) > floor) return i;
  return kImpulseOnset;
}

double irContentLengthMs(ChainTestProcessor& proc, const char* blockId) {
  const juce::var state = proc.getChainState(-1);
  if (const auto* lane = state["chain"].getArray())
    for (const auto& item : *lane)
      if (item["blockId"].toString() == blockId)
        return static_cast<double>(item["irContentLengthMs"]);
  return -1.0;
}

int irNumChannelsOf(ChainTestProcessor& proc, const char* blockId) {
  const juce::var state = proc.getChainState(-1);
  if (const auto* lane = state["chain"].getArray())
    for (const auto& item : *lane)
      if (item["blockId"].toString() == blockId)
        return static_cast<int>(item["irNumChannels"]);
  return -1;
}
}  // namespace

// Size 10%/100%/1000% should scale the reported content length (the "X.XXs"
// tooltip / D Len source) by the same 0.1x/1x/10x ratio - the direct,
// UI-visible signature of vari-speed duration change. Cross-checked against
// the actually-convolved impulse tail so this isn't just testing the
// getChainState arithmetic in isolation.
TEST(IrSizeTest, DurationScalesWithVariSpeedRatio) {
  constexpr int kTotalBlocks = 700;  // generous: covers the 1000% (10x) case
  const auto impulse = makeDelayedImpulse(kTotalBlocks);
  const float floor = 1e-4f;

  auto runWithSize = [&](double sizeNorm) {
    ChainTestProcessor proc;
    proc.setPlayConfigDetails(2, 2, kFs, kBlock);
    proc.prepareToPlay(kFs, kBlock);
    seedMonoIrChain(proc, "blk-a");
    EXPECT_TRUE(waitForChainLoaded(proc)) << "IR block never finished loading from cache";
    EXPECT_TRUE(proc.setBlockParam("blk-a", "mix", 1.0));
    const double baselineMs = irContentLengthMs(proc, "blk-a");
    setSizeAndWaitForRebuild(proc, "blk-a", sizeNorm);
    const double scaledMs = irContentLengthMs(proc, "blk-a");
    letAudioGoIdle();
    const auto [outL, outR] = processStereo(proc, impulse);
    juce::ignoreUnused(outR);
    const size_t tail = lastAboveFloor(outL, floor);
    return std::make_tuple(baselineMs, scaledMs, tail > kImpulseOnset ? tail - kImpulseOnset : 0);
  };

  const auto [base100, ms100, tail100] = runWithSize(0.5);   // 100%, unchanged
  const auto [base10, ms10, tail10] = runWithSize(0.0);      // 10%, 10x faster
  const auto [base1000, ms1000, tail1000] = runWithSize(1.0);  // 1000%, 10x slower

  ASSERT_GT(base100, 0.0);
  // Size=100% must be a genuine no-op on the reported length.
  EXPECT_NEAR(ms100, base100, base100 * 0.01);

  std::printf(
      "[IrSizeTest] contentLengthMs: 10%%=%.2f 100%%=%.2f 1000%%=%.2f | measured tail (samples): "
      "10%%=%zu 100%%=%zu 1000%%=%zu\n",
      ms10, ms100, ms1000, tail10, tail100, tail1000);

  // Loose tolerances: loadImpulseResponse's own resampler/kernel-size
  // rounding isn't exact-ratio, and the measured tail is a floor-crossing on
  // convolved output, not the raw kernel length - this only needs to show
  // vari-speed is genuinely happening in roughly the right proportion, the
  // same spirit as IrDecayTest.TruncatesViaAttackAndDecayLength's own
  // tolerance.
  EXPECT_NEAR(ms10, base10 * 0.1, base10 * 0.1 * 0.25)
      << "Size=10% doesn't appear to be ~10x shortening the reported length";
  EXPECT_NEAR(ms1000, base1000 * 10.0, base1000 * 10.0 * 0.25)
      << "Size=1000% doesn't appear to be ~10x lengthening the reported length";
  EXPECT_GT(tail100, 0u) << "100% run produced no measurable tail at all - fixture broken";
  EXPECT_LT(tail10, tail100 / 3)
      << "Size=10% doesn't appear to be shortening the convolved tail";
  // A floor-crossing on convolved output doesn't scale exactly proportional
  // to Size even when the underlying kernel genuinely does (a stretched
  // kernel's energy spreads thinner, crossing the fixed floor earlier than a
  // strict 10x would suggest) - the contentLengthMs checks above are the
  // precise measurement (both land within ~0.1% of the exact 10x/0.1x
  // ratio); this just needs to confirm the *direction*, generously.
  EXPECT_GT(tail1000, tail100 * 2)
      << "Size=1000% doesn't appear to be lengthening the convolved tail";
}

// The loudness half of Size (see ChainBlock.h's irSizeGainCompensation):
// with Normalise::no, JUCE's Convolution applies its own declared-rate
// magnitude-preserving gain on top of the resample Size's vari-speed trick
// causes, which nets out to the convolved output scaling by
// 1/sqrt(durationRatio) - stretching (Size>100%) gets quieter, compressing
// (Size<100%) gets louder, neither intentional. irSizeGainCompensation
// multiplies the exact inverse back in. Measured via a short RMS window
// right after the impulse (well inside even the shortest of these three
// tails) rather than DurationScalesWithVariSpeedRatio's floor-crossing tail
// length, since loudness - not duration - is what's under test here.
TEST(IrSizeTest, LoudnessCompensationKeepsOutputLevelRoughlyConstant) {
  constexpr int kTotalBlocks = 700;
  const auto impulse = makeDelayedImpulse(kTotalBlocks);
  constexpr size_t kWindowSamples = static_cast<size_t>(kFs * 0.08);  // 80ms

  auto rmsOfWindow = [](const std::vector<float>& v, size_t start, size_t length) {
    const size_t end = std::min(v.size(), start + length);
    double sumSq = 0.0;
    for (size_t i = start; i < end; ++i) sumSq += static_cast<double>(v[i]) * v[i];
    return end > start ? std::sqrt(sumSq / static_cast<double>(end - start)) : 0.0;
  };

  auto rmsAtSize = [&](double sizeNorm) {
    ChainTestProcessor proc;
    proc.setPlayConfigDetails(2, 2, kFs, kBlock);
    proc.prepareToPlay(kFs, kBlock);
    // reverb-ir-6s-test.wav specifically, not the default mono fixture: its
    // much larger total energy (6s of dense content) drives
    // computeIrNormalizationGain's content-only gain well below the 1.0
    // ceiling, leaving enough headroom for this test to demonstrate the
    // compensation cleanly - a fixture whose base gain already sits near
    // 1.0 has too little room left for the ceiling to let a meaningful
    // compensation through, by design (see
    // LoudnessCompensationNeverExceedsUnityOutput below, which deliberately
    // uses that hotter fixture to pin the ceiling itself, not full parity).
    seedMonoIrChain(proc, "blk-a", "reverb-ir-6s-test.wav");
    EXPECT_TRUE(waitForChainLoaded(proc)) << "IR block never finished loading from cache";
    EXPECT_TRUE(proc.setBlockParam("blk-a", "mix", 1.0));
    setSizeAndWaitForRebuild(proc, "blk-a", sizeNorm);
    letAudioGoIdle();
    const auto [outL, outR] = processStereo(proc, impulse);
    juce::ignoreUnused(outR);
    return rmsOfWindow(outL, kImpulseOnset, kWindowSamples);
  };

  // durationRatio=2/0.5, not the 4/0.25 used elsewhere in this file - the 6s
  // fixture's own effective-duration cap (kIrSizeMaxEffectiveSeconds=20s)
  // would otherwise clamp a 4x stretch (24s) to something short of the
  // requested ratio and throw off the predicted compensation.
  const double rmsBaseline = rmsAtSize(0.5);      // 100% (durationRatio=1)
  const double rmsStretched = rmsAtSize(0.6505);  // ~200% (durationRatio=2)
  const double rmsCompressed = rmsAtSize(0.3495);  // ~50% (durationRatio=0.5)

  ASSERT_GT(rmsBaseline, 0.0) << "fixture produced no measurable output at all";
  std::printf("[IrSizeTest] windowed RMS: 100%%=%.6f ~200%%=%.6f ~50%%=%.6f\n", rmsBaseline,
             rmsStretched, rmsCompressed);

  // Uncompensated, these would differ from baseline by a factor of
  // 1/sqrt(2)=0.707 and 1/sqrt(0.5)=1.414 respectively. Compensated, both
  // should land close to parity with baseline; ±35% is generous enough to
  // absorb the resampler's own run-to-run jitter (see
  // IrSizeWidthTest.BothReapplyAfterStateRestore's note on non-bit-exact
  // reproducibility) while still clearly failing if the compensation
  // regresses to a no-op or the wrong direction.
  EXPECT_NEAR(rmsStretched, rmsBaseline, rmsBaseline * 0.35)
      << "Size stretched (durationRatio=2) isn't compensated back to ~baseline loudness";
  EXPECT_NEAR(rmsCompressed, rmsBaseline, rmsBaseline * 0.35)
      << "Size compressed (durationRatio=0.5) isn't compensated back to ~baseline loudness";
}

// The explicit ask this feature was built for: compensating Size's loudness
// drop must never risk clipping. It can't, by construction - the
// compensated value still rides the same 1.0 ceiling
// computeIrNormalizationGain's content-only gain already obeyed (see
// Processor.cpp's jlimit(0.0f, 1.0f, ...) around irNormalizationSmoother) -
// but this pins that guarantee at the actual sample level, at the most
// extreme Size setting (1000%, the largest compensation factor available),
// rather than trusting the derivation alone.
TEST(IrSizeTest, LoudnessCompensationNeverExceedsUnityOutput) {
  constexpr int kTotalBlocks = 700;
  const auto impulse = makeDelayedImpulse(kTotalBlocks);

  ChainTestProcessor proc;
  proc.setPlayConfigDetails(2, 2, kFs, kBlock);
  proc.prepareToPlay(kFs, kBlock);
  seedMonoIrChain(proc, "blk-a");
  ASSERT_TRUE(waitForChainLoaded(proc));
  ASSERT_TRUE(proc.setBlockParam("blk-a", "mix", 1.0));
  setSizeAndWaitForRebuild(proc, "blk-a", 1.0);  // 1000%: durationRatio=10, compensation=sqrt(10)
  letAudioGoIdle();

  const auto [outL, outR] = processStereo(proc, impulse);
  float peak = 0.0f;
  for (float s : outL) peak = std::max(peak, std::abs(s));
  for (float s : outR) peak = std::max(peak, std::abs(s));

  std::printf("[IrSizeTest] peak output at Size=1000%% (max compensation): %.4f\n",
             static_cast<double>(peak));
  EXPECT_LE(peak, 1.0f) << "Size's loudness compensation produced a sample above unity";
}

// Always resamples fresh from the frozen ChainBlock::irRawSamples, never
// from a previously-shaped buffer - so several round trips through extreme,
// unrelated Size values, ending back at 100%, must land on the *same*
// content length a single direct jump to 100% does, not drift further away
// with each hop (the signature a real "resample of a resample" bug would
// leave: cumulative loss compounding with every additional round trip).
//
// Measured via the reported content length (precise to ~0.1% per
// DurationScalesWithVariSpeedRatio above), not a raw sample-accurate audio
// diff: JUCE's Convolution::loadImpulseResponse resampler/FFT-partition
// construction isn't bit-exact-reproducible run-to-run even for two
// independently-built engines targeting the identical declared rate (see
// IrSizeWidthTest.BothReapplyAfterStateRestore's own note) - that jitter is
// unrelated to compounding and would swamp a tight audio-diff tolerance
// here regardless of whether compounding is actually happening.
TEST(IrSizeTest, RepeatedAdjustmentsNeverCompoundQualityLoss) {
  ChainTestProcessor direct;
  direct.setPlayConfigDetails(2, 2, kFs, kBlock);
  direct.prepareToPlay(kFs, kBlock);
  seedMonoIrChain(direct, "blk-a");
  ASSERT_TRUE(waitForChainLoaded(direct));
  setSizeAndWaitForRebuild(direct, "blk-a", 0.5);  // straight to 100%, once
  const double directMs = irContentLengthMs(direct, "blk-a");

  ChainTestProcessor fiddled;
  fiddled.setPlayConfigDetails(2, 2, kFs, kBlock);
  fiddled.prepareToPlay(kFs, kBlock);
  seedMonoIrChain(fiddled, "blk-a");
  ASSERT_TRUE(waitForChainLoaded(fiddled));
  // Several round trips through extreme, unrelated Size values before
  // returning to 100% - each rebuild must read the untouched original
  // again, never a prior rebuild's already-resampled output.
  for (double sizeNorm : {0.0, 1.0, 0.2, 0.9, 0.5}) {
    setSizeAndWaitForRebuild(fiddled, "blk-a", sizeNorm);
  }
  const double fiddledMs = irContentLengthMs(fiddled, "blk-a");

  std::printf(
      "[IrSizeTest] contentLengthMs after a direct jump to 100%%=%.4f vs after 5 round trips "
      "ending at 100%%=%.4f\n",
      directMs, fiddledMs);
  EXPECT_NEAR(fiddledMs, directMs, directMs * 0.005)
      << "returning Size to 100% after several round trips doesn't reproduce a direct jump's "
         "content length - rebuilds may be compounding off a previously-shaped buffer instead "
         "of the frozen source";
}

// Bug fix, 2026-09-17: prepareBlockModelOffThread builds a freshly-loaded/
// swapped IR's engine straight from the raw file, with none of the block's
// existing Size/Width/envelope/Trim Init baked in - only setBlockIrDecay/
// Size/Width/TrimInit ever queue prepareIrShapeRebuild. For a brand-new
// block that's a correct no-op (everything's still default), but swapping
// a *different* model into a block that already has a non-default Size set
// used to leave the knob showing the old value while the audio silently
// reverted to 100%/untouched. Fixed by having loadToneInBackground/
// switchModelInBackground/convertBlockTypeInBackground reapply the block's
// existing shape after a successful swap (see blockHasNonDefaultIrShape,
// ProcessorChain.cpp) - this pins that fix via the real switchModel entry
// point, not just the rebuild helper directly.
TEST(IrSizeTest, PersistsAcrossModelSwapWithinSameBlock) {
  ChainTestProcessor proc;
  proc.setPlayConfigDetails(2, 2, kFs, kBlock);
  proc.prepareToPlay(kFs, kBlock);

  juce::ValueTree block = makeIrBlockTree("blk-a", 1, 100, "reverb-ir-mono-test.wav");
  cacheExtraModel(block, 101, "cab-ir-test.wav");
  juce::ValueTree state("ChainSnapshot");
  state.setProperty("stereoEnabled", false, nullptr);
  juce::ValueTree lane("ChainBlocks");
  lane.appendChild(block, nullptr);
  state.appendChild(lane, nullptr);
  state.appendChild(juce::ValueTree("RightChainBlocks"), nullptr);
  proc.restoreFromTree(state);
  ASSERT_TRUE(waitForChainLoaded(proc));

  setSizeAndWaitForRebuild(proc, "blk-a", 1.0);  // 1000%, well away from the 100% no-op default

  ASSERT_TRUE(proc.switchModel("blk-a", 101, makeModelVar(101, "cab2")));
  const auto deadline = juce::Time::getMillisecondCounter() + 15000u;
  bool settled = false;
  while (!settled && juce::Time::getMillisecondCounter() < deadline) {
    pumpAudio(proc, 100, /*alreadyWarm=*/false);
    settled = switchModelSettled(proc, "blk-a", 101);
  }
  ASSERT_TRUE(settled) << "cached model switch never completed";
  // The switch's own swap-fade settling is enough for activeModelId/loaded
  // to update, but the *shape* reapply this test is pinning is a second,
  // separately-queued background rebuild (see blockHasNonDefaultIrShape) -
  // give it the same settle budget setSizeAndWaitForRebuild's callers rely
  // on elsewhere in this file.
  pumpAudio(proc, 1200, /*alreadyWarm=*/false);

  ChainTestProcessor freshDefault;
  freshDefault.setPlayConfigDetails(2, 2, kFs, kBlock);
  freshDefault.prepareToPlay(kFs, kBlock);
  seedMonoIrChain(freshDefault, "blk-a", "cab-ir-test.wav");
  ASSERT_TRUE(waitForChainLoaded(freshDefault));
  const double defaultMs = irContentLengthMs(freshDefault, "blk-a");

  const double swappedMs = irContentLengthMs(proc, "blk-a");
  std::printf(
      "[IrSizeTest] cab-ir-test.wav content length: fresh-load/Size=100%%=%.2fms | "
      "swapped-in-with-Size=1000%%-already-set=%.2fms\n",
      defaultMs, swappedMs);

  EXPECT_NEAR(swappedMs, defaultMs * 10.0, defaultMs * 10.0 * 0.05)
      << "Size wasn't reapplied to the newly swapped-in model - looks like it reverted to 100%";
}

// Decay Length's fraction is expressed relative to the (Size-independent,
// sample-count-based) truncated content, so a fixed Decay Length must keep
// truncating at the same *relative* position regardless of Size - the tail
// extent should scale by Size's own duration ratio, with no special-casing
// needed between the two (see prepareIrShapeRebuild).
TEST(IrSizeTest, EnvelopeFractionsScaleAutomaticallyWithSize) {
  constexpr int kTotalBlocks = 200;
  const auto impulse = makeDelayedImpulse(kTotalBlocks);
  const float floor = 1e-4f;

  auto runWithSize = [&](double sizeNorm) {
    ChainTestProcessor proc;
    proc.setPlayConfigDetails(2, 2, kFs, kBlock);
    proc.prepareToPlay(kFs, kBlock);
    seedMonoIrChain(proc, "blk-a");
    EXPECT_TRUE(waitForChainLoaded(proc));
    EXPECT_TRUE(proc.setBlockParam("blk-a", "mix", 1.0));
    EXPECT_TRUE(proc.setBlockIrDecay("blk-a", /*init=*/1.0, /*attackLen=*/0.0, /*attackCurve=*/0.5,
                                     /*decayLen=*/0.5, /*decayLevel=*/1.0, /*decayCurve=*/0.5));
    pumpAudio(proc, 1200, /*alreadyWarm=*/false);
    setSizeAndWaitForRebuild(proc, "blk-a", sizeNorm);
    letAudioGoIdle();
    const auto [outL, outR] = processStereo(proc, impulse);
    juce::ignoreUnused(outR);
    const size_t tail = lastAboveFloor(outL, floor);
    return tail > kImpulseOnset ? tail - kImpulseOnset : 0;
  };

  const size_t tail100 = runWithSize(0.5);   // 100%
  const size_t tail200 = runWithSize(0.75);  // 200% (irSizeDurationRatio(0.75) == 2.0 exactly)

  ASSERT_GT(tail100, 0u) << "100% run produced no measurable tail at all";
  std::printf("[IrSizeTest] Decay Length=0.5 tail extent: Size=100%%->%zu, Size=200%%->%zu\n",
              tail100, tail200);
  // Loose (not exact) proportionality for the same reason as
  // DurationScalesWithVariSpeedRatio above; this just needs to show the
  // truncation point moved roughly with Size, not stayed fixed (which is
  // what a special-cased/unscaled envelope would look like).
  EXPECT_GT(tail200, static_cast<size_t>(tail100 * 1.5))
      << "Decay Length=0.5's truncation point doesn't appear to scale with Size - the envelope "
         "may not be composing with Size automatically";
}

// setBlockIrDecay-style params are registered with pushChainHistory
// ("param:<id>:decay"); Size/Width must follow the same convention so
// undo/redo works within the plugin, exactly like the other structural
// chain mutators (Branch, Merge, Decay, block-type conversion).
TEST(IrSizeTest, SurvivesUndoRedo) {
  ChainTestProcessor proc;
  proc.setPlayConfigDetails(2, 2, kFs, kBlock);
  proc.prepareToPlay(kFs, kBlock);
  seedMonoIrChain(proc, "blk-a");
  ASSERT_TRUE(waitForChainLoaded(proc));

  setSizeAndWaitForRebuild(proc, "blk-a", 0.2);
  const double afterSet = irContentLengthMs(proc, "blk-a");

  ASSERT_TRUE(proc.undoChain());
  pumpAudio(proc, 1200, /*alreadyWarm=*/false);
  const double afterUndo = irContentLengthMs(proc, "blk-a");
  EXPECT_GT(afterUndo, afterSet)
      << "undo didn't restore the pre-Size content length (Size wasn't pushed to chain history?)";

  ASSERT_TRUE(proc.redoChain());
  pumpAudio(proc, 1200, /*alreadyWarm=*/false);
  const double afterRedo = irContentLengthMs(proc, "blk-a");
  EXPECT_NEAR(afterRedo, afterSet, afterSet * 0.05)
      << "redo didn't restore the post-Size content length";
}

// kIrSizeMaxEffectiveSeconds (ChainBlock.h): Size's effective duration is
// capped regardless of source length or how far the knob is turned - an
// already-long source (here 6s) stretched to 1000% would unclamped reach
// 60s, empirically measured (under realistic concurrent UI load:
// getChainState/getIrWaveform/setBlockParam all hammering chainMutex at
// once, not an isolated idle-UI measurement) to produce an actual
// over-budget real-time callback. 30s was tried first on the strength of
// that callback-timing margin but still produced audible crackles on long
// sources in real-world use, so the cap was tightened to 20s.
TEST(IrSizeTest, EffectiveLengthClampedOnLongSource) {
  ChainTestProcessor proc;
  proc.setPlayConfigDetails(2, 2, kFs, kBlock);
  proc.prepareToPlay(kFs, kBlock);
  seedMonoIrChain(proc, "blk-a", "reverb-ir-6s-test.wav");
  ASSERT_TRUE(waitForChainLoaded(proc));
  const double baselineMs = irContentLengthMs(proc, "blk-a");
  ASSERT_NEAR(baselineMs, 6000.0, 50.0) << "fixture's own detected content length drifted";

  setSizeAndWaitForRebuild(proc, "blk-a", 1.0);  // 1000% -> unclamped would be 60s
  const double cappedMs = irContentLengthMs(proc, "blk-a");
  std::printf("[IrSizeTest] 6s source at Size=1000%%: reported length=%.1fms (cap=20000ms)\n",
              cappedMs);
  EXPECT_NEAR(cappedMs, 20000.0, 50.0)
      << "effective length wasn't clamped to kIrSizeMaxEffectiveSeconds on a long source";
}

// The cap must never affect a source short enough that even 1000% stays
// well under the ceiling - it's the effective *result* that's bounded, not
// a blanket reduction of Size's own range.
TEST(IrSizeTest, ShortSourceUnaffectedByCap) {
  ChainTestProcessor proc;
  proc.setPlayConfigDetails(2, 2, kFs, kBlock);
  proc.prepareToPlay(kFs, kBlock);
  seedMonoIrChain(proc, "blk-a");  // ~1.2s detected content
  ASSERT_TRUE(waitForChainLoaded(proc));
  const double baselineMs = irContentLengthMs(proc, "blk-a");
  ASSERT_LT(baselineMs, 3000.0) << "fixture's own detected content length drifted";

  setSizeAndWaitForRebuild(proc, "blk-a", 1.0);  // 1000%, well under the 20s cap either way
  const double scaledMs = irContentLengthMs(proc, "blk-a");
  EXPECT_NEAR(scaledMs, baselineMs * 10.0, baselineMs * 10.0 * 0.02)
      << "a short source's Size=1000% was clamped when it never should have been";
}

// No stereo image to widen: setBlockIrWidth must no-op (false, no rebuild
// queued) on a mono-source IR - matches the UI's disabled knob.
TEST(IrWidthTest, LocksOutOnMonoSource) {
  ChainTestProcessor proc;
  proc.setPlayConfigDetails(2, 2, kFs, kBlock);
  proc.prepareToPlay(kFs, kBlock);
  seedMonoIrChain(proc, "blk-a", "reverb-ir-mono-test.wav");
  ASSERT_TRUE(waitForChainLoaded(proc));
  EXPECT_EQ(irNumChannelsOf(proc, "blk-a"), 1);

  EXPECT_FALSE(proc.setBlockIrWidth("blk-a", 0.9))
      << "setBlockIrWidth should refuse a mono-source IR block";
}

// Default (never touching the Width knob) must reproduce the exact prior
// behavior: the IR's true recorded stereo image, unconditionally - NOT the
// knob's own 0% bipolar center, which is genuinely mono. Regression guard
// for that distinction (see ChainBlock::widthNormalized's own comment).
TEST(IrWidthTest, DefaultPreservesOriginalStereoImageNotMono) {
  ChainTestProcessor proc;
  proc.setPlayConfigDetails(2, 2, kFs, kBlock);
  proc.prepareToPlay(kFs, kBlock);
  seedMonoIrChain(proc, "blk-a", "reverb-ir-stereo-test.wav");
  ASSERT_TRUE(waitForChainLoaded(proc));
  ASSERT_EQ(irNumChannelsOf(proc, "blk-a"), 2);
  ASSERT_TRUE(proc.setBlockParam("blk-a", "mix", 1.0));
  letAudioGoIdle();

  constexpr int kWarmupBlocks = 15;
  processStereo(proc, makeNoise(kWarmupBlocks * kBlock, 1111, 0.25f));
  const auto [outL, outR] = processStereo(proc, makeNoise(20 * kBlock, 4242, 0.25f));

  const float diff = settledMaxChannelDiff(outL, outR, /*skip=*/0);
  std::printf("[IrWidthTest] default (untouched) Width: max |L-R| = %.6f\n",
              static_cast<double>(diff));
  EXPECT_GT(diff, 1e-3f)
      << "a freshly loaded stereo IR sounds mono by default - Width's default must be 100%, "
         "not its 0%/mono bipolar center";
}

// Center (0%) genuinely collapses to mono: L and R must converge once both
// channels are driven from the same mono-summed source.
TEST(IrWidthTest, CenterCollapsesToMono) {
  ChainTestProcessor proc;
  proc.setPlayConfigDetails(2, 2, kFs, kBlock);
  proc.prepareToPlay(kFs, kBlock);
  seedMonoIrChain(proc, "blk-a", "reverb-ir-stereo-test.wav");
  ASSERT_TRUE(waitForChainLoaded(proc));
  ASSERT_TRUE(proc.setBlockParam("blk-a", "mix", 1.0));
  ASSERT_TRUE(setWidthAndWaitForRebuild(proc, "blk-a", 0.5));
  letAudioGoIdle();

  constexpr int kWarmupBlocks = 15;
  processStereo(proc, makeNoise(kWarmupBlocks * kBlock, 1111, 0.25f));
  const auto [outL, outR] = processStereo(proc, makeNoise(20 * kBlock, 4242, 0.25f));

  const float diff = settledMaxChannelDiff(outL, outR, /*skip=*/0);
  std::printf("[IrWidthTest] Width=0%% (center): max |L-R| = %.9f\n", static_cast<double>(diff));
  EXPECT_LT(diff, 1e-4f) << "Width=0% doesn't appear to be collapsing the stereo image to mono";
}

// Beyond 100%, one channel blends toward the polarity-inverted opposite
// channel; at the full +200%/-200% extreme that blend is complete, so the
// two output channels should be exact negations of each other.
TEST(IrWidthTest, ExtremesProduceAntiPhaseChannels) {
  auto runAtWidth = [](double widthNorm) {
    ChainTestProcessor proc;
    proc.setPlayConfigDetails(2, 2, kFs, kBlock);
    proc.prepareToPlay(kFs, kBlock);
    seedMonoIrChain(proc, "blk-a", "reverb-ir-stereo-test.wav");
    EXPECT_TRUE(waitForChainLoaded(proc));
    EXPECT_TRUE(proc.setBlockParam("blk-a", "mix", 1.0));
    EXPECT_TRUE(setWidthAndWaitForRebuild(proc, "blk-a", widthNorm));
    letAudioGoIdle();
    constexpr int kWarmupBlocks = 15;
    processStereo(proc, makeNoise(kWarmupBlocks * kBlock, 1111, 0.25f));
    return processStereo(proc, makeNoise(20 * kBlock, 4242, 0.25f));
  };

  for (const double widthNorm : {0.0, 1.0}) {  // -200% and +200%
    const auto [outL, outR] = runAtWidth(widthNorm);
    ASSERT_EQ(outL.size(), outR.size());
    float maxAbsDiff = 0.0f;
    for (size_t i = 48000; i < outL.size(); ++i)
      maxAbsDiff = std::max(maxAbsDiff, std::abs(outL[i] + outR[i]));
    std::printf("[IrWidthTest] Width normalized=%.1f: max |L+R| (0 == exact anti-phase) = %.6f\n",
                widthNorm, static_cast<double>(maxAbsDiff));
    EXPECT_LT(maxAbsDiff, 5e-3f)
        << "Width's full extreme doesn't appear to produce anti-phase L/R channels";
  }
}

// Sign flips L/R orientation within the 0-100% crossfade: -100% and +100%
// should be a straight channel swap of each other.
TEST(IrWidthTest, SignSwapsChannelOrientation) {
  auto runAtWidth = [](double widthNorm) {
    ChainTestProcessor proc;
    proc.setPlayConfigDetails(2, 2, kFs, kBlock);
    proc.prepareToPlay(kFs, kBlock);
    seedMonoIrChain(proc, "blk-a", "reverb-ir-stereo-test.wav");
    EXPECT_TRUE(waitForChainLoaded(proc));
    EXPECT_TRUE(proc.setBlockParam("blk-a", "mix", 1.0));
    EXPECT_TRUE(setWidthAndWaitForRebuild(proc, "blk-a", widthNorm));
    letAudioGoIdle();
    constexpr int kWarmupBlocks = 15;
    processStereo(proc, makeNoise(kWarmupBlocks * kBlock, 1111, 0.25f));
    return processStereo(proc, makeNoise(20 * kBlock, 4242, 0.25f));
  };

  const auto [posL, posR] = runAtWidth(0.75);  // +100%: original orientation
  const auto [negL, negR] = runAtWidth(0.25);  // -100%: swapped orientation

  ASSERT_EQ(posL.size(), negL.size());
  float maxDiffSwapped = 0.0f;
  for (size_t i = 48000; i < posL.size(); ++i) {
    maxDiffSwapped = std::max(maxDiffSwapped, std::abs(posL[i] - negR[i]));
    maxDiffSwapped = std::max(maxDiffSwapped, std::abs(posR[i] - negL[i]));
  }
  std::printf("[IrWidthTest] max |posL-negR| and |posR-negL| (0 == exact swap) = %.6f\n",
              static_cast<double>(maxDiffSwapped));
  EXPECT_LT(maxDiffSwapped, 5e-3f)
      << "Width=-100% doesn't appear to be an exact L/R swap of Width=+100%";
}

// Both Size and Width persist and survive a full save/restore round trip,
// same as the envelope (ir_decay_tests.cpp's ShapeReappliesAfterStateRestore).
TEST(IrSizeWidthTest, BothReapplyAfterStateRestore) {
  constexpr int kTotalBlocks = 90;
  const auto impulse = makeDelayedImpulse(kTotalBlocks);

  ChainTestProcessor proc;
  proc.setPlayConfigDetails(2, 2, kFs, kBlock);
  proc.prepareToPlay(kFs, kBlock);
  seedMonoIrChain(proc, "blk-a", "reverb-ir-stereo-test.wav");
  ASSERT_TRUE(waitForChainLoaded(proc));
  ASSERT_TRUE(proc.setBlockParam("blk-a", "mix", 1.0));
  setSizeAndWaitForRebuild(proc, "blk-a", 0.7);
  ASSERT_TRUE(setWidthAndWaitForRebuild(proc, "blk-a", 0.9));
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
  std::printf("[IrSizeWidthTest] max |diff| before save vs after restore: %.9f\n",
              static_cast<double>(maxAbsDiff));
  // Looser than IrDecayTest.ShapeReappliesAfterStateRestore's 1e-4 (which
  // reaches ~1e-8 in practice): that test never touches Size, so its two
  // engines are built at the exact same declared rate (ratio 1.0). This one
  // uses Size=70% (irSizeDurationRatio(0.7) is an irrational-ish ~2.51x, not
  // a round number), so both convolvers go through JUCE's resampler at a
  // genuinely non-trivial ratio, independently, in two separate process
  // instances - a small floating-point divergence there is expected and
  // audibly negligible (~-42 dB), unlike a broken reapply, which would show
  // up as near-total difference, silence, or the flat/untouched engine.
  EXPECT_LT(maxAbsDiff, 0.02f)
      << "restore didn't reapply the persisted Size/Width shape to the reloaded engine";
}
