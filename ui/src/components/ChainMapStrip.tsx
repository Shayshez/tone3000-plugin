import React, { useEffect, useRef, useState } from 'react';
import { DragDropProvider } from '@dnd-kit/react';
import { useSortable, isSortable } from '@dnd-kit/react/sortable';
import { KeyboardSensor, PointerActivationConstraints, PointerSensor } from '@dnd-kit/dom';
import type { DragEndEvent, Sensors } from '@dnd-kit/dom';
import { arrayMove } from '@dnd-kit/helpers';
import { ClipboardPaste, Home, Plus } from './icons';
import { ChromeIconButton, ChromeTextButton } from './ChromeIconButton';
import { HELP, helpProps } from './helpText';
import { useHorizontalWheelScroll } from '../hooks/useHorizontalWheelScroll';
import { getUiScale } from '../hooks/useUiScale';
import { useChainActions } from '../hooks/useChainActions';
import { blockTypeMenuItems, useTileMenu } from './GalleryBlock';
import { TileMenu } from './TileMenu';
import { useToast } from './Toast';
import {
  BLACK,
  BORDER,
  BRAND_ORANGE,
  BRAND_YELLOW,
  DISABLED_OPACITY,
  GRAY,
  HIGHLIGHT,
  MUTED,
  WHITE,
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
      actions.addModel(insertBlockId) targeting GalleryLane's own "+"
      tiles use. */
  onAdd: (insertBlockId: string) => void;
  /** Paste the copied block into this slot; null while there's nothing
      valid to paste (the menu shows Paste disabled) - same
      canPaste/actions.pasteBlock(index) gating GalleryLane's own "+"
      tiles use, threaded down from ChainView via ChainBlock. */
  onPasteBlockAt: ((index: number) => void) | null;
  /** The strip's own persistent Home chip: jumps straight to the gallery
      from anywhere, regardless of navigation depth - unlike a chip's own
      jump (which only ever lands on that block), this always leaves the
      whole detail view. */
  onGoHome: () => void;
  /** L/R child tabs that expand the currently-open chip inline (see
      ChainMapChildTab) - omitted/undefined for a block with no children of
      its own. Today only a Dual Mono block populates this. */
  currentChildTabs?: ChainMapChildTab[];
}

/** One tab in a Dual Mono chip's inline-expanded state (see
    currentChildTabs) - a mini version of the ordinary chip + EQ-mark
    pattern above, reused rather than a separate "L·EQ" label so this stays
    as compact as the rest of the strip. A side (L/R) carries `eq`; a plain
    target with no shortcut of its own (Stereo Processing) omits it. */
export interface ChainMapChildTab {
  /** Also this tab's accessible name (aria-label/help) when `icon` replaces
      it visually. */
  label: string;
  /** Rendered in place of `label`'s text when present - the Stereo tab uses
      this for the same two-overlapping-rings glyph the gallery tile's own
      Stereo shortcut and TileChannelBadge use (GalleryBlock's StereoGlyph),
      since "Stereo" spelled out read as heavier than a single-letter L/R
      side by side. */
  icon?: React.ReactNode;
  /** This tab's own view is currently showing. */
  active: boolean;
  onSelect: () => void;
  eq?: {
    /** This side's EQ view is currently showing - colors the mark GRAY
        instead of the idle dark tint, same convention as an ordinary
        chip's own mark colors it GRAY instead of MUTED. */
    active: boolean;
    /** This side's EQ is on and not flat - colors the mark BRAND_ORANGE,
        same convention as eqModified above. */
    modified: boolean;
    onSelect: () => void;
  };
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

/** Persistent, always-first chip: jumps straight to the gallery from
    anywhere, any depth - not sortable (it isn't a chain member), not part
    of `localItems`. Doubles as the strip's own "home" - see
    ChainBlockHeaderNav's onGoHome. */
const ChainMapHomeChip: React.FC<{ onGoHome: () => void }> = ({ onGoHome }) => (
  <div style={{ width: `${CHIP_WIDTH}rem`, height: `${CHIP_HEIGHT}rem`, flexShrink: 0 }}>
    <ChromeIconButton
      help={HELP.chainMapHome}
      onClick={onGoHome}
      style={{ width: '100%', height: '100%' }}
    >
      <Home />
    </ChromeIconButton>
  </div>
);

/** The row of L/R tabs appended inline when the current chip expands (see
    ChainMapChildTab and ChainMapTile's `expanded`) - grows the chip into a
    wider pill rather than floating a separate panel below it, so there's
    exactly one shape to look at, not two. Each tab is a mini version of an
    ordinary chip: a label (jumps to that side's plain view) plus the exact
    same gray/orange EQ-mark underline every top-level chip already carries
    (jumps straight to that side's EQ) - reusing that established mark
    instead of a second labeled button per side keeps this compact. */
const ChainMapChildTabRow: React.FC<{ tabs: ChainMapChildTab[] }> = ({ tabs }) => (
  <div style={{ display: 'flex', alignItems: 'stretch', gap: '2rem', padding: '0 4rem 0 2rem' }}>
    {tabs.map((tab) => (
      <div
        key={tab.label}
        style={{ position: 'relative', width: `${EQ_MARK_WIDTH + EQ_MARK_PILL_PAD * 2}rem` }}
      >
        <button
          type="button"
          onClick={tab.onSelect}
          aria-label={tab.icon ? tab.label : undefined}
          style={{
            all: 'unset',
            boxSizing: 'border-box',
            cursor: 'pointer',
            width: '100%',
            height: '100%',
            display: 'flex',
            alignItems: 'center',
            justifyContent: 'center',
            borderRadius: '5rem',
            fontFamily: 'inherit',
            // Deliberately smaller/lighter than the chip's own "DUAL" label
            // (CHIP_FONT_SIZE, weight 600) - these are secondary
            // sub-navigation, not the block's name, and reading as equally
            // heavy made the expanded pill feel bulkier than it needed to.
            fontSize: `${CHIP_FONT_SIZE - 2}rem`,
            fontWeight: 500,
            color: tab.active ? BLACK : 'rgba(0, 0, 0, 0.6)',
            backgroundColor: tab.active ? 'rgba(0, 0, 0, 0.08)' : 'transparent',
          }}
        >
          {tab.icon ?? tab.label}
        </button>
        {tab.eq && (
          <button
            type="button"
            className="chain-map-eq-btn"
            onClick={tab.eq.onSelect}
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
                  // This tab always sits on the expanded pill's own WHITE
                  // background (unlike an ordinary chip, which is only white
                  // when it's the current one) - MUTED (near-white) would be
                  // invisible here even at rest, so the idle state uses a
                  // dark tint instead.
                  backgroundColor: tab.eq.modified
                    ? BRAND_ORANGE
                    : tab.eq.active
                      ? GRAY
                      : 'rgba(0, 0, 0, 0.3)',
                }}
              />
            </span>
          </button>
        )}
      </div>
    ))}
  </div>
);

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
  /** Only ever passed (and only ever non-empty) for the current chip - see
      ChainMapChildTab. */
  childTabs?: ChainMapChildTab[];
}> = ({ item, index, isCurrent, onSelect, onSelectEq, childTabs }) => {
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

  // Only the current chip ever expands, and only when it actually has
  // child tabs to offer (see ChainMapChildTab) - a Dual Mono block's L/R
  // today. Grows into a wider pill inline (pushing later chips right)
  // rather than a floating panel: exactly one shape to read, and the
  // strip's own horizontal scroll already exists to absorb the extra
  // width instead of needing new layout machinery for it.
  const expanded = isCurrent && !!childTabs && childTabs.length > 0;

  return (
    // Carries the bypass dimming (so a bypassed block with a modified EQ
    // still shows both signals) and the drag-ghost dimming, combined; when
    // expanded, also the shared white pill background/radius the two
    // segments (chip + tabs) sit inside - overflow:hidden so neither
    // segment's own corners peek out past the pill's rounded ones.
    <div
      ref={ref}
      style={{
        position: 'relative',
        display: expanded ? 'flex' : undefined,
        alignItems: expanded ? 'stretch' : undefined,
        width: expanded ? undefined : `${CHIP_WIDTH}rem`,
        height: `${CHIP_HEIGHT}rem`,
        flexShrink: 0,
        borderRadius: expanded ? '6rem' : undefined,
        overflow: expanded ? 'hidden' : undefined,
        backgroundColor: expanded ? WHITE : undefined,
        opacity: (enabled ? 1 : DISABLED_OPACITY) * (isDragging ? DRAG_GHOST_OPACITY : 1),
      }}
    >
      {/* Positioning context for the EQ mark below (absolutely placed
          inside this same fixed CHIP_WIDTH × CHIP_HEIGHT box) - stays this
          exact size even when the outer box above grows for the tabs
          beside it. */}
      <div
        style={{ position: 'relative', width: `${CHIP_WIDTH}rem`, height: '100%', flexShrink: 0 }}
      >
        {expanded ? (
          <button
            ref={handleRef}
            type="button"
            onClick={(e) => (e.altKey ? handleToggleEnabled() : onSelect(item.blockId))}
            {...helpProps(
              `${item.tone.title} · ${BLOCK_TYPE_LABEL[item.blockType]}${
                enabled ? '' : ' · Bypassed'
              }${modified ? ' · EQ' : ''} · ⌥-click: bypass.`
            )}
            style={{
              all: 'unset',
              boxSizing: 'border-box',
              cursor: 'pointer',
              width: '100%',
              height: '100%',
              display: 'flex',
              alignItems: 'center',
              justifyContent: 'center',
              fontFamily: 'inherit',
              fontSize: `${CHIP_FONT_SIZE}rem`,
              fontWeight: 600,
              color: BLACK,
            }}
          >
            {BLOCK_TYPE_LABEL[item.blockType]}
          </button>
        ) : (
          <ChromeTextButton
            ref={handleRef}
            onClick={(e) => (e.altKey ? handleToggleEnabled() : onSelect(item.blockId))}
            // Alt/Option-click toggles bypass, reusing the exact same
            // setBlockParam('enabled', ...) action the header's Power button
            // uses - same convention as every other secondary action in this
            // app (KnobControl/BlockEqView/IrEnvelopeGraph's own Alt-click
            // resets). Used to be a double-click, but the strip's own
            // navigation made that unreliable: clicking any OTHER chip first
            // (the double-click's first of two real clicks) could make the
            // currently-expanded chip collapse (see ChainMapTile's
            // `expanded`), shifting every chip after it in the row before
            // the second click landed - so a double-click meant to bypass
            // one block could navigate to a different one instead. A single
            // click with a held modifier has no second click to land
            // anywhere, so it can't be disrupted by that shift at all.
            help={`${item.tone.title} · ${BLOCK_TYPE_LABEL[item.blockType]}${
              enabled ? '' : ' · Bypassed'
            }${modified ? ' · EQ' : ''} · ⌥-click: bypass.`}
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
        )}
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
      {expanded && <ChainMapChildTabRow tabs={childTabs} />}
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
 * label), and the whole row is left-aligned rather than centered - it was
 * centered once (the classic flex "shrink-to-content, cap at the parent"
 * trick), but that meant ANY change to the row's total content width
 * silently shifted every chip's on-screen position, including ones well to
 * the left of whatever changed. The currently-open chip's inline expand/
 * collapse (see ChainMapTile's `expanded`, driven by isCurrent) changes
 * that total width on every navigation in or out of a block with childTabs
 * (today, only Dual Mono) - centered, that recentered the *entire* row
 * under the pointer the instant you clicked a different chip, which is
 * exactly what made a double-click bypass gesture on some OTHER chip
 * unreliable (see the bypass Alt-click note below - that's since moved off
 * double-click for this same reason, but left-aligning the row is still the
 * right fix for the underlying instability, not just a workaround for the
 * gesture that exposed it). Left-aligned, collapsing/expanding a chip only
 * shifts chips *after* it in the row, not the ones before it or the Home
 * chip. The inner scroller still caps at `max-width: 100%` of the outer box
 * and scrolls internally past that, unchanged.
 */
export const ChainMapStrip: React.FC<ChainMapStripProps> = ({
  items,
  currentBlockId,
  onSelect,
  onSelectEq,
  onAdd,
  onPasteBlockAt,
  onGoHome,
  currentChildTabs,
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
        justifyContent: 'flex-start',
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
        <ChainMapHomeChip onGoHome={onGoHome} />
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
                childTabs={item.blockId === currentBlockId ? currentChildTabs : undefined}
              />
            )
          )}
        </DragDropProvider>
      </div>
    </div>
  );
};
