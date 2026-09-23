import React, { useCallback, useEffect, useLayoutEffect, useRef, useState } from 'react';
import { getUiScale, rem } from '../hooks/useUiScale';
import { DragDropProvider } from '@dnd-kit/react';
import { isSortable } from '@dnd-kit/react/sortable';
import { KeyboardSensor, PointerActivationConstraints, PointerSensor } from '@dnd-kit/dom';
import type { DragDropManager, DragEndEvent, DragStartEvent, Sensors } from '@dnd-kit/dom';
import { arrayMove } from '@dnd-kit/helpers';
import { ChainBlock } from './ChainBlock';
import { EdgeFade, GalleryLane, EDGE_FADE_WIDTH, TILE_SIZE, TILE_GAP } from './GalleryLane';
import { useChainActions } from '../hooks/useChainActions';
import type { DetailView } from '../hooks/useDetailViewStack';
import { useHorizontalWheelScroll } from '../hooks/useHorizontalWheelScroll';
import { FONT_MONO, WHITE } from './theme';
import type { ChainItem, ToneBlock } from '../types/chain';
import { isInsertSlot } from '../types/chain';

/**
 * Chain gallery: blocks render as square image tiles in a horizontal,
 * left-to-right lane over a static ghost rail of plus circles joined by
 * connector lines. Dragging a tile away reveals the rail behind its slot.
 * (Lane internals live in GalleryLane.tsx; this component owns the drag
 * orchestration.) Tap/click opens the detail takeover; drag a tile to
 * reorder.
 */

/**
 * The block whose detail takeover is open, persisted so it survives this
 * component unmounting while the tone browser (and its OAuth redirect) is up.
 * Cleared from Plugin on preset load so a remount lands on the gallery.
 */
export const DETAIL_BLOCK_STORAGE_KEY = 't3k.detailBlockId';

/**
 * Gallery scroll offset in design px, persisted for the same reason: the
 * scroller unmounts under the detail takeover, the tone browser, and the
 * tuner, and coming back should land where the user left off (issue #82).
 * Cleared from Plugin on preset load so a new chain starts at the left edge.
 */
export const CHAIN_SCROLL_STORAGE_KEY = 't3k.chainScroll';

interface ChainViewProps {
  chain: ChainItem[];
  /** Whether the native block clipboard holds a copied block (enables Paste
      on insert slots). Survives preset switches: the clipboard snapshot is
      self-contained, not a reference into the current chain. */
  canPaste: boolean;
  sampleRate: number;
  /** Default NAM A2 size for new blocks; the detail card's size chip only
      shows when a block differs from it. */
  namSlimSizeDefault: number;
  /** Block info view: drop the meter-band bottom pad so scroll reaches the faceplate. */
  onFillToFaceplate?: (fill: boolean) => void;
  /** Bumped on preset load so an open detail takeover returns to the gallery. */
  returnToGallery?: number;
}

/** Design-px of travel before a drag engages, so tap/click stays a click.
    Scaled to real px per gesture so the feel tracks the rendered tile size. */
const GALLERY_DRAG_DISTANCE_PX = 6;

const sensors: Sensors = [
  // Distance-only activation (the stock constraints add a 200ms hold trigger,
  // which would turn a slow click-to-open into a drag). The sensor's default
  // guard already keeps buttons and other interactive chrome from starting
  // drags, so power/swap/trash stay clicks.
  PointerSensor.configure({
    activationConstraints: () => [
      new PointerActivationConstraints.Distance({
        value: GALLERY_DRAG_DISTANCE_PX * getUiScale(),
      }),
    ],
  }),
  // Stock keyboard sorting: Space or Enter on a focused tile picks it up,
  // arrows snap it one slot per press (the sortable's SortableKeyboardPlugin
  // owns the targeting), Space/Enter drops, Escape cancels. A grab can only
  // start on the focused tile, so this stays intentional: Space/Enter
  // anywhere else still falls through to the host DAW (see keyPassthrough.ts).
  KeyboardSensor,
];

/** Scroll-restore target queued for the next return to the gallery: either a
    blockId (explicit Back — the block still exists, see onBack) or an index
    (the open block vanished out from under us — trash, undo, redo... see the
    scroll-restore effect in ChainView). */
type PendingScroll = { kind: 'id'; blockId: string } | { kind: 'index'; index: number };

/** Id of the ⌥-duplicate stand-in: the inert copy of the dragged block that
    holds its home slot while the standard drag machinery runs untouched. */
const DUP_STAND_IN_ID = '__duplicate-stand-in__';

export const ChainView: React.FC<ChainViewProps> = ({
  chain,
  canPaste,
  sampleRate,
  namSlimSizeDefault,
  onFillToFaceplate,
  returnToGallery = 0,
}) => {
  const actions = useChainActions();
  const wheelScrollRef = useHorizontalWheelScroll<HTMLDivElement>();
  // Live scroll-div node, kept only so the recenter effect below (see
  // pendingScrollTargetRef) can read scrollWidth/clientWidth and override
  // galleryScrollRef's raw-offset restore with a recomputed tile position.
  const galleryScrollElRef = useRef<HTMLDivElement | null>(null);
  // One callback ref wires the scroller: it restores the saved offset before
  // first paint, persists it as the user scrolls, and attaches the wheel
  // hook's panning. The hook returns a cleanup (and once a ref callback
  // returns a cleanup React never calls it with null), so it must be
  // forwarded here, not swallowed: StrictMode's dev double-attach would
  // stack a second wheel listener.
  const galleryScrollRef = useCallback(
    (el: HTMLDivElement) => {
      galleryScrollElRef.current = el;
      // Stored in design px so a window rescale between visits lands in the
      // same place; an offset past the end (the chain shrank) clamps on
      // assignment.
      const saved = Number(sessionStorage.getItem(CHAIN_SCROLL_STORAGE_KEY));
      if (saved > 0) el.scrollLeft = saved * getUiScale();
      const save = () =>
        sessionStorage.setItem(CHAIN_SCROLL_STORAGE_KEY, String(el.scrollLeft / getUiScale()));
      el.addEventListener('scroll', save, { passive: true });
      const wheelCleanup = wheelScrollRef(el);
      return () => {
        el.removeEventListener('scroll', save);
        if (typeof wheelCleanup === 'function') wheelCleanup();
        galleryScrollElRef.current = null;
      };
    },
    [wheelScrollRef]
  );
  // Persisted so the detail takeover survives this component unmounting: a
  // swap from the detail view opens the tone browser (which replaces the whole
  // chain view, and may bounce through the tone3000.com OAuth redirect). The
  // swap keeps the same blockId, so we reopen the detail view for it on return.
  // Cleared when the user backs out, so gallery-initiated swaps land on the
  // gallery, not a stale detail view.
  const [detailBlockId, setDetailBlockId] = useState<string | null>(() =>
    sessionStorage.getItem(DETAIL_BLOCK_STORAGE_KEY)
  );
  useEffect(() => {
    if (detailBlockId) sessionStorage.setItem(DETAIL_BLOCK_STORAGE_KEY, detailBlockId);
    else sessionStorage.removeItem(DETAIL_BLOCK_STORAGE_KEY);
  }, [detailBlockId]);
  // Preset load (Plugin) bumps this while we may be unmounted under the tuner
  // or tone browser; skip 0 so a restored detail after OAuth still opens.
  useEffect(() => {
    if (returnToGallery) setDetailBlockId(null);
  }, [returnToGallery]);
  // Set alongside detailBlockId only by the gallery tile's own EQ/Stereo
  // shortcuts (GalleryBlock's onOpenEq/onOpenStereo) - ChainBlock reads it
  // once, as its detail-view stack's *initial* seed, the moment it mounts
  // fresh for that block (it's conditionally rendered, so opening from the
  // gallery is always a fresh mount - see the render guard below). A plain
  // onOpen always resets this to 'main' first, so a stale shortcut from a
  // previous open can never leak into an unrelated later one. Not
  // persisted like detailBlockId above - surviving an OAuth-swap round
  // trip with a sub-view still showing isn't worth the complexity for
  // what's a one-shot navigation hint.
  const [initialDetailView, setInitialDetailView] = useState<DetailView>('main');
  /** The item under drag; drives the DragOverlay ghost. */
  const [activeDrag, setActiveDrag] = useState<ChainItem | null>(null);

  /** ⌥ held during the current drag; the drop duplicates instead of moving. */
  const altDragRef = useRef(false);

  /**
   * Optimistic mirror of the chain. Drag gestures mutate this immediately
   * (final order on drop) so nothing snaps back while the native mutation +
   * resync roundtrip completes; it resyncs from props whenever native
   * reports a new state and no drag is in flight.
   */
  const [items, setItems] = useState<ChainItem[]>(chain);
  const draggingRef = useRef(false);

  // Resync the optimistic items only when native actually reports new state
  // (and no drag is in flight). `items` must NOT be a dependency here: an
  // earlier version included it and unconditionally set a fresh array, which
  // re-triggered itself in a silent render loop.
  useEffect(() => {
    if (!draggingRef.current) setItems(chain);
  }, [chain]);

  const resetItems = () => setItems(chain);

  /**
   * Insert (or remove) the ⌥-duplicate stand-in: an inert copy of the
   * dragged block pinned at its home slot. The standard drag machinery
   * (traveling hole, parting neighbors, drop index) runs completely
   * untouched; with the home slot visibly occupied, the exact same gesture
   * reads as pulling a *copy* out instead of moving the block. Rebuilt from
   * native state so toggling ⌥ mid-drag also undoes any optimistic reflow.
   */
  const setDuplicateStandIn = (item: ChainItem | null) =>
    setItems(() => {
      const next = [...chain];
      if (item != null) {
        const index = next.findIndex((i) => i.blockId === item.blockId);
        if (index !== -1) next.splice(index, 0, { ...item, blockId: DUP_STAND_IN_ID });
      }
      return next;
    });

  // ⌥ tracking rides pointermove (drags move constantly, and the webview can
  // drop bare modifier keydowns, see KnobControl) with key events for
  // in-place toggles. Tone blocks only; inserts have nothing to duplicate.
  useEffect(() => {
    if (activeDrag == null || isInsertSlot(activeDrag)) return;
    if (altDragRef.current) setDuplicateStandIn(activeDrag); // ⌥ down at drag start
    const track = (e: PointerEvent | KeyboardEvent) => {
      if (e.altKey === altDragRef.current) return;
      altDragRef.current = e.altKey;
      setDuplicateStandIn(e.altKey ? activeDrag : null);
    };
    window.addEventListener('pointermove', track);
    window.addEventListener('keydown', track);
    window.addEventListener('keyup', track);
    return () => {
      window.removeEventListener('pointermove', track);
      window.removeEventListener('keydown', track);
      window.removeEventListener('keyup', track);
    };
    // setDuplicateStandIn closes over chain; that's stable for the life of a
    // drag (native doesn't push mid-gesture).
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [activeDrag]);

  const handleDragStart = (event: DragStartEvent, manager: DragDropManager) => {
    draggingRef.current = true;
    const id = String(event.operation.source?.id);
    setActiveDrag(items.find((i) => i.blockId === id) ?? null);
    // Seed from the press that started the drag; the tracker effect keeps it
    // live from here (and inserts the stand-in once activeDrag lands).
    const activator = manager.dragOperation.activatorEvent;
    altDragRef.current = activator instanceof PointerEvent && activator.altKey;
  };

  const handleDragEnd = (event: DragEndEvent) => {
    draggingRef.current = false;
    setActiveDrag(null);
    const duplicating = altDragRef.current;
    altDragRef.current = false;
    const { source, target } = event.operation;
    if (event.canceled || !target || !isSortable(source)) {
      resetItems();
      return;
    }

    const activeId = String(source.id);

    // source.index is the optimistic index the drag settled on (the
    // sortable plugin keeps it live during the gesture).
    let laneItems = items;
    const oldIndex = laneItems.findIndex((i) => i.blockId === activeId);
    const newIndex = Math.min(source.index, laneItems.length - 1);
    if (oldIndex !== -1 && oldIndex !== newIndex) {
      laneItems = arrayMove(laneItems, oldIndex, newIndex);
      setItems(laneItems);
    }
    const finalIndex = laneItems.findIndex((i) => i.blockId === activeId);

    // ⌥-drop: same layout, same index math; the mutation is a clone instead
    // of a move. The stand-in holds the home slot, so `finalIndex` already
    // counts the original staying put; the optimistic items match the
    // post-clone chain pixel-for-pixel until the resync swaps in real ids.
    if (duplicating && finalIndex !== -1 && !isInsertSlot(laneItems[finalIndex])) {
      actions.duplicateBlock(activeId, finalIndex);
      return;
    }

    // Commit to native: a reorder. The chainChanged resync converges the
    // optimistic state.
    const nativeIds = chain.map((i) => i.blockId);
    const localIds = laneItems.map((i) => i.blockId);
    if (nativeIds.join() !== localIds.join()) actions.reorderBlocks(localIds);
  };

  // Resolve the detail block; it can disappear underneath us (undo, trash
  // from the detail header, redo of a delete...), in which case we fall back
  // to the gallery.
  const detailBlock =
    detailBlockId != null
      ? ((chain.find(
          (item): item is ToneBlock => !isInsertSlot(item) && item.blockId === detailBlockId
        ) ?? null) as ToneBlock | null)
      : null;

  // Scroll-restore target queued for the next return to the gallery; consumed
  // synchronously by the effect below, so no render should ever observe it.
  const pendingScrollTargetRef = useRef<PendingScroll | null>(null);

  // The open detail block's last-known index, kept fresh on every render
  // it's still resolvable (a plain ref write during render — cheap, and
  // there's no cleaner hook for "remember the last real value before a
  // prop-driven change replaces it"). If the block then vanishes from
  // underneath us, there's nothing left to look it up by id, so the effect
  // below falls back to this instead.
  const lastDetailIndexRef = useRef<number | null>(null);
  // The most recent detailBlockId that has actually resolved to a real block
  // at least once. Distinguishes a genuine "vanished out from under us"
  // (trash/undo/redo — this id WAS showing, and now isn't) from "chain just
  // hasn't caught up yet" on this exact render — e.g. a ChainMapStrip "+" add
  // reopens the view via a brand-new ChainView mount (the tone browser fully
  // unmounted the old one), landing detailBlockId on the freshly created
  // block before its native chain-state sync has landed in props. Only the
  // former should clear detailBlockId and fall back to a scroll position;
  // the latter should just wait for the next render instead of discarding
  // the id and stranding the gallery scrolled to the far left.
  const confirmedDetailBlockIdRef = useRef<string | null>(null);
  if (detailBlock != null) {
    confirmedDetailBlockIdRef.current = detailBlockId;
    const index = items.findIndex((i) => i.blockId === detailBlock.blockId);
    if (index !== -1) lastDetailIndexRef.current = index;
  }

  // Center the just-closed (or just-vanished) block's tile, overriding
  // galleryScrollRef's raw sessionStorage restore above: recomputing the
  // tile's position (rather than replaying a raw offset) survives the chain
  // reshaping while the takeover was open (the block moved, or a preceding
  // block was deleted/inserted) — a raw offset alone (issue #82's fix)
  // would land on the wrong tile in that case.
  useLayoutEffect(() => {
    if (detailBlock != null) return;

    // detailBlockId only reaches null a step ahead of us, via onBack itself
    // (which seeds pendingScrollTargetRef in the same breath it clears this
    // state). Landing here with detailBlockId still set means either the
    // block genuinely disappeared out from under us (trash, undo, redo,
    // anything native-initiated - no explicit Back to rely on) or this id
    // has never resolved yet on THIS ChainView instance (a brand-new mount
    // whose chain prop hasn't caught up to a just-created block - see
    // confirmedDetailBlockIdRef). Only the former is a real vanish: treating
    // the latter the same way would discard a valid pending navigation and
    // strand the gallery scrolled to the far left the moment chain does
    // catch up (nothing left to restore, since lastDetailIndexRef never got
    // a chance to record this id either).
    if (detailBlockId != null) {
      if (confirmedDetailBlockIdRef.current !== detailBlockId) return;
      setDetailBlockId(null);
      if (lastDetailIndexRef.current != null) {
        pendingScrollTargetRef.current = { kind: 'index', index: lastDetailIndexRef.current };
      }
    }

    const target = pendingScrollTargetRef.current;
    const el = galleryScrollElRef.current;
    if (target == null || el == null) return;
    pendingScrollTargetRef.current = null;

    let index: number;
    if (target.kind === 'id') {
      index = items.findIndex((i) => i.blockId === target.blockId);
      if (index === -1) return;
    } else {
      // The vacated slot may now be past the end (e.g. the removed block was
      // the last real one, leaving only trailing insert slots).
      index = Math.min(target.index, items.length - 1);
      if (index < 0) return;
    }

    const centerDesignPx = EDGE_FADE_WIDTH + index * (TILE_SIZE + TILE_GAP) + TILE_SIZE / 2;
    const centerPx = centerDesignPx * getUiScale();
    const max = el.scrollWidth - el.clientWidth;
    el.scrollLeft = Math.max(0, Math.min(max, centerPx - el.clientWidth / 2));
  }, [detailBlock, detailBlockId, items]);

  // A brand-new mount (ChainMapStrip's "+" — see useToneLoadFlow's
  // navigateToDetail) can land here with detailBlockId already pointing at a
  // just-created block before this instance's chain prop has caught up to
  // it: chain state resyncs off the native `chainChanged` push event
  // (useChainState.ts), a separate round trip that lags a beat behind the
  // loadTone call that already resolved with the new id. The layout effect
  // above already knows not to discard detailBlockId or queue a scroll
  // target during that gap (confirmedDetailBlockIdRef), but that alone still
  // left this render falling through to the gallery return below - a real,
  // visible flash of the gallery at whatever a freshly mounted scroller
  // defaults to (the far left), then a snap into the detail view once chain
  // catches up. That flash *was* the "bounces back to the main screen,
  // scrolled left" regression, even though nothing was ever technically
  // "restored" to. Render nothing instead until it resolves.
  const awaitingDetailBlock =
    detailBlock == null &&
    detailBlockId != null &&
    confirmedDetailBlockIdRef.current !== detailBlockId;
  if (awaitingDetailBlock) {
    return null;
  }

  if (detailBlock) {
    // Another enabled+loaded NAM after this block in the chain. This mirrors
    // the DSP's lastNamIndex scan (Processor.cpp): with calibration on, such
    // a block hands off at calibrated output level instead of normalizing.
    const detailIndex = chain.findIndex((item) => item.blockId === detailBlock.blockId);

    const namDownstream = chain
      .slice(detailIndex + 1)
      .some(
        (item): item is ToneBlock =>
          !isInsertSlot(item) &&
          item.tone.format?.toLowerCase() === 'nam' &&
          item.loaded &&
          item.params.enabled
      );

    // Shared by the back arrow's onBack prop and the breadcrumb's "Gallery"
    // segment, so there's only one "return to the gallery" implementation
    // rather than two copies that could drift.
    const goToGallery = () => {
      pendingScrollTargetRef.current = { kind: 'id', blockId: detailBlock.blockId };
      setDetailBlockId(null);
    };

    return (
      <div
        style={{
          display: 'flex',
          flexDirection: 'column',
          alignItems: 'center',
          height: '100%',
          // Top-align under the shared 24px middle-band pad (Plugin); the
          // card bottom then sits 24px above the faceplate when the column
          // matches the meter height (Figma). Info view scrolls ← BLOCK + card.
          justifyContent: 'flex-start',
          boxSizing: 'border-box',
        }}
      >
        <ChainBlock
          block={detailBlock}
          namDownstream={namDownstream}
          sampleRate={sampleRate}
          namSlimSizeDefault={namSlimSizeDefault}
          initialView={initialDetailView}
          onBack={goToGallery}
          chainStripItems={chain}
          onJumpToBlock={setDetailBlockId}
          onAddBlockAt={(insertBlockId) =>
            actions.addModel(insertBlockId, { navigateToDetail: true })
          }
          onPasteBlockAt={canPaste ? (index) => actions.pasteBlock(index) : null}
          onFillToFaceplate={onFillToFaceplate}
        />
      </div>
    );
  }

  return (
    <div
      style={{
        display: 'flex',
        flexDirection: 'row',
        alignItems: 'stretch',
        height: '100%',
        boxSizing: 'border-box',
        padding: '0 24rem',
      }}
    >
      <DragDropProvider sensors={sensors} onDragStart={handleDragStart} onDragEnd={handleDragEnd}>
        <div style={{ position: 'relative', flex: 1, minWidth: 0, display: 'flex' }}>
          {/* Section title. Absolutely positioned so it sits in the top-left
              dead space without shifting the vertically/horizontally
              centered lane. left matches the lane's EDGE_FADE_WIDTH inset so
              the label lines up with the first tile; top is 0 because Plugin
              already applies the shared 24px middle-band pad. */}
          <span
            style={{
              position: 'absolute',
              top: 0,
              left: rem(EDGE_FADE_WIDTH),
              zIndex: 1,
              pointerEvents: 'none',
              fontFamily: FONT_MONO,
              fontSize: '16rem',
              fontWeight: 400,
              letterSpacing: 'normal',
              textTransform: 'uppercase',
              color: WHITE,
            }}
          >
            Signal Chain
          </span>
          <div
            ref={galleryScrollRef}
            className="hide-scrollbar"
            style={{
              flex: 1,
              minWidth: 0,
              overflowX: 'auto',
              overflowY: 'hidden',
              display: 'flex',
              flexDirection: 'column',
              justifyContent: 'center',
            }}
          >
            <div
              style={{
                position: 'relative',
                display: 'flex',
                flexDirection: 'column',
                width: 'max-content',
                minWidth: '100%',
                padding: `0 ${EDGE_FADE_WIDTH}rem`,
                boxSizing: 'border-box',
                // No transform on this wrapper: a transformed ancestor becomes
                // the containing block for position:fixed descendants, and
                // dnd-kit positions the dragged tile in fixed viewport
                // coordinates. In webviews without top-layer (popover)
                // promotion the tile would render offset by this box's origin,
                // a big down-right jump at pickup in DAW hosts.
              }}
            >
              <GalleryLane
                items={items}
                tileSize={TILE_SIZE}
                onOpen={(blockId) => {
                  setInitialDetailView('main');
                  setDetailBlockId(blockId);
                }}
                onOpenEq={(blockId) => {
                  setInitialDetailView('eq');
                  setDetailBlockId(blockId);
                }}
                onOpenStereo={(blockId) => {
                  setInitialDetailView('stereo');
                  setDetailBlockId(blockId);
                }}
                onAdd={(insertBlockId) => actions.addModel(insertBlockId)}
                onPasteBlock={canPaste ? (index) => actions.pasteBlock(index) : null}
              />
            </div>
          </div>
          <EdgeFade side="left" />
          <EdgeFade side="right" />
        </div>
      </DragDropProvider>
    </div>
  );
};
