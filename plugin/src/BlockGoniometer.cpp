#include "BlockGoniometer.h"

BlockGoniometer::BlockGoniometer() {
  ringL.resize(kRingSize, 0.0f);
  ringR.resize(kRingSize, 0.0f);
}

void BlockGoniometer::prepare(double sampleRate) {
  std::fill(ringL.begin(), ringL.end(), 0.0f);
  std::fill(ringR.begin(), ringR.end(), 0.0f);
  writePos.store(0, std::memory_order_relaxed);
  decimateCounter = 0;
  readPos = 0;
  corrMeter.prepare(sampleRate);
}

void BlockGoniometer::pushSamples(const float* l, const float* r, int numSamples) {
  int pos = writePos.load(std::memory_order_relaxed);
  float sumLR = 0.0f, sumLL = 0.0f, sumRR = 0.0f;
  for (int i = 0; i < numSamples; ++i) {
    sumLR += l[i] * r[i];
    sumLL += l[i] * l[i];
    sumRR += r[i] * r[i];
    if (decimateCounter == 0) {
      ringL[static_cast<size_t>(pos)] = l[i];
      ringR[static_cast<size_t>(pos)] = r[i];
      pos = (pos + 1) & (kRingSize - 1);
    }
    decimateCounter = (decimateCounter + 1) % kDecimation;
  }
  writePos.store(pos, std::memory_order_release);
  corrMeter.update(sumLR, sumLL, sumRR, numSamples);
}

juce::var BlockGoniometer::getPoints() {
  const int end = writePos.load(std::memory_order_acquire);
  int available = (end - readPos) & (kRingSize - 1);
  if (available <= 0)
    return juce::var(juce::Array<juce::var>());

  available = std::min(available, kMaxPointsPerPoll);
  const int start = (end - available) & (kRingSize - 1);
  readPos = end;

  juce::Array<juce::var> flat;
  flat.ensureStorageAllocated(available * 2);
  for (int i = 0; i < available; ++i) {
    const int idx = (start + i) & (kRingSize - 1);
    flat.add(static_cast<double>(ringL[static_cast<size_t>(idx)]));
    flat.add(static_cast<double>(ringR[static_cast<size_t>(idx)]));
  }
  return juce::var(flat);
}
