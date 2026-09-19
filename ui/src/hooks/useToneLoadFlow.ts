import { useCallback } from 'react';
import type { useChainState } from './useChainState';
import { DETAIL_BLOCK_STORAGE_KEY } from '../components/ChainView';
import type { ChainSide } from '../types/chain';
import type { Model, Tone } from '../types/tone';

type ChainStateActions = ReturnType<typeof useChainState>['actions'];

// Swap/insert targets must survive the Select flow's full-page OAuth
// redirect (the webview navigates to tone3000.com and back, remounting
// React), so they live in sessionStorage rather than component state.
// Native falls back gracefully when an id went stale.
const SWAP_STORAGE_KEY = 't3k.pendingSwapBlockId';
const INSERT_TARGET_STORAGE_KEY = 't3k.pendingInsertBlockId';
// A Dual Mono child slot isn't a lane insert slot (see ToneBlock.dualLeft/
// dualRight) - it needs two values (which block, which side), encoded as
// one string since sessionStorage only holds strings anyway. Exactly one of
// SWAP_STORAGE_KEY / INSERT_TARGET_STORAGE_KEY / this is ever pending at a
// time, same as the other two.
const DUAL_TARGET_STORAGE_KEY = 't3k.pendingDualBlockId';
/** Set only when the pending add/swap should open its resulting block's
    detail view once it lands (ChainMapStrip's "+", or the detail card's own
    ⇄ swap button — see ChainActions.addModel's and .swapBlock's own
    `navigateToDetail`); absent for the gallery's own "+" tiles and tile
    swap action, both of which stay right where they were (issue: a gallery
    swap was landing on the swapped block's detail view instead of staying
    on the gallery, because this used to be a swap-only, unconditional
    `OPEN_DETAIL_ON_SWAP = true` with no per-call-site distinction - it
    assumed a swap always targets an already-open block, which the gallery
    tile's own swap action disproves). Shared between add and swap since
    exactly one of SWAP_STORAGE_KEY / INSERT_TARGET_STORAGE_KEY is ever
    pending at a time, so one flag unambiguously describes whichever flow is
    in flight. */
const NAVIGATE_ON_PICK_KEY = 't3k.pendingPickNavigate';

/** Sanity cap for dropped files; real .nam files and IRs are a few MB, and
    the bytes ride the native bridge as base64 strings. */
const MAX_LOCAL_FILE_BYTES = 50 * 1024 * 1024;

/** Cap on models loaded from one dropped folder (matches the catalog's
    per-tone model limit). */
const MAX_FOLDER_MODELS = 300;

/** FileReader emits base64 directly (as a data URL), skipping a manual
    ArrayBuffer-to-string pass over multi-MB files. */
const readFileBase64 = (file: File) =>
  new Promise<string>((resolve, reject) => {
    const reader = new FileReader();
    reader.onload = () => resolve((reader.result as string).split(',', 2)[1] ?? '');
    reader.onerror = () => reject(reader.error);
    reader.readAsDataURL(file);
  });

const extensionOf = (name: string) => name.slice(name.lastIndexOf('.') + 1).toLowerCase();

const stripExtension = (name: string) => {
  const dot = name.lastIndexOf('.');
  return dot > 0 ? name.slice(0, dot) : name;
};

/** All files under a dropped directory, subfolders included. readEntries
    hands out batches (Chromium caps them at 100), so each reader drains in
    a loop. */
const readDirectoryFiles = async (root: FileSystemDirectoryEntry): Promise<File[]> => {
  const files: File[] = [];
  const pending: FileSystemDirectoryEntry[] = [root];
  while (pending.length > 0) {
    const reader = pending.pop()!.createReader();
    for (;;) {
      const entries = await new Promise<FileSystemEntry[]>((resolve, reject) =>
        reader.readEntries(resolve, reject)
      );
      if (entries.length === 0) break;
      for (const entry of entries) {
        if (entry.isDirectory) pending.push(entry as FileSystemDirectoryEntry);
        else
          files.push(
            await new Promise<File>((resolve, reject) =>
              (entry as FileSystemFileEntry).file(resolve, reject)
            )
          );
      }
    }
  }
  return files;
};

interface UseToneLoadFlowOptions {
  actions: ChainStateActions;
  stereoEnabled: boolean;
  /** The connection gate's action wrapper (see useConnectionGate). */
  requireConnection: (action: () => void | Promise<void>) => void;
  /** Open or close the in-plugin tone browser. */
  setShowToneBrowser: (show: boolean) => void;
}

/**
 * The "get a tone into the chain" flow: the add (+) and swap entry points
 * that open the browser, and the landing handler that routes a picked tone
 * into the remembered target slot or block.
 */
export function useToneLoadFlow({
  actions,
  stereoEnabled,
  requireConnection,
  setShowToneBrowser,
}: UseToneLoadFlowOptions) {
  // A fully-resolved tone landed (Select callback or a browser card pick).
  // If a swap was pending, replace that block in place; otherwise add the
  // tone at the remembered insert slot.
  const handleToneSelected = useCallback(
    async (tone: Tone & { models: Model[] }) => {
      if (!tone.models || tone.models.length === 0) {
        console.error('Tone has no models');
        return;
      }

      // Consume the pending targets up front so they can never leak into a
      // later selection. (Each flow clears the other keys before starting.)
      const swapBlockId = sessionStorage.getItem(SWAP_STORAGE_KEY);
      sessionStorage.removeItem(SWAP_STORAGE_KEY);
      const insertBlockId = sessionStorage.getItem(INSERT_TARGET_STORAGE_KEY);
      sessionStorage.removeItem(INSERT_TARGET_STORAGE_KEY);
      const dualTarget = sessionStorage.getItem(DUAL_TARGET_STORAGE_KEY);
      sessionStorage.removeItem(DUAL_TARGET_STORAGE_KEY);

      const toneJson = JSON.stringify(tone);

      // Shared between the swap and add branches below - see its own
      // comment. Read once, up front, same as the swap/insert targets.
      const navigateToDetail = sessionStorage.getItem(NAVIGATE_ON_PICK_KEY) === '1';
      sessionStorage.removeItem(NAVIGATE_ON_PICK_KEY);

      // Closing the browser unmounts it and mounts a fresh ChainView (see
      // Plugin's showToneBrowser ternary), whose detail-view state reads
      // DETAIL_BLOCK_STORAGE_KEY once, at that mount. So the browser can only
      // close *after* we know which block to hand it — closing first and
      // writing the key once the native call resolves would land one render
      // too late (issue #114).
      if (swapBlockId) {
        const swapped = await actions.swapTone(swapBlockId, toneJson);
        if (swapped) {
          if (navigateToDetail) sessionStorage.setItem(DETAIL_BLOCK_STORAGE_KEY, swapBlockId);
          setShowToneBrowser(false);
          return;
        }
        console.warn('Swap target no longer exists; adding tone as a new block');
      }

      // Dual Mono child slot: always reopens the wrapper's own detail view
      // (there's no gallery-tile entry point for a dual slot pick, unlike
      // swap/add above - handleAddToDualSlot is only ever called from
      // inside that detail view - see its own comment), so this is
      // unconditional, not a per-call-site navigateToDetail flag. Closing
      // the browser unmounts ChainView (see the comment above), which loses
      // its in-memory detailBlockId even though the *value* never actually
      // changed - re-stamping DETAIL_BLOCK_STORAGE_KEY here is what the
      // freshly-mounted ChainView reads on its next render (issue: without
      // this, picking a tone for one side landed back on the gallery
      // instead of staying put for the second side - the same fix the
      // detail card's own swap button already needs and takes, see its
      // `navigateToDetail: true`).
      if (dualTarget) {
        const [dualBlockId, sideFlag] = dualTarget.split(':');
        const childId = await actions.loadToneIntoDualSlot(
          dualBlockId,
          sideFlag === 'L',
          toneJson
        );
        if (!childId) console.error('Dual Mono slot target no longer exists');
        else sessionStorage.setItem(DETAIL_BLOCK_STORAGE_KEY, dualBlockId);
        setShowToneBrowser(false);
        return;
      }

      const blockId = await actions.loadTone(toneJson, insertBlockId ?? undefined);
      if (!blockId) {
        console.error('Failed to load tone');
        setShowToneBrowser(false);
        return;
      }
      if (navigateToDetail) sessionStorage.setItem(DETAIL_BLOCK_STORAGE_KEY, blockId);
      setShowToneBrowser(false);
    },
    [actions, setShowToneBrowser]
  );

  // Add: remember the clicked insert slot, then open the browser. The active
  // side also goes to native state (it has to survive the OAuth redirect) as
  // the fallback for when the slot id goes stale, e.g. undone away mid-flow.
  const handleAddModel = useCallback(
    (side: ChainSide, insertBlockId: string, options?: { navigateToDetail?: boolean }) => {
      requireConnection(async () => {
        sessionStorage.removeItem(SWAP_STORAGE_KEY);
        sessionStorage.setItem(INSERT_TARGET_STORAGE_KEY, insertBlockId);
        if (options?.navigateToDetail) sessionStorage.setItem(NAVIGATE_ON_PICK_KEY, '1');
        else sessionStorage.removeItem(NAVIGATE_ON_PICK_KEY);
        if (stereoEnabled) await actions.setActiveSide(side);
        setShowToneBrowser(true);
      });
    },
    [actions, requireConnection, setShowToneBrowser, stereoEnabled]
  );

  // Dual Mono slot: remember which block + side, then run the same browse
  // flow as add/swap. No side/stereo bookkeeping to survive the OAuth
  // redirect (unlike handleAddModel's stereoEnabled/setActiveSide) - a dual
  // slot's own side (left/right child) is independent of the chain's
  // stereo mode.
  const handleAddToDualSlot = useCallback(
    (dualBlockId: string, isLeftSide: boolean) => {
      requireConnection(() => {
        sessionStorage.removeItem(SWAP_STORAGE_KEY);
        sessionStorage.removeItem(INSERT_TARGET_STORAGE_KEY);
        sessionStorage.setItem(DUAL_TARGET_STORAGE_KEY, `${dualBlockId}:${isLeftSide ? 'L' : 'R'}`);
        sessionStorage.removeItem(NAVIGATE_ON_PICK_KEY);
        setShowToneBrowser(true);
      });
    },
    [requireConnection, setShowToneBrowser]
  );

  // Swap: remember the target block, then run the same browse flow as add.
  // The pending swap id is consumed when the picked tone lands. Was
  // unconditionally `OPEN_DETAIL_ON_SWAP = true` (issue #114): a real trace
  // (gallery-scroll-reset investigation) confirmed the gallery tile's own
  // swap action shares this exact handler and was forcing its way into the
  // swapped block's detail view instead of staying on the gallery, since
  // nothing distinguished it from the detail card's ⇄ button. Mirrors
  // handleAddModel's own per-call-site `navigateToDetail` for the same
  // reason: the caller, not this shared handler, knows whether it's already
  // showing a detail view worth staying in.
  const handleSwapBlock = useCallback(
    (blockId: string, options?: { navigateToDetail?: boolean }) => {
      requireConnection(() => {
        sessionStorage.removeItem(INSERT_TARGET_STORAGE_KEY);
        sessionStorage.setItem(SWAP_STORAGE_KEY, blockId);
        if (options?.navigateToDetail) sessionStorage.setItem(NAVIGATE_ON_PICK_KEY, '1');
        else sessionStorage.removeItem(NAVIGATE_ON_PICK_KEY);
        setShowToneBrowser(true);
      });
    },
    [requireConnection, setShowToneBrowser]
  );

  // Drop a local .nam/.wav (or a folder of them) on a tile: no browser, no
  // auth, the file bytes ride the bridge and native validates/loads them.
  // An insert slot adds a block; an existing tone tile swaps in place.
  // `category` is the empty-slot split drop zone's explicit IR/Cab choice
  // (see ChainActions.loadLocalFile's own comment) - passed straight
  // through to loadLocalTone's forceGear param. Resolves to a user-facing
  // error message (the tile toasts it), or null on success.
  const handleDropFile = useCallback(
    async (
      targetBlockId: string,
      item: DataTransferItem,
      category?: 'ir' | 'cab'
    ): Promise<string | null> => {
      // Synchronous reads: the DataTransferItem goes inert once the drop
      // handler yields (the entry/file objects stay usable).
      const entry = item.webkitGetAsEntry();
      const singleFile = entry?.isDirectory ? null : item.getAsFile();

      try {
        // A folder loads as one multi-model tone: title from the folder,
        // one model per file of its majority extension (.nam vs .wav, which
        // also decides NAM vs IR), everything else ignored.
        if (entry?.isDirectory) {
          const all = await readDirectoryFiles(entry as FileSystemDirectoryEntry);
          const nams = all.filter((f) => extensionOf(f.name) === 'nam');
          const wavs = all.filter((f) => extensionOf(f.name) === 'wav');
          const files = nams.length >= wavs.length ? nams : wavs;
          if (files.length === 0) return 'No .nam or .wav files in the folder';
          if (files.length > MAX_FOLDER_MODELS)
            return `Folder has too many files (max ${MAX_FOLDER_MODELS})`;
          if (files.some((f) => f.size > MAX_LOCAL_FILE_BYTES)) return 'A file is too large';

          // Directory read order is unspecified; natural name order makes
          // the model list stable ("amp 2" before "amp 10").
          files.sort((a, b) => a.name.localeCompare(b.name, undefined, { numeric: true }));
          const payload = await Promise.all(
            files.map(async (f) => ({ name: f.name, data: await readFileBase64(f) }))
          );
          return await actions.loadLocalTone(entry.name, payload, targetBlockId, category);
        }

        if (!singleFile) return "Couldn't read the dropped file";
        const extension = extensionOf(singleFile.name);
        if (extension !== 'nam' && extension !== 'wav')
          return 'Only .nam and .wav files are supported';
        if (singleFile.size > MAX_LOCAL_FILE_BYTES) return 'File is too large';
        return await actions.loadLocalTone(
          stripExtension(singleFile.name),
          [{ name: singleFile.name, data: await readFileBase64(singleFile) }],
          targetBlockId,
          category
        );
      } catch (error) {
        console.error('Local file drop failed:', error);
        return "Couldn't read the dropped file";
      }
    },
    [actions]
  );

  // Abandon any pending targets (browser closed without picking, logout).
  const clearPendingTargets = useCallback(() => {
    sessionStorage.removeItem(SWAP_STORAGE_KEY);
    sessionStorage.removeItem(INSERT_TARGET_STORAGE_KEY);
    sessionStorage.removeItem(DUAL_TARGET_STORAGE_KEY);
    sessionStorage.removeItem(NAVIGATE_ON_PICK_KEY);
  }, []);

  return {
    handleToneSelected,
    handleAddModel,
    handleAddToDualSlot,
    handleSwapBlock,
    handleDropFile,
    clearPendingTargets,
  };
}
