#include "AutoOffset.h"

#include <juce_dsp/juce_dsp.h>
#include <algorithm>
#include <cmath>
#include <vector>

void AutoOffset::prepare(double newSampleRate) {
  sampleRate = newSampleRate > 0.0 ? newSampleRate : 48000.0;
  sweepSamples = static_cast<int>(std::llround(kSweepSeconds * sampleRate));

  const int maxLag = static_cast<int>(std::ceil(kMaxLagMs * 0.001 * sampleRate));
  const int tailSamples =
      std::max(static_cast<int>(std::llround(kTailSeconds * sampleRate)), 2 * maxLag);
  capacity = sweepSamples + tailSamples;

  probeBuffer.setSize(1, sweepSamples);
  captureBuffer.setSize(2, capacity);
  buildSweep();

  written.store(0, std::memory_order_relaxed);
  setState(State::Idle);
}

void AutoOffset::buildSweep() {
  // Exponential sine sweep (Farina 2000):
  //   x(t) = A * sin(K * (exp(t/T * ln(f2/f1)) - 1)),  K = 2*pi*f1*T / ln(f2/f1)
  // Frequency rises exponentially from f1 to f2 over T seconds. Raised-cosine
  // edge fades avoid spectral splatter into the models.
  const double f1 = kSweepLowHz;
  const double f2 = kSweepHighFraction * std::min(sampleRate, 48000.0);
  const double T = sweepSamples / sampleRate;
  const double logRatio = std::log(f2 / f1);
  const double K = 2.0 * juce::MathConstants<double>::pi * f1 * T / logRatio;

  float* p = probeBuffer.getWritePointer(0);
  const int fadeSamples =
      std::min(sweepSamples / 4,
               static_cast<int>(std::llround(kProbeEdgeFadeSeconds * sampleRate)));

  for (int i = 0; i < sweepSamples; ++i) {
    const double t = i / sampleRate;
    const double phase = K * (std::exp(t / T * logRatio) - 1.0);
    float s = kProbeAmplitude * static_cast<float>(std::sin(phase));

    if (i < fadeSamples) {
      s *= 0.5f - 0.5f * std::cos(juce::MathConstants<float>::pi * i / fadeSamples);
    } else if (i >= sweepSamples - fadeSamples) {
      const int j = sweepSamples - 1 - i;
      s *= 0.5f - 0.5f * std::cos(juce::MathConstants<float>::pi * j / fadeSamples);
    }
    p[i] = s;
  }
}

void AutoOffset::arm() {
  if (state() != State::Idle)
    return;

  probePos = 0;
  muteStep = -1.0f / std::max(1.0f, static_cast<float>(kMuteFadeSeconds * sampleRate));
  rampStep = 1.0f / std::max(1.0f, static_cast<float>(kRampBackSeconds * sampleRate));
  // The release-store publishes the reset cursor together with the state
  // flip; the audio thread only touches it once it observes FadeOut.
  written.store(0, std::memory_order_relaxed);
  setState(State::FadeOut);
}

void AutoOffset::cancel() {
  if (state() == State::Idle)
    return;
  // Redirect to the ramp from wherever we are; applyOutputGain finishes the
  // transition to Idle (immediately if the gain never left 1).
  setState(State::RampBack);
}

void AutoOffset::resume() {
  if (state() == State::Analyzing)
    setState(State::RampBack);
}

bool AutoOffset::renderProbeInput(float* probeOut, int numSamples) {
  const State s = state();

  // FadeOut only ramps the plugin's own OUTPUT gain down (applyOutputGain);
  // left alone, the chains being measured would keep chewing on whatever
  // real audio was flowing right up until the sweep starts. For stateful
  // models (NAM's recurrent/dilated-conv path) that leftover history biases
  // the two sides' transient response differently, which the cross-
  // correlation reads as a spurious secondary peak - occasionally strong
  // enough to beat the true near-zero-lag peak, with an essentially random
  // polarity sign. Feeding silence here (paired with the caller resetting
  // both sides' model state to a matching baseline before arming - see
  // TONE3000Processor::armAutoOffsetFor) gives both chains the same known
  // starting point before the sweep proper begins.
  if (s == State::FadeOut) {
    std::fill(probeOut, probeOut + numSamples, 0.0f);
    return true;
  }

  if (s != State::Probing && s != State::Tail)
    return false;

  const float* sweep = probeBuffer.getReadPointer(0);
  for (int i = 0; i < numSamples; ++i) {
    probeOut[i] = probePos < sweepSamples ? sweep[probePos] : 0.0f;
    ++probePos;
  }

  if (s == State::Probing && probePos >= sweepSamples)
    setState(State::Tail);
  return true;
}

void AutoOffset::captureChainOutputs(const float* chainL, const float* chainR, int numSamples) {
  const State s = state();
  if (s != State::Probing && s != State::Tail)
    return;

  const int have = written.load(std::memory_order_relaxed);
  const int take = std::min(numSamples, capacity - have);
  if (take <= 0)
    return;

  captureBuffer.copyFrom(0, have, chainL, take);
  captureBuffer.copyFrom(1, have, chainR, take);
  written.store(have + take, std::memory_order_relaxed);

  if (have + take >= capacity)
    setState(State::Captured);
}

void AutoOffset::applyOutputGain(juce::AudioBuffer<float>& output) {
  const State s = state();
  if (s == State::Idle) {
    outputGain = 1.0f;
    return;
  }

  const int n = output.getNumSamples();
  const int channels = output.getNumChannels();

  if (s == State::FadeOut) {
    for (int i = 0; i < n; ++i) {
      outputGain = std::max(0.0f, outputGain + muteStep);
      for (int c = 0; c < channels; ++c)
        output.getWritePointer(c)[i] *= outputGain;
    }
    if (outputGain <= 0.0f)
      setState(State::Probing);
    return;
  }

  if (s == State::RampBack) {
    for (int i = 0; i < n; ++i) {
      outputGain = std::min(1.0f, outputGain + rampStep);
      for (int c = 0; c < channels; ++c)
        output.getWritePointer(c)[i] *= outputGain;
    }
    if (outputGain >= 1.0f)
      setState(State::Idle);
    return;
  }

  // Probing / Tail / Captured / Analyzing: hard mute. Captured/Analyzing
  // stay muted on purpose: the live signal is back in the chains
  // (re-settling their state), but nothing is audible until resume().
  output.clear();
  outputGain = 0.0f;
}

float AutoOffset::progress() const {
  return capacity > 0
             ? juce::jlimit(0.0f, 1.0f, static_cast<float>(written.load(std::memory_order_relaxed)) /
                                            static_cast<float>(capacity))
             : 0.0f;
}

AutoOffset::Result AutoOffset::analyze() {
  Result result;
  if (state() != State::Captured) {
    cancel();
    return result;
  }
  setState(State::Analyzing);

  const int n = written.load(std::memory_order_relaxed);
  const int maxLag = static_cast<int>(std::ceil(kMaxLagMs * 0.001 * sampleRate));
  const float* l = captureBuffer.getReadPointer(0);
  const float* r = captureBuffer.getReadPointer(1);

  // Generalized cross-correlation via FFT: c = IFFT(W * FFT(L) * conj(FFT(R))),
  // where c[k] = sum_n L[n]*R[n-k]. A peak at positive k means L is a delayed
  // copy of R (the left chain lags) -> delay the right chain -> positive ms,
  // matching the StereoOffset sign convention. Zero-padding past n + maxLag
  // keeps circular wrap-around out of the searched window; negative lags
  // live at indices fftSize - k. One-shot on the message thread, so
  // allocating here is fine.
  const int fftOrder = static_cast<int>(std::ceil(std::log2(std::max(2, n + maxLag))));
  const int fftSize = 1 << fftOrder;
  juce::dsp::FFT fft(fftOrder);

  std::vector<float> specL(static_cast<size_t>(fftSize) * 2, 0.0f);
  std::vector<float> specR(static_cast<size_t>(fftSize) * 2, 0.0f);
  std::copy(l, l + n, specL.begin());
  std::copy(r, r + n, specR.begin());
  fft.performRealOnlyForwardTransform(specL.data());
  fft.performRealOnlyForwardTransform(specR.data());

  // Cross-spectrum with soft PHAT weighting (see the header): normalizing
  // each bin by |X|^rho whitens away the chains' voicing differences so the
  // peak approaches a delta at the true lag; rho < 1 keeps near-empty bins
  // from being amplified into noise.
  constexpr float eps = 1.0e-12f;
  for (int bin = 0; bin < fftSize; ++bin) {
    const float ar = specL[static_cast<size_t>(2 * bin)];
    const float ai = specL[static_cast<size_t>(2 * bin) + 1];
    const float br = specR[static_cast<size_t>(2 * bin)];
    const float bi = specR[static_cast<size_t>(2 * bin) + 1];
    const float xr = ar * br + ai * bi;
    const float xi = ai * br - ar * bi;
    const float mag = std::sqrt(xr * xr + xi * xi);
    const float w = 1.0f / std::max(std::pow(mag, kPhatRho), eps);
    specL[static_cast<size_t>(2 * bin)] = xr * w;
    specL[static_cast<size_t>(2 * bin) + 1] = xi * w;
  }

  // Keep the whitened cross-spectrum: the sub-sample refinement below
  // evaluates its inverse DFT at fractional lags.
  const std::vector<float> crossSpec(specL);

  fft.performRealOnlyInverseTransform(specL.data());

  auto corrAt = [&](int lag) -> float {
    return specL[static_cast<size_t>(lag >= 0 ? lag : fftSize + lag)];
  };

  // PHAT peak search: finds where the two sides PLAUSIBLY line up, immune
  // to their voicing difference. bestLagPhat/bestAbsPhat is its own global
  // argmax, kept only as the "is there any real feature here at all"
  // reference below - the final pick is decided differently (next block).
  int bestLagPhat = 0;
  float bestAbsPhat = std::abs(corrAt(0));
  for (int lag = -maxLag; lag <= maxLag; ++lag) {
    const float a = std::abs(corrAt(lag));
    if (a > bestAbsPhat) {
      bestAbsPhat = a;
      bestLagPhat = lag;
    }
  }

  // Raw time-domain agreement at an integer lag - what actually determines
  // constructive vs. destructive summing when the two sides play together,
  // the same thing an ear or a correlation meter reads. PHAT's per-bin
  // equalization is excellent at surfacing CANDIDATE lags immune to
  // voicing differences, but its aggregate magnitude/sign ranks candidates
  // by phase-cleanliness, not by how much they actually agree - confirmed
  // in the field: PHAT's global argmax kept winning over a nearby candidate
  // that raw correlation (and a live correlation meter) clearly preferred,
  // and the winning lag itself drifted between otherwise-identical runs.
  // So: let PHAT propose every local maximum as a candidate, but let the
  // RAW correlation decide which one is real.
  auto rawDot = [&](int lag) -> double {
    double d = 0.0;
    const int from = std::max(0, lag);
    const int to = std::min(n, n + lag);
    for (int i = from; i < to; ++i)
      d += static_cast<double>(l[i]) * r[i - lag];
    return d;
  };
  auto rawConfidenceAt = [&](int lag) -> double {
    double eL = 0.0, eR = 0.0;
    const int from = std::max(0, lag);
    const int to = std::min(n, n + lag);
    for (int i = from; i < to; ++i) {
      eL += static_cast<double>(l[i]) * l[i];
      eR += static_cast<double>(r[i - lag]) * r[i - lag];
    }
    const double denom = std::sqrt(std::max(eL * eR, 1.0e-24));
    return std::abs(rawDot(lag)) / denom;
  };

  int bestLag = bestLagPhat;
  double bestRawConfidence = rawConfidenceAt(bestLagPhat);
  for (int lag = -maxLag; lag <= maxLag; ++lag) {
    const float a = std::abs(corrAt(lag));
    const float prev = std::abs(corrAt(lag > -maxLag ? lag - 1 : lag));
    const float next = std::abs(corrAt(lag < maxLag ? lag + 1 : lag));
    if (a < prev || a < next)
      continue;  // not a local maximum
    if (a < 0.25f * bestAbsPhat)
      continue;  // too weak in the voicing-immune metric to be plausible
    const double conf = rawConfidenceAt(lag);
    if (conf > bestRawConfidence) {
      bestRawConfidence = conf;
      bestLag = lag;
    }
  }
  const bool inverted = rawDot(bestLag) < 0.0;

  // Peak sharpness stays about bestLagPhat/bestAbsPhat, NOT the raw-
  // confidence-chosen bestLag: its job is "is this capture healthy at all"
  // (a spliced/broken/out-of-range capture reads flat everywhere, PHAT
  // magnitude included), independent of which specific candidate raw
  // confidence ends up preferring. Gating it on bestLag instead would
  // reject exactly the cases this redesign exists for: raw confidence
  // deliberately overriding PHAT's own (voicing-biased) favorite lands on
  // a candidate that isn't PHAT's global max by construction, so measuring
  // sharpness against that lag instead of PHAT's own argmax would call a
  // strong, genuine, high-raw-confidence match "unsharp" and throw it away
  // (observed in the field: confidence 0.734, a clearly dominant positive
  // peak - rejected anyway because a stronger PHAT peak sat elsewhere).
  const int guard = static_cast<int>(std::llround(0.001 * sampleRate));
  float secondAbs = 0.0f;
  for (int lag = -maxLag; lag <= maxLag; ++lag) {
    if (std::abs(lag - bestLagPhat) > guard)
      secondAbs = std::max(secondAbs, std::abs(corrAt(lag)));
  }
  result.peakSharpness = bestAbsPhat / std::max(secondAbs, eps);

  // Diagnostic: the whole local-maxima landscape, not just the winner - see
  // Result::debugPeaks. A local max of |corrAt| beats both its immediate
  // neighbors; edges of the window are checked against their one neighbor.
  {
    struct Peak {
      int lag;
      float value;
    };
    std::vector<Peak> peaks;
    for (int lag = -maxLag; lag <= maxLag; ++lag) {
      const float v = corrAt(lag);
      const float a = std::abs(v);
      const float prev = std::abs(corrAt(lag > -maxLag ? lag - 1 : lag));
      const float next = std::abs(corrAt(lag < maxLag ? lag + 1 : lag));
      if (a >= prev && a >= next)
        peaks.push_back({lag, v});
    }
    std::sort(peaks.begin(), peaks.end(),
             [](const Peak& x, const Peak& y) { return std::abs(x.value) > std::abs(y.value); });
    juce::String s;
    for (size_t i = 0; i < peaks.size() && i < 5; ++i) {
      if (i > 0)
        s << ", ";
      s << juce::String(peaks[i].lag * 1000.0 / sampleRate, 2) << "ms:"
        << (peaks[i].value < 0.0f ? "-" : "+")
        << juce::String(std::abs(peaks[i].value) / bestAbsPhat, 2)
        << (peaks[i].lag == bestLag ? "*" : "");
    }
    result.debugPeaks = s;
  }

  // Sub-sample refinement: evaluate the whitened cross-spectrum's inverse
  // DFT (exact band-limited interpolation of the correlation) on a fine
  // grid of +-1 sample around the integer peak, then parabolic-fit on the
  // fine grid, where the parabola's position-dependent bias is negligible.
  // Upper-half bins are the negative frequencies: at integer lags treating
  // them as +bin aliases invisibly (phases wrap by whole turns), but at
  // fractional lags they must be evaluated at their signed frequency or the
  // estimate gets pulled toward the nearest integer.
  auto corrFrac = [&](double tau) -> double {
    double acc = 0.0;
    const double w0 = 2.0 * juce::MathConstants<double>::pi * tau / fftSize;
    for (int bin = 0; bin < fftSize; ++bin) {
      const double xr = crossSpec[static_cast<size_t>(2 * bin)];
      const double xi = crossSpec[static_cast<size_t>(2 * bin) + 1];
      const double th = w0 * (bin <= fftSize / 2 ? bin : bin - fftSize);
      acc += xr * std::cos(th) - xi * std::sin(th);
    }
    return inverted ? -acc : acc;
  };

  const double step = 1.0 / kFineStepsPerSample;
  double fineTau = bestLag;
  double fineVal = corrFrac(fineTau);
  for (double tau = bestLag - 1.0; tau <= bestLag + 1.0 + 1e-9; tau += step) {
    const double v = corrFrac(tau);
    if (v > fineVal) {
      fineVal = v;
      fineTau = tau;
    }
  }
  {
    const double ym = corrFrac(fineTau - step);
    const double yp = corrFrac(fineTau + step);
    const double denom = ym - 2.0 * fineVal + yp;
    if (std::abs(denom) > 1e-20)
      fineTau += juce::jlimit(-0.5, 0.5, 0.5 * (ym - yp) / denom) * step;
  }

  // Confidence: normalized correlation at the (rounded) winning lag,
  // recomputed in the time domain on the raw captures, immune to FFT
  // scaling and the PHAT weighting. Its magnitude is the confidence (1 =
  // identical up to gain, shift and polarity); the sign was the polarity
  // verdict above.
  const int k = static_cast<int>(std::llround(fineTau));
  double dot = 0.0, energyL = 0.0, energyR = 0.0;
  const int from = std::max(0, k);
  const int to = std::min(n, n + k);
  for (int i = from; i < to; ++i) {
    dot += static_cast<double>(l[i]) * r[i - k];
    energyL += static_cast<double>(l[i]) * l[i];
    energyR += static_cast<double>(r[i - k]) * r[i - k];
  }
  const double denom = std::sqrt(std::max(energyL * energyR, 1.0e-24));

  result.offsetMs = juce::jlimit(-kMaxLagMs, kMaxLagMs,
                                 static_cast<float>(fineTau * 1000.0 / sampleRate));
  // Authoritative polarity verdict: the raw dot product's own sign at the
  // final refined lag (already computed above for confidence) - see the
  // comment by rawDotSignAt. Almost always agrees with the seed used to
  // steer the sub-sample search; when it doesn't (the seed seeded on the
  // integer lag, this reads the fractional one), this is the one that
  // ships.
  result.inverted = dot < 0.0;
  result.confidence = juce::jlimit(0.0f, 1.0f, static_cast<float>(std::abs(dot) / denom));

  // Auto Balance: same energyL/energyR this confidence score already
  // computed above, read as a loudness ratio instead - a second consumer
  // of the one capture, not a second probe (see Result::gainDeltaDb).
  const int measured = to - from;
  const double meanSqL = measured > 0 ? energyL / measured : 0.0;
  const double meanSqR = measured > 0 ? energyR / measured : 0.0;
  if (meanSqL < kAutoBalanceSilenceFloor || meanSqR < kAutoBalanceSilenceFloor) {
    result.silent = true;
  } else {
    result.boostRight = meanSqR < meanSqL;
    const double ratio = std::max(meanSqL, meanSqR) / std::min(meanSqL, meanSqR);
    result.gainDeltaDb = static_cast<float>(10.0 * std::log10(ratio));
  }
  return result;
}
