#include "NamEngine.h"
#include "NAM/slimmable.h"
#include "RtWorkerPool.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <stdexcept>

NamEngine::NamEngine(std::vector<std::unique_ptr<nam::DSP>> phaseInstances, int factor,
                     std::vector<std::unique_ptr<nam::DSP>> rightPhaseInstances)
    : instances(std::move(phaseInstances)),
      rightInstances(std::move(rightPhaseInstances)),
      oversampleFactor(factor) {
  if (instances.empty())
    throw std::invalid_argument("NamEngine needs at least one model instance");
  for (const auto& instance : instances) {
    if (instance == nullptr)
      throw std::invalid_argument("NAM model cannot be null");
  }
  for (const auto& instance : rightInstances) {
    if (instance == nullptr)
      throw std::invalid_argument("NAM model cannot be null");
  }
  if (!rightInstances.empty() && rightInstances.size() != instances.size())
    throw std::invalid_argument("NamEngine right set must match the left instance count");
  const auto count = static_cast<int>(instances.size());
  if (factor < 1 || (count != 1 && count != factor))
    throw std::invalid_argument("NamEngine instance count must be 1 or the oversampling factor");

  // Informational only; the chain domain runs the model at the chain rate
  // regardless (the loader logs a warning on mismatch).
  try {
    modelSampleRate = primary().GetExpectedSampleRate();
  } catch (...) {
    modelSampleRate = kChainBaseSampleRate;
  }
}

void NamEngine::prepare(int newMaxBlockSize) {
  maxBlockSize = newMaxBlockSize;

  const auto count = instances.size();
  // +1: a carried phase offset can hand one phase an extra frame when a
  // defensive slice isn't divisible by the phase count.
  const size_t perPhaseCapacity = static_cast<size_t>(maxBlockSize) / count + 1;
  phaseInputs.assign(count, std::vector<double>(perPhaseCapacity));
  phaseOutputs.assign(count, std::vector<double>(perPhaseCapacity));
  const auto rightCount = rightInstances.size();
  rightPhaseInputs.assign(rightCount, std::vector<double>(perPhaseCapacity));
  rightPhaseOutputs.assign(rightCount, std::vector<double>(perPhaseCapacity));
  phaseFrames.assign(count, 0);
  phaseOffset = 0;

  const double chainRate = kChainBaseSampleRate * oversampleFactor;
  rightHoldFrames = static_cast<int>(kRightHoldSeconds * chainRate);
  rightHoldRemaining = 0;
  rightBlend = 0.0f;
  rightBlendStep = static_cast<float>(1.0 / (kRightFadeSeconds * chainRate));

  const auto prepareInstance = [&](nam::DSP& instance) {
    instance.ResetAndPrewarm(instanceSampleRate(), static_cast<int>(perPhaseCapacity));

    // SlimmableContainer / SlimmableWavenet: after max buffer size is set on the
    // root DSP, SetSlimmableSize re-selects the active tier and ResetAndPrewarms
    // it (see NAM render tools and NeuralAmpModelerCore tests). Without this,
    // container models can stay on a stale or uninitialized sub-path.
    if (auto* slimmable = dynamic_cast<nam::SlimmableModel*>(&instance))
      slimmable->SetSlimmableSize(requestedSlimmableSize);
  };
  for (auto& instance : instances)
    prepareInstance(*instance);
  for (auto& instance : rightInstances)
    prepareInstance(*instance);

  isPrepared = true;
}

void NamEngine::resetState() {
  if (!isPrepared)
    return;

  const int perPhaseCapacity =
      phaseInputs.empty() ? 0 : static_cast<int>(phaseInputs.front().size());
  for (auto& instance : instances)
    instance->ResetAndPrewarm(instanceSampleRate(), perPhaseCapacity);
  for (auto& instance : rightInstances)
    instance->ResetAndPrewarm(instanceSampleRate(), perPhaseCapacity);

  std::fill(phaseFrames.begin(), phaseFrames.end(), 0);
  phaseOffset = 0;
  rightHoldRemaining = 0;
  rightBlend = 0.0f;
}

void NamEngine::setSlimmableSize(double val) {
  requestedSlimmableSize = juce::jlimit(0.0, 1.0, val);
  if (!isPrepared)
    return;
  for (auto* set : {&instances, &rightInstances}) {
    for (auto& instance : *set) {
      if (auto* slimmable = dynamic_cast<nam::SlimmableModel*>(instance.get()))
        slimmable->SetSlimmableSize(requestedSlimmableSize);
    }
  }
}

void NamEngine::process(juce::AudioBuffer<float>& buffer, RtWorkerPool* pool) {
  if (!isPrepared || maxBlockSize < 1) {
    throw std::runtime_error("NamEngine must be prepared before processing");
  }

  const int numSamples = buffer.getNumSamples();
  const int numChannels = buffer.getNumChannels();
  const int count = static_cast<int>(instances.size());

  // NAM models are mono: channel 0 runs through the left set, channel 1
  // through the right set while the two differ (see the header comment).
  // Buffers larger than the prepared size are run in prepared-size slices
  // (models stream statefully, so slicing is exact); throwing here would
  // permanently disable the block when a startup prepare races the host's
  // actual block size.
  float* leftChannel = buffer.getWritePointer(0);
  float* rightChannel =
      numChannels > 1 && !rightInstances.empty() ? buffer.getWritePointer(1) : nullptr;

  if (rightChannel != nullptr) {
    bool channelsDiffer = false;
    for (int i = 0; i < numSamples && !channelsDiffer; ++i)
      channelsDiffer = std::abs(leftChannel[i] - rightChannel[i]) > kChannelsDifferThreshold;
    rightHoldRemaining =
        channelsDiffer ? rightHoldFrames : std::max(0, rightHoldRemaining - numSamples);
  }
  const bool rightWanted = rightChannel != nullptr && rightHoldRemaining > 0;
  const bool rightRuns = rightChannel != nullptr && (rightWanted || rightBlend > 0.0f);

  for (int offset = 0; offset < numSamples; offset += maxBlockSize) {
    const int chunk = juce::jmin(maxBlockSize, numSamples - offset);

    // Deinterleave into the per-phase streams (single instance = one phase =
    // a straight copy). This doubles as the float → double conversion.
    std::fill(phaseFrames.begin(), phaseFrames.end(), 0);
    int phase = phaseOffset;
    for (int i = 0; i < chunk; ++i) {
      const auto frame = static_cast<size_t>(phaseFrames[static_cast<size_t>(phase)]++);
      phaseInputs[static_cast<size_t>(phase)][frame] =
          static_cast<double>(leftChannel[offset + i]);
      if (rightRuns)
        rightPhaseInputs[static_cast<size_t>(phase)][frame] =
            static_cast<double>(rightChannel[offset + i]);
      if (++phase == count)
        phase = 0;
    }

    // One job per instance that has work: each touches only its own model
    // and buffers, so any schedule produces the same bits as the serial
    // loop below.
    struct PhaseJob {
      nam::DSP* model;
      NAM_SAMPLE* input;
      NAM_SAMPLE* output;
      int frames;
      std::atomic<bool>* failed;
    };
    std::atomic<bool> phaseFailed{false};
    jassert(count * 2 <= RtWorkerPool::kMaxJobs);  // count == oversample factor <= 8
    PhaseJob jobs[RtWorkerPool::kMaxJobs];
    void* ctxs[RtWorkerPool::kMaxJobs];
    int jobCount = 0;
    const auto addJob = [&](nam::DSP& model, std::vector<double>& input,
                            std::vector<double>& output, int frames) {
      if (frames <= 0)
        return;
      jobs[jobCount] = {&model, input.data(), output.data(), frames, &phaseFailed};
      ctxs[jobCount] = &jobs[jobCount];
      ++jobCount;
    };
    for (int p = 0; p < count; ++p) {
      const auto i = static_cast<size_t>(p);
      addJob(*instances[i], phaseInputs[i], phaseOutputs[i], phaseFrames[i]);
      if (rightRuns)
        addJob(*rightInstances[i], rightPhaseInputs[i], rightPhaseOutputs[i], phaseFrames[i]);
    }

    if (pool != nullptr && jobCount > 1) {
      // Fork across the pool (see the header comment). Exceptions can't
      // cross threads, so a job traps its own and the joiner rethrows once
      // all are done; the caller's RT failure handling (disable the block,
      // log off-thread) works exactly as for a serial throw.
      pool->forkJoin(
          [](void* ctx) {
            auto& j = *static_cast<PhaseJob*>(ctx);
            try {
              NAM_SAMPLE* inputPtrs[] = {j.input};
              NAM_SAMPLE* outputPtrs[] = {j.output};
              j.model->process(inputPtrs, outputPtrs, j.frames);
            } catch (...) {
              j.failed->store(true, std::memory_order_release);
            }
          },
          ctxs, jobCount);
      if (phaseFailed.load(std::memory_order_acquire))
        throw std::runtime_error("NAM phase processing failed");
    } else {
      for (int j = 0; j < jobCount; ++j) {
        NAM_SAMPLE* inputPtrs[] = {jobs[j].input};
        NAM_SAMPLE* outputPtrs[] = {jobs[j].output};
        jobs[j].model->process(inputPtrs, outputPtrs, jobs[j].frames);
      }
    }

    // Reinterleave (and convert back to float). Reuse phaseFrames as read
    // cursors by counting back up from zero.
    std::fill(phaseFrames.begin(), phaseFrames.end(), 0);
    phase = phaseOffset;
    for (int i = 0; i < chunk; ++i) {
      const auto frame = static_cast<size_t>(phaseFrames[static_cast<size_t>(phase)]++);
      const auto left = static_cast<float>(phaseOutputs[static_cast<size_t>(phase)][frame]);
      leftChannel[offset + i] = left;
      if (rightRuns) {
        const auto right =
            static_cast<float>(rightPhaseOutputs[static_cast<size_t>(phase)][frame]);
        rightBlend = rightWanted ? std::min(1.0f, rightBlend + rightBlendStep)
                                 : std::max(0.0f, rightBlend - rightBlendStep);
        rightChannel[offset + i] =
            rightBlend >= 1.0f ? right : left + (right - left) * rightBlend;
      }
      if (++phase == count)
        phase = 0;
    }

    phaseOffset = (phaseOffset + chunk) % count;
  }

  // ...and fan the result out to any channel without its own run.
  for (int ch = rightRuns ? 2 : 1; ch < numChannels; ++ch) {
    buffer.copyFrom(ch, 0, buffer, 0, 0, numSamples);
  }
}
