import { useCallback, useEffect, useMemo, useRef, useState } from 'react';
import { useAudioBackend } from './useAudioBackend';
import type {
  BlockParamName,
  ChainSide,
  ChainState,
  ChainStateResponse,
  EqBand,
  InputMode,
} from '../types/chain';
import { isUnchanged, SLIM_SIZE_LITE } from '../types/chain';

/**
 * Fallback poll cadence for chain state. The primary sync channel is the
 * native `chainChanged` push event (the editor watches the revision counter
 * and emits within ~50 ms of any mutation); I keep this slow poll purely as
 * a safety net in case an event is dropped (e.g. while the webview is
 * hidden). Unchanged revisions short-circuit natively, so it's near free.
 */
const FALLBACK_POLL_INTERVAL_MS = 3000;

const EMPTY_STATE: ChainState = {
  revision: -1,
  canUndo: false,
  canRedo: false,
  atDefault: true,
  stereoEnabled: false,
  activeSide: 'left',
  stereoInput: false,
  stereoOutput: true,
  standalone: false,
  inputMode: 'stereo',
  namSlimSizeDefault: SLIM_SIZE_LITE,
  multiCore: true,
  sampleRate: 48000,
  chain: [],
};

/**
 * Single owner of the plugin chain state on the JS side.
 *
 * Sync model:
 * - Native is the source of truth; we hold a revision-tagged snapshot.
 * - Native pushes a `chainChanged` event on every revision bump; we resync on
 *   it (plus a slow fallback poll, and immediately after our own mutations).
 * - Continuous params (knob drags) go through `setBlockParam` fire-and-forget;
 *   the card keeps its own optimistic knob value, native defers the revision
 *   bump until the gesture settles, and the resulting push converges everyone.
 */
export function useChainState() {
  const backend = useAudioBackend();

  const native = useMemo(
    () => ({
      getChainState: backend.getPluginFunction('getChainState'),
      loadTone: backend.getPluginFunction('loadTone'),
      addEqBlock: backend.getPluginFunction('addEqBlock'),
      addDualMonoBlock: backend.getPluginFunction('addDualMonoBlock'),
      loadToneIntoDualSlot: backend.getPluginFunction('loadToneIntoDualSlot'),
      removeDualSlotContent: backend.getPluginFunction('removeDualSlotContent'),
      copyDualSlotFromSibling: backend.getPluginFunction('copyDualSlotFromSibling'),
      convertBlockToDualMono: backend.getPluginFunction('convertBlockToDualMono'),
      collapseDualMonoToSingle: backend.getPluginFunction('collapseDualMonoToSingle'),
      setDualImage: backend.getPluginFunction('setDualImage'),
      setDualLinked: backend.getPluginFunction('setDualLinked'),
      setDualSolo: backend.getPluginFunction('setDualSolo'),
      setDualMuted: backend.getPluginFunction('setDualMuted'),
      setDualAlign: backend.getPluginFunction('setDualAlign'),
      setDualStereoProcessingEnabled: backend.getPluginFunction('setDualStereoProcessingEnabled'),
      setDualInvert: backend.getPluginFunction('setDualInvert'),
      loadLocalTone: backend.getPluginFunction('loadLocalTone'),
      swapTone: backend.getPluginFunction('swapTone'),
      refreshToneMetadata: backend.getPluginFunction('refreshToneMetadata'),
      switchModel: backend.getPluginFunction('switchModel'),
      retryModelLoad: backend.getPluginFunction('retryModelLoad'),
      removeChainBlock: backend.getPluginFunction('removeChainBlock'),
      reorderChainBlocks: backend.getPluginFunction('reorderChainBlocks'),
      moveBlockToChain: backend.getPluginFunction('moveBlockToChain'),
      duplicateChainBlock: backend.getPluginFunction('duplicateChainBlock'),
      copyChainBlock: backend.getPluginFunction('copyChainBlock'),
      pasteChainBlock: backend.getPluginFunction('pasteChainBlock'),
      setBlockParam: backend.getPluginFunction('setBlockParam'),
      setBlockEqBand: backend.getPluginFunction('setBlockEqBand'),
      setBlockEqEnabled: backend.getPluginFunction('setBlockEqEnabled'),
      setBlockEqPre: backend.getPluginFunction('setBlockEqPre'),
      resetBlockEq: backend.getPluginFunction('resetBlockEq'),
      setStereoMode: backend.getPluginFunction('setStereoMode'),
      setInputMode: backend.getPluginFunction('setInputMode'),
      setBlockSlimSize: backend.getPluginFunction('setBlockSlimSize'),
      setBlockIrDecay: backend.getPluginFunction('setBlockIrDecay'),
      setBlockIrSize: backend.getPluginFunction('setBlockIrSize'),
      setBlockIrWidth: backend.getPluginFunction('setBlockIrWidth'),
      setBlockIrTrimInit: backend.getPluginFunction('setBlockIrTrimInit'),
      setBlockIrReverse: backend.getPluginFunction('setBlockIrReverse'),
      resetBlockIrShape: backend.getPluginFunction('resetBlockIrShape'),
      setBlockIrCategory: backend.getPluginFunction('setBlockIrCategory'),
      convertBlockType: backend.getPluginFunction('convertBlockType'),
      setNamSlimSizeDefault: backend.getPluginFunction('setNamSlimSizeDefault'),
      setMultiCore: backend.getPluginFunction('setMultiCore'),
      setActiveEditChain: backend.getPluginFunction('setActiveEditChain'),
      swapChains: backend.getPluginFunction('swapChains'),
      setChainBranch: backend.getPluginFunction('setChainBranch'),
      clearChainBranch: backend.getPluginFunction('clearChainBranch'),
      undoChain: backend.getPluginFunction('undoChain'),
      redoChain: backend.getPluginFunction('redoChain'),
      resetToDefault: backend.getPluginFunction('resetToDefault'),
    }),
    [backend]
  );

  const [state, setState] = useState<ChainState>(EMPTY_STATE);
  const revisionRef = useRef(-1);

  const refresh = useCallback(
    async (force = false) => {
      try {
        const res = (await native.getChainState(
          force ? -1 : revisionRef.current
        )) as ChainStateResponse | null;
        if (!res || typeof res.revision !== 'number') return;
        if (isUnchanged(res)) return;
        revisionRef.current = res.revision;
        setState(res);
      } catch (error) {
        console.error('Error loading chain state:', error);
      }
    },
    [native]
  );

  useEffect(() => {
    refresh(true);
    const unsubscribe = backend.addEventListener('chainChanged', () => refresh());
    const interval = setInterval(() => refresh(), FALLBACK_POLL_INTERVAL_MS);
    return () => {
      unsubscribe();
      clearInterval(interval);
    };
  }, [backend, refresh]);

  /** Run a mutation, then resync from native regardless of outcome. The
      native bridge is untyped, so T asserts each call's known return shape. */
  const run = useCallback(
    async <T>(label: string, fn: () => Promise<unknown>): Promise<T | null> => {
      let result: T | null = null;
      try {
        result = (await fn()) as T;
      } catch (error) {
        console.error(`Chain mutation failed (${label}):`, error);
      }
      await refresh();
      return result;
    },
    [refresh]
  );

  const actions = useMemo(
    () => ({
      /** Add a tone at an insert slot (the one the user clicked, when given;
          stale/absent ids land at the active lane's first insert). Resolves
          to the new blockId ('' on failure). */
      loadTone: (toneJson: string, targetInsertId?: string) =>
        run<string>('loadTone', () => native.loadTone(toneJson, targetInsertId ?? '')),
      /** Load dropped local file(s) as one block (a single .nam/.wav, or a
          folder's files; bytes as base64). `targetInsertId` is an insert
          slot (adds) or an existing tone block (swaps in place).
          `forceGear` is the empty-slot split drop zone's explicit IR/Cab
          choice (see ChainActions.loadLocalFile) - forwarded to native's
          own forceGear param as-is. Bug fix: 'ir' is NOT inert like a plain
          '' - native keys "skip the content-duration category guess" off
          *any* non-empty gear (see loadTone, ProcessorChain.cpp), so
          collapsing 'ir' to '' here silently let a short dropped IR file
          guess its way back to Cab despite the user's explicit choice.
          Native validates each file (NAM must be A2). Resolves to a
          user-facing error message, or null on success. */
      loadLocalTone: async (
        title: string,
        files: { name: string; data: string }[],
        targetInsertId: string,
        forceGear?: 'ir' | 'cab'
      ) => {
        const res = await run<{ blockId?: string; error?: string } | null>('loadLocalTone', () =>
          native.loadLocalTone(title, files, targetInsertId, forceGear ?? '')
        );
        if (res?.blockId) return null;
        return res?.error ?? "Couldn't load the file";
      },
      /** Replace an existing block's tone in place (keeps position + params). */
      swapTone: (blockId: string, toneJson: string) =>
        run<boolean>('swapTone', () => native.swapTone(blockId, toneJson)),
      /** Best-effort metadata re-sync from a fresh /tones/{id} payload:
          native merges it into every block holding that tone (stored models
          preserved). Metadata only, not undoable; no-op when unchanged. */
      refreshToneMetadata: (toneJson: string) =>
        run<boolean>('refreshToneMetadata', () => native.refreshToneMetadata(toneJson)),
      /** `modelJson` is the full model object (id/name/model_url); native
          only stores the active model and resolves the switch from this. */
      switchModel: (blockId: string, modelId: number, modelJson: string) =>
        run<boolean>('switchModel', () => native.switchModel(blockId, modelId, modelJson)),
      /** Retry a failed model download (block.loadFailed). */
      retryModelLoad: (blockId: string) =>
        run<boolean>('retryModelLoad', () => native.retryModelLoad(blockId)),
      /** The tile menu's standalone "EQ" row (GalleryBlock's
          blockTypeMenuItems) - adds a ChainBlockType::EQ block, no submenu,
          nothing to pick or load. */
      addEqBlock: (targetInsertId: string) =>
        run<string>('addEqBlock', () => native.addEqBlock(targetInsertId)),
      /** The tile menu's "Dual Mono" row - adds a ChainBlockType::DUAL_MONO
          block, both child slots empty until loadToneIntoDualSlot fills
          one. */
      addDualMonoBlock: (targetInsertId: string) =>
        run<string>('addDualMonoBlock', () => native.addDualMonoBlock(targetInsertId)),
      loadToneIntoDualSlot: (dualBlockId: string, isLeftSide: boolean, toneJson: string) =>
        run<string>('loadToneIntoDualSlot', () =>
          native.loadToneIntoDualSlot(dualBlockId, isLeftSide, toneJson)
        ),
      removeDualSlotContent: (dualBlockId: string, isLeftSide: boolean) =>
        run('removeDualSlotContent', () => native.removeDualSlotContent(dualBlockId, isLeftSide)),
      /** An empty side's own "copy from sibling" button - fills it with a
          clone of the loaded sibling's own content, the quick start for
          artificial stereo before diverging with Align/Pan/Ø. */
      copyDualSlotFromSibling: (dualBlockId: string, toLeftSide: boolean) =>
        run<string>('copyDualSlotFromSibling', () =>
          native.copyDualSlotFromSibling(dualBlockId, toLeftSide)
        ),
      /** The occupied tile's own right-click "Convert to Dual Mono" row -
          replaces the block in place with a fresh Dual Mono wrapper, Left
          seeded from its own content, Right empty. */
      convertBlockToDualMono: (blockId: string) =>
        run<string>('convertBlockToDualMono', () => native.convertBlockToDualMono(blockId)),
      /** Mirror image - the Dual Mono block's own "collapse to single" row,
          only valid with exactly one side loaded. */
      collapseDualMonoToSingle: (blockId: string) =>
        run<string>('collapseDualMonoToSingle', () => native.collapseDualMonoToSingle(blockId)),
      /** Fire-and-forget, safe at knob-drag rates - same shape as
          setBlockParam above, not run()'s coalescing treatment. */
      setDualImage: (blockId: string, leftPan: number, rightPan: number, width: number) => {
        Promise.resolve(native.setDualImage(blockId, leftPan, rightPan, width)).catch((error) =>
          console.error('setDualImage failed:', error)
        );
      },
      setDualLinked: (blockId: string, linked: boolean) =>
        run('setDualLinked', () => native.setDualLinked(blockId, linked)),
      setDualSolo: (blockId: string, isLeftSide: boolean, soloed: boolean) =>
        run('setDualSolo', () => native.setDualSolo(blockId, isLeftSide, soloed)),
      setDualMuted: (blockId: string, isLeftSide: boolean, muted: boolean) =>
        run('setDualMuted', () => native.setDualMuted(blockId, isLeftSide, muted)),
      setDualInvert: (blockId: string, isLeftSide: boolean, inverted: boolean) =>
        run('setDualInvert', () => native.setDualInvert(blockId, isLeftSide, inverted)),
      /** Fire-and-forget, same shape as setDualImage above - Align's whole
          knob/toggle surface lands together, called continuously while a
          knob drags. */
      setDualAlign: (
        blockId: string,
        enabled: boolean,
        offset: number,
        wobble: number,
        wobbleEnabled: boolean,
        crossover: number,
        crossoverEnabled: boolean,
        diffuseEnabled: boolean
      ) => {
        Promise.resolve(
          native.setDualAlign(
            blockId,
            enabled,
            offset,
            wobble,
            wobbleEnabled,
            crossover,
            crossoverEnabled,
            diffuseEnabled
          )
        ).catch((error) => console.error('setDualAlign failed:', error));
      },
      setDualStereoProcessingEnabled: (blockId: string, enabled: boolean) =>
        run('setDualStereoProcessingEnabled', () =>
          native.setDualStereoProcessingEnabled(blockId, enabled)
        ),
      removeBlock: (blockId: string) =>
        run('removeChainBlock', () => native.removeChainBlock(blockId)),
      reorderBlocks: (orderedIds: string[]) =>
        run('reorderChainBlocks', () => native.reorderChainBlocks(orderedIds)),
      /** Move a block into the other lane at the given index (stereo drag). */
      moveBlockToChain: (blockId: string, side: ChainSide, index: number) =>
        run<boolean>('moveBlockToChain', () => native.moveBlockToChain(blockId, side, index)),
      /** Clone a live tone block (all settings + model) into `side` at
          `index` (alt-drag duplicate). Landing on an insert slot fills it;
          anywhere else splices in. Resolves to the new blockId ('' on
          failure). */
      duplicateBlock: (sourceBlockId: string, side: ChainSide, index: number) =>
        run<string>('duplicateChainBlock', () =>
          native.duplicateChainBlock(sourceBlockId, side, index)
        ),
      /** Snapshot a block (tone + settings + model bytes) into the native
          block clipboard. Self-contained: paste keeps working after preset
          switches or deleting the source. `canPaste` flips via the resync. */
      copyBlock: (blockId: string) =>
        run<boolean>('copyChainBlock', () => native.copyChainBlock(blockId)),
      /** Rebuild the copied block into `side` at `index` (an insert slot
          there is filled). Resolves to the new blockId ('' on failure). */
      pasteBlock: (side: ChainSide, index: number) =>
        run<string>('pasteChainBlock', () => native.pasteChainBlock(side, index)),
      setStereoMode: (enabled: boolean) =>
        run('setStereoMode', () => native.setStereoMode(enabled)),
      /** Which channels of a stereo source feed the plugin (faceplate button). */
      setInputMode: (mode: InputMode) => run('setInputMode', () => native.setInputMode(mode)),
      /** The block's NAM A2 size (0 = lite, 1 = full; see BlockParams.
          slimSize). Retiers the loaded engine natively under a short fade;
          part of the chain state, so it lands in presets and undo. */
      setBlockSlimSize: (blockId: string, slimSize: number) =>
        run<boolean>('setBlockSlimSize', () => native.setBlockSlimSize(blockId, slimSize)),
      /** Explicit IR content category (see ToneBlock.irCategory); resets Mix
          (and the -18 dB cab pad) to the new category's fixed default. */
      setBlockIrCategory: (blockId: string, category: 'cab' | 'irPlayer') =>
        run<boolean>('setBlockIrCategory', () => native.setBlockIrCategory(blockId, category)),
      /** Convert a loaded IR block into a real CAB block, or back - see
          ChainActions.convertBlockType. */
      convertBlockType: (blockId: string, targetType: 'cab' | 'ir') =>
        run<boolean>('convertBlockType', () => native.convertBlockType(blockId, targetType)),
      /** IR envelope: a 2-segment Attack/Decay shape (see BlockParams.
          initLevel/attackLength/attackCurve/decayLength/decayLevel/
          decayCurve). Rebuilds the convolver off-thread under a wet-mute
          fade; the caller debounces drag-rate calls (this isn't
          fire-and-forget-safe at knob-drag rates the way setBlockParam is -
          each call queues a real engine rebuild). All six values travel
          together so a drag on one can't clobber another's in-flight
          value. */
      setBlockIrDecay: (
        blockId: string,
        initLevelNormalized: number,
        attackLengthNormalized: number,
        attackCurveNormalized: number,
        decayLengthNormalized: number,
        decayLevelNormalized: number,
        decayCurveNormalized: number
      ) =>
        run<boolean>('setBlockIrDecay', () =>
          native.setBlockIrDecay(
            blockId,
            initLevelNormalized,
            attackLengthNormalized,
            attackCurveNormalized,
            decayLengthNormalized,
            decayLevelNormalized,
            decayCurveNormalized
          )
        ),
      /** IR Size: vari-speed duration/pitch (see BlockParams.size). Same
          off-thread-rebuild caveat as setBlockIrDecay - the caller commits
          once per drag gesture (on release), not continuously. */
      setBlockIrSize: (blockId: string, sizeNormalized: number) =>
        run<boolean>('setBlockIrSize', () => native.setBlockIrSize(blockId, sizeNormalized)),
      /** IR Width: stereo-image crossfade/phase-inversion (see BlockParams.
          width). Same off-thread-rebuild/commit-on-release caveat as
          setBlockIrSize; native no-ops on a mono-source IR. */
      setBlockIrWidth: (blockId: string, widthNormalized: number) =>
        run<boolean>('setBlockIrWidth', () => native.setBlockIrWidth(blockId, widthNormalized)),
      /** Trim Init: manual leading-silence-trim toggle (see BlockParams.
          trimInit/trimRelaxed). Same off-thread rebuild shape as
          setBlockIrSize/setBlockIrWidth, but a plain on/off. `relaxed`
          defaults false so 2-arg call sites keep the standard threshold. */
      setBlockIrTrimInit: (blockId: string, enabled: boolean, relaxed = false) =>
        run<boolean>('setBlockIrTrimInit', () =>
          native.setBlockIrTrimInit(blockId, enabled, relaxed)
        ),
      /** Reverse: manual backward-playback toggle (see BlockParams.
          reverse). Same off-thread rebuild shape as setBlockIrTrimInit. */
      setBlockIrReverse: (blockId: string, enabled: boolean) =>
        run<boolean>('setBlockIrReverse', () => native.setBlockIrReverse(blockId, enabled)),
      /** Resets every IR shaping parameter to default in one step (see
          BlockParams). Single undo entry regardless of how many fields
          change. */
      resetBlockIrShape: (blockId: string) =>
        run<boolean>('resetBlockIrShape', () => native.resetBlockIrShape(blockId)),
      /** Default NAM A2 size for newly added blocks (machine-wide; existing
          blocks keep their own size). Persists on disk. */
      setNamSlimSizeDefault: (slimSize: number) =>
        run('setNamSlimSizeDefault', () => native.setNamSlimSizeDefault(slimSize)),
      /** Multi-core processing (machine-wide). Pure scheduling: applies
          instantly and persists on disk. */
      setMultiCore: (enabled: boolean) => run('setMultiCore', () => native.setMultiCore(enabled)),
      setActiveSide: (side: ChainSide) =>
        run('setActiveEditChain', () => native.setActiveEditChain(side)),
      /** Swap the Left and Right chains wholesale (stereo only). Undoable. */
      swapChains: () => run<boolean>('swapChains', () => native.swapChains()),
      /** Branch the other lane off `side` after one of its tone blocks
          (stereo only). The other lane's input becomes the tapped signal.
          Undoable; native forces the input mode off "stereo". */
      setBranch: (side: ChainSide, afterBlockId: string) =>
        run<boolean>('setChainBranch', () => native.setChainBranch(side, afterBlockId)),
      /** Revert to two fully independent chains. Undoable. */
      clearBranch: () => run<boolean>('clearChainBranch', () => native.clearChainBranch()),
      /**
       * Fire-and-forget param setter (safe at knob-drag rates). Booleans are
       * sent as 0/1; the revision bump on native makes pollers converge.
       */
      setBlockParam: (blockId: string, param: BlockParamName, value: number | boolean) => {
        const numeric = typeof value === 'boolean' ? (value ? 1 : 0) : value;
        Promise.resolve(native.setBlockParam(blockId, param, numeric)).catch((error) =>
          console.error(`setBlockParam(${param}) failed:`, error)
        );
      },
      /**
       * Fire-and-forget whole-band EQ update (safe at dot-drag rates). The
       * band object is the atomic mutation unit, clean for undo/redo later.
       */
      setBlockEqBand: (blockId: string, bandIndex: number, band: EqBand) => {
        Promise.resolve(native.setBlockEqBand(blockId, bandIndex, band)).catch((error) =>
          console.error('setBlockEqBand failed:', error)
        );
      },
      /** EQ power/bypass: band settings persist, processing is skipped. */
      setBlockEqEnabled: (blockId: string, enabled: boolean) =>
        run<boolean>('setBlockEqEnabled', () => native.setBlockEqEnabled(blockId, enabled)),
      /** EQ position: pre = before the block's model, off = after the model (wet only). */
      setBlockEqPre: (blockId: string, pre: boolean) =>
        run<boolean>('setBlockEqPre', () => native.setBlockEqPre(blockId, pre)),
      /** Back to flat defaults (and native skips EQ processing again). */
      resetBlockEq: (blockId: string) =>
        run<boolean>('resetBlockEq', () => native.resetBlockEq(blockId)),
      /** Step the chain edit history. No-ops (false) at the stack ends. */
      undo: () => run<boolean>('undoChain', () => native.undoChain()),
      redo: () => run<boolean>('redoChain', () => native.redoChain()),
      /** Back to the factory-default state: empty mono chain, faceplate
          params at defaults, no active preset. Undoable (chain part). */
      resetToDefault: () => run<boolean>('resetToDefault', () => native.resetToDefault()),
    }),
    [native, run]
  );

  return {
    chain: state.chain,
    chainRight: state.chainRight ?? null,
    branch: state.branch ?? null,
    canUndo: state.canUndo ?? false,
    canRedo: state.canRedo ?? false,
    canPaste: state.canPasteBlock ?? false,
    atDefault: state.atDefault,
    activePreset: state.preset ?? null,
    stereoEnabled: state.stereoEnabled,
    stereoInput: state.stereoInput ?? false,
    stereoOutput: state.stereoOutput ?? true,
    inputMode: state.inputMode ?? 'stereo',
    namSlimSizeDefault: state.namSlimSizeDefault ?? SLIM_SIZE_LITE,
    multiCore: state.multiCore ?? true,
    standalone: state.standalone ?? false,
    sampleRate: state.sampleRate || 48000,
    refresh,
    actions,
  };
}
