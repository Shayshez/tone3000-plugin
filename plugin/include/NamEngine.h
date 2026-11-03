#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <memory>
#include <vector>
#include "NAM/dsp.h"
#include "ChainDomain.h"

/**
 * A JUCE-compatible host for a nam::DSP model, running in the chain domain
 * (kChainBaseSampleRate × oversampling factor; see ChainDomain.h).
 * Sample-rate conversion is NOT this class's job (the whole chain stage sits
 * behind one resampling boundary + oversampler in the processor), so all this
 * does is:
 *  - float ↔ double conversion (NAM models process NAM_SAMPLE == double),
 *  - per-channel processing of stereo buffers (below),
 *  - slimmable (A2 container) tier selection,
 *  - phase-interleaved oversampled processing (below).
 *
 * Phase-interleaved oversampling:
 * A NAM model oversampled by N with its convolution dilations scaled by N is
 * mathematically identical to N independent copies of the *unscaled* model,
 * each processing every Nth sample of the oversampled stream at the native
 * rate: every scaled dilation lands taps exactly N samples apart (within one
 * phase), and 1×1 convolutions/activations are per-sample. So instead of
 * patching dilation scaling into NeuralAmpModelerCore, the engine holds N
 * instances of the same model and interleaves them. The receptive field stays
 * constant in seconds (the model sounds the same) while its nonlinear
 * harmonics land in the widened band where the chain's decimation filter
 * removes them instead of letting them alias.
 *
 * Recurrent architectures (LSTM) update state on consecutive samples and
 * can't be phase-split; the loader gives them a single instance that runs
 * time-scaled at the full chain rate (matching NAM-Oversampler's behavior
 * for such models). In practice every loadable model is phase-safe (the
 * catalog and the local-file gate only admit A2 WaveNets), so this is a
 * defensive path.
 *
 * Multi-core phases:
 * The phase instances are fully independent (separate models, disjoint
 * per-phase buffers), so process() can fork them across the processor's
 * RtWorkerPool (the same pool that forks the stereo lanes; a lane job
 * forking its phases is the pool's supported one-deep nesting). Scheduling
 * is the only thing that changes: deinterleave/reinterleave stay on the
 * calling thread and every phase writes only its own buffers, so parallel
 * output is bit-identical to serial. Passing no pool runs the classic
 * sequential loop.
 *
 * Stereo:
 * NAM models are single-channel, so a stereo signal needs a second, fully
 * independent set of instances for the right channel: two amps, one per side,
 * which is what keeps an upstream stereo image alive through the block. That
 * set only runs while the two channels actually differ. Identical channels
 * (a mono source on a stereo bus, a mono rig before any widening block) run
 * the left set alone and fan out, bit for bit the single-amp result at
 * single-amp CPU. Channels must stay identical for kRightHoldSeconds before
 * the right set idles (longer than any model's receptive field, so the two
 * sets have converged and handing back is inaudible), and a waking right set
 * crossfades in from the left output over kRightFadeSeconds, covering
 * whatever stale history it still holds.
 */
class RtWorkerPool;

class NamEngine {
public:
  /** Takes ownership of the phase instances, all built from the same model
      config. One instance runs at the full chain rate (kChainBaseSampleRate ×
      `oversampleFactor`); N > 1 instances run phase-interleaved, each at
      1/Nth of it. The loader picks the count: `oversampleFactor` instances
      for phase-safe architectures, one otherwise. `rightInstances` is the
      independent right-channel set, same count and config; left empty, the
      engine is mono and always fans out. Throws std::invalid_argument on
      empty/null instances, an invalid factor or a mismatched right set. */
  NamEngine(std::vector<std::unique_ptr<nam::DSP>> instances, int oversampleFactor,
            std::vector<std::unique_ptr<nam::DSP>> rightInstances = {});
  ~NamEngine() = default;

  NamEngine(const NamEngine&) = delete;
  NamEngine& operator=(const NamEngine&) = delete;
  NamEngine(NamEngine&&) = default;
  NamEngine& operator=(NamEngine&&) = default;

  /** Prepare for processing in the chain domain. `maxBlockSize` is the
      largest per-call frame count (the chain-domain block size). */
  void prepare(int maxBlockSize);

  /** Process a chain-domain buffer in place: channel 0 through the model,
      channel 1 through the right set while it differs from channel 0, or
      fanned out from channel 0 otherwise (see the header comment). Must be
      prepared first.
      With a pool and more than one instance to run, they fork across the
      pool's workers (bit-identical to the serial loop; see the header
      comment); a failure rethrows as std::runtime_error on this thread once
      every instance has completed. `pool` may be null (serial). */
  void process(juce::AudioBuffer<float>& buffer, RtWorkerPool* pool = nullptr);

  /** The rate the model reports it was trained at. Purely informational;
      the chain always feeds it the chain rate (A2 models are all 48 kHz). */
  double getModelSampleRate() const { return modelSampleRate; }

  /** The oversampling factor this engine was built for. The apply path
      compares it against the live factor: a mismatch (the setting changed
      while the load was in flight) means the phase count is wrong and the
      block must be rebuilt. */
  int getOversampleFactor() const { return oversampleFactor; }

  bool hasInputLevel() const { return primary().HasInputLevel(); }
  double getInputLevel() const { return primary().GetInputLevel(); }
  bool hasOutputLevel() const { return primary().HasOutputLevel(); }
  double getOutputLevel() const { return primary().GetOutputLevel(); }
  bool hasLoudness() const { return primary().HasLoudness(); }
  double getLoudness() const { return primary().GetLoudness(); }

  /**
   * Requested slimmable size (0.0 = lite, 1.0 = full). Clamped to [0.0, 1.0].
   * A no-op for models that aren't SlimmableModel (non-container A2 files).
   * NAM tier mappers assign the boundary value to the tier above (a two-tier
   * container selects lite for [0, 0.5) and full for [0.5, 1.0]), so the lite
   * request must be 0.0; 0.5 would select full.
   * Applied in prepare() and immediately if already prepared. Fans out to
   * every phase instance so all phases always run the same tier.
   */
  void setSlimmableSize(double val);

  double getSlimmableSize() const noexcept { return requestedSlimmableSize; }

  /** Resets every phase instance to its deterministic post-load state
      (Reset + prewarm), without touching buffer sizes. Recurrent/dilated-
      conv architectures carry state across calls, so two instances fed
      identical input can still disagree if they came into it with
      different recent history; this gives a caller a known, matching
      baseline to measure from (Auto Align/Balance's probe - see
      AutoOffset.h). Message thread only: prewarm() is not real-time safe,
      same restriction as prepare(). */
  void resetState();

private:
  /** Largest per-sample L/R gap (-120 dB) still treated as identical: an
      upstream mono convolver's two channels can settle a ULP apart. */
  static constexpr float kChannelsDifferThreshold = 1.0e-6f;
  /** Seconds the channels must stay identical before the right set idles. */
  static constexpr double kRightHoldSeconds = 1.0;
  /** Seconds a right set takes to blend in from (or back to) the left output. */
  static constexpr double kRightFadeSeconds = 0.05;

  /** Instance 0: the reference for metadata queries (all instances share
      one model config, so levels/loudness/rate are identical). */
  nam::DSP& primary() const { return *instances.front(); }

  /** The rate each instance runs at: chain rate for a single instance, the
      base rate for phase instances. */
  double instanceSampleRate() const {
    return kChainBaseSampleRate * oversampleFactor / static_cast<double>(instances.size());
  }

  std::vector<std::unique_ptr<nam::DSP>> instances;
  std::vector<std::unique_ptr<nam::DSP>> rightInstances;  // empty = mono engine
  int oversampleFactor = 1;
  double modelSampleRate;

  // Per-phase double-precision I/O (NAM expects double); the deinterleave IS
  // the float→double conversion. A single instance is just the one-phase
  // case of the same path. Sized in prepare().
  std::vector<std::vector<double>> phaseInputs;
  std::vector<std::vector<double>> phaseOutputs;
  std::vector<std::vector<double>> rightPhaseInputs;
  std::vector<std::vector<double>> rightPhaseOutputs;
  std::vector<int> phaseFrames;

  // Which phase the next incoming sample belongs to. Chain buffers are
  // normally divisible by the phase count (the oversampler guarantees it),
  // but tracking the offset keeps every instance's sample stream continuous
  // even if a defensive slice path hands over a partial block.
  int phaseOffset = 0;

  // Right-set activity, in chain-rate samples: frames left before it idles,
  // and its blend against the left output (0 = fan-out, 1 = fully its own).
  int rightHoldFrames = 0;
  int rightHoldRemaining = 0;
  float rightBlend = 0.0f;
  float rightBlendStep = 0.0f;

  bool isPrepared = false;
  int maxBlockSize = 0;

  double requestedSlimmableSize{1.0};
};
