import React, { useCallback, useEffect, useRef, useState } from 'react';
import { useSortable } from '@dnd-kit/react/sortable';
import {
  ArrowLeftRight,
  ClipboardPaste,
  Copy,
  File,
  FolderClosed,
  PlusCircle,
  Power,
  Trash2,
} from './icons';
import { BlockEnergyBorder, BlockLed } from './BlockLed';
import { GearIcon, ToneImage } from './GearIcon';
import { LoadingDots } from './LoadingDots';
import { RetryLoadBadge } from './RetryLoadBadge';
import { meterId } from '../hooks/useMeters';
import { useChainActions } from '../hooks/useChainActions';
import { HELP, helpProps, toneTileHelp } from './helpText';
import type { ChainSide, ChainItem, ToneBlock } from '../types/chain';
import { isEqFlat, isInsertSlot } from '../types/chain';
import { ChromeIconButton } from './ChromeIconButton';
import { TileMenu } from './TileMenu';
import type { TileMenuAnchor, TileMenuItem } from './TileMenu';
import type { ChainActions } from '../hooks/useChainActions';
import { useToast } from './Toast';
import { FONT_MONO, HIGHLIGHT, ICON_SIZE, SURFACE, SURFACE_RAISED, WHITE } from './theme';
import { IS_IOS } from '../hooks/useUiScale';

/**
 * Gallery view of a chain block: a square tone image with quick actions
 * (power / swap / trash) overlaid along the top edge and a simplified
 * horizontal output level + clip strip along the bottom. Tap/click opens
 * the detail card; dragging the tile reorders it.
 *
 * While dragging, the tile itself travels with the pointer (dnd-kit's
 * Feedback plugin lifts it out of the lane and leaves a hidden placeholder
 * holding its slot, which reveals the ghost rail behind it), dimmed like the
 * old chain's dragged card.
 */

/** Opacity of the tile while it travels with the pointer. */
const DRAG_GHOST_OPACITY = 0.75;

/** File-drag drop-target chrome (tone tiles + add tile). */
const FILE_DROP_BORDER = '2rem dashed rgba(0, 209, 59, 0.50)';
const ADD_TILE_BORDER_WIDTH = 2;
const ADD_TILE_BORDER = `${ADD_TILE_BORDER_WIDTH}rem dashed rgba(141, 141, 147, 0.65)`;

const isFileDrag = (e: React.DragEvent) => e.dataTransfer.types.includes('Files');

// The global drop swallow (main.tsx) only stops the webview navigating away;
// accepting a drop also needs every dragover cancelled with the file-copy
// effect, or the OS shows a rejection cursor.
const armFileDrag = (e: React.DragEvent, setArmed: (v: boolean) => void) => {
  if (!isFileDrag(e)) return;
  e.preventDefault();
  e.dataTransfer.dropEffect = 'copy';
  setArmed(true);
};

const disarmFileDrag = (e: React.DragEvent, setArmed: (v: boolean) => void) => {
  // Crossing into a child still fires dragLeave on the parent; ignore those
  // or the upload icon / border flicker as the pointer moves across the tile.
  if (e.currentTarget.contains(e.relatedTarget as Node)) return;
  setArmed(false);
};

/** Keep tile buttons from taking focus on press: the webview scrolls the
    focused element into view, which nudges the whole lane by a pixel. */
const preventFocus = (e: React.MouseEvent) => e.preventDefault();

/** Right-click → tile-local anchor for the tile's action sheet (suppresses
    the OS context menu; macOS ctrl-click lands here too). Ctrl-click also
    fires a synthetic `click` after `contextmenu`; `shouldIgnoreClick`
    swallows that so the tile doesn't navigate away under the menu. */

/** How long a touch is held before the tile menu opens: the system's own
    long-press delay (same as useTouchHold). */
const LONG_PRESS_MS = 500;

/** Real-px drift that abandons the long press. Kept under the drag sensor's
    activation distance, so a press that starts travelling becomes a drag and
    never also a menu. */
const LONG_PRESS_SLOP_PX = 5;

/** Real px the menu drops below the touch point. The menu opens while the
    finger is still down, so the release, and the mouse pair WebKit replays
    at it, must land outside the menu or letting go would pick whatever row
    sat under the finger. Dropping the sheet also keeps it readable past the
    fingertip. */
const LONG_PRESS_MENU_DROP_PX = 24;

/** How long a set suppression stays valid. A ctrl-click's synthetic click
    follows within the same task, but the mouse pair WebKit replays after a
    touch can land much later or not at all, and an unconsumed flag must
    expire rather than swallow the next honest tap. Matches the replay
    window in helpText.ts. */
const SUPPRESS_CLICK_MS = 700;

/** Exported so ChainMapStrip's own "+" chip (a block's chain-map strip, not
    the gallery) can carry the identical right-click menu/long-press
    behavior instead of duplicating this iOS-long-press machinery. */
export const useTileMenu = () => {
  const [menuAnchor, setMenuAnchor] = useState<TileMenuAnchor | null>(null);
  // Deadline (performance.now ms) under which the next click is swallowed.
  const suppressClickUntilRef = useRef(0);
  const openMenu = useCallback((e: React.MouseEvent) => {
    e.preventDefault();
    e.stopPropagation();
    suppressClickUntilRef.current = performance.now() + SUPPRESS_CLICK_MS;
    // Viewport coords: TileMenu portals to body and positions with
    // position:fixed at these real-px coordinates.
    setMenuAnchor({ clientX: e.clientX, clientY: e.clientY });
  }, []);
  const closeMenu = useCallback(() => setMenuAnchor(null), []);

  // iOS only: WKWebView never delivers `contextmenu` for a long press, so a
  // touch hold has to open the sheet itself. Every other engine fires the
  // native event and lands in openMenu above.
  //
  // The menu fires on its own timer, like the system's, while the finger is
  // still down. Travel past the slop cancels it: past there the press is a
  // drag (the sensor's distance activation), never also a menu; if the
  // finger drags on after the menu already opened, the menu yields and the
  // drag proceeds. The release is watched on window in the capture phase,
  // because a drag that did start takes pointer capture and no pointerup
  // reaches the tile at all.
  const pressStart = useRef<{ x: number; y: number; id: number; fired: boolean } | null>(null);
  const holdTimer = useRef<number | undefined>(undefined);
  const releaseListener = useRef<((e: PointerEvent) => void) | null>(null);

  const cancelLongPress = useCallback(() => {
    pressStart.current = null;
    if (holdTimer.current !== undefined) window.clearTimeout(holdTimer.current);
    holdTimer.current = undefined;
    if (releaseListener.current) {
      window.removeEventListener('pointerup', releaseListener.current, true);
      window.removeEventListener('pointercancel', releaseListener.current, true);
      releaseListener.current = null;
    }
  }, []);

  // The release listener lives on window; a tile can unmount mid-press (undo,
  // a preset load), so drop it on unmount.
  useEffect(() => cancelLongPress, [cancelLongPress]);

  const longPressProps = {
    onPointerDown: (e: React.PointerEvent) => {
      if (!IS_IOS || e.pointerType !== 'touch') return;
      cancelLongPress();
      const start = { x: e.clientX, y: e.clientY, id: e.pointerId, fired: false };
      pressStart.current = start;

      holdTimer.current = window.setTimeout(() => {
        holdTimer.current = undefined;
        if (pressStart.current !== start) return;
        start.fired = true;
        // The release that follows can fire a click on the tile; swallow it
        // exactly as the ctrl-click path does.
        suppressClickUntilRef.current = performance.now() + SUPPRESS_CLICK_MS;
        setMenuAnchor({ clientX: start.x, clientY: start.y + LONG_PRESS_MENU_DROP_PX });
      }, LONG_PRESS_MS);

      const onRelease = (ev: PointerEvent) => {
        if (pressStart.current === start && ev.pointerId === start.id) cancelLongPress();
      };
      releaseListener.current = onRelease;
      window.addEventListener('pointerup', onRelease, true);
      window.addEventListener('pointercancel', onRelease, true);
    },
    onPointerMove: (e: React.PointerEvent) => {
      const start = pressStart.current;
      if (start == null || e.pointerId !== start.id) return;
      if (
        Math.abs(e.clientX - start.x) <= LONG_PRESS_SLOP_PX &&
        Math.abs(e.clientY - start.y) <= LONG_PRESS_SLOP_PX
      )
        return;
      if (start.fired) closeMenu();
      cancelLongPress();
    },
  };
  /** True when a tile click should be ignored (followed a contextmenu or a
      touch hold, is a modifier-click, or the menu is already open, in which
      case it closes). */
  const shouldIgnoreClick = useCallback(
    (e: React.MouseEvent) => {
      if (performance.now() < suppressClickUntilRef.current) {
        suppressClickUntilRef.current = 0;
        return true;
      }
      if (e.ctrlKey || e.metaKey) return true;
      if (menuAnchor) {
        closeMenu();
        return true;
      }
      return false;
    },
    [menuAnchor, closeMenu]
  );
  return { menuAnchor, openMenu, closeMenu, shouldIgnoreClick, longPressProps };
};

/** The Load File / Load Folder rows inside a block-type row's flyout (see
    blockTypeMenuItems below). Local loading must not depend on drag-and-drop
    alone: Linux never delivers OS file drags to the embedded webview, so
    these rows are the only way local files get in there. An insert slot
    adds; a tone tile swaps in place (same targeting as a drop). `category`
    forces the picked file's IR/Cab reading the same way the split drop
    zone's own IR/Cab halves do (see SplitFileDropZone) - inert if the pick
    turns out to be a .nam file. */
const localLoadMenuItems = (
  category: 'ir' | 'cab',
  targetBlockId: string,
  actions: ChainActions,
  toast: ReturnType<typeof useToast>
): TileMenuItem[] => {
  const pick = async (kind: 'file' | 'folder') => {
    const error = await actions.pickLocalFile(targetBlockId, kind, category);
    if (error) toast.show(error);
  };
  return [
    {
      label: 'Load File',
      icon: <File size={16} />,
      help: HELP.loadFileTile,
      onSelect: () => void pick('file'),
    },
    {
      label: 'Load Folder',
      icon: <FolderClosed size={16} />,
      help: HELP.loadFolderTile,
      onSelect: () => void pick('folder'),
    },
  ];
};

/** Top-level block-type rows for the tile menus' local-load section: one per
    loadable ChainBlockType today (Cab, IR), each flying out its own Load
    File / Load Folder choice on hover (see TileMenu's submenu support) -
    the more block types this menu grows (EQ, Utility, ...), the more rows
    join this same list, each with whatever sub-choices it needs. Mirrors
    the split drop zone's IR/Cab halves (SplitFileDropZone) as the tile
    menus' own equivalent of that split. Exported for ChainMapStrip's own
    "+" chip - same rows, same targeting, just a different chip shell. */
export const blockTypeMenuItems = (
  targetBlockId: string,
  actions: ChainActions,
  toast: ReturnType<typeof useToast>
): TileMenuItem[] => [
  {
    label: 'Cab',
    icon: <GearIcon gear="cab" size={16} color="currentColor" />,
    help: HELP.addCabTile,
    submenu: localLoadMenuItems('cab', targetBlockId, actions, toast),
  },
  {
    label: 'IR',
    icon: <GearIcon gear="ir" size={16} color="currentColor" />,
    help: HELP.addIrTile,
    submenu: localLoadMenuItems('ir', targetBlockId, actions, toast),
  },
  {
    label: 'EQ',
    icon: <GearIcon gear="eq" size={16} color="currentColor" />,
    help: HELP.addEqTile,
    // No submenu: unlike Cab/IR there is no file to pick, so this commits
    // immediately - the only leaf row directly on the root menu rather than
    // behind a flyout.
    onSelect: () => actions.addEqBlock(targetBlockId),
  },
  {
    label: 'Dual Mono',
    icon: <GearIcon gear="dualMono" size={16} color="currentColor" />,
    help: HELP.addDualMonoTile,
    // Same leaf-row shape as EQ: no submenu, since content for each of the
    // block's two fixed slots is picked afterward from its own detail view
    // (Select Tone flow), not at add time.
    onSelect: () => actions.addDualMonoBlock(targetBlockId),
  },
];

/** Interactive wiring for a tile's chrome. */
interface TileActions {
  onOpen: (e: React.MouseEvent) => void;
  onTogglePower: (e: React.MouseEvent) => void;
  /** Jump straight to this block's detail view with its EQ panel already
      open - same destination as ChainMapStrip's own EQ shortcut mark, just
      reachable from the gallery instead of from inside an already-open
      block. */
  onOpenEq: (e: React.MouseEvent) => void;
  onSwap: (e: React.MouseEvent) => void;
  onRemove: (e: React.MouseEvent) => void;
  /** Retry a failed model download (shown when block.loadFailed). */
  onRetryLoad: () => void;
}

/** The two-half IR/Cab drop target shown while a file drag is armed -
    shared by AddTile (an empty slot) and GalleryBlock's own occupied-tile
    drop (see TileSurface) so both entry points route a dropped local .wav
    the same way: IR always lands a plain IR block (content-duration guess
    decides Cab/IrPlayer within it, same as before this existed); Cab forces
    a real ChainBlockType::CAB block regardless of the file's own length
    (see loadLocalTone's forceGear param) - no more "drop, then notice it
    guessed wrong, then convert" two-step, on an empty slot or an existing
    tile alike. */
const SplitFileDropZone: React.FC<{
  hoverHalf: 'ir' | 'cab' | null;
  onArmHalf: (category: 'ir' | 'cab') => (e: React.DragEvent) => void;
  onDropOnHalf: (category: 'ir' | 'cab') => (e: React.DragEvent) => void;
}> = ({ hoverHalf, onArmHalf, onDropOnHalf }) => {
  const half = (category: 'ir' | 'cab', label: string) => (
    <div
      onDragOver={onArmHalf(category)}
      onDrop={onDropOnHalf(category)}
      style={{
        flex: 1,
        display: 'flex',
        alignItems: 'center',
        justifyContent: 'center',
        backgroundColor: hoverHalf === category ? HIGHLIGHT : 'transparent',
      }}
    >
      <span
        style={{
          fontFamily: FONT_MONO,
          fontSize: '13rem',
          fontWeight: 400,
          color: WHITE,
          textTransform: 'uppercase',
          pointerEvents: 'none',
        }}
      >
        {label}
      </span>
    </div>
  );
  return (
    <div style={{ display: 'flex', width: '100%', height: '100%' }}>
      {half('ir', 'IR')}
      <div style={{ width: '1rem', backgroundColor: 'rgba(235, 235, 245, 0.24)' }} />
      {half('cab', 'Cab')}
    </div>
  );
};

/** One side's own gallery-tile preview inside the Dual Mono split (see
    DualMonoTileImage): the real tone artwork once loaded, a small loading
    scrim while its model downloads, or a bare "+" hint over an empty
    socket - deliberately not the full AddTile/mini-slot treatment (the
    whole tile is one click target that opens the detail view; a half
    isn't independently interactive here). */
const DualMonoTileHalf: React.FC<{ child: ChainItem | undefined; halfSize: number }> = ({
  child,
  halfSize,
}) => {
  const filled = child != null && !isInsertSlot(child);
  if (!filled) {
    return (
      <div
        style={{
          width: '100%',
          height: '100%',
          display: 'flex',
          alignItems: 'center',
          justifyContent: 'center',
          backgroundColor: SURFACE,
        }}
      >
        <PlusCircle size={Math.round(halfSize * 0.22)} color={HIGHLIGHT} />
      </div>
    );
  }
  const busy = child.modelLoading || (!child.loaded && !child.loadFailed);
  return (
    <div style={{ width: '100%', height: '100%', position: 'relative' }}>
      <div style={{ position: 'absolute', inset: 0, opacity: busy ? 0.35 : 1 }}>
        <ToneImage
          src={child.tone.images?.[0]}
          alt={child.tone.title}
          gear={child.tone.gear}
          local={child.tone.local}
          blockType={child.blockType}
          boxSize={halfSize}
          draggable={false}
        />
      </div>
      {busy && (
        <div
          style={{
            position: 'absolute',
            inset: 0,
            display: 'flex',
            alignItems: 'center',
            justifyContent: 'center',
            pointerEvents: 'none',
          }}
        >
          <LoadingDots />
        </div>
      )}
    </div>
  );
};

/** A Dual Mono tile's own artwork: each side's real thumbnail split down
    the middle (matching SplitFileDropZone's own half/divider/half
    layout), instead of the generic "00 DUAL MONO" glyph every other
    LocalTypeGlyph fallback uses - lets Left/Right content be told apart
    from the gallery without opening the block (the punch-list ask this
    exists for). Both empty (a just-added block) still reads clearly via
    the two "+" hints side by side. */
const DualMonoTileImage: React.FC<{ block: ToneBlock; size: number }> = ({ block, size }) => (
  <div style={{ position: 'relative', width: '100%', height: '100%' }}>
    <div style={{ display: 'flex', width: '100%', height: '100%' }}>
      <DualMonoTileHalf child={block.dualLeft?.[0]} halfSize={size / 2} />
      <div style={{ width: '1rem', backgroundColor: 'rgba(235, 235, 245, 0.24)', flexShrink: 0 }} />
      <DualMonoTileHalf child={block.dualRight?.[0]} halfSize={size / 2} />
    </div>
    {/* Block-type identity, kept even once real artwork fills both halves -
        two split thumbnails alone don't read as "this is a Dual Mono
        block" at a glance the way the old generic glyph did. Bottom-left:
        clear of the hover-only top action strip and BlockLed's own
        bottom-right corner. Same abbreviated label ChainMapStrip's
        BLOCK_TYPE_LABEL already uses for this type. */}
    <div
      style={{
        position: 'absolute',
        bottom: '8rem',
        left: '8rem',
        padding: '3rem 8rem',
        borderRadius: '4rem',
        backgroundColor: 'rgba(0, 0, 0, 0.55)',
        pointerEvents: 'none',
      }}
    >
      <span
        style={{
          fontFamily: FONT_MONO,
          fontSize: '11rem',
          fontWeight: 400,
          color: WHITE,
          textTransform: 'uppercase',
          letterSpacing: '0.5rem',
        }}
      >
        Dual
      </span>
    </div>
  </div>
);

/**
 * The complete tile visual: artwork, loading scrim, top action strip and
 * bottom meter. `dragging` pins the action strip visible while the tile
 * travels with the pointer (hover state can't reach it mid-drag).
 */
const TileSurface: React.FC<{
  block: ToneBlock;
  size: number;
  enabled: boolean;
  dragging: boolean;
  /** OS file drag is hovering this tile (split IR/Cab drop target + dashed
      green border) - same split treatment as an empty slot's AddTile, so
      swapping an occupied tile's content routes IR/Cab exactly like adding
      a new one instead of falling back to the old single-zone guess. */
  dropArmed: boolean;
  hoverHalf: 'ir' | 'cab' | null;
  onArmHalf: (category: 'ir' | 'cab') => (e: React.DragEvent) => void;
  onDropOnHalf: (category: 'ir' | 'cab') => (e: React.DragEvent) => void;
  actions: TileActions;
}> = ({
  block,
  size,
  enabled,
  dragging,
  dropArmed,
  hoverHalf,
  onArmHalf,
  onDropOnHalf,
  actions,
}) => {
  const { blockId, tone } = block;
  // Same derivation as ChainBlock.tsx's own eqActive / ChainMapStrip's
  // eqModified: EQ powered on and not flat (a flat or bypassed EQ is
  // skipped natively, so neither counts as "shaping the sound").
  const eqActive = block.params.eq.enabled && !isEqFlat(block.params.eq);

  // A model download/prepare is in flight: `modelLoading` covers switches
  // (where the previous model keeps playing, so `loaded` stays true) and
  // `!loaded` covers fresh blocks that have nothing to play yet.
  const busy = block.modelLoading || (!block.loaded && !block.loadFailed);
  const outMeterId = meterId.blockOut(blockId);

  return (
    // Outer shell stays overflow-visible so inset energy glow isn't needed
    // outside the tile; kept for a stable size box around the face.
    <div
      style={{
        width: `${size}rem`,
        height: `${size}rem`,
        position: 'relative',
        flexShrink: 0,
      }}
    >
      <div
        // Header reveals on :hover via CSS (see index.css), since JS hover state
        // dies across drag re-renders. The traveling tile pins it visible.
        className={dragging ? 'gallery-tile tile-chrome-visible' : 'gallery-tile'}
        onClick={actions.onOpen}
        {...helpProps(toneTileHelp(tone.title))}
        style={{
          width: `${size}rem`,
          height: `${size}rem`,
          borderRadius: '16rem',
          backgroundColor: SURFACE,
          position: 'relative',
          overflow: 'hidden',
          cursor: 'pointer',
          boxSizing: 'border-box',
          border: dropArmed ? FILE_DROP_BORDER : undefined,
          // Touch drags: without this, touch devices claim the gesture for
          // lane scrolling and pointercancel kills the drag instantly. Drag
          // wins on the tile face; lanes still pan from the gaps around it.
          touchAction: 'none',
        }}
      >
        {dropArmed ? (
          <div style={{ position: 'absolute', inset: 0 }}>
            <SplitFileDropZone
              hoverHalf={hoverHalf}
              onArmHalf={onArmHalf}
              onDropOnHalf={onDropOnHalf}
            />
          </div>
        ) : (
          <div
            style={{
              position: 'absolute',
              inset: 0,
              // A Dual Mono tile has no download of its own (loaded=true the
              // instant it exists - see addDualMonoBlock) and dims each
              // side's own artwork internally instead (DualMonoTileHalf),
              // so only `enabled` gates its opacity here.
              opacity:
                block.blockType === 'dualMono'
                  ? enabled
                    ? 1
                    : 0.35
                  : enabled && !busy && !block.loadFailed
                    ? 1
                    : 0.35,
              transition: 'opacity 0.2s ease',
            }}
          >
            {block.blockType === 'dualMono' ? (
              <DualMonoTileImage block={block} size={size} />
            ) : (
              <ToneImage
                src={tone.images?.[0]}
                alt={tone.title}
                gear={tone.gear}
                local={tone.local}
                blockType={block.blockType}
                boxSize={size}
                iconSize={64}
                draggable={false}
              />
            )}
          </div>
        )}

        {/* Busy dots while the model downloads natively; if the download
            failed, a retry affordance instead (dots would spin forever). */}
        {!dropArmed && (busy || block.loadFailed) && (
          <div
            style={{
              position: 'absolute',
              inset: 0,
              display: 'flex',
              alignItems: 'center',
              justifyContent: 'center',
              // Clicks pass through to the tile except on the retry button.
              pointerEvents: 'none',
            }}
          >
            {block.loadFailed ? (
              <div style={{ pointerEvents: 'auto' }}>
                <RetryLoadBadge onRetry={actions.onRetryLoad} />
              </div>
            ) : (
              <LoadingDots />
            )}
          </div>
        )}

        {/* Translucent strip under the quick actions so they read on any art.
            Fades in with the header (opacity only, never a layout change). */}
        {!dropArmed && (
          <div
            className="tile-chrome"
            style={{
              position: 'absolute',
              top: 0,
              left: 0,
              right: 0,
              height: '32rem',
              background: 'rgba(0, 0, 0, 0.35)',
              pointerEvents: 'none',
            }}
          />
        )}

        {/* Top quick-action bar (hover-revealed): power on the left, EQ/swap/
            trash clustered on the right. */}
        {!dropArmed && (
          <div
            className="tile-chrome"
            style={{
              position: 'absolute',
              top: 0,
              left: 0,
              right: 0,
              display: 'flex',
              flexDirection: 'row',
              alignItems: 'center',
              justifyContent: 'space-between',
              padding: '4rem',
            }}
          >
            <ChromeIconButton
              tone="power"
              on={enabled}
              help={HELP.blockPower}
              onClick={actions.onTogglePower}
              onMouseDown={preventFocus}
            >
              <Power size={ICON_SIZE} />
            </ChromeIconButton>
            <div style={{ display: 'flex', gap: '16rem' }}>
              {/* Same yellow-armed/muted-idle chrome as the block detail
                  view's own EQ button (ChainBlock.tsx's eqActive), and the
                  exact same "EQ" text treatment (textBoxStyle's 12rem/400
                  mono, not this box's own icon sizing) rather than an icon
                  glyph - a glance at the gallery already shows which blocks
                  have EQ shaping the sound, not just which ones are
                  bypassed. */}
              <ChromeIconButton
                tone="armed"
                on={eqActive}
                help={HELP.galleryEqShortcut}
                onClick={actions.onOpenEq}
                onMouseDown={preventFocus}
                style={{ fontFamily: FONT_MONO, fontSize: '12rem', fontWeight: 400 }}
              >
                EQ
              </ChromeIconButton>
              {/* A standalone EQ block has no tone/model to replace - see
                  ChainBlockType::EQ. Same for Dual Mono: it has no tone of
                  its own either, only its two child slots (swapped/cleared
                  from its own detail view instead). */}
              {block.blockType !== 'eq' && block.blockType !== 'dualMono' && (
                <ChromeIconButton
                  help={HELP.swapTone}
                  onClick={actions.onSwap}
                  onMouseDown={preventFocus}
                >
                  <ArrowLeftRight size={ICON_SIZE} />
                </ChromeIconButton>
              )}
              <ChromeIconButton
                help={HELP.removeBlock}
                onClick={actions.onRemove}
                onMouseDown={preventFocus}
              >
                <Trash2 size={ICON_SIZE} />
              </ChromeIconButton>
            </div>
          </div>
        )}

        {/* Clip latch lives outside the overflow:hidden face so it stacks
            above the inset glow; red dot only while clipped. */}
      </div>

      {!dropArmed && <BlockEnergyBorder meterId={outMeterId} borderRadius={16} />}
      {!dropArmed && (
        <div style={{ position: 'absolute', bottom: '8rem', right: '8rem', zIndex: 4 }}>
          <BlockLed meterId={outMeterId} size={10} />
        </div>
      )}
    </div>
  );
};

interface GalleryBlockProps {
  block: ToneBlock;
  /** Position within the lane; keeps the sortable registry in sync. */
  index: number;
  /** The lane this tile sorts in. */
  group: ChainSide;
  /** Tile edge, px. */
  size: number;
  /** Open the detail takeover for this block. */
  onOpen: (blockId: string) => void;
  /** Open the detail takeover for this block with its EQ panel already
      showing - the gallery's quick-access EQ button. */
  onOpenEq: (blockId: string) => void;
}

/** Memoized so a lane re-render (e.g. another tile's optimistic state) only
    reaches tiles whose block snapshot actually changed. Mutations come from
    the ChainActions context, so there are no per-render callback props to
    defeat the memo. */
export const GalleryBlock: React.FC<GalleryBlockProps> = React.memo(
  ({ block, index, group, size, onOpen, onOpenEq }) => {
    const { blockId, params } = block;
    const actions = useChainActions();
    const toast = useToast();
    const { menuAnchor, openMenu, closeMenu, shouldIgnoreClick, longPressProps } = useTileMenu();

    // Optimistic power state; native converges via the chainChanged resync
    // (same pattern as the detail card).
    const [enabled, setEnabled] = useState(params.enabled);
    useEffect(() => setEnabled(params.enabled), [params.enabled]);
    // True while an OS file drag hovers the tile (split IR/Cab drop target +
    // green dash - see SplitFileDropZone/AddTile's own copy of this same
    // pattern, which this mirrors so an occupied tile routes a dropped
    // local file exactly like an empty slot does).
    const [dropArmed, setDropArmed] = useState(false);
    const [hoverHalf, setHoverHalf] = useState<'ir' | 'cab' | null>(null);

    const { ref, isDragging } = useSortable({ id: blockId, index, group });

    const handleTogglePower = useCallback(
      (e: React.MouseEvent) => {
        e.stopPropagation();
        setEnabled((prev) => {
          actions.setBlockParam(blockId, 'enabled', !prev);
          return !prev;
        });
      },
      [actions, blockId]
    );

    const handleDropOnHalf = (category: 'ir' | 'cab') => async (e: React.DragEvent) => {
      e.preventDefault();
      e.stopPropagation();
      setDropArmed(false);
      setHoverHalf(null);
      const item = e.dataTransfer.items[0];
      if (!item) return;
      const error = await actions.loadLocalFile(blockId, item, category);
      if (error) toast.show(error);
    };

    const armHalf = (category: 'ir' | 'cab') => (e: React.DragEvent) => {
      if (!isFileDrag(e)) return;
      e.preventDefault();
      e.stopPropagation();
      e.dataTransfer.dropEffect = 'copy';
      setHoverHalf(category);
    };

    return (
      <div
        ref={ref}
        onContextMenu={openMenu}
        {...longPressProps}
        onDragOver={(e) => armFileDrag(e, setDropArmed)}
        onDragLeave={(e) => {
          disarmFileDrag(e, setDropArmed);
          if (!e.currentTarget.contains(e.relatedTarget as Node)) setHoverHalf(null);
        }}
        style={{
          // Dim the tile while it travels with the pointer; the hidden
          // placeholder dnd-kit leaves in the lane reveals the plus-circle
          // rail behind the vacated slot.
          opacity: isDragging ? DRAG_GHOST_OPACITY : 1,
          flexShrink: 0,
          position: 'relative',
          // Above the neighboring tiles while the action sheet is up.
          zIndex: menuAnchor ? 5 : undefined,
        }}
      >
        <TileSurface
          block={block}
          size={size}
          enabled={enabled}
          dragging={isDragging}
          dropArmed={dropArmed}
          hoverHalf={hoverHalf}
          onArmHalf={armHalf}
          onDropOnHalf={handleDropOnHalf}
          actions={{
            onOpen: (e) => {
              if (shouldIgnoreClick(e)) return;
              onOpen(blockId);
            },
            onTogglePower: handleTogglePower,
            onOpenEq: (e) => {
              e.stopPropagation();
              onOpenEq(blockId);
            },
            onSwap: (e) => {
              e.stopPropagation();
              // No navigateToDetail: this tile is on the gallery, not a
              // detail view, so the swap should leave the user right there
              // (mirrors the gallery's own "+" tiles - see ChainBlock.tsx's
              // swap button for the opposite case).
              actions.swapBlock(blockId);
            },
            onRemove: (e) => {
              e.stopPropagation();
              actions.removeBlock(blockId);
            },
            onRetryLoad: () => actions.retryLoad(blockId),
          }}
        />
        {menuAnchor && (
          <TileMenu
            anchor={menuAnchor}
            onClose={closeMenu}
            items={[
              {
                label: 'Copy',
                icon: <Copy size={16} />,
                help: HELP.copyBlock,
                onSelect: () => actions.copyBlock(blockId),
              },
              ...blockTypeMenuItems(blockId, actions, toast),
            ]}
          />
        )}
      </div>
    );
  }
);
GalleryBlock.displayName = 'GalleryBlock';

/** Plus glyph: 48 on mono tiles (224), 40 on stereo (160). Half of that is
    the radius the routing lines run edge-to-circle against. */
export const plusIconSize = (tileSize: number) => (tileSize <= 160 ? 40 : 48);

/** Lucide's circle-plus draws its circle at r=10 inside the 24-unit viewBox,
    so the visible ring sits 2/24 of the rendered size in from the icon's
    bounding box (measured to the stroke's centerline). Connector lines must
    overshoot the box by this much to actually meet the ring; stopping half a
    stroke short (at the box edge) reads as a hairline gap. */
export const plusCircleInset = (iconSize: number) => (iconSize * 2) / 24;

/** Which tile edges get a routing line into the plus circle (signal-flow
    continuation of the lane's connector lines). */
export type AddTileRouting = 'left' | 'right' | 'both' | 'none';

/** Face of the insert slot tile. */
const addTileFaceStyle = (size: number): React.CSSProperties => ({
  width: `${size}rem`,
  height: `${size}rem`,
  borderRadius: '16rem',
  backgroundColor: SURFACE_RAISED,
  border: ADD_TILE_BORDER,
  position: 'relative',
  display: 'flex',
  alignItems: 'center',
  justifyContent: 'center',
  color: '#ffffff',
  flexShrink: 0,
  boxSizing: 'border-box',
});

interface AddTileProps {
  /** Insert slot block id. */
  id: string;
  /** Position within the lane; keeps the sortable registry in sync. */
  index: number;
  /** The lane this tile sorts in. */
  group: ChainSide;
  size: number;
  routing: AddTileRouting;
  onClick: () => void;
  /** Paste the copied block into this slot; null while there's nothing valid
      to paste (the action sheet shows Paste disabled). */
  onPaste?: (() => void) | null;
}

/** The insert slot as a dashed add tile, sortable so the insert point can be
    repositioned within its lane, like any other block. Routing lines continue
    the lane's connector line through to the plus circle. Also the drop zone
    for local .nam / IR .wav files (loaded natively, no browser flow). */
export const AddTile: React.FC<AddTileProps> = ({
  id,
  index,
  group,
  size,
  routing,
  onClick,
  onPaste = null,
}) => {
  const { menuAnchor, openMenu, closeMenu, shouldIgnoreClick, longPressProps } = useTileMenu();
  const actions = useChainActions();
  const toast = useToast();
  // True while an OS file drag hovers the tile (drop-target highlight).
  const [dropArmed, setDropArmed] = useState(false);
  // Which half of the split drop zone the pointer is currently over - only
  // meaningful while dropArmed (the halves only render then). An empty slot
  // has no existing block to infer a category from, unlike a swap onto an
  // occupied tile (GalleryBlock's own handleDrop, unchanged) or a catalog
  // pick (gear comes from the tone metadata) - so the drop target itself has
  // to carry the choice.
  const [hoverHalf, setHoverHalf] = useState<'ir' | 'cab' | null>(null);
  const { ref, isDragging } = useSortable({ id, index, group });

  const handleDropOnHalf = (category: 'ir' | 'cab') => async (e: React.DragEvent) => {
    e.preventDefault();
    e.stopPropagation();
    setDropArmed(false);
    setHoverHalf(null);
    // The item (not files[0]): folders only surface through the entry API.
    const item = e.dataTransfer.items[0];
    if (!item) return;
    const error = await actions.loadLocalFile(id, item, category);
    if (error) toast.show(error);
  };

  // Own dragover per half (not just the tile-wide armFileDrag above): the
  // browser only allows a drop on an element that itself keeps cancelling
  // dragover, and only one half should highlight at a time as the pointer
  // crosses between them.
  const armHalf = (category: 'ir' | 'cab') => (e: React.DragEvent) => {
    if (!isFileDrag(e)) return;
    e.preventDefault();
    e.stopPropagation();
    e.dataTransfer.dropEffect = 'copy';
    setHoverHalf(category);
  };

  // Anchored inside the tile's border (absolute children position against
  // the padding box), so the run to the plus ring is a border-width shorter
  // than measured from the tile's outer edge.
  const routingLine = (edge: 'left' | 'right') => (
    <div
      style={{
        position: 'absolute',
        top: '50%',
        [edge]: 0,
        width: `${size / 2 - plusIconSize(size) / 2 + plusCircleInset(plusIconSize(size)) - ADD_TILE_BORDER_WIDTH}rem`,
        height: '2rem',
        backgroundColor: '#ffffff',
        transform: 'translateY(-50%)',
      }}
    />
  );

  return (
    <div
      ref={ref}
      onClick={(e) => {
        if (shouldIgnoreClick(e)) return;
        onClick();
      }}
      onContextMenu={openMenu}
      {...longPressProps}
      onDragOver={(e) => armFileDrag(e, setDropArmed)}
      onDragLeave={(e) => {
        disarmFileDrag(e, setDropArmed);
        // Crossing between the two halves below still fires dragLeave on
        // this (their shared parent) - the same bubbling disarmFileDrag
        // itself already guards against, via the same contains() check.
        if (!e.currentTarget.contains(e.relatedTarget as Node)) setHoverHalf(null);
      }}
      {...helpProps(HELP.addTile)}
      style={{
        ...addTileFaceStyle(size),
        ...(dropArmed ? { border: FILE_DROP_BORDER } : {}),
        opacity: isDragging ? DRAG_GHOST_OPACITY : 1,
        cursor: 'pointer',
        // Touch drags need the gesture (see the tone tile face).
        touchAction: 'none',
        // Above the neighboring tiles while the action sheet is up.
        zIndex: menuAnchor ? 5 : undefined,
        // Clips the two drop-zone halves' hover tint to the tile's own
        // rounded corners below - without this their square tint rects
        // would poke past the border radius.
        overflow: 'hidden',
      }}
    >
      {!dropArmed &&
        !isDragging &&
        (routing === 'left' || routing === 'both') &&
        routingLine('left')}
      {!dropArmed &&
        !isDragging &&
        (routing === 'right' || routing === 'both') &&
        routingLine('right')}
      {dropArmed ? (
        <SplitFileDropZone
          hoverHalf={hoverHalf}
          onArmHalf={armHalf}
          onDropOnHalf={handleDropOnHalf}
        />
      ) : (
        <PlusCircle size={plusIconSize(size)} strokeWidth={1} />
      )}
      {menuAnchor && (
        <TileMenu
          anchor={menuAnchor}
          onClose={closeMenu}
          items={[
            {
              label: 'Paste',
              icon: <ClipboardPaste size={16} />,
              help: HELP.pasteBlock,
              disabled: onPaste == null,
              onSelect: () => onPaste?.(),
            },
            ...blockTypeMenuItems(id, actions, toast),
          ]}
        />
      )}
    </div>
  );
};
