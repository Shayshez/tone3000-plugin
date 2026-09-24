#include "Processor.h"

#include <algorithm>
#include <set>

// ####################
// SCENES & CHANNELS
// ####################
//
// See the SCENES & CHANNELS section in Processor.h. Two rules carry the
// design:
//
//  1. Live state is the truth for whatever is active: the active scene is
//     the live chain, and each block's active channel is the live block.
//     Slots are refreshed from the live state when switching away and read
//     straight off it whenever state is serialized, so edits land where they
//     belong with no per-edit bookkeeping.
//  2. A scene stores only {bypass, channel} per block; a channel stores the
//     block's full settings (minus bypass/identity/NAM size) plus its model.
//
// Blocks a scene has never seen (added while another scene was active) keep
// their live state when that scene is applied, and are captured into it when
// it is next left. Entries for blocks that no longer exist are pruned at
// serialization.

namespace {

bool blockHasModel(const ChainBlock& block) {
  return block.type == ChainBlockType::NAM || block.type == ChainBlockType::IR ||
         block.type == ChainBlockType::CAB;
}

// The block's stored catalog object for its active model (void if absent).
juce::var activeModelData(const ChainBlock& block) {
  if (const auto* models = block.toneVar["models"].getArray())
    for (const auto& m : *models)
      if (static_cast<int>(m["id"]) == block.activeModelId)
        return m;
  return {};
}

// Settings that shape an IR kernel: a change needs an off-thread rebuild.
const char* const kIrShapeProps[] = {"initLevel",  "attackLength", "attackCurve", "decayLength",
                                     "decayLevel", "decayCurve",   "size",        "width",
                                     "trimInit",   "trimRelaxed",  "reverse"};

// Kernel signature of an IR/Cab channel (or of a block's live settings, via
// serializeBlockSettings): the model, plus the shape for IR blocks (Cab has
// none). Two channels with equal keys convolve identically, so they share a
// warm engine. Values are rounded so a session round trip keeps the key.
juce::String irChannelKey(const juce::ValueTree& channel, ChainBlockType type) {
  juce::String key = "m" + channel.getProperty("activeModelId", 0).toString();
  if (type == ChainBlockType::IR)
    for (const char* prop : kIrShapeProps)
      key << "|" << juce::String(static_cast<double>(channel.getProperty(prop, 0.0)), 5);
  return key;
}

bool isIrType(const ChainBlock& block) {
  return block.type == ChainBlockType::IR || block.type == ChainBlockType::CAB;
}

}  // namespace

void TONE3000Processor::forEachSceneBlockConst(const Lane& lane,
                                               const std::function<void(const ChainBlock&)>& fn) {
  for (const auto& block : lane) {
    if (block == nullptr || block->type == ChainBlockType::INSERT)
      continue;
    fn(*block);
    if (block->type == ChainBlockType::DUAL_MONO) {
      forEachSceneBlockConst(block->dualLeft, fn);
      forEachSceneBlockConst(block->dualRight, fn);
    }
  }
}

void TONE3000Processor::forEachSceneBlock(Lane& lane, const std::function<void(ChainBlock&)>& fn) {
  for (auto& block : lane) {
    if (block == nullptr || block->type == ChainBlockType::INSERT)
      continue;
    fn(*block);
    if (block->type == ChainBlockType::DUAL_MONO) {
      forEachSceneBlock(block->dualLeft, fn);
      forEachSceneBlock(block->dualRight, fn);
    }
  }
}

// ---------------------------------------------------------------------------
// Channels

juce::ValueTree TONE3000Processor::captureChannel(const ChainBlock& block) const {
  juce::ValueTree channel = serializeBlockSettings(block).createCopy();
  for (const char* prop : {"id", "type", "enabled", "slimSize", "channel"})
    channel.removeProperty(prop, nullptr);
  channel.removeChild(channel.getChildWithName("Channels"), nullptr);
  if (blockHasModel(block)) {
    const juce::var model = activeModelData(block);
    if (model.isObject())
      channel.setProperty("channelModel", juce::JSON::toString(model, true), nullptr);
  }
  return channel;
}

juce::ValueTree TONE3000Processor::defaultChannel(const ChainBlock& block) const {
  juce::ValueTree channel = captureChannel(block);
  // Defaults straight off a fresh block's member initializers.
  const ChainBlock fresh(block.id, block.type);
  const juce::ValueTree defaults = serializeBlockSettings(fresh);
  // Kept from the live block: identity, content, bypass (scene) and NAM size.
  static const juce::Identifier kept[] = {"id",       "type",          "enabled",
                                          "slimSize", "irCategory",    "toneId",
                                          "toneJson", "activeModelId", "channel"};
  for (int i = 0; i < defaults.getNumProperties(); ++i) {
    const juce::Identifier name = defaults.getPropertyName(i);
    if (std::find(std::begin(kept), std::end(kept), name) == std::end(kept))
      channel.setProperty(name, defaults.getProperty(name), nullptr);
  }
  // EQ: the fresh block's (flat, off).
  for (const auto& child : defaults) {
    channel.removeChild(channel.getChildWithName(child.getType()), nullptr);
    channel.appendChild(child.createCopy(), nullptr);
  }
  // Same per-category Mix a freshly loaded IR gets (Cab 100%, IR Player 25%).
  if (block.type == ChainBlockType::IR || block.type == ChainBlockType::CAB)
    channel.setProperty("mix", block.irCategory == IrCategory::Cab ? 1.0f : 0.25f, nullptr);
  return channel;
}

void TONE3000Processor::applyChannel(ChainBlock& block, const juce::ValueTree& channel) {
  if (!channel.isValid())
    return;
  // IR kernel params before, to know whether a rebuild is needed.
  const juce::ValueTree before = serializeBlockSettings(block);

  // Bypass belongs to the scene, NAM size to the block; no "id" property, so
  // applyBlockSettings leaves the channel slots alone.
  juce::ValueTree merged = channel.createCopy();
  merged.setProperty("enabled", block.enabled, nullptr);
  merged.setProperty("slimSize", block.namSlimSize, nullptr);
  applyBlockSettings(block, merged);
  block.predelay.setDelayMs(block.predelayNormalized * BlockPredelay::kMaxDelayMs);

  // A channel may hold a different tone (capture) than the others.
  const juce::String toneJson = channel.getProperty("toneJson").toString();
  if (blockHasModel(block) && toneJson.isNotEmpty() && toneJson != block.toneJson) {
    const juce::var toneVar = juce::JSON::parse(toneJson);
    if (toneVar.isObject())
      setToneOnBlock(block, static_cast<int>(channel.getProperty("toneId", 0)), toneJson, toneVar);
  }

  const int modelId = static_cast<int>(channel.getProperty("activeModelId", 0));
  const bool modelChanges = blockHasModel(block) && modelId != 0 && modelId != block.activeModelId;

  // IR/Cab: a warm engine for this channel's kernel (model + shape) makes
  // the switch a crossfade instead of a load / rebuild.
  if (isIrType(block) && block.toneVar.isObject()) {
    const juce::String key = irChannelKey(channel, block.type);
    if (key != irChannelKey(before, block.type) &&
        swapToWarmIrEngine(block, key, irChannelKey(before, block.type))) {
      if (modelChanges)
        adoptSwappedModel(block, modelId,
                          juce::JSON::parse(channel.getProperty("channelModel").toString()));
      return;
    }
  }

  if (modelChanges) {
    const juce::var modelData = juce::JSON::parse(channel.getProperty("channelModel").toString());
    if (modelData.isObject() && block.toneVar.isObject() &&
        !(block.type == ChainBlockType::NAM && swapToWarmNamEngine(block, modelId, modelData)))
      switchModelLocked(block, modelId, modelData);  // IR shape re-applies after the load
  } else if (block.type == ChainBlockType::IR || block.type == ChainBlockType::CAB) {
    bool shapeMoved = false;
    for (const char* prop : kIrShapeProps)
      shapeMoved = shapeMoved || before.getProperty(prop) != channel.getProperty(prop, before.getProperty(prop));
    if (shapeMoved)
      requestIrShapeRebuild(block);
  }
}

namespace {
// Dual Mono side blocks (both lanes, inserts skipped).
template <typename Fn>
void forEachDualSide(ChainBlock& wrapper, Fn&& fn) {
  for (auto* lane : {&wrapper.dualLeft, &wrapper.dualRight})
    for (auto& side : *lane)
      if (side != nullptr && side->type != ChainBlockType::INSERT)
        fn(*side);
}
}  // namespace

ChainBlock* TONE3000Processor::channelGroupRoot(const std::string& blockId) {
  for (auto& block : chain) {
    if (block == nullptr || block->type != ChainBlockType::DUAL_MONO)
      continue;
    ChainBlock* parent = nullptr;
    forEachDualSide(*block, [&](ChainBlock& side) {
      if (side.id == blockId)
        parent = block.get();
    });
    if (parent != nullptr)
      return parent;
  }
  return findBlockById(blockId);
}

void TONE3000Processor::switchBlockChannel(ChainBlock& block, int channel) {
  channel = juce::jlimit(0, kNumBlockChannels - 1, channel);
  if (block.type == ChainBlockType::DUAL_MONO) {
    forEachDualSide(block, [&](ChainBlock& side) {
      // A side loaded after the group left channel A still carries its
      // live state as channel A: relabel it to the group's channel first.
      if (side.activeChannel != block.activeChannel) {
        side.channels[static_cast<size_t>(block.activeChannel)] = juce::ValueTree();
        side.activeChannel = block.activeChannel;
      }
      switchSingleBlockChannel(side, channel);
    });
  }
  switchSingleBlockChannel(block, channel);
}

void TONE3000Processor::switchSingleBlockChannel(ChainBlock& block, int channel) {
  if (channel == block.activeChannel)
    return;
  block.channels[static_cast<size_t>(block.activeChannel)] = captureChannel(block);
  // An unused channel starts from defaults (same tone/model); "Copy To"
  // is the explicit way to start from another channel.
  const juce::ValueTree target = block.channels[static_cast<size_t>(channel)].isValid()
                                     ? block.channels[static_cast<size_t>(channel)]
                                     : defaultChannel(block);
  block.activeChannel = channel;
  applyChannel(block, target);
}

bool TONE3000Processor::selectBlockChannel(const std::string& blockId, int channel) {
  if (channel < 0 || channel >= kNumBlockChannels)
    return false;
  juce::ScopedLock lock(chainMutex);
  ChainBlock* block = channelGroupRoot(blockId);
  if (block == nullptr || block->type == ChainBlockType::INSERT)
    return false;
  if (channel == block->activeChannel)
    return true;
  pushChainHistory();
  switchBlockChannel(*block, channel);
  refreshIrTailLength();
  refreshWarmEngines();
  bumpChainRevision();
  return true;
}

bool TONE3000Processor::copyBlockChannel(const std::string& blockId, int from, int to) {
  if (from < 0 || from >= kNumBlockChannels || to < 0 || to >= kNumBlockChannels || from == to)
    return false;
  juce::ScopedLock lock(chainMutex);
  ChainBlock* block = channelGroupRoot(blockId);
  if (block == nullptr || block->type == ChainBlockType::INSERT)
    return false;
  pushChainHistory();
  auto copyOne = [&](ChainBlock& b) {
    const juce::ValueTree& stored = b.channels[static_cast<size_t>(from)];
    const juce::ValueTree source =
        from == b.activeChannel || !stored.isValid() ? captureChannel(b) : stored.createCopy();
    if (to == b.activeChannel)
      applyChannel(b, source);
    else
      b.channels[static_cast<size_t>(to)] = source;
  };
  copyOne(*block);
  if (block->type == ChainBlockType::DUAL_MONO)
    forEachDualSide(*block, copyOne);
  refreshWarmEngines();
  bumpChainRevision();
  return true;
}

// ---------------------------------------------------------------------------
// Scenes

TONE3000Processor::Scene TONE3000Processor::captureLiveScene() const {
  Scene live;
  live.name = scenes[static_cast<size_t>(activeScene)].name;
  live.levelDb = scenes[static_cast<size_t>(activeScene)].levelDb;
  forEachSceneBlockConst(chain, [&](const ChainBlock& b) {
    live.blocks[b.id] = SceneBlockState{b.enabled, b.activeChannel};
  });
  return live;
}

TONE3000Processor::Scene TONE3000Processor::effectiveScene(int index) const {
  return index == activeScene ? captureLiveScene() : scenes[static_cast<size_t>(index)];
}

void TONE3000Processor::applyScene(const Scene& scene) {
  // Dual Mono sides follow their wrapper's channel (switchBlockChannel).
  std::set<std::string> dualSides;
  for (auto& block : chain)
    if (block != nullptr && block->type == ChainBlockType::DUAL_MONO)
      forEachDualSide(*block, [&](ChainBlock& side) { dualSides.insert(side.id); });

  forEachSceneBlock(chain, [&](ChainBlock& b) {
    const auto it = scene.blocks.find(b.id);
    if (it == scene.blocks.end())
      return;
    b.enabled = it->second.enabled;  // glides on the block's wet fade
    if (dualSides.count(b.id) == 0)
      switchBlockChannel(b, it->second.channel);
  });
  refreshIrTailLength();
  sceneLevelDb.store(scene.levelDb);
}

bool TONE3000Processor::selectScene(int index) {
  if (index < 0 || index >= kNumScenes)
    return false;
  {
    juce::ScopedLock lock(chainMutex);
    if (index != activeScene) {
      scenes[static_cast<size_t>(activeScene)] = captureLiveScene();
      activeScene = index;
      applyScene(scenes[static_cast<size_t>(index)]);
      refreshWarmEngines();
      bumpChainRevision();
    }
  }
  // Outside the lock: host notification can call back into us.
  if (juce::MessageManager::getInstanceWithoutCreating() != nullptr &&
      juce::MessageManager::getInstance()->isThisTheMessageThread())
    syncSceneParam();
  else {
    sceneParamDirty.store(true);
    triggerAsyncUpdate();
  }
  return true;
}

void TONE3000Processor::syncSceneParam() {
  auto* param = parameters.getParameter("scene");
  if (param == nullptr)
    return;
  const int active = getActiveScene();
  const float normalized = param->convertTo0to1(static_cast<float>(active));
  if (std::abs(param->getValue() - normalized) < 1.0e-4f)
    return;
  syncingSceneParam.store(true);
  param->setValueNotifyingHost(normalized);
  syncingSceneParam.store(false);
}

int TONE3000Processor::getActiveScene() const {
  juce::ScopedLock lock(chainMutex);
  return activeScene;
}

bool TONE3000Processor::renameScene(int index, const juce::String& name) {
  if (index < 0 || index >= kNumScenes)
    return false;
  juce::ScopedLock lock(chainMutex);
  pushChainHistory();
  scenes[static_cast<size_t>(index)].name = name.trim().substring(0, 24);
  bumpChainRevision();
  return true;
}

bool TONE3000Processor::setSceneLevel(int index, double levelDb) {
  if (index < 0 || index >= kNumScenes)
    return false;
  juce::ScopedLock lock(chainMutex);
  pushChainHistory("sceneLevel:" + juce::String(index));
  const float db = static_cast<float>(juce::jlimit(-24.0, 12.0, levelDb));
  scenes[static_cast<size_t>(index)].levelDb = db;
  if (index == activeScene)
    sceneLevelDb.store(db);
  deferredRevisionBump();
  return true;
}

bool TONE3000Processor::copyScene(int from, int to) {
  if (from < 0 || from >= kNumScenes || to < 0 || to >= kNumScenes || from == to)
    return false;
  juce::ScopedLock lock(chainMutex);
  pushChainHistory();
  Scene source = effectiveScene(from);
  source.name = scenes[static_cast<size_t>(to)].name;  // content, not the name
  scenes[static_cast<size_t>(to)] = source;
  if (to == activeScene)
    applyScene(source);
  refreshWarmEngines();
  bumpChainRevision();
  return true;
}

bool TONE3000Processor::setSceneBlockEnabled(int sceneIndex, const std::string& blockId,
                                             bool enabled) {
  if (sceneIndex < 0 || sceneIndex >= kNumScenes)
    return false;
  juce::ScopedLock lock(chainMutex);
  ChainBlock* block = findBlockById(blockId);
  if (block == nullptr || block->type == ChainBlockType::INSERT)
    return false;
  pushChainHistory();
  if (sceneIndex == activeScene) {
    block->enabled = enabled;
  } else {
    auto [it, inserted] = scenes[static_cast<size_t>(sceneIndex)].blocks.try_emplace(
        blockId, SceneBlockState{block->enabled, block->activeChannel});
    it->second.enabled = enabled;
  }
  bumpChainRevision();
  return true;
}

bool TONE3000Processor::setSceneBlockChannel(int sceneIndex, const std::string& blockId,
                                             int channel) {
  if (sceneIndex < 0 || sceneIndex >= kNumScenes || channel < 0 || channel >= kNumBlockChannels)
    return false;
  juce::ScopedLock lock(chainMutex);
  ChainBlock* block = channelGroupRoot(blockId);  // a Dual Mono side -> its group
  if (block == nullptr || block->type == ChainBlockType::INSERT)
    return false;
  pushChainHistory();
  if (sceneIndex == activeScene) {
    switchBlockChannel(*block, channel);
    refreshIrTailLength();
  } else {
    auto setOne = [&](ChainBlock& b) {
      // A channel picked for another scene must exist to be warmed up.
      if (!b.channels[static_cast<size_t>(channel)].isValid() && channel != b.activeChannel)
        b.channels[static_cast<size_t>(channel)] = defaultChannel(b);
      auto [it, inserted] = scenes[static_cast<size_t>(sceneIndex)].blocks.try_emplace(
          b.id, SceneBlockState{b.enabled, b.activeChannel});
      it->second.channel = channel;
    };
    setOne(*block);
    if (block->type == ChainBlockType::DUAL_MONO)
      forEachDualSide(*block, setOne);
  }
  refreshWarmEngines();
  bumpChainRevision();
  return true;
}

// ---------------------------------------------------------------------------
// Warm engines (gapless NAM model changes between channels)

bool TONE3000Processor::swapToWarmNamEngine(ChainBlock& block, int modelId,
                                            const juce::var& modelData) {
  std::unique_ptr<NamEngine> incoming;
  if (auto it = block.warmNamEngines.find(modelId); it != block.warmNamEngines.end()) {
    incoming = std::move(it->second);
    block.warmNamEngines.erase(it);
  } else if (block.xfadeOutgoingNam != nullptr && block.xfadeOutgoingModelId == modelId) {
    // Switching straight back mid-crossfade: the engine still fading out is
    // the one we want.
    incoming = std::move(block.xfadeOutgoingNam);
    block.xfadeActive = false;
  }
  if (incoming == nullptr || block.namEngine == nullptr || !block.loaded)
    return false;
  if (incoming->getOversampleFactor() != chainOversampleFactor.load())
    return false;  // stale after an oversampling change; refresh rebuilds it

  // A crossfade still running hands its outgoing engine back to the pool.
  if (block.xfadeOutgoingNam != nullptr)
    block.warmNamEngines[block.xfadeOutgoingModelId] = std::move(block.xfadeOutgoingNam);

  const int domain = chainDomainBlockSize();
  incoming->resetState();  // no leftovers from when it last played
  if (block.xfadeScratch.getNumSamples() < domain)
    block.xfadeScratch.setSize(2, juce::jmax(1, domain), false, false, true);
  block.xfadeOutgoingNam = std::move(block.namEngine);
  block.xfadeOutgoingModelId = block.activeModelId;
  block.namEngine = std::move(incoming);
  block.xfadeGain.reset(chainSampleRate(), kSceneXfadeSeconds);
  block.xfadeGain.setCurrentAndTargetValue(0.0f);
  block.xfadeGain.setTargetValue(1.0f);
  block.xfadeActive = true;

  adoptSwappedModel(block, modelId, modelData);
  return true;
}

void TONE3000Processor::adoptSwappedModel(ChainBlock& block, int modelId,
                                          const juce::var& modelData) {
  if (modelData.isObject() && block.toneVar.isObject() &&
      !static_cast<bool>(block.toneVar["local"])) {
    juce::Array<juce::var> models;
    models.add(modelData);
    block.toneVar.getDynamicObject()->setProperty("models", models);
    block.toneJson = juce::JSON::toString(block.toneVar);
    block.toneSummary = makeToneSummary(block.toneVar);
  }
  block.activeModelId = modelId;
  block.modelLoading = false;
  block.loadFailed = false;
}

bool TONE3000Processor::swapToWarmIrEngine(ChainBlock& block, const juce::String& key,
                                           const juce::String& outgoingKey) {
  auto it = block.warmIrEngines.find(key);
  if (it == block.warmIrEngines.end() || it->second.convolverMono == nullptr ||
      block.convolverMono == nullptr || !block.loaded)
    return false;
  WarmIrEngine incoming = std::move(it->second);
  block.warmIrEngines.erase(it);

  // A crossfade still running hands its outgoing engine back to the pool.
  if (block.xfadeOutgoingIr.convolverMono != nullptr)
    block.warmIrEngines[block.xfadeOutgoingIrKey] = std::move(block.xfadeOutgoingIr);

  // Live engine -> outgoing (keeps its tail: it plays on while fading out).
  WarmIrEngine& out = block.xfadeOutgoingIr;
  out = WarmIrEngine();
  out.convolverMono = std::move(block.convolverMono);
  out.convolverStereo = std::move(block.convolverStereo);
  out.irNumChannels = block.irNumChannels;
  out.irLengthBaseSamples = block.irLengthBaseSamples;
  out.irIsLong = block.irIsLong;
  out.irNormalizationGainLinear = block.irNormalizationGainLinear;
  out.irEffectiveNormalizationGainLinear = block.irEffectiveNormalizationGainLinear;
  std::swap(out.irRawSamples, block.irRawSamples);
  out.irRawSampleRate = block.irRawSampleRate;
  out.irContentLengthSamples = block.irContentLengthSamples;
  out.irOnsetSamples = block.irOnsetSamples;
  out.irOnsetSamplesRelaxed = block.irOnsetSamplesRelaxed;
  std::swap(out.irWaveformPeaks, block.irWaveformPeaks);
  block.xfadeOutgoingIrKey = outgoingKey;
  block.xfadeOutgoingIrGain = juce::jlimit(0.0f, 1.0f, out.irEffectiveNormalizationGainLinear);

  // Incoming -> live, from a clean state (no leftovers from its last run).
  incoming.convolverMono->reset();
  if (incoming.convolverStereo != nullptr)
    incoming.convolverStereo->reset();
  block.convolverMono = std::move(incoming.convolverMono);
  block.convolverStereo = std::move(incoming.convolverStereo);
  block.irNumChannels = incoming.irNumChannels;
  block.irLengthBaseSamples = incoming.irLengthBaseSamples;
  block.irIsLong = incoming.irIsLong;
  block.irNormalizationGainLinear = incoming.irNormalizationGainLinear;
  block.irEffectiveNormalizationGainLinear = incoming.irEffectiveNormalizationGainLinear;
  std::swap(block.irRawSamples, incoming.irRawSamples);
  block.irRawSampleRate = incoming.irRawSampleRate;
  block.irContentLengthSamples = incoming.irContentLengthSamples;
  block.irOnsetSamples = incoming.irOnsetSamples;
  block.irOnsetSamplesRelaxed = incoming.irOnsetSamplesRelaxed;
  std::swap(block.irWaveformPeaks, incoming.irWaveformPeaks);
  // Each engine carries its own level through the fade (see the IR branch
  // in processBlock), so the shared smoother lands on the new one at once.
  block.irNormalizationSmoother.setCurrentAndTargetValue(
      juce::jlimit(0.0f, 1.0f, block.irEffectiveNormalizationGainLinear));
  // Any shape rebuild still in flight belongs to the channel just left.
  ++block.irShapingGeneration;

  // The fade runs at the base rate, inside the block's convolution island.
  const int baseBlock = juce::jmax(1, chainBaseBlockSize());
  if (block.xfadeScratch.getNumSamples() < baseBlock || block.xfadeScratch.getNumChannels() < 2)
    block.xfadeScratch.setSize(2, baseBlock, false, false, true);
  block.xfadeGain.reset(kChainBaseSampleRate, kSceneXfadeSeconds);
  block.xfadeGain.setCurrentAndTargetValue(0.0f);
  block.xfadeGain.setTargetValue(1.0f);
  block.xfadeActive = true;
  return true;
}

bool TONE3000Processor::isSceneModelWarm(const std::string& blockId, int modelId) const {
  juce::ScopedLock lock(chainMutex);
  const ChainBlock* block = const_cast<TONE3000Processor*>(this)->findBlockById(blockId);
  if (block == nullptr)
    return false;
  return block->warmNamEngines.count(modelId) > 0 ||
         (block->xfadeOutgoingNam != nullptr && block->xfadeOutgoingModelId == modelId);
}

bool TONE3000Processor::sceneReferencesModel(const std::string& blockId, int modelId) const {
  const ChainBlock* block = const_cast<TONE3000Processor*>(this)->findBlockById(blockId);
  return block != nullptr && block->channelReferencesModel(modelId);
}

void TONE3000Processor::refreshWarmEngines() {
  forEachSceneBlock(chain, [&](ChainBlock& b) {
    if (isIrType(b)) {
      refreshWarmIrEngines(b);
      return;
    }
    if (b.type != ChainBlockType::NAM)
      return;
    // Models of this block's OTHER used channels (at most 3 per block).
    std::map<int, juce::var> needed;  // model id -> catalog object
    for (int c = 0; c < kNumBlockChannels; ++c) {
      const juce::ValueTree& slot = b.channels[static_cast<size_t>(c)];
      if (c == b.activeChannel || !slot.isValid())
        continue;
      const int modelId = static_cast<int>(slot.getProperty("activeModelId", 0));
      if (modelId != 0 && modelId != b.activeModelId)
        needed.emplace(modelId, juce::JSON::parse(slot.getProperty("channelModel").toString()));
    }

    // A finished crossfade's engine goes back to the pool if still needed.
    if (!b.xfadeActive && b.xfadeOutgoingNam != nullptr) {
      if (needed.count(b.xfadeOutgoingModelId) > 0 &&
          b.warmNamEngines.count(b.xfadeOutgoingModelId) == 0)
        b.warmNamEngines[b.xfadeOutgoingModelId] = std::move(b.xfadeOutgoingNam);
      else
        b.xfadeOutgoingNam.reset();
    }

    for (auto it = b.warmNamEngines.begin(); it != b.warmNamEngines.end();) {
      const bool stale = it->second == nullptr ||
                         it->second->getOversampleFactor() != chainOversampleFactor.load();
      if (needed.count(it->first) == 0 || stale)
        it = b.warmNamEngines.erase(it);
      else
        ++it;
    }

    for (const auto& [modelId, modelData] : needed) {
      const bool inFade = b.xfadeOutgoingNam != nullptr && b.xfadeOutgoingModelId == modelId;
      if (b.warmNamEngines.count(modelId) > 0 || b.warmPending.count(modelId) > 0 || inFade ||
          !modelData.isObject())
        continue;
      b.warmPending.insert(modelId);
      struct PrewarmJob : public juce::ThreadPoolJob {
        TONE3000Processor& processor;
        std::string blockId;
        int modelId;
        juce::var modelData;
        PrewarmJob(TONE3000Processor& p, std::string bid, int mid, juce::var data)
            : ThreadPoolJob("Prewarm Scene Model"), processor(p), blockId(std::move(bid)),
              modelId(mid), modelData(std::move(data)) {}
        JobStatus runJob() override {
          processor.prewarmModelInBackground(blockId, modelId, modelData);
          return jobHasFinished;
        }
      };
      loadingThreadPool.addJob(new PrewarmJob(*this, b.id, modelId, modelData), true);
    }
  });
}

void TONE3000Processor::prewarmModelInBackground(const std::string& blockId, int modelId,
                                                 juce::var modelData) {
  std::vector<uint8_t> bytes;
  double slimSize = 1.0;
  {
    juce::ScopedLock lock(chainMutex);
    ChainBlock* block = findBlockById(blockId);
    if (block == nullptr)
      return;
    slimSize = block->namSlimSize;
    if (auto it = block->modelCache.find(modelId); it != block->modelCache.end())
      bytes = it->second;
  }
  const bool fetched = bytes.empty();
  if (fetched)
    bytes = fetchModelFromUrl(modelData["model_url"].toString());

  PreparedBlockModel prepared;
  if (!bytes.empty())
    prepared = prepareBlockModelOffThread(ChainBlockType::NAM, bytes,
                                          modelData["name"].toString() + ".nam", slimSize,
                                          std::nullopt);

  juce::ScopedLock lock(chainMutex);
  ChainBlock* block = findBlockById(blockId);
  if (block == nullptr)
    return;
  block->warmPending.erase(modelId);
  if (fetched && !bytes.empty())
    block->modelCache[modelId] = bytes;  // so presets/sessions can embed it
  if (!prepared.success || prepared.namEngine == nullptr) {
    juce::Logger::writeToLog("[Scenes] Prewarm failed for model " + juce::String(modelId) +
                             " (block " + juce::String(blockId) + ")");
    return;
  }
  if (!sceneReferencesModel(blockId, modelId) || modelId == block->activeModelId ||
      prepared.namEngine->getOversampleFactor() != chainOversampleFactor.load())
    return;  // no longer needed (or stale); dropped with `prepared`
  if (prepared.preparedBlockSize < chainDomainBlockSize())
    prepared.namEngine->prepare(chainDomainBlockSize());
  block->warmNamEngines[modelId] = std::move(prepared.namEngine);
}

void TONE3000Processor::refreshWarmIrEngines(ChainBlock& b) {
  // Kernels of this block's OTHER used channels that differ from the live one.
  const juce::String liveKey = irChannelKey(serializeBlockSettings(b), b.type);
  std::map<juce::String, juce::ValueTree> needed;  // key -> channel tree
  for (int c = 0; c < kNumBlockChannels; ++c) {
    const juce::ValueTree& slot = b.channels[static_cast<size_t>(c)];
    if (c == b.activeChannel || !slot.isValid())
      continue;
    const juce::String key = irChannelKey(slot, b.type);
    if (key != liveKey && static_cast<int>(slot.getProperty("activeModelId", 0)) != 0)
      needed.emplace(key, slot);
  }

  // A finished crossfade's engine goes back to the pool if still needed.
  if (!b.xfadeActive && b.xfadeOutgoingIr.convolverMono != nullptr) {
    if (needed.count(b.xfadeOutgoingIrKey) > 0 &&
        b.warmIrEngines.count(b.xfadeOutgoingIrKey) == 0)
      b.warmIrEngines[b.xfadeOutgoingIrKey] = std::move(b.xfadeOutgoingIr);
    b.xfadeOutgoingIr = WarmIrEngine();
  }

  for (auto it = b.warmIrEngines.begin(); it != b.warmIrEngines.end();)
    it = needed.count(it->first) == 0 ? b.warmIrEngines.erase(it) : std::next(it);

  for (const auto& [key, slot] : needed) {
    const bool inFade = b.xfadeOutgoingIr.convolverMono != nullptr && b.xfadeOutgoingIrKey == key;
    if (b.warmIrEngines.count(key) > 0 || b.warmIrPending.count(key) > 0 || inFade)
      continue;
    b.warmIrPending.insert(key);
    struct PrewarmIrJob : public juce::ThreadPoolJob {
      TONE3000Processor& processor;
      std::string blockId;
      juce::String key;
      juce::ValueTree channel;
      PrewarmIrJob(TONE3000Processor& p, std::string bid, juce::String k, juce::ValueTree ch)
          : ThreadPoolJob("Prewarm Channel IR"), processor(p), blockId(std::move(bid)),
            key(std::move(k)), channel(std::move(ch)) {}
      JobStatus runJob() override {
        processor.prewarmIrInBackground(blockId, key, channel);
        return jobHasFinished;
      }
    };
    loadingThreadPool.addJob(new PrewarmIrJob(*this, b.id, key, slot.createCopy()), true);
  }
}

void TONE3000Processor::prewarmIrInBackground(const std::string& blockId,
                                              const juce::String& key,
                                              juce::ValueTree channel) {
  const int modelId = static_cast<int>(channel.getProperty("activeModelId", 0));
  const juce::var modelData = juce::JSON::parse(channel.getProperty("channelModel").toString());
  ChainBlockType type = ChainBlockType::IR;
  IrCategory category = IrCategory::IrPlayer;
  WarmIrEngine warm;
  bool sameModel = false;
  std::vector<uint8_t> bytes;
  {
    juce::ScopedLock lock(chainMutex);
    ChainBlock* block = findBlockById(blockId);
    if (block == nullptr || !isIrType(*block))
      return;
    type = block->type;
    category = type == ChainBlockType::CAB ? IrCategory::Cab : block->irCategory;
    if (modelId == block->activeModelId && block->irRawSamples.getNumSamples() > 0) {
      // Same file, other shape: start from the live model's decoded data.
      sameModel = true;
      warm.irRawSamples = block->irRawSamples;
      warm.irRawSampleRate = block->irRawSampleRate;
      warm.irNumChannels = block->irNumChannels;
      warm.irIsLong = block->irIsLong;
      warm.irNormalizationGainLinear = block->irNormalizationGainLinear;
      warm.irContentLengthSamples = block->irContentLengthSamples;
      warm.irOnsetSamples = block->irOnsetSamples;
      warm.irOnsetSamplesRelaxed = block->irOnsetSamplesRelaxed;
      warm.irWaveformPeaks = block->irWaveformPeaks;
    } else if (auto it = block->modelCache.find(modelId); it != block->modelCache.end()) {
      bytes = it->second;
    }
  }

  bool fetched = false;
  bool ok = true;
  if (!sameModel) {
    if (bytes.empty() && modelData.isObject()) {
      bytes = fetchModelFromUrl(modelData["model_url"].toString());
      fetched = !bytes.empty();
    }
    PreparedBlockModel prepared;
    if (!bytes.empty())
      prepared = prepareBlockModelOffThread(ChainBlockType::IR, bytes,
                                            modelData["name"].toString() + ".wav", 0.0, category);
    ok = prepared.success && prepared.convolverMono != nullptr;
    if (ok) {
      warm.convolverMono = std::move(prepared.convolverMono);
      warm.convolverStereo = std::move(prepared.convolverStereo);
      warm.irNumChannels = prepared.irNumChannels;
      warm.irLengthBaseSamples = prepared.irLengthBaseSamples;
      warm.irIsLong = prepared.irIsLong;
      warm.irNormalizationGainLinear = prepared.irNormalizationGainLinear;
      warm.irEffectiveNormalizationGainLinear = prepared.irNormalizationGainLinear;
      warm.irRawSamples = std::move(prepared.irRawSamples);
      warm.irRawSampleRate = prepared.irRawSampleRate;
      warm.irContentLengthSamples = prepared.irContentLengthSamples;
      warm.irOnsetSamples = prepared.irOnsetSamples;
      warm.irOnsetSamplesRelaxed = prepared.irOnsetSamplesRelaxed;
      warm.irWaveformPeaks = std::move(prepared.irWaveformPeaks);
    }
  }

  // IR blocks: build the channel's own shape from the raw samples (a Cab has
  // no shape; a freshly loaded kernel already is its channel).
  if (ok && type == ChainBlockType::IR && warm.irRawSamples.getNumSamples() > 0) {
    const bool trim = static_cast<bool>(channel.getProperty("trimInit", false));
    const bool relaxed = static_cast<bool>(channel.getProperty("trimRelaxed", false));
    auto f = [&](const char* prop, float fallback) {
      return static_cast<float>(channel.getProperty(prop, fallback));
    };
    PreparedIrShapeRebuild shaped = prepareIrShapeRebuild(
        warm.irRawSamples, warm.irRawSampleRate, warm.irContentLengthSamples,
        trim ? (relaxed ? warm.irOnsetSamplesRelaxed : warm.irOnsetSamples) : 0,
        static_cast<bool>(channel.getProperty("reverse", false)), warm.irNumChannels,
        warm.irIsLong, f("initLevel", 1.0f), f("attackLength", 0.0f), f("attackCurve", 0.5f),
        f("decayLength", 1.0f), f("decayLevel", 1.0f), f("decayCurve", 0.5f), f("size", 0.5f),
        f("width", 0.75f));
    ok = shaped.success && shaped.convolverMono != nullptr;
    if (ok) {
      warm.convolverMono = std::move(shaped.convolverMono);
      warm.convolverStereo = std::move(shaped.convolverStereo);
      warm.irLengthBaseSamples = shaped.irLengthBaseSamples;
      warm.irEffectiveNormalizationGainLinear =
          warm.irNormalizationGainLinear * shaped.irSizeGainCompensation;
    }
  }

  WarmIrEngine dropped;  // destroyed after the lock is released
  {
    juce::ScopedLock lock(chainMutex);
    ChainBlock* block = findBlockById(blockId);
    if (block == nullptr)
      return;
    block->warmIrPending.erase(key);
    if (fetched)
      block->modelCache[modelId] = bytes;  // so presets/sessions can embed it
    if (!ok) {
      juce::Logger::writeToLog("[Scenes] IR prewarm failed for channel of block " +
                               juce::String(blockId));
      return;
    }
    // Still wanted? (the channel may have changed or become the live one)
    bool wanted = false;
    const juce::String liveKey = irChannelKey(serializeBlockSettings(*block), block->type);
    for (int c = 0; c < kNumBlockChannels; ++c) {
      const juce::ValueTree& slot = block->channels[static_cast<size_t>(c)];
      wanted = wanted || (c != block->activeChannel && slot.isValid() &&
                          irChannelKey(slot, block->type) == key);
    }
    if (!wanted || key == liveKey) {
      dropped = std::move(warm);
      return;
    }
    // The base block size may have moved while this was building.
    const juce::dsp::ProcessSpec spec{kChainBaseSampleRate,
                                      static_cast<juce::uint32>(juce::jmax(1, chainBaseBlockSize())),
                                      2};
    warm.convolverMono->prepare(spec);  // keeps the loaded impulse
    if (warm.convolverStereo != nullptr)
      warm.convolverStereo->prepare(spec);
    std::swap(dropped, block->warmIrEngines[key]);
    block->warmIrEngines[key] = std::move(warm);
  }
}

bool TONE3000Processor::isChannelWarm(const std::string& blockId, int channel) {
  juce::ScopedLock lock(chainMutex);
  ChainBlock* block = findBlockById(blockId);
  if (block == nullptr || channel < 0 || channel >= kNumBlockChannels)
    return false;
  const juce::ValueTree& slot = block->channels[static_cast<size_t>(channel)];
  if (channel == block->activeChannel || !slot.isValid())
    return true;
  if (isIrType(*block)) {
    const juce::String key = irChannelKey(slot, block->type);
    return key == irChannelKey(serializeBlockSettings(*block), block->type) ||
           block->warmIrEngines.count(key) > 0 ||
           (block->xfadeOutgoingIr.convolverMono != nullptr && block->xfadeOutgoingIrKey == key);
  }
  const int modelId = static_cast<int>(slot.getProperty("activeModelId", 0));
  return modelId == block->activeModelId || block->warmNamEngines.count(modelId) > 0 ||
         (block->xfadeOutgoingNam != nullptr && block->xfadeOutgoingModelId == modelId);
}

// ---------------------------------------------------------------------------
// Persistence: rides the chain snapshot (session state, presets, undo).
// Channels ride each block's own settings (serializeBlockSettings).

void TONE3000Processor::serializeScenes(juce::ValueTree& snapshot) const {
  std::set<std::string> liveIds;
  forEachSceneBlockConst(chain, [&](const ChainBlock& b) { liveIds.insert(b.id); });

  juce::ValueTree scenesTree("Scenes");
  scenesTree.setProperty("active", activeScene, nullptr);
  for (int s = 0; s < kNumScenes; ++s) {
    const Scene scene = effectiveScene(s);
    juce::ValueTree sceneTree("Scene");
    sceneTree.setProperty("name", scene.name, nullptr);
    sceneTree.setProperty("level", scene.levelDb, nullptr);
    for (const auto& [id, state] : scene.blocks) {
      if (liveIds.count(id) == 0)
        continue;  // pruned: the block is gone
      juce::ValueTree b("SceneBlock");
      b.setProperty("id", juce::String(id), nullptr);
      b.setProperty("enabled", state.enabled, nullptr);
      b.setProperty("channel", state.channel, nullptr);
      sceneTree.appendChild(b, nullptr);
    }
    scenesTree.appendChild(sceneTree, nullptr);
  }
  snapshot.appendChild(scenesTree, nullptr);
}

void TONE3000Processor::restoreScenes(const juce::ValueTree& snapshot) {
  for (auto& scene : scenes)
    scene = Scene();
  activeScene = 0;

  const juce::ValueTree scenesTree = snapshot.getChildWithName("Scenes");
  if (scenesTree.isValid()) {
    activeScene = juce::jlimit(0, kNumScenes - 1, static_cast<int>(scenesTree.getProperty("active", 0)));
    int s = 0;
    for (const auto& sceneTree : scenesTree) {
      if (s >= kNumScenes || !sceneTree.hasType("Scene"))
        continue;
      Scene& scene = scenes[static_cast<size_t>(s++)];
      scene.name = sceneTree.getProperty("name").toString();
      scene.levelDb = static_cast<float>(sceneTree.getProperty("level", 0.0f));
      for (const auto& b : sceneTree) {
        if (!b.hasType("SceneBlock"))
          continue;
        scene.blocks[b.getProperty("id").toString().toStdString()] = SceneBlockState{
            static_cast<bool>(b.getProperty("enabled", true)),
            juce::jlimit(0, kNumBlockChannels - 1, static_cast<int>(b.getProperty("channel", 0)))};
      }
    }
  }
  // The restored chain already IS the active scene; only the level needs
  // pushing to the audio thread. Warm engines are refreshed by the caller
  // once the chain is rebuilt (restoreChainSnapshot).
  sceneLevelDb.store(scenes[static_cast<size_t>(activeScene)].levelDb);
}
