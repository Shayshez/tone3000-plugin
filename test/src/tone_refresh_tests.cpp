// Tone metadata refresh tests
//
// refreshToneMetadata (the UI's best-effort background sync on expanding a
// block): merges a fresh /tones/{id} API payload into every block holding
// that tone. These pin the contracts:
//
//   - fresh metadata (title, counts, url) lands on every block that holds
//     that tone, while each block's stored models array survives untouched
//     (it carries the active model's model_url, which retries/reloads need),
//   - it is purely a metadata write: no undo step, and the revision only
//     bumps when something actually changed (an identical payload no-ops),
//   - mismatched tone ids, invalid payloads and local (drop-loaded) tones
//     are all silent no-ops.
#include "Processor.h"
#include "chain_test_helpers.h"

#include <gtest/gtest.h>

namespace {

// A fresh /tones/{id}-style payload whose models array deliberately names a
// model the block does NOT store, so "stored models survive" can't pass
// vacuously.
juce::String freshPayload(int toneId, const juce::String& title, int downloads,
                          int favorites = 0, bool isFavorite = false) {
  return "{\"id\":" + juce::String(toneId) + ",\"title\":\"" + title +
         "\",\"format\":\"ir\",\"downloads_count\":" + juce::String(downloads) +
         ",\"favorites_count\":" + juce::String(favorites) +
         ",\"is_favorite\":" + juce::String(isFavorite ? "true" : "false") +
         ",\"url\":\"https://tone3000.com/tones/fresh-" + juce::String(toneId) +
         "\",\"models\":[{\"id\":999,\"name\":\"served-model\","
         "\"model_url\":\"https://test.invalid/served.wav\"}]}";
}

// The Nth tone block in the chain-state payload.
juce::var toneBlockAt(const juce::var& state, int n) {
  if (const auto* lane = state["chain"].getArray())
    for (const auto& item : *lane)
      if (item["kind"].toString() == "tone" && n-- == 0)
        return item;
  return {};
}

TEST(ToneRefreshTest, UpdatesMetadataOnEveryMatchingBlockAndPreservesStoredModels) {
  ChainTestProcessor proc;

  // Two blocks holding the same tone, but with *different* active models, so
  // the per-block merge provably keeps each block's own stored models array.
  juce::ValueTree state("ChainSnapshot");
  juce::ValueTree left("ChainBlocks");
  left.appendChild(makeIrBlockTree("blk-a", 1, 100), nullptr);
  left.appendChild(makeIrBlockTree("blk-b", 1, 101), nullptr);
  state.appendChild(left, nullptr);
  proc.restoreFromTree(state);
  ASSERT_TRUE(waitForChainLoaded(proc));

  const bool couldUndoBefore = static_cast<bool>(proc.getChainState(-1)["canUndo"]);

  EXPECT_TRUE(proc.refreshToneMetadata(freshPayload(1, "Fresh Title", 42, 7, true)));

  const juce::var after = proc.getChainState(-1);
  for (int n = 0; n < 2; ++n) {
    const juce::var block = toneBlockAt(after, n);
    ASSERT_TRUE(block.isObject()) << n;
    EXPECT_EQ(block["tone"]["title"].toString(), "Fresh Title") << n;
    EXPECT_EQ(static_cast<int>(block["tone"]["downloads_count"]), 42) << n;
    EXPECT_EQ(static_cast<int>(block["tone"]["favorites_count"]), 7) << n;
    EXPECT_TRUE(static_cast<bool>(block["tone"]["is_favorite"])) << n;
    EXPECT_EQ(block["tone"]["url"].toString(), "https://tone3000.com/tones/fresh-1") << n;
    // The stored models array survives: still exactly the block's own active
    // model, not the payload's "served-model".
    const auto* models = block["tone"]["models"].getArray();
    ASSERT_NE(models, nullptr) << n;
    ASSERT_EQ(models->size(), 1) << n;
    EXPECT_EQ(models->getReference(0)["name"].toString(), "cab") << n;
  }
  EXPECT_EQ(static_cast<int>(toneBlockAt(after, 0)["tone"]["models"][0]["id"]), 100);
  EXPECT_EQ(static_cast<int>(toneBlockAt(after, 1)["tone"]["models"][0]["id"]), 101);

  // Metadata only: the active models never moved, and no undo step appeared.
  EXPECT_EQ(static_cast<int>(toneBlockAt(after, 0)["activeModelId"]), 100);
  EXPECT_EQ(static_cast<int>(toneBlockAt(after, 1)["activeModelId"]), 101);
  EXPECT_EQ(static_cast<bool>(after["canUndo"]), couldUndoBefore);
}

TEST(ToneRefreshTest, IdenticalPayloadIsANoOp) {
  ChainTestProcessor proc;
  juce::ValueTree state("ChainSnapshot");
  juce::ValueTree left("ChainBlocks");
  left.appendChild(makeIrBlockTree("blk-a", 1, 100), nullptr);
  state.appendChild(left, nullptr);
  proc.restoreFromTree(state);
  ASSERT_TRUE(waitForChainLoaded(proc));

  ASSERT_TRUE(proc.refreshToneMetadata(freshPayload(1, "Fresh Title", 42)));
  const int revisionAfterFirst = static_cast<int>(proc.getChainState(-1)["revision"]);

  // Same payload again: nothing changed server-side, so no write, no
  // revision bump (i.e. no UI resync and no host "unsaved changes").
  EXPECT_FALSE(proc.refreshToneMetadata(freshPayload(1, "Fresh Title", 42)));
  EXPECT_EQ(static_cast<int>(proc.getChainState(-1)["revision"]), revisionAfterFirst);
}

TEST(ToneRefreshTest, MismatchedInvalidAndLocalPayloadsNoOp) {
  ChainTestProcessor proc;

  // One catalog block (tone 1) and one drop-loaded local block (tone 7).
  auto localBlock = makeIrBlockTree("blk-local", 7, 700);
  localBlock.setProperty("toneJson",
                         "{\"id\":7,\"title\":\"My Drop\",\"format\":\"ir\",\"local\":true,"
                         "\"models\":[{\"id\":700,\"name\":\"drop.wav\","
                         "\"model_url\":\"https://test.invalid/drop.wav\"}]}",
                         nullptr);
  juce::ValueTree state("ChainSnapshot");
  juce::ValueTree left("ChainBlocks");
  left.appendChild(makeIrBlockTree("blk-a", 1, 100), nullptr);
  left.appendChild(localBlock, nullptr);
  state.appendChild(left, nullptr);
  proc.restoreFromTree(state);
  ASSERT_TRUE(waitForChainLoaded(proc));

  // No block holds tone 999; garbage and id-less payloads don't parse.
  EXPECT_FALSE(proc.refreshToneMetadata(freshPayload(999, "Nobody", 1)));
  EXPECT_FALSE(proc.refreshToneMetadata("not json"));
  EXPECT_FALSE(proc.refreshToneMetadata("{\"title\":\"no id\"}"));

  // A same-id API tone must never overwrite a local (drop-loaded) one.
  EXPECT_FALSE(proc.refreshToneMetadata(freshPayload(7, "Impostor", 1)));

  const juce::var after = proc.getChainState(-1);
  EXPECT_EQ(after["chain"][0]["tone"]["title"].toString(), "Test IR");
  EXPECT_EQ(after["chain"][1]["tone"]["title"].toString(), "My Drop");
}

}  // namespace
