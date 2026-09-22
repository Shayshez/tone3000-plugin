#include "BlockEq.h"
#include <cmath>

BlockEq::BandRole BlockEq::roleForIndex(int index) {
  if (index == 0) return BandRole::LowCut;
  if (index == 1) return BandRole::LowShelf;
  if (index == kNumBands - 2) return BandRole::HighShelf;
  if (index == kNumBands - 1) return BandRole::HighCut;
  return BandRole::Bell;
}

int BlockEq::snapPoles(int requested) {
  int best = kPoleOptions[0];
  int bestDiff = std::abs(requested - best);
  for (int option : kPoleOptions) {
    const int diff = std::abs(requested - option);
    if (diff < bestDiff) {
      best = option;
      bestDiff = diff;
    }
  }
  return best;
}

std::array<BlockEq::Band, BlockEq::kNumBands> BlockEq::defaultBands() {
  return {{
      // Low/High Cut start OFF (Band::on = false) rather than parked at the
      // range edge - being off is now an explicit per-band choice, so the
      // freq can sit at a musically useful spot for whenever the user
      // switches it on instead of at a meaningless extreme.
      {80.0f, 0.0f, 0.71f, 4, false},   // Low Cut
      {100.0f, 0.0f, 0.71f, 4},         // Low Shelf
      {250.0f, 0.0f, 1.0f, 4},          // Bell
      {650.0f, 0.0f, 1.0f, 4},          // Bell
      {1600.0f, 0.0f, 1.0f, 4},         // Bell
      {3500.0f, 0.0f, 1.4f, 4},         // Bell
      {8000.0f, 0.0f, 0.71f, 4},        // High Shelf
      {12000.0f, 0.0f, 0.71f, 4, false},// High Cut
  }};
}

BlockEq::BlockEq() : bands(defaultBands()) {
  updateActivity();
}

void BlockEq::prepare(double newSampleRate) {
  sampleRate = newSampleRate > 0.0 ? newSampleRate : 48000.0;
  for (int i = 0; i < kNumBands; ++i) {
    const BandRole role = roleForIndex(i);
    if (role == BandRole::LowCut || role == BandRole::HighCut)
      updateCutBand(i);
    else
      updateBand(i);
    for (auto& stage : stages[static_cast<size_t>(i)])
      stage.resetState();
  }
  updateActivity();
}

bool BlockEq::setBand(int index, const Band& band) {
  if (index < 0 || index >= kNumBands)
    return false;
  bands[static_cast<size_t>(index)] = clampBand(band);
  const BandRole role = roleForIndex(index);
  if (role == BandRole::LowCut || role == BandRole::HighCut)
    updateCutBand(index);
  else
    updateBand(index);

  const bool wasActive = anyBandActive;
  updateActivity();
  // Coming back from the flat-skip path: filter state is stale, clear it so
  // the first processed block doesn't ring with old history.
  if (!wasActive && anyBandActive)
    for (auto& bandStages : stages)
      for (auto& stage : bandStages)
        stage.resetState();
  return true;
}

bool BlockEq::setBandFromVar(int index, const juce::var& bandVar) {
  if (!bandVar.isObject())
    return false;
  Band band;
  band.freqHz = static_cast<float>(static_cast<double>(bandVar.getProperty("freqHz", 1000.0)));
  band.gainDb = static_cast<float>(static_cast<double>(bandVar.getProperty("gainDb", 0.0)));
  band.q = static_cast<float>(static_cast<double>(bandVar.getProperty("q", 1.0)));
  band.poles = static_cast<int>(bandVar.getProperty("poles", 4));
  band.on = static_cast<bool>(bandVar.getProperty("on", true));
  return setBand(index, band);
}

void BlockEq::resetToDefault() {
  bands = defaultBands();
  enabled = true;
  pre = false;
  for (int i = 0; i < kNumBands; ++i) {
    const BandRole role = roleForIndex(i);
    if (role == BandRole::LowCut || role == BandRole::HighCut)
      updateCutBand(i);
    else
      updateBand(i);
    for (auto& stage : stages[static_cast<size_t>(i)])
      stage.resetState();
  }
  updateActivity();
}

void BlockEq::setEnabled(bool shouldBeEnabled) {
  // Re-engaging after a bypass: filter state is stale, clear it so the first
  // processed block doesn't ring with old history.
  if (!enabled && shouldBeEnabled)
    for (auto& bandStages : stages)
      for (auto& stage : bandStages)
        stage.resetState();
  enabled = shouldBeEnabled;
}

void BlockEq::setPre(bool shouldBePre) {
  // Moving position mid-signal: the filters hold history from the other tap
  // point, clear it so the first processed block doesn't ring.
  if (pre != shouldBePre)
    for (auto& bandStages : stages)
      for (auto& stage : bandStages)
        stage.resetState();
  pre = shouldBePre;
}

void BlockEq::process(juce::AudioBuffer<float>& buffer) {
  const int numSamples = buffer.getNumSamples();
  const int numChannels = juce::jmin(buffer.getNumChannels(), 2);

  for (int b = 0; b < kNumBands; ++b) {
    if (!bandActive[static_cast<size_t>(b)])
      continue;
    const int stageCount = numStages[static_cast<size_t>(b)];
    for (int s = 0; s < stageCount; ++s) {
      auto& filter = stages[static_cast<size_t>(b)][static_cast<size_t>(s)];
      for (int ch = 0; ch < numChannels; ++ch) {
        auto* data = buffer.getWritePointer(ch);
        for (int i = 0; i < numSamples; ++i)
          data[i] = filter.processSample(data[i], ch);
      }
    }
  }
}

bool BlockEq::isBandActive(int index, const Band& band) {
  if (!band.on)
    return false;
  switch (roleForIndex(index)) {
    case BandRole::Bell:
    case BandRole::LowShelf:
    case BandRole::HighShelf:
      return std::abs(band.gainDb) >= 0.05f;
    // A Cut band has no "trivial" gain setting to auto-detect the way
    // Shelf/Bell do - once it's on, it shapes the signal.
    case BandRole::LowCut:
    case BandRole::HighCut:
      return true;
  }
  return false;
}

BlockEq::Band BlockEq::clampBand(Band band) {
  band.freqHz = juce::jlimit(kMinFreqHz, kMaxFreqHz, band.freqHz);
  band.gainDb = juce::jlimit(-kMaxAbsGainDb, kMaxAbsGainDb, band.gainDb);
  band.q = juce::jlimit(kMinQ, kMaxQ, band.q);
  band.poles = snapPoles(band.poles);
  return band;
}

// RBJ Audio EQ Cookbook coefficients, A = 10^(dB/40). Keep in exact sync with
// the TypeScript mirror in ui/src/components/eqMath.ts.
void BlockEq::updateBand(int index) {
  const Band& band = bands[static_cast<size_t>(index)];
  Biquad& f = stages[static_cast<size_t>(index)][0];
  numStages[static_cast<size_t>(index)] = 1;
  bandActive[static_cast<size_t>(index)] = isBandActive(index, band);

  const double freq = juce::jlimit(static_cast<double>(kMinFreqHz),
                                   juce::jmin(static_cast<double>(kMaxFreqHz), sampleRate * 0.49),
                                   static_cast<double>(band.freqHz));
  const double A = std::pow(10.0, band.gainDb / 40.0);
  const double omega = 2.0 * juce::MathConstants<double>::pi * freq / sampleRate;
  const double sn = std::sin(omega);
  const double cs = std::cos(omega);
  const double alpha = sn / (2.0 * band.q);
  const double sqrtA = std::sqrt(A);

  double b0 = 1.0, b1 = 0.0, b2 = 0.0, a0 = 1.0, a1 = 0.0, a2 = 0.0;

  switch (roleForIndex(index)) {
    case BandRole::Bell:
      b0 = 1.0 + alpha * A;
      b1 = -2.0 * cs;
      b2 = 1.0 - alpha * A;
      a0 = 1.0 + alpha / A;
      a1 = -2.0 * cs;
      a2 = 1.0 - alpha / A;
      break;
    case BandRole::LowShelf:
      b0 = A * ((A + 1.0) - (A - 1.0) * cs + 2.0 * sqrtA * alpha);
      b1 = 2.0 * A * ((A - 1.0) - (A + 1.0) * cs);
      b2 = A * ((A + 1.0) - (A - 1.0) * cs - 2.0 * sqrtA * alpha);
      a0 = (A + 1.0) + (A - 1.0) * cs + 2.0 * sqrtA * alpha;
      a1 = -2.0 * ((A - 1.0) + (A + 1.0) * cs);
      a2 = (A + 1.0) + (A - 1.0) * cs - 2.0 * sqrtA * alpha;
      break;
    case BandRole::HighShelf:
      b0 = A * ((A + 1.0) + (A - 1.0) * cs + 2.0 * sqrtA * alpha);
      b1 = -2.0 * A * ((A - 1.0) + (A + 1.0) * cs);
      b2 = A * ((A + 1.0) + (A - 1.0) * cs - 2.0 * sqrtA * alpha);
      a0 = (A + 1.0) - (A - 1.0) * cs + 2.0 * sqrtA * alpha;
      a1 = 2.0 * ((A - 1.0) - (A + 1.0) * cs);
      a2 = (A + 1.0) - (A - 1.0) * cs - 2.0 * sqrtA * alpha;
      break;
    case BandRole::LowCut:
    case BandRole::HighCut:
      jassertfalse;  // handled by updateCutBand instead
      break;
  }

  const double norm = 1.0 / a0;
  f.b0 = static_cast<float>(b0 * norm);
  f.b1 = static_cast<float>(b1 * norm);
  f.b2 = static_cast<float>(b2 * norm);
  f.a1 = static_cast<float>(a1 * norm);
  f.a2 = static_cast<float>(a2 * norm);
}

// Cascaded Low/High Cut: `band.poles` poles = ceil(poles/2) biquad-family
// stages in series (a single first-order leftover section when poles is odd,
// plus one 2nd-order RBJ highpass/lowpass section per pole pair). Each
// 2nd-order section's Q follows the standard Butterworth pole-angle formula,
// scaled by band.q/0.7071 so the default Q reproduces a true maximally-flat
// response for any pole count and the knob adds/removes a resonant bump from
// there - verified against published Butterworth Q tables for N=4/6/8. Keep
// in exact sync with the TypeScript mirror in ui/src/components/eqMath.ts.
void BlockEq::updateCutBand(int index) {
  const Band& band = bands[static_cast<size_t>(index)];
  const bool isHighpass = roleForIndex(index) == BandRole::LowCut;
  bandActive[static_cast<size_t>(index)] = isBandActive(index, band);

  const double freq = juce::jlimit(static_cast<double>(kMinFreqHz),
                                   juce::jmin(static_cast<double>(kMaxFreqHz), sampleRate * 0.49),
                                   static_cast<double>(band.freqHz));
  const double omega = 2.0 * juce::MathConstants<double>::pi * freq / sampleRate;
  const double sn = std::sin(omega);
  const double cs = std::cos(omega);

  const int poles = band.poles;
  const int numSecondOrder = poles / 2;
  const bool hasFirstOrder = (poles % 2) != 0;
  auto& bandStages = stages[static_cast<size_t>(index)];
  int stageIdx = 0;

  if (hasFirstOrder) {
    // Bilinear-transformed first-order section (prewarped tan), no Q
    // dependence at all - a 6 dB/oct slope is just a single real pole.
    const double K = std::tan(omega * 0.5);
    double b0, b1, a1;
    if (isHighpass) {
      b0 = 1.0 / (1.0 + K);
      b1 = -b0;
      a1 = (K - 1.0) / (1.0 + K);
    } else {
      b0 = K / (1.0 + K);
      b1 = b0;
      a1 = (K - 1.0) / (1.0 + K);
    }
    Biquad& f = bandStages[static_cast<size_t>(stageIdx)];
    f.b0 = static_cast<float>(b0);
    f.b1 = static_cast<float>(b1);
    f.b2 = 0.0f;
    f.a1 = static_cast<float>(a1);
    f.a2 = 0.0f;
    ++stageIdx;
  }

  constexpr double kReferenceQ = 0.70710678118654752;  // 1/sqrt(2)
  const double qScale = static_cast<double>(band.q) / kReferenceQ;
  for (int k = 1; k <= numSecondOrder; ++k) {
    const double qBase =
        1.0 / (2.0 * std::sin((2.0 * k - 1.0) * juce::MathConstants<double>::pi /
                              (2.0 * poles)));
    const double alpha = sn / (2.0 * qBase * qScale);

    double b0, b1, b2;
    if (isHighpass) {
      b0 = (1.0 + cs) * 0.5;
      b1 = -(1.0 + cs);
      b2 = (1.0 + cs) * 0.5;
    } else {
      b0 = (1.0 - cs) * 0.5;
      b1 = 1.0 - cs;
      b2 = (1.0 - cs) * 0.5;
    }
    const double a0 = 1.0 + alpha;
    const double a1 = -2.0 * cs;
    const double a2 = 1.0 - alpha;

    const double norm = 1.0 / a0;
    Biquad& f = bandStages[static_cast<size_t>(stageIdx)];
    f.b0 = static_cast<float>(b0 * norm);
    f.b1 = static_cast<float>(b1 * norm);
    f.b2 = static_cast<float>(b2 * norm);
    f.a1 = static_cast<float>(a1 * norm);
    f.a2 = static_cast<float>(a2 * norm);
    ++stageIdx;
  }

  numStages[static_cast<size_t>(index)] = stageIdx;
}

void BlockEq::updateActivity() {
  anyBandActive = false;
  for (bool active : bandActive)
    anyBandActive = anyBandActive || active;
}

juce::var BlockEq::toVar() const {
  juce::Array<juce::var> bandArray;
  for (const auto& band : bands) {
    auto* obj = new juce::DynamicObject();
    obj->setProperty("freqHz", static_cast<double>(band.freqHz));
    obj->setProperty("gainDb", static_cast<double>(band.gainDb));
    obj->setProperty("q", static_cast<double>(band.q));
    obj->setProperty("poles", band.poles);
    obj->setProperty("on", band.on);
    bandArray.add(juce::var(obj));
  }
  auto* eq = new juce::DynamicObject();
  eq->setProperty("enabled", enabled);
  eq->setProperty("pre", pre);
  eq->setProperty("bands", bandArray);
  return juce::var(eq);
}

juce::ValueTree BlockEq::toValueTree() const {
  juce::ValueTree tree("Eq");
  tree.setProperty("enabled", enabled, nullptr);
  tree.setProperty("pre", pre, nullptr);
  for (const auto& band : bands) {
    juce::ValueTree bandTree("Band");
    bandTree.setProperty("freqHz", static_cast<double>(band.freqHz), nullptr);
    bandTree.setProperty("gainDb", static_cast<double>(band.gainDb), nullptr);
    bandTree.setProperty("q", static_cast<double>(band.q), nullptr);
    bandTree.setProperty("poles", band.poles, nullptr);
    bandTree.setProperty("on", band.on, nullptr);
    tree.appendChild(bandTree, nullptr);
  }
  return tree;
}

void BlockEq::restoreFromValueTree(const juce::ValueTree& tree) {
  bands = defaultBands();
  enabled = true;
  pre = false;
  // Anything that isn't exactly today's 8-band shape (including the old
  // 6-band/typed schema) is left at the flat default above rather than
  // remapped - the same "silently fold away" policy this codebase already
  // applies to other obsolete saved-state shapes.
  if (tree.isValid() && tree.hasType("Eq") && tree.getNumChildren() == kNumBands) {
    enabled = static_cast<bool>(tree.getProperty("enabled", true));
    pre = static_cast<bool>(tree.getProperty("pre", false));
    for (int i = 0; i < kNumBands; ++i) {
      const auto bandTree = tree.getChild(i);
      Band band;
      band.freqHz = static_cast<float>(static_cast<double>(bandTree.getProperty("freqHz", 1000.0)));
      band.gainDb = static_cast<float>(static_cast<double>(bandTree.getProperty("gainDb", 0.0)));
      band.q = static_cast<float>(static_cast<double>(bandTree.getProperty("q", 1.0)));
      band.poles = static_cast<int>(bandTree.getProperty("poles", 4));
      band.on = static_cast<bool>(bandTree.getProperty("on", true));
      bands[static_cast<size_t>(i)] = clampBand(band);
    }
  }
  for (int i = 0; i < kNumBands; ++i) {
    const BandRole role = roleForIndex(i);
    if (role == BandRole::LowCut || role == BandRole::HighCut)
      updateCutBand(i);
    else
      updateBand(i);
    for (auto& stage : stages[static_cast<size_t>(i)])
      stage.resetState();
  }
  updateActivity();
}
