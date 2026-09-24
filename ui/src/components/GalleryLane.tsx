import React from 'react';
import { PlusCircle } from './icons';
import { GalleryBlock, AddTile, plusIconSize, plusCircleInset } from './GalleryBlock';
import type { AddTileRouting } from './GalleryBlock';
import type { ChainItem, ToneBlock } from '../types/chain';
import { adjacentInsertSlots, isInsertSlot } from '../types/chain';
/**
 * Lane-level pieces of the chain gallery (see ChainView for the drag
 * orchestration that owns them): the ghost rail, the lane of tiles, and the
 * scroll-edge fades.
 */

export const TILE_SIZE = 224;
/** Gap between tiles: the visible run of each connector line. */
export const TILE_GAP = 24;
/** Gutter inside the scroll area; tiles fade out under it while scrolling. */
export const EDGE_FADE_WIDTH = 32;

/** True for an enabled block that widens whatever it receives into a real
    stereo image on its own: a Dual Mono block (its own Pan/Align - see
    ChainBlock.tsx's isDualMono branch) or a true stereo IR (independent L/R
    convolution - see GalleryBlock.tsx's own isTrueStereoIr). Disabled means
    bypassed/dry, so it passes through whatever channel count it already
    received rather than widening anything itself. */
const isStereoWidener = (block: ToneBlock): boolean =>
  block.params.enabled &&
  (block.blockType === 'dualMono' || (block.blockType === 'ir' && (block.irNumChannels ?? 1) >= 2));

/** True once an earlier block in the chain has already widened the signal
    into real stereo (see isStereoWidener): every block downstream of it
    processes that widened signal, so its own channel badge (see
    TileChannelBadge) should read Stereo too - an EQ (or any other block)
    placed after a Dual Mono block or a stereo IR was reading Mono here
    despite genuinely carrying a stereo signal. */
export const isDownstreamOfStereoWidener = (items: ChainItem[], index: number): boolean =>
  items.slice(0, index).some((it) => !isInsertSlot(it) && isStereoWidener(it));

/** Signal-flow routing lines for an add tile at the given lane position. */
const addTileRouting = (index: number, count: number): AddTileRouting => {
  if (count <= 1) return 'none';
  if (index === 0) return 'right';
  if (index === count - 1) return 'left';
  return 'both';
};

/**
 * Static ghost rail behind the lane: one plus circle per slot, connector
 * lines between them. The circles sit hidden behind the (opaque) tiles and
 * appear when a slot is vacated mid-drag; only the line runs inside the gaps
 * are visible otherwise.
 */
const GhostRail: React.FC<{ slots: number; tileSize: number }> = ({ slots, tileSize }) => (
  <div
    style={{
      position: 'absolute',
      inset: 0,
      display: 'flex',
      flexDirection: 'row',
      alignItems: 'center',
      gap: `${TILE_GAP}rem`,
      pointerEvents: 'none',
      zIndex: 1,
    }}
  >
    {Array.from({ length: slots }, (_, i) => (
      <span
        key={`${i}-rail`}
        style={{
          width: `${tileSize}rem`,
          height: `${tileSize}rem`,
          display: 'flex',
          alignItems: 'center',
          justifyContent: 'center',
          position: 'relative',
          flexShrink: 0,
        }}
      >
        {i > 0 && (
          // Runs from the previous slot's plus ring to this slot's, extended
          // past each icon's bounding box by plusCircleInset so the line
          // actually meets the drawn circle (see GalleryBlock).
          <div
            style={{
              position: 'absolute',
              left: `${-(TILE_GAP + tileSize / 2 - plusIconSize(tileSize) / 2 + plusCircleInset(plusIconSize(tileSize)))}rem`,
              top: '50%',
              width: `${TILE_GAP + tileSize - plusIconSize(tileSize) + 2 * plusCircleInset(plusIconSize(tileSize))}rem`,
              height: '2rem',
              backgroundColor: '#ffffff',
              transform: 'translateY(-50%)',
            }}
          />
        )}
        <PlusCircle size={plusIconSize(tileSize)} strokeWidth={1} />
      </span>
    ))}
  </div>
);

/** The lane of tiles over its ghost rail. Native keeps the chain at its
    minimum slot layout (5 tiles, always ≥1 insert), so each item here is a
    real block, insert slots included, and every tile is reorderable. */
export const GalleryLane: React.FC<{
  items: ChainItem[];
  tileSize: number;
  onOpen: (blockId: string) => void;
  /** Open the detail takeover with its EQ panel already showing - the
      gallery tile's own EQ quick-access button. */
  onOpenEq: (blockId: string) => void;
  /** Open the detail takeover with its Stereo Processing panel already
      showing - a Dual Mono tile's own quick-access button, next to EQ's. */
  onOpenStereo: (blockId: string) => void;
  /** Open the tone browser targeting the clicked insert slot. */
  onAdd: (insertBlockId: string) => void;
  /** Paste the copied block into the insert slot at this index; null while
      there's nothing valid to paste (insert action sheets show Paste
      disabled). */
  onPasteBlock?: ((index: number) => void) | null;
}> = ({ items, tileSize, onOpen, onOpenEq, onOpenStereo, onAdd, onPasteBlock = null }) => (
  <div style={{ position: 'relative', width: 'max-content' }}>
    <GhostRail slots={items.length} tileSize={tileSize} />
    <div
      style={{
        display: 'flex',
        flexDirection: 'row',
        alignItems: 'center',
        gap: `${TILE_GAP}rem`,
        position: 'relative',
        zIndex: 2,
      }}
    >
      {items.map((item, index) =>
        isInsertSlot(item) ? (
          <AddTile
            key={item.blockId}
            id={item.blockId}
            index={index}
            group="chain"
            size={tileSize}
            routing={addTileRouting(index, items.length)}
            onClick={() => onAdd(item.blockId)}
            onPaste={onPasteBlock != null ? () => onPasteBlock(index) : null}
            isLast={index === items.length - 1}
          />
        ) : (
          <GalleryBlock
            key={item.blockId}
            block={item}
            index={index}
            group="chain"
            size={tileSize}
            onOpen={onOpen}
            onOpenEq={onOpenEq}
            onOpenStereo={onOpenStereo}
            stereo={isDownstreamOfStereoWidener(items, index)}
            slotLeft={adjacentInsertSlots(items, index).left}
            slotRight={adjacentInsertSlots(items, index).right}
          />
        )
      )}
    </div>
  </div>
);

/** Fade the lane out under the gutters as it scrolls, so content slides
    behind a smooth ramp to the background instead of hard-clipping. */
export const EdgeFade: React.FC<{ side: 'left' | 'right' }> = ({ side }) => (
  <div
    style={{
      position: 'absolute',
      top: 0,
      bottom: 0,
      // Overhang the outer edge by a design px: at fractional UI scales the
      // scrollport's clip edge and this overlay can round to different
      // device pixels, which would leave a subpixel strip of content visible
      // just past the fade. The overhang end is solid black over the black
      // background, so it never shows.
      [side]: '-1rem',
      width: `${EDGE_FADE_WIDTH + 1}rem`,
      background: `linear-gradient(to ${side === 'left' ? 'right' : 'left'}, #000000, rgba(0, 0, 0, 0))`,
      pointerEvents: 'none',
      zIndex: 3,
    }}
  />
);
