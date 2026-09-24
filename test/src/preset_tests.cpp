// PresetManager file-layer tests, run against a throwaway temp directory.
//
// The store moved to a magic-prefixed binary ValueTree format (same T3KB
// framing as plugin state); these pin the format, the same-name-overwrite
// save path, the user/factory split, and the custom ordering.
#include "PresetManager.h"

#include <gtest/gtest.h>
#include <juce_core/juce_core.h>
#include <juce_data_structures/juce_data_structures.h>

namespace {

// Fresh temp preset root per test, deleted on destruction.
struct TempPresetDir {
  TempPresetDir()
      : dir(juce::File::getSpecialLocation(juce::File::tempDirectory)
                .getChildFile("t3k-preset-tests-" + juce::Uuid().toString())) {
    dir.createDirectory();
  }
  ~TempPresetDir() { dir.deleteRecursively(); }
  juce::File dir;
};

juce::ValueTree makePreset(const juce::String& marker) {
  juce::ValueTree preset(PresetManager::kPresetTag);
  preset.setProperty("marker", marker, nullptr);
  return preset;
}

TEST(PresetManagerTest, SaveLoadRoundTripAndSameNameOverwrites) {
  TempPresetDir tmp;
  PresetManager mgr(tmp.dir);

  const auto info = mgr.save("Lead", makePreset("v1"));
  ASSERT_TRUE(info.id.isNotEmpty());
  EXPECT_FALSE(info.factory);

  juce::ValueTree loaded = mgr.load(info.id);
  ASSERT_TRUE(loaded.isValid());
  EXPECT_EQ(loaded.getProperty("marker").toString(), juce::String("v1"));
  EXPECT_EQ(loaded.getProperty("name").toString(), juce::String("Lead"));

  // Saving the same name again is the update path: same id, new payload,
  // still exactly one preset in the list.
  const auto updated = mgr.save("Lead", makePreset("v2"));
  EXPECT_EQ(updated.id, info.id);
  EXPECT_EQ(mgr.load(info.id).getProperty("marker").toString(), juce::String("v2"));
  EXPECT_EQ(mgr.list().size(), 1u);
}

TEST(PresetManagerTest, RenameAndRemoveApplyToUserPresetsOnly) {
  TempPresetDir tmp;
  PresetManager mgr(tmp.dir);

  const auto info = mgr.save("Old Name", makePreset("x"));
  ASSERT_TRUE(mgr.rename(info.id, "New Name"));
  EXPECT_EQ(mgr.load(info.id).getProperty("name").toString(), juce::String("New Name"));
  EXPECT_FALSE(mgr.rename(info.id, "   "));  // blank names refused

  // A factory preset (file dropped into Factory/) refuses rename and remove.
  tmp.dir.getChildFile("Factory").createDirectory();
  PresetManager seeded(tmp.dir);
  const auto factoryFile =
      tmp.dir.getChildFile("Factory").getChildFile(juce::String("clean") +
                                                   PresetManager::kFileExtension);
  {
    juce::FileOutputStream out(factoryFile);
    ASSERT_TRUE(out.openedOk());
    out.write("T3KB", 4);
    makePreset("f").writeToStream(out);
  }
  EXPECT_FALSE(seeded.rename("factory:clean", "Hacked"));
  EXPECT_FALSE(seeded.remove("factory:clean"));

  EXPECT_TRUE(mgr.remove(info.id));
  EXPECT_FALSE(mgr.load(info.id).isValid());
}

TEST(PresetManagerTest, SystemFactoryPresetsListedAndLocalOverrideWins) {
  TempPresetDir tmp;
  TempPresetDir system;  // stands in for the installer-shipped Factory dir

  auto writeFactoryFile = [](const juce::File& file, const juce::String& name) {
    juce::ValueTree preset(PresetManager::kPresetTag);
    preset.setProperty("name", name, nullptr);
    juce::FileOutputStream out(file);
    ASSERT_TRUE(out.openedOk());
    out.write("T3KB", 4);
    preset.writeToStream(out);
  };
  writeFactoryFile(system.dir.getChildFile(juce::String("clean") + PresetManager::kFileExtension),
                   "Shipped Clean");
  writeFactoryFile(system.dir.getChildFile(juce::String("lead") + PresetManager::kFileExtension),
                   "Shipped Lead");
  tmp.dir.getChildFile("Factory").createDirectory();
  writeFactoryFile(tmp.dir.getChildFile("Factory").getChildFile(
                       juce::String("clean") + PresetManager::kFileExtension),
                   "Local Clean");

  // Same stem in both dirs collapses to one entry, with the local file
  // winning; the untouched shipped preset still lists and loads.
  PresetManager mgr(tmp.dir, system.dir);
  const auto presets = mgr.list();
  ASSERT_EQ(presets.size(), 2u);
  EXPECT_EQ(presets[0].name, juce::String("Local Clean"));
  EXPECT_TRUE(presets[0].factory);
  EXPECT_EQ(presets[1].name, juce::String("Shipped Lead"));
  EXPECT_EQ(mgr.load("factory:clean").getProperty("name").toString(),
            juce::String("Local Clean"));
  EXPECT_EQ(mgr.load("factory:lead").getProperty("name").toString(),
            juce::String("Shipped Lead"));

  // Shipped presets are as read-only as local factory ones.
  EXPECT_FALSE(mgr.rename("factory:lead", "Hacked"));
  EXPECT_FALSE(mgr.remove("factory:lead"));
}

TEST(PresetManagerTest, UserSectionListsBeforeFactory) {
  // The list order is the MIDI program-change order, and user presets own
  // the low numbers. The factory preset is named to sort first so only the
  // section rule can put it last.
  TempPresetDir tmp;
  PresetManager mgr(tmp.dir);
  tmp.dir.getChildFile("Factory").createDirectory();
  {
    juce::ValueTree preset(PresetManager::kPresetTag);
    preset.setProperty("name", "AAA Factory", nullptr);
    juce::FileOutputStream out(tmp.dir.getChildFile("Factory").getChildFile(
        juce::String("aaa") + PresetManager::kFileExtension));
    ASSERT_TRUE(out.openedOk());
    out.write("T3KB", 4);
    preset.writeToStream(out);
  }
  const auto alpha = mgr.save("Alpha", makePreset("a"));
  const auto zulu = mgr.save("Zulu", makePreset("z"));

  auto presets = mgr.list();
  ASSERT_EQ(presets.size(), 3u);
  EXPECT_FALSE(presets[0].factory);
  EXPECT_FALSE(presets[1].factory);
  EXPECT_TRUE(presets[2].factory);

  // A custom order (order.json) keeps the sections separated too.
  ASSERT_TRUE(mgr.move(zulu.id, -1));
  presets = mgr.list();
  ASSERT_EQ(presets.size(), 3u);
  EXPECT_EQ(presets[0].id, zulu.id);
  EXPECT_EQ(presets[1].id, alpha.id);
  EXPECT_TRUE(presets[2].factory);
}

TEST(PresetManagerTest, ListSkipsCorruptAndForeignFiles) {
  TempPresetDir tmp;
  PresetManager mgr(tmp.dir);
  mgr.save("Good", makePreset("ok"));

  // Legacy XML and truncated files must be ignored, not crash or list.
  tmp.dir.getChildFile(juce::String("legacy") + PresetManager::kFileExtension)
      .replaceWithText("<T3KPreset name=\"Old XML\"/>");
  tmp.dir.getChildFile(juce::String("trunc") + PresetManager::kFileExtension)
      .replaceWithText("T3");

  const auto presets = mgr.list();
  ASSERT_EQ(presets.size(), 1u);
  EXPECT_EQ(presets[0].name, juce::String("Good"));
  EXPECT_FALSE(mgr.load("user:legacy").isValid());
}

TEST(PresetManagerTest, MovePersistsOrderWithinTheUserSection) {
  TempPresetDir tmp;
  PresetManager mgr(tmp.dir);
  const auto a = mgr.save("Alpha", makePreset("a"));
  const auto b = mgr.save("Beta", makePreset("b"));
  const auto c = mgr.save("Gamma", makePreset("c"));

  // Name order by default; moving Gamma up one lands it between the others,
  // and the order survives a fresh manager (order.json).
  ASSERT_TRUE(mgr.move(c.id, -1));
  PresetManager fresh(tmp.dir);
  const auto presets = fresh.list();
  ASSERT_EQ(presets.size(), 3u);
  EXPECT_EQ(presets[0].id, a.id);
  EXPECT_EQ(presets[1].id, c.id);
  EXPECT_EQ(presets[2].id, b.id);

  // Edges are refused: Alpha is already first.
  EXPECT_FALSE(fresh.move(a.id, -1));
}

TEST(PresetManagerTest, MoveShiftsByDeltaWithinTheSection) {
  TempPresetDir tmp;
  PresetManager mgr(tmp.dir);
  const auto a = mgr.save("Alpha", makePreset("a"));
  const auto b = mgr.save("Beta", makePreset("b"));
  const auto c = mgr.save("Gamma", makePreset("c"));
  const auto d = mgr.save("Delta", makePreset("d"));

  // Name order Alpha, Beta, Delta, Gamma. Sliding Delta back two puts it first.
  ASSERT_TRUE(mgr.move(d.id, -2));
  auto presets = mgr.list();
  ASSERT_EQ(presets.size(), 4u);
  EXPECT_EQ(presets[0].id, d.id);
  EXPECT_EQ(presets[1].id, a.id);
  EXPECT_EQ(presets[2].id, b.id);
  EXPECT_EQ(presets[3].id, c.id);

  // A delta that would leave the section is a no-op at the edge.
  EXPECT_FALSE(mgr.move(d.id, -4));
}

TEST(PresetManagerTest, RemovedPresetRestoresWithItsOrderSlot) {
  // The delete toast's Undo: remove() hands back the bytes, restore() puts
  // the same file back, and since order.json still names the id, it lands
  // in its old position rather than at the alphabetical tail.
  TempPresetDir tmp;
  PresetManager mgr(tmp.dir);
  const auto a = mgr.save("Alpha", makePreset("a"));
  const auto b = mgr.save("Beta", makePreset("b"));
  const auto c = mgr.save("Gamma", makePreset("c"));
  ASSERT_TRUE(mgr.move(c.id, -2));  // Gamma, Alpha, Beta

  juce::MemoryBlock bytes;
  ASSERT_TRUE(mgr.remove(c.id, &bytes));
  EXPECT_FALSE(bytes.isEmpty());
  EXPECT_FALSE(mgr.load(c.id).isValid());

  ASSERT_TRUE(mgr.restore(c.id, bytes));
  EXPECT_EQ(mgr.load(c.id)["marker"].toString(), "c");
  const auto presets = mgr.list();
  ASSERT_EQ(presets.size(), 3u);
  EXPECT_EQ(presets[0].id, c.id);
  EXPECT_EQ(presets[1].id, a.id);
  EXPECT_EQ(presets[2].id, b.id);

  // An existing file under the id is never clobbered (a second restore).
  EXPECT_FALSE(mgr.restore(c.id, bytes));

  // Factory ids and empty payloads are refused.
  EXPECT_FALSE(mgr.restore("factory:x", bytes));
  EXPECT_FALSE(mgr.restore(a.id, juce::MemoryBlock()));
}

}  // namespace
