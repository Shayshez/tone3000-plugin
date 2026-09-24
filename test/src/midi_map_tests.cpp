// MIDI mapping engine tests
//
// MidiMapper driven through the real processor (headless build), the way
// processBlock feeds it. These pin the mapping contracts that footswitch
// hardware depends on:
//
//   - learn captures a CC and the mapping round-trips through getState,
//   - setCcMapping assigns a typed CC without learn, validates its input,
//     and supersedes an armed learn on the same target,
//   - CC press detection: momentary switches (127 then 0) fire once per
//     stomp, and switches that only ever send low values still fire on
//     every press (the regression behind "CC works for notes but not CC"),
//   - note-ons always toggle,
//   - CC on a continuous parameter tracks absolutely,
//   - block-power targets deliver their position and lane (left / right),
//   - preset prev/next virtual targets deliver coalesced step deltas,
//   - program changes deliver their program number unmapped.
//
// The mapper defers non-parameter work to the message thread via
// AsyncUpdater, so these tests pump the dispatch loop (the test target
// builds with JUCE_MODAL_LOOPS_PERMITTED=1 for runDispatchLoopUntil).
#include "Processor.h"

#include <gtest/gtest.h>
#include <juce_events/juce_events.h>

namespace {

// Deliver pending AsyncUpdater callbacks (learn commits, preset steps).
void pumpMessages(int ms = 20) {
  juce::MessageManager::getInstance()->runDispatchLoopUntil(ms);
}

juce::MidiBuffer ccEvent(int number, int value, int channel = 1) {
  juce::MidiBuffer midi;
  midi.addEvent(juce::MidiMessage::controllerEvent(channel, number, value), 0);
  return midi;
}

juce::MidiBuffer noteOnEvent(int note, int channel = 1) {
  juce::MidiBuffer midi;
  midi.addEvent(juce::MidiMessage::noteOn(channel, note, (juce::uint8)100), 0);
  return midi;
}

// Arm learn for `targetId` and answer with the given event; the mapping
// exists once the async commit lands.
void learn(TONE3000Processor& proc, const juce::String& targetId, const juce::MidiBuffer& midi) {
  proc.midiMapper.startLearn(targetId);
  pumpMessages();
  proc.midiMapper.processMidi(midi);
  pumpMessages();
}

float paramValue(TONE3000Processor& proc, const char* id) {
  return proc.parameters.getRawParameterValue(id)->load();
}

TEST(MidiMapperTest, LearnCapturesCcAndRoundTripsThroughState) {
  TONE3000Processor proc;
  learn(proc, "gateEnabled", ccEvent(24, 0));

  const juce::var state = proc.midiMapper.getState();
  const auto* mappings = state["mappings"].getArray();
  ASSERT_NE(mappings, nullptr);
  ASSERT_EQ(mappings->size(), 1);
  EXPECT_EQ((*mappings)[0]["targetId"].toString(), "gateEnabled");
  EXPECT_EQ((*mappings)[0]["source"].toString(), "cc");
  EXPECT_EQ(static_cast<int>((*mappings)[0]["number"]), 24);
  EXPECT_TRUE(state["learnTargetId"].toString().isEmpty()) << "learn must disarm after capture";
}

TEST(MidiMapperTest, SetCcMappingAssignsWithoutLearn) {
  TONE3000Processor proc;
  ASSERT_TRUE(proc.midiMapper.setCcMapping("inputLevel", 11));

  const juce::var state = proc.midiMapper.getState();
  const auto* mappings = state["mappings"].getArray();
  ASSERT_NE(mappings, nullptr);
  ASSERT_EQ(mappings->size(), 1);
  EXPECT_EQ((*mappings)[0]["targetId"].toString(), "inputLevel");
  EXPECT_EQ((*mappings)[0]["source"].toString(), "cc");
  EXPECT_EQ(static_cast<int>((*mappings)[0]["number"]), 11);

  proc.midiMapper.processMidi(ccEvent(11, 127));
  EXPECT_NEAR(paramValue(proc, "inputLevel"),
              proc.parameters.getParameter("inputLevel")->convertFrom0to1(1.0f), 1e-4f);
}

TEST(MidiMapperTest, SetCcMappingValidatesAndSupersedesLearn) {
  TONE3000Processor proc;
  EXPECT_FALSE(proc.midiMapper.setCcMapping("inputLevel", 128));
  EXPECT_FALSE(proc.midiMapper.setCcMapping("inputLevel", -1));
  EXPECT_FALSE(proc.midiMapper.setCcMapping("noSuchTarget", 1));

  // Typing while the same target is listening replaces the learn: the map
  // holds the typed CC and the armed state clears.
  proc.midiMapper.startLearn("gateEnabled");
  pumpMessages();
  ASSERT_TRUE(proc.midiMapper.setCcMapping("gateEnabled", 24));
  pumpMessages();

  const juce::var state = proc.midiMapper.getState();
  EXPECT_TRUE(state["learnTargetId"].toString().isEmpty()) << "typed CC must disarm the learn";
  const float initial = paramValue(proc, "gateEnabled");
  proc.midiMapper.processMidi(ccEvent(24, 127));
  EXPECT_NE(paramValue(proc, "gateEnabled"), initial);
}

TEST(MidiMapperTest, LowValueOnlyCcFiresEveryPress) {
  // A footswitch programmed to send only low values (value 0 per press) must
  // toggle on every message; a plain "value ≥ 64" press gate drops all of
  // them, which is exactly the field report this pins down.
  TONE3000Processor proc;
  learn(proc, "gateEnabled", ccEvent(24, 0));

  const float initial = paramValue(proc, "gateEnabled");
  proc.midiMapper.processMidi(ccEvent(24, 0));
  EXPECT_NE(paramValue(proc, "gateEnabled"), initial) << "first low-value press dropped";
  proc.midiMapper.processMidi(ccEvent(24, 0));
  EXPECT_EQ(paramValue(proc, "gateEnabled"), initial) << "second low-value press dropped";
}

TEST(MidiMapperTest, MomentaryCcFiresOncePerStomp) {
  // A momentary switch sends 127 on press and 0 on release: the pair must
  // flip the target once, not twice, and the next stomp must fire again.
  TONE3000Processor proc;
  learn(proc, "gateEnabled", ccEvent(25, 127));

  const float initial = paramValue(proc, "gateEnabled");
  proc.midiMapper.processMidi(ccEvent(25, 127));
  const float afterPress = paramValue(proc, "gateEnabled");
  EXPECT_NE(afterPress, initial);
  proc.midiMapper.processMidi(ccEvent(25, 0));
  EXPECT_EQ(paramValue(proc, "gateEnabled"), afterPress) << "release must not re-toggle";

  proc.midiMapper.processMidi(ccEvent(25, 127));
  EXPECT_EQ(paramValue(proc, "gateEnabled"), initial);
  proc.midiMapper.processMidi(ccEvent(25, 0));
  EXPECT_EQ(paramValue(proc, "gateEnabled"), initial);
}

TEST(MidiMapperTest, NoteOnAlwaysToggles) {
  TONE3000Processor proc;
  learn(proc, "gateEnabled", noteOnEvent(40));

  const float initial = paramValue(proc, "gateEnabled");
  proc.midiMapper.processMidi(noteOnEvent(40));
  EXPECT_NE(paramValue(proc, "gateEnabled"), initial);
  proc.midiMapper.processMidi(noteOnEvent(40));
  EXPECT_EQ(paramValue(proc, "gateEnabled"), initial);
}

TEST(MidiMapperTest, CcOnContinuousParameterIsAbsolute) {
  TONE3000Processor proc;
  learn(proc, "inputLevel", ccEvent(11, 64));

  proc.midiMapper.processMidi(ccEvent(11, 127));
  EXPECT_NEAR(paramValue(proc, "inputLevel"),
              proc.parameters.getParameter("inputLevel")->convertFrom0to1(1.0f), 1e-4f);
  proc.midiMapper.processMidi(ccEvent(11, 0));
  EXPECT_NEAR(paramValue(proc, "inputLevel"),
              proc.parameters.getParameter("inputLevel")->convertFrom0to1(0.0f), 1e-4f);
}

TEST(MidiMapperTest, BlockPowerTargetsDeliverTheirPosition) {
  TONE3000Processor proc;
  learn(proc, "block1Power", ccEvent(30, 127));
  learn(proc, "block2Power", ccEvent(31, 127));

  std::vector<int> toggles;
  proc.midiMapper.onBlockPowerToggle = [&toggles](int index) { toggles.push_back(index); };

  proc.midiMapper.processMidi(ccEvent(30, 127));
  proc.midiMapper.processMidi(ccEvent(31, 127));
  pumpMessages();

  ASSERT_EQ(toggles.size(), 2u);
  EXPECT_EQ(toggles[0], 0);
  EXPECT_EQ(toggles[1], 1);
}

TEST(MidiMapperTest, PresetStepTargetsDeliverCoalescedDeltas) {
  TONE3000Processor proc;
  learn(proc, "presetNext", ccEvent(26, 127));
  learn(proc, "presetPrevious", ccEvent(27, 127));

  // Observe the mapper's delivery instead of loading real presets (the
  // processor's own hook walks the on-disk preset list).
  std::vector<int> deltas;
  proc.midiMapper.onPresetStep = [&deltas](int delta) { deltas.push_back(delta); };

  // One next stomp (momentary pair) → one +1.
  proc.midiMapper.processMidi(ccEvent(26, 127));
  proc.midiMapper.processMidi(ccEvent(26, 0));
  pumpMessages();
  ASSERT_EQ(deltas.size(), 1u);
  EXPECT_EQ(deltas[0], 1);

  // One previous stomp → one -1.
  proc.midiMapper.processMidi(ccEvent(27, 127));
  pumpMessages();
  ASSERT_EQ(deltas.size(), 2u);
  EXPECT_EQ(deltas[1], -1);

  // Two rapid next presses before the async hop coalesce into one +2,
  // landing two presets ahead, same as delivering them separately.
  deltas.clear();
  proc.midiMapper.processMidi(ccEvent(26, 127));
  proc.midiMapper.processMidi(ccEvent(26, 127));
  pumpMessages();
  ASSERT_EQ(deltas.size(), 1u);
  EXPECT_EQ(deltas[0], 2);
}

TEST(MidiMapperTest, ProgramChangeDeliversProgramNumber) {
  // Program changes need no mapping. Observe the delivery instead of loading
  // real presets (the processor's hook indexes the on-disk list; the
  // PresetManager tests pin that user presets lead that list).
  TONE3000Processor proc;
  std::vector<int> programs;
  proc.midiMapper.onProgramChange = [&programs](int program) { programs.push_back(program); };

  juce::MidiBuffer midi;
  midi.addEvent(juce::MidiMessage::programChange(1, 5), 0);
  proc.midiMapper.processMidi(midi);
  pumpMessages();

  ASSERT_EQ(programs.size(), 1u);
  EXPECT_EQ(programs[0], 5);
}

TEST(MidiMapperTest, SceneTargetsJumpAndStep) {
  // Footswitch-style scene control: "Scene N" jumps, Next/Previous steps
  // (wrapping 4 -> 1), each press once (a momentary 127 then 0 fires once).
  TONE3000Processor proc;
  ASSERT_TRUE(proc.midiMapper.setCcMapping("scene3", 20));
  ASSERT_TRUE(proc.midiMapper.setCcMapping("sceneNext", 21));
  ASSERT_TRUE(proc.midiMapper.setCcMapping("scenePrevious", 22));
  pumpMessages();

  proc.midiMapper.processMidi(ccEvent(20, 127));
  proc.midiMapper.processMidi(ccEvent(20, 0));
  pumpMessages();
  EXPECT_EQ(proc.getActiveScene(), 2);

  proc.midiMapper.processMidi(ccEvent(21, 127));
  proc.midiMapper.processMidi(ccEvent(21, 0));
  pumpMessages();
  EXPECT_EQ(proc.getActiveScene(), 3);

  proc.selectScene(0);
  proc.midiMapper.processMidi(ccEvent(22, 127));
  pumpMessages();
  EXPECT_EQ(proc.getActiveScene(), 3) << "previous from scene 1 wraps to scene 4";

  EXPECT_FALSE(proc.midiMapper.setCcMapping("scene5", 23));
}

// "Scene Select": one CC whose value picks the scene (0 = Scene 1), or a
// note mapping answering to four consecutive notes.
TEST(MidiMapperTest, SceneSelectByCcValueOrConsecutiveNotes) {
  TONE3000Processor proc;
  ASSERT_TRUE(proc.midiMapper.setCcMapping("sceneSelect", 30));
  pumpMessages();
  proc.midiMapper.processMidi(ccEvent(30, 2));
  pumpMessages();
  EXPECT_EQ(proc.getActiveScene(), 2);
  proc.midiMapper.processMidi(ccEvent(30, 0));
  pumpMessages();
  EXPECT_EQ(proc.getActiveScene(), 0);
  proc.midiMapper.processMidi(ccEvent(30, 9));  // out of range: ignored
  pumpMessages();
  EXPECT_EQ(proc.getActiveScene(), 0);

  ASSERT_TRUE(proc.midiMapper.setNoteMapping("sceneSelect", 36));
  pumpMessages();
  juce::MidiBuffer note;
  note.addEvent(juce::MidiMessage::noteOn(1, 39, 0.8f), 0);
  proc.midiMapper.processMidi(note);
  pumpMessages();
  EXPECT_EQ(proc.getActiveScene(), 3) << "note 36 = Scene 1, so 39 = Scene 4";
  proc.midiMapper.processMidi(ccEvent(30, 1));  // the CC mapping was replaced
  pumpMessages();
  EXPECT_EQ(proc.getActiveScene(), 3);
}

}  // namespace
