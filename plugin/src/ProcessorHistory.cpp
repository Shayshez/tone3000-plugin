#include "Processor.h"
#include <map>

// #############################
// UNDO / REDO (chain history)
// #############################
//
// Snapshot-based: every chain mutator records the pre-mutation settings
// (ChainHistory entries are settings-only ValueTrees: tone JSON + params,
// never model bytes). Undo/redo restore a snapshot by *reconciling* it
// against the live chains: blocks whose id/tone/model still match keep their
// loaded engines and in-memory model caches, so undoing a knob tweak costs a
// few property writes, while undoing a structural edit only reloads the
// blocks that actually changed.

juce::ValueTree TONE3000Processor::captureChainSnapshot(bool includeModelData) const {
  juce::ValueTree snapshot("ChainSnapshot");

  juce::ValueTree blocks("ChainBlocks");
  const ModelKeepPredicate keepSceneModels = [this](const std::string& blockId, int modelId) {
    return sceneReferencesModel(blockId, modelId);
  };
  serializeChainToTree(chain, blocks, includeModelData, &keepSceneModels);
  snapshot.appendChild(blocks, nullptr);
  // Scenes ride every snapshot (undo, presets, session state).
  serializeScenes(snapshot);

  return snapshot;
}

void TONE3000Processor::pushChainHistory(const juce::String& coalesceKey) {
  // Mid-gesture updates skip the (comparatively pricey) serialization
  // entirely; the entry on top of the stack already holds the pre-gesture
  // state, which is exactly what undo should restore.
  if (chainHistory.shouldCoalesce(coalesceKey))
    return;
  chainHistory.push(captureChainSnapshot(), coalesceKey);
}

void TONE3000Processor::queueActiveModelLoad(ChainBlock& block) {
  // EQ blocks have no model/tone to fetch at all - see ChainBlockType::EQ's
  // own comment. Every generic path that reconstructs a block this way
  // (duplicate, paste, restore, undo/redo reconciliation) funnels through
  // here, so this one early-out is the whole story for all of them.
  if (block.type == ChainBlockType::EQ) {
    block.loaded = true;
    block.loadFailed = false;
    block.modelLoading = false;
    return;
  }

  // DUAL_MONO has no tone/model of its own either (its children do) - mark
  // it loaded the same way, but recurse into whichever children are
  // present so *their* loads still get queued (they're real tone-bearing
  // blocks, reconstructed fresh by reconcileChainFromTree below just like
  // any top-level block).
  //
  // The Pan/Width smoothers also need a hard reset here (configures the
  // ramp *duration*, not just the target): a block reconstructed by
  // undo/redo/restore never passes through prepareChain (that only
  // revisits blocks that already existed at prepareToPlay/rate-change
  // time), so without this its very first live Pan/Width drag would step
  // instead of glide. Same duration prepareChain's own DUAL_MONO branch
  // and addDualMonoBlock use.
  if (block.type == ChainBlockType::DUAL_MONO) {
    block.loaded = true;
    block.loadFailed = false;
    block.modelLoading = false;
    block.dualLeftPanSmoother.reset(chainSampleRate(), 0.05f);
    block.dualRightPanSmoother.reset(chainSampleRate(), 0.05f);
    block.dualWidthSmoother.reset(chainSampleRate(), 0.05f);
    block.dualLeftSoloGainSmoother.reset(chainSampleRate(), 0.05f);
    block.dualRightSoloGainSmoother.reset(chainSampleRate(), 0.05f);
    block.dualLeftPanSmoother.setCurrentAndTargetValue(block.dualLeftPanNormalized);
    block.dualRightPanSmoother.setCurrentAndTargetValue(block.dualRightPanNormalized);
    block.dualWidthSmoother.setCurrentAndTargetValue(block.dualWidthNormalized);
    const bool leftForcedSilent = block.dualLeftMuted;
    const bool rightForcedSilent = block.dualRightMuted;
    block.dualLeftSoloGainSmoother.setCurrentAndTargetValue(
        leftForcedSilent || (block.dualSoloRight && !block.dualSoloLeft) ? 0.0f : 1.0f);
    block.dualRightSoloGainSmoother.setCurrentAndTargetValue(
        rightForcedSilent || (block.dualSoloLeft && !block.dualSoloRight) ? 0.0f : 1.0f);
    block.dualLeftPolaritySmoother.reset(chainSampleRate(), 0.05f);
    block.dualRightPolaritySmoother.reset(chainSampleRate(), 0.05f);
    block.dualLeftPolaritySmoother.setCurrentAndTargetValue(block.dualLeftInvert ? -1.0f : 1.0f);
    block.dualRightPolaritySmoother.setCurrentAndTargetValue(block.dualRightInvert ? -1.0f
                                                                                   : 1.0f);
    // Same "never passes through prepareChain" reasoning applies to Align:
    // its StereoOffset owns a JUCE DelayLine that is unusable (unsized,
    // effectively garbage state) until prepare() actually runs once.
    block.dualAlign.prepare(chainSampleRate(), chainDomainBlockSize());
    block.dualGoniometer.prepare(chainSampleRate());
    for (auto& child : block.dualLeft)
      if (child)
        queueActiveModelLoad(*child);
    for (auto& child : block.dualRight)
      if (child)
        queueActiveModelLoad(*child);
    return;
  }

  // Every bail below leaves the block unloadable, so flag it so the UI shows
  // the retry affordance instead of a loader that can never resolve, and log
  // at release level (these paths are the needle for "stuck loading after
  // relaunch" reports).
  auto bail = [&block](const juce::String& reason) {
    juce::Logger::writeToLog("[ModelLoader] Cannot queue load for block " +
                             juce::String(block.id) + ": " + reason);
    block.loaded = false;
    block.loadFailed = true;
    block.modelLoading = false;
  };

  juce::DynamicObject* toneObj = block.toneVar.getDynamicObject();
  if (toneObj == nullptr) {
    bail("stored tone JSON did not parse");
    return;
  }

  juce::var modelsVar = toneObj->getProperty("models");
  if (modelsVar.isArray()) {
    for (const auto& modelVar : *modelsVar.getArray()) {
      juce::DynamicObject* modelObj = modelVar.getDynamicObject();
      if (modelObj == nullptr ||
          static_cast<int>(modelObj->getProperty("id")) != block.activeModelId)
        continue;

      const juce::String modelUrl = modelObj->getProperty("model_url").toString();
      const juce::String modelName = modelObj->getProperty("name").toString();

      // switchModelInBackground prefers the block's in-memory model cache and
      // only hits the network when the bytes are gone: ideal for undo/redo.
      loadingThreadPool.addJob(std::function<void()>(
          [this, blockId = block.id, modelId = block.activeModelId, modelUrl, modelName]() {
            switchModelInBackground(blockId, modelId, modelUrl, modelName);
          }));
      return;
    }
  }

  // The stored tone can't name the active model (no models array, or its
  // entry is gone: states written by older builds could drift toneJson and
  // activeModelId apart). When the state carried the bytes, load straight
  // from the cache instead of stranding the block on a retry that can never
  // resolve (issue #127 logs show exactly this: "not in stored tone JSON"
  // bails on blocks whose bytes sat in the embedded cache). No URL to pass;
  // a cache miss inside the job fails into the same retry UI as the bail.
  if (block.modelCache.count(block.activeModelId) != 0) {
    juce::Logger::writeToLog("[ModelLoader] Active model " + juce::String(block.activeModelId) +
                             " missing from stored tone JSON; loading from cached bytes (block " +
                             juce::String(block.id) + ")");
    loadingThreadPool.addJob(std::function<void()>(
        [this, blockId = block.id, modelId = block.activeModelId]() {
          switchModelInBackground(blockId, modelId, juce::String(),
                                  "model " + juce::String(modelId));
        }));
    return;
  }

  if (!modelsVar.isArray()) {
    bail("stored tone JSON has no models array");
    return;
  }
  bail("active model " + juce::String(block.activeModelId) + " not in stored tone JSON");
}

void TONE3000Processor::reconcileChainFromTree(const juce::ValueTree& chainState, Lane& target,
                                               Lane& retired, bool padInserts) {
  // Park the live blocks by id so matching ones can be moved back with their
  // engines/model caches intact. Anything left over at the end is a removal
  // and goes into `retired`; the caller destroys those after releasing
  // chainMutex (engine teardown is heavy).
  std::map<std::string, std::unique_ptr<ChainBlock>> existing;
  for (auto& b : target)
    if (b)
      existing[b->id] = std::move(b);
  target.clear();

  for (int i = 0; i < chainState.getNumChildren(); ++i) {
    juce::ValueTree blockState = chainState.getChild(i);
    if (!blockState.hasType("ChainBlock"))
      continue;

    const std::string blockId = blockState.getProperty("id").toString().toStdString();
    const ChainBlockType type =
        chainBlockTypeFromString(blockState.getProperty("type").toString());
    const int toneId = blockState.getProperty("toneId", 0);

    // Reusable when identity matches: same block, same type, same tone. A
    // swapped tone (same id, different tone) rebuilds like a fresh block.
    std::unique_ptr<ChainBlock> block;
    auto it = existing.find(blockId);
    if (it != existing.end() && it->second->type == type &&
        (type == ChainBlockType::INSERT || it->second->toneId == toneId)) {
      block = std::move(it->second);
      existing.erase(it);
    } else {
      block = std::make_unique<ChainBlock>(blockId, type);
    }

    applyBlockSettings(*block, blockState);

    if (type == ChainBlockType::INSERT) {
      target.push_back(std::move(block));
      continue;
    }

    const int activeModelId = blockState.getProperty("activeModelId", 0);
    const bool modelChanged = block->activeModelId != activeModelId;
    const juce::String toneJson = blockState.getProperty("toneJson").toString();
    // Re-parse the cached tone var/summary only when the tone actually
    // changed (reused blocks keep theirs; fresh blocks always parse).
    if (!block->toneVar.isObject() || block->toneJson != toneJson)
      setToneOnBlock(*block, toneId, toneJson, juce::JSON::parse(toneJson));
    else
      block->toneId = toneId;
    block->activeModelId = activeModelId;

    // DUAL_MONO only: reconcile the two fixed child slots the same way,
    // recursing into the nested DualLeftBlocks/DualRightBlocks children -
    // mirrors serializeChainToTree's own nesting. padInserts=false: a dual
    // slot never carries insert placeholders (see the declaration's own
    // comment). Threads the same `retired` vector so a removed/replaced
    // child tears its engine down after the lock, exactly like top-level
    // removals already do.
    if (type == ChainBlockType::DUAL_MONO) {
      reconcileChainFromTree(blockState.getChildWithName("DualLeftBlocks"), block->dualLeft,
                             retired, /*padInserts=*/false);
      reconcileChainFromTree(blockState.getChildWithName("DualRightBlocks"), block->dualRight,
                             retired, /*padInserts=*/false);
    }

    // Engines survive only when the loaded model is still the right one.
    // Everything else (fresh block, model switch, load still in flight)
    // goes through the background loader: cache-first, network fallback.
    if (modelChanged || !block->loaded) {
      block->loaded = false;
      block->loadFailed = false;  // fresh load queued below, back to loading UI
      block->modelLoading = true;
      // Project files and presets embed model bytes; seed the in-memory
      // cache with the ones the block's tone still references (the active
      // model, plus a local tone's full stored list) so those load and
      // switch offline. Anything else is audition dead weight from states
      // written by builds that persisted the whole cache (issue #127):
      // unreachable through the stored tone, so seeding it would only
      // balloon RAM and ride every later save. Skipping it here is what
      // slims an already-bloated project on its next save. Undo snapshots
      // are settings-only (no ModelCache child), so this is a no-op there.
      // (toneJson/activeModelId were applied above, so referencesModel
      // judges against exactly what this restore is installing.)
      const juce::ValueTree cacheState = blockState.getChildWithName("ModelCache");
      for (int j = 0; j < cacheState.getNumChildren(); ++j) {
        const juce::ValueTree cachedModel = cacheState.getChild(j);
        const int modelId = cachedModel.getProperty("modelId");
        if (!block->referencesModel(modelId) && !sceneReferencesModel(block->id, modelId))
          continue;
        if (block->modelCache.find(modelId) != block->modelCache.end())
          continue;
        const juce::var dataVar = cachedModel.getProperty("data");
        if (const auto* raw = dataVar.getBinaryData()) {
          const auto* bytes = static_cast<const uint8_t*>(raw->getData());
          block->modelCache[modelId].assign(bytes, bytes + raw->getSize());
        } else {
          juce::Logger::writeToLog("[Restore] Embedded model bytes for model " +
                                   juce::String(modelId) + " missing (block " +
                                   juce::String(block->id) + "); will refetch");
        }
      }
      // One release-level line per reloading block: enough to diagnose
      // "stuck loading after relaunch" reports from a user's log file.
      juce::Logger::writeToLog(
          "[Restore] Block " + juce::String(block->id) + " tone " + juce::String(toneId) +
          " model " + juce::String(activeModelId) +
          (block->modelCache.count(activeModelId) != 0 ? " (cached)" : " (needs fetch)") +
          " queued for load");
      queueActiveModelLoad(*block);
    }

    target.push_back(std::move(block));
  }

  // Reconciled chains always come back up to the minimum slot layout.
  // Snapshots from this build already satisfy the invariant; legacy
  // states/presets that carried a single insert get padded here. Pad only,
  // never trim: a snapshot may legitimately carry surplus slots the user
  // positioned (addInsertSlot), and undo/redo/preset loads must bring them
  // back exactly. Skipped for a Dual Mono child slot (padInserts=false) - it
  // never carries insert placeholders in the first place.
  if (padInserts)
    normalizeLaneInserts(target, /*trimSurplus=*/false);

  // Whatever is still parked was removed by this restore.
  for (auto& [id, b] : existing)
    retired.push_back(std::move(b));
}

TONE3000Processor::Lane TONE3000Processor::restoreChainSnapshot(const juce::ValueTree& snapshot) {
  Lane retired;
  if (!snapshot.isValid())
    return retired;

  // A legacy snapshot's "RightChainBlocks"/"stereoEnabled"/"branchSide"/
  // "branchAfterBlockId" fields (from before the global stereo-lane mode was
  // removed) are simply never read here - the old right-lane content and
  // branch/solo/invert/align state silently fold away, leaving only the old
  // Left lane, with no error and no user-facing message.
  // Scenes first: the rebuild below filters each block's cached model bytes
  // and must keep the ones other scenes select (sceneReferencesModel). A
  // snapshot without scenes (older presets/sessions, reset to default)
  // leaves all eight empty: each starts as the live chain when visited.
  restoreScenes(snapshot);
  reconcileChainFromTree(snapshot.getChildWithName("ChainBlocks"), chain, retired);
  refreshWarmEngines();
  // The host's "scene" parameter follows the restored active scene (async:
  // we hold chainMutex here, and a host notification may call back in).
  sceneParamDirty.store(true);
  triggerAsyncUpdate();

  // Restores can add/remove/retire IR blocks wholesale (undo/redo, presets,
  // project load), so resync the host-facing tail length.
  refreshIrTailLength();

  bumpChainRevision();
  return retired;
}

bool TONE3000Processor::undoChain() {
  // No-op undos (hotkey spam at the stack end) must not dip the audio, so
  // check the stack before arming the fade.
  {
    juce::ScopedLock lock(chainMutex);
    if (!chainHistory.canUndo())
      return false;
  }

  // A restore can restructure the chain arbitrarily; mute-splice it like
  // any structural edit. Constructed before the lock: the audio thread
  // needs chainMutex to run the fade down.
  ChainEditFade editFade(*this);

  Lane retired;  // destroyed after the lock; see restoreChainSnapshot
  {
    juce::ScopedLock lock(chainMutex);
    if (!chainHistory.canUndo())
      return false;
    retired = restoreChainSnapshot(chainHistory.undo(captureChainSnapshot()));
  }
  DBG("Chain undo applied");

  // The restore may have queued background reloads (undone model/tone
  // changes); hold the mute until they land (bounded), not just through
  // the splice, or the dry input plays while the engines rebuild. A
  // settings-only undo has nothing loading and releases immediately.
  editFade.releaseWhenChainLoadsSettle();
  return true;
}

bool TONE3000Processor::redoChain() {
  {
    juce::ScopedLock lock(chainMutex);
    if (!chainHistory.canRedo())
      return false;
  }

  // See undoChain: mute-splice the restore.
  ChainEditFade editFade(*this);

  Lane retired;  // destroyed after the lock; see restoreChainSnapshot
  {
    juce::ScopedLock lock(chainMutex);
    if (!chainHistory.canRedo())
      return false;
    retired = restoreChainSnapshot(chainHistory.redo(captureChainSnapshot()));
  }
  DBG("Chain redo applied");

  // See undoChain: hold the mute through any queued reloads.
  editFade.releaseWhenChainLoadsSettle();
  return true;
}
