#include "Processor.h"

#include <set>

// ####################
// SCENES
// ####################
//
// See the SCENES section in Processor.h for the model. Two rules carry the
// whole design:
//
//  1. The active scene IS the live chain. Its stored slot may be stale; it is
//     refreshed from the chain when switching away (selectScene) and read
//     straight off the chain whenever scenes are serialized (effectiveScene),
//     so every edit lands in the active scene with no per-edit bookkeeping.
//  2. Switching applies only per-scene values (bypass, model, and the block's
//     opt-in perSceneParams). Shared values are never written, which is what
//     makes them shared.
//
// Blocks a scene has never seen (added while another scene was active) keep
// their live state when that scene is applied and are captured into it when
// it is next left. Entries for blocks that no longer exist are pruned at
// serialization.

namespace {

// Normalized value of one per-scene-capable continuous param ("eq" is
// handled separately as a tree).
float readSceneParam(const ChainBlock& block, const juce::String& name) {
  if (name == "inputGain") return block.inputGainNormalized;
  if (name == "outputGain") return block.outputGainNormalized;
  if (name == "mix") return block.mixNormalized;
  if (name == "predelay") return block.predelayNormalized;
  return 0.0f;
}

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

}  // namespace

const std::vector<juce::String>& TONE3000Processor::sceneParamNames() {
  static const std::vector<juce::String> names = {"inputGain", "outputGain", "mix", "predelay",
                                                   "eq"};
  return names;
}

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

TONE3000Processor::SceneBlockState TONE3000Processor::captureSceneBlock(
    const ChainBlock& block) const {
  SceneBlockState state;
  state.enabled = block.enabled;
  if (blockHasModel(block)) {
    state.modelId = block.activeModelId;
    state.modelData = activeModelData(block);
  }
  for (const auto& name : block.perSceneParams) {
    if (name == "eq")
      state.eq = block.eq.toValueTree();
    else
      state.params[name] = readSceneParam(block, name);
  }
  return state;
}

TONE3000Processor::Scene TONE3000Processor::captureLiveScene() const {
  Scene live;
  live.name = scenes[static_cast<size_t>(activeScene)].name;
  live.levelDb = scenes[static_cast<size_t>(activeScene)].levelDb;
  forEachSceneBlockConst(chain, [&](const ChainBlock& b) { live.blocks[b.id] = captureSceneBlock(b); });
  return live;
}

TONE3000Processor::Scene TONE3000Processor::effectiveScene(int index) const {
  return index == activeScene ? captureLiveScene() : scenes[static_cast<size_t>(index)];
}

void TONE3000Processor::applySceneBlock(const SceneBlockState& state, ChainBlock& block) {
  // Bypass: the block's own wet fade glides it (see processChainOnBuffer).
  block.enabled = state.enabled;

  for (const auto& name : block.perSceneParams) {
    if (name == "eq") {
      if (state.eq.isValid())
        block.eq.restoreFromValueTree(state.eq);
      continue;
    }
    const auto it = state.params.find(name);
    if (it == state.params.end())
      continue;
    const float v = juce::jlimit(0.0f, 1.0f, it->second);
    // Continuous params ride the block's smoothers, so these glide too.
    if (name == "inputGain") block.inputGainNormalized = v;
    else if (name == "outputGain") block.outputGainNormalized = v;
    else if (name == "mix") block.mixNormalized = v;
    else if (name == "predelay") {
      block.predelayNormalized = v;
      block.predelay.setDelayMs(v * BlockPredelay::kMaxDelayMs);
    }
  }

  // Model: only when it actually differs and we know its catalog object.
  if (blockHasModel(block) && state.modelId != 0 && state.modelId != block.activeModelId &&
      state.modelData.isObject() && block.toneVar.isObject())
    switchModelLocked(block, state.modelId, state.modelData);
}

void TONE3000Processor::applyScene(const Scene& scene) {
  forEachSceneBlock(chain, [&](ChainBlock& b) {
    const auto it = scene.blocks.find(b.id);
    if (it != scene.blocks.end())
      applySceneBlock(it->second, b);
  });
  refreshIrTailLength();  // predelay may have moved
  sceneLevelDb.store(scene.levelDb);
}

bool TONE3000Processor::selectScene(int index) {
  if (index < 0 || index >= kNumScenes)
    return false;
  juce::ScopedLock lock(chainMutex);
  if (index == activeScene)
    return true;
  scenes[static_cast<size_t>(activeScene)] = captureLiveScene();
  activeScene = index;
  applyScene(scenes[static_cast<size_t>(index)]);
  bumpChainRevision();
  return true;
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
  bumpChainRevision();
  return true;
}

bool TONE3000Processor::setBlockParamPerScene(const std::string& blockId, const juce::String& param,
                                              bool perScene) {
  const auto& names = sceneParamNames();
  if (std::find(names.begin(), names.end(), param) == names.end())
    return false;
  juce::ScopedLock lock(chainMutex);
  ChainBlock* block = findBlockById(blockId);
  if (block == nullptr || block->type == ChainBlockType::INSERT)
    return false;
  if ((block->perSceneParams.count(param) > 0) == perScene)
    return true;

  pushChainHistory();
  if (perScene) {
    block->perSceneParams.insert(param);
    // Seed every stored scene with the current value, so the param starts
    // identical everywhere and only diverges when edited in a scene.
    for (int s = 0; s < kNumScenes; ++s) {
      if (s == activeScene)
        continue;
      auto& entry = scenes[static_cast<size_t>(s)].blocks[block->id];
      if (param == "eq")
        entry.eq = block->eq.toValueTree();
      else
        entry.params[param] = readSceneParam(*block, param);
    }
  } else {
    // Shared again: the current (active scene's) value simply stays for all.
    block->perSceneParams.erase(param);
    for (auto& scene : scenes) {
      auto it = scene.blocks.find(block->id);
      if (it == scene.blocks.end())
        continue;
      if (param == "eq")
        it->second.eq = juce::ValueTree();
      else
        it->second.params.erase(param);
    }
  }
  bumpChainRevision();
  return true;
}

// ---------------------------------------------------------------------------
// Persistence: rides the chain snapshot (session state, presets, undo).

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
      if (state.modelId != 0) {
        b.setProperty("modelId", state.modelId, nullptr);
        if (state.modelData.isObject())
          b.setProperty("modelData", juce::JSON::toString(state.modelData, true), nullptr);
      }
      for (const auto& [name, value] : state.params)
        b.setProperty("p_" + name, value, nullptr);
      if (state.eq.isValid())
        b.appendChild(state.eq.createCopy(), nullptr);
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
        SceneBlockState state;
        state.enabled = static_cast<bool>(b.getProperty("enabled", true));
        state.modelId = static_cast<int>(b.getProperty("modelId", 0));
        if (b.hasProperty("modelData"))
          state.modelData = juce::JSON::parse(b.getProperty("modelData").toString());
        for (int i = 0; i < b.getNumProperties(); ++i) {
          const juce::String prop = b.getPropertyName(i).toString();
          if (prop.startsWith("p_"))
            state.params[prop.substring(2)] = static_cast<float>(b.getProperty(prop));
        }
        if (b.getNumChildren() > 0)
          state.eq = b.getChild(0).createCopy();
        scene.blocks[b.getProperty("id").toString().toStdString()] = std::move(state);
      }
    }
  }
  // The restored chain already IS the active scene; only the level needs
  // pushing to the audio thread.
  sceneLevelDb.store(scenes[static_cast<size_t>(activeScene)].levelDb);
}
