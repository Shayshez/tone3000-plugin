#pragma once
#include <juce_core/juce_core.h>
#include <juce_data_structures/juce_data_structures.h>
#include <juce_audio_basics/juce_audio_basics.h>
#include <array>

/**
 * Eight-band parametric EQ, one per chain block. Runs on the block's wet
 * signal by default (after the model, before the dry/wet mix, so the dry
 * share of Mix stays untouched); the `pre` flag moves it between the
 * block's input gain and its model instead, shaping the signal
 * that drives the amp/IR. Self-contained module: band parameters, biquad
 * coefficient math (RBJ cookbook, mirrored exactly by
 * ui/src/components/eqMath.ts so the drawn curve is the audio truth),
 * processing, and (de)serialization.
 *
 * Fixed channel-strip roles by index, no user-facing type selector: band 0
 * is always Low Cut, band 1 Low Shelf, the last two bands are High Shelf
 * then High Cut, everything between is a Bell (see roleForIndex()). Low/High
 * Cut get a discrete Pole count (1/3/4/6/8 = 6/18/24/36/48 dB/oct) instead of
 * Gain, cascading that many biquad/first-order stages in series; every other
 * band is a single RBJ biquad as before.
 *
 * Threading model: setters run on the message thread while `chainMutex` is
 * held (the audio thread holds the same lock during processing), so plain
 * members are safe and all transcendental math happens off the audio thread.
 * process() does zero allocation.
 *
 * Per-band bypass: each band carries its own `on` flag (the UI's band icon
 * doubles as this toggle) alongside the EQ-wide `enabled` power button.
 * Flat-skip: every band precomputes an `active` flag when its params change -
 * `on == false` is always inert; a Bell/shelf band with ~0 dB gain is inert
 * even while `on`; a Low/High Cut is active whenever it's on (it has no
 * "trivial" setting to auto-detect, unlike gain). When no band is active,
 * isActive() is false and callers skip process() entirely; a flat/bypassed
 * EQ costs one branch per audio block.
 *
 * Bypass: `enabled` (the EQ power button) gates isActive() the same way, so a
 * bypassed EQ keeps its band settings but costs nothing on the audio thread.
 */
class BlockEq {
public:
  static constexpr int kNumBands = 8;
  static constexpr float kMinFreqHz = 20.0f;
  static constexpr float kMaxFreqHz = 20000.0f;
  static constexpr float kMaxAbsGainDb = 24.0f;
  static constexpr float kMinQ = 0.1f;
  static constexpr float kMaxQ = 20.0f;
  /** Biquad stages a Low/High Cut band can cascade (ceil(8/2)). */
  static constexpr int kMaxCutStages = 4;
  /** Supported pole counts (6/18/24/36/48 dB/oct). */
  static constexpr std::array<int, 5> kPoleOptions{1, 3, 4, 6, 8};

  enum class BandRole { LowCut, LowShelf, Bell, HighShelf, HighCut };

  /** Fixed channel-strip role by position (mirrored by the UI). */
  static BandRole roleForIndex(int index);

  /** Nearest supported pole count to `requested`. */
  static int snapPoles(int requested);

  struct Band {
    float freqHz{1000.0f};
    float gainDb{0.0f};
    float q{0.71f};
    /** Pole count, meaningful only for Low/High Cut bands (see
        roleForIndex()); kept on every band for a uniform struct shape. */
    int poles{4};
    /** Per-band bypass: the UI's band icon doubles as this toggle. Low/High
        Cut default off (a cut is an active choice, not a baseline state, so
        it stays out of the way until the user reaches for it); every other
        band defaults on (matches its own already-inert-at-0dB default). */
    bool on{true};
  };

  /** Guitar/bass-voiced defaults, all flat (0 dB) and every band left on
      except the two cuts (see Band::on): low cut 80 Hz, low shelf 100 Hz,
      bells at 250 (mud) / 650 (boxiness) / 1.6k (presence) / 3.5k (bite,
      tighter Q), high shelf 8 kHz (fizz/air), high cut 12 kHz. */
  static std::array<Band, kNumBands> defaultBands();

  BlockEq();

  /** Message thread (under chainMutex). Recomputes coefficients for the given
      sample rate; resets filter state. */
  void prepare(double sampleRate);

  /** Message thread (under chainMutex). Clamps values, recomputes the band's
      coefficients and activity. Returns false for an out-of-range index. */
  bool setBand(int index, const Band& band);

  /** Message thread (under chainMutex). Parses { freqHz, gainDb, q, poles, on }. */
  bool setBandFromVar(int index, const juce::var& bandVar);

  /** Message thread (under chainMutex). Back to flat defaults (and enabled). */
  void resetToDefault();

  /** Message thread (under chainMutex). Bypass toggle: band settings are
      kept; a disabled EQ is skipped exactly like a flat one. */
  void setEnabled(bool shouldBeEnabled);
  bool isEnabled() const { return enabled; }

  /** Message thread (under chainMutex). Position toggle: true = before the
      block's model (after its input gain), false = after the model on the
      wet path (default). Filter state resets on change; the EQ taps a
      different signal point. */
  void setPre(bool shouldBePre);
  bool isPre() const { return pre; }

  bool isActive() const { return enabled && anyBandActive; }

  /** Audio thread (under chainMutex). Processes up to 2 channels in place.
      Only call when isActive(). */
  void process(juce::AudioBuffer<float>& buffer);

  /** { enabled, pre, bands: [{ freqHz, gainDb, q, poles } x8] } for the UI
      chain state. */
  juce::var toVar() const;

  /** ValueTree persistence (plugin state save/restore). */
  juce::ValueTree toValueTree() const;
  void restoreFromValueTree(const juce::ValueTree& tree);

private:
  struct Biquad {
    float b0{1.0f}, b1{0.0f}, b2{0.0f}, a1{0.0f}, a2{0.0f};  // normalized (a0 == 1)
    float z1[2]{0.0f, 0.0f}, z2[2]{0.0f, 0.0f};              // TDF2 state per channel

    inline float processSample(float x, int ch) noexcept {
      const float y = b0 * x + z1[ch];
      z1[ch] = b1 * x - a1 * y + z2[ch];
      z2[ch] = b2 * x - a2 * y;
      return y;
    }
    void resetState() { z1[0] = z1[1] = z2[0] = z2[1] = 0.0f; }
  };

  static bool isBandActive(int index, const Band& band);
  static Band clampBand(Band band);
  /** Bell/Low Shelf/High Shelf: single RBJ biquad, unchanged math. */
  void updateBand(int index);
  /** Low/High Cut: cascades `band.poles` worth of stages (an odd leftover
      first-order section plus Butterworth-Q second-order sections). */
  void updateCutBand(int index);
  void updateActivity();

  std::array<Band, kNumBands> bands;
  std::array<std::array<Biquad, kMaxCutStages>, kNumBands> stages;
  std::array<int, kNumBands> numStages{};
  std::array<bool, kNumBands> bandActive{};
  bool anyBandActive{false};
  bool enabled{true};
  bool pre{false};
  double sampleRate{48000.0};
};
