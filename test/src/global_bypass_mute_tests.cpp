// Global Bypass / Mute tests
//
// The header's Bypass and Mute buttons (APVTS bools `bypass` / `outputMute`),
// driven like a host would. These pin:
//
//   - bypass is the host's bypass parameter (getBypassParameter),
//   - a bypassed plugin outputs the dry input bit-exactly, delayed by exactly
//     the latency it reports (0 at 48 kHz, the boundary's PDC elsewhere), so
//     bypassing never shifts timing and the Output knob is bypassed too,
//   - mute is true digital silence, and toggling either one mid-stream
//     glides (no step) and settles on the target.
#include "Processor.h"
#include "test_helpers.h"

#include <gtest/gtest.h>

#include <vector>

namespace {

std::vector<float> run(TONE3000Processor& proc, const std::vector<float>& in, int blockSize) {
  std::vector<float> out(in.size(), 0.0f);
  juce::AudioBuffer<float> buffer(2, blockSize);
  juce::MidiBuffer midi;
  for (size_t off = 0; off < in.size(); off += static_cast<size_t>(blockSize)) {
    buffer.copyFrom(0, 0, in.data() + off, blockSize);
    buffer.copyFrom(1, 0, in.data() + off, blockSize);
    proc.processBlock(buffer, midi);
    std::copy(buffer.getReadPointer(0), buffer.getReadPointer(0) + blockSize, out.begin() + off);
  }
  return out;
}

void setBool(TONE3000Processor& proc, const char* id, bool on) {
  proc.parameters.getParameter(id)->setValueNotifyingHost(on ? 1.0f : 0.0f);
}

TEST(GlobalBypassMuteTest, BypassIsTheHostBypassParameter) {
  TONE3000Processor proc;
  ASSERT_NE(proc.getBypassParameter(), nullptr);
  EXPECT_EQ(proc.getBypassParameter(), proc.parameters.getParameter("bypass"));
}

TEST(GlobalBypassMuteTest, BypassPassesDryInputDelayedByReportedLatency) {
  for (double hostRate : {48000.0, 44100.0, 96000.0}) {
    TONE3000Processor proc;
    // A non-unity Output knob proves the whole plugin, output stage
    // included, is out of the path.
    proc.parameters.getParameter("outputLevel")->setValueNotifyingHost(0.9f);
    setBool(proc, "bypass", true);
    proc.setPlayConfigDetails(2, 2, hostRate, 512);
    proc.prepareToPlay(hostRate, 512);
    const int latency = proc.getLatencySamples();

    const auto in = makeNoise(40 * 512, 99, 0.25f);
    const auto out = run(proc, in, 512);
    for (int i = 0; i < latency; ++i)
      ASSERT_EQ(out[static_cast<size_t>(i)], 0.0f) << hostRate << " Hz, sample " << i;
    for (size_t i = static_cast<size_t>(latency); i < in.size(); ++i)
      ASSERT_EQ(out[i], in[i - static_cast<size_t>(latency)])
          << hostRate << " Hz: bypass not bit-exact dry at sample " << i;
  }
}

TEST(GlobalBypassMuteTest, MuteIsTrueSilenceAndTogglesGlide) {
  TONE3000Processor proc;
  setBool(proc, "outputMute", true);
  proc.setPlayConfigDetails(2, 2, kFs, 512);
  proc.prepareToPlay(kFs, 512);

  const auto in = makeSine(20 * 512, 997.0, 0.5f);
  for (float s : run(proc, in, 512))
    ASSERT_EQ(s, 0.0f);

  // Unmute: glides back up (first sample still ~silent), then full level.
  setBool(proc, "outputMute", false);
  const auto up = run(proc, in, 512);
  EXPECT_LT(std::abs(up[0]), 1e-3f);
  float tailPeak = 0.0f;
  for (size_t i = up.size() - 2048; i < up.size(); ++i)
    tailPeak = std::max(tailPeak, std::abs(up[i]));
  EXPECT_GT(tailPeak, 0.45f);

  // Re-mute mid-stream: settles on exact zero after the 20 ms glide.
  setBool(proc, "outputMute", true);
  const auto down = run(proc, in, 512);
  EXPECT_GT(std::abs(down[5]) + std::abs(down[6]) + std::abs(down[7]), 0.0f) << "mute stepped";
  for (size_t i = 2048; i < down.size(); ++i)
    ASSERT_EQ(down[i], 0.0f) << "mute not settled at sample " << i;
}

TEST(GlobalBypassMuteTest, BypassEngagesAndReleasesMidStream) {
  TONE3000Processor proc;
  proc.parameters.getParameter("outputLevel")->setValueNotifyingHost(0.2f);  // audibly quieter
  proc.setPlayConfigDetails(2, 2, kFs, 512);
  proc.prepareToPlay(kFs, 512);

  const auto in = makeNoise(20 * 512, 7, 0.25f);
  run(proc, in, 512);
  setBool(proc, "bypass", true);
  const auto out = run(proc, in, 512);
  // Settled bypass at 48 kHz: the input itself.
  for (size_t i = 2048; i < out.size(); ++i)
    ASSERT_EQ(out[i], in[i]) << "bypass not settled at sample " << i;
  // ...and it glided in rather than stepping on the first sample.
  EXPECT_NE(out[0], in[0]);
}

TEST(GlobalBypassMuteTest, TunerMuteSilencesButIsNeverSaved) {
  // "Mute while tuning" is transient: it silences like Mute but leaves the
  // user's Mute parameter alone and never rides the session state, so a
  // plugin closed with the tuner open doesn't reopen muted.
  TONE3000Processor proc;
  proc.setPlayConfigDetails(2, 2, kFs, 512);
  proc.prepareToPlay(kFs, 512);
  const auto in = makeSine(20 * 512, 997.0, 0.5f);
  run(proc, in, 512);

  proc.setTunerMute(true);
  const auto muted = run(proc, in, 512);
  for (size_t i = 2048; i < muted.size(); ++i)
    ASSERT_EQ(muted[i], 0.0f) << "tuner mute not silent at " << i;
  EXPECT_LT(proc.parameters.getParameter("outputMute")->getValue(), 0.5f)
      << "tuner mute must not flip the user's Mute parameter";

  // Saved while tuner-muted, restored into a fresh instance: not muted.
  juce::MemoryBlock state;
  proc.getStateInformation(state);
  TONE3000Processor restored;
  restored.setStateInformation(state.getData(), static_cast<int>(state.getSize()));
  restored.setPlayConfigDetails(2, 2, kFs, 512);
  restored.prepareToPlay(kFs, 512);
  EXPECT_FALSE(restored.isTunerMuteActive());
  float peak = 0.0f;
  const auto out = run(restored, in, 512);
  for (size_t i = 2048; i < out.size(); ++i) peak = std::max(peak, std::abs(out[i]));
  EXPECT_GT(peak, 0.1f) << "restored session came back muted";

  // Releasing it brings the sound back.
  proc.setTunerMute(false);
  const auto back = run(proc, in, 512);
  float backPeak = 0.0f;
  for (size_t i = 4096; i < back.size(); ++i) backPeak = std::max(backPeak, std::abs(back[i]));
  EXPECT_GT(backPeak, 0.1f);
}

}  // namespace
