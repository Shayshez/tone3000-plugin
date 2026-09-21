#pragma once
#include "ImageDeck.h"
#include <juce_core/juce_core.h>
#include <atomic>
#include <vector>

/**
 * Per-Dual-Mono-block goniometer (X-Y scope of the block's true final
 * output - post Align, Solo/Mute/Ø, Pan/Width, AND Master EQ, the exact
 * same buffer that continues down the chain) for the Stereo Processing
 * screen's live phase/mono-safety visualization - the in-plugin equivalent
 * of what the user was reading off an external correlation meter while
 * dialing in Align. Must be fed from the literal output buffer, not some
 * earlier stage: Pan/Width/Vol changes need to visibly move this meter
 * exactly like they'd move a listener's ear, or the meter is measuring the
 * wrong thing (a real bug this class's own git history hit once already).
 *
 * Also runs its own continuous DeckCorrelation (see ImageDeck.h) fed from
 * the exact same samples as the scatter points, deliberately NOT the
 * StereoOffset's own correlation() - that one only updates while Align's
 * engine is actively processing (isRunning()), so centering Offset and
 * disengaging Align would freeze it at a stale value. This meter is a
 * general-purpose monitor of the two sides' current relationship, live
 * whenever the Stereo Processing view is open, regardless of whether
 * Align/Ø are doing anything right now.
 *
 * Same threading shape as BlockSpectrum: the audio thread writes into a
 * lock-free ring, only while `enabled` (the UI view is actually open).
 * Unlike BlockSpectrum (which analyzes a fixed-size window on demand),
 * getPoints() *drains* everything pushed since the last call - a scatter
 * plot wants every sample a chance to be drawn, not a periodic snapshot,
 * or fast transients between polls would never appear. Message-thread only
 * (`readPos`), single consumer (the polled native getter).
 *
 * Decimated 1-in-kDecimation at push time (a plain stride, not a filter -
 * aliasing doesn't matter for a visual scatter, unlike real audio
 * decimation) so the polled payload stays a few hundred points at the
 * UI's actual poll rate instead of audio-rate density human eyes can't
 * resolve anyway.
 */
class BlockGoniometer {
public:
  BlockGoniometer();

  void prepare(double sampleRate);

  void setEnabled(bool shouldBeEnabled) { enabled.store(shouldBeEnabled); }
  bool isEnabled() const { return enabled.load(std::memory_order_relaxed); }

  /** Real-time thread. l/r must be the same length; no allocation. */
  void pushSamples(const float* l, const float* r, int numSamples);

  /** Message thread. Returns a flat var array [l0, r0, l1, r1, ...] of
      everything pushed since the last call (capped at kMaxPointsPerPoll -
      a stalled poller silently loses older history, never grows unbounded). */
  juce::var getPoints();

  /** Continuous ~300ms running L/R correlation (-1..1, 1 when idle/silent).
      See the class comment for why this is a separate, always-live meter
      rather than StereoOffset's own correlation(). Readable from any
      thread. */
  float correlation() const { return corrMeter.value(); }

private:
  static constexpr int kDecimation = 4;
  static constexpr int kRingSize = 1 << 13;  // 8192 pairs, power of 2
  static constexpr int kMaxPointsPerPoll = 4096;

  std::vector<float> ringL, ringR;
  std::atomic<int> writePos{0};
  std::atomic<bool> enabled{false};
  int decimateCounter = 0;
  DeckCorrelation corrMeter;

  // Message thread only.
  int readPos = 0;
};
