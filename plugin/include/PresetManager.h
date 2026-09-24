#pragma once
#include <juce_core/juce_core.h>
#include <juce_data_structures/juce_data_structures.h>
#include <vector>

/**
 * On-disk internal preset store. Pure file layer: one XML file per preset,
 * no knowledge of what a preset contains (the processor builds/consumes the
 * ValueTree payloads). Message-thread only.
 *
 * Layout:
 *   <user data dir>/TONE3000/Presets/<uuid>.t3kpreset   (user presets)
 *   <user data dir>/TONE3000/Presets/Factory/…          (read-only factory)
 *   <system data dir>/TONE3000/Presets/Factory/…        (installer-shipped)
 *
 * The system Factory folder is where installers drop shipped presets
 * (macOS /Library/Application Support, Windows ProgramData). Both Factory
 * dirs are scanned; a user-Factory file with the same stem wins so local
 * overrides of a shipped preset are possible.
 *
 * Ids are "user:<stem>" / "factory:<stem>" so the two namespaces can never
 * collide and the UI can tell them apart without extra lookups. Display
 * names live *inside* the file (filenames are uuids), so any characters are
 * fine and renames never touch the filesystem name.
 *
 * The list is rescanned on every call; it's a handful of stat()s, and it
 * keeps multiple plugin instances sharing the folder coherent for free.
 *
 * Ordering: user presets always come before factory presets (the browser's
 * two sections; a player's own presets own the low MIDI program-change
 * numbers). Within each section a custom order can be set via move() and
 * persists in order.json beside the preset files; presets not in the order
 * file (new saves, first run) fall back to name order after the ordered
 * ones. List order is user-facing truth: the browser, prev/next stepping
 * and MIDI program-change numbers all follow it.
 */
class PresetManager {
public:
  struct Info {
    juce::String id;
    juce::String name;
    bool factory{false};
  };

  static constexpr const char* kFileExtension = ".t3kpreset";
  static constexpr const char* kPresetTag = "T3KPreset";

  PresetManager();

  /** Store presets under an explicit base directory (tests use a temp dir).
      `systemFactory` stands in for the installer-shipped Factory dir; the
      default keeps temp stores isolated from presets installed on the
      machine. */
  explicit PresetManager(const juce::File& baseDir, const juce::File& systemFactory = {});

  /** All presets, user first then factory, each section sorted by name. */
  std::vector<Info> list() const;

  /** Full preset tree for an id, or an invalid tree when missing/corrupt. */
  juce::ValueTree load(const juce::String& id) const;

  /** Store a preset under `name`. A user preset with the same name is
      overwritten in place (same id); that's the "update" path, since the
      save popover is the only write UI. Returns the resulting Info, or an
      empty-id Info on IO failure. */
  Info save(const juce::String& name, juce::ValueTree preset) const;

  /** Rename a user preset (rewrites the name inside the file). */
  bool rename(const juce::String& id, const juce::String& newName) const;

  /** Delete a user preset. Factory presets are refused. The store for the
      real user folder moves the file to the OS trash (recoverable even if
      the app's own Undo is missed); stores on an explicit base directory
      (tests) delete outright, so test runs never litter the real Trash.
      `removedBytes`, when given, receives the file's contents first, for
      restore(). */
  bool remove(const juce::String& id, juce::MemoryBlock* removedBytes = nullptr) const;

  /** Write a removed user preset back under its original id (same file
      name, so its slot in the custom order comes back too - remove() never
      prunes order.json). Refuses factory ids, empty payloads, and an id whose
      file exists (never clobbers). A same-name save made since the delete
      has its own id, so both then coexist. */
  bool restore(const juce::String& id, const juce::MemoryBlock& bytes) const;

  /** Move a preset by `delta` steps within its section (negative = earlier).
      Clamped to the factory/user boundary so the browser's sections and the
      global order can't disagree. Persists the whole current order. */
  bool move(const juce::String& id, int delta) const;

private:
  juce::File fileForId(const juce::String& id) const;
  static juce::File defaultSystemFactoryDir();
  static juce::ValueTree readPresetFile(const juce::File& file);
  static bool writePresetFile(const juce::File& file, const juce::ValueTree& preset);

  juce::File orderFile() const;
  juce::StringArray readOrder() const;
  bool writeOrder(const juce::StringArray& ids) const;

  juce::File userDir;
  juce::File factoryDir;        // user-local Factory/ (dev drops, Linux install)
  juce::File systemFactoryDir;  // installer-shipped Factory/ (invalid when absent)
  bool trashOnRemove{false};     // true only for the real user store (default ctor)
};
