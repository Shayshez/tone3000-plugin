#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_dsp/juce_dsp.h>
#include <array>
#include <atomic>
#include <cmath>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>
#include "BlockEq.h"
#include "BlockPredelay.h"
#include "BlockSpectrum.h"
#include "ChainOversampler.h"
#include "BlockGoniometer.h"
#include "NamEngine.h"
#include "StereoOffset.h"

// IR Size: vari-speed duration/pitch ratio from the normalized 0..1 knob
// value (see ChainBlock::sizeNormalized). Exponential/log taper, not linear,
// so halving/doubling playback speed takes equal knob travel in either
// direction: 10% (10x faster, pitch up) at normalized=0, 100% (unchanged) at
// 0.5, 1000% (10x slower, pitch down) at 1.0. This is a duration multiplier,
// not a resample of the buffer itself - see prepareIrShapeRebuild's use of
// it: the trimmed/enveloped buffer's sample *count* never changes, only the
// sample rate declared to the convolver's loadImpulseResponse, which is what
// actually produces the vari-speed effect (same mechanism as playing a file
// back at the wrong sample rate). Shared by the rebuild (which needs the
// effective declared rate) and the chain-state serializer (which needs the
// resulting displayed length/D Len) so the two can never drift apart.
inline float irSizeDurationRatio(float sizeNormalized) {
  return std::pow(10.0f, 2.0f * (juce::jlimit(0.0f, 1.0f, sizeNormalized) - 0.5f));
}

// Upper bound on Size's *effective* (post-stretch) duration, regardless of
// source length or how far the knob is turned. Measured, not guessed: with
// a concurrent thread hammering chainMutex the way real UI activity does
// (getChainState pulls, getIrWaveform panel reads, setBlockParam drags -
// all things that briefly hold the lock processBlock's ordinary-contention
// path falls back to blocking on, see Processor.cpp's own comment on that),
// per-callback worst-case cost at a 512-sample/48kHz callback (10.667ms
// budget) climbed from ~1.6ms at 6s effective tail to a confirmed
// over-budget callback (10.88ms) at 60s, repeatably. 30s was tried first on
// the strength of that callback-timing margin (~4.7ms worst case, well
// under half the budget) but still produced audible crackles on long
// source files in real-world use - the callback-timing measurement alone
// wasn't the full risk picture. Tightened to 20s to leave real margin.
// Only ever clamps the slow-down direction (a shorter effective duration is
// never a real-time risk); irSizeClampedDurationRatio below is what
// prepareIrShapeRebuild and the chain-state serializer actually use, so the
// applied engine and the displayed length can never disagree.
constexpr double kIrSizeMaxEffectiveSeconds = 20.0;

inline float irSizeClampedDurationRatio(float sizeNormalized, double contentDurationSeconds) {
  const float rawRatio = irSizeDurationRatio(sizeNormalized);
  if (contentDurationSeconds <= 0.0)
    return rawRatio;
  const double maxRatio = kIrSizeMaxEffectiveSeconds / contentDurationSeconds;
  return static_cast<float>(juce::jmin(static_cast<double>(rawRatio), maxRatio));
}

// Compensates the volume drop Size introduces when stretching a kernel out
// (durationRatio > 1, lower pitch) - and the corresponding volume gain when
// compressing it (durationRatio < 1, higher pitch). Root cause: with
// Convolution::Normalise::no, prepareIrShapeRebuild's declared-sample-rate
// trick (irSizeDurationRatio above) makes JUCE apply its own
// magnitude-preserving gain of declaredRate/baseRate on the resampled
// kernel (see juce_Convolution.cpp's ConvolutionEngineFactory::makeEngine),
// while the interpolation-resampling step ahead of it changes the kernel's
// *energy* by the inverse factor - net effect, kernel energy scales by
// exactly one factor of declaredRate/baseRate = 1/durationRatio (matching
// computeIrNormalizationGain's own L2-energy loudness model, which this
// extends rather than replaces). Output level for a broadband input tracks
// the kernel's L2 norm (sqrt of energy) under that same model, so the
// resulting loudness *shift* from durationRatio alone is 1/sqrt(durationRatio)
// - this returns its exact inverse. Multiplying it onto the block's existing
// (content-only, Size-independent) irNormalizationGainLinear restores parity
// with Size=100%'s already-vetted loudness for any Size setting; it can
// never exceed that already-accepted baseline level, so it introduces no new
// clipping risk beyond what Size=100% already carries.
inline float irSizeGainCompensation(float durationRatio) {
  return durationRatio > 0.0f ? std::sqrt(durationRatio) : 1.0f;
}

// Chain block types. CAB is a real cabinet IR block (site tones tagged
// gear == "cab"; see parseToneForLoading/toneEngineType in ProcessorChain.cpp):
// structurally minimal by design - no predelay/envelope/waveform fields, a
// single convolver, unconditionally the #89 fix's Cab truncation/mix
// (see prepareBlockModelOffThread/applyPreparedModelToChainBlock). IR keeps carrying IrCategory::Cab for content
// that was loaded before this split existed / local file drops guessed as
// cab-length; that's a separate, still-live classification, not this type.
//
// EQ is the plainest block of all: no model, no IR, no tone content to
// download or switch - just this block's own `eq` member (every block
// already carries one, for the PRE/POST EQ around its model) shaping
// whatever passes through, at a fixed mixNormalized=1.0 (its class default,
// never touched - there's no dry content to blend against, so no Mix
// control exists in the UI). Added via addEqBlock (ProcessorChain.cpp),
// never through the tone-loading pipeline every other type goes through:
// it's synthesized with `loaded = true` from the moment it's created, and
// queueActiveModelLoad (ProcessorHistory.cpp) short-circuits to the same
// for the paths that reconstruct it (duplicate/paste/restore/undo). Because
// it matches none of the NAM/IR/CAB branches in Processor.cpp's per-block
// dispatch, the *existing*, type-agnostic PRE/POST eq.isPre()/isActive()
// checks that already run for every block are the entire DSP story here -
// no new processing branch needed.
//
// DUAL_MONO is a fixed, non-extensible pair: exactly two optional child
// slots (dualLeft/dualRight below), never an open-ended sub-chain - this is
// a deliberate redesign of an earlier "Split/Merge zone" concept (an
// extensible dual-mono sub-lane between two marker blocks) that turned out
// to be the wrong shape for the UI: nearly every visual bug that design hit
// traced back to fitting an extensible mini-chain inside one gallery tile.
// A fixed pair sidesteps that class of problem entirely - the gallery tile
// stays one ordinary TILE_SIZE square; "two things live here" is confined
// to this block's own detail view. Added via addDualMonoBlock, synthesized
// with `loaded = true` like EQ (the block itself has no tone of its own -
// its children do); queueActiveModelLoad recurses into whichever children
// are present instead of early-returning outright. Unlike EQ, DOES get a
// real new DSP dispatch branch (Processor.cpp): it seeds two mono scratch
// buffers, runs each present child through the ordinary single-block
// processing path, and recombines with Pan L/Pan R/Width (see
// dualLeftPanNormalized et al. below).
enum class ChainBlockType { NAM, IR, INSERT, CAB, EQ, DUAL_MONO };

inline juce::String chainBlockTypeToString(ChainBlockType type) {
  switch (type) {
    case ChainBlockType::NAM: return "nam";
    case ChainBlockType::INSERT: return "insert";
    case ChainBlockType::CAB: return "cab";
    case ChainBlockType::EQ: return "eq";
    case ChainBlockType::DUAL_MONO: return "dualMono";
    case ChainBlockType::IR: break;
  }
  return "ir";
}

inline ChainBlockType chainBlockTypeFromString(const juce::String& s) {
  if (s == "nam") return ChainBlockType::NAM;
  if (s == "insert") return ChainBlockType::INSERT;
  if (s == "cab") return ChainBlockType::CAB;
  if (s == "eq") return ChainBlockType::EQ;
  if (s == "dualMono") return ChainBlockType::DUAL_MONO;
  return ChainBlockType::IR;
}

// Explicit IR content category (IR blocks only). Replaces a duration-derived
// short/long split as the source of every audible IR default: real catalog
// content includes cabs manually trimmed to seconds of file length around
// tens of ms of real signal, and reverb/space IRs genuinely span the same
// range cab file lengths do, so no length measurement - raw or detected - is
// a safe stand-in for what the content actually *is*. Cab = real cabinet
// content: hard-capped to its first 500 ms at load (ProcessorModelLoader.cpp)
// and always the uniform engine, unconditionally safe regardless of the
// source file. IrPlayer = everything else (space/reverb/outboard/
// experimental/generic IR, or unknown): no cap, engine picked adaptively from
// detected content length (see ChainBlock::irIsLong). See
// TONE3000Processor::setBlockIrCategory; shipped to the UI as `irCategory`
// ("cab"/"irPlayer").
enum class IrCategory { Cab, IrPlayer };

inline juce::String irCategoryToString(IrCategory category) {
  return category == IrCategory::Cab ? "cab" : "irPlayer";
}

inline IrCategory irCategoryFromString(const juce::String& s) {
  return s == "cab" ? IrCategory::Cab : IrCategory::IrPlayer;
}

// Wet-path fade time (see ChainBlock::wetFadeGain): every discontinuous
// per-block transition (engine swap, power toggle, block add/removal)
// glides the block's wet mix through bypass over this ramp instead of
// splicing the waveform (audible click). Also the ramp for the global
// chain-edit fade (reorder/cross-lane moves mute-splice the chain output).
constexpr double kWetFadeSeconds = 0.025;
// Scene switch crossfade between two warm NAM engines (both run meanwhile).
constexpr double kSceneXfadeSeconds = 0.03;
// Channels per block (see ChainBlock::channels).
constexpr int kNumBlockChannels = 4;

// Minimum tiles in the chain. The chain always presents at least this many
// blocks (tones + insert placeholders), and always at least one insert
// placeholder, so an empty chain shows kMinLaneSlots empty slots, and once
// the user has filled them all there is still one trailing empty slot to add
// into. The invariant (insertCount == max(kMinLaneSlots - toneCount, 1)) is
// enforced by TONE3000Processor::normalizeLaneInserts after every structural
// change.
constexpr int kMinLaneSlots = 5;

// Chain block data structure
// A prepared IR/Cab engine for one of a block's other channels (see
// ChainBlock::warmIrEngines): the convolvers plus every model-derived field
// that has to travel with them when they become the live engine.
struct WarmIrEngine {
  std::unique_ptr<juce::dsp::Convolution> convolverMono;
  std::unique_ptr<juce::dsp::Convolution> convolverStereo;
  int irNumChannels = 1;
  int irLengthBaseSamples = 0;
  bool irIsLong = false;
  float irNormalizationGainLinear = 1.0f;           // content-only
  float irEffectiveNormalizationGainLinear = 1.0f;  // with Size compensation
  juce::AudioBuffer<float> irRawSamples;
  double irRawSampleRate = 0.0;
  int irContentLengthSamples = 0;
  int irOnsetSamples = 0;
  int irOnsetSamplesRelaxed = 0;
  std::vector<std::pair<float, float>> irWaveformPeaks;
};

struct ChainBlock {
  std::string id;  // Chain block UUID
  ChainBlockType type;

  // Tone metadata (full tone JSON stored for complete state persistence)
  int toneId;
  juce::String toneJson;  // Complete tone JSON from TONE3000 API
  int activeModelId;      // Currently active model ID (single source of truth)

  // Parsed-once copy of toneJson (full API payload; model switching needs
  // the model URLs) and the slim projection getChainState ships to the UI
  // (title/images/user/model names only). Both are ref-counted vars, so
  // serializing chain state is O(1) per block instead of a JSON re-parse.
  // Set together wherever toneJson is set; see setToneOnBlock.
  juce::var toneVar;
  juce::var toneSummary;

  // Model cache: stores downloaded model data by model ID
  std::map<int, std::vector<uint8_t>> modelCache;

  // True when the block's stored tone can still name this model: it is the
  // active model, or the toneVar models array lists it (local tones keep
  // their full list; catalog tones collapse to the active model on every
  // switch, see switchModel). This is the persistence boundary for
  // modelCache: saves embed bytes and restores re-seed them only for
  // referenced models (serializeChainToTree / reconcileChainFromTree).
  // Anything else in the cache is an in-memory audition convenience;
  // persisting those bytes is what bloated DAW projects by hundreds of MB
  // (issue #127).
  bool referencesModel(int modelId) const {
    if (modelId == activeModelId)
      return true;
    if (const auto* models = toneVar["models"].getArray())
      for (const auto& model : *models)
        if (static_cast<int>(model["id"]) == modelId)
          return true;
    return false;
  }

  // True when one of the block's other (inactive) channels uses modelId: its
  // bytes must stay cached and its engine warm (see Scenes & Channels).
  bool channelReferencesModel(int modelId) const {
    for (int c = 0; c < static_cast<int>(channels.size()); ++c)
      if (c != activeChannel && channels[static_cast<size_t>(c)].isValid() &&
          static_cast<int>(channels[static_cast<size_t>(c)].getProperty("activeModelId", 0)) ==
              modelId)
        return true;
    return false;
  }

  // State flags
  bool loaded;   // True when active model is loaded and ready
  bool enabled;  // True when block is enabled in processing chain

  // True when the last download/prepare of the active model failed (network
  // down, tone3000.com unreachable, bad model data). The UI swaps its loading
  // dots for a retry affordance targeting retryModelLoad. Runtime-only,
  // never persisted; cleared whenever a new load is queued.
  bool loadFailed{false};

  // True while a background download/prepare of the active model is in
  // flight. Split from `loaded` so a model switch/tone swap keeps the
  // previous engine processing (`loaded` stays true) while the replacement
  // downloads; the UI keys its loading affordances off this flag.
  // Runtime-only, never persisted.
  bool modelLoading{false};

  // One-shot: armed by loadTone (Select-flow) so the block's first
  // successful load sets the default mix from its IR category (Cab = 100%
  // wet, IrPlayer = 50%; see irCategory below and applyPreparedModelToChain
  // Block). Cleared on first apply; never set by swaps/switches/restores,
  // which keep the user's mix. Runtime-only, never persisted.
  bool applyDefaultMixOnLoad{false};

  // Click-free wet-path fade (audio thread) + swap handshake.
  // `wetFadeGain` multiplies the block's wet mix and is the smoothing path
  // for every transition whose end state is bypass: the audio thread targets
  // it at 1 while the block wants to be heard (`enabled` and no swap
  // pending) and 0 otherwise, so power toggles glide through bypass, fresh
  // blocks fade in from bypass, and removals fade out before detaching.
  //
  // The handshake: another thread raises `swapFadePending` (engine swap,
  // failure drop, removal), the audio thread fades to silence and raises
  // `swapFadeDone`, and the requester then applies its change under the
  // chain lock (see requestSwapFadeAndWait: bounded wait; when no
  // callbacks are running the change applies directly, nothing is audible).
  //
  // Two fade shapes, picked by `swapMuteWet`:
  //  - false (bypass fade): wetFadeGain rides the mix and glides the
  //    post-mix Out Gain to unity in step, so the output crossfades toward
  //    the block's dry input at pass-through level. Right for transitions
  //    that END at bypass (power off, removal, failure drop, fresh-block
  //    fade-in; unity dry is what plays afterwards anyway).
  //  - true (wet mute): engine swaps end back at wet, and their dry input
  //    was never audible; at 100% mix crossfading through it blasts ~50 ms
  //    of the un-cabbed/un-ampped signal (a raw amp head into no cab is a
  //    loud bright burst). Instead `swapWetMuteGain` mutes just the wet
  //    term while the dry share of the user's mix holds steady: the old
  //    engine dips to silence, engines swap, the new one fades in from
  //    silence. wetFadeGain stays at 1 throughout.
  // Both gains are plain multipliers in the mix loop, so the shapes compose
  // (a power toggle mid-swap still glides to bypass through wetFadeGain).
  std::atomic<bool> swapFadePending{false};
  std::atomic<bool> swapFadeDone{false};
  std::atomic<bool> swapMuteWet{false};
  juce::LinearSmoothedValue<float> wetFadeGain;
  juce::LinearSmoothedValue<float> swapWetMuteGain{1.0f};

  // Set by the audio thread when NAM processing throws (the block is disabled
  // in the same breath). The message thread drains it in getChainState and
  // writes the log line there; string building/logging is not RT-safe.
  std::atomic<bool> rtProcessingFailed{false};

  // NAM-specific processing (runs at the chain rate; see ChainDomain.h)
  std::unique_ptr<NamEngine> namEngine;

  // Scenes, gapless model switching (see ProcessorScenes.cpp). Prepared NAM
  // engines for the other models this block's scenes select, keyed by model
  // id, ready to swap in without a load. Message thread only (under
  // chainMutex); the audio thread never touches the map.
  std::map<int, std::unique_ptr<NamEngine>> warmNamEngines;
  std::set<int> warmPending;  // prewarm jobs in flight
  // Crossfade after a scene switch: the previous engine keeps running on a
  // copy of the block's input while the new one fades in over
  // kSceneXfadeSeconds, so the switch has no gap. The audio thread clears
  // xfadeActive when the fade completes; the message thread then returns
  // the outgoing engine to the warm pool (or drops it).
  std::unique_ptr<NamEngine> xfadeOutgoingNam;
  int xfadeOutgoingModelId = 0;
  bool xfadeActive = false;
  juce::LinearSmoothedValue<float> xfadeGain{1.0f};
  juce::AudioBuffer<float> xfadeScratch;
  juce::LinearSmoothedValue<float> namNormalizationSmoother;

  // IR/Cab counterpart (see ProcessorScenes.cpp): prepared engines for the
  // other channels, keyed by the channel's kernel signature (model + shape,
  // since two channels can share an IR file but shape it differently).
  // Message thread only (under chainMutex). A switch crossfades from the
  // outgoing engine (xfadeOutgoingIr, reusing xfadeGain/xfadeActive/
  // xfadeScratch at the base rate, inside the IR island) to the new one.
  std::map<juce::String, WarmIrEngine> warmIrEngines;
  std::set<juce::String> warmIrPending;
  WarmIrEngine xfadeOutgoingIr;
  juce::String xfadeOutgoingIrKey;
  float xfadeOutgoingIrGain = 1.0f;  // outgoing engine's clamped normalization

  // IR-specific processing.
  // convolverMono: IR channel 0 loaded with Stereo::no; applies the same (left) kernel to
  //   every audio channel. Always present for a loaded IR; used as the mono fallback.
  // convolverStereo: IR loaded with Stereo::yes; audio ch0 ⊗ IR ch0, audio ch1 ⊗ IR ch1.
  //   Only created when the IR file actually has >= 2 channels (true stereo IR).
  // The convolution engine is picked at load time by irCategory/irIsLong:
  // Cab always gets JUCE's uniform zero-latency engine, IrPlayer the
  // two-stage non-uniform engine (also zero latency) once detected content
  // runs long; see prepareBlockModelOffThread.
  std::unique_ptr<juce::dsp::Convolution> convolverMono;
  std::unique_ptr<juce::dsp::Convolution> convolverStereo;
  // Convolution always runs at kChainBaseSampleRate: when the chain is
  // oversampled this island decimates the block's wet path to the base rate
  // around the convolver and interpolates back (linear processing gains
  // nothing from oversampling; its CPU scales ~quadratically with the rate).
  // Bypass (zero-cost) at factor 1. See ChainOversampler.h.
  ChainOversampler irBaseRateIsland;
  int irNumChannels{1};  // channels in the loaded IR file (1 or 2)
  // Loaded IR length in base-rate samples (post trim + resample, read off
  // the built engine). Feeds refreshIrTailLength / getTailLengthSeconds so
  // hosts render real reverb tails.
  int irLengthBaseSamples{0};
  // Engine-selection signal ONLY - purely a CPU/latency-profile decision,
  // fully decoupled from every audible IR default (those come from
  // irCategory below). Always false for Cab (unconditionally the uniform
  // engine, see ProcessorModelLoader.cpp). For IrPlayer, true when a load-
  // time RMS scan finds real audible content past ~1 s (the two-stage
  // non-uniform engine pays off there); never derived from raw/trimmed file
  // length, which is exactly as untrustworthy for this as it once was for
  // the audible pad/mix bug (files are routinely trimmed/padded well past
  // their real content). Shipped to the UI as `irLong`.
  bool irIsLong{false};
  juce::LinearSmoothedValue<float> irNormalizationSmoother;
  // Content-only unit-energy gain (computeIrNormalizationGain), fixed at
  // load time - deliberately Size-independent, never touched by a shaping
  // rebuild. irEffectiveNormalizationGainLinear below is what actually
  // drives irNormalizationSmoother/playback.
  float irNormalizationGainLinear{1.0f};
  // What irNormalizationSmoother's target is actually set to on the audio
  // thread (Processor.cpp's IR/CAB branches): irNormalizationGainLinear
  // times Size's loudness compensation (irSizeGainCompensation above),
  // recomputed by rebuildIrShapeInBackground on every IR shape rebuild.
  // A *separate* field from irNormalizationGainLinear, not that field
  // mutated in place, so the base content gain stays the stable, always-
  // recomputable-from-scratch reference a repeated Size edit multiplies
  // against - mutating it in place would compound on every rebuild.
  // Starts equal to irNormalizationGainLinear (Size defaults to 100%, so
  // compensation is a no-op) until the first shape rebuild sets it for
  // real.
  float irEffectiveNormalizationGainLinear{1.0f};

  // Explicit IR content category (see IrCategory above): the sole source of
  // this block's default mix (Processor.cpp /
  // applyPreparedModelToChainBlock). Site-loaded tones resolve it
  // synchronously from the tone's `gear` metadata (loadTone), before the
  // download even starts. Persisted; a user edit (setBlockIrCategory) is a
  // real, permanent setting, never re-derived afterward.
  IrCategory irCategory{IrCategory::IrPlayer};
  // One-shot, runtime-only (never persisted): true when this block's
  // category isn't known yet at load time (local file drops have no `gear`
  // tag; state saved before this field existed has no persisted value
  // either) and must be seeded once from the load-time content scan instead
  // (applyPreparedModelToChainBlock). Never set for swaps/switches, which
  // keep the block's existing category exactly like they keep its mix.
  bool irCategoryNeedsDurationGuess{false};

  // Untouched, file-rate copy of the loaded IR (source of truth for the
  // waveform display and future Length/Decay/Curve editing) plus a
  // downsampled { min, max } per column for that display. Runtime-only:
  // never persisted, rebuilt from the cached model bytes on restore the
  // same way convolverMono itself is. Cleared when the block's IR is
  // swapped/removed (see ProcessorModelLoader.cpp's apply step).
  juce::AudioBuffer<float> irRawSamples;
  double irRawSampleRate{0.0};
  // Detected end of audible content within irRawSamples (file-rate samples):
  // -60 dB relative to peak, scanned backward in ~2ms pooled-RMS windows,
  // plus a margin (see computeIrContentLengthSamples). Single source of
  // truth for the waveform display's auto-fit trim and the length label -
  // shipped to the UI as irContentLengthMs (ms, at irRawSampleRate).
  int irContentLengthSamples{0};
  // Detected START of audible content within irRawSamples (file-rate
  // samples): mirrors irContentLengthSamples but scans forward from sample
  // 0 (see computeIrOnsetSamples) - where a leading-silence recording
  // artifact actually ends. Deliberately untouched by every shaping edit,
  // same as irContentLengthSamples; only trimInitEnabled decides whether
  // prepareIrShapeRebuild actually starts its copy here instead of at 0.
  // Load-time only: never re-measured on a Length/Decay/Size/Width edit.
  int irOnsetSamples{0};
  // Same detection, computed once more at load time with a higher (less
  // sensitive) threshold - see computeIrOnsetSamples's thresholdDb param.
  // Some IRs (slow-building reverb tails, room tone) clear -60dB well before
  // their audible transient, so the standard threshold lands the onset too
  // early; trimRelaxed switches to this instead of re-scanning on toggle.
  int irOnsetSamplesRelaxed{0};
  // User toggle for the above: off by default (deliberately NOT automatic -
  // detecting "silence" by an RMS threshold is a heuristic, and applying it
  // unconditionally on every load could clip an IR that's quiet-but-
  // intentional at the very start). Persisted (see ProcessorState.cpp).
  // Feeds prepareIrShapeRebuild as trimStartSamples (irOnsetSamples/
  // irOnsetSamplesRelaxed when on, 0 when off) via
  // setBlockIrTrimInit/rebuildIrShapeInBackground - the same off-thread-
  // rebuild shape as sizeNormalized/widthNormalized.
  bool trimInitEnabled{false};
  // Which onset detection threshold trimInitEnabled uses: standard (false)
  // or relaxed (true, irOnsetSamplesRelaxed). Meaningless while
  // trimInitEnabled is false, but kept as its own field (rather than folded
  // into a tri-state enum) so existing trimInitEnabled call sites/tests
  // don't need to change - see setBlockIrTrimInit's relaxed param.
  bool trimRelaxed{false};
  // Reverse: plays the fully-shaped kernel backward. Off by default,
  // manual-only (see setBlockIrReverse) - same coalesced off-thread-rebuild
  // shape as trimInitEnabled, just a plain user preference with nothing to
  // detect. Applied last in prepareIrShapeRebuild, after Trim Init's copy
  // offset and the Attack/Decay envelope, so it reverses exactly what's
  // audible (whatever shape the user already dialed in), not the raw
  // source - and never disturbs Trim Init's own onset detection, which
  // always scans irRawSamples in its true, un-reversed orientation.
  bool reverseEnabled{false};
  std::vector<std::pair<float, float>> irWaveformPeaks;

  // Predelay: delays the wet signal before it enters the convolver (see
  // BlockPredelay). Runs inside irBaseRateIsland, always at the base rate.
  // predelayNormalized is the persisted 0..1 knob value (same convention as
  // mixNormalized/inputGainNormalized); the engine is driven in real ms via
  // setDelayMs(predelayNormalized * BlockPredelay::kMaxDelayMs).
  BlockPredelay predelay;
  float predelayNormalized{0.0f};

  // IR envelope: a 2-segment Attack/Decay shape (Space Designer-style) over
  // the loaded IR, truncating it (+ short fade-out) and applying a gain
  // envelope in one pass - see TONE3000Processor::prepareIrShapeRebuild for
  // the exact formula. Rebuilds the convolver engine off-thread rather than
  // processing in real time (see rebuildIrShapeInBackground) - unlike
  // Predelay, this edits what the convolver *is*, not a real-time DSP stage.
  //
  // Init Level is the level at sample 0 (the origin point, not part of
  // either segment). The Attack segment runs from there up to unity/0dB -
  // pinned, not a knob: standard AD-envelope semantics (Attack always
  // reaches full level; only Decay's target level is adjustable) - then the
  // Decay segment continues from that peak to the truncated content's end
  // (Decay Length, Decay Level). Both adjustable levels are unipolar
  // attenuation-only (0..1, 1.0 = unity/0dB, 0.0 = genuine silence - see
  // prepareIrShapeRebuild's levelToDb for why that needs a small
  // float-safety floor internally but still lands on exact silence at the
  // sample the knob targets). Each segment has its own continuous curve
  // control (0.5 default = linear-in-dB/"exponential", sweeping
  // steeper-front-loaded below and back-loaded above - same kCurveMax
  // power-curve formula Length+Decay used before this became two segments).
  //
  // Length semantics: decayLengthNormalized is the TOTAL truncated length -
  // the real "End" position, exactly the old standalone Length knob's own
  // convention: a fraction of the block's full frozen detected content
  // (irContentLengthSamples, load-time). attackLengthNormalized is NOT an
  // independent segment length - it's a fraction *of that total*, marking
  // where the peak (the Attack/Decay boundary) sits within it, so it's
  // naturally bounded to [0, the total] by construction (a fraction of a
  // fraction), no separate clamp/edge-case needed, and dragging Attack
  // Length alone can never change the total window length - only Decay
  // Length does that. Defaults (attackLength 0.0, decayLength 1.0, every
  // level 1.0) reconstruct the pre-segment behavior exactly: no attack ramp,
  // decay spans the full content, flat/unity envelope - a genuine no-op,
  // matching the old lengthNormalized=1.0/level=unity defaults.
  // irIsLong/irNumChannels and irRawSamples/irContentLengthSamples/
  // irWaveformPeaks never change from an envelope edit - only the live
  // convolverMono/convolverStereo and irLengthBaseSamples do; the waveform
  // display's fixed window and the default mix stay exactly
  // what they were at load.
  float initLevelNormalized{1.0f};
  float attackLengthNormalized{0.0f};
  float attackCurveNormalized{0.5f};
  float decayLengthNormalized{1.0f};
  float decayLevelNormalized{1.0f};
  float decayCurveNormalized{0.5f};

  // IR Size: vari-speed duration/pitch over the frozen source (see
  // irSizeDurationRatio above and setBlockIrSize). Normalized 0..1, 0.5 =
  // 100%/unchanged. Applied upstream of the envelope in
  // prepareIrShapeRebuild (it only changes the declared sample rate handed
  // to the convolver, never the trimmed buffer's sample count), so every
  // envelope fraction above keeps landing at the same relative position
  // automatically - no special-casing needed between the two.
  float sizeNormalized{0.5f};

  // IR Width: stereo image control, independent of the chain-level Balance/
  // Pan/Align controls (see setBlockIrWidth). Normalized 0..1, 0.5 = 0%/
  // mono (the knob's bipolar center). 0..100% (normalized 0.25..0.75)
  // crossfades mono <-> the IR's own recorded stereo image; sign (which
  // half of 0..1 it's on) only flips L/R orientation there, not the blend
  // amount. Beyond 100% (normalized <0.25 or >0.75) adds partial phase
  // inversion of one channel - which channel is picked by the same sign,
  // not by magnitude (see prepareIrShapeRebuild). Locked at 0% in the UI
  // whenever the loaded IR is mono (irNumChannels == 1), since there's no
  // stereo image to widen; never applied to convolverMono, which always
  // stays the untouched-image fallback engine.
  //
  // Default is 0.75 (100%/full original stereo), deliberately NOT the
  // knob's own 0.5 center: 0.5 is genuinely mono, and every stereo IR
  // played its true recorded image unconditionally before this control
  // existed - defaulting new/existing blocks to 0.5 would silently fold
  // them to mono on load. 0.75 preserves that exact prior behavior as the
  // no-op starting point; the knob's visual center is just where 0% lives
  // on its bipolar track, not where it starts.
  float widthNormalized{0.75f};

  // Bumped by setBlockIrDecay, captured by rebuildIrShapeInBackground's
  // caller as the generation it's targeting: a "latest wins" supersede
  // guard, since a rebuild must read *all seven* shaping params fresh off
  // the block rather than trusting a single stashed target value.
  std::atomic<int> irShapingGeneration{0};

  // Per-block loudness normalization toggle, NAM only (off = the capture's
  // true level, which is real information; IR normalization is always on
  // because an IR file's absolute level means nothing). On by default; part
  // of the chain state so presets carry their own gain staging. The UI
  // exposes it as an optional (=) header control behind an advanced
  // preference.
  bool normalizeEnabled{true};

  // Per-block NAM A2 size, stored in NAM's own slimmable-size domain (0..1;
  // 0.0 = lite, 1.0 = full, and the tier boundary belongs to the tier above,
  // so 0.5 already selects full). The value feeds
  // NamEngine::setSlimmableSize verbatim. Inert for IR blocks, like
  // normalizeEnabled. Part of the chain state so presets carry each block's
  // size; new blocks start at the machine-wide default
  // (TONE3000Processor::setNamSlimSizeDefault) and setBlockSlimSize retiers
  // the loaded engine in place.
  double namSlimSize{0.0};

  // Per-block controls (normalized 0..1)
  float inputGainNormalized{0.5f};  // 0.5 = unity gain; drives the block harder/softer
  juce::LinearSmoothedValue<float> inputGainSmoother;
  float outputGainNormalized{0.5f};  // 0.5 = unity gain
  juce::LinearSmoothedValue<float> outputGainSmoother;
  float mixNormalized{1.0f};  // 0 = dry, 1 = wet
  juce::LinearSmoothedValue<float> mixSmoother;

  // Per-block meter levels (dB, -60 floor). Written by the audio thread every
  // block, read by the UI via getMeterLevels(). Input is measured post
  // input-gain (what the model actually receives), output post mix + Out
  // Gain.
  std::atomic<float> inputMeterDb{-60.0f};
  std::atomic<float> outputMeterDb{-60.0f};

  // Per-block 6-band EQ: on the wet signal after the model by default
  // (before Out Gain and the mix), or between the input gain and the model
  // when its pre flag is on. Flat by default, in which case processing is
  // skipped entirely (single branch per audio block).
  BlockEq eq;

  // Channels (Fractal-style): up to kNumBlockChannels full versions of this
  // block - model, knobs, EQ, IR shape, Dual Mono image... everything except
  // bypass (a scene's call) and identity/tone. Scenes pick a channel per
  // block. The ACTIVE channel is the live block itself; its slot here may be
  // stale and is refreshed on switch-away/serialize. An invalid slot has
  // never been used and starts as a copy of the live block when selected.
  // See TONE3000Processor's CHANNELS section (ProcessorScenes.cpp).
  int activeChannel = 0;
  std::array<juce::ValueTree, 4> channels;

  // Spectrum analyzer for the EQ editor backdrop. Only fed by the audio thread
  // while the UI has this block's EQ view open (atomic enabled flag).
  BlockSpectrum spectrum;

  // DUAL_MONO only: the two fixed child slots. Each is a
  // std::vector<std::unique_ptr<ChainBlock>> holding 0 or 1 element - never
  // more, never an extensible chain of its own (see ChainBlockType::
  // DUAL_MONO's own comment above; the vector shape is purely an
  // implementation convenience, not a UI capability) - so runDualMono
  // (Processor.cpp) can drive each side through the existing, well-tested
  // processChainOnBuffer directly with zero new per-block DSP dispatch, and
  // an empty side (size 0) already behaves as inert pass-through with no
  // special-casing (processChainOnBuffer over an empty vector is a no-op).
  // Self-referential via unique_ptr is legal here the same way it was for
  // the earlier zone design: inline member functions are parsed in
  // complete-class context.
  std::vector<std::unique_ptr<ChainBlock>> dualLeft;
  std::vector<std::unique_ptr<ChainBlock>> dualRight;
  // DUAL_MONO only: recombine controls for blending dualLeft/dualRight back
  // into the main signal - ported near-verbatim from the earlier zone
  // design's own zoneLeftPan/zoneRightPan/zoneWidth (see runDualMono,
  // Processor.cpp): each side panned by constant-power gains, the two
  // panned images summed, then blended against the mono sum by width.
  // Defaults hard-left/hard-right/full-width, matching the old design's own
  // reasoning: each side keeps its own place until the user dials in
  // something else. Plain per-block fields, not APVTS parameters - these
  // blocks are created/destroyed dynamically, same as mixNormalized.
  float dualLeftPanNormalized{0.0f};
  float dualRightPanNormalized{1.0f};
  float dualWidthNormalized{1.0f};
  // DUAL_MONO only: smoothed views of the three fields above, so a live
  // knob drag (setDualImage, called on every tick) glides instead of
  // stepping. Reset/seeded in prepareChain's DUAL_MONO branch and at
  // creation time (addDualMonoBlock) - same duration every other per-block
  // smoother uses; the recombine step reads these, never the raw
  // *Normalized fields directly.
  juce::LinearSmoothedValue<float> dualLeftPanSmoother;
  juce::LinearSmoothedValue<float> dualRightPanSmoother;
  juce::LinearSmoothedValue<float> dualWidthSmoother;
  // DUAL_MONO only: Link - UI convenience, not applied natively beyond
  // persisting the flag itself (see setDualLinked's own comment). The
  // actual mirror/sync math (Pan reflected around center, Mix/Vol matched)
  // runs in the UI, which re-sends both sides' values through the existing
  // setDualImage/setBlockParam setters - this field exists purely so the
  // toggle's own on/off state survives undo/redo, state restore, and a
  // second open editor window, same as every other per-block bool here.
  bool dualLinked{false};
  // DUAL_MONO only: exclusive per-side solo (see setDualSolo's own comment
  // on the storage/math split). At most one is ever true - the setter
  // enforces exclusivity.
  bool dualSoloLeft{false};
  bool dualSoloRight{false};
  // DUAL_MONO only: per-side Mute, unconditional (empty or loaded - see
  // setDualMuted's own comment for why this is NOT the child's own
  // `enabled` bypass toggle: bypass crossfades to the dry, unprocessed
  // input, which is not silence, so an ordinary block's Power button is the
  // wrong tool for "mute this side"). Applied as a smoothed 0/1 gain on
  // that side's contribution to the recombine (see dualLeftSoloGainSmoother
  // below), the same true-silence mechanism Solo already uses. Persists
  // unconditionally, same "doesn't require the state it gates to be true
  // right now" shape as dualLinked above.
  bool dualLeftMuted{false};
  bool dualRightMuted{false};
  // Smoothed 0/1 gain view of Solo (above) *and* the Mute fields - a raw
  // instant 0<->1 multiply on dl/dr would click, so this rides a smoother
  // (20ms ramps) same as every other discontinuous gain change in this
  // codebase. Reset/seeded alongside the pan/width smoothers above (same
  // call sites, ChainBlock.h's own convention); the recombine step sets
  // each one's *target* every call (never a hard reset) so a live solo or
  // mute toggle glides.
  juce::LinearSmoothedValue<float> dualLeftSoloGainSmoother;
  juce::LinearSmoothedValue<float> dualRightSoloGainSmoother;
  // DUAL_MONO only: per-side polarity flip (Ø) - two amp captures don't
  // share a polarity convention, so one side can arrive 180 degrees out
  // against the other; genuinely inverted, not fixable by Align's delay
  // alone (Align corrects timing, this corrects sign). Plain per-block
  // fields like every other DUAL_MONO-only control here - see
  // setDualInvert. Applied as a smoothed +-1 gain
  // multiplied into the same per-sample recombine loop that already reads
  // dualLeft/RightSoloGainSmoother (runDualMono), same "glide, never step"
  // reasoning; reset/seeded alongside the smoothers above.
  bool dualLeftInvert{false};
  bool dualRightInvert{false};
  juce::LinearSmoothedValue<float> dualLeftPolaritySmoother;
  juce::LinearSmoothedValue<float> dualRightPolaritySmoother;

  // DUAL_MONO only: Align - the same corrective-delay + advanced-deck
  // (Wobble/Crossover/Diffuse) engine the global stereo-chain-mode Align
  // uses (see StereoOffset.h), scoped to this block's own dl/dr pair
  // instead of the two top-level lanes. runDualMono runs it on the sides'
  // raw output, right before the Pan/Width recombine - the direct analogue
  // of processImageStage running the global engine on chL/chR before
  // Balance+Pan (Processor.cpp). Plain per-block fields, not APVTS
  // parameters, same reasoning as dualLeftPanNormalized above: these
  // blocks are created/destroyed dynamically. Defaults mirror the global
  // Align's own defaults (off, centered, same wobble/crossover spans) so a
  // fresh Dual Mono block starts as a no-op, identical to today.
  StereoOffset dualAlign;
  bool dualAlignEnabled{false};
  float dualAlignOffsetNormalized{0.5f};  // bipolar, 0.5 = center = 0 ms
  float dualAlignWobbleNormalized{0.25f};
  bool dualAlignWobbleEnabled{false};
  float dualAlignCrossoverNormalized{0.5f};  // log map 32.5..520 Hz; 0.5 = 130 Hz
  bool dualAlignCrossoverEnabled{false};
  bool dualAlignDiffuseEnabled{false};
  // Master bypass for the whole Stereo Processing screen (Align's Offset/
  // Wobble/Crossover/Diffuse AND Ø), same "Power bypasses everything on
  // this page, without touching any of the dialed-in values" shape the EQ
  // pill's own Power button already has (BlockEq's `enabled`). Overrides
  // at the point of use in runDualMono (forces Align disengaged, Ø
  // neutral) rather than mutating dualAlignEnabled/dualLeftInvert/
  // dualRightInvert themselves, so turning it back on restores exactly
  // what was dialed in. Default true (not bypassed) - purely an
  // additional override, so existing states/tests are unaffected.
  bool dualStereoProcessingEnabled{true};

  // DUAL_MONO only: X-Y scope (goniometer) of the two sides' post-Align
  // output, for the Stereo Processing screen's live phase/mono-safety
  // visualization - see BlockGoniometer.h. Same "only capture while its UI
  // view is open" gating as `spectrum` above; fed in runDualMono right
  // after the Align block processes dl/dr, before the Pan/Width recombine,
  // same point dualAlign's own correlation() LED reads from.
  BlockGoniometer dualGoniometer;

  ChainBlock(const std::string& blockId, ChainBlockType blockType)
      : id(blockId), type(blockType), toneId(0), activeModelId(0), loaded(false),
        enabled(true) {}
};
