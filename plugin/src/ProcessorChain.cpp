#include "Processor.h"
#include <algorithm>
#include <cmath>

// ####################
// CHAIN MANAGEMENT
// ####################

// The lane loadTone inserts into: Left in mono mode, or the side the UI armed
// before launching the Select flow in stereo.
std::vector<std::unique_ptr<ChainBlock>>& TONE3000Processor::activeChain() {
  if (stereoEnabled.load() && pendingAddSide == ChainSide::Right)
    return lane(ChainSide::Right);
  return lane(ChainSide::Left);
}

namespace {
// TONE3000Processor::Lane isn't nameable from a free function (private
// alias), so this works in terms of the bare vector type directly - Lane*
// and this type's pointer are identical since Lane is only a type alias.
// Recurses one level into a Dual Mono block's own children: structurally
// the only nesting this data model allows (a dual child is never itself
// DUAL_MONO - see ChainBlock.h), so one level is the whole story, no
// deeper recursion possible regardless of what the type system permits.
ChainBlock* findInLane(std::vector<std::unique_ptr<ChainBlock>>& lane,
                       const std::string& blockId) {
  for (auto& b : lane) {
    if (!b)
      continue;
    if (b->id == blockId)
      return b.get();
    if (b->type == ChainBlockType::DUAL_MONO) {
      if (ChainBlock* found = findInLane(b->dualLeft, blockId))
        return found;
      if (ChainBlock* found = findInLane(b->dualRight, blockId))
        return found;
    }
  }
  return nullptr;
}
}  // namespace

// Find a block by id across both lanes and any Dual Mono block's own
// children (ids are globally unique) - required so async model loading
// (queueToneLoad's background completion) can re-locate a dual child after
// the fetch finishes, same as it does for any top-level block.
ChainBlock* TONE3000Processor::findBlockById(const std::string& blockId) {
  for (auto& l : lanes)
    if (ChainBlock* found = findInLane(l, blockId))
      return found;
  return nullptr;
}

namespace {

bool isInsertBlock(const std::unique_ptr<ChainBlock>& b) {
  return b != nullptr && b->type == ChainBlockType::INSERT;
}

// Which onset detection result Trim Init should use - see
// ChainBlock::trimRelaxed/irOnsetSamplesRelaxed. Independent of
// trimInitEnabled: callers decide separately whether trim is on at all.
int irEffectiveOnsetSamples(const ChainBlock& block) {
  return block.trimRelaxed ? block.irOnsetSamplesRelaxed : block.irOnsetSamples;
}

// Forward declarations: defined further down (with queueIrShapeRebuild's
// other, older use sites), but needed here for the model-load/swap paths
// above those - a swapped-in IR must reapply the block's *existing*
// Size/Width/envelope/Trim Init onto its freshly-loaded content, or those
// settings silently stop affecting the audio while still showing their old
// values on the knobs (see blockHasNonDefaultIrShape's own comment).
void queueIrShapeRebuild(TONE3000Processor& processor, ChainBlock& block,
                        juce::ThreadPool& loadingThreadPool);
bool blockHasNonDefaultIrShape(const ChainBlock& block);

}  // namespace

// See the declaration for the invariant. Called after every structural lane
// change (load/remove/cross-lane move/stereo seed/snapshot restore); pure
// bookkeeping: no revision bump, no history entry of its own.
void TONE3000Processor::normalizeLaneInserts(Lane& l) {
  const int total = static_cast<int>(l.size());
  int inserts = static_cast<int>(std::count_if(l.begin(), l.end(), isInsertBlock));
  const int tones = total - inserts;
  const int required = std::max(kMinLaneSlots - tones, 1);

  // Trim overshoot back-to-front so slots the user positioned stay put.
  for (int i = total - 1; i >= 0 && inserts > required; --i) {
    if (isInsertBlock(l[static_cast<size_t>(i)])) {
      l.erase(l.begin() + i);
      --inserts;
    }
  }

  while (inserts < required) {
    l.push_back(std::make_unique<ChainBlock>(juce::Uuid().toString().toStdString(),
                                             ChainBlockType::INSERT));
    ++inserts;
  }
}

namespace {

// Everything loadTone/swapTone need from a raw tone JSON string.
struct ParsedTone {
  bool valid = false;
  int toneId = 0;
  int firstModelId = 0;
  juce::String modelUrl;
  juce::String modelName;
  ChainBlockType type = ChainBlockType::NAM;
  // Raw catalog `gear` tag, lowercased ("cab", "space", "outboard", ...; empty
  // when absent). IR blocks only; see irCategoryFromGear.
  juce::String gear;
  // True for a drop-loaded local file tone (see loadLocalTone): no catalog
  // metadata exists at all, `gear` above is always empty for these.
  bool local = false;
  // Parsed tone with its models pruned to just the one being loaded; native
  // only ever stores the active model; the catalog stays on the API and the
  // UI pages it in for the picker.
  juce::var toneVar;
  juce::String toneJson;  // `toneVar` re-serialized (what the block persists)
};

// gear -> IrCategory. `cab` is the catalog's exclusive tag for real cabinet
// content - nothing else can legitimately mean Cab - so every other tag
// (space/outboard/experimental/generic ir) and a missing tag both fall
// through to IrPlayer.
IrCategory irCategoryFromGear(const juce::String& gear) {
  return gear == "cab" ? IrCategory::Cab : IrCategory::IrPlayer;
}

// The engine type a tone requires, from its parsed JSON (`format`, with the
// legacy `platform` fallback, and `gear` for the Cab Block split). While a
// tone swap is in flight the block's own `type` still describes the *old*
// engine (which keeps processing until the new model is applied), so loads
// must key off the tone, not the block.
ChainBlockType toneEngineType(const juce::var& toneVar, ChainBlockType fallback) {
  auto* obj = toneVar.getDynamicObject();
  if (obj == nullptr)
    return fallback;
  juce::String format = obj->getProperty("format").toString().toLowerCase();
  if (format.isEmpty())
    format = obj->getProperty("platform").toString().toLowerCase();
  if (format.isEmpty())
    return fallback;
  if (format != "nam" && obj->getProperty("gear").toString().toLowerCase() == "cab")
    return ChainBlockType::CAB;
  return format == "nam" ? ChainBlockType::NAM : ChainBlockType::IR;
}

ParsedTone parseToneForLoading(const juce::String& toneJsonString) {
  ParsedTone out;

  juce::var toneVar = juce::JSON::parse(toneJsonString);
  juce::DynamicObject* toneObj = toneVar.getDynamicObject();
  if (toneObj == nullptr) {
    DBG("Tone JSON is not a valid object");
    return out;
  }

  out.toneId = toneObj->getProperty("id");
  // The API renamed `platform` to `format`; fall back to `platform` for tone
  // JSON persisted by older builds.
  juce::String format = toneObj->getProperty("format").toString().toLowerCase();
  if (format.isEmpty())
    format = toneObj->getProperty("platform").toString().toLowerCase();
  juce::var modelsVar = toneObj->getProperty("models");

  if (!modelsVar.isArray() || modelsVar.getArray()->size() == 0) {
    DBG("Tone has no models");
    return out;
  }

  juce::DynamicObject* firstModel = modelsVar.getArray()->getReference(0).getDynamicObject();
  if (firstModel == nullptr) {
    DBG("First model is not a valid object");
    return out;
  }

  out.firstModelId = firstModel->getProperty("id");
  out.modelUrl = firstModel->getProperty("model_url").toString();
  out.modelName = firstModel->getProperty("name").toString();
  out.gear = toneObj->getProperty("gear").toString().toLowerCase();
  out.local = static_cast<bool>(toneObj->getProperty("local"));
  // `gear == "cab"` is the catalog's exclusive tag for real cabinet content
  // (see irCategoryFromGear above) - a site-loaded cab tone gets a real
  // ChainBlockType::CAB block, not IR. Local file drops never carry a `gear`
  // tag (see finishLocalToneLoad), so they're unaffected and keep defaulting
  // to IR/NAM exactly as before.
  if (format == "nam")
    out.type = ChainBlockType::NAM;
  else if (out.gear == "cab")
    out.type = ChainBlockType::CAB;
  else
    out.type = ChainBlockType::IR;

  // Store only the model being loaded; native persists just the active
  // model; the catalog stays on the API. Local tones are the exception:
  // their model list *is* the dropped files (no API to page the others back
  // in from), so it stays whole.
  if (!out.local) {
    juce::Array<juce::var> prunedModels;
    prunedModels.add(modelsVar.getArray()->getReference(0));
    toneObj->setProperty("models", prunedModels);
  }

  out.toneVar = toneVar;
  out.toneJson = juce::JSON::toString(toneVar);
  out.valid = true;
  return out;
}

}  // namespace

juce::var TONE3000Processor::makeToneSummary(const juce::var& toneVar) {
  auto* tone = toneVar.getDynamicObject();
  if (tone == nullptr)
    return {};

  juce::DynamicObject::Ptr out = new juce::DynamicObject();
  out->setProperty("id", tone->getProperty("id"));
  out->setProperty("title", tone->getProperty("title"));
  // Older persisted tone JSON used `platform` instead of `format`.
  juce::var format = tone->getProperty("format");
  if (format.toString().isEmpty())
    format = tone->getProperty("platform");
  out->setProperty("format", format);
  out->setProperty("gear", tone->getProperty("gear"));

  // Drop-loaded local file(s) (see loadLocalTone): no catalog metadata
  // exists, so the UI trims its catalog chrome (share, counts) and feeds the
  // model picker from the summary's model list instead of the API.
  const bool local = static_cast<bool>(tone->getProperty("local"));
  if (local)
    out->setProperty("local", true);

  // Catalog totals for the model picker's "n/N" and the folder stat (only
  // the active model is stored, so the UI can't count the catalog itself).
  // NAM uses the v2-architecture total; IR and other formats use models_count.
  out->setProperty("models_count", tone->getProperty("models_count"));
  out->setProperty("a2_models_count", tone->getProperty("a2_models_count"));
  // Tone-info stats row: downloads, bookmarks, models (same order as the
  // TONE3000 tone card).
  out->setProperty("downloads_count", tone->getProperty("downloads_count"));
  out->setProperty("favorites_count", tone->getProperty("favorites_count"));
  // Signed-in /tones/{id} sync: whether this user has favorited the tone.
  // Omitted when the stored payload predates the field (signed-out loads,
  // older chains) so the UI treats it as unknown rather than false.
  if (tone->hasProperty("is_favorite"))
    out->setProperty("is_favorite", tone->getProperty("is_favorite"));

  // Canonical public page URL (title slug + id); the UI's share action
  // copies it. Skipped when absent (very old stored tone JSON) so the UI
  // never sees a null; it falls back to the plain /tones/{id} path.
  const juce::String url = tone->getProperty("url").toString();
  if (url.isNotEmpty())
    out->setProperty("url", url);

  // Publish time for the creator line's relative timestamp. Omitted when
  // absent (older stored tones) so the UI skips the "· 3d" suffix.
  const juce::String publishedAt = tone->getProperty("published_at").toString();
  if (publishedAt.isNotEmpty())
    out->setProperty("published_at", publishedAt);

  // Only the first image is ever rendered (block artwork).
  juce::Array<juce::var> images;
  if (auto* imgs = tone->getProperty("images").getArray(); imgs != nullptr && !imgs->isEmpty())
    images.add(imgs->getReference(0));
  out->setProperty("images", images);

  if (auto* user = tone->getProperty("user").getDynamicObject()) {
    juce::DynamicObject::Ptr u = new juce::DynamicObject();
    u->setProperty("username", user->getProperty("username"));
    u->setProperty("avatar_url", user->getProperty("avatar_url"));
    out->setProperty("user", juce::var(u.get()));
  }

  // Catalog tones store only the active model (see parseToneForLoading /
  // switchModel); the picker pages the full catalog from the API
  // client-side. Local tones store all their models, and the switch call
  // needs each one's stash URL (there is no catalog to fetch it from), so
  // for them model_url ships in the summary too.
  juce::Array<juce::var> models;
  if (auto* modelsArr = tone->getProperty("models").getArray()) {
    for (const auto& m : *modelsArr) {
      if (auto* model = m.getDynamicObject()) {
        juce::DynamicObject::Ptr slim = new juce::DynamicObject();
        slim->setProperty("id", model->getProperty("id"));
        slim->setProperty("name", model->getProperty("name"));
        if (local)
          slim->setProperty("model_url", model->getProperty("model_url"));
        models.add(juce::var(slim.get()));
      }
    }
  }
  out->setProperty("models", models);

  return out.get();
}

void TONE3000Processor::setToneOnBlock(ChainBlock& block, int toneId, const juce::String& toneJson,
                                       const juce::var& parsedTone) {
  block.toneId = toneId;
  block.toneJson = toneJson;
  block.toneVar = parsedTone;
  block.toneSummary = makeToneSummary(parsedTone);
}

void TONE3000Processor::queueToneLoad(const std::string& blockId, int modelId,
                                      const juce::String& modelUrl,
                                      const juce::String& modelName, ChainBlockType type) {
  struct LoadToneJob : public juce::ThreadPoolJob {
    TONE3000Processor& processor;
    std::string blockId;
    int modelId;
    juce::String modelUrl;
    juce::String modelName;
    ChainBlockType type;

    LoadToneJob(TONE3000Processor& p, const std::string& bid, int mid, const juce::String& url,
                const juce::String& name, ChainBlockType t)
        : ThreadPoolJob("Load Tone"), processor(p), blockId(bid), modelId(mid), modelUrl(url),
          modelName(name), type(t) {}

    JobStatus runJob() override {
      processor.loadToneInBackground(blockId, modelId, modelUrl, modelName, type);
      return jobHasFinished;
    }
  };

  loadingThreadPool.addJob(new LoadToneJob(*this, blockId, modelId, modelUrl, modelName, type),
                           true);
}

std::string TONE3000Processor::loadTone(const juce::String& toneJsonString,
                                        const std::string& targetInsertId) {
  juce::ScopedLock lock(chainMutex);

  const ParsedTone parsed = parseToneForLoading(toneJsonString);
  if (!parsed.valid)
    return "";

  pushChainHistory();

  // Collision-proof block id (the old 4-digit random ids could collide with
  // long-lived sessions and undo snapshots).
  std::string blockId = juce::Uuid().toString().toStdString();

  auto block = std::make_unique<ChainBlock>(blockId, parsed.type);
  setToneOnBlock(*block, parsed.toneId, parsed.toneJson, parsed.toneVar);
  block->activeModelId = parsed.firstModelId;
  // Fresh blocks load at the machine-wide default A2 size; from here on the
  // size is the block's own (setBlockSlimSize, presets, undo).
  block->namSlimSize = namSlimSizeDefault.load();
  block->loaded = false;
  block->modelLoading = true;
  // The right default mix depends on the block's IR category, applied on
  // first successful apply (see applyPreparedModelToChainBlock).
  block->applyDefaultMixOnLoad = true;

  if (parsed.type == ChainBlockType::IR) {
    // A plain dropped file (no catalog `gear`, and no explicit choice from
    // the empty-slot split drop zone either - see loadLocalTone's forceGear
    // and finishLocalToneLoad) has no known category yet; seed it once the
    // model lands and its real content can be scanned (see ChainBlock::
    // irCategoryNeedsDurationGuess / applyPreparedModelToChainBlock). A
    // local drop that DID carry an explicit gear (the split zone's "IR"
    // half sets gear="ir", never collapsed to empty - the very content-
    // duration guess this skips is exactly what silently overrode that
    // explicit choice back to Cab for a short dropped file, before this
    // fix) is known immediately, same as catalog metadata below - no need
    // to wait for the model to arrive, and no guess to contradict it later.
    if (parsed.local && parsed.gear.isEmpty()) {
      block->irCategoryNeedsDurationGuess = true;
    } else {
      // Known immediately (catalog metadata, or the split zone's explicit
      // choice) - no need to wait for the model to arrive.
      block->irCategory = irCategoryFromGear(parsed.gear);
    }
  }

  DBG("Created tone block: " << parsed.toneId << " (block: " << blockId << ")");
  DBG("Queueing first model for background loading: " << parsed.modelName);

  // Resolve the slot the tone lands in: the insert the user clicked (looked
  // up across both lanes; ids are globally unique), or the active lane's
  // first insert when the id is stale/absent (chain edited mid-flow, or an
  // older UI that doesn't send one).
  Lane* targetLane = nullptr;
  Lane::iterator slot;
  if (!targetInsertId.empty()) {
    for (auto& l : lanes) {
      auto it = std::find_if(l.begin(), l.end(), [&](const std::unique_ptr<ChainBlock>& b) {
        return isInsertBlock(b) && b->id == targetInsertId;
      });
      if (it != l.end()) {
        targetLane = &l;
        slot = it;
        break;
      }
    }
  }
  if (targetLane == nullptr) {
    targetLane = &activeChain();
    slot = std::find_if(targetLane->begin(), targetLane->end(), isInsertBlock);
  }

  // The tone takes the slot's position; the consumed insert dies here (it has
  // no engines, so destroying it under the lock is fine). Alignment then
  // re-pads the lane, which appends a fresh trailing insert once every
  // minimum slot holds a tone, and keeps a branched layout's lane ends even.
  if (slot != targetLane->end())
    *slot = std::move(block);
  else
    targetLane->push_back(std::move(block));
  alignBranchLaneLengths();

  bumpChainRevision();
  queueToneLoad(blockId, parsed.firstModelId, parsed.modelUrl, parsed.modelName, parsed.type);

  return blockId;
}

std::string TONE3000Processor::addEqBlock(const std::string& targetInsertId) {
  // Structural like duplicate/paste (a whole new block splices into the
  // running chain), so mute-splice instead of relying on one block's own
  // wet fade.
  ChainEditFade editFade(*this);
  juce::ScopedLock lock(chainMutex);

  pushChainHistory();

  const std::string blockId = juce::Uuid().toString().toStdString();
  auto block = std::make_unique<ChainBlock>(blockId, ChainBlockType::EQ);

  // Synthetic tone: just enough for makeToneSummary/the UI's title and
  // GearIcon fallback to have something to show. No catalog entry, no
  // model, nothing to download - see ChainBlockType::EQ's own comment for
  // why this skips the entire tone-loading pipeline every other type goes
  // through.
  juce::DynamicObject::Ptr tone = new juce::DynamicObject();
  tone->setProperty("id", 0);
  tone->setProperty("local", true);
  tone->setProperty("title", "EQ");
  tone->setProperty("format", "eq");
  const juce::var toneVar(tone.get());
  setToneOnBlock(*block, 0, juce::JSON::toString(toneVar, true), toneVar);
  // Nothing to load: the block IS its own content from the moment it
  // exists. mixNormalized/inputGainNormalized/outputGainNormalized all
  // keep their class defaults (1.0/0.5/0.5 - full wet, unity, unity); there
  // is no Mix or In Gain control in the UI for this type, so nothing ever
  // moves them off those defaults.
  block->loaded = true;

  // Same slot-resolution as loadTone: the insert the user right-clicked
  // (looked up across both lanes; ids are globally unique), or the active
  // lane's first insert when the id is stale/absent.
  Lane* targetLane = nullptr;
  Lane::iterator slot;
  if (!targetInsertId.empty()) {
    for (auto& l : lanes) {
      auto it = std::find_if(l.begin(), l.end(), [&](const std::unique_ptr<ChainBlock>& b) {
        return isInsertBlock(b) && b->id == targetInsertId;
      });
      if (it != l.end()) {
        targetLane = &l;
        slot = it;
        break;
      }
    }
  }
  if (targetLane == nullptr) {
    targetLane = &activeChain();
    slot = std::find_if(targetLane->begin(), targetLane->end(), isInsertBlock);
  }

  if (slot != targetLane->end())
    *slot = std::move(block);
  else
    targetLane->push_back(std::move(block));
  alignBranchLaneLengths();

  bumpChainRevision();
  return blockId;
}

std::string TONE3000Processor::addDualMonoBlock(const std::string& targetInsertId) {
  // Same shape as addEqBlock: structural, mute-splice, synthetic tone,
  // synchronous (no model of its own to fetch).
  ChainEditFade editFade(*this);
  juce::ScopedLock lock(chainMutex);

  pushChainHistory();

  const std::string blockId = juce::Uuid().toString().toStdString();
  auto block = std::make_unique<ChainBlock>(blockId, ChainBlockType::DUAL_MONO);

  juce::DynamicObject::Ptr tone = new juce::DynamicObject();
  tone->setProperty("id", 0);
  tone->setProperty("local", true);
  tone->setProperty("title", "Dual Mono");
  tone->setProperty("format", "dualMono");
  const juce::var toneVar(tone.get());
  setToneOnBlock(*block, 0, juce::JSON::toString(toneVar, true), toneVar);
  // Nothing to load: the block itself has no tone of its own - its two
  // children (empty until the UI loads content into a side) do. Both
  // dualLeft/dualRight start empty (see ChainBlock.h's own comment).
  block->loaded = true;

  // The Pan/Width smoothers must be reset (which configures the ramp
  // *duration*, not just the target) right here: this block comes to life
  // synchronously, not through prepareChain's next pass (that only
  // revisits blocks that already existed at prepareToPlay/rate-change
  // time). Same duration prepareChain's own DUAL_MONO branch uses.
  block->dualLeftPanSmoother.reset(chainSampleRate(), 0.05f);
  block->dualRightPanSmoother.reset(chainSampleRate(), 0.05f);
  block->dualWidthSmoother.reset(chainSampleRate(), 0.05f);
  block->dualLeftSoloGainSmoother.reset(chainSampleRate(), 0.05f);
  block->dualRightSoloGainSmoother.reset(chainSampleRate(), 0.05f);
  block->dualLeftPanSmoother.setCurrentAndTargetValue(block->dualLeftPanNormalized);
  block->dualRightPanSmoother.setCurrentAndTargetValue(block->dualRightPanNormalized);
  block->dualWidthSmoother.setCurrentAndTargetValue(block->dualWidthNormalized);
  // Neither side soloed at creation - both start at full gain.
  block->dualLeftSoloGainSmoother.setCurrentAndTargetValue(1.0f);
  block->dualRightSoloGainSmoother.setCurrentAndTargetValue(1.0f);
  block->dualLeftPolaritySmoother.reset(chainSampleRate(), 0.05f);
  block->dualRightPolaritySmoother.reset(chainSampleRate(), 0.05f);
  // Neither side inverted at creation - both start at normal (+1) polarity.
  block->dualLeftPolaritySmoother.setCurrentAndTargetValue(1.0f);
  block->dualRightPolaritySmoother.setCurrentAndTargetValue(1.0f);
  // Same reasoning: dualAlign comes to life synchronously too, not through
  // prepareChain's next pass.
  block->dualAlign.prepare(chainSampleRate(), chainDomainBlockSize());
  block->dualGoniometer.prepare(chainSampleRate());

  // Same slot-resolution as addEqBlock.
  Lane* targetLane = nullptr;
  Lane::iterator slot;
  if (!targetInsertId.empty()) {
    for (auto& l : lanes) {
      auto it = std::find_if(l.begin(), l.end(), [&](const std::unique_ptr<ChainBlock>& b) {
        return isInsertBlock(b) && b->id == targetInsertId;
      });
      if (it != l.end()) {
        targetLane = &l;
        slot = it;
        break;
      }
    }
  }
  if (targetLane == nullptr) {
    targetLane = &activeChain();
    slot = std::find_if(targetLane->begin(), targetLane->end(), isInsertBlock);
  }

  if (slot != targetLane->end())
    *slot = std::move(block);
  else
    targetLane->push_back(std::move(block));
  alignBranchLaneLengths();

  bumpChainRevision();
  return blockId;
}

std::string TONE3000Processor::loadToneIntoDualSlot(const std::string& dualBlockId,
                                                     bool isLeftSide,
                                                     const juce::String& toneJsonString) {
  const ParsedTone parsed = parseToneForLoading(toneJsonString);
  if (!parsed.valid)
    return "";

  // Declared before the lock/fade so it destructs AFTER both release: engine
  // teardown is heavy (same reasoning as removeChainBlock's own oldChild).
  std::unique_ptr<ChainBlock> oldChild;
  // Structural like addEqBlock/addDualMonoBlock - mute-splice unconditionally
  // rather than only when replacing an already-loaded child: simpler, and a
  // fresh load into an empty side (unloaded either way) costs nothing
  // audible from the splice.
  ChainEditFade editFade(*this);
  juce::ScopedLock lock(chainMutex);

  ChainBlock* dualBlock = findBlockById(dualBlockId);
  if (dualBlock == nullptr || dualBlock->type != ChainBlockType::DUAL_MONO)
    return "";

  pushChainHistory();

  const std::string blockId = juce::Uuid().toString().toStdString();
  auto block = std::make_unique<ChainBlock>(blockId, parsed.type);
  setToneOnBlock(*block, parsed.toneId, parsed.toneJson, parsed.toneVar);
  block->activeModelId = parsed.firstModelId;
  block->namSlimSize = namSlimSizeDefault.load();
  block->loaded = false;
  block->modelLoading = true;
  block->applyDefaultMixOnLoad = true;

  if (parsed.type == ChainBlockType::IR) {
    if (parsed.local && parsed.gear.isEmpty())
      block->irCategoryNeedsDurationGuess = true;
    else
      block->irCategory = irCategoryFromGear(parsed.gear);
  }

  auto& side = isLeftSide ? dualBlock->dualLeft : dualBlock->dualRight;
  if (!side.empty())
    oldChild = std::move(side[0]);
  side.clear();
  side.push_back(std::move(block));

  bumpChainRevision();
  queueToneLoad(blockId, parsed.firstModelId, parsed.modelUrl, parsed.modelName, parsed.type);

  return blockId;
}

bool TONE3000Processor::removeDualSlotContent(const std::string& dualBlockId, bool isLeftSide) {
  std::unique_ptr<ChainBlock> oldChild;  // destroyed after lock/fade release
  ChainEditFade editFade(*this);
  juce::ScopedLock lock(chainMutex);

  ChainBlock* dualBlock = findBlockById(dualBlockId);
  if (dualBlock == nullptr || dualBlock->type != ChainBlockType::DUAL_MONO)
    return false;

  auto& side = isLeftSide ? dualBlock->dualLeft : dualBlock->dualRight;
  if (side.empty())
    return true;  // already empty

  pushChainHistory();
  oldChild = std::move(side[0]);
  side.clear();
  bumpChainRevision();
  return true;
}

std::string TONE3000Processor::landToneBlock(std::unique_ptr<ChainBlock> block,
                                             const juce::String& side, int index) {
  const std::string newId = block->id;
  Lane& target = side == "right" ? lane(ChainSide::Right) : lane(ChainSide::Left);
  index = juce::jlimit(0, static_cast<int>(target.size()), index);
  if (index < static_cast<int>(target.size()) && isInsertBlock(target[static_cast<size_t>(index)]))
    target[static_cast<size_t>(index)] = std::move(block);  // paste fills the empty tile
  else
    target.insert(target.begin() + index, std::move(block));
  alignBranchLaneLengths();

  bumpChainRevision();
  queueActiveModelLoad(*findBlockById(newId));
  return newId;
}

std::string TONE3000Processor::duplicateChainBlock(const std::string& sourceBlockId,
                                                   const juce::String& side, int index) {
  // Structural like reorder/move (a whole new block splices into the running
  // chain), so mute-splice instead of relying on one block's wet fade.
  ChainEditFade editFade(*this);
  juce::ScopedLock lock(chainMutex);

  const ChainBlock* source = findBlockById(sourceBlockId);
  if (source == nullptr || source->type == ChainBlockType::INSERT) {
    DBG("duplicateChainBlock: source not a tone block: " << sourceBlockId);
    return "";
  }
  if (side == "right" && !stereoEnabled.load()) {
    DBG("duplicateChainBlock: right lane requires stereo mode");
    return "";
  }

  pushChainHistory();

  // The settings ride the same per-block tree the undo/state paths use, so
  // "everything the block remembers" stays defined in exactly one place
  // (serializeBlockSettings/applyBlockSettings: gains, mix, enabled,
  // normalize, A2 size, EQ). Tone identity and model bytes are copied
  // directly: the clone re-loads its model cache-first, so it comes up
  // without a network round trip and sounds identical the moment the engine
  // lands.
  const std::string newId = juce::Uuid().toString().toStdString();
  auto clone = std::make_unique<ChainBlock>(newId, source->type);
  applyBlockSettings(*clone, serializeBlockSettings(*source));
  setToneOnBlock(*clone, source->toneId, source->toneJson, source->toneVar);
  clone->activeModelId = source->activeModelId;
  clone->modelCache = source->modelCache;
  clone->loaded = false;
  clone->modelLoading = true;
  clone->applyDefaultMixOnLoad = false;  // the copied mix is a setting, not a default

  landToneBlock(std::move(clone), side, index);
  DBG("Duplicated block " << sourceBlockId << " -> " << newId << " (" << side << " @ " << index
                          << ")");
  return newId;
}

bool TONE3000Processor::copyChainBlock(const std::string& blockId) {
  juce::ScopedLock lock(chainMutex);

  const ChainBlock* source = findBlockById(blockId);
  if (source == nullptr || source->type == ChainBlockType::INSERT) {
    DBG("copyChainBlock: source not a tone block: " << blockId);
    return false;
  }

  // A self-contained snapshot, not a reference: the same per-block tree the
  // undo/preset paths persist, plus the in-memory model bytes so a later
  // paste comes up offline. Copying never touches the chain, so no history
  // entry; the revision bump only publishes `canPasteBlock` to the UI.
  blockClipboardSettings = serializeBlockSettings(*source);
  blockClipboardModelCache = source->modelCache;

  bumpChainRevision();
  DBG("Copied block " << blockId << " to the block clipboard");
  return true;
}

std::string TONE3000Processor::pasteChainBlock(const juce::String& side, int index) {
  // Structural like duplicate: a whole new block lands in the running chain.
  ChainEditFade editFade(*this);
  juce::ScopedLock lock(chainMutex);

  if (!blockClipboardSettings.isValid()) {
    DBG("pasteChainBlock: clipboard is empty");
    return "";
  }
  if (side == "right" && !stereoEnabled.load()) {
    DBG("pasteChainBlock: right lane requires stereo mode");
    return "";
  }

  pushChainHistory();

  // Rebuild from the snapshot the way undo/preset restores do (fresh id: the
  // copied block may still be in the chain, and ids are globally unique).
  // The tone JSON is re-parsed rather than shared so nothing in the live
  // chain can mutate the clipboard behind our back (see switchModel, which
  // edits a block's toneVar in place).
  const std::string newId = juce::Uuid().toString().toStdString();
  const ChainBlockType type =
      chainBlockTypeFromString(blockClipboardSettings.getProperty("type").toString());
  auto block = std::make_unique<ChainBlock>(newId, type);
  applyBlockSettings(*block, blockClipboardSettings);
  const juce::String toneJson = blockClipboardSettings.getProperty("toneJson").toString();
  setToneOnBlock(*block, blockClipboardSettings.getProperty("toneId", 0), toneJson,
                 juce::JSON::parse(toneJson));
  block->activeModelId = blockClipboardSettings.getProperty("activeModelId", 0);
  block->modelCache = blockClipboardModelCache;
  block->loaded = false;
  block->modelLoading = true;
  block->applyDefaultMixOnLoad = false;  // the copied mix is a setting, not a default

  landToneBlock(std::move(block), side, index);
  DBG("Pasted clipboard block -> " << newId << " (" << side << " @ " << index << ")");
  return newId;
}

bool TONE3000Processor::swapTone(const std::string& blockId, const juce::String& toneJsonString) {
  juce::ScopedLock lock(chainMutex);

  ChainBlock* block = findBlockById(blockId);
  if (block == nullptr || block->type == ChainBlockType::INSERT) {
    DBG("swapTone: block not found or is insert block: " << blockId);
    return false;
  }

  const ParsedTone parsed = parseToneForLoading(toneJsonString);
  if (!parsed.valid)
    return false;

  pushChainHistory();

  // Replace the tone in place: same block id (chain position preserved). A
  // catalog swap keeps the user's params (enabled/gains/mix/envelope) - the
  // old engines (and the block type they belong to) stay live and keep
  // processing until the new model is spliced in by
  // applyPreparedModelToChainBlock (which also stamps the new type);
  // `modelLoading` drives the UI's loading state meanwhile.
  setToneOnBlock(*block, parsed.toneId, parsed.toneJson, parsed.toneVar);
  block->activeModelId = parsed.firstModelId;
  block->modelLoading = true;
  block->loadFailed = false;
  block->modelCache.clear();

  // Bug fix, 2026-09-17: a *local* file drop onto an already-occupied tile
  // (GalleryBlock.tsx's own drop, including the split IR/Cab zone) kept
  // every one of the old content's mix/envelope values, when the user
  // expects a dropped file to behave like a fresh add of that type - Mix in
  // particular has a real per-category rule (Cab 100%, IrPlayer 50%, see
  // applyPreparedModelToChainBlock) that a stale carried-over value
  // silently violates. Scoped to `parsed.local` specifically: a *catalog*
  // swap (Select-flow's swap button) is deliberately NOT included here -
  // that's the "keep the user's params" case the comment above describes,
  // unchanged. Mirrors loadTone's own unconditional default-mix arming for
  // a fresh add; applyPreparedModelToChainBlock consumes and clears this
  // flag once applied either way.
  if (parsed.local)
    block->applyDefaultMixOnLoad = true;

  // Bug fix, 2026-09-17: irCategory/irCategoryNeedsDurationGuess previously
  // carried straight over from whatever the block's *previous* content had
  // - loadToneInBackground (shared with a fresh loadTone add, via
  // queueToneLoad below) reads them assuming the caller already resolved
  // them for the tone now being loaded, which loadTone's own add path does
  // but this swap path never did. Invisible for catalog-to-catalog swaps
  // (every non-cab catalog tone resolves to the same IrPlayer regardless),
  // but a real bug for any local drop landing on an already-occupied tile
  // (GalleryBlock.tsx): a plain unlabeled swap kept reusing the previous
  // content's category instead of re-guessing the new file's own duration,
  // and the empty-slot split zone's explicit IR/Cab choice (forceGear) had
  // no effect at all once swapped onto an occupied tile - exactly mirrors
  // loadTone's own resolution now, so both cases behave identically
  // whether the drop lands on an empty slot or an existing tile.
  if (parsed.type == ChainBlockType::IR) {
    if (parsed.local && parsed.gear.isEmpty()) {
      block->irCategoryNeedsDurationGuess = true;
    } else {
      block->irCategory = irCategoryFromGear(parsed.gear);
      block->irCategoryNeedsDurationGuess = false;
    }
  }

  DBG("Swapped tone on block " << blockId << " -> tone " << parsed.toneId);

  bumpChainRevision();
  queueToneLoad(blockId, parsed.firstModelId, parsed.modelUrl, parsed.modelName, parsed.type);

  return true;
}

bool TONE3000Processor::refreshToneMetadata(const juce::String& toneJsonString) {
  const juce::var freshVar = juce::JSON::parse(toneJsonString);
  juce::DynamicObject* fresh = freshVar.getDynamicObject();
  if (fresh == nullptr)
    return false;
  const int toneId = fresh->getProperty("id");
  if (toneId == 0)
    return false;

  juce::ScopedLock lock(chainMutex);

  bool changed = false;
  for (const ChainSide side : {ChainSide::Left, ChainSide::Right}) {
    for (auto& block : lane(side)) {
      if (block->type == ChainBlockType::INSERT || block->toneId != toneId)
        continue;
      // Local tones have no catalog behind them; a same-id API tone is a
      // different thing entirely and must never overwrite one.
      if (static_cast<bool>(block->toneVar["local"]))
        continue;

      // Fresh payload wholesale, except the stored models array: native
      // persists only the active model, and queueActiveModelLoad / retry /
      // switchModel resolve the download URL from that entry. The API
      // payload's models list has no such guarantee.
      juce::var mergedVar = freshVar.clone();
      mergedVar.getDynamicObject()->setProperty("models", block->toneVar["models"]);
      const juce::String mergedJson = juce::JSON::toString(mergedVar);
      if (mergedJson == block->toneJson)
        continue;

      setToneOnBlock(*block, toneId, mergedJson, mergedVar);
      changed = true;
    }
  }

  if (changed) {
    DBG("Refreshed metadata for tone " << toneId);
    bumpChainRevision();
  }
  return changed;
}

bool TONE3000Processor::switchModel(const std::string& blockId, int modelId,
                                    const juce::var& modelData) {
  juce::ScopedLock lock(chainMutex);

  ChainBlock* block = findBlockById(blockId);
  if (block == nullptr) {
    DBG("Block not found: " << blockId);
    return false;
  }

  if (!block->toneVar.isObject()) {
    DBG("Block has no parsed tone metadata");
    return false;
  }

  // Native only stores the *active* model (the catalog lives on the API and
  // the UI pages it in), so the switch always carries the full model object.
  juce::DynamicObject* model = modelData.getDynamicObject();
  if (model == nullptr || static_cast<int>(model->getProperty("id")) != modelId ||
      model->getProperty("model_url").toString().isEmpty()) {
    DBG("switchModel: missing or invalid model data for ID: " << modelId);
    return false;
  }

  const juce::String modelUrl = model->getProperty("model_url").toString();
  const juce::String modelName = model->getProperty("name").toString();

  DBG("Queueing model switch: " << modelName << " (ID: " << modelId << ")");

  pushChainHistory();

  // The new model becomes the tone's sole stored model. Local tones keep
  // their full model list instead (the picked model is already in it; see
  // parseToneForLoading), so only the active id moves.
  if (!static_cast<bool>(block->toneVar["local"])) {
    juce::Array<juce::var> models;
    models.add(modelData);
    block->toneVar.getDynamicObject()->setProperty("models", models);
    block->toneJson = juce::JSON::toString(block->toneVar);
    block->toneSummary = makeToneSummary(block->toneVar);
  }

  // The previous engine keeps processing (loaded stays true) while the new
  // model downloads/prepares; the swap itself is spliced in with a fade.
  block->activeModelId = modelId;
  block->modelLoading = true;
  block->loadFailed = false;
  bumpChainRevision();

  struct SwitchModelJob : public juce::ThreadPoolJob {
    TONE3000Processor& processor;
    std::string blockId;
    int modelId;
    juce::String modelUrl;
    juce::String modelName;

    SwitchModelJob(TONE3000Processor& p, const std::string& bid, int mid,
                  const juce::String& url, const juce::String& name)
        : ThreadPoolJob("Switch Model"), processor(p), blockId(bid), modelId(mid), modelUrl(url),
          modelName(name) {}

    JobStatus runJob() override {
      processor.switchModelInBackground(blockId, modelId, modelUrl, modelName);
      return jobHasFinished;
    }
  };

  loadingThreadPool.addJob(new SwitchModelJob(*this, blockId, modelId, modelUrl, modelName), true);

  return true;
}

bool TONE3000Processor::setDualImage(const std::string& blockId, double leftPanNormalized,
                                     double rightPanNormalized, double widthNormalized) {
  juce::ScopedLock lock(chainMutex);
  ChainBlock* block = findBlockById(blockId);
  if (block == nullptr || block->type != ChainBlockType::DUAL_MONO) {
    DBG("setDualImage: not a DUAL_MONO block: " << blockId);
    return false;
  }

  // Continuous (called every drag tick) - coalesces a whole gesture into one
  // undo step, same idiom as setBlockParam's continuous params.
  pushChainHistory("param:" + juce::String(blockId) + ":dualImage");

  block->dualLeftPanNormalized = juce::jlimit(0.0f, 1.0f, static_cast<float>(leftPanNormalized));
  block->dualRightPanNormalized =
      juce::jlimit(0.0f, 1.0f, static_cast<float>(rightPanNormalized));
  block->dualWidthNormalized = juce::jlimit(0.0f, 1.0f, static_cast<float>(widthNormalized));
  // Smoothers take the new target only - runDualMono's recombine glides
  // toward it, never steps (see the fields' own comment in ChainBlock.h).
  block->dualLeftPanSmoother.setTargetValue(block->dualLeftPanNormalized);
  block->dualRightPanSmoother.setTargetValue(block->dualRightPanNormalized);
  block->dualWidthSmoother.setTargetValue(block->dualWidthNormalized);

  deferredRevisionBump();
  return true;
}

// UI-only toggle at heart (see the field's own comment in ChainBlock.h) -
// native's whole job here is remembering the on/off state across undo/
// redo, state restore, and a second open editor window, same as any other
// per-block bool (trimInitEnabled, reverseEnabled, ...). The actual
// mirror/sync behavior lives in the UI, re-sending both sides' Pan/Mix/Vol
// through the existing setDualImage/setBlockParam setters whenever linked.
bool TONE3000Processor::setDualLinked(const std::string& blockId, bool linked) {
  juce::ScopedLock lock(chainMutex);
  ChainBlock* block = findBlockById(blockId);
  if (block == nullptr || block->type != ChainBlockType::DUAL_MONO) {
    DBG("setDualLinked: not a DUAL_MONO block: " << blockId);
    return false;
  }
  if (block->dualLinked == linked)
    return true;

  pushChainHistory();
  block->dualLinked = linked;
  deferredRevisionBump();
  return true;
}

// Exclusive per-side solo (see the fields' own comment in ChainBlock.h) -
// setting one side's solo on always clears the other's, mirroring the
// chain-level stereo pan rail's own soloLeft/soloRight toggle behavior
// (GalleryLane.tsx). runDualMono reads dualSoloLeft/dualSoloRight live
// every call (same as the pan/width fields), gliding the actual gain
// change through dualLeftSoloGainSmoother/dualRightSoloGainSmoother so a
// live toggle mid-signal never clicks.
bool TONE3000Processor::setDualSolo(const std::string& blockId, bool isLeftSide, bool soloed) {
  juce::ScopedLock lock(chainMutex);
  ChainBlock* block = findBlockById(blockId);
  if (block == nullptr || block->type != ChainBlockType::DUAL_MONO) {
    DBG("setDualSolo: not a DUAL_MONO block: " << blockId);
    return false;
  }
  bool& thisSide = isLeftSide ? block->dualSoloLeft : block->dualSoloRight;
  bool& otherSide = isLeftSide ? block->dualSoloRight : block->dualSoloLeft;
  if (thisSide == soloed && (!soloed || !otherSide))
    return true;

  pushChainHistory();
  thisSide = soloed;
  if (soloed) otherSide = false;
  deferredRevisionBump();
  return true;
}

// Per-side Mute (see the fields' own comment in ChainBlock.h) - a plain
// persisted bool exactly like setDualLinked, no exclusivity to enforce
// (independent per side, unconditional whether the side is empty or
// loaded).
bool TONE3000Processor::setDualMuted(const std::string& blockId, bool isLeftSide, bool muted) {
  juce::ScopedLock lock(chainMutex);
  ChainBlock* block = findBlockById(blockId);
  if (block == nullptr || block->type != ChainBlockType::DUAL_MONO) {
    DBG("setDualMuted: not a DUAL_MONO block: " << blockId);
    return false;
  }
  bool& target = isLeftSide ? block->dualLeftMuted : block->dualRightMuted;
  if (target == muted)
    return true;

  pushChainHistory();
  target = muted;
  deferredRevisionBump();
  return true;
}

// Independent per side (unlike Solo's exclusivity) - see the fields' own
// comment in ChainBlock.h.
bool TONE3000Processor::setDualInvert(const std::string& blockId, bool isLeftSide,
                                      bool inverted) {
  juce::ScopedLock lock(chainMutex);
  ChainBlock* block = findBlockById(blockId);
  if (block == nullptr || block->type != ChainBlockType::DUAL_MONO) {
    DBG("setDualInvert: not a DUAL_MONO block: " << blockId);
    return false;
  }
  bool& target = isLeftSide ? block->dualLeftInvert : block->dualRightInvert;
  if (target == inverted)
    return true;

  pushChainHistory();
  target = inverted;
  deferredRevisionBump();
  return true;
}

// See the field's own comment (ChainBlock.h) - continuous (called on every
// drag tick, same as setDualImage), one call for the whole Align control
// surface since runDualMono only ever consumes these seven values together.
bool TONE3000Processor::setDualAlign(const std::string& blockId, bool enabled,
                                     double offsetNormalized, double wobbleNormalized,
                                     bool wobbleEnabled, double crossoverNormalized,
                                     bool crossoverEnabled, bool diffuseEnabled) {
  juce::ScopedLock lock(chainMutex);
  ChainBlock* block = findBlockById(blockId);
  if (block == nullptr || block->type != ChainBlockType::DUAL_MONO) {
    DBG("setDualAlign: not a DUAL_MONO block: " << blockId);
    return false;
  }

  pushChainHistory("param:" + juce::String(blockId) + ":dualAlign");

  block->dualAlignEnabled = enabled;
  block->dualAlignOffsetNormalized =
      juce::jlimit(0.0f, 1.0f, static_cast<float>(offsetNormalized));
  block->dualAlignWobbleNormalized =
      juce::jlimit(0.0f, 1.0f, static_cast<float>(wobbleNormalized));
  block->dualAlignWobbleEnabled = wobbleEnabled;
  block->dualAlignCrossoverNormalized =
      juce::jlimit(0.0f, 1.0f, static_cast<float>(crossoverNormalized));
  block->dualAlignCrossoverEnabled = crossoverEnabled;
  block->dualAlignDiffuseEnabled = diffuseEnabled;

  deferredRevisionBump();
  return true;
}

// See the field's own comment (ChainBlock.h) - a plain persisted bool
// exactly like setDualLinked.
bool TONE3000Processor::setDualStereoProcessingEnabled(const std::string& blockId, bool enabled) {
  juce::ScopedLock lock(chainMutex);
  ChainBlock* block = findBlockById(blockId);
  if (block == nullptr || block->type != ChainBlockType::DUAL_MONO) {
    DBG("setDualStereoProcessingEnabled: not a DUAL_MONO block: " << blockId);
    return false;
  }
  if (block->dualStereoProcessingEnabled == enabled)
    return true;

  pushChainHistory();
  block->dualStereoProcessingEnabled = enabled;
  deferredRevisionBump();
  return true;
}

bool TONE3000Processor::removeChainBlock(const std::string& blockId) {
  // Removal is bypass, so glide the block's wet mix to bypass first; then
  // detaching it is inaudible. Bounded wait (~one fade; skipped when audio
  // is stopped), on the message thread, imperceptible for a click gesture.
  requestSwapFadeAndWait(blockId);

  // Detached under the lock, destroyed after releasing it: engine teardown
  // (NAM graph, convolution state) is heavy and the audio thread may be
  // waiting on chainMutex.
  std::unique_ptr<ChainBlock> removed;
  {
    juce::ScopedLock lock(chainMutex);

    for (auto& chain : lanes) {
      auto it = std::find_if(
          chain.begin(), chain.end(),
          [&blockId](const std::unique_ptr<ChainBlock>& block) { return block->id == blockId; });
      if (it != chain.end()) {
        if (isInsertBlock(*it)) {
          DBG("Cannot remove insert block");
          return false;
        }
        pushChainHistory();
        removed = std::move(*it);
        chain.erase(it);
        // Dropping below the minimum grows the lane back to it (at the end);
        // removing the tapped block clears the branch, and a shortened trunk
        // re-aligns the branch lane's end.
        alignBranchLaneLengths();
        refreshIrTailLength();  // a long-tailed IR may just have left the chain
        bumpChainRevision();
        break;
      }
    }
  }

  if (removed == nullptr) {
    DBG("Failed to remove chain block: " << blockId << " (not found)");
    return false;
  }

  DBG("Removed chain block: " << blockId);
  return true;
}

bool TONE3000Processor::reorderChainBlocks(const std::vector<std::string>& newOrder) {
  // Reordering nonlinear blocks changes the chain's waveform discontinuously
  // (no single block to fade), so mute-splice: glide the chain output to
  // silence, apply, glide back (~25 ms each way; see ChainEditFade).
  ChainEditFade editFade(*this);
  juce::ScopedLock lock(chainMutex);

  // Both lanes render at once now, so the target chain is inferred from the
  // ids themselves: the order must be a permutation of exactly one lane.
  // (Block ids are globally unique across both chains.)
  auto isPermutationOf = [](const std::vector<std::unique_ptr<ChainBlock>>& chain,
                            const std::vector<std::string>& order) {
    if (order.size() != chain.size())
      return false;
    std::vector<std::string> chainIds, orderIds = order;
    for (const auto& block : chain)
      chainIds.push_back(block->id);
    std::sort(chainIds.begin(), chainIds.end());
    std::sort(orderIds.begin(), orderIds.end());
    return chainIds == orderIds;
  };

  std::vector<std::unique_ptr<ChainBlock>>* target = nullptr;
  for (auto& l : lanes)
    if (isPermutationOf(l, newOrder)) {
      target = &l;
      break;
    }

  if (target == nullptr) {
    DBG("Failed to reorder chain blocks: order is not a permutation of either chain");
    return false;
  }
  auto& chain = *target;

  pushChainHistory();

  std::vector<std::unique_ptr<ChainBlock>> reorderedBlocks;
  reorderedBlocks.reserve(chain.size());
  std::vector<std::unique_ptr<ChainBlock>> originalBlocks = std::move(chain);

  for (const std::string& blockId : newOrder) {
    auto it = std::find_if(
        originalBlocks.begin(), originalBlocks.end(),
        [&blockId](const std::unique_ptr<ChainBlock>& block) {
          return block && block->id == blockId;
        });
    reorderedBlocks.push_back(std::move(*it));
  }

  chain = std::move(reorderedBlocks);
  // The tap follows its block to the new position (a moved tap changes the
  // branch lane's indent, so its trailing inserts re-align).
  alignBranchLaneLengths();
  bumpChainRevision();
  DBG("Successfully reordered chain blocks (including insert block)");
  return true;
}

bool TONE3000Processor::moveBlockToChain(const std::string& blockId, const juce::String& side,
                                         int index) {
  // Cross-lane moves change both chains at once; mute-splice like reorder.
  ChainEditFade editFade(*this);
  juce::ScopedLock lock(chainMutex);

  if (!stereoEnabled.load()) {
    DBG("moveBlockToChain: only valid in stereo mode");
    return false;
  }

  auto& target = lane(side == "right" ? ChainSide::Right : ChainSide::Left);
  auto& source = lane(side == "right" ? ChainSide::Left : ChainSide::Right);

  auto it = std::find_if(source.begin(), source.end(),
                         [&blockId](const std::unique_ptr<ChainBlock>& block) {
                           return block && block->id == blockId;
                         });
  if (it == source.end()) {
    DBG("moveBlockToChain: block not found in the other lane: " << blockId);
    return false;
  }
  if (isInsertBlock(*it)) {
    DBG("moveBlockToChain: insert slots stay in their lane");
    return false;
  }

  pushChainHistory();

  auto block = std::move(*it);
  source.erase(it);
  index = juce::jlimit(0, static_cast<int>(target.size()), index);
  target.insert(target.begin() + index, std::move(block));

  // The tone count changed on both sides: the source may need a slot back,
  // the target may shed a (trailing) surplus one. The tapped block leaving
  // the trunk clears the branch; otherwise the lane ends re-align.
  alignBranchLaneLengths();

  bumpChainRevision();
  DBG("Moved block " << blockId << " to " << side << " chain at index " << index);
  return true;
}

// A background download/prepare failed: leave the block unloaded but flip
// loadFailed (with a revision bump) so the UI swaps its loading dots for a
// retry affordance instead of spinning forever.
void TONE3000Processor::markBlockLoadFailed(const std::string& blockId) {
  // The previous engine kept playing during the download; the UI already
  // shows the new tone/model, so on failure the block drops out of
  // processing to match, glided to bypass first, never spliced.
  requestSwapFadeAndWait(blockId);

  juce::ScopedLock lock(chainMutex);
  if (ChainBlock* block = findBlockById(blockId)) {
    juce::Logger::writeToLog("[ModelLoader] Load failed for block " + juce::String(blockId) +
                             ", showing retry");
    block->loaded = false;
    block->loadFailed = true;
    block->modelLoading = false;
    block->swapFadePending.store(false);  // never leave the block faded out
    bumpChainRevision();
  }
}

// Re-queue the block's active model (retry after a failed download). The
// background loader is cache-first, so this only hits the network for the
// bytes that actually failed to arrive.
bool TONE3000Processor::retryModelLoad(const std::string& blockId) {
  juce::ScopedLock lock(chainMutex);
  ChainBlock* block = findBlockById(blockId);
  if (block == nullptr || block->type == ChainBlockType::INSERT || !block->loadFailed)
    return false;

  block->loadFailed = false;
  block->modelLoading = true;
  bumpChainRevision();  // back to the loading state in the UI
  queueActiveModelLoad(*block);
  return true;
}

void TONE3000Processor::loadToneInBackground(const std::string& blockId, int firstModelId,
                                             const juce::String& modelUrl,
                                             const juce::String& modelName, ChainBlockType type) {
  DBG("[Background] Loading tone for block: " << blockId);

  std::vector<uint8_t> modelData = fetchModelFromUrl(modelUrl);
  if (modelData.empty()) {
    DBG("[Background] Failed to fetch model from URL");
    markBlockLoadFailed(blockId);
    return;
  }

  const juce::String filename =
      modelName + (type == ChainBlockType::NAM ? ".nam" : ".wav");

  double namSlimSize = 0.0;
  std::optional<IrCategory> knownCategory;
  {
    juce::ScopedLock lock(chainMutex);
    ChainBlock* block = findBlockById(blockId);
    if (block == nullptr) {
      juce::Logger::writeToLog("[Background] Tone load dropped, block not found: " +
                               juce::String(blockId));
      return;
    }

    block->modelCache[firstModelId] = modelData;
    namSlimSize = block->namSlimSize;
    if (type == ChainBlockType::IR && !block->irCategoryNeedsDurationGuess)
      knownCategory = block->irCategory;
  }

  PreparedBlockModel prepared =
      prepareBlockModelOffThread(type, modelData, filename, namSlimSize, knownCategory);
  const bool applied = prepared.success;

  // A swapped tone's previous engine may still be audibly processing; let
  // the audio thread fade it out before the outcome is applied. On success
  // the wet path mutes in place (the dry input is never exposed, see
  // ChainBlock.h); on failure the block is dropped from processing, so it
  // glides to bypass, which is what plays afterwards.
  requestSwapFadeAndWait(blockId, prepared.success);

  {
    juce::ScopedLock lock(chainMutex);

    ChainBlock* block = findBlockById(blockId);
    if (block == nullptr) {
      DBG("[Background] Block not found after prepare: " << blockId);
      return;
    }

    if (block->activeModelId != firstModelId) {
      // Superseded by a newer switch/swap while this one loaded; that job
      // owns the block's loading state now; just make sure the block isn't
      // left faded out.
      block->swapFadePending.store(false);
      return;
    }

    applyPreparedModelToChainBlock(*block, type, prepared);
    // Reapply this block's own existing shape onto the freshly swapped-in
    // content (see blockHasNonDefaultIrShape) - a swap alone never does
    // this, only setBlockIrDecay/Size/Width/TrimInit do.
    if (prepared.success && block->type == ChainBlockType::IR &&
        blockHasNonDefaultIrShape(*block))
      queueIrShapeRebuild(*this, *block, loadingThreadPool);
  }
  // `prepared` now holds the block's *previous* engines (if any); they are
  // destroyed here, after the lock; teardown is too heavy to hold it.

  if (applied) {
    DBG("[Background] Successfully loaded tone for block: " << blockId);
  }
}

void TONE3000Processor::switchModelInBackground(const std::string& blockId, int modelId,
                                                const juce::String& modelUrl,
                                                const juce::String& modelName) {
  DBG("[Background] Switching model for block: " << blockId << " to model ID: " << modelId);

  std::vector<uint8_t> modelData;
  bool needsFetch = false;
  ChainBlockType blockTypeForPrepare = ChainBlockType::NAM;
  double namSlimSize = 0.0;
  std::optional<IrCategory> knownCategory;

  {
    juce::ScopedLock lock(chainMutex);

    ChainBlock* block = findBlockById(blockId);
    if (block == nullptr) {
      juce::Logger::writeToLog("[Background] Load dropped, block not found: " + juce::String(blockId));
      return;
    }

    if (block->activeModelId != modelId) {
      juce::Logger::writeToLog("[Background] Load for model " + juce::String(modelId) +
                               " superseded before it started (block " + juce::String(blockId) + ")");
      return;
    }

    // Key the prepare off the *tone's* format, not block->type: during an
    // in-flight tone swap the block keeps its previous type (that engine is
    // still processing) while this job builds the new tone's engine.
    blockTypeForPrepare = toneEngineType(block->toneVar, block->type);
    namSlimSize = block->namSlimSize;
    if (blockTypeForPrepare == ChainBlockType::IR && !block->irCategoryNeedsDurationGuess)
      knownCategory = block->irCategory;
    auto cacheIt = block->modelCache.find(modelId);

    if (cacheIt != block->modelCache.end()) {
      DBG("[Background] Using cached model data");
      modelData = cacheIt->second;
    } else {
      needsFetch = true;
    }
  }

  if (needsFetch) {
    DBG("[Background] Fetching model from URL: " << modelUrl);
    modelData = fetchModelFromUrl(modelUrl);

    if (modelData.empty()) {
      DBG("[Background] Failed to fetch model from URL");
      markBlockLoadFailed(blockId);
      return;
    }
  } else {
    // Local-model stash upkeep for cache-hit loads (fetches do their own in
    // fetchModelFromUrl); no-op for catalog URLs.
    refreshLocalStashCopy(modelUrl, modelData);
  }

  const juce::String filename =
      modelName + (blockTypeForPrepare == ChainBlockType::NAM ? ".nam" : ".wav");

  PreparedBlockModel prepared = prepareBlockModelOffThread(blockTypeForPrepare, modelData, filename,
                                                           namSlimSize, knownCategory);
  const bool applied = prepared.success;

  // The outgoing model keeps processing until this moment; fade it out on
  // the audio thread so the outcome can't click. On success the wet path
  // mutes in place (an engine swap must never expose the block's dry input:
  // at 100% mix that's a burst of the un-cabbed/un-ampped signal, see
  // ChainBlock.h); on a failed prepare the block is dropped, so it glides
  // to bypass instead.
  requestSwapFadeAndWait(blockId, prepared.success);

  {
    juce::ScopedLock lock(chainMutex);

    ChainBlock* block = findBlockById(blockId);
    if (block == nullptr) {
      DBG("[Background] Block was removed during fetch/install");
      return;
    }

    if (needsFetch) {
      block->modelCache[modelId] = modelData;
    }

    if (block->activeModelId != modelId) {
      // Superseded by a newer switch/swap while this one downloaded; that
      // job owns the block's loading state now. The bytes stay cached above.
      block->swapFadePending.store(false);
      return;
    }

    applyPreparedModelToChainBlock(*block, blockTypeForPrepare, prepared);
    // Same reapply as loadToneInBackground - see its own comment.
    if (prepared.success && block->type == ChainBlockType::IR &&
        blockHasNonDefaultIrShape(*block))
      queueIrShapeRebuild(*this, *block, loadingThreadPool);
  }
  // `prepared` now holds the block's *previous* engines (if any); they are
  // destroyed here, after the lock; teardown is too heavy to hold it.

  if (applied) {
    DBG("[Background] Successfully switched to model ID: " << modelId);
  }
}

bool TONE3000Processor::convertBlockType(const std::string& blockId,
                                         const juce::String& targetTypeStr) {
  const ChainBlockType targetType =
      targetTypeStr == "cab" ? ChainBlockType::CAB : ChainBlockType::IR;

  std::vector<uint8_t> modelData;
  juce::String modelName;

  {
    juce::ScopedLock lock(chainMutex);

    ChainBlock* block = findBlockById(blockId);
    if (block == nullptr || !block->loaded) {
      DBG("convertBlockType: block not found or not loaded: " << blockId);
      return false;
    }

    // Only a genuine IR<->CAB round trip; NAM/insert blocks (and a block
    // already at the target type) aren't eligible.
    const bool validDirection =
        (targetType == ChainBlockType::CAB && block->type == ChainBlockType::IR) ||
        (targetType == ChainBlockType::IR && block->type == ChainBlockType::CAB);
    if (!validDirection) {
      DBG("convertBlockType: not a valid IR<->CAB conversion for block: " << blockId);
      return false;
    }

    const int modelId = block->activeModelId;
    const auto cacheIt = block->modelCache.find(modelId);
    if (cacheIt == block->modelCache.end()) {
      // Shouldn't happen for a loaded block (see queueActiveModelLoad /
      // switchModelInBackground: every successful apply caches its bytes
      // first), but a currently-loading block could still be mid-download
      // with the previous model's cache entry not yet superseded.
      DBG("convertBlockType: no cached model bytes for block: " << blockId);
      return false;
    }
    modelData = cacheIt->second;

    if (const auto* modelsArr = block->toneVar["models"].getArray())
      for (const auto& modelVar : *modelsArr)
        if (static_cast<int>(modelVar["id"]) == modelId) {
          modelName = modelVar["name"].toString();
          break;
        }
    if (modelName.isEmpty())
      modelName = block->toneSummary["title"].toString();

    pushChainHistory();

    // The previous engine keeps processing (loaded stays true) while the
    // conversion prepares off-thread; it's spliced in with the same swap
    // fade a model switch uses.
    block->modelLoading = true;
    bumpChainRevision();
  }

  DBG("Queueing block type conversion: " << blockId << " -> " << targetTypeStr);

  struct ConvertBlockTypeJob : public juce::ThreadPoolJob {
    TONE3000Processor& processor;
    std::string blockId;
    ChainBlockType targetType;
    std::vector<uint8_t> modelData;
    juce::String filename;

    ConvertBlockTypeJob(TONE3000Processor& p, const std::string& bid, ChainBlockType type,
                        std::vector<uint8_t> data, const juce::String& name)
        : ThreadPoolJob("Convert Block Type"), processor(p), blockId(bid), targetType(type),
          modelData(std::move(data)), filename(name) {}

    JobStatus runJob() override {
      processor.convertBlockTypeInBackground(blockId, targetType, modelData, filename);
      return jobHasFinished;
    }
  };

  loadingThreadPool.addJob(
      new ConvertBlockTypeJob(*this, blockId, targetType, std::move(modelData),
                              modelName + ".wav"),
      true);

  return true;
}

void TONE3000Processor::convertBlockTypeInBackground(const std::string& blockId,
                                                      ChainBlockType targetType,
                                                      const std::vector<uint8_t>& modelData,
                                                      const juce::String& filename) {
  DBG("[Background] Converting block type: " << blockId << " -> "
                                              << chainBlockTypeToString(targetType));

  // Forced, not guessed: an explicit user conversion always lands on exactly
  // the type it asked for, regardless of what the source file's real content
  // looks like (see prepareBlockModelOffThread's isCabKnown - CAB is
  // unconditional from `type` alone; IrPlayer here is just as explicit, so
  // the round trip back never re-runs the duration guess IrCategory carries
  // for an ordinary first-time load).
  const std::optional<IrCategory> knownCategory =
      targetType == ChainBlockType::CAB ? std::optional<IrCategory>(IrCategory::Cab)
                                        : std::optional<IrCategory>(IrCategory::IrPlayer);

  PreparedBlockModel prepared =
      prepareBlockModelOffThread(targetType, modelData, filename, 0.0, knownCategory);

  // The previous engine keeps processing until this moment; fade it out on
  // the audio thread so the outcome can't click - same handshake as a model
  // switch (mute-in-place on success, drop to bypass on a failed prepare).
  requestSwapFadeAndWait(blockId, prepared.success);

  {
    juce::ScopedLock lock(chainMutex);

    ChainBlock* block = findBlockById(blockId);
    if (block == nullptr) {
      DBG("[Background] Block was removed during conversion");
      return;
    }

    applyPreparedModelToChainBlock(*block, targetType, prepared);
    // Same reapply as loadToneInBackground - see its own comment. Only ever
    // fires for the IrPlayer target; CAB carries no shaping fields at all.
    if (prepared.success && block->type == ChainBlockType::IR &&
        blockHasNonDefaultIrShape(*block))
      queueIrShapeRebuild(*this, *block, loadingThreadPool);

    // Converting back to IR Player always lands in that category explicitly
    // (applyPreparedModelToChainBlock only seeds irCategory from the
    // detected-duration guess when irCategoryNeedsDurationGuess is armed,
    // which it isn't here) - the button is labeled "IR Player", so that's
    // what the block becomes, never a re-guessed "Cab" off short content.
    if (prepared.success && targetType == ChainBlockType::IR) {
      block->irCategory = IrCategory::IrPlayer;
      block->irCategoryNeedsDurationGuess = false;
    }
  }
  // `prepared` now holds the block's *previous* engines (if any); they are
  // destroyed here, after the lock; teardown is too heavy to hold it.

  if (prepared.success) {
    DBG("[Background] Successfully converted block type: " << blockId);
  }
}

// Promote a settled continuous gesture (knob/EQ drag) into a real revision
// bump. Mid-gesture edits only record a timestamp (deferredRevisionBump);
// once the gesture has been quiet for kGestureSettleMs the next revision
// check converges everyone on the final values with a single full resync.
// Called by the editor's push timer and by getChainState.
juce::uint32 TONE3000Processor::getCurrentChainRevision() const {
  if (const auto pendingAt = pendingParamBumpAt.load(); pendingAt != 0 &&
      juce::Time::currentTimeMillis() - pendingAt >= kGestureSettleMs) {
    pendingParamBumpAt.store(0);
    bumpChainRevision();
  }
  return chainRevision.load();
}

juce::var TONE3000Processor::getChainState(int knownRevision) const {
  // Per-block copy of everything the payload needs. juce::String and
  // juce::var are refcounted, so a row copy is a handful of pointer bumps.
  // I deliberately defer the DynamicObject building (hash-map inserts, many
  // small allocations) until after chainMutex is released, so a UI resync
  // can't stall the audio thread on lock contention.
  struct BlockRow {
    juce::String id;
    bool isInsert = false;
    // Shipped for every tone row so the UI can tell a real ChainBlockType::
    // CAB block apart from IR/NAM without inferring it from tone.format
    // (CAB tones still report format "ir" from the catalog - gear is what
    // actually distinguishes them, and native has already resolved that
    // into the block's real type by load time).
    ChainBlockType blockType = ChainBlockType::IR;
    juce::var toneSummary;
    int toneId = 0;
    int activeModelId = 0;
    bool loaded = false, loadFailed = false, modelLoading = false, irLong = false;
    double irContentLengthMs = 0.0;
    double irRawContentLengthMs = 0.0;
    double irOnsetFraction = 0.0;
    IrCategory irCategory = IrCategory::IrPlayer;
    bool hasInputDbu = false, hasOutputDbu = false;
    double inputDbu = 0.0, outputDbu = 0.0;
    bool enabled = true, normalize = true;
    double slimSize = 0.0;
    float inputGain = 0.5f, outputGain = 0.5f, mix = 1.0f, predelay = 0.0f;
    float initLevel = 1.0f;
    float attackLength = 0.0f, attackCurve = 0.5f;
    float decayLength = 1.0f, decayLevel = 1.0f, decayCurve = 0.5f;
    float size = 0.5f, width = 0.75f;
    bool trimInit = false;
    bool trimRelaxed = false;
    bool reverse = false;
    int irNumChannels = 1;
    juce::var eq;
    bool rtFailed = false;
    // DUAL_MONO only: the two fixed child slots (each 0 or 1 row - see
    // ChainBlock::dualLeft/dualRight), recombine controls, and whether the
    // enclosing lane currently has a spare physical channel to widen into
    // (false = folds to mono; mirrors runDualMono's own widen-vs-fold
    // decision, which this can't read directly since it isn't inside the
    // audio callback - stereo mode means every lane runs with exactly one
    // physical channel, so any Dual Mono block in it must fold).
    std::vector<BlockRow> dualLeft, dualRight;
    float dualLeftPan = 0.0f, dualRightPan = 1.0f, dualWidth = 1.0f;
    bool dualChannelLimited = false;
    bool dualLinked = false, dualSoloLeft = false, dualSoloRight = false;
    bool dualLeftMuted = false, dualRightMuted = false;
    bool dualLeftInvert = false, dualRightInvert = false;
    bool dualAlignEnabled = false;
    float dualAlignOffset = 0.5f, dualAlignWobble = 0.25f, dualAlignCrossover = 0.5f;
    bool dualAlignWobbleEnabled = false, dualAlignCrossoverEnabled = false;
    bool dualAlignDiffuseEnabled = false;
    bool dualStereoProcessingEnabled = true;
  };

  juce::uint32 revision = 0;
  std::vector<BlockRow> left, right;
  bool stereo = false, canUndo = false, canRedo = false, branched = false;
  bool canPaste = false, atDefault = false;
  juce::String presetId, presetName, branchSide, activeSide, branchAfter;

  {
    juce::ScopedLock lock(chainMutex);

    // Read under the lock so the revision always matches the snapshot:
    // mutators bump the revision while holding chainMutex too. (Also promotes
    // any settled gesture edit into a bump.)
    revision = getCurrentChainRevision();

    // Cheap early-out for the UI poll loop when nothing changed.
    if (knownRevision >= 0 && static_cast<juce::uint32>(knownRevision) == revision) {
      juce::DynamicObject::Ptr unchanged = new juce::DynamicObject();
      unchanged->setProperty("revision", static_cast<int>(revision));
      unchanged->setProperty("unchanged", true);
      return unchanged.get();
    }

    // std::function, not auto: DUAL_MONO rows recurse into this same lambda
    // for their two child slots (a plain auto lambda can't reference
    // itself). `stereo` is captured by reference from the outer scope even
    // though it's assigned after this lambda is first used below - only
    // read once the lambda actually runs (during copyLane's own body,
    // after `stereo` is set), never during its construction.
    std::function<void(const Lane&, std::vector<BlockRow>&)> copyLane =
        [&copyLane, &stereo](const Lane& l, std::vector<BlockRow>& out) {
      out.reserve(l.size());
      for (const auto& block : l) {
        BlockRow row;
        row.id = juce::String(block->id);
        // Drain the audio thread's failure flag; the RT path can't build
        // strings or write logs, so it gets reported below, outside the lock.
        row.rtFailed = block->rtProcessingFailed.exchange(false);
        if (block->type == ChainBlockType::INSERT) {
          row.isInsert = true;
          out.push_back(std::move(row));
          continue;
        }
        row.blockType = block->type;
        row.toneSummary = block->toneSummary;
        row.toneId = block->toneId;
        row.activeModelId = block->activeModelId;
        row.loaded = block->loaded;
        row.loadFailed = block->loadFailed;
        row.modelLoading = block->modelLoading;
        row.irLong = block->type == ChainBlockType::IR && block->irIsLong;
        if (block->type == ChainBlockType::IR && block->irRawSampleRate > 0.0) {
          // Reflects Trim Init (see ChainBlock::trimInitEnabled/
          // irOnsetSamples) exactly like prepareIrShapeRebuild's own
          // trimStartSamples math - same source-duration reasoning,
          // duplicated by necessity (no shared code across the two call
          // sites). Scaled by Size's *clamped* duration ratio on top so the
          // displayed length/D Len always matches what prepareIrShapeRebuild
          // actually built, never a pre-clamp number the engine silently
          // capped (see kIrSizeMaxEffectiveSeconds).
          const int trimStartSamples =
              block->trimInitEnabled ? irEffectiveOnsetSamples(*block) : 0;
          const double contentDurationSeconds =
              juce::jmax(0, block->irContentLengthSamples - trimStartSamples) /
              block->irRawSampleRate;
          row.irRawContentLengthMs = contentDurationSeconds * 1000.0;
          row.irContentLengthMs = contentDurationSeconds * 1000.0 *
                                  irSizeClampedDurationRatio(block->sizeNormalized,
                                                             contentDurationSeconds);
          // Onset as a fraction of irContentLengthSamples specifically -
          // the exact span irWaveformPeaks was downsampled over
          // (computeIrWaveformPeaks) - not irRawContentLengthMs above,
          // which is already trim-adjusted when Trim Init is on and would
          // make this circular (see kIrSizeMaxEffectiveSeconds's own
          // circularity story for why that distinction matters). Always
          // shipped, regardless of trimInitEnabled - the UI decides whether
          // to crop the waveform backdrop with it. Picks the relaxed
          // detection's onset when trimRelaxed is set, same selector as
          // trimStartSamples above, so the dotted marker always matches
          // whichever onset would actually be used if trim were on.
          if (block->irContentLengthSamples > 0)
            row.irOnsetFraction = juce::jlimit(
                0.0, 1.0,
                static_cast<double>(irEffectiveOnsetSamples(*block)) /
                    block->irContentLengthSamples);
        }
        row.irCategory = block->irCategory;
        row.irNumChannels = block->irNumChannels;
        // NAM calibration metadata off the loaded engine, absent when the
        // model carries none. Non-finite values never ship; the JSON bridge
        // can't carry them (and the DSP rejects them too).
        if (block->type == ChainBlockType::NAM && block->namEngine != nullptr) {
          if (block->namEngine->hasInputLevel() &&
              std::isfinite(block->namEngine->getInputLevel())) {
            row.hasInputDbu = true;
            row.inputDbu = block->namEngine->getInputLevel();
          }
          if (block->namEngine->hasOutputLevel() &&
              std::isfinite(block->namEngine->getOutputLevel())) {
            row.hasOutputDbu = true;
            row.outputDbu = block->namEngine->getOutputLevel();
          }
        }
        row.enabled = block->enabled;
        row.normalize = block->normalizeEnabled;
        row.slimSize = block->namSlimSize;
        row.inputGain = block->inputGainNormalized;
        row.outputGain = block->outputGainNormalized;
        row.mix = block->mixNormalized;
        row.predelay = block->predelayNormalized;
        row.initLevel = block->initLevelNormalized;
        row.attackLength = block->attackLengthNormalized;
        row.attackCurve = block->attackCurveNormalized;
        row.decayLength = block->decayLengthNormalized;
        row.decayLevel = block->decayLevelNormalized;
        row.decayCurve = block->decayCurveNormalized;
        row.size = block->sizeNormalized;
        row.width = block->widthNormalized;
        row.trimInit = block->trimInitEnabled;
        row.trimRelaxed = block->trimRelaxed;
        row.reverse = block->reverseEnabled;
        row.eq = block->eq.toVar();

        if (block->type == ChainBlockType::DUAL_MONO) {
          copyLane(block->dualLeft, row.dualLeft);
          copyLane(block->dualRight, row.dualRight);
          row.dualLeftPan = block->dualLeftPanNormalized;
          row.dualRightPan = block->dualRightPanNormalized;
          row.dualWidth = block->dualWidthNormalized;
          row.dualChannelLimited = stereo;
          row.dualLinked = block->dualLinked;
          row.dualSoloLeft = block->dualSoloLeft;
          row.dualSoloRight = block->dualSoloRight;
          row.dualLeftMuted = block->dualLeftMuted;
          row.dualRightMuted = block->dualRightMuted;
          row.dualLeftInvert = block->dualLeftInvert;
          row.dualRightInvert = block->dualRightInvert;
          row.dualAlignEnabled = block->dualAlignEnabled;
          row.dualAlignOffset = block->dualAlignOffsetNormalized;
          row.dualAlignWobble = block->dualAlignWobbleNormalized;
          row.dualAlignWobbleEnabled = block->dualAlignWobbleEnabled;
          row.dualAlignCrossover = block->dualAlignCrossoverNormalized;
          row.dualAlignCrossoverEnabled = block->dualAlignCrossoverEnabled;
          row.dualAlignDiffuseEnabled = block->dualAlignDiffuseEnabled;
          row.dualStereoProcessingEnabled = block->dualStereoProcessingEnabled;
        }

        out.push_back(std::move(row));
      }
    };

    // Read before the first copyLane call - DUAL_MONO rows need it (see
    // copyLane's own capture comment) and it's cheap/stable to read this
    // early regardless.
    stereo = stereoEnabled.load();
    copyLane(lane(ChainSide::Left), left);
    if (stereo)
      copyLane(lane(ChainSide::Right), right);

    canUndo = chainHistory.canUndo();
    canRedo = chainHistory.canRedo();
    canPaste = blockClipboardSettings.isValid();
    atDefault = isChainAtDefault();
    presetId = activePresetId;
    presetName = activePresetName;
    branched = stereo && !branchAfterBlockId.empty();
    if (branched) {
      branchSide = branchSourceSide == ChainSide::Right ? "right" : "left";
      branchAfter = juce::String(branchAfterBlockId);
    }
    activeSide = pendingAddSide == ChainSide::Right ? "right" : "left";
  }

  // Lock released; build the payload. std::function, not auto: DUAL_MONO
  // rows recurse into this same lambda for their two child slots.
  std::function<juce::Array<juce::var>(const std::vector<BlockRow>&)> serializeChain =
      [&serializeChain](const std::vector<BlockRow>& rows) {
    juce::Array<juce::var> chainArray;
    for (const auto& row : rows) {
      if (row.rtFailed)
        juce::Logger::writeToLog("[NAM] Processing failed for block " + row.id +
                                 "; block disabled");

      juce::DynamicObject::Ptr item = new juce::DynamicObject();
      item->setProperty("blockId", row.id);
      item->setProperty("blockType", chainBlockTypeToString(row.blockType));

      if (row.isInsert) {
        item->setProperty("kind", "insert");
        chainArray.add(juce::var(item.get()));
        continue;
      }

      item->setProperty("kind", "tone");

      // Slim tone summary, nested (not spread) so runtime fields never
      // collide with tone fields. Built once when the tone was set (see
      // makeToneSummary) and shipped by reference.
      juce::var toneVar = row.toneSummary;
      if (!toneVar.isObject()) {
        // Corrupt toneJson: emit a minimal stand-in so the UI's `tone`
        // field is always an object.
        juce::DynamicObject::Ptr fallback = new juce::DynamicObject();
        fallback->setProperty("id", row.toneId);
        fallback->setProperty("title", "Tone " + juce::String(row.toneId));
        fallback->setProperty("models", juce::Array<juce::var>());
        toneVar = juce::var(fallback.get());
      }
      item->setProperty("tone", toneVar);

      item->setProperty("activeModelId", row.activeModelId);
      item->setProperty("loaded", row.loaded);
      item->setProperty("loadFailed", row.loadFailed);
      item->setProperty("modelLoading", row.modelLoading);
      // Engine-selection signal only (uniform vs non-uniform convolution);
      // no audible meaning any more - see irCategory below.
      item->setProperty("irLong", row.irLong);
      // Detected content length (ms), same window computeIrWaveformPeaks
      // trims the waveform display to - single source of truth. 0 for NAM
      // blocks and IR blocks not yet loaded. Already scaled by Size's
      // clamped ratio (see above), so it moves with the Size knob.
      item->setProperty("irContentLengthMs", row.irContentLengthMs);
      // Load-time content length (ms), *not* scaled by Size - the fixed
      // reference the Size knob's own display clamps against (see
      // sizePercentScale in knobScale.ts). Using irContentLengthMs there
      // instead would be circular: it already reflects the current clamped
      // Size ratio, so re-deriving the clamp from it converges wrong (a
      // knob stuck reading 100% once the engine ever clamped once).
      item->setProperty("irRawContentLengthMs", row.irRawContentLengthMs);
      // Detected onset (see computeIrOnsetSamples), as a fraction 0..1 of
      // irContentLengthSamples - the exact span the waveform backdrop
      // (irWaveformPeaks) was downsampled over. Always shipped regardless
      // of trimInit; the UI crops the backdrop with it only when trimInit
      // is on (see WaveformDisplay/IrEnvelopeGraph's startFraction prop).
      item->setProperty("irOnsetFraction", row.irOnsetFraction);
      // Explicit IR category ("cab"/"irPlayer"): drives the UI's Mix knob
      // default/Alt-click reset and the Out knob help (Cab carries the
      // -18 dB pad, IrPlayer doesn't). See IrCategory in ChainBlock.h.
      item->setProperty("irCategory", irCategoryToString(row.irCategory));
      // Channels in the loaded IR file (1 or 2), never re-derived in the UI
      // (see ChainBlock::irNumChannels) - the sole source of truth for
      // locking the Width knob on a mono-source IR.
      item->setProperty("irNumChannels", row.irNumChannels);

      if (row.hasInputDbu)
        item->setProperty("inputLevelDbu", row.inputDbu);
      if (row.hasOutputDbu)
        item->setProperty("outputLevelDbu", row.outputDbu);

      juce::DynamicObject::Ptr params = new juce::DynamicObject();
      params->setProperty("enabled", row.enabled);
      params->setProperty("normalize", row.normalize);
      params->setProperty("slimSize", row.slimSize);
      params->setProperty("inputGain", row.inputGain);
      params->setProperty("outputGain", row.outputGain);
      params->setProperty("mix", row.mix);
      params->setProperty("predelay", row.predelay);
      params->setProperty("initLevel", row.initLevel);
      params->setProperty("attackLength", row.attackLength);
      params->setProperty("attackCurve", row.attackCurve);
      params->setProperty("decayLength", row.decayLength);
      params->setProperty("decayLevel", row.decayLevel);
      params->setProperty("decayCurve", row.decayCurve);
      params->setProperty("size", row.size);
      params->setProperty("width", row.width);
      params->setProperty("trimInit", row.trimInit);
      params->setProperty("trimRelaxed", row.trimRelaxed);
      params->setProperty("reverse", row.reverse);
      params->setProperty("eq", row.eq);
      params->setProperty("dualLeftPan", row.dualLeftPan);
      params->setProperty("dualRightPan", row.dualRightPan);
      params->setProperty("dualWidth", row.dualWidth);
      params->setProperty("dualLinked", row.dualLinked);
      params->setProperty("dualSoloLeft", row.dualSoloLeft);
      params->setProperty("dualSoloRight", row.dualSoloRight);
      params->setProperty("dualLeftMuted", row.dualLeftMuted);
      params->setProperty("dualRightMuted", row.dualRightMuted);
      params->setProperty("dualLeftInvert", row.dualLeftInvert);
      params->setProperty("dualRightInvert", row.dualRightInvert);
      params->setProperty("dualAlignEnabled", row.dualAlignEnabled);
      params->setProperty("dualStereoProcessingEnabled", row.dualStereoProcessingEnabled);
      params->setProperty("dualAlignOffset", row.dualAlignOffset);
      params->setProperty("dualAlignWobble", row.dualAlignWobble);
      params->setProperty("dualAlignWobbleEnabled", row.dualAlignWobbleEnabled);
      params->setProperty("dualAlignCrossover", row.dualAlignCrossover);
      params->setProperty("dualAlignCrossoverEnabled", row.dualAlignCrossoverEnabled);
      params->setProperty("dualAlignDiffuseEnabled", row.dualAlignDiffuseEnabled);
      item->setProperty("params", juce::var(params.get()));

      // DUAL_MONO only: the two fixed child slots, nested the same shape as
      // the top-level chain/chainRight arrays (each is 0 or 1 item - see
      // ChainBlock::dualLeft/dualRight). dualChannelLimited mirrors
      // runDualMono's own widen-vs-fold decision (see BlockRow's own
      // comment) so the UI can dim Pan/Width when they're currently inert.
      if (row.blockType == ChainBlockType::DUAL_MONO) {
        item->setProperty("dualLeft", serializeChain(row.dualLeft));
        item->setProperty("dualRight", serializeChain(row.dualRight));
        item->setProperty("dualChannelLimited", row.dualChannelLimited);
      }

      chainArray.add(juce::var(item.get()));
    }
    return chainArray;
  };

  juce::DynamicObject::Ptr state = new juce::DynamicObject();
  state->setProperty("revision", static_cast<int>(revision));
  state->setProperty("chain", serializeChain(left));
  if (stereo)
    state->setProperty("chainRight", serializeChain(right));
  // History flags ride along with the chain state: they only ever change
  // together with a revision bump (mutation, undo/redo or a state load).
  state->setProperty("canUndo", canUndo);
  state->setProperty("canRedo", canRedo);
  // Whether the in-app block clipboard holds a copied block (Paste enabled
  // on insert slots). The clipboard snapshot is self-contained, so this
  // stays true across preset switches and after the source block is gone.
  state->setProperty("canPasteBlock", canPaste);
  // True when nothing distinguishes this state from a fresh instance (see
  // isChainAtDefault); the top bar's New button greys out on it.
  state->setProperty("atDefault", atDefault);
  if (presetId.isNotEmpty()) {
    juce::DynamicObject::Ptr preset = new juce::DynamicObject();
    preset->setProperty("id", presetId);
    preset->setProperty("name", presetName);
    state->setProperty("preset", juce::var(preset.get()));
  }
  state->setProperty("stereoEnabled", stereo);
  // Active branch (stereo mode only): which lane is the trunk and which of
  // its tone blocks feeds the other lane. Absent when the chains are
  // independent, and while mono, where a set branch lies dormant.
  if (branched) {
    juce::DynamicObject::Ptr branch = new juce::DynamicObject();
    branch->setProperty("side", branchSide);
    branch->setProperty("afterBlockId", branchAfter);
    state->setProperty("branch", juce::var(branch.get()));
  }
  state->setProperty("activeSide", activeSide);
  // True when a real stereo source feeds the plugin (stereo host bus or a
  // stereo standalone input device); drives the faceplate input-mode button
  // and the dual input meters.
  state->setProperty("stereoInput", stereoInputDetected.load());
  // Output-side twin: false on a mono rig (mono host bus, or a one-channel
  // standalone output device). Spread is idle then (the UI greys it out),
  // and stereo chains are summed to mono (see processImageStage): the UI
  // dims the pans and shows the MONO chip on the pan rail.
  state->setProperty("stereoOutput", stereoOutputDetected.load());
  // True in the standalone app; gates standalone-only settings.
  state->setProperty("standalone", isStandalone());
  state->setProperty("inputMode", inputModeToString(getInputMode()));
  // Machine-wide settings ride this payload because they change together
  // with a revision bump, like everything else Settings displays.
  state->setProperty("namSlimSizeDefault", namSlimSizeDefault.load());
  state->setProperty("multiCore", multiCoreEnabled.load());
  // The EQ editor mirrors the biquad math client-side; block EQs run in the
  // chain domain, so the drawn curve must use the live chain rate (48 kHz x
  // oversampling factor), not the host rate (see ChainDomain.h).
  state->setProperty("sampleRate", chainSampleRate());
  return state.get();
}

juce::var TONE3000Processor::getMeterLevels() const {
  juce::DynamicObject::Ptr root = new juce::DynamicObject();
  // Main meters ship as [L, R] pairs (mono sources report L == R). The UI
  // store derives the combined mono value as max(L, R).
  auto channelPair = [](float l, float r) {
    juce::Array<juce::var> pair;
    pair.add(l);
    pair.add(r);
    return juce::var(pair);
  };
  root->setProperty("input", channelPair(inputMeterLevelL.load(), inputMeterLevelR.load()));
  root->setProperty("output", channelPair(outputMeterLevelL.load(), outputMeterLevelR.load()));
  // Audio-callback load as a 0..1 proportion (the hint bar shows it as a %).
  root->setProperty("cpu", loadMeasurer.getLoadAsProportion());
  // Stereo-image output correlation (-1..1, 1 when the engine is idle) for
  // the mono-compatibility meter: whichever image engine the mode runs
  // (Spread in mono, the Align deck in stereo). Riding this poll costs no
  // extra bridge traffic.
  root->setProperty("correlation", stereoEnabled.load() ? stereoOffset.correlation()
                                                        : spread.correlation());

  juce::DynamicObject::Ptr blocks = new juce::DynamicObject();
  {
    // Meter values are atomics; the lock only guards chain iteration. Hold
    // time is a few property writes, so contention with the audio thread is
    // negligible even at per-frame polling rates.
    juce::ScopedLock lock(chainMutex);
    for (const auto& chain : lanes) {
      for (const auto& block : chain) {
        if (block->type == ChainBlockType::INSERT)
          continue;
        juce::DynamicObject::Ptr levels = new juce::DynamicObject();
        levels->setProperty("in", block->inputMeterDb.load());
        levels->setProperty("out", block->outputMeterDb.load());
        // Mono-safety readout for the block's own Stereo Processing screen -
        // the goniometer's own continuous correlation (see BlockGoniometer's
        // own comment for why this is NOT dualAlign.correlation(), which
        // freezes whenever Align itself isn't actively running).
        if (block->type == ChainBlockType::DUAL_MONO)
          levels->setProperty("alignCorrelation", block->dualGoniometer.correlation());
        blocks->setProperty(juce::String(block->id), juce::var(levels.get()));
      }
    }
  }
  root->setProperty("blocks", juce::var(blocks.get()));
  return root.get();
}

// ####################
// STEREO MODE
// ####################
void TONE3000Processor::setStereoMode(bool enabled) {
  // Mono ↔ stereo rewires the whole routing (one chain on both channels ↔
  // two independent lanes) with no single block to fade, so mute-splice like
  // reorder. Cheap early-out first: no fade when nothing changes.
  if (stereoEnabled.load() == enabled)
    return;

  ChainEditFade editFade(*this);
  juce::ScopedLock lock(chainMutex);

  if (stereoEnabled.load() == enabled)
    return;

  pushChainHistory();

  auto& right = lane(ChainSide::Right);
  stereoEnabled.store(enabled);

  if (!enabled)
    pendingAddSide = ChainSide::Left;

  // Branching only runs between two chains: dormant while mono (the fields
  // persist so toggling back re-engages the branch), live again in stereo.
  // Alignment also seeds the right chain's minimum slot layout the first
  // time stereo is enabled (legacy states that only carried one insert get
  // padded here too), and a re-engaging branch re-evens the lane ends
  // (mono edits may have changed the trunk's length underneath it).
  alignBranchLaneLengths();

  // A re-engaged branch means a single mono source again, so re-enforce the
  // input-mode invariant (the fold may have gone back to stereo while the
  // branch lay dormant).
  if (rtBranchTapIndex >= 0 && getInputMode() == InputMode::Stereo)
    inputMode.store(static_cast<int>(InputMode::Left));

  // Make sure the right chain's engines are ready to run in the chain domain.
  if (enabled)
    prepareChain(right);

  bumpChainRevision();
  DBG("Stereo mode " << (enabled ? "enabled" : "disabled"));
}

// ####################
// CHAIN BRANCHING
// ####################

// Re-resolve branchAfterBlockId into rtBranchTapIndex for the RT path.
// Clears the branch entirely when the tapped block is no longer a tone block
// in the trunk lane (removed, moved across, stale snapshot). Validated in
// mono mode too, so edits made while the branch is dormant can't leave a
// stale id behind. Called after every structural change; callers own
// history/revision/fade; this is pure bookkeeping.
void TONE3000Processor::refreshBranchTapIndex() {
  rtBranchTapIndex = -1;
  if (branchAfterBlockId.empty())
    return;

  const auto& trunk = lane(branchSourceSide);
  int tapIdx = -1;
  for (int i = 0; i < static_cast<int>(trunk.size()); ++i) {
    const auto& b = trunk[static_cast<size_t>(i)];
    if (b != nullptr && b->id == branchAfterBlockId && b->type != ChainBlockType::INSERT) {
      tapIdx = i;
      break;
    }
  }
  if (tapIdx == -1) {
    DBG("Branch tap block left the trunk lane; reverting to independent chains");
    branchAfterBlockId.clear();
    return;
  }

  // Dormant in mono mode: the fields persist (a mono round trip brings the
  // branch back) but the RT path ignores them (like the right lane itself).
  if (stereoEnabled.load())
    rtBranchTapIndex = tapIdx;
}

// Keep the lanes' visible ends even while a branch is active. The branch
// lane renders indented past the trunk's tap gap (its input is that trunk
// prefix's output), so with both lanes at the per-lane baseline its rail
// overshoots the trunk's end by the whole indent, all trailing empty
// placeholders. Those are free real estate, so trim them (never below the
// one insert every lane keeps) until both lanes end on the same slot
// column. Trim-only, best-effort: no lane ever grows extra placeholders
// past the per-lane baseline, tone blocks never move, so lanes that
// genuinely need to be uneven (a long trunk, or branch tones running past
// the trunk's end) simply stay uneven. Without an active branch this
// restores the plain per-lane baseline. Pure bookkeeping like
// normalizeLaneInserts: callers own history/revision/fade.
void TONE3000Processor::alignBranchLaneLengths() {
  // Baseline first (also undoes earlier alignment, so branch moves never
  // compound), then re-resolve the tap: insert churn can shift its index.
  normalizeLaneInserts(lane(ChainSide::Left));
  normalizeLaneInserts(lane(ChainSide::Right));
  refreshBranchTapIndex();
  if (rtBranchTapIndex < 0)
    return;

  const Lane& trunk = lane(branchSourceSide);
  Lane& branchLane =
      lane(branchSourceSide == ChainSide::Left ? ChainSide::Right : ChainSide::Left);

  // The branch lane's first tile sits `indent` slot columns into the trunk.
  const int indent = rtBranchTapIndex + 1;
  const int targetSlots = static_cast<int>(trunk.size()) - indent;

  int inserts =
      static_cast<int>(std::count_if(branchLane.begin(), branchLane.end(), isInsertBlock));
  while (static_cast<int>(branchLane.size()) > targetSlots && inserts > 1 &&
         isInsertBlock(branchLane.back())) {
    branchLane.pop_back();
    --inserts;
  }
}

bool TONE3000Processor::setChainBranch(const juce::String& side,
                                       const std::string& afterBlockId) {
  // Rerouting one whole lane's input is structural (no single block to
  // fade), so mute-splice like reorder.
  ChainEditFade editFade(*this);
  juce::ScopedLock lock(chainMutex);

  if (!stereoEnabled.load()) {
    DBG("setChainBranch: only valid in stereo mode");
    return false;
  }

  const ChainSide trunkSide = side == "right" ? ChainSide::Right : ChainSide::Left;
  const auto& trunk = lane(trunkSide);
  const bool tapExists =
      std::any_of(trunk.begin(), trunk.end(), [&](const std::unique_ptr<ChainBlock>& b) {
        return b != nullptr && b->id == afterBlockId && b->type != ChainBlockType::INSERT;
      });
  if (!tapExists) {
    DBG("setChainBranch: tap block not a tone block in the " << side << " lane: "
                                                             << afterBlockId);
    return false;
  }

  // Re-pointing to the spot already tapped is a no-op: no history entry,
  // no revision bump (the UI can re-fire on fast clicks).
  if (branchSourceSide == trunkSide && branchAfterBlockId == afterBlockId)
    return true;

  pushChainHistory();

  branchSourceSide = trunkSide;
  branchAfterBlockId = afterBlockId;
  // The branch lane just gained an indent past the tap gap; its surplus
  // trailing insert placeholders trim away to end level with the trunk.
  alignBranchLaneLengths();

  // A branched chain has a single (mono) source: the trunk's channel. A
  // stereo input fold would silently drop the other channel, so force a
  // definite pick; the UI hides the "stereo" option while branched.
  if (getInputMode() == InputMode::Stereo)
    inputMode.store(static_cast<int>(InputMode::Left));

  bumpChainRevision();
  DBG("Chain branch set: " << side << " after block " << afterBlockId);
  return true;
}

bool TONE3000Processor::clearChainBranch() {
  {
    juce::ScopedLock lock(chainMutex);
    if (branchAfterBlockId.empty())
      return false;  // nothing to clear: no fade, no history entry
  }

  ChainEditFade editFade(*this);
  juce::ScopedLock lock(chainMutex);
  if (branchAfterBlockId.empty())
    return false;

  pushChainHistory();
  branchAfterBlockId.clear();
  // Independent lanes go back to the plain per-lane baseline (any trailing
  // inserts trimmed for the branch grow back).
  alignBranchLaneLengths();
  bumpChainRevision();
  DBG("Chain branch cleared; chains independent again");
  return true;
}

void TONE3000Processor::setActiveEditChain(const juce::String& side) {
  juce::ScopedLock lock(chainMutex);
  if (side == "right")
    pendingAddSide = ChainSide::Right;
  else if (side == "left")
    pendingAddSide = ChainSide::Left;
  bumpChainRevision();
}

bool TONE3000Processor::swapChains() {
  // Both lanes change output channel at once; mute-splice like reorder.
  ChainEditFade editFade(*this);
  juce::ScopedLock lock(chainMutex);

  if (!stereoEnabled.load())
    return false;

  pushChainHistory();
  // Insert slots travel with their lane (ids are lane-agnostic UUIDs, so
  // global uniqueness is preserved); each lane's slot invariant moves
  // wholesale with its blocks.
  std::swap(lane(ChainSide::Left), lane(ChainSide::Right));

  // The trunk lane moved sides; the branch (and the lane-end alignment its
  // geometry drives) moves with it.
  branchSourceSide =
      branchSourceSide == ChainSide::Left ? ChainSide::Right : ChainSide::Left;
  alignBranchLaneLengths();

  // Polarity flips describe the captures, so they travel with their lanes
  // (pans stay put: they're image placement, not chain state).
  auto* invLeft = parameters.getParameter("chainInvertLeft");
  auto* invRight = parameters.getParameter("chainInvertRight");
  if (invLeft != nullptr && invRight != nullptr && invLeft->getValue() != invRight->getValue()) {
    const float left = invLeft->getValue();
    invLeft->setValueNotifyingHost(invRight->getValue());
    invRight->setValueNotifyingHost(left);
  }

  bumpChainRevision();
  DBG("Swapped Left/Right chains");
  return true;
}

bool TONE3000Processor::setBlockParam(const std::string& blockId, const juce::String& param,
                                      double value) {
  juce::ScopedLock lock(chainMutex);
  ChainBlock* block = findBlockById(blockId);
  if (block == nullptr || block->type == ChainBlockType::INSERT)
    return false;

  // Validate before recording history, so failed calls never leave an entry.
  const bool isContinuous = param == "inputGain" || param == "outputGain" || param == "mix" ||
                            param == "predelay";
  const bool isKnown = isContinuous || param == "enabled" || param == "normalize";
  if (!isKnown) {
    DBG("setBlockParam: unknown param: " << param);
    return false;
  }

  // Continuous params coalesce a whole knob drag into one undo step.
  pushChainHistory(isContinuous ? "param:" + juce::String(blockId) + ":" + param
                                : juce::String());

  if (param == "enabled") {
    block->enabled = value > 0.5;
  } else if (param == "normalize") {
    block->normalizeEnabled = value > 0.5;
  } else if (param == "inputGain") {
    block->inputGainNormalized = juce::jlimit(0.0f, 1.0f, static_cast<float>(value));
  } else if (param == "outputGain") {
    block->outputGainNormalized = juce::jlimit(0.0f, 1.0f, static_cast<float>(value));
  } else if (param == "mix") {
    block->mixNormalized = juce::jlimit(0.0f, 1.0f, static_cast<float>(value));
  } else if (param == "predelay") {
    block->predelayNormalized = juce::jlimit(0.0f, 1.0f, static_cast<float>(value));
    block->predelay.setDelayMs(block->predelayNormalized * BlockPredelay::kMaxDelayMs);
    refreshIrTailLength();
  }

  // Continuous drags settle into one bump after the gesture ends; discrete
  // toggles resync immediately.
  if (isContinuous)
    deferredRevisionBump();
  else
    bumpChainRevision();
  return true;
}

bool TONE3000Processor::setBlockIrCategory(const std::string& blockId,
                                           const juce::String& category) {
  const IrCategory newCategory = irCategoryFromString(category);

  juce::ScopedLock lock(chainMutex);
  ChainBlock* block = findBlockById(blockId);
  if (block == nullptr || block->type != ChainBlockType::IR) {
    DBG("setBlockIrCategory: not an IR block: " << blockId);
    return false;
  }
  if (block->irCategory == newCategory)
    return true;

  pushChainHistory();
  block->irCategory = newCategory;
  // V1: no memory of a prior per-category mix - switching category always
  // resets to its fixed default (Cab 100%, IrPlayer 25%), same as a fresh
  // load (see applyPreparedModelToChainBlock). Both this and the -18 dB cab
  // pad (Processor.cpp) are pulled from smoothed values every block, so a
  // live block glides through the change rather than clicking.
  block->mixNormalized = newCategory == IrCategory::Cab ? 1.0f : 0.25f;

  bumpChainRevision();
  return true;
}

bool TONE3000Processor::setBlockSlimSize(const std::string& blockId, double slimSize) {
  slimSize = juce::jlimit(0.0, 1.0, slimSize);

  // Cheap early-out first: no fade (and no history entry) when nothing
  // changes.
  {
    juce::ScopedLock lock(chainMutex);
    const ChainBlock* block = findBlockById(blockId);
    if (block == nullptr || block->type != ChainBlockType::NAM) {
      DBG("setBlockSlimSize: not a NAM block: " << blockId);
      return false;
    }
    if (block->namSlimSize == slimSize)
      return true;
  }

  // Retiering swaps the running engine's weights discontinuously and can
  // prewarm the new submodel while holding chainMutex, so mute-splice like
  // a structural edit; the fade keeps the audio thread off the lock (see
  // processBlock's try-lock) while the swap runs.
  ChainEditFade editFade(*this);
  juce::ScopedLock lock(chainMutex);

  ChainBlock* block = findBlockById(blockId);
  if (block == nullptr || block->type != ChainBlockType::NAM)
    return false;
  if (block->namSlimSize == slimSize)
    return true;

  pushChainHistory();
  block->namSlimSize = slimSize;
  // A still-loading block only records the size here: the in-flight prepare
  // read the old value, and applyPreparedModelToChainBlock re-asserts the
  // block's size when that engine lands.
  if (block->namEngine != nullptr)
    block->namEngine->setSlimmableSize(slimSize);

  bumpChainRevision();
  return true;
}

namespace {
// Shared by setBlockIrDecay: bumps the block's shaping generation and queues
// the rebuild job. Caller must already hold chainMutex.
void queueIrShapeRebuild(TONE3000Processor& processor, ChainBlock& block,
                         juce::ThreadPool& loadingThreadPool) {
  const int generation = ++block.irShapingGeneration;
  const std::string blockId = block.id;

  struct RebuildIrShapeJob : public juce::ThreadPoolJob {
    TONE3000Processor& processor;
    std::string blockId;
    int targetGeneration;

    RebuildIrShapeJob(TONE3000Processor& p, const std::string& bid, int generation)
        : ThreadPoolJob("Rebuild IR Shape"), processor(p), blockId(bid),
          targetGeneration(generation) {}

    JobStatus runJob() override {
      processor.rebuildIrShapeInBackground(blockId, targetGeneration);
      return jobHasFinished;
    }
  };
  loadingThreadPool.addJob(new RebuildIrShapeJob(processor, blockId, generation), true);
}

// Shared by loadToneInBackground/switchModelInBackground/
// convertBlockTypeInBackground: a freshly loaded/swapped IR's engine is
// built straight from the raw file by prepareBlockModelOffThread, with none
// of the block's own Size/Width/envelope/Trim Init baked in (those only
// ever get applied by prepareIrShapeRebuild, queued above). For a NEW block
// that's a correct no-op (everything's still at its default), but for a
// SWAP onto a block that already carries a non-default shape from whatever
// was loaded before, skipping the reapply left the knobs showing the old
// values while the audio silently reverted to untouched/100% - this is
// what the caller checks before queuing that rebuild. Only meaningful for
// ChainBlockType::IR; CAB blocks carry no shaping fields at all (see
// ChainBlockType's own comment), and NAM resets them to default on every
// swap (applyPreparedModelToChainBlock's NAM branch) so never needs this.
bool blockHasNonDefaultIrShape(const ChainBlock& block) {
  return block.sizeNormalized != 0.5f || block.widthNormalized != 0.75f ||
        block.trimInitEnabled || block.reverseEnabled || block.initLevelNormalized != 1.0f ||
        block.attackLengthNormalized != 0.0f || block.attackCurveNormalized != 0.5f ||
        block.decayLengthNormalized != 1.0f || block.decayLevelNormalized != 1.0f ||
        block.decayCurveNormalized != 0.5f;
}
}  // namespace

bool TONE3000Processor::setBlockIrDecay(const std::string& blockId, double initLevelNormalized,
                                        double attackLengthNormalized,
                                        double attackCurveNormalized,
                                        double decayLengthNormalized,
                                        double decayLevelNormalized,
                                        double decayCurveNormalized) {
  const float clampedInitLevel = juce::jlimit(0.0f, 1.0f, static_cast<float>(initLevelNormalized));
  const float clampedAttackLength =
      juce::jlimit(0.0f, 1.0f, static_cast<float>(attackLengthNormalized));
  const float clampedAttackCurve =
      juce::jlimit(0.0f, 1.0f, static_cast<float>(attackCurveNormalized));
  const float clampedDecayLength =
      juce::jlimit(0.0f, 1.0f, static_cast<float>(decayLengthNormalized));
  const float clampedDecayLevel =
      juce::jlimit(0.0f, 1.0f, static_cast<float>(decayLevelNormalized));
  const float clampedDecayCurve =
      juce::jlimit(0.0f, 1.0f, static_cast<float>(decayCurveNormalized));

  juce::ScopedLock lock(chainMutex);
  ChainBlock* block = findBlockById(blockId);
  if (block == nullptr || block->type != ChainBlockType::IR || !block->loaded) {
    juce::Logger::writeToLog("setBlockIrDecay: not a loaded IR block: " + juce::String(blockId));
    return false;
  }
  if (block->initLevelNormalized == clampedInitLevel &&
      block->attackLengthNormalized == clampedAttackLength &&
      block->attackCurveNormalized == clampedAttackCurve &&
      block->decayLengthNormalized == clampedDecayLength &&
      block->decayLevelNormalized == clampedDecayLevel &&
      block->decayCurveNormalized == clampedDecayCurve)
    return true;

  // Coalesced like a knob drag (see setBlockParam's continuous params): the
  // persisted values update immediately (undo/redo, presets, duplication all
  // see them right away), the audible rebuild trails behind on the loader
  // pool. All six arrive together (like setBlockEqBand's whole-band
  // updates) so a drag on one can't clobber another's in-flight value.
  pushChainHistory("param:" + juce::String(blockId) + ":decay");
  block->initLevelNormalized = clampedInitLevel;
  block->attackLengthNormalized = clampedAttackLength;
  block->attackCurveNormalized = clampedAttackCurve;
  block->decayLengthNormalized = clampedDecayLength;
  block->decayLevelNormalized = clampedDecayLevel;
  block->decayCurveNormalized = clampedDecayCurve;
  deferredRevisionBump();
  queueIrShapeRebuild(*this, *block, loadingThreadPool);
  return true;
}

bool TONE3000Processor::setBlockIrSize(const std::string& blockId, double sizeNormalized) {
  const float clampedSize = juce::jlimit(0.0f, 1.0f, static_cast<float>(sizeNormalized));

  juce::ScopedLock lock(chainMutex);
  ChainBlock* block = findBlockById(blockId);
  if (block == nullptr || block->type != ChainBlockType::IR || !block->loaded) {
    juce::Logger::writeToLog("setBlockIrSize: not a loaded IR block: " + juce::String(blockId));
    return false;
  }
  if (block->sizeNormalized == clampedSize)
    return true;

  // Same coalesced-rebuild shape as setBlockIrDecay: the persisted value
  // updates immediately (undo/redo, presets, duplication see it right
  // away), the audible rebuild trails behind on the loader pool.
  pushChainHistory("param:" + juce::String(blockId) + ":size");
  block->sizeNormalized = clampedSize;
  deferredRevisionBump();
  queueIrShapeRebuild(*this, *block, loadingThreadPool);
  return true;
}

bool TONE3000Processor::setBlockIrWidth(const std::string& blockId, double widthNormalized) {
  const float clampedWidth = juce::jlimit(0.0f, 1.0f, static_cast<float>(widthNormalized));

  juce::ScopedLock lock(chainMutex);
  ChainBlock* block = findBlockById(blockId);
  if (block == nullptr || block->type != ChainBlockType::IR || !block->loaded) {
    juce::Logger::writeToLog("setBlockIrWidth: not a loaded IR block: " + juce::String(blockId));
    return false;
  }
  // No stereo image to widen - matches the UI's disabled knob. A no-op
  // return rather than silently clamping to mono, so a stale/racing UI call
  // never queues a pointless rebuild.
  if (block->irNumChannels <= 1) {
    juce::Logger::writeToLog("setBlockIrWidth: mono source, ignored: " + juce::String(blockId));
    return false;
  }
  if (block->widthNormalized == clampedWidth)
    return true;

  pushChainHistory("param:" + juce::String(blockId) + ":width");
  block->widthNormalized = clampedWidth;
  deferredRevisionBump();
  queueIrShapeRebuild(*this, *block, loadingThreadPool);
  return true;
}

bool TONE3000Processor::setBlockIrTrimInit(const std::string& blockId, bool enabled,
                                           bool relaxed) {
  juce::ScopedLock lock(chainMutex);
  ChainBlock* block = findBlockById(blockId);
  if (block == nullptr || block->type != ChainBlockType::IR || !block->loaded) {
    juce::Logger::writeToLog("setBlockIrTrimInit: not a loaded IR block: " + juce::String(blockId));
    return false;
  }
  if (block->trimInitEnabled == enabled && block->trimRelaxed == relaxed)
    return true;

  pushChainHistory("param:" + juce::String(blockId) + ":trimInit");
  block->trimInitEnabled = enabled;
  block->trimRelaxed = relaxed;
  deferredRevisionBump();
  queueIrShapeRebuild(*this, *block, loadingThreadPool);
  return true;
}

bool TONE3000Processor::setBlockIrReverse(const std::string& blockId, bool enabled) {
  juce::ScopedLock lock(chainMutex);
  ChainBlock* block = findBlockById(blockId);
  if (block == nullptr || block->type != ChainBlockType::IR || !block->loaded) {
    juce::Logger::writeToLog("setBlockIrReverse: not a loaded IR block: " + juce::String(blockId));
    return false;
  }
  if (block->reverseEnabled == enabled)
    return true;

  pushChainHistory("param:" + juce::String(blockId) + ":reverse");
  block->reverseEnabled = enabled;
  deferredRevisionBump();
  queueIrShapeRebuild(*this, *block, loadingThreadPool);
  return true;
}

bool TONE3000Processor::resetBlockIrShape(const std::string& blockId) {
  juce::ScopedLock lock(chainMutex);
  ChainBlock* block = findBlockById(blockId);
  if (block == nullptr || block->type != ChainBlockType::IR || !block->loaded) {
    juce::Logger::writeToLog("resetBlockIrShape: not a loaded IR block: " + juce::String(blockId));
    return false;
  }
  if (!blockHasNonDefaultIrShape(*block))
    return true;

  // One history entry for every field below, not one per field - the whole
  // point of a single Reset action undoing as a single step.
  pushChainHistory("param:" + juce::String(blockId) + ":resetShape");
  block->initLevelNormalized = 1.0f;
  block->attackLengthNormalized = 0.0f;
  block->attackCurveNormalized = 0.5f;
  block->decayLengthNormalized = 1.0f;
  block->decayLevelNormalized = 1.0f;
  block->decayCurveNormalized = 0.5f;
  block->sizeNormalized = 0.5f;
  block->widthNormalized = 0.75f;
  block->trimInitEnabled = false;
  block->trimRelaxed = false;
  block->reverseEnabled = false;
  deferredRevisionBump();
  queueIrShapeRebuild(*this, *block, loadingThreadPool);
  return true;
}

void TONE3000Processor::rebuildIrShapeInBackground(const std::string& blockId,
                                                    int targetGeneration) {
  juce::AudioBuffer<float> rawCopy;
  double rawSampleRate = 0.0;
  int origContentLengthSamples = 0;
  int trimStartSamples = 0;
  bool reverseEnabled = false;
  int irNumChannels = 1;
  bool engineLongIr = false;
  float initLevelNormalized = 1.0f;
  float attackLengthNormalized = 0.0f;
  float attackCurveNormalized = 0.5f;
  float decayLengthNormalized = 1.0f;
  float decayLevelNormalized = 1.0f;
  float decayCurveNormalized = 0.5f;
  float sizeNormalized = 0.5f;
  float widthNormalized = 0.75f;

  {
    juce::ScopedLock lock(chainMutex);
    ChainBlock* block = findBlockById(blockId);
    if (block == nullptr || block->type != ChainBlockType::IR ||
        block->irRawSamples.getNumSamples() <= 0) {
      juce::Logger::writeToLog("[Background] IR shape rebuild dropped, block not found/not IR: " +
                               juce::String(blockId));
      return;
    }
    if (block->irShapingGeneration.load() != targetGeneration) {
      // A newer drag/undo/redo already retargeted this block; that request
      // owns it now (either already running or about to be queued).
      juce::Logger::writeToLog("[Background] IR shape rebuild for " + juce::String(blockId) +
                               " superseded before it started");
      return;
    }
    rawCopy = block->irRawSamples;  // copy: irRawSamples must stay the untouched original
    rawSampleRate = block->irRawSampleRate;
    origContentLengthSamples = block->irContentLengthSamples;
    trimStartSamples = block->trimInitEnabled ? irEffectiveOnsetSamples(*block) : 0;
    reverseEnabled = block->reverseEnabled;
    irNumChannels = block->irNumChannels;
    engineLongIr = block->irIsLong;  // frozen classification, never re-decided here
    initLevelNormalized = block->initLevelNormalized;
    attackLengthNormalized = block->attackLengthNormalized;
    attackCurveNormalized = block->attackCurveNormalized;
    decayLengthNormalized = block->decayLengthNormalized;
    decayLevelNormalized = block->decayLevelNormalized;
    decayCurveNormalized = block->decayCurveNormalized;
    sizeNormalized = block->sizeNormalized;
    widthNormalized = block->widthNormalized;
  }

  PreparedIrShapeRebuild prepared = prepareIrShapeRebuild(
      rawCopy, rawSampleRate, origContentLengthSamples, trimStartSamples, reverseEnabled,
      irNumChannels, engineLongIr, initLevelNormalized, attackLengthNormalized,
      attackCurveNormalized, decayLengthNormalized, decayLevelNormalized, decayCurveNormalized,
      sizeNormalized, widthNormalized);

  // The outgoing engine keeps processing until this moment; fade it out on
  // the audio thread first. Same shape as an engine swap (see ChainBlock.h):
  // the wet path mutes in place, the dry share of the user's mix never gets
  // exposed.
  requestSwapFadeAndWait(blockId, /*muteWetOnly=*/true);

  {
    juce::ScopedLock lock(chainMutex);
    ChainBlock* block = findBlockById(blockId);
    if (block == nullptr) {
      juce::Logger::writeToLog("[Background] Block removed during IR shape rebuild: " +
                               juce::String(blockId));
      return;
    }

    // Whatever the outcome below, this rebuild attempt is over: clear the
    // fade flags unconditionally, before branching - mirrors
    // applyPreparedModelToChainBlock exactly. This clear must happen on
    // every path, success included, or swapFadePending/swapMuteWet stay
    // stuck true and the wet term is pinned silent forever (see
    // ChainBlock.h's swapWetMuteGain).
    block->swapFadePending.store(false);
    block->swapFadeDone.store(false);
    block->swapMuteWet.store(false);

    if (block->irShapingGeneration.load() != targetGeneration) {
      // Superseded while this rebuild was running; the newer job's own
      // requestSwapFadeAndWait re-arms the fade flags for its own attempt.
      juce::Logger::writeToLog("[Background] IR shape rebuild for " + juce::String(blockId) +
                               " superseded before it finished");
      return;
    }
    if (!prepared.success) {
      juce::Logger::writeToLog("[Background] IR shape rebuild failed for " +
                               juce::String(blockId));
      return;
    }

    std::swap(block->convolverMono, prepared.convolverMono);
    std::swap(block->convolverStereo, prepared.convolverStereo);
    block->irLengthBaseSamples = prepared.irLengthBaseSamples;
    // Deliberately untouched: irIsLong, irRawSamples, irRawSampleRate,
    // irContentLengthSamples, irWaveformPeaks, irNumChannels - the waveform
    // display's fixed window and the block's output-pad/default-mix
    // classification never move because of a Length or Decay edit.

    // Size's loudness compensation (see ChainBlock.h's
    // irSizeGainCompensation) rides on top of the block's content-only
    // irNormalizationGainLinear, landing in irEffectiveNormalizationGainLinear
    // - the audio thread's per-block Processor.cpp reads (and re-targets
    // the smoother from) that field, not the base one, so this is the only
    // place that value actually changes. Recomputed from the stable base
    // every time rather than compounded in place, so repeated Size edits
    // can't drift. setTargetValue, not setCurrentAndTargetValue: this can
    // fire mid-playback (a live Size drag), and a smoothed ramp into the
    // new level avoids a click the way every other IR shaping edit already
    // does.
    block->irEffectiveNormalizationGainLinear =
        block->irNormalizationGainLinear * prepared.irSizeGainCompensation;
    block->irNormalizationSmoother.setTargetValue(block->irEffectiveNormalizationGainLinear);

    refreshIrTailLength();  // irLengthBaseSamples changed; same call predelay makes
    bumpChainRevision();
    juce::Logger::writeToLog("[Background] IR shape rebuild applied for " +
                             juce::String(blockId) + ": " +
                             juce::String(prepared.irLengthBaseSamples / kChainBaseSampleRate, 2) +
                             " s");
  }
  // `prepared` now holds the block's *previous* engines; destroyed here,
  // after the lock (convolution teardown is too heavy to hold it).
}

bool TONE3000Processor::toggleBlockPower(int position, bool rightLane) {
  // The Right lane only processes in stereo mode; a mapped right-block stomp
  // outside it must not edit chain state the user can't see.
  if (rightLane && !isStereoMode())
    return false;

  // Resolve the position to a block id under the lock, then route through
  // setBlockParam so a MIDI stomp is exactly a UI power click: undoable,
  // revision-bumped, same validation.
  std::string blockId;
  bool enabled = false;
  {
    juce::ScopedLock lock(chainMutex);
    int seen = 0;
    for (const auto& block : lane(rightLane ? ChainSide::Right : ChainSide::Left)) {
      if (block->type == ChainBlockType::INSERT)
        continue;
      if (seen++ == position) {
        blockId = block->id;
        enabled = block->enabled;
        break;
      }
    }
  }
  if (blockId.empty())
    return false;  // chain shorter than the mapped slot
  return setBlockParam(blockId, "enabled", enabled ? 0.0 : 1.0);
}

// ####################
// PER-BLOCK EQ
// ####################
bool TONE3000Processor::setBlockEqBand(const std::string& blockId, int bandIndex,
                                       const juce::var& bandVar) {
  juce::ScopedLock lock(chainMutex);
  ChainBlock* block = findBlockById(blockId);
  if (block == nullptr || block->type == ChainBlockType::INSERT)
    return false;
  if (!bandVar.isObject() || bandIndex < 0 || bandIndex >= BlockEq::kNumBands)
    return false;

  // A whole dot/slider drag coalesces into one undo step.
  pushChainHistory("eq:" + juce::String(blockId) + ":" + juce::String(bandIndex));

  if (!block->eq.setBandFromVar(bandIndex, bandVar))
    return false;

  // Band edits arrive at drag rate; converge pollers after the gesture ends.
  deferredRevisionBump();
  return true;
}

bool TONE3000Processor::setBlockEqEnabled(const std::string& blockId, bool enabled) {
  juce::ScopedLock lock(chainMutex);
  ChainBlock* block = findBlockById(blockId);
  if (block == nullptr || block->type == ChainBlockType::INSERT)
    return false;
  if (block->eq.isEnabled() == enabled)
    return true;

  pushChainHistory();
  block->eq.setEnabled(enabled);
  bumpChainRevision();
  return true;
}

bool TONE3000Processor::setBlockEqPre(const std::string& blockId, bool pre) {
  juce::ScopedLock lock(chainMutex);
  ChainBlock* block = findBlockById(blockId);
  if (block == nullptr || block->type == ChainBlockType::INSERT)
    return false;
  if (block->eq.isPre() == pre)
    return true;

  pushChainHistory();
  block->eq.setPre(pre);
  bumpChainRevision();
  return true;
}

bool TONE3000Processor::resetBlockEq(const std::string& blockId) {
  juce::ScopedLock lock(chainMutex);
  ChainBlock* block = findBlockById(blockId);
  if (block == nullptr || block->type == ChainBlockType::INSERT)
    return false;

  pushChainHistory();
  block->eq.resetToDefault();
  bumpChainRevision();
  return true;
}

bool TONE3000Processor::setBlockSpectrumEnabled(const std::string& blockId, bool enabled) {
  juce::ScopedLock lock(chainMutex);
  ChainBlock* block = findBlockById(blockId);
  if (block == nullptr || block->type == ChainBlockType::INSERT)
    return false;

  block->spectrum.setEnabled(enabled);
  return true;
}

juce::var TONE3000Processor::getBlockSpectrum(const std::string& blockId) {
  // getSpectrum does the FFT on this (message) thread over a lock-free ring,
  // so the chain lock is only held for the block lookup + analysis.
  juce::ScopedLock lock(chainMutex);
  ChainBlock* block = findBlockById(blockId);
  if (block == nullptr || block->type == ChainBlockType::INSERT)
    return {};

  return block->spectrum.getSpectrum();
}

bool TONE3000Processor::setDualGoniometerEnabled(const std::string& blockId, bool enabled) {
  juce::ScopedLock lock(chainMutex);
  ChainBlock* block = findBlockById(blockId);
  if (block == nullptr || block->type != ChainBlockType::DUAL_MONO) {
    DBG("setDualGoniometerEnabled: not a DUAL_MONO block: " << blockId);
    return false;
  }

  block->dualGoniometer.setEnabled(enabled);
  return true;
}

juce::var TONE3000Processor::getDualGoniometer(const std::string& blockId) {
  // getPoints drains the ring on this (message) thread, so the chain lock
  // is only held for the block lookup, same as getBlockSpectrum above.
  juce::ScopedLock lock(chainMutex);
  ChainBlock* block = findBlockById(blockId);
  if (block == nullptr || block->type != ChainBlockType::DUAL_MONO)
    return {};

  return block->dualGoniometer.getPoints();
}

juce::var TONE3000Processor::getIrWaveform(const std::string& blockId) {
  juce::ScopedLock lock(chainMutex);
  ChainBlock* block = findBlockById(blockId);
  if (block == nullptr || block->type != ChainBlockType::IR || block->irWaveformPeaks.empty())
    return {};

  juce::Array<juce::var> mins, maxs;
  mins.ensureStorageAllocated(static_cast<int>(block->irWaveformPeaks.size()));
  maxs.ensureStorageAllocated(static_cast<int>(block->irWaveformPeaks.size()));
  for (const auto& [mn, mx] : block->irWaveformPeaks) {
    mins.add(mn);
    maxs.add(mx);
  }

  juce::DynamicObject::Ptr obj = new juce::DynamicObject();
  obj->setProperty("mins", mins);
  obj->setProperty("maxs", maxs);
  return juce::var(obj.get());
}

void TONE3000Processor::disableAllBlockSpectrums() {
  juce::ScopedLock lock(chainMutex);
  for (auto& chain : lanes)
    for (auto& block : chain)
      block->spectrum.setEnabled(false);
}

void TONE3000Processor::setAccessToken(const juce::String& token) {
  juce::ScopedLock lock(accessTokenMutex);
  accessToken = token;
  DBG("TONE3000 access token updated (" << token.length() << " chars)");
}

juce::String TONE3000Processor::getAccessToken() const {
  juce::ScopedLock lock(accessTokenMutex);
  return accessToken;
}
