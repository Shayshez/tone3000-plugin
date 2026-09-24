import { createContext, useContext } from 'react';
import type { BlockParamName, EqBand, ToneBlock } from '../types/chain';
import type { Model, Tone } from '../types/tone';

/**
 * Everything a chain block (gallery tile or detail card) can do, bundled
 * into one context so the tree doesn't thread a dozen callback props from
 * `Plugin` down through `ChainView`, and so leaf components can be
 * `React.memo`d without every parent re-render defeating it via fresh
 * lambdas.
 *
 * The provider value lives in `Plugin` and is memoized there; everything in
 * it is either a `useChainState` action (stable) or a stable callback.
 */
export interface ChainActions {
  /** Launch the Select flow, adding into the clicked insert slot.
      `navigateToDetail` (default false, the gallery's own "+" tiles): when
      true (ChainMapStrip's "+"), open the newly added block's detail view
      once it lands instead of leaving the caller wherever it was. */
  addModel: (insertBlockId: string, options?: { navigateToDetail?: boolean }) => void;
  /** Load a drop on a tile: a .nam / .wav file (NAM must be A2), or a folder
      of them (one block, one model per file). An insert slot adds; an
      existing tone tile swaps in place. `category` is the empty-slot split
      drop zone's explicit IR/Cab choice (AddTile.tsx) - 'cab' forces a real
      Cab block regardless of the file's own detected length; omitted (every
      other drop target) keeps the existing content-duration guess.
      Resolves to a user-facing error message, or null on success. */
  loadLocalFile: (
    targetBlockId: string,
    item: DataTransferItem,
    category?: 'ir' | 'cab'
  ) => Promise<string | null>;
  /** Menu-driven sibling of loadLocalFile: native opens its OS file picker
      and loads the pick (a .nam/.wav file, or a folder of them) from its
      path. Same targeting rules; the reliable route on Linux, where OS file
      drags never reach the embedded webview. `category` is the tile menu's
      own Cab/IR row choice (see GalleryBlock's blockTypeMenuItems) - same
      meaning as loadLocalFile's own, inert when the pick turns out to be
      .nam. Resolves to a user-facing error message, or null on success or
      when the dialog is cancelled. */
  pickLocalFile: (
    targetBlockId: string,
    kind: 'file' | 'folder',
    category?: 'ir' | 'cab'
  ) => Promise<string | null>;
  /** The tile menu's standalone "EQ" row (see GalleryBlock's
      blockTypeMenuItems): adds a ChainBlockType::EQ block at the given
      insert slot - no submenu, no file to pick, nothing to load. Unlike
      Cab/IR's rows this doesn't navigate to the new block's detail view
      either, matching those rows' own behavior (add and stay put). */
  addEqBlock: (targetInsertId: string) => void;
  /** The tile menu's "Dual Mono" row: adds a ChainBlockType::DUAL_MONO
      block at the given insert slot - a fixed pair of two child slots,
      both empty until addToDualSlot fills one. No submenu, no navigation,
      same "add and stay put" behavior as addEqBlock. */
  addDualMonoBlock: (targetInsertId: string) => void;
  /** A Dual Mono child slot's own "+"/swap: opens the Select flow the same
      way `addModel` does for an ordinary insert slot, remembering which
      block + side to load the picked tone into (see useToneLoadFlow's
      handleAddToDualSlot) - not a direct native call, same shape as
      `addModel` not being one either. */
  addToDualSlot: (dualBlockId: string, isLeftSide: boolean) => void;
  /** Clears one side of a Dual Mono block back to empty. */
  removeDualSlotContent: (dualBlockId: string, isLeftSide: boolean) => void;
  /** An empty side's own "copy from sibling" button: fills it with a clone
      of the loaded sibling's own content. Only valid when this side is
      empty and the sibling is loaded. */
  copyDualSlotFromSibling: (dualBlockId: string, toLeftSide: boolean) => void;
  /** The occupied tile's own right-click "Convert to Dual Mono" row:
      replaces this block in place with a fresh Dual Mono wrapper, Left
      seeded from its own content, Right left empty. Resolves to the new
      wrapper's blockId (null on failure) - the caller jumps the detail
      view to it, since this block's own id stops resolving. */
  convertBlockToDualMono: (blockId: string) => Promise<string | null>;
  /** Mirror image - a Dual Mono block's own "collapse to single" button:
      replaces the wrapper in place with an ordinary block seeded from
      whichever side is loaded. Only valid with exactly one side loaded.
      Resolves to the new block's blockId (null on failure), same
      "caller jumps the detail view" reasoning as convertBlockToDualMono. */
  collapseDualMonoToSingle: (blockId: string) => Promise<string | null>;
  /** Live Pan L/Pan R/Width for a Dual Mono block's recombine - called
      continuously while a knob drags (native-side smoothing handles
      click-avoidance), same idiom as every other continuous param. */
  setDualImage: (blockId: string, leftPan: number, rightPan: number, width: number) => void;
  /** Persists a Dual Mono block's Link toggle - the mirror/sync behavior
      itself runs client-side (see BlockParams.dualLinked's own comment),
      this just remembers on/off. */
  setDualLinked: (blockId: string, linked: boolean) => void;
  /** Exclusive per-side solo for a Dual Mono block - soloing one side
      clears the other's. */
  setDualSolo: (blockId: string, isLeftSide: boolean, soloed: boolean) => void;
  /** Per-side Mute for a Dual Mono block - true silence, unconditional
      whether the side is empty or loaded (see BlockParams.dualLeftMuted's
      own comment). */
  setDualMuted: (blockId: string, isLeftSide: boolean, muted: boolean) => void;
  /** Per-side polarity flip (Ø) for a Dual Mono block - independent per
      side, unlike Solo's exclusivity. */
  setDualInvert: (blockId: string, isLeftSide: boolean, inverted: boolean) => void;
  /** Align for a Dual Mono block - one bundled call, same idiom as
      setDualImage (a whole knob/toggle surface lands together, called
      continuously while a knob drags). */
  setDualAlign: (
    blockId: string,
    enabled: boolean,
    offset: number,
    wobble: number,
    wobbleEnabled: boolean,
    crossover: number,
    crossoverEnabled: boolean,
    diffuseEnabled: boolean
  ) => void;
  /** Master bypass for the whole Stereo Processing screen (Align + Ø),
      without touching any of the dialed-in values. */
  setDualStereoProcessingEnabled: (blockId: string, enabled: boolean) => void;
  removeBlock: (blockId: string) => void;
  /** Launch the Select flow to replace this block's tone in place.
      `navigateToDetail` (default false, the gallery tile's own swap action):
      when true (the detail card's ⇄ button), land on the swapped block's
      detail view once it lands instead of leaving the caller on the
      gallery. Mirrors addModel's own option - see its doc comment. */
  swapBlock: (blockId: string, options?: { navigateToDetail?: boolean }) => void;
  /** Copy the tone's TONE3000 URL; resolves true when it hit the clipboard. */
  shareBlock: (block: ToneBlock) => Promise<boolean>;
  /** Reorder the chain (full order including its insert slot). */
  reorderBlocks: (orderedIds: string[]) => void;
  /** Move a block to the given index within the chain (drag reorder). */
  moveBlock: (blockId: string, index: number) => void;
  /** Clone a live tone block (all settings + model) into `index` (alt-drag
      duplicate). An insert slot there is filled, otherwise the clone
      splices in. */
  duplicateBlock: (sourceBlockId: string, index: number) => void;
  /** Copy a block into the native block clipboard (tone + settings + model
      bytes). The snapshot is self-contained, so pasting keeps working after
      preset switches or deleting the source block. */
  copyBlock: (blockId: string) => void;
  /** Paste the copied block into `index` (the insert slot there is filled).
      Gate on `canPaste` from useChainState. */
  pasteBlock: (index: number) => void;
  /** Put a fresh empty "+" slot at lane `index` (the chip menu's Add Slot
      Left/Right); one undo step. */
  addInsertSlot: (index: number) => void;
  /** Remove an empty "+" slot (never the lane's rightmost one); one undo step. */
  removeInsertSlot: (insertBlockId: string) => void;
  /** Native only stores the active model, so the switch always carries the
      model object (paged in from the API by the picker, or a local tone's
      own model list); id/name/model_url is all native needs. */
  switchModel: (
    blockId: string,
    modelId: number,
    model: Pick<Model, 'id' | 'name' | 'model_url'>
  ) => Promise<void>;
  /** Retry a failed model download (`block.loadFailed`); re-queues the
      block's active model through the native background loader. */
  retryLoad: (blockId: string) => void;
  /**
   * Fetch a tone's full model catalog (tones max out at 300 models; NAM is
   * architecture-filtered). Backs the detail card's model picker, as the
   * persisted block only carries the active model.
   */
  listToneModels: (toneId: number, format: string | undefined) => Promise<Model[]>;
  /**
   * Fetch a tone's full catalog metadata (description, makes, tags, url).
   * Backs the detail card's info panel; not written into saved state.
   */
  getTone: (toneId: number) => Promise<Tone>;
  /** Favorite / unfavorite a tone for the signed-in user (idempotent). */
  setToneFavorite: (toneId: number, favorite: boolean) => Promise<void>;
  /** Push a fresh /tones/{id} payload into native, which merges it into
      every block holding that tone (stored models preserved). Best-effort
      background sync: metadata only, not undoable, no-op when unchanged. */
  refreshToneMetadata: (toneJson: string) => void;
  /** Fire-and-forget per-block param setter (see useChainState). */
  setBlockParam: (blockId: string, param: BlockParamName, value: number | boolean) => void;
  /** The block's NAM A2 size (0 = lite, 1 = full); retiers the loaded
      engine natively. Backs the header LITE/FULL toggle. */
  setBlockSlimSize: (blockId: string, slimSize: number) => void;
  /** Explicit IR content category (see ToneBlock.irCategory); resets Mix to
      the new category's fixed default. IR blocks only. */
  setBlockIrCategory: (blockId: string, category: 'cab' | 'irPlayer') => void;
  /** Convert a loaded IR block into a real ChainBlockType::CAB block, or a
      CAB block back into an IR block (see ToneBlock.blockType) - the header
      "Cab / IR Player" control's actual conversion action. The loaded sample
      carries over: "cab" applies the same 500ms truncation a
      site-loaded Cab tone gets, "ir" restores the full original sample (the
      round trip doesn't remember the truncation) and lands explicitly in
      the IrCategory::IrPlayer category. No-op for an unloaded block or a
      block already at the target type. */
  convertBlockType: (blockId: string, targetType: 'cab' | 'ir') => void;
  /** IR envelope: a 2-segment Attack/Decay shape (see BlockParams.initLevel/
      attackLength/attackCurve/decayLength/decayLevel/decayCurve). Not
      fire-and-forget-safe at knob-drag rates - rebuilds the convolver
      off-thread, so callers should debounce (see ChainBlock.tsx's shaping
      row). All six values arrive together (like setBlockEqBand's
      whole-band updates) so a drag on one can't clobber another's in-flight
      value. */
  setBlockIrDecay: (
    blockId: string,
    initLevelNormalized: number,
    attackLengthNormalized: number,
    attackCurveNormalized: number,
    decayLengthNormalized: number,
    decayLevelNormalized: number,
    decayCurveNormalized: number
  ) => void;
  /** IR Size: vari-speed duration/pitch (see BlockParams.size). Same
      off-thread-rebuild caveat as setBlockIrDecay - callers commit once per
      drag gesture (on release), not continuously. */
  setBlockIrSize: (blockId: string, sizeNormalized: number) => void;
  /** IR Width: stereo-image crossfade/phase-inversion (see BlockParams.
      width). Same off-thread-rebuild/commit-on-release caveat as
      setBlockIrSize; no-op on a mono-source IR. */
  setBlockIrWidth: (blockId: string, widthNormalized: number) => void;
  /** Trim Init: manual leading-silence-trim toggle (see BlockParams.
      trimInit/trimRelaxed). Same off-thread rebuild shape as setBlockIrSize/
      setBlockIrWidth, but a plain on/off - no drag gesture to debounce.
      `relaxed` (default false) picks the less-sensitive onset threshold;
      always pass both together so a single click is a single undo step. */
  setBlockIrTrimInit: (blockId: string, enabled: boolean, relaxed?: boolean) => void;
  /** Reverse: manual backward-playback toggle (see BlockParams.reverse).
      Same off-thread rebuild shape as setBlockIrTrimInit. */
  setBlockIrReverse: (blockId: string, enabled: boolean) => void;
  /** Resets every IR shaping parameter (Init/Attack/Decay, Size, Width,
      Trim Init, Reverse) to default in one step - a single undo entry
      regardless of how many fields actually change. */
  resetBlockIrShape: (blockId: string) => void;
  /** Fire-and-forget whole-band EQ setter (see useChainState). */
  setBlockEqBand: (blockId: string, bandIndex: number, band: EqBand) => void;
  /** EQ power/bypass: band settings persist, processing is skipped. */
  setBlockEqEnabled: (blockId: string, enabled: boolean) => void;
  /** EQ position: pre = before the block's model, off = after the model (wet only). */
  setBlockEqPre: (blockId: string, pre: boolean) => void;
  resetBlockEq: (blockId: string) => void;
  /** Copy this block's EQ bands into the native EQ clipboard. */
  copyBlockEq: (blockId: string) => void;
  /** Paste the copied EQ bands onto this block's EQ (powers it on; one undo
      step). Gate on `canPasteEq`. */
  pasteBlockEq: (blockId: string) => void;
  /** Whether the EQ clipboard holds copied bands. */
  canPasteEq: boolean;
  /** Switch a block's active channel (0-3 = A-D). */
  selectBlockChannel: (blockId: string, channel: number) => void;
  /** Overwrite channel `to` with channel `from`'s settings. */
  copyBlockChannel: (blockId: string, from: number, to: number) => void;
  /**
   * Whether a TONE3000 session is present. Auth-dependent block actions
   * (model switching, where native re-downloads the model with a Bearer token)
   * disable themselves when signed out.
   */
  authenticated: boolean;
  /** Kick off TONE3000 login (connection-gated); used by the info panel CTA. */
  login: () => void;
}

const ChainActionsContext = createContext<ChainActions | null>(null);

export const ChainActionsProvider = ChainActionsContext.Provider;

export function useChainActions(): ChainActions {
  const actions = useContext(ChainActionsContext);
  if (!actions) throw new Error('useChainActions must be used inside a ChainActionsProvider');
  return actions;
}
