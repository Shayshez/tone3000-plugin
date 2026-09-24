#include "Processor.h"
#if !HEADLESS
#include "Editor.h"
#endif
#include <cmath>
#include <limits>
#include <random>
#include <cstring>
#include <tuple>

// StandalonePluginHolder: used to inspect the audio device's active channels
// so we can detect a mono input or output (see standaloneMonoInput /
// standaloneMonoOutput).
#if !HEADLESS && JucePlugin_Build_Standalone && ! JUCE_USE_CUSTOM_PLUGIN_STANDALONE_APP
#include <juce_audio_plugin_client/Standalone/juce_StandaloneFilterWindow.h>
#endif

// ##############
// MAIN PROCESSOR
// ##############
TONE3000Processor::TONE3000Processor()
    : AudioProcessor(BusesProperties()
                         .withInput("Input", juce::AudioChannelSet::stereo(), true)
                         .withOutput("Output", juce::AudioChannelSet::stereo(), true)),
      parameters(*this, nullptr, juce::Identifier("PARAMETERS"), createParameterLayout()),
      // Unity-gain seeds so the duplicators own valid coefficients from birth;
      // prepareToPlay applies the real voicing via updateEqCoefficients().
      bassFilter(juce::dsp::IIR::Coefficients<float>::makeLowShelf(48000, 150.0f, 0.707f, 1.0f)),
      midFilter(juce::dsp::IIR::Coefficients<float>::makePeakFilter(48000, 425.0f, 0.7f, 1.0f)),
      trebleFilter(juce::dsp::IIR::Coefficients<float>::makeHighShelf(48000, 1800.0f, 0.707f, 1.0f)),
      // 2 threads for background loading, +1 so a chain-edit-fade release
      // waiter (releaseChainEditFadeWhenLoadsSettle) never serializes the
      // very loads it is waiting on.
      loadingThreadPool(3) {
  // Attach the file logger first thing: state restore (and the background
  // model loads it queues) runs before prepareToPlay, and its diagnostics
  // used to vanish because the logger didn't exist yet.
  if (!juce::Logger::getCurrentLogger()) {
    juce::Logger::setCurrentLogger(new juce::FileLogger(getLogFile(), "TONE3000 JUCE Log"));
  }

  // One-line snapshot of everything read from the shared machine-wide
  // settings file at construction, plus the file's own path: the first
  // thing to check when a "settings/login don't persist" report comes in
  // (wrong/unwritable path, or the file simply isn't there yet).
  juce::Logger::writeToLog(
      "[Processor] Settings file: " + getSettingsFile().getFullPathName() +
      " (exists=" + juce::String(getSettingsFile().existsAsFile() ? "yes" : "no") +
      ") | multiCore=" + juce::String(multiCoreEnabled.load() ? "on" : "off"));

  resolveParamRefs();

  // Age out unused drop-loaded model stash files and sweep IR temp files
  // leaked by older builds (both no-ops after the process's first instance).
  cleanLocalModelStash();
  cleanLeakedIrTempFiles();

  // Oversampling settings apply through a message-thread bounce (see
  // applyOversamplingSettings); the relays can fire from any thread.
  parameters.addParameterListener("osEnabled", this);
  parameters.addParameterListener("osFactor", this);

  // The preset-managed faceplate params feed getChainState's atDefault flag,
  // so their changes must reach pollers (see parameterChanged).
  for (const auto& paramId : presetParameterIds())
    parameters.addParameterListener(paramId, this);

  // MIDI performance events (delivered on the message thread, see
  // MidiMapper): program changes and prev/next steps walk the preset list, a
  // mapped block-power stomp routes through the normal undoable chain edit
  // path.
  midiMapper.onProgramChange = [this](int program) { loadPresetAtIndex(program); };
  midiMapper.onPresetStep = [this](int delta) { stepPreset(delta); };
  midiMapper.onBlockPowerToggle = [this](int index) { toggleBlockPower(index); };

  // Starts at its minimum slot layout (kMinLaneSlots pass-through insert
  // placeholders).
  normalizeLaneInserts(chain);
  // Built once (capturing only `this`) so invoking the boundary on the audio
  // thread never constructs a std::function per block.
  chainStageFunc = [this](float** inputs, float** outputs, int numFrames) {
    processOversampledChainStage(inputs, outputs, numFrames);
  };
  DBG("TONE3000Processor constructed");
}

// One-time string-keyed lookups; everything after this reads the atomics.
void TONE3000Processor::resolveParamRefs() {
  auto get = [this](const char* id) { return parameters.getRawParameterValue(id); };
  paramRefs.inputLevel = get("inputLevel");
  paramRefs.outputLevel = get("outputLevel");
  paramRefs.outputPan = get("outputPan");
  paramRefs.toneBass = get("toneBass");
  paramRefs.toneMid = get("toneMid");
  paramRefs.toneTreble = get("toneTreble");
  paramRefs.gateThreshold = get("gateThreshold");
  paramRefs.gateEnabled = get("gateEnabled");
  paramRefs.toneEqEnabled = get("toneEqEnabled");
  paramRefs.targetLoudness = get("targetLoudness");
  paramRefs.calibrateInput = get("calibrateInput");
  paramRefs.inputCalibrationLevel = get("inputCalibrationLevel");
  paramRefs.osEnabled = get("osEnabled");
  paramRefs.osFactor = get("osFactor");
  paramRefs.bypass = get("bypass");
  paramRefs.outputMute = get("outputMute");
}

juce::AudioProcessorParameter* TONE3000Processor::getBypassParameter() const {
  return parameters.getParameter("bypass");
}

juce::AudioProcessorValueTreeState::ParameterLayout TONE3000Processor::createParameterLayout() {
  juce::AudioProcessorValueTreeState::ParameterLayout layout;
  // The second ParameterID argument is the AU version hint. AU (Logic /
  // GarageBand) keys parameter identity on it, so once released: never reuse
  // or renumber a hint, and give every new parameter a fresh one.

  // Normalized 0..1 knob params get an explicit 1e-4 interval. The
  // (min, max, default) AudioParameterFloat constructor silently bakes a
  // 0.01 interval into the range, and the webview slider relay snaps every
  // UI-set value to that grid: 0.48 dB steps on the ±24 dB knobs, which
  // mangled typed values (-4.0 landed on -3.8; GitHub issue #16). 1e-4
  // matches the UI's own text-entry/fine-drag rounding (KnobControl rounds
  // to 4 decimals), so the snap never moves a value the UI can produce.
  const auto normParam = [](const char* id, int versionHint, float defaultValue) {
    return std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{id, versionHint}, id,
        juce::NormalisableRange<float>(0.0f, 1.0f, 0.0001f), defaultValue);
  };

  layout.add(normParam("inputLevel", 1, 0.5f));
  layout.add(std::make_unique<juce::AudioParameterFloat>(
      juce::ParameterID{"toneBass", 2}, "toneBass", 0.0f, 10.0f, 5.0f));
  layout.add(std::make_unique<juce::AudioParameterFloat>(
      juce::ParameterID{"toneMid", 3}, "toneMid", 0.0f, 10.0f, 5.0f));
  layout.add(std::make_unique<juce::AudioParameterFloat>(
      juce::ParameterID{"toneTreble", 4}, "toneTreble", 0.0f, 10.0f, 5.0f));
  layout.add(normParam("outputLevel", 5, 0.5f));
  layout.add(std::make_unique<juce::AudioParameterFloat>(
      juce::ParameterID{"gateThreshold", 6}, "gateThreshold", -100.0f, 0.0f, -80.0f));
  layout.add(std::make_unique<juce::AudioParameterFloat>(
      juce::ParameterID{"targetLoudness", 7}, "targetLoudness", -60.0f, 0.0f, -18.0f));
  layout.add(std::make_unique<juce::AudioParameterBool>(
      juce::ParameterID{"calibrateInput", 8}, "calibrateInput", false));
  layout.add(std::make_unique<juce::AudioParameterFloat>(
      juce::ParameterID{"inputCalibrationLevel", 9}, "inputCalibrationLevel", -60.0f, 60.0f, 12.0f));

  // Faceplate power switches (gate + global 3-band tone stack).
  layout.add(std::make_unique<juce::AudioParameterBool>(
      juce::ParameterID{"gateEnabled", 10}, "gateEnabled", true));
  layout.add(std::make_unique<juce::AudioParameterBool>(
      juce::ParameterID{"toneEqEnabled", 11}, "toneEqEnabled", true));

  // Pan trim: 0.5 = centered (no effect), otherwise an opposing ±12 dB trim
  // between the two output channels, applied in the post-chain image stage
  // (see processImageStage). Active whenever the rig can reproduce two
  // channels, positioning whatever the chain output already carries there
  // (duplicated mono, genuine stereo input, or a Dual Mono block's own
  // widening) — the UI hides the knob when the rig can't.
  layout.add(normParam("outputPan", 12, 0.5f));

  // Oversampling (Advanced settings; see ChainOversampler.h). Deliberately
  // not automatable: a factor change rebuilds every NAM engine and
  // re-prepares the whole chain. Choice index i maps to factor 2^(i+1).
  layout.add(std::make_unique<juce::AudioParameterBool>(
      juce::ParameterID{"osEnabled", 34}, "osEnabled", false,
      juce::AudioParameterBoolAttributes().withAutomatable(false)));
  layout.add(std::make_unique<juce::AudioParameterChoice>(
      juce::ParameterID{"osFactor", 35}, "osFactor", juce::StringArray{"2x", "4x", "8x"}, 0,
      juce::AudioParameterChoiceAttributes().withAutomatable(false)));

  // Global Bypass (also the host's bypass parameter, see getBypassParameter)
  // and output Mute. Performance switches, not tone: deliberately absent
  // from presetParameterIds, so loading a preset never flips them.
  layout.add(std::make_unique<juce::AudioParameterBool>(
      juce::ParameterID{"bypass", 36}, "Bypass", false));
  layout.add(std::make_unique<juce::AudioParameterBool>(
      juce::ParameterID{"outputMute", 37}, "Mute", false));

  return layout;
}

int TONE3000Processor::resolvedOversampleFactor() const {
  if (paramRefs.osEnabled == nullptr || paramRefs.osFactor == nullptr)
    return 1;
  if (paramRefs.osEnabled->load() < 0.5f)
    return 1;
  const int choiceIndex = static_cast<int>(std::lround(paramRefs.osFactor->load()));
  return 1 << (juce::jlimit(0, 2, choiceIndex) + 1);
}

void TONE3000Processor::parameterChanged(const juce::String& parameterID, float newValue) {
  juce::ignoreUnused(newValue);
  if (parameterID == "osEnabled" || parameterID == "osFactor") {
    // Defer to the message thread: this can fire from the UI relays or host
    // restore, and the apply is heavy.
    triggerAsyncUpdate();
    return;
  }
  // A preset-managed faceplate param moved, so getChainState's atDefault may
  // have flipped. Deferred like block-param drags: a real bump per change
  // would re-ship the whole chain state at knob-drag/automation rates.
  deferredRevisionBump();
}

void TONE3000Processor::handleAsyncUpdate() {
  applyOversamplingSettings();
}

// Message thread. Re-rates the whole chain domain after an osEnabled/osFactor
// change: the oversampler and every linear engine (EQ/spectrum/smoothers,
// plus each IR block's base-rate island; the convolvers themselves stay at
// the base rate untouched) re-prepare in place under the chain-edit fade;
// NAM engines need a different phase count, so they drop to dry passthrough
// and rebuild off-thread from the block's in-memory model cache, fading back
// in as each lands.
void TONE3000Processor::applyOversamplingSettings() {
  const int newFactor = resolvedOversampleFactor();
  if (newFactor == chainOversampleFactor.load())
    return;

  juce::Logger::writeToLog("[Processor] Oversampling ×" +
                           juce::String(chainOversampleFactor.load()) + " -> ×" +
                           juce::String(newFactor) + " (chain rate " +
                           juce::String(kChainBaseSampleRate * newFactor) + " Hz)");

  // Glide the chain output to silence, splice the rate change in between
  // callbacks, glide back: the same fade structural chain edits use.
  ChainEditFade fade(*this);
  juce::ScopedLock lock(chainMutex);

  chainOversampleFactor.store(newFactor);
  chainOversampler.prepare(newFactor, juce::jmax(1, chainBaseBlockSize()));

  // The chain-domain scratch grows with the factor; the RT path never
  // resizes it.
  laneDryScratch.setSize(2, juce::jmax(1, chainDomainBlockSize()), false, false, true);
  laneDryScratch.clear();
  for (auto* scratch : {&dualLeftBuf, &dualLeftDryScratch, &dualRightBuf, &dualRightDryScratch}) {
    scratch->setSize(1, juce::jmax(1, chainDomainBlockSize()), false, false, true);
    scratch->clear();
  }

  prepareChain(chain);

  // IR blocks need nothing here: their convolvers run at the base rate
  // behind per-block islands (re-prepared by prepareChain above), so neither
  // the kernel nor the tail report moves with the factor.
  for (auto& block : chain) {
    if (block->type == ChainBlockType::NAM && block->loaded && !block->modelLoading) {
      // In-flight loads are left alone: the apply path's factor-drift guard
      // re-queues them itself.
      block->loaded = false;
      block->modelLoading = true;
      queueActiveModelLoad(*block);
    }
  }

  bumpChainRevision();
}

TONE3000Processor::~TONE3000Processor() {
  parameters.removeParameterListener("osEnabled", this);
  parameters.removeParameterListener("osFactor", this);
  for (const auto& paramId : presetParameterIds())
    parameters.removeParameterListener(paramId, this);
  cancelPendingUpdate();

  releaseResources();

  // Clean up the chain only when the processor is actually being destroyed
  {
    juce::ScopedLock lock(chainMutex);
    chain.clear();
  }

  juce::Logger::writeToLog("[Processor] Destructor called");

  // Clean up the logger to prevent leaks
  juce::Logger::setCurrentLogger(nullptr);
}

// #############
// JUCE SETTINGS
// #############
const juce::String TONE3000Processor::getName() const {
  return "TONE3000";
}

bool TONE3000Processor::acceptsMidi() const {
  // MIDI in feeds the mapping engine only (CC/note → parameter, see
  // MidiMapper); the plugin is still an audio effect, not a synth.
  return true;
}
bool TONE3000Processor::producesMidi() const {
  return false;
}
bool TONE3000Processor::isMidiEffect() const {
  return false;
}
double TONE3000Processor::getTailLengthSeconds() const {
  // Two tail sources, report the longer one:
  //  - The longest loaded IR (reverb IRs run whole seconds; cab IRs sit far
  //    below the DC-blocker floor). Tracked lock-free in irTailBaseSamples
  //    (this potentially RT-adjacent query must not take the chain lock)
  //    and refreshed wherever the set of live IR engines changes (see
  //    refreshIrTailLength). IRs always convolve at the base rate, so the
  //    count is over kChainBaseSampleRate regardless of oversampling.
  //  - The 5 Hz first-order DC blocker decays over ~10 cycles (2 s); the
  //    reference NAM plugin reports the same allowance for VST3 tail checks.
  const double irTailSeconds = irTailBaseSamples.load() / kChainBaseSampleRate;
  const double dcBlockerTailSeconds = 10.0 / 5.0;
  return std::max(irTailSeconds, dcBlockerTailSeconds);
}

int TONE3000Processor::getNumPrograms() {
  return 1;
}
int TONE3000Processor::getCurrentProgram() {
  return 0;
}
void TONE3000Processor::setCurrentProgram(int index) {
  juce::ignoreUnused(index);
}
const juce::String TONE3000Processor::getProgramName(int index) {
  juce::ignoreUnused(index);
  return {};
}
void TONE3000Processor::changeProgramName(int index, const juce::String& newName) {
  juce::ignoreUnused(index, newName);
}

// #############################
// PREPARE A SINGLE CHAIN
// #############################
// Everything in a chain lives in the chain domain: chainSampleRate(), block
// sizes up to chainDomainBlockSize(). Host-rate changes only ever re-prepare
// because the domain block size depends on the host block size; oversampling
// changes re-prepare because the rate itself moves.
void TONE3000Processor::prepareChain(std::vector<std::unique_ptr<ChainBlock>>& blocks) {
  const int domainBlockSize = chainDomainBlockSize();
  const double chainRate = chainSampleRate();

  for (auto& block : blocks) {
    if (block->type == ChainBlockType::NAM) {
      if (block->namEngine != nullptr) {
        block->namEngine->prepare(domainBlockSize);
        DBG("NAM engine prepared for block: " << block->id);
      } else {
        DBG("Warning: NAM block " << block->id << " has no engine to prepare");
      }
    } else if ((block->type == ChainBlockType::IR || block->type == ChainBlockType::CAB) &&
               block->convolverMono != nullptr) {
      // Convolvers always run at the base rate behind the block's island
      // (see ChainBlock::irBaseRateIsland), so their spec only tracks the
      // base block size, never the oversampling factor. CAB blocks reuse
      // this same convolverMono/island machinery directly (single-slot v1;
      // see ChainBlockType::CAB) rather than a separate field.
      juce::dsp::ProcessSpec spec{kChainBaseSampleRate,
                                  static_cast<juce::uint32>(chainBaseBlockSize()), 2};
      block->convolverMono->prepare(spec);
      if (block->convolverStereo != nullptr)
        block->convolverStereo->prepare(spec);

      // Reset normalization smoother to current gain to prevent jumps on
      // re-prepare - irEffectiveNormalizationGainLinear, so a re-prepare
      // mid-Size-edit doesn't momentarily snap back to the uncompensated
      // base gain.
      if (block->loaded) {
        block->irNormalizationSmoother.reset(chainRate, 0.05f);
        block->irNormalizationSmoother.setCurrentAndTargetValue(
            block->irEffectiveNormalizationGainLinear);
      }

      DBG("IR/CAB convolvers re-prepared for block: " << block->id);
    }

    // Every IR/CAB block keeps its base-rate island in step with the live
    // factor (bypass at ×1). Prepared even while unloaded; a later engine
    // apply re-prepares anyway, this just keeps the invariant simple.
    if (block->type == ChainBlockType::IR || block->type == ChainBlockType::CAB) {
      block->irBaseRateIsland.prepare(chainOversampleFactor.load(),
                                      juce::jmax(1, chainBaseBlockSize()));
    }
    if (block->type == ChainBlockType::IR) {
      // Always at the base rate (see BlockPredelay), and snapped to the
      // stored value, not ramped: a host resample/oversampling re-prepare
      // has no live signal continuity to protect. IR only - CAB has no
      // predelay field at all (structurally minimal by design).
      block->predelay.prepare(kChainBaseSampleRate,
                              block->predelayNormalized * BlockPredelay::kMaxDelayMs);
    }

    if (block->type == ChainBlockType::DUAL_MONO) {
      // Hard reset (not just a new target): a re-prepare has no live signal
      // continuity to protect, same reasoning as every other smoother above.
      block->dualLeftPanSmoother.reset(chainRate, 0.05f);
      block->dualRightPanSmoother.reset(chainRate, 0.05f);
      block->dualWidthSmoother.reset(chainRate, 0.05f);
      block->dualLeftSoloGainSmoother.reset(chainRate, 0.05f);
      block->dualRightSoloGainSmoother.reset(chainRate, 0.05f);
      block->dualLeftPanSmoother.setCurrentAndTargetValue(block->dualLeftPanNormalized);
      block->dualRightPanSmoother.setCurrentAndTargetValue(block->dualRightPanNormalized);
      block->dualWidthSmoother.setCurrentAndTargetValue(block->dualWidthNormalized);
      const bool leftForcedSilent = block->dualLeftMuted;
      const bool rightForcedSilent = block->dualRightMuted;
      block->dualLeftSoloGainSmoother.setCurrentAndTargetValue(
          leftForcedSilent || (block->dualSoloRight && !block->dualSoloLeft) ? 0.0f : 1.0f);
      block->dualRightSoloGainSmoother.setCurrentAndTargetValue(
          rightForcedSilent || (block->dualSoloLeft && !block->dualSoloRight) ? 0.0f : 1.0f);
      block->dualLeftPolaritySmoother.reset(chainRate, 0.05f);
      block->dualRightPolaritySmoother.reset(chainRate, 0.05f);
      block->dualLeftPolaritySmoother.setCurrentAndTargetValue(block->dualLeftInvert ? -1.0f
                                                                                     : 1.0f);
      block->dualRightPolaritySmoother.setCurrentAndTargetValue(block->dualRightInvert ? -1.0f
                                                                                       : 1.0f);
      // Re-prepare has no live signal continuity to protect (same as every
      // engine above); StereoOffset::prepare is itself a hard reset.
      block->dualAlign.prepare(chainRate, domainBlockSize);
      block->dualGoniometer.prepare(chainRate);
      // Recurses at most one level deep: a dual child is never itself a
      // DUAL_MONO block (nothing on the creation path can produce one -
      // loadToneIntoDualSlot only ever loads an ordinary tone), so this
      // can't recurse further regardless of what the type system would
      // technically allow.
      prepareChain(block->dualLeft);
      prepareChain(block->dualRight);
    }

    // Initialize per-block smoothers (input gain, output gain, mix, NAM
    // normalization). The RT path only ever calls setTargetValue on these;
    // reset() belongs here and in the model-apply path, never per block.
    block->inputGainSmoother.reset(chainRate, 0.05f);
    block->outputGainSmoother.reset(chainRate, 0.05f);
    block->mixSmoother.reset(chainRate, 0.05f);
    block->namNormalizationSmoother.reset(chainRate, 0.05f);
    block->inputGainSmoother.setCurrentAndTargetValue(1.0f);   // updated on first process
    block->outputGainSmoother.setCurrentAndTargetValue(1.0f);  // updated on first process
    block->mixSmoother.setCurrentAndTargetValue(block->mixNormalized);
    block->namNormalizationSmoother.setCurrentAndTargetValue(1.0f);
    block->wetFadeGain.reset(chainRate, kWetFadeSeconds);
    block->wetFadeGain.setCurrentAndTargetValue(block->enabled ? 1.0f : 0.0f);
    block->swapWetMuteGain.reset(chainRate, kWetFadeSeconds);
    block->swapWetMuteGain.setCurrentAndTargetValue(1.0f);

    // Per-block EQ + spectrum analyzer need the sample rate for their math.
    block->eq.prepare(chainRate);
    block->spectrum.prepare(chainRate);
  }
}

// See the declaration. Predelay pushes an IR's audible tail out in time
// (silence, then the IR), so it counts toward the reported length too, or
// hosts would truncate a predelayed reverb tail's rendering early.
void TONE3000Processor::refreshIrTailLength() {
  int maxSamples = 0;
  for (const auto& b : chain)
    if (b->type == ChainBlockType::IR && b->convolverMono != nullptr) {
      const int predelaySamples = static_cast<int>(std::llround(
          b->predelayNormalized * BlockPredelay::kMaxDelayMs * 0.001 * kChainBaseSampleRate));
      maxSamples = std::max(maxSamples, b->irLengthBaseSamples + predelaySamples);
    } else if (b->type == ChainBlockType::CAB && b->convolverMono != nullptr) {
      // No predelay field on CAB (structurally minimal by design) - just
      // the kernel length.
      maxSamples = std::max(maxSamples, b->irLengthBaseSamples);
    }
  irTailBaseSamples.store(maxSamples);
}

// Constant-power pan gains for a chain at position `pan` (0 = hard left,
// 1 = hard right): cos into the left output, sin into the right.
static std::pair<float, float> constantPowerPanGains(float pan) {
  const float angle = juce::jlimit(0.0f, 1.0f, pan) * juce::MathConstants<float>::halfPi;
  return {std::cos(angle), std::sin(angle)};
}

// Peak absolute sample across the buffer's first `numChannels` channels.
static float bufferPeak(const juce::AudioBuffer<float>& buffer, int numChannels, int numSamples) {
  float peak = 0.0f;
  for (int ch = 0; ch < numChannels; ++ch) {
    const auto* data = buffer.getReadPointer(ch);
    for (int i = 0; i < numSamples; ++i)
      peak = std::max(peak, std::abs(data[i]));
  }
  return peak;
}

// Volume-style knob taper shared by every gain/level control (main In/Out
// Level, per-block In/Out gain, Dual Mono's per-side Vol - anything using
// gainDbScale on the UI side, which mirrors this exactly): 0.5 = unity
// (0 dB), 1.0 = +24 dB, unchanged from the old plain ±24 dB linear map.
// Below unity, though, the curve is logarithmic (40 dB per decade of knob
// travel toward zero) rather than linear, so it reaches true silence
// (-inf dB, exact zero gain) as the knob approaches fully closed instead of
// flooring at a still-audible -24 dB - closed should mean muted, not just
// quiet. The last fraction of a percent of travel is treated as fully
// closed outright so an exactly-zero knob value (and float rounding near
// it) always reads as true silence, not a very large but finite negative
// dB value.
static constexpr float kGainKnobSilenceThreshold = 0.001f;
static float gainKnobDb(float norm) {
  if (norm <= kGainKnobSilenceThreshold)
    return -std::numeric_limits<float>::infinity();
  if (norm >= 0.5f)
    return (norm - 0.5f) * 48.0f;
  return 40.0f * std::log10(norm / 0.5f);
}

// Main-stage level as a linear gain: 0.5 = unity, +24 dB at max, true
// silence at/near fully closed - see gainKnobDb.
static float mainStageGain(float level) {
  return juce::Decibels::decibelsToGain(gainKnobDb(level));
}

// Inverse of gainKnobDb: what knob position reads as this many dB. Mirrors
// gainDbScale.fromDisplay in knobScale.ts exactly, same as gainKnobDb
// mirrors that file's own toDisplay - used by Auto Balance to turn "raise
// this side by N dB" into a new outputGainNormalized. Non-finite (-inf,
// silence) maps to fully closed; the caller clamps to [0, 1] the same way
// pollDualAutoAlign already clamps its own ms-to-knob-position inverse.
static float gainKnobNormFromDb(float db) {
  if (!std::isfinite(db))
    return 0.0f;
  if (db >= 0.0f)
    return 0.5f + db / 48.0f;
  return 0.5f * std::pow(10.0f, db / 40.0f);
}

// Per-channel pan gain: 0.5 = centered, unity on both channels. Sweeping
// toward one side only attenuates the OPPOSITE channel, down to silence at
// a hard pan - it never boosts the near channel above unity, so turning the
// knob can only ever reduce perceived loudness, never spike it. (An earlier
// version mirrored the old two-lane Balance control, which boosted the near
// side up to +12 dB while cutting the other - fine for balancing two
// independently-gain-staged chains, wrong for panning a single already-
// coherent signal, where it read as a loudness jump.) Equal-power taper
// (cosine) rather than linear, so the fade-out doesn't sound abrupt.
static float panChainGain(float pan, int channel) {
  const float p = (pan - 0.5f) * 2.0f;  // -1 (hard left) .. +1 (hard right)
  const float away = channel == 0 ? juce::jmax(0.0f, p) : juce::jmax(0.0f, -p);
  return std::cos(away * juce::MathConstants<float>::halfPi);
}

// The two gains of the post-chain image stage: a pan tilt across whatever
// the two channels already carry (see the image stage's own comment on when
// this is active). At the centered default both are unity.
struct ImageGains { float l, r; };
static ImageGains imageMatrixGains(float pan) {
  return {panChainGain(pan, 0), panChainGain(pan, 1)};
}

// Stereo input = stereo main bus, minus the standalone case where it isn't
// really: a mono input device. Pure capability; the input-mode selection
// doesn't affect it (the UI needs the button to stay visible so the user can
// cycle back to stereo). Stereo output is the same idea on the way out: a
// mono host bus or a one-channel output device can't reproduce a stereo
// image, so Pan stays inert (the UI greys/hides it). Both reported through
// getChainState, so bump the revision on change.
void TONE3000Processor::updateStereoIoDetection() {
  const bool stereoIn = getMainBusNumInputChannels() >= 2 && !standaloneMonoInput.load();
  const bool stereoOut = getMainBusNumOutputChannels() >= 2 && !standaloneMonoOutput.load();
  const bool inChanged = stereoInputDetected.exchange(stereoIn) != stereoIn;
  const bool outChanged = stereoOutputDetected.exchange(stereoOut) != stereoOut;
  if (inChanged || outChanged)
    bumpChainRevision();
}

void TONE3000Processor::setInputMode(InputMode mode) {
  inputMode.store(static_cast<int>(mode));
  bumpChainRevision();
  DBG("Input mode: " << inputModeToString(mode));
}

// Physical group delay of a ChainBoundaryResampler at this host rate, in
// host samples. GetLatency() only counts the warm-up prefill and misses the
// residual group delay of the two Lanczos kernels (2-5 samples, growing with
// the rate ratio), so an impulse is run through a scratch boundary and the
// peak located: exact by construction, and cheap enough for prepareToPlay.
static int measureChainBoundaryLatency(double hostRate, int blockSize) {
  ChainBoundaryResampler probe(kChainBaseSampleRate);
  probe.Reset(hostRate, blockSize);

  const int total = ((probe.GetLatency() + 2 * blockSize) / blockSize + 1) * blockSize;
  juce::AudioBuffer<float> in(2, total), out(2, total);
  in.clear();
  out.clear();
  in.setSample(0, 0, 1.0f);
  in.setSample(1, 0, 1.0f);

  auto identity = [](float** inputs, float** outputs, int frames) {
    juce::FloatVectorOperations::copy(outputs[0], inputs[0], frames);
    juce::FloatVectorOperations::copy(outputs[1], inputs[1], frames);
  };
  for (int offset = 0; offset < total; offset += blockSize) {
    float* ins[2] = {in.getWritePointer(0, offset), in.getWritePointer(1, offset)};
    float* outs[2] = {out.getWritePointer(0, offset), out.getWritePointer(1, offset)};
    probe.ProcessBlock(ins, outs, blockSize, identity);
  }

  int peak = 0;
  float best = 0.0f;
  const float* y = out.getReadPointer(0);
  for (int i = 0; i < total; ++i)
    if (std::abs(y[i]) > best) {
      best = std::abs(y[i]);
      peak = i;
    }
  return peak;
}

// Tone-stack corners are fixed by design, but hosts can run rates low enough
// to put them above Nyquist (clap-validator probes 1234.5678 Hz, where even
// the 1 kHz mid peak is out of range) and JUCE's bilinear designs return
// unstable coefficients there (the guarding jassert compiles out in release,
// and the filter output runs away to ±inf). Pin the corner just under
// Nyquist instead: magnitude response degrades gracefully, stability holds.
static float clampBelowNyquist(double sampleRate, float frequency) {
  return juce::jmin(frequency, static_cast<float>(sampleRate * 0.49));
}

// #############################
// PREPARATIONS BEFORE RT THREAD
// #############################
void TONE3000Processor::prepareToPlay(double sampleRate, int samplesPerBlock) {
  hostSampleRate = sampleRate;
  maxBlockSize = samplesPerBlock;

  tuner.prepare(sampleRate);

  // CPU readout: proportion of the callback budget spent in processBlock.
  loadMeasurer.reset(sampleRate, samplesPerBlock);

  juce::Logger::writeToLog("[Processor] prepareToPlay: sampleRate=" + juce::String(sampleRate) +
                           ", samplesPerBlock=" + juce::String(samplesPerBlock));

  // Prime the cached parameter values from the resolved atomics.
  updateCachedParameters();


  // Detect mono input/output devices in the standalone app. The device
  // restarts (and re-runs prepareToPlay) whenever the user changes the audio
  // setup, so this stays in sync with the selected device. Hosts (VST3/AU)
  // never take this path; channel layouts there come from the bus
  // configuration.
  standaloneMonoInput.store(false);
  standaloneMonoOutput.store(false);
#if !HEADLESS && JucePlugin_Build_Standalone && ! JUCE_USE_CUSTOM_PLUGIN_STANDALONE_APP
  if (wrapperType == wrapperType_Standalone) {
    if (auto* holder = juce::StandalonePluginHolder::getInstance())
      if (auto* device = holder->deviceManager.getCurrentAudioDevice()) {
        standaloneMonoInput.store(device->getActiveInputChannels().countNumberOfSetBits() == 1);
        standaloneMonoOutput.store(device->getActiveOutputChannels().countNumberOfSetBits() ==
                                   1);
      }
  }
#endif

  updateStereoIoDetection();

  // Chain-domain resampling boundary.
  // Engaged whenever the host rate differs from the chain base rate, even
  // for an empty chain, so reported latency is a constant per host rate and
  // chain edits never trigger a PDC change. At a 48k host the boundary is
  // dropped entirely and the chain stage runs directly on the host buffer.
  const bool boundaryNeeded = std::abs(sampleRate - kChainBaseSampleRate) > 0.1;
  if (boundaryNeeded) {
    if (chainBoundary == nullptr)
      chainBoundary = std::make_unique<ChainBoundaryResampler>(kChainBaseSampleRate);
    chainBoundary->Reset(sampleRate, juce::jmax(1, samplesPerBlock));
    // Not GetLatency(): that under-reports by the Lanczos kernels' group
    // delay, and hosts align dry paths against this number.
    chainBoundaryLatency = measureChainBoundaryLatency(sampleRate, juce::jmax(1, samplesPerBlock));
  } else {
    chainBoundary.reset();
    chainBoundaryLatency = 0;
  }
  // The oversampler is minimum-phase (zero reported latency), so the boundary
  // remains the only latency source at any factor.
  setLatencySamples(chainBoundaryLatency);
  DBG("Chain boundary " << (boundaryNeeded ? "engaged" : "bypassed")
      << " (latency: " << chainBoundaryLatency << " samples)");

  // Chain oversampler.
  // Resolve the requested factor before anything chain-domain is sized: the
  // domain block size and rate both depend on it. Hosts re-run prepareToPlay
  // freely, so this also picks up a factor restored from session state.
  // Prepare every engine in the chain for the chain domain (fixed rate; the
  // domain block size depends on the host rate/block size).
  {
    juce::ScopedLock lock(chainMutex);
    chainOversampleFactor.store(resolvedOversampleFactor());
    chainOversampler.prepare(chainOversampleFactor.load(), juce::jmax(1, chainBaseBlockSize()));
    DBG("Chain oversampling ×" << chainOversampleFactor.load() << " (chain rate: "
        << chainSampleRate() << " Hz)");

    // Restore-time loads can land before the host resolves the saved
    // factor here. prepare() cannot change an engine's phase count.
    // In-flight loads already have a factor guard at installation.
    for (auto& block : chain) {
      if (block->type == ChainBlockType::NAM && block->loaded && !block->modelLoading &&
          block->namEngine != nullptr &&
          block->namEngine->getOversampleFactor() != chainOversampleFactor.load()) {
        block->loaded = false;
        block->modelLoading = true;
        queueActiveModelLoad(*block);
      }
    }
    prepareChain(chain);
  }

  juce::dsp::ProcessSpec spec{sampleRate, static_cast<juce::uint32>(samplesPerBlock), 2};
  bassFilter.prepare(spec);
  midFilter.prepare(spec);
  trebleFilter.prepare(spec);
  dcBlocker.prepare(spec);
  autoOffset.prepare(sampleRate);
  inputGate.prepare(sampleRate);

  // Chain-edit fade: host-rate. Primed audible normally, but a device can
  // start while a fade session holds the chain (launch: state restore arms
  // the mute, then the audio device opens while models still load). Priming
  // to 1 then would blast the half-loaded chain for the glide-down; honor
  // the pending mute instead and mark it landed (pre-callback, so snapping
  // is safe; this also unblocks any requester waiting out a device
  // restart).
  chainEditFadeGain.reset(sampleRate, kWetFadeSeconds);
  const bool editFadeHeld = chainEditFadePending.load();
  chainEditFadeGain.setCurrentAndTargetValue(editFadeHeld ? 0.0f : 1.0f);
  if (editFadeHeld)
    chainEditFadeDone.store(true);

  // Output-stage gain, primed from the current parameters so a restored
  // session doesn't glide in from the wrong level.
  outputGainSmoother.reset(sampleRate, 0.02);
  outputGainSmoother.setCurrentAndTargetValue(mainStageGain(cacheOutputLevel));

  // Global Bypass / Mute: primed from the current parameters (a session
  // restored bypassed or muted starts that way, no glide). The dry delay
  // matches the latency reported above; scratch is sized generously so a
  // host overshooting its promised block size doesn't allocate here.
  sceneGainSmoother.reset(sampleRate, 0.03);
  sceneGainSmoother.setCurrentAndTargetValue(juce::Decibels::decibelsToGain(sceneLevelDb.load()));
  bypassMix.reset(sampleRate, 0.02);
  bypassMix.setCurrentAndTargetValue(cacheBypass ? 1.0f : 0.0f);
  muteGain.reset(sampleRate, 0.02);
  muteGain.setCurrentAndTargetValue(cacheOutputMute ? 0.0f : 1.0f);
  bypassDry.setSize(2, juce::jmax(samplesPerBlock, 8192));
  for (auto& ring : bypassDelayRing)
    ring.assign(static_cast<size_t>(chainBoundaryLatency), 0.0f);
  bypassDelayPos = 0;

  // Post-chain pan gain: 20 ms ramps, primed from the current parameters and
  // rig so a restored session doesn't fade in from the wrong pan. Mirrors
  // the gain resolution in processImageStage (stereoOutputDetected was just
  // updated above).
  {
    const bool applyPan = stereoOutputDetected.load();
    const auto g = imageMatrixGains(applyPan ? cacheOutputPan : 0.5f);
    imageGainL.reset(sampleRate, 0.02);
    imageGainR.reset(sampleRate, 0.02);
    imageGainL.setCurrentAndTargetValue(g.l);
    imageGainR.setCurrentAndTargetValue(g.r);
  }

  // First-order high-pass at 5 Hz: removes DC offset from nonlinear NAM models
  // while staying audibly and phase-wise transparent down to the lowest bass
  // fundamentals (matches the reference NeuralAmpModelerPlugin behavior).
  *dcBlocker.state =
      *juce::dsp::IIR::Coefficients<float>::makeFirstOrderHighPass(sampleRate, 5.0f);

  bassFilter.reset();
  midFilter.reset();
  trebleFilter.reset();
  dcBlocker.reset();

  // Scratch buffers, sized once here; the RT path never resizes them.
  // The dry scratch lives in the chain domain, where a callback can carry
  // more frames than the host block (e.g. a 44.1k host upsampled to 48k).
  laneDryScratch.setSize(2, chainDomainBlockSize(), false, false, true);
  laneDryScratch.clear();
  for (auto* scratch : {&dualLeftBuf, &dualLeftDryScratch, &dualRightBuf, &dualRightDryScratch}) {
    scratch->setSize(1, chainDomainBlockSize(), false, false, true);
    scratch->clear();
  }
  chainScratchChannel.setSize(1, samplesPerBlock, false, false, true);
  chainScratchChannel.clear();

  // (Re)start the worker pool with the new callback geometry. It idles until
  // a callback actually forks (a NAM model's oversampling phase instances);
  // starting it here unconditionally keeps the multi-core toggle a pure
  // dispatch gate.
  rtWorkerPool.start(sampleRate, samplesPerBlock);

  // Tone stack coefficients at the real host rate (the construction-time
  // seeds are unity at 48 kHz).
  updateEqCoefficients();
}

// #################
// RELEASE RESOURCES
// #################
void TONE3000Processor::releaseResources() {
  juce::Logger::writeToLog("[Processor] releaseResources() called");

  // The worker pool only lives while the host is running audio callbacks
  // (prepareToPlay restarts it). Stopping here also guarantees no worker
  // outlives the buffers/lanes a stale job could reference.
  rtWorkerPool.stop();

  // DO NOT clear chain blocks here! They should persist across bypass/unbypassed states.
  // Chain blocks are managed by the plugin's state system and should only be cleared
  // when the plugin is actually destroyed or when explicitly requested by the user.
  
  // Only reset the audio processing components, not the plugin state
  bassFilter.reset();
  midFilter.reset();
  trebleFilter.reset();
  dcBlocker.reset();
  inputGate.reset();
}

bool TONE3000Processor::isBusesLayoutSupported(const BusesLayout& layouts) const {
  if (layouts.getMainOutputChannelSet() != juce::AudioChannelSet::mono() &&
      layouts.getMainOutputChannelSet() != juce::AudioChannelSet::stereo())
    return false;

#if !JucePlugin_IsSynth
  if (layouts.getMainOutputChannelSet() != layouts.getMainInputChannelSet())
    return false;
#endif

  return true;
}

// ######################
// UPDATE EQ COEFFICIENTS
// ######################
// Voicing follows the reference NeuralAmpModelerPlugin tone stack
// (BasicNamToneStack, https://github.com/sdatkinson/NeuralAmpModelerPlugin):
// knobs run 0-10 with 5 flat, each band maps linearly to dB with its own
// sweep (bass ±20 dB, mid ±15 dB, treble ±10 dB), and the mid bell widens
// when boosting (Q 0.7 vs 1.5) so pushed mids don't turn honky.
void TONE3000Processor::updateEqCoefficients() {
  const double rate = getSampleRate();
  const float bassDb = 4.0f * (cacheBassTone - 5.0f);
  const float midDb = 3.0f * (cacheMidTone - 5.0f);
  const float trebleDb = 2.0f * (cacheTrebleTone - 5.0f);

  *bassFilter.state = *juce::dsp::IIR::Coefficients<float>::makeLowShelf(
      rate, clampBelowNyquist(rate, 150.0f), 0.707f,
      juce::Decibels::decibelsToGain(bassDb));
  *midFilter.state = *juce::dsp::IIR::Coefficients<float>::makePeakFilter(
      rate, clampBelowNyquist(rate, 425.0f), midDb < 0.0f ? 1.5f : 0.7f,
      juce::Decibels::decibelsToGain(midDb));
  *trebleFilter.state = *juce::dsp::IIR::Coefficients<float>::makeHighShelf(
      rate, clampBelowNyquist(rate, 1800.0f), 0.707f,
      juce::Decibels::decibelsToGain(trebleDb));
}

// ######################
// GLOBAL 3-BAND TONE STACK
// ######################
// Runs once per block, after the DC blocker (post-chain). Skipped entirely
// while powered off; filters reset on re-enable so no stale state rings in.
void TONE3000Processor::processToneStack(juce::AudioBuffer<float>& buffer) {
  if (cacheToneEqEnabled) {
    if (!toneEqWasEnabled) {
      bassFilter.reset();
      midFilter.reset();
      trebleFilter.reset();
    }
    if (eqParamsDirty) {
      updateEqCoefficients();
      eqParamsDirty = false;
    }

    juce::dsp::AudioBlock<float> eqBlock(buffer);
    juce::dsp::ProcessContextReplacing<float> eqContext(eqBlock);
    bassFilter.process(eqContext);
    midFilter.process(eqContext);
    trebleFilter.process(eqContext);
  }
  toneEqWasEnabled = cacheToneEqEnabled;
}

// ####################
// UPDATE CACHED PARAMS
// ####################
void TONE3000Processor::updateCachedParameters() {
  constexpr float epsilon = 1e-5f;

  // Plain atomic loads; the string-keyed lookups happened once in
  // resolveParamRefs(). `tone` marks the tone-stack floats whose changes
  // must dirty the EQ coefficients.
  auto updateFloat = [&](float& cached, const std::atomic<float>* param, bool tone = false) {
    const float value = param->load();
    if (std::abs(value - cached) > epsilon) {
      cached = value;
      if (tone)
        eqParamsDirty = true;
    }
  };

  updateFloat(cacheInputLevel, paramRefs.inputLevel);
  updateFloat(cacheOutputLevel, paramRefs.outputLevel);
  updateFloat(cacheOutputPan, paramRefs.outputPan);
  updateFloat(cacheBassTone, paramRefs.toneBass, true);
  updateFloat(cacheMidTone, paramRefs.toneMid, true);
  updateFloat(cacheTrebleTone, paramRefs.toneTreble, true);
  updateFloat(cacheGateThreshold, paramRefs.gateThreshold);
  updateFloat(cacheTargetLoudness, paramRefs.targetLoudness);
  updateFloat(cacheInputCalibrationLevel, paramRefs.inputCalibrationLevel);

  auto loadBool = [](const std::atomic<float>* param) { return param->load() > 0.5f; };
  cacheCalibrateInput = loadBool(paramRefs.calibrateInput);
  cacheGateEnabled = loadBool(paramRefs.gateEnabled);
  cacheToneEqEnabled = loadBool(paramRefs.toneEqEnabled);
  cacheBypass = loadBool(paramRefs.bypass);
  cacheOutputMute = loadBool(paramRefs.outputMute);
}

// See the declaration (Processor.h). Called from processChainOnBuffer's own
// per-block loop in place of the ordinary dry-copy/input-gain/model/mix
// pipeline - a Dual Mono block splits into two independent per-block runs
// then recombines, a different shape than every other block type's single
// continuous path.
void TONE3000Processor::runDualMono(ChainBlock& dualBlock, juce::AudioBuffer<float>& buffer) {
  const int numSamples = buffer.getNumSamples();
  const int numChannels = buffer.getNumChannels();

  jassert(dualLeftBuf.getNumSamples() >= numSamples);
  jassert(dualRightBuf.getNumSamples() >= numSamples);
  // Views of exactly this block's length over the preallocated scratch (no
  // allocation: referencing constructor). The scratch itself is sized for
  // the largest chain-domain block, and processChainOnBuffer runs
  // buffer.getNumSamples() samples - handing it the scratch directly made
  // each side process its full capacity every call, so any block shorter
  // than the maximum (hosts like Logic vary block sizes) pushed stale
  // samples from earlier blocks through the sides' NAM/IR state: loud,
  // harsh noise. Fixed-size hosts (the standalone) never showed it.
  juce::AudioBuffer<float> dl(dualLeftBuf.getArrayOfWritePointers(), 1, numSamples);
  juce::AudioBuffer<float> dr(dualRightBuf.getArrayOfWritePointers(), 1, numSamples);

  // Seed: 2 channels present -> channel 0 feeds left, channel 1 feeds
  // right, distinctly. Only 1 channel present -> duplicated into both (a
  // genuine mono signal diverging into two independent paths).
  //
  // Auto Align probe: while armed for THIS block (see armDualAutoAlign),
  // both sides eat the identical sweep instead of the instrument - the
  // per-block analogue of the global mechanism's own top-of-processBlock
  // injection (see AutoOffset.h, and the guarded global call site this
  // reuses the same engine instance with, so only one of the two ever
  // actually renders per block). renderProbeInput is a no-op outside its
  // own Probing/Tail sub-states (returns false, dest untouched), so this
  // only overrides the normal seed while a sweep is actually playing.
  const bool autoOffsetTargetsThisBlock = dualBlock.id == autoOffsetTargetBlockId;
  if (autoOffsetTargetsThisBlock && autoOffset.renderProbeInput(dl.getWritePointer(0), numSamples)) {
    dr.copyFrom(0, 0, dl, 0, 0, numSamples);
  } else {
    dl.copyFrom(0, 0, buffer, 0, 0, numSamples);
    dr.copyFrom(0, 0, buffer, numChannels > 1 ? 1 : 0, 0, numSamples);
  }

  // While the probe sweep is actually in the capture window (Probing/Tail -
  // AutoOffset.h), pin both sides at 100% wet. The raw dry sweep is bit-
  // identical on both sides, so any dry bleed-through (a Mix knob left
  // under 100% - IR Player defaults to 25%) correlates trivially at zero
  // lag and non-inverted regardless of the true model relationship, and
  // dilutes/masks the real measurement - see processChainOnBuffer's
  // forceFullWet and AutoOffset.h's integration contract.
  const AutoOffset::State autoOffsetState = autoOffset.state();
  const bool forceFullWetForProbe =
      autoOffsetTargetsThisBlock && (autoOffsetState == AutoOffset::State::Probing ||
                                     autoOffsetState == AutoOffset::State::Tail);

  // Run each present child (0 or 1 element - see ChainBlock::dualLeft/
  // dualRight) through the ordinary per-block path. An empty side is a
  // no-op, leaving dl/dr as the seeded pass-through signal - a Dual Mono
  // block with nothing loaded on either side is inaudible pass-through,
  // same as adding any other still-empty block.
  processChainOnBuffer(dualBlock.dualLeft, dl, dualLeftDryScratch, 0, -1, forceFullWetForProbe);
  processChainOnBuffer(dualBlock.dualRight, dr, dualRightDryScratch, 0, -1, forceFullWetForProbe);

  // Auto Align probe capture: the raw chain outputs BEFORE Align's own
  // delay/polarity - the absolute misalignment, matching the global
  // mechanism's own capture point (processImageStage, before
  // stereoOffset.process - see its own comment there).
  if (dualBlock.id == autoOffsetTargetBlockId)
    autoOffset.captureChainOutputs(dl.getReadPointer(0), dr.getReadPointer(0), numSamples);

  // Align: corrective delay + advanced deck (Wobble/Crossover/Diffuse) on
  // the sides' raw output, before the Pan/Width recombine below - the
  // direct per-block analogue of processImageStage running the global
  // StereoOffset on chL/chR before Balance+Pan. Runs regardless of
  // numChannels/widen-vs-fold: dl/dr are always two independent chain
  // outputs about to be combined (panned+summed, or folded straight to
  // mono), and a timing/phase mismatch between them causes the same comb-
  // filtering/cancellation either way - if anything it matters MORE in the
  // fold case, where the two sides sum directly into one channel.
  {
    float* alignChannels[2] = {dl.getWritePointer(0), dr.getWritePointer(0)};
    juce::AudioBuffer<float> alignImage(alignChannels, 2, numSamples);
    // dualStereoProcessingEnabled is the whole Stereo Processing screen's
    // master bypass - forces disengaged here without touching the stored
    // dualAlignEnabled, so re-enabling restores exactly what was dialed in.
    dualBlock.dualAlign.setTarget(
        StereoOffsetParams::fromNormalized(
            dualBlock.dualAlignOffsetNormalized, dualBlock.dualAlignWobbleNormalized,
            dualBlock.dualAlignCrossoverNormalized, dualBlock.dualAlignWobbleEnabled,
            dualBlock.dualAlignCrossoverEnabled, dualBlock.dualAlignDiffuseEnabled),
        dualBlock.dualStereoProcessingEnabled && dualBlock.dualAlignEnabled);
    if (dualBlock.dualAlign.isRunning())
      dualBlock.dualAlign.process(alignImage);
  }

  // Recombine: constant-power pan per side, summed, then blended against
  // the mono sum by width. Widen-vs-fold is decided purely by
  // buffer.getNumChannels() - checking it directly is the real rule, not
  // an approximation of one, so no extra plumbing is needed to reach it
  // from here.
  const float* dlData = dl.getReadPointer(0);
  const float* drData = dr.getReadPointer(0);
  float peak = 0.0f;

  // Re-arm every smoother's target from the raw fields every call, same
  // idiom every other per-block smoother uses: the live setters only ever
  // target them, so this is what a fresh block's fields reach even before
  // the UI calls one for the first time. Solo (and Mute, forced-silent
  // below - see their own comment in ChainBlock.h) apply in both the widen
  // and fold branches below (muting a side is muting a side regardless of
  // the physical channel count), so it's armed here rather than duplicated
  // in each branch. Mute is unconditional (empty or loaded) - true
  // silence via this same gain, not the child's own `enabled` bypass
  // (bypass crossfades to the dry, unprocessed input - audible, not
  // silent, a real confusion this exact split used to cause).
  const bool leftForcedSilent = dualBlock.dualLeftMuted;
  const bool rightForcedSilent = dualBlock.dualRightMuted;
  dualBlock.dualLeftSoloGainSmoother.setTargetValue(
      leftForcedSilent || (dualBlock.dualSoloRight && !dualBlock.dualSoloLeft) ? 0.0f : 1.0f);
  dualBlock.dualRightSoloGainSmoother.setTargetValue(
      rightForcedSilent || (dualBlock.dualSoloLeft && !dualBlock.dualSoloRight) ? 0.0f : 1.0f);
  // Ø: same "re-arm every call" idiom as Solo above - folded into the same
  // per-sample gain multiply in both branches below (a +-1 sign is just
  // another gain), so a live Ø toggle glides through the same smoother
  // rather than needing a separate pass. Same master-bypass override as
  // Align above: forced neutral (+1) without touching the stored
  // dualLeftInvert/dualRightInvert.
  const bool stereoProcessingBypassed = !dualBlock.dualStereoProcessingEnabled;
  dualBlock.dualLeftPolaritySmoother.setTargetValue(
      !stereoProcessingBypassed && dualBlock.dualLeftInvert ? -1.0f : 1.0f);
  dualBlock.dualRightPolaritySmoother.setTargetValue(
      !stereoProcessingBypassed && dualBlock.dualRightInvert ? -1.0f : 1.0f);

  if (numChannels >= 2) {
    dualBlock.dualLeftPanSmoother.setTargetValue(dualBlock.dualLeftPanNormalized);
    dualBlock.dualRightPanSmoother.setTargetValue(dualBlock.dualRightPanNormalized);
    dualBlock.dualWidthSmoother.setTargetValue(dualBlock.dualWidthNormalized);

    // Pan/width read per-sample off the smoothers, not the raw *Normalized
    // fields directly - a live knob drag glides instead of stepping.
    float* outL = buffer.getWritePointer(0);
    float* outR = buffer.getWritePointer(1);
    for (int i = 0; i < numSamples; ++i) {
      const float leftPan = dualBlock.dualLeftPanSmoother.getNextValue();
      const float rightPan = dualBlock.dualRightPanSmoother.getNextValue();
      const float width = dualBlock.dualWidthSmoother.getNextValue();
      const float leftGain = dualBlock.dualLeftSoloGainSmoother.getNextValue() *
                             dualBlock.dualLeftPolaritySmoother.getNextValue();
      const float rightGain = dualBlock.dualRightSoloGainSmoother.getNextValue() *
                              dualBlock.dualRightPolaritySmoother.getNextValue();
      const auto gL = constantPowerPanGains(leftPan);
      const auto gR = constantPowerPanGains(rightPan);
      const float l = dlData[i] * leftGain;
      const float r = drData[i] * rightGain;
      const float mono = 0.5f * (l + r);
      const float panL = l * gL.first + r * gR.first;
      const float panR = l * gL.second + r * gR.second;
      outL[i] = mono + (panL - mono) * width;
      outR[i] = mono + (panR - mono) * width;
      peak = std::max(peak, std::max(std::abs(outL[i]), std::abs(outR[i])));
    }
  } else {
    // Pinned to one physical channel: Pan/Width are inert here, folding to
    // plain mono, ½(l + r).
    float* outMono = buffer.getWritePointer(0);
    for (int i = 0; i < numSamples; ++i) {
      const float leftGain = dualBlock.dualLeftSoloGainSmoother.getNextValue() *
                             dualBlock.dualLeftPolaritySmoother.getNextValue();
      const float rightGain = dualBlock.dualRightSoloGainSmoother.getNextValue() *
                              dualBlock.dualRightPolaritySmoother.getNextValue();
      outMono[i] = 0.5f * (dlData[i] * leftGain + drData[i] * rightGain);
      peak = std::max(peak, std::abs(outMono[i]));
    }
  }

  // Master EQ: the wrapper's own BlockEq (every ChainBlock carries one,
  // already prepared in prepareChain), applied once to the recombined
  // signal - no PRE concept here (unlike an ordinary block, this wrapper
  // has no model stage of its own to sit in front of), so isPre() is
  // deliberately never consulted. Re-measure peak afterward so the meter
  // reflects what actually leaves the block, same as an ordinary block's
  // own meter already does post-EQ.
  if (dualBlock.eq.isActive()) {
    dualBlock.eq.process(buffer);
    peak = 0.0f;
    for (int ch = 0; ch < buffer.getNumChannels(); ++ch) {
      const float* data = buffer.getReadPointer(ch);
      for (int i = 0; i < numSamples; ++i) peak = std::max(peak, std::abs(data[i]));
    }
  }

  // Feed the master EQ editor's analyzer with the block's final (post-EQ)
  // output, same as an ordinary block's own end-of-loop feed in
  // processChainOnBuffer - only while the UI has that analyzer open
  // (isEnabled(), toggled via setBlockSpectrumEnabled). BlockEqView/
  // SpectrumBackdrop already call generically by blockId, so the wrapper's
  // own id just needed this same push site runDualMono was missing.
  if (dualBlock.spectrum.isEnabled())
    dualBlock.spectrum.pushSamples(buffer.getReadPointer(0),
                                   buffer.getNumChannels() > 1 ? buffer.getReadPointer(1)
                                                                : nullptr,
                                   numSamples);

  // Stereo Processing screen's goniometer/correlation: the block's true
  // final output (post Pan/Width AND post Master EQ) - the whole point is
  // to show what this block actually hands downstream, so it must reflect
  // Pan/Width/Vol exactly like the ear does (an earlier version captured
  // before the recombine and missed all of that - a real bug, not a design
  // choice). A fold-mode block (one physical channel) has no L/R to speak
  // of; feeding the same mono signal to both sides is honest about that -
  // it reads as a vertical line / +1 correlation, which is exactly true.
  if (dualBlock.dualGoniometer.isEnabled())
    dualBlock.dualGoniometer.pushSamples(
        buffer.getReadPointer(0),
        buffer.getNumChannels() > 1 ? buffer.getReadPointer(1) : buffer.getReadPointer(0),
        numSamples);

  // No single well-defined "input" for a two-way split, unlike an ordinary
  // block's own input meter - both meters read the recombined output peak.
  const float peakDb = peak > 0.0f ? juce::Decibels::gainToDecibels(peak) : -60.0f;
  dualBlock.inputMeterDb.store(std::max(-60.0f, peakDb));
  dualBlock.outputMeterDb.store(std::max(-60.0f, peakDb));
}

// ##########################
// RT PROCESS A SINGLE CHAIN
// ##########################
// Runs the per-block chain loop on `buffer`. The buffer may be mono (a single side in stereo
// mode) or 1-2 channels (mono mode). All per-channel work is keyed on buffer.getNumChannels().
// Must be called while holding chainMutex.
void TONE3000Processor::processChainOnBuffer(std::vector<std::unique_ptr<ChainBlock>>& blocks,
                                             juce::AudioBuffer<float>& buffer,
                                             juce::AudioBuffer<float>& dryScratch, int beginIdx,
                                             int endIdx, bool forceFullWet) {
  const int numSamples = buffer.getNumSamples();
  const int numChannels = buffer.getNumChannels();

  if (endIdx < 0)
    endIdx = static_cast<int>(blocks.size());
  beginIdx = juce::jlimit(0, static_cast<int>(blocks.size()), beginIdx);
  endIdx = juce::jlimit(beginIdx, static_cast<int>(blocks.size()), endIdx);

  // Highest index of an enabled+loaded NAM block. Used twice: a stereo IR is only worth
  // processing in true stereo when there is no NAM downstream to collapse the image back to
  // mono, and NAM blocks *before* this index hand off at calibrated output level (see the
  // post-model gain stage below) while the last one keeps loudness normalization.
  // Computed over the whole lane even for a partial range: a branched trunk
  // is still one chain split around the tap, not two chains.
  int lastNamIndex = -1;
  for (int i = 0; i < static_cast<int>(blocks.size()); ++i) {
    const auto& b = blocks[i];
    if (b->type == ChainBlockType::NAM && b->loaded && b->enabled)
      lastNamIndex = i;
  }

  for (int idx = beginIdx; idx < endIdx; ++idx) {
    const auto& block = blocks[idx];
    if (block->type == ChainBlockType::INSERT) {
      continue;  // Insert block is pass-through, no audio effect
    }

    // Wet-path fade (see ChainBlock.h): power toggles and pending engine
    // swaps glide the block's wet mix to silence instead of splicing the
    // waveform. Bypass-bound transitions ride wetFadeGain (output crossfades
    // toward dry); engine swaps ride swapWetMuteGain (wet term mutes, the
    // dry share of the user's mix holds and never exposes the un-processed
    // input). A disabled block keeps processing until the glide reaches
    // bypass, then is skipped exactly like before.
    const bool swapPending = block->swapFadePending.load();
    const bool muteSwap = swapPending && block->swapMuteWet.load();
    const bool wantsWet = block->enabled && !(swapPending && !muteSwap);
    block->wetFadeGain.setTargetValue(block->loaded && wantsWet ? 1.0f : 0.0f);
    block->swapWetMuteGain.setTargetValue(muteSwap ? 0.0f : 1.0f);
    const bool wetSilent =
        !block->wetFadeGain.isSmoothing() && block->wetFadeGain.getCurrentValue() <= 0.001f;
    if (wetSilent)
      block->swapFadeDone.store(true);  // a waiting requester may splice now

    if (!block->loaded || (!wantsWet && wetSilent)) {
      // Not processing: park the block's meters at the floor. The EQ view can
      // still be open, so keep its analyzer fed with the pass-through audio.
      block->inputMeterDb.store(-60.0f);
      block->outputMeterDb.store(-60.0f);
      if (block->spectrum.isEnabled())
        block->spectrum.pushSamples(buffer.getReadPointer(0),
                                    numChannels > 1 ? buffer.getReadPointer(1) : nullptr,
                                    numSamples);
      continue;
    }

    if (block->type == ChainBlockType::DUAL_MONO) {
      // Self-contained: seeds/recombines its own two children directly into
      // `buffer`, in place of the ordinary dry-copy/input-gain/model/mix
      // pipeline below (which assumes one continuous signal path, not a
      // split-then-recombine shape). Still needs its OWN dry/wet crossfade
      // here, though: skipping the generic mix loop below also skips the
      // only place that ever calls wetFadeGain.getNextValue() - without
      // that, the smoother's countdown never advances, isSmoothing() never
      // clears, wetSilent (above) never goes true, and a "bypassed" Dual
      // Mono block keeps processing at full strength forever instead of
      // ever reaching the skip branch - visually off, audibly still live.
      jassert(dryScratch.getNumChannels() >= numChannels);
      jassert(dryScratch.getNumSamples() >= numSamples);
      dryScratch.copyFrom(0, 0, buffer, 0, 0, numSamples);
      if (numChannels > 1)
        dryScratch.copyFrom(1, 0, buffer, 1, 0, numSamples);

      runDualMono(*block, buffer);

      float* wetL = buffer.getWritePointer(0);
      float* wetR = numChannels > 1 ? buffer.getWritePointer(1) : nullptr;
      const float* dryL = dryScratch.getReadPointer(0);
      const float* dryR = numChannels > 1 ? dryScratch.getReadPointer(1) : nullptr;
      for (int i = 0; i < numSamples; ++i) {
        const float fade = block->wetFadeGain.getNextValue();
        wetL[i] = dryL[i] * (1.0f - fade) + wetL[i] * fade;
        if (wetR)
          wetR[i] = dryR[i] * (1.0f - fade) + wetR[i] * fade;
      }
      continue;
    }

    // Prepare dry copy before processing for mix (reuse the lane's scratch)
    jassert(dryScratch.getNumChannels() >= numChannels);
    jassert(dryScratch.getNumSamples() >= numSamples);
    dryScratch.copyFrom(0, 0, buffer, 0, 0, numSamples);
    if (numChannels > 1) {
      dryScratch.copyFrom(1, 0, buffer, 1, 0, numSamples);
    }

    // Per-block input gain (0.5 == unity, +24 dB at max, true silence at/near
    // fully closed - see gainKnobDb), applied after the dry copy so Mix
    // still blends against the untouched signal; this drives the block's
    // DSP harder/softer like a drive control. The block input meter reads
    // the post-gain signal (what the model actually receives).
    {
      block->inputGainSmoother.setTargetValue(
          juce::Decibels::decibelsToGain(gainKnobDb(block->inputGainNormalized)));

      float blockInputPeak = 0.0f;
      auto* left = buffer.getWritePointer(0);
      auto* right = numChannels > 1 ? buffer.getWritePointer(1) : nullptr;
      for (int i = 0; i < numSamples; ++i) {
        const float g = block->inputGainSmoother.getNextValue();
        left[i] *= g;
        blockInputPeak = std::max(blockInputPeak, std::abs(left[i]));
        if (right) {
          right[i] *= g;
          blockInputPeak = std::max(blockInputPeak, std::abs(right[i]));
        }
      }

      // EQ in the PRE position: between the block's input gain and its model,
      // shaping what drives the amp/IR. Skipped entirely when flat/bypassed
      // (or in the default post position; see the POST stage below).
      if (block->eq.isPre() && block->eq.isActive()) {
        block->eq.process(buffer);
        // Re-measure so the input meter still reads what the model receives.
        blockInputPeak = bufferPeak(buffer, numChannels, numSamples);
      }

      const float blockInputDb =
          blockInputPeak > 0.0f ? juce::Decibels::gainToDecibels(blockInputPeak) : -60.0f;
      block->inputMeterDb.store(std::max(-60.0f, blockInputDb));
    }

    if (block->type == ChainBlockType::NAM) {
      // NAM Processing (the engine runs at the chain rate, no per-block resampling)
      try {
        jassert(numSamples <= dryScratch.getNumSamples());

        if (block->namEngine == nullptr) {
          DBG("Warning: NAM block " << block->id << " has no engine - skipping");
          continue;
        }

        // Calculate additional calibration gain for this specific NAM block
        float calibrationGain = 1.0f;
        if (cacheCalibrateInput && block->namEngine->hasInputLevel()) {
          const double modelInputLevel = block->namEngine->getInputLevel();
          const double calibrationAdjustmentDb = cacheInputCalibrationLevel - modelInputLevel;
          calibrationGain = juce::Decibels::decibelsToGain(static_cast<float>(calibrationAdjustmentDb));
        }

        // Apply calibration gain to the buffer
        if (calibrationGain != 1.0f) {
          buffer.applyGain(0, 0, numSamples, calibrationGain);
          if (numChannels > 1) {
            buffer.applyGain(1, 0, numSamples, calibrationGain);
          }
        }

        // Process with the NAM engine (handles mono conversion internally).
        // With multi-core on, the engine forks its oversampling phase
        // instances across the worker pool (rtPhasePool, resolved per
        // callback); nested inside a lane fork this is the pool's supported
        // one-deep nesting. Null = phases run serially on this thread.
        block->namEngine->process(buffer, rtPhasePool);

        // Post-model gain: calibrated hand-off OR loudness normalization,
        // never both; they have contradictory goals (reproduce the capture
        // rig's true level vs. make every capture equally loud).
        //
        // Calibrated hand-off applies only mid-chain (another NAM downstream)
        // when calibration is on and the model carries output_level_dbu.
        // Gain = model output dBu - user's calibration dBu converts the
        // model's output back into the user's analog reference frame; the
        // downstream NAM's input calibration then converts from that frame
        // into its own model's, so the user's setting cancels and the
        // hand-off carries exactly the level of physically plugging device A
        // into device B. Normalizing mid-chain instead would wreck the drive
        // level into the next model that calibration exists to preserve.
        //
        // The last NAM block deliberately stays on normalization: calibrated
        // output at the chain's end would swing overall volume with each
        // capture's metadata (a cranked-amp model can sit 20+ dB hot). Net
        // effect: calibration governs drive/character, normalization governs
        // listening level. No clamp on the hand-off gain (it's a physical
        // level difference, not a guess), only a metadata sanity check that
        // falls back to normalization when the value is junk.
        // The smoother was prepared off the RT path (prepareChain / model
        // apply); here we only ever move its target.
        const float targetLufs = cacheTargetLoudness;  // use live target
        float blockGain = 1.0f;
        bool calibratedHandOff = false;
        if (cacheCalibrateInput && idx < lastNamIndex && block->namEngine->hasOutputLevel()) {
          const float modelOutputLevel = static_cast<float>(block->namEngine->getOutputLevel());
          if (std::isfinite(modelOutputLevel) && modelOutputLevel >= -60.0f &&
              modelOutputLevel <= 60.0f) {
            blockGain =
                juce::Decibels::decibelsToGain(modelOutputLevel - cacheInputCalibrationLevel);
            calibratedHandOff = true;
          }
        }
        if (!calibratedHandOff && block->normalizeEnabled) {
          float modelLoudnessDb = targetLufs;  // Default fallback
          if (block->namEngine->hasLoudness()) {
            modelLoudnessDb = static_cast<float>(block->namEngine->getLoudness());
          }
          if (!std::isfinite(modelLoudnessDb) || modelLoudnessDb < -100.0f || modelLoudnessDb > 0.0f) {
            modelLoudnessDb = targetLufs;
          }
          const float gainAdjustmentDb = juce::jlimit(-12.0f, 12.0f, targetLufs - modelLoudnessDb);
          blockGain = juce::Decibels::decibelsToGain(gainAdjustmentDb);
        }
        block->namNormalizationSmoother.setTargetValue(blockGain);

        // Apply per-block normalization / hand-off gain to the buffer
        auto* left = buffer.getWritePointer(0);
        auto* right = numChannels > 1 ? buffer.getWritePointer(1) : nullptr;

        for (int i = 0; i < numSamples; ++i) {
          const float g = block->namNormalizationSmoother.getNextValue();
          left[i] *= g;
          if (right) right[i] *= g;
        }
      } catch (const std::exception&) {
        // RT-safe failure path: disable the block (stops it re-throwing every
        // block) and flag it; the message thread writes the log line when it
        // next serializes the chain; string building/logging can't run here.
        block->loaded = false;
        block->rtProcessingFailed.store(true);
        bumpChainRevision();  // wake the UI poll so the flag is drained
        buffer.copyFrom(0, 0, dryScratch, 0, 0, numSamples);
        if (numChannels > 1) {
          buffer.copyFrom(1, 0, dryScratch, 1, 0, numSamples);
        }
        continue;
      }
    } else if (block->type == ChainBlockType::IR && block->convolverMono != nullptr) {
      // IR Processing.
      try {
        // True-stereo only when: the IR file is stereo, the working buffer is stereo, and no
        // NAM block downstream would collapse the image back to mono. Otherwise apply the IR's
        // left channel to every audio channel (convolverMono, Stereo::no).
        const bool noNamAfter = (idx > lastNamIndex);
        const bool useStereoIr = block->irNumChannels > 1 && numChannels > 1 && noNamAfter &&
                                 block->convolverStereo != nullptr;
        auto& convolver = useStereoIr ? *block->convolverStereo : *block->convolverMono;

        // Convolution runs at the base rate inside the block's island: when
        // the chain is oversampled the island decimates the wet path, hands
        // the convolver base-rate frames, and interpolates back (a direct
        // pass at ×1). Linear processing gains nothing above the base rate;
        // this keeps IR CPU flat across oversampling factors and the IR
        // sound bit-identical to the non-oversampled chain.
        block->irBaseRateIsland.processBaseRateIsland(
            buffer.getArrayOfWritePointers(), numChannels, numSamples,
            [&convolver, &predelay = block->predelay,
             numChannels](float* const* baseChannels, int baseFrames) {
              // Predelay runs on the wet signal here, right before the
              // convolver, always at the base rate the island already
              // decimated to (see BlockPredelay for why it lives here
              // rather than padding the loaded IR itself).
              juce::AudioBuffer<float> baseBuffer(baseChannels, numChannels, baseFrames);
              predelay.process(baseBuffer);

              juce::dsp::AudioBlock<float> irBlock(baseChannels, static_cast<size_t>(numChannels),
                                                   static_cast<size_t>(baseFrames));
              convolver.process(juce::dsp::ProcessContextReplacing<float>(irBlock));
            });

        // Unit-energy normalization, always on: an IR file's absolute level
        // is an accident of capture/export (unlike a NAM capture's, which is
        // real information, hence NAM's normalize toggle). Attenuation-only;
        // smoother is prepared in prepareChain / model apply, only the
        // target moves on the RT path. irEffectiveNormalizationGainLinear
        // (not the base irNormalizationGainLinear) so Size's loudness
        // compensation (see ChainBlock.h's irSizeGainCompensation) rides
        // along here too - and the same 1.0 ceiling still applies to it,
        // so Size compensation can restore parity with Size=100%'s loudness
        // but never boost past this stage's own already-accepted maximum.
        block->irNormalizationSmoother.setTargetValue(
            juce::jlimit(0.0f, 1.0f, block->irEffectiveNormalizationGainLinear));
        for (int i = 0; i < numSamples; ++i) {
          const float g = block->irNormalizationSmoother.getNextValue();
          for (int ch = 0; ch < numChannels; ++ch) {
            buffer.getWritePointer(ch)[i] *= g;
          }
        }
      } catch (const std::exception& e) {
        DBG("Error in IR processing for block " << block->id << ": " << e.what());
      }
    } else if (block->type == ChainBlockType::CAB && block->convolverMono != nullptr) {
      // Cab Block processing (ChainBlockType::CAB, single-slot v1). Mono
      // kernel always applied to every channel present - convolverStereo is
      // never built for CAB, so there's no useStereoIr-style choice to make.
      // Deliberately NOT sharing the IR branch above despite the similar
      // shape: that branch's lambda runs block->predelay.process() on every
      // call, and CAB has no predelay field at all (never prepared for this
      // type - see prepareChain) - reusing it here would process a stale,
      // zero-capacity ring buffer, the same class of bug that crashed the
      // very first version of this block.
      try {
        block->irBaseRateIsland.processBaseRateIsland(
            buffer.getArrayOfWritePointers(), numChannels, numSamples,
            [&convolver = *block->convolverMono, numChannels](float* const* baseChannels,
                                                               int baseFrames) {
              juce::dsp::AudioBlock<float> irBlock(baseChannels, static_cast<size_t>(numChannels),
                                                   static_cast<size_t>(baseFrames));
              convolver.process(juce::dsp::ProcessContextReplacing<float>(irBlock));
            });

        // Unit-energy normalization, same rule as a plain IR block's. CAB
        // has no Size knob, so irEffectiveNormalizationGainLinear always
        // equals the base gain here - read it anyway for the same reason
        // the IR branch above does (one field the audio thread trusts).
        block->irNormalizationSmoother.setTargetValue(
            juce::jlimit(0.0f, 1.0f, block->irEffectiveNormalizationGainLinear));
        for (int i = 0; i < numSamples; ++i) {
          const float g = block->irNormalizationSmoother.getNextValue();
          for (int ch = 0; ch < numChannels; ++ch) {
            buffer.getWritePointer(ch)[i] *= g;
          }
        }
      } catch (const std::exception& e) {
        DBG("Error in Cab Block processing for block " << block->id << ": " << e.what());
      }
    }

    // EQ in the POST position (default): shapes the wet signal after the
    // model, before the dry/wet mix, so the dry share of Mix passes
    // untouched. Skipped entirely when flat/bypassed (the PRE position ran
    // before the model).
    if (!block->eq.isPre() && block->eq.isActive()) {
      block->eq.process(buffer);
    }

    // Per-block output stage: blend the wet signal with dry, then apply Out
    // Gain (centered at 0.5 == unity, ±24 dB) to the combined result. The
    // knob is the block's output fader, not a wet trim, so it has to move
    // the dry share of Mix too.
    //
    // No fixed cab pad: IR and Cab blocks both get always-on unit-energy
    // normalization (see the IR/CAB branches above), which already lands the
    // wet path at about the dry level. A -18 dB cab pad used to sit on top
    // of that - a leftover from before normalization was always on - and
    // double-compensated: Cab blocks ran ~18 dB under the same file loaded
    // as an IR Player.
    // 0.5 == unity, +24 dB at max, true silence at/near fully closed - see
    // gainKnobDb. Also drives Dual Mono's per-side Vol knobs, which write
    // this same outputGain param (see handleDualChildVolChange in
    // ChainBlock.tsx).
    block->outputGainSmoother.setTargetValue(
        juce::Decibels::decibelsToGain(gainKnobDb(block->outputGainNormalized)));
    block->mixSmoother.setTargetValue(forceFullWet ? 1.0f
                                                   : juce::jlimit(0.0f, 1.0f, block->mixNormalized));

    float blockOutputPeak = 0.0f;
    for (int i = 0; i < numSamples; ++i) {
      // wetFadeGain rides the mix (bypass-bound glides crossfade toward
      // dry) and glides the post-mix Out Gain to unity in step, so a
      // completed fade lands exactly on the skipped block's pass-through.
      // swapWetMuteGain rides the wet term only, pre-mix
      // (engine swaps dip the wet path to silence without exposing the dry
      // input); see ChainBlock.h.
      const float wetGain = block->swapWetMuteGain.getNextValue();
      const float outGain = block->outputGainSmoother.getNextValue();
      const float fade = block->wetFadeGain.getNextValue();
      const float m = block->mixSmoother.getNextValue() * fade;
      const float postGain = 1.0f + (outGain - 1.0f) * fade;
      float wetL = buffer.getWritePointer(0)[i] * wetGain;
      float dryL = dryScratch.getReadPointer(0)[i];
      buffer.getWritePointer(0)[i] = (dryL * (1.0f - m) + wetL * m) * postGain;
      blockOutputPeak = std::max(blockOutputPeak, std::abs(buffer.getWritePointer(0)[i]));
      if (numChannels > 1) {
        float wetR = buffer.getWritePointer(1)[i] * wetGain;
        float dryR = dryScratch.getReadPointer(1)[i];
        buffer.getWritePointer(1)[i] = (dryR * (1.0f - m) + wetR * m) * postGain;
        blockOutputPeak = std::max(blockOutputPeak, std::abs(buffer.getWritePointer(1)[i]));
      }
    }

    // Fade handshake: tell a waiting requester the wet path is fully
    // silent and its change (swap/removal) can splice in silently. Which
    // gain carried the fade depends on the swap's shape (see ChainBlock.h).
    {
      const auto& fadeGain = muteSwap ? block->swapWetMuteGain : block->wetFadeGain;
      if (block->swapFadePending.load() && !fadeGain.isSmoothing() &&
          fadeGain.getCurrentValue() <= 0.001f)
        block->swapFadeDone.store(true);
    }

    // Block output meter: post EQ + mix + Out Gain, i.e. what this block
    // hands to the next one in the chain.
    const float blockOutputDb =
        blockOutputPeak > 0.0f ? juce::Decibels::gainToDecibels(blockOutputPeak) : -60.0f;
    block->outputMeterDb.store(std::max(-60.0f, blockOutputDb));

    // Feed the EQ editor's analyzer with the block's final output, only while
    // that block's EQ view is actually open in the UI.
    if (block->spectrum.isEnabled())
      block->spectrum.pushSamples(buffer.getReadPointer(0),
                                  numChannels > 1 ? buffer.getReadPointer(1) : nullptr,
                                  numSamples);
  }
}

// ##############################
// RT CHAIN STAGE (chain rate)
// ##############################
// The oversampled entry to the chain stage: the callable both invocation
// paths share (the boundary callback and the direct 48k-host path). Raises
// the rate by the current factor around processChainStage; transparent
// passthrough when oversampling is off. Called with chainMutex held.
void TONE3000Processor::processOversampledChainStage(float** inputs, float** outputs,
                                                     int numFrames) {
  chainOversampler.process(inputs, outputs, numFrames,
                           [this](float** chainIns, float** chainOuts, int chainFrames) {
                             processChainStage(chainIns, chainOuts, chainFrames);
                           });
}

// The encapsulated side of the chain-domain boundary: the chain over the
// given channel pointers. Invoked at the chain rate (48 kHz × oversampling
// factor) via processOversampledChainStage. Called with chainMutex held
// (processBlock takes it).
void TONE3000Processor::processChainStage(float** inputs, float** outputs, int numFrames) {
  // The boundary hands us distinct input/output buffers; the chain processes
  // in place, so move the audio to the output side first. On the direct path
  // the pointers alias and the copies are skipped.
  for (int ch = 0; ch < 2; ++ch) {
    if (outputs[ch] != inputs[ch])
      std::memcpy(outputs[ch], inputs[ch], sizeof(float) * static_cast<size_t>(numFrames));
  }

  juce::AudioBuffer<float> chainBuffer(outputs, rtChainChannels, numFrames);
  processChainOnBuffer(chain, chainBuffer, laneDryScratch);
}

// ##############################
// POST-CHAIN OUTPUT PAN (host rate)
// ##############################
// Runs right after each chain-stage slice, before the downstream stages
// (DC / tone stack / output gain + meters) so they see the real image.
// Pan (see imageMatrixGains) tilts whatever the two channels already carry
// at this point — duplicated mono, genuine stereo input, or a Dual Mono
// block's own widening earlier in the chain all land here as two real
// channels to position. Only meaningful on a rig that can reproduce two
// channels (`stereoRig`), matching the UI hiding the knob otherwise. The
// centered default is the identity and skips the loop.
void TONE3000Processor::processImageStage(float* chL, float* chR, int numFrames,
                                          bool stereoRig) {
  // The gain has work only on a rig that can actually reproduce two
  // channels. Both gains are smoothed so knob moves glide instead of
  // stepping (pop).
  if (stereoRig) {
    const auto g = imageMatrixGains(cacheOutputPan);
    imageGainL.setTargetValue(g.l);
    imageGainR.setTargetValue(g.r);

    const bool smoothing = imageGainL.isSmoothing() || imageGainR.isSmoothing();
    const bool identity =
        std::abs(g.l - 1.0f) < 1.0e-4f && std::abs(g.r - 1.0f) < 1.0e-4f;
    if (smoothing || !identity) {
      for (int i = 0; i < numFrames; ++i) {
        chL[i] *= imageGainL.getNextValue();
        chR[i] *= imageGainR.getNextValue();
      }
    }
  }
}

// ################
// RT PROCESS BLOCK
// ################
void TONE3000Processor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi) {
  juce::ScopedNoDenormals noDenormals;
  // Times this whole callback against its real-time budget (the CPU readout).
  juce::AudioProcessLoadMeasurer::ScopedTimer loadTimer(loadMeasurer, buffer.getNumSamples());

  // Mapped MIDI first, so parameter moves (bypass stomps, expression sweeps)
  // land before this block's cached-parameter refresh below.
  midiMapper.processMidi(midi);

  const int numSamples = buffer.getNumSamples();
  const int numChannels = buffer.getNumChannels();

  if (numSamples <= 0 || numChannels <= 0) {
    DBG("Invalid buffer: samples=" << numSamples << ", channels=" << numChannels);
    buffer.clear();
    outputMeterLevelL.store(-60.0f);
    outputMeterLevelR.store(-60.0f);
    return;
  }

  updateCachedParameters();

  // Global Bypass dry path: the host's input as delivered (a standalone mono
  // input device mirrored onto both channels, so bypass isn't left-only),
  // delayed by the reported latency. Captured every block, bypassed or not,
  // so the delay ring is warm the moment bypass engages.
  const int dryChannels = juce::jmin(numChannels, 2);
  if (bypassDry.getNumSamples() < numSamples)
    bypassDry.setSize(2, numSamples, false, false, true);  // host overshoot only
  for (int ch = 0; ch < dryChannels; ++ch)
    bypassDry.copyFrom(ch, 0, buffer, ch, 0, numSamples);
  if (dryChannels > 1 && standaloneMonoInput.load())
    bypassDry.copyFrom(1, 0, bypassDry, 0, 0, numSamples);
  if (const int delayLen = static_cast<int>(bypassDelayRing[0].size()); delayLen > 0) {
    int pos = bypassDelayPos;
    for (int ch = 0; ch < dryChannels; ++ch) {
      auto* ring = bypassDelayRing[static_cast<size_t>(ch)].data();
      auto* d = bypassDry.getWritePointer(ch);
      pos = bypassDelayPos;
      for (int i = 0; i < numSamples; ++i) {
        const float delayed = ring[pos];
        ring[pos] = d[i];
        d[i] = delayed;
        if (++pos == delayLen)
          pos = 0;
      }
    }
    bypassDelayPos = pos;
  }

  // Heartbeat for isAudioActive(): fade handshakes skip their bounded waits
  // when no callbacks are running (nothing is audible then).
  lastAudioCallbackMs.store(juce::Time::currentTimeMillis());

  // Input fold-down, up front so everything downstream (meters, tuner,
  // chains) sees the effective source:
  // - Mono input device (standalone): signal only arrives on channel 0,
  //   so mirror it.
  // - Input mode L/R on a stereo source: duplicate the chosen channel onto
  //   both, exactly like a host feeding a mono source to a stereo bus.
  if (numChannels > 1) {
    if (standaloneMonoInput.load()) {
      buffer.copyFrom(1, 0, buffer, 0, 0, numSamples);
    } else {
      switch (static_cast<InputMode>(inputMode.load())) {
        case InputMode::Left: buffer.copyFrom(1, 0, buffer, 0, 0, numSamples); break;
        case InputMode::Right: buffer.copyFrom(0, 0, buffer, 1, 0, numSamples); break;
        case InputMode::Stereo: break;
      }
    }
  }

  // #########################
  // Input gain + noise gate
  // #########################

  // Input gain (level ±24 dB), constant across the block. Computed up front
  // so the meters can show the post-gain level without a second pass over
  // the samples.
  const float inputGain = mainStageGain(cacheInputLevel);

  // Per-channel input meters: raw peaks scaled by the input gain, so the
  // meter tracks the knob. Pre-gate on purpose: a closed gate would
  // otherwise read as a dead input. Mono sources report the same level on
  // both channels.
  auto peakToDb = [](float peak) {
    return peak > 0.0f ? std::max(-60.0f, juce::Decibels::gainToDecibels(peak)) : -60.0f;
  };
  {
    float peakL = 0.0f, peakR = 0.0f;
    const auto* l = buffer.getReadPointer(0);
    for (int i = 0; i < numSamples; ++i)
      peakL = std::max(peakL, std::abs(l[i]));
    if (numChannels > 1) {
      const auto* r = buffer.getReadPointer(1);
      for (int i = 0; i < numSamples; ++i)
        peakR = std::max(peakR, std::abs(r[i]));
    } else {
      peakR = peakL;
    }
    inputMeterLevelL.store(peakToDb(peakL * inputGain));
    inputMeterLevelR.store(peakToDb(peakR * inputGain));
  }

  // Feed the tuner from the raw input (pre-gain, pre-gate) while the tuner
  // screen is open. Channel 0 only: guitar sources are mono, and mixing
  // channels risks phase cancellation.
  if (tuner.isEnabled())
    tuner.pushSamples(buffer.getReadPointer(0), numSamples);

  // Apply the input gain (vectorized).
  for (int ch = 0; ch < numChannels; ++ch)
    buffer.applyGain(ch, 0, numSamples, inputGain);

  // Noise gate, post input gain so the threshold knob's dB meaning matches
  // the level heading into the chain. Envelope/hysteresis gate (NoiseGate.h);
  // re-enabling resets the detector so a stale envelope never gates the
  // first block.
  if (cacheGateEnabled) {
    if (!gateWasEnabled)
      inputGate.reset();
    inputGate.setThresholdDb(cacheGateThreshold);
    inputGate.process(buffer);
  }
  gateWasEnabled = cacheGateEnabled;

  // ####################
  // MODULAR CHAIN PROCESSING (chain domain: 48 kHz × OS factor, see ChainDomain.h)
  // ####################
  // Runs under chainMutex, but the render thread must never *block* behind a
  // long splice. Preset/undo restores hold chainMutex on the message thread
  // while they decode megabytes of embedded model bytes; a blocking lock here
  // stalls the CoreAudio render thread for 100+ ms, which overloads the
  // driver and can restart the device (observed in the field: repeated
  // prepareToPlay/releaseResources cycles and a fallback to the OS default
  // device right after heavy preset loads). So: try the lock first. If it's
  // contended while the chain-edit fade has fully landed (pending && done ⇒
  // the tap below outputs silence no matter what the chain produces), skip
  // the stage wait-free: inaudible, and the splice can take as long as it
  // needs. Contention outside a landed fade is ordinary and brief (UI state
  // pulls, engine installs), so fall back to the blocking lock as before.

  // The rig can reproduce a stereo image: two buffer channels AND a detected
  // stereo output (a standalone mono output device still hands us a stereo
  // buffer but plays only channel 0). Gates the output Pan (see
  // processImageStage).
  const bool stereoRig =
      numChannels >= 2 && stereoOutputDetected.load(std::memory_order_relaxed);

  const auto runChainStage = [&] {
    rtChainChannels = juce::jmin(numChannels, 2);

    // One multi-core resolution per callback (under chainMutex): the phase
    // fork only needs the setting and live workers (see ChainOversampler's
    // own NAM phase-instance fork - the only remaining fork now that the
    // chain is a single lane).
    const bool multiCore =
        multiCoreEnabled.load(std::memory_order_relaxed) && rtWorkerPool.isRunning();
    rtPhasePool = multiCore ? &rtWorkerPool : nullptr;

    // Hosts occasionally exceed the block size they promised in prepareToPlay.
    // Feed the chain stage in prepared-size slices so the boundary's internal
    // buffers (and the chain-domain scratch) can never overflow; a single
    // pass in the normal case.
    const int maxSlice = juce::jmax(1, maxBlockSize);
    for (int offset = 0; offset < numSamples; offset += maxSlice) {
      const int sliceLen = juce::jmin(maxSlice, numSamples - offset);

      // The boundary is a fixed 2-channel container, so a mono host buffer
      // gets the scratch as its (silent, unused) second channel.
      float* channels[2] = {buffer.getWritePointer(0) + offset,
                            numChannels > 1 ? buffer.getWritePointer(1) + offset
                                            : chainScratchChannel.getWritePointer(0)};

      if (chainBoundary != nullptr)
        chainBoundary->ProcessBlock(channels, channels, sliceLen, chainStageFunc);
      else
        processOversampledChainStage(channels, channels, sliceLen);

      processImageStage(channels[0], channels[1], sliceLen, stereoRig);
    }
  };

  {
    juce::ScopedTryLock tryLock(chainMutex);
    if (tryLock.isLocked()) {
      runChainStage();
    } else if (chainEditFadePending.load() && chainEditFadeDone.load()) {
      // A splice owns the lock and the output is held at silence: hand the
      // downstream stages a cleared buffer instead of raw input (the tap
      // would zero it anyway, but the DC blocker sits before the tap).
      buffer.clear();
    } else {
      juce::ScopedLock lock(chainMutex);
      runChainStage();
    }
  }

  // ##########
  // DC blocker
  // ##########
  {
    // ProcessorDuplicator runs an independent filter instance per channel, so a
    // single duplicator covers the whole (mono or stereo) buffer. Running a
    // second one here would high-pass every channel twice.
    juce::dsp::AudioBlock<float> block(buffer);
    juce::dsp::ProcessContextReplacing<float> context(block);
    dcBlocker.process(context);
  }

  // ##########
  // Chain-edit fade (see ChainEditFade): structural edits that can't be
  // expressed as one block's wet fade (reorder, cross-lane move, preset /
  // undo restores) glide the whole chain output to silence, splice the edit
  // in between callbacks, and glide back. Idle cost: one atomic load + one
  // branch.
  //
  // The tap deliberately sits AFTER the image stage and DC blocker:
  //  - Image stage: its hard stops on mode switches (forceIdle) land
  //    upstream of this gain, so they still splice into silence.
  //  - DC blocker: NAM models can idle at a DC offset, and a restored rig's
  //    blocks fade in while the chain is held muted. With the tap upstream
  //    of the blocker, the blocker would settle to zero state during the
  //    hold and the new rig's DC would step through it at release, an
  //    audible thump on every preset/undo switch. Downstream of the
  //    blocker, the blocker tracks the live chain (including its DC)
  //    throughout the hold, so release ramps in an already-centered signal.
  // ##########
  {
    const bool editPending = chainEditFadePending.load();
    chainEditFadeGain.setTargetValue(editPending ? 0.0f : 1.0f);
    if (chainEditFadeGain.isSmoothing()) {
      for (int i = 0; i < numSamples; ++i) {
        const float g = chainEditFadeGain.getNextValue();
        for (int ch = 0; ch < numChannels; ++ch)
          buffer.getWritePointer(ch)[i] *= g;
      }
    } else if (editPending && chainEditFadeGain.getCurrentValue() <= 0.001f) {
      // Fully faded: hold silence until the editor thread finishes its splice.
      buffer.clear();
      chainEditFadeDone.store(true);
    }
  }

  // ##########
  // EQ section (global 3-band tone stack), post-chain.
  // ##########
  processToneStack(buffer);

  // ##########
  // Auto-align probe mute: fades the output before the probe starts, holds
  // silence through capture and analysis, ramps back after the result is
  // applied (see AutoOffset.h). Sits before the output stage so the meters
  // show the mute honestly. Idle cost: one atomic load.
  // ##########
  autoOffset.applyOutputGain(buffer);

  // ###########
  // Output gain (level ±24 dB, same on both channels; the balance trim
  // lives in the post-chain image matrix above, pre-pan). Smoothed so knob
  // moves glide instead of stepping once per block. Then the global Bypass
  // crossfade (to the latency-matched dry input, at unity - the Output knob
  // is part of what's bypassed) and Mute, last of all. Per-channel output
  // meters ride the same pass, so they show exactly what leaves the plugin.
  // ###########
  {
    outputGainSmoother.setTargetValue(mainStageGain(cacheOutputLevel));
    bypassMix.setTargetValue(cacheBypass ? 1.0f : 0.0f);
    sceneGainSmoother.setTargetValue(juce::Decibels::decibelsToGain(sceneLevelDb.load()));
    muteGain.setTargetValue(
        cacheOutputMute || tunerMuteActive.load(std::memory_order_relaxed) ? 0.0f : 1.0f);

    float peakL = 0.0f, peakR = 0.0f;
    auto* l = buffer.getWritePointer(0);
    auto* r = numChannels > 1 ? buffer.getWritePointer(1) : nullptr;
    const auto* dryL = bypassDry.getReadPointer(0);
    const auto* dryR = bypassDry.getReadPointer(dryChannels > 1 ? 1 : 0);
    for (int i = 0; i < numSamples; ++i) {
      // Scene level rides with the Output knob (both bypassed by Bypass).
      const float g = outputGainSmoother.getNextValue() * sceneGainSmoother.getNextValue();
      const float b = bypassMix.getNextValue();
      const float m = muteGain.getNextValue();
      l[i] = (l[i] * g * (1.0f - b) + dryL[i] * b) * m;
      peakL = std::max(peakL, std::abs(l[i]));
      if (r) {
        r[i] = (r[i] * g * (1.0f - b) + dryR[i] * b) * m;
        peakR = std::max(peakR, std::abs(r[i]));
      }
    }
    if (numChannels < 2) 
      peakR = peakL;
    outputMeterLevelL.store(peakToDb(peakL));
    outputMeterLevelR.store(peakToDb(peakR));
  }
}

// ##################
// ENABLE EDITOR / UI
// ##################
bool TONE3000Processor::hasEditor() const {
  return true;
}

// ##############
// CREATE EDITOR
// ##############
juce::AudioProcessorEditor* TONE3000Processor::createEditor() {
#if !HEADLESS
  return new TONE3000Editor(*this);
#else
  return nullptr;
#endif
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() {
  return new TONE3000Processor();
}

// #########################
// AUTO ALIGN (probe-based time alignment for a Dual Mono block's own sides)
// #########################
// One [=] press runs a deterministic internal sweep through the block's two
// sides with the output muted for under half a second; the whole measurement
// (probe schedule, capture, GCC-PHAT estimation) lives in AutoOffset
// (AutoOffset.h). The processor's share is the probe injection / capture
// tap / mute stage in runDualMono, and applying the result to the block's
// own fields here on the message thread.

namespace {
// Rejection gate: peak sharpness is the PHAT-native quality metric and sits
// far above this on every healthy run (5+ measured across real NAM/IR rig
// pairs; junk in-window peaks land near 1), so a failure means the capture
// was disturbed (a chain edit spliced mid-probe, a silently broken chain)
// or the true misalignment is beyond the ±24 ms the knob can express.
// Reject and log instead of setting a junk offset. The raw-waveform
// confidence is deliberately NOT gated: two differently voiced rigs
// legitimately read low there even when perfectly aligned (0.14 measured);
// it rides along in the log and poll payload as a diagnostic.
constexpr float kAutoOffsetMinSharpness = 2.0f;
// Lags under this are already aligned for any practical purpose (well under
// a sample's worth of imaging); don't power Align on over nothing.
constexpr float kAutoOffsetSilentMs = 0.05f;
// Polarity gate: field-tested against real Dual Mono rigs, every wrong
// polarity call (confirmed wrong by ear AND by the live correlation meter,
// even after fixing the underlying peak-selection to prefer raw time-
// domain agreement over the PHAT-whitened metric - see AutoOffset.cpp)
// measured confidence at or under 0.51; nothing above that has been
// observed to misfire. Below this, Ø is left exactly as the user set it -
// Align still applies the timing correction (that part has held up), it
// just stops guessing at polarity when the raw signal agreement is too
// weak to trust. A missed flip costs the user one manual Ø click with the
// correlation meter open; a wrong one costs trust in the whole feature.
constexpr float kAutoOffsetMinPolarityConfidence = 0.6f;
}  // namespace

void TONE3000Processor::cancelAutoOffset() { autoOffset.cancel(); }

// Resets a Dual Mono side's own stateful DSP (NAM's recurrent/dilated-conv
// path, IR's convolution history) to a deterministic post-load baseline
// before the probe measures it. Without this, a side that was just
// processing real audio carries whatever transient state that left behind
// into the sweep, and if the two sides' recent history differed at all
// (genuinely different stereo content, or just different silence-floor
// noise) their responses to the *identical* sweep diverge for its opening
// stretch - read by the cross-correlation as a spurious peak, sometimes
// strong enough to beat the true near-zero-lag one, with an effectively
// coin-flip polarity sign (see AutoOffset.h). Message thread only -
// NamEngine::resetState()/prewarm() is not real-time safe.
namespace {
void flushDualSideState(ChainBlock* side) {
  if (side == nullptr)
    return;
  if (side->namEngine != nullptr)
    side->namEngine->resetState();
  if (side->convolverMono != nullptr)
    side->convolverMono->reset();
  if (side->convolverStereo != nullptr)
    side->convolverStereo->reset();
  // Snap to 100% wet now rather than letting processChainOnBuffer's
  // forceFullWet glide there over mixSmoother's normal ~50ms ramp: the
  // sweep starts within milliseconds of this (FadeOut is only 5ms), and a
  // dry-contaminated ramp-in would still color the capture's opening
  // stretch. Stepping is inaudible here - the output is muted the whole
  // time (see AutoOffset::applyOutputGain).
  side->mixSmoother.setCurrentAndTargetValue(1.0f);
}
}  // namespace

// Shared by armDualAutoAlign and armDualAutoBalance - both just need "is
// this a real DUAL_MONO block, is the engine free" before claiming it and
// starting the identical sweep; only what the poll applies afterward
// differs between the two.
bool TONE3000Processor::armAutoOffsetFor(const std::string& blockId) {
  // Offline renders must never print the probe's silence into the bounce.
  if (isNonRealtime())
    return false;
  juce::ScopedLock lock(chainMutex);
  ChainBlock* block = findBlockById(blockId);
  if (block == nullptr || block->type != ChainBlockType::DUAL_MONO) {
    DBG("armAutoOffsetFor: not a DUAL_MONO block: " << blockId);
    return false;
  }
  // Only claim the engine if it's actually free - arm() itself also no-ops
  // while busy, but this keeps autoOffsetTargetBlockId from being stolen
  // out from under whichever other block's measurement is already running.
  if (autoOffset.state() != AutoOffset::State::Idle)
    return false;

  // Both sides start the probe from the same known state - see the header
  // comment above and AutoOffset.h's integration contract, step 0.
  flushDualSideState(block->dualLeft.empty() ? nullptr : block->dualLeft.front().get());
  flushDualSideState(block->dualRight.empty() ? nullptr : block->dualRight.front().get());
  autoOffsetTargetBlockId = blockId;
  autoOffset.arm();
  return true;
}

bool TONE3000Processor::armDualAutoAlign(const std::string& blockId) {
  return armAutoOffsetFor(blockId);
}

bool TONE3000Processor::armDualAutoBalance(const std::string& blockId) {
  return armAutoOffsetFor(blockId);
}

// Message thread (UI poll). The analysis (a one-shot FFT over the capture)
// also runs here, never on the audio thread; the output stays muted until
// resume(), applying the result to the target block's own fields
// (ChainBlock.h) before the unmute.
juce::var TONE3000Processor::pollDualAutoAlign(const std::string& blockId) {
  juce::DynamicObject::Ptr obj = new juce::DynamicObject();

  // Not the block this poller is asking about (a stale poll after another
  // measurement claimed the engine, or this one was never armed) - nothing
  // to report.
  if (blockId != autoOffsetTargetBlockId) {
    obj->setProperty("state", "idle");
    return juce::var(obj.get());
  }

  switch (autoOffset.state()) {
    case AutoOffset::State::FadeOut:
    case AutoOffset::State::Probing:
    case AutoOffset::State::Tail:
    case AutoOffset::State::Analyzing:
      obj->setProperty("state", "listening");
      obj->setProperty("progress", static_cast<double>(autoOffset.progress()));
      break;
    case AutoOffset::State::Captured: {
      juce::ScopedLock lock(chainMutex);
      ChainBlock* block = findBlockById(blockId);
      if (block == nullptr) {
        // The target vanished mid-measurement (removed, undo) - drop the
        // result and let the output ramp back rather than leaving the
        // whole plugin stuck muted forever waiting for a poll that can
        // never apply anywhere.
        autoOffset.analyze();
        autoOffset.resume();
        obj->setProperty("state", "timeout");
        break;
      }

      const auto result = autoOffset.analyze();
      if (result.peakSharpness < kAutoOffsetMinSharpness) {
        autoOffset.resume();
        obj->setProperty("state", "timeout");
        obj->setProperty("confidence", result.confidence);
        obj->setProperty("peakSharpness", result.peakSharpness);
        juce::Logger::writeToLog("[AutoOffset] Rejected (block " + juce::String(blockId) +
                                 "): confidence " + juce::String(result.confidence, 3) +
                                 ", sharpness " + juce::String(result.peakSharpness, 2) +
                                 ", peaks [" + result.debugPeaks + "]");
        break;
      }

      pushChainHistory("param:" + juce::String(blockId) + ":dualAlign");

      // ms -> knob position, the StereoOffsetParams::fromNormalized inverse.
      const float norm = juce::jlimit(
          0.0f, 1.0f, 0.5f + result.offsetMs / (2.0f * StereoOffsetParams::kMaxOffsetMs));
      block->dualAlignOffsetNormalized = norm;
      // Power Align on when there's a real correction to hear - an
      // effectively-zero result still rewrites the offset (clearing a
      // stale knob value) but leaves the power switch alone.
      if (std::abs(result.offsetMs) >= kAutoOffsetSilentMs)
        block->dualAlignEnabled = true;

      // Polarity: capture happens pre-Align/pre-Ø (see runDualMono's own
      // capture site), so result.inverted is the two sides' absolute
      // relative polarity and the Ø flags must end up XOR-matching it.
      // Toggling only the right side preserves an absolute both-sides flip
      // the user may already have set. Gated on confidence, unlike the
      // timing correction above - see kAutoOffsetMinPolarityConfidence.
      bool polarityFlipped = false;
      const bool invertedNow = block->dualLeftInvert != block->dualRightInvert;
      if (result.confidence >= kAutoOffsetMinPolarityConfidence && result.inverted != invertedNow) {
        block->dualRightInvert = !block->dualRightInvert;
        polarityFlipped = true;
      }

      deferredRevisionBump();
      autoOffset.resume();
      obj->setProperty("state", "done");
      obj->setProperty("matchedMs", result.offsetMs);
      obj->setProperty("polarityFlipped", polarityFlipped);
      const juce::String polarityNote =
          polarityFlipped
              ? ", Ø toggled"
              : (result.inverted != invertedNow
                     ? ", Ø left alone (confidence below " +
                           juce::String(kAutoOffsetMinPolarityConfidence, 2) + ")"
                     : "");
      juce::Logger::writeToLog(
          "[AutoOffset] Aligned Dual Mono block " + juce::String(blockId) + " (offset " +
          juce::String(result.offsetMs, 3) + " ms, " + (result.inverted ? "inverted" : "normal") +
          ", confidence " + juce::String(result.confidence, 3) + ", sharpness " +
          juce::String(result.peakSharpness, 1) + polarityNote +
          ", peaks [" + result.debugPeaks + "])");
      break;
    }
    case AutoOffset::State::RampBack:
    case AutoOffset::State::Idle:
      obj->setProperty("state", "idle");
      break;
  }
  return juce::var(obj.get());
}

// Loudness reads below this gap are already even for any practical
// purpose; skip the param write/history entry rather than nudging a knob
// by a fraction of a dB.
constexpr float kAutoBalanceSilentDb = 0.1f;

juce::var TONE3000Processor::pollDualAutoBalance(const std::string& blockId) {
  juce::DynamicObject::Ptr obj = new juce::DynamicObject();

  if (blockId != autoOffsetTargetBlockId) {
    obj->setProperty("state", "idle");
    return juce::var(obj.get());
  }

  switch (autoOffset.state()) {
    case AutoOffset::State::FadeOut:
    case AutoOffset::State::Probing:
    case AutoOffset::State::Tail:
    case AutoOffset::State::Analyzing:
      obj->setProperty("state", "listening");
      obj->setProperty("progress", static_cast<double>(autoOffset.progress()));
      break;
    case AutoOffset::State::Captured: {
      juce::ScopedLock lock(chainMutex);
      ChainBlock* block = findBlockById(blockId);
      if (block == nullptr) {
        // Same "target vanished mid-measurement" fallback pollDualAutoAlign
        // takes - drop the result rather than leaving the output muted
        // forever waiting for a poll that can never apply anywhere.
        autoOffset.analyze();
        autoOffset.resume();
        obj->setProperty("state", "timeout");
        break;
      }

      const auto result = autoOffset.analyze();
      if (result.silent) {
        autoOffset.resume();
        obj->setProperty("state", "timeout");
        juce::Logger::writeToLog("[AutoBalance] Rejected (block " + juce::String(blockId) +
                                 "): one side read silent");
        break;
      }

      // The quieter side's own child - Balance never touches the Dual
      // block's own fields, only whichever side needs raising (see
      // Result::boostRight).
      ChainBlock* target = result.boostRight
                               ? (block->dualRight.empty() ? nullptr : block->dualRight.front().get())
                               : (block->dualLeft.empty() ? nullptr : block->dualLeft.front().get());
      if (target == nullptr) {
        // The side vanished mid-measurement (removed while probing) - same
        // "nothing left to apply to" fallback as the null-block case above.
        autoOffset.resume();
        obj->setProperty("state", "timeout");
        break;
      }

      if (result.gainDeltaDb >= kAutoBalanceSilentDb) {
        pushChainHistory("param:" + juce::String(target->id) + ":outputGain");
        // currentDb can't actually be -inf here: a side sitting at true
        // silence (Vol knob fully closed) would have measured silent
        // itself and been rejected above, before reaching this branch. If
        // it somehow were, targetDb stays -inf and gainKnobNormFromDb
        // floors to fully closed - a safe no-op, not a wrong value.
        const float targetDb = gainKnobDb(target->outputGainNormalized) + result.gainDeltaDb;
        target->outputGainNormalized = juce::jlimit(0.0f, 1.0f, gainKnobNormFromDb(targetDb));
        deferredRevisionBump();
      }

      autoOffset.resume();
      obj->setProperty("state", "done");
      obj->setProperty("gainDeltaDb", result.gainDeltaDb);
      obj->setProperty("boostedSide", result.boostRight ? "R" : "L");
      juce::Logger::writeToLog(
          "[AutoBalance] Balanced Dual Mono block " + juce::String(blockId) + " (" +
          juce::String(result.boostRight ? "R" : "L") + " +" +
          juce::String(result.gainDeltaDb, 2) + " dB)");
      break;
    }
    case AutoOffset::State::RampBack:
    case AutoOffset::State::Idle:
      obj->setProperty("state", "idle");
      break;
  }
  return juce::var(obj.get());
}

// #########################
// DIAGNOSTIC LOG FILE
// #########################
// Mirrors juce::FileLogger::createDefaultAppLogger's path so the logger and the
// UI's copy/reveal actions always target the same file.
juce::File TONE3000Processor::getLogFile() {
  return juce::FileLogger::getSystemLogFileFolder()
      .getChildFile("TONE3000")
      .getChildFile("TONE3000.log");
}
