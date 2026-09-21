import React, { useEffect, useRef, useState } from 'react';
import { DragDropProvider } from '@dnd-kit/react';
import { useSortable, isSortable } from '@dnd-kit/react/sortable';
import { KeyboardSensor, PointerActivationConstraints, PointerSensor } from '@dnd-kit/dom';
import type { DragEndEvent, Sensors } from '@dnd-kit/dom';
import { arrayMove } from '@dnd-kit/helpers';
import { ClipboardPaste, Plus } from './icons';
import { ChromeIconButton, ChromeTextButton } from './ChromeIconButton';
import { HELP, helpProps } from './helpText';
import { useHorizontalWheelScroll } from '../hooks/useHorizontalWheelScroll';
import { getUiScale } from '../hooks/useUiScale';
import { useChainActions } from '../hooks/useChainActions';
import { blockTypeMenuItems, useTileMenu } from './GalleryBlock';
import { TileMenu } from './TileMenu';
import { useToast } from './Toast';
import {
  BORDER,
  BRAND_ORANGE,
  BRAND_YELLOW,
  DISABLED_OPACITY,
  GRAY,
  HIGHLIGHT,
  MUTED,
} from './theme';
import type { ChainItem, ToneBlock } from '../types/chain';
import { BLOCK_TYPE_LABEL, isEqFlat, isInsertSlot } from '../types/chain';

/** Every chip (tone label or "+") is this exact box, regardless of label
    length, so the strip reads as a uniform row rather than ragged pill
    widths. Sized up from the shared TEXT_BOX_HEIGHT/ICON_BOX_SIZE default
    (20rem) so chips feel like a real navigation control, not a footnote. */
const CHIP_WIDTH = 44;
const CHIP_HEIGHT = 28;
const CHIP_FONT_SIZE = 13;

/** EQ shortcut mark (see eqModified below): a thin bar near a tone chip's
    bottom edge, absolutely positioned INSIDE the chip's existing fixed
    CHIP_WIDTH × CHIP_HEIGHT box rather than added as extra height below it -
    every chip (tone or "+") must stay that exact same size, and the strip's
    scroll row clips anything that hangs outside a chip's own box
    (overflowY: hidden). EQ_MARK_HIT_HEIGHT is the taller invisible click
    target around the thin visual bar; EQ_MARK_BOTTOM_INSET keeps both clear
    of the chip's own bottom border without sitting far from the edge. */
const EQ_MARK_WIDTH = 21;
const EQ_MARK_HEIGHT = 2;
const EQ_MARK_HIT_HEIGHT = 8;
const EQ_MARK_BOTTOM_INSET = 1;
/** Hover pill around the bar (see the .chain-map-eq-btn:hover rule below):
    lets the mark visibly light up on its own, independent of the chip's own
    hover state (which has none), so the boundary between "click here to jump
    to the block" and "click here to jump straight to its EQ" is felt, not
    just seen. Sized to the invisible hit area's full height so hovering
    anywhere in the click target lights up the same pill the click lands on. */
const EQ_MARK_PILL_PAD = 6;

/** Same derivation the block detail view's EQ button lights up yellow with
    (see ChainBlock.tsx's eqActive): EQ powered on and not flat (a flat or
    bypassed EQ is skipped natively, so neither is "modified" in effect). */
const eqModified = (item: ToneBlock): boolean =>
  item.params.eq.enabled && !isEqFlat(item.params.eq);

/** Dim a chip while it travels with the pointer, same treatment (and same
    value) as the gallery's own tiles (see GalleryBlock.DRAG_GHOST_OPACITY). */
const DRAG_GHOST_OPACITY = 0.75;

/** Every chip in a given strip instance sorts against every other one; only
    one ChainMapStrip is ever mounted at a time (one open block's detail
    view), so a fixed group key is enough - no per-instance id needed. */
const SORT_GROUP = 'chain-map-strip';

/** Distance-only drag activation, same feel as the gallery's own lanes (see
    ChainView's GALLERY_DRAG_DISTANCE_PX) - duplicated rather than imported
    from ChainView.tsx, which already imports ChainBlock.tsx which imports
    this file; importing back from ChainView would be circular. */
const STRIP_DRAG_DISTANCE_PX = 6;

const sensors: Sensors = [
  PointerSensor.configure({
    activationConstraints: () => [
      new PointerActivationConstraints.Distance({
        value: STRIP_DRAG_DISTANCE_PX * getUiScale(),
      }),
    ],
  }),
  KeyboardSensor,
];

interface ChainMapStripProps {
  /** The open block's whole lane, insert slots included, in chain order —
      the same ChainItem[] GalleryLane renders, so the strip's slot order
      matches the real chain 1:1 (gaps show as a "+" at their real position,
      not folded into one trailing add button). Lane-local like the
      Prev/Next chevrons this replaces (issue #83) — a branch only taps the
      other lane's signal, it never merges the two arrays, so this stays
      correct in stereo too, branched or not (see ChainView). */
  items: ChainItem[];
  currentBlockId: string;
  /** Jump straight into another block's detail view — the same mechanism
      the gallery's own tap-to-open already uses (ChainView's
      setDetailBlockId), just addressable from any block, not only the
      lane-adjacent one. */
  onSelect: (blockId: string) => void;
  /** Same destination as `onSelect`, plus opening the block's EQ view once
      there (see ChainBlock's showEq) — only the always-present EQ shortcut
      mark below a chip uses this; the rest of the chip keeps the plain
      `onSelect` jump. */
  onSelectEq: (blockId: string) => void;
  /** Add a block at this specific insert slot, via the existing add-tone
      flow (TONE3000 browser or local file) — the same
      actions.addModel(side, insertBlockId) targeting GalleryLane's own "+"
      tiles use. */
  onAdd: (insertBlockId: string) => void;
  /** Paste the copied block into this slot; null while there's nothing
      valid to paste (the menu shows Paste disabled) - same
      canPaste/actions.pasteBlock(side, index) gating GalleryLane's own "+"
      tiles use, threaded down from ChainView via ChainBlock. */
  onPasteBlockAt: ((index: number) => void) | null;
}

/** The "+" insert-slot chip: sortable like every other chip (dragging a tone
    chip past it reorders it like any other array member - it doesn't get
    "filled" or consumed, matching GalleryLane's own AddTile), and carries
    the identical right-click menu GalleryLane's own AddTile does (Paste /
    block-type rows, each flying out Load File / Load Folder - see
    blockTypeMenuItems) via the same shared useTileMenu hook, so the two
    "+" entry points behave identically rather than one lagging the other. */
const ChainMapAddChip: React.FC<{
  id: string;
  index: number;
  onAdd: (id: string) => void;
  onPasteBlockAt: ((index: number) => void) | null;
}> = ({ id, index, onAdd, onPasteBlockAt }) => {
  const { ref, isDragging } = useSortable({ id, index, group: SORT_GROUP });
  const actions = useChainActions();
  const toast = useToast();
  const { menuAnchor, openMenu, closeMenu, shouldIgnoreClick, longPressProps } = useTileMenu();

  return (
    <div
      ref={ref}
      style={{ position: 'relative', width: `${CHIP_WIDTH}rem`, height: `${CHIP_HEIGHT}rem` }}
      onContextMenu={openMenu}
      {...longPressProps}
    >
      <ChromeIconButton
        help={HELP.addTile}
        onClick={(e) => {
          if (shouldIgnoreClick(e)) return;
          onAdd(id);
        }}
        // Plain ChromeIconButton chrome has a transparent border (fine
        // floating beside a knob, but it read as a bare icon here, next to
        // chips that all carry a real idle border) - match the tone chips'
        // own idle frame so the "+" reads as one more uniform member of the
        // row, not a stray icon.
        style={{
          width: '100%',
          height: '100%',
          border: BORDER,
          opacity: isDragging ? DRAG_GHOST_OPACITY : 1,
        }}
      >
        <Plus />
      </ChromeIconButton>
      {menuAnchor && (
        <TileMenu
          anchor={menuAnchor}
          onClose={closeMenu}
          items={[
            {
              label: 'Paste',
              icon: <ClipboardPaste size={16} />,
              help: HELP.pasteBlock,
              disabled: onPasteBlockAt == null,
              onSelect: () => onPasteBlockAt?.(index),
            },
            ...blockTypeMenuItems(id, actions, toast),
          ]}
        />
      )}
    </div>
  );
};

/** A tone/IR/CAB chip: navigable (main body), sortable (drag anywhere on the
    main body - see the handle note below), and carries the always-present EQ
    shortcut mark.

    Sortable wiring: `ref` (the whole chip, main body + EQ mark together) is
    what travels with the pointer during a drag, but `handleRef` - not `ref`
    - goes on the main ChromeTextButton specifically. dnd-kit's own pointer
    sensor already refuses to start a drag from inside any OTHER real
    interactive element nested in the sortable's box (that's what already
    keeps the gallery's own Power/Swap/Trash buttons click-only, no extra
    code needed there) - `handle` is the same mechanism aimed the other way:
    it marks the main body as the one place a drag is ALLOWED to start,
    while the EQ mark (a real, separate <button>, neither the sortable
    element nor inside the handle) stays automatically excluded and click
    (or hover-pill) only. */
const ChainMapTile: React.FC<{
  item: ToneBlock;
  index: number;
  isCurrent: boolean;
  onSelect: (blockId: string) => void;
  onSelectEq: (blockId: string) => void;
}> = ({ item, index, isCurrent, onSelect, onSelectEq }) => {
  const { ref, handleRef, isDragging } = useSortable({
    id: item.blockId,
    index,
    group: SORT_GROUP,
  });
  const actions = useChainActions();
  const modified = eqModified(item);

  // Optimistic power state (same pattern as the detail card's own Power
  // button and the gallery tile's - see ChainBlock.tsx's handleToggleEnabled
  // and GalleryBlock.tsx's handleTogglePower): native converges via the
  // chainChanged resync, which setBlockParam itself doesn't wait for.
  const [enabled, setEnabled] = useState(item.params.enabled);
  useEffect(() => setEnabled(item.params.enabled), [item.params.enabled]);
  const handleToggleEnabled = () => {
    setEnabled((prev) => {
      actions.setBlockParam(item.blockId, 'enabled', !prev);
      return !prev;
    });
  };

  return (
    // Positioning context for the EQ mark below (absolutely placed inside
    // this same fixed CHIP_WIDTH × CHIP_HEIGHT box); also carries the
    // bypass dimming (so a bypassed block with a modified EQ still shows
    // both signals) and the drag-ghost dimming, combined.
    <div
      ref={ref}
      style={{
        position: 'relative',
        width: `${CHIP_WIDTH}rem`,
        height: `${CHIP_HEIGHT}rem`,
        opacity: (enabled ? 1 : DISABLED_OPACITY) * (isDragging ? DRAG_GHOST_OPACITY : 1),
      }}
    >
      <ChromeTextButton
        ref={handleRef}
        onClick={() => onSelect(item.blockId)}
        // Double-click toggles bypass, reusing the exact same
        // setBlockParam('enabled', ...) action the header's Power button
        // uses. A double-click is two real click events before the browser
        // recognizes a dblclick, so the single click's own onSelect fires
        // first (navigating here if this wasn't already the open block) -
        // deliberately not suppressed: swallowing that first click would
        // need a timer delaying every ordinary single click's navigation
        // (the far more common case) just to special-case a double-click,
        // and "jump to this block, then bypass it" reads as one coherent
        // action rather than a glitch. Distance-based drag activation can't
        // misfire from this either: each click of a double-click is its own
        // fresh near-zero-movement press/release, tracked independently by
        // the sensor (confirmed from its source, not assumed).
        onDoubleClick={handleToggleEnabled}
        help={`${item.tone.title} · ${BLOCK_TYPE_LABEL[item.blockType]}${
          enabled ? '' : ' · Bypassed'
        }${modified ? ' · EQ' : ''} · Double-click: bypass.`}
        open={isCurrent}
        style={{
          width: '100%',
          height: '100%',
          fontSize: `${CHIP_FONT_SIZE}rem`,
          // Active (non-bypassed), not the one currently open: a thin
          // BRAND_YELLOW outline on the plain idle chrome (no fill) -
          // enough to read as "on" at a glance without turning most of
          // the strip solid yellow (a full armed fill, tried first,
          // overwhelmed the row since most blocks in a chain are
          // active and only a couple are usually bypassed). The
          // currently-open chip already reads as unambiguously current
          // via its own white `open` fill, so it skips this accent.
          ...(enabled && !isCurrent ? { border: `1rem solid ${BRAND_YELLOW}` } : null),
        }}
      >
        {BLOCK_TYPE_LABEL[item.blockType]}
      </ChromeTextButton>
      {/* EQ shortcut: always present and always clickable (jumps in AND
          opens the EQ view directly, unlike the rest of the chip), so it
          reads as a standing affordance rather than something that only
          appears once you've already gone looking for it. Color is the
          secondary "has this been touched" signal: MUTED (flat/default,
          the same idle tone the chip's own idle text uses) vs BRAND_ORANGE
          (shaped away from flat) - never BRAND_YELLOW, so it can't be
          confused with the active/bypass accents above. Absolutely
          positioned within the chip's own box (not added height below it)
          and close to the bottom edge without sitting flush against the
          chip's own border - the scroll row clips anything that extends
          past the chip (overflowY: hidden). Not part of the sortable
          handle (see the component doc above), so it can never misfire as
          a drag-start. */}
      <button
        type="button"
        className="chain-map-eq-btn"
        onClick={() => onSelectEq(item.blockId)}
        {...helpProps(HELP.chainMapEqMark)}
        style={{
          position: 'absolute',
          left: 0,
          right: 0,
          bottom: `${EQ_MARK_BOTTOM_INSET}rem`,
          height: `${EQ_MARK_HIT_HEIGHT}rem`,
          display: 'flex',
          alignItems: 'center',
          justifyContent: 'center',
          padding: 0,
          border: 'none',
          background: 'transparent',
          cursor: 'pointer',
        }}
      >
        <span
          className="chain-map-eq-pill"
          style={{
            display: 'flex',
            alignItems: 'center',
            justifyContent: 'center',
            width: `${EQ_MARK_WIDTH + EQ_MARK_PILL_PAD * 2}rem`,
            height: `${EQ_MARK_HIT_HEIGHT}rem`,
            borderRadius: `${EQ_MARK_HIT_HEIGHT / 2}rem`,
          }}
        >
          <span
            aria-hidden
            style={{
              width: `${EQ_MARK_WIDTH}rem`,
              height: `${EQ_MARK_HEIGHT}rem`,
              borderRadius: `${EQ_MARK_HEIGHT / 2}rem`,
              // MUTED (near-white) is invisible on this one chip's own
              // WHITE `open` fill - GRAY instead, still reading as the
              // same idle/unmodified signal everywhere else.
              backgroundColor: modified ? BRAND_ORANGE : isCurrent ? GRAY : MUTED,
            }}
          />
        </span>
      </button>
    </div>
  );
};

/**
 * Persistent chain-map strip in the block detail view's header row (replaces
 * the old Prev/Next chevrons, issue #83): the open block's whole lane
 * rendered left to right in chain order — a compact NAM/IR/CAB chip for
 * every tone block, a "+" for every insert slot at its real position — with
 * the open block highlighted and bypassed blocks visually flat. Hovering a
 * chip surfaces the tone's name (and bypass state) via the shared faceplate
 * help readout (see helpText.ts); clicking one jumps straight to that
 * block, dragging one reorders it within this lane (or for a "+", adds a
 * new block right there).
 *
 * Drag-reorder mirrors the chain gallery's own (see GalleryLane/ChainView):
 * same dnd-kit sortable mechanism, same distance-only activation, same
 * dragged-tile dimming, same same-lane-only scope (there's only ever one
 * lane here) - adapted into its own DragDropProvider, since the gallery's
 * own provider only wraps the plain-gallery view and unmounts entirely while
 * a block's detail view (and this strip) is open. Same-lane reflow while
 * dragging is dnd-kit's built-in behavior (OptimisticSortingPlugin), not
 * anything this component drives itself - only the settled result at drop
 * needs handling here (see handleDragEnd). Unlike the gallery, there's no
 * ⌥-duplicate-while-dragging here (not asked for, kept out of scope) and no
 * touchAction:'none' on the chips (the gallery's tiles are big with roomy
 * gaps to swipe-scroll the lane from; these chips are small and packed edge
 * to edge, so a touch drag may be less reliable here in exchange for the
 * strip staying swipe-scrollable - a deliberate, accepted trade-off).
 *
 * Every chip is the same fixed size (uniform width/height regardless of
 * label) and the whole row centers itself within the header when it fits.
 * The centering is the classic flex "shrink-to-content, cap at the parent"
 * trick rather than a measured/JS toggle, so it degrades cleanly on old
 * WebKit too (see vite.config's build target note): the outer flex box
 * always centers; the inner scroller sizes to its content up to
 * `max-width: 100%` of that outer box, so once the chips overflow, the
 * inner fills the full available width (centering a full-width box is a
 * no-op) and scrolls internally from the left, exactly like the old
 * left-aligned behavior — no measurement, no resize listener.
 */
export const ChainMapStrip: React.FC<ChainMapStripProps> = ({
  items,
  currentBlockId,
  onSelect,
  onSelectEq,
  onAdd,
  onPasteBlockAt,
}) => {
  const wheelScrollRef = useHorizontalWheelScroll<HTMLDivElement>();
  const actions = useChainActions();

  // Optimistic local order, mirroring ChainView's own `lanes`: only resynced
  // from the native-derived `items` prop while no drag of ours is in
  // flight, so a mid-drag chain-state push (e.g. another block's meter/param
  // update) can't yank a chip out from under the pointer.
  const [localItems, setLocalItems] = useState(items);
  const draggingRef = useRef(false);
  useEffect(() => {
    if (!draggingRef.current) setLocalItems(items);
  }, [items]);

  const handleDragStart = () => {
    draggingRef.current = true;
  };

  const handleDragEnd = (event: DragEndEvent) => {
    draggingRef.current = false;
    const { source, target } = event.operation;
    if (event.canceled || !target || !isSortable(source)) {
      setLocalItems(items);
      return;
    }
    const activeId = String(source.id);
    const oldIndex = localItems.findIndex((i) => i.blockId === activeId);
    if (oldIndex === -1) return;
    // The vacated slot may now be past the end.
    const newIndex = Math.min(source.index, localItems.length - 1);
    if (oldIndex === newIndex) return;
    const reordered = arrayMove(localItems, oldIndex, newIndex);
    setLocalItems(reordered);
    actions.reorderBlocks(reordered.map((i) => i.blockId));
  };

  return (
    <div
      style={{
        display: 'flex',
        justifyContent: 'center',
        flex: 1,
        minWidth: 0,
      }}
    >
      <div
        ref={wheelScrollRef}
        className="hide-scrollbar"
        style={{
          display: 'flex',
          alignItems: 'center',
          gap: '6rem',
          maxWidth: '100%',
          overflowX: 'auto',
          overflowY: 'hidden',
        }}
      >
        {/* See EQ_MARK_PILL_PAD: a real CSS :hover rule, not the inline
            onMouseEnter/onMouseLeave mutation used elsewhere in the app for
            this same purpose, because the pill's resting background has to
            stay unset (transparent) for BOTH color states below it to show
            through - an inline style here would out-rank any hover rule.
            One rule for every chip's mark, same idiom as TileMenu's own
            hover row. */}
        <style>{`.chain-map-eq-btn:hover .chain-map-eq-pill { background-color: ${HIGHLIGHT}; }`}</style>
        <DragDropProvider sensors={sensors} onDragStart={handleDragStart} onDragEnd={handleDragEnd}>
          {localItems.map((item, index) =>
            isInsertSlot(item) ? (
              <ChainMapAddChip
                key={item.blockId}
                id={item.blockId}
                index={index}
                onAdd={onAdd}
                onPasteBlockAt={onPasteBlockAt}
              />
            ) : (
              <ChainMapTile
                key={item.blockId}
                item={item}
                index={index}
                isCurrent={item.blockId === currentBlockId}
                onSelect={onSelect}
                onSelectEq={onSelectEq}
              />
            )
          )}
        </DragDropProvider>
      </div>
    </div>
  );
};
