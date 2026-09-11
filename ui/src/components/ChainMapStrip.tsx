import React from 'react';
import { Plus } from './icons';
import { ChromeIconButton, ChromeTextButton } from './ChromeIconButton';
import { HELP } from './helpText';
import { useHorizontalWheelScroll } from '../hooks/useHorizontalWheelScroll';
import { DISABLED_OPACITY } from './theme';
import type { ChainItem, ToneBlock } from '../types/chain';
import { isInsertSlot } from '../types/chain';

/** Abbreviated strip label per native block type (ToneBlock.blockType):
    mirrors the header's own NAM/IR split, plus the newer CAB type. */
const BLOCK_TYPE_LABEL: Record<ToneBlock['blockType'], string> = {
  nam: 'NAM',
  ir: 'IR',
  cab: 'CAB',
};

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
  /** Add a block at this specific insert slot, via the existing add-tone
      flow (TONE3000 browser or local file) — the same
      actions.addModel(side, insertBlockId) targeting GalleryLane's own "+"
      tiles use. */
  onAdd: (insertBlockId: string) => void;
}

/**
 * Persistent chain-map strip in the block detail view's header row (replaces
 * the old Prev/Next chevrons, issue #83): the open block's whole lane
 * rendered left to right in chain order — a compact NAM/IR/CAB chip for
 * every tone block, a "+" for every insert slot at its real position — with
 * the open block highlighted and bypassed blocks dimmed. Hovering a chip
 * surfaces the tone's name (and bypass state) via the shared faceplate help
 * readout (see helpText.ts); clicking one jumps straight to that block, or
 * (for a "+") adds a new block right there. Chips never shrink or wrap —
 * the row scrolls horizontally instead once a chain overflows the
 * available width.
 */
export const ChainMapStrip: React.FC<ChainMapStripProps> = ({
  items,
  currentBlockId,
  onSelect,
  onAdd,
}) => {
  const wheelScrollRef = useHorizontalWheelScroll<HTMLDivElement>();
  return (
    <div
      ref={wheelScrollRef}
      className="hide-scrollbar"
      style={{
        display: 'flex',
        alignItems: 'center',
        gap: '6rem',
        flex: 1,
        minWidth: 0,
        overflowX: 'auto',
        overflowY: 'hidden',
      }}
    >
      {items.map((item) =>
        isInsertSlot(item) ? (
          <ChromeIconButton
            key={item.blockId}
            help={HELP.addTile}
            onClick={() => onAdd(item.blockId)}
          >
            <Plus />
          </ChromeIconButton>
        ) : (
          <ChromeTextButton
            key={item.blockId}
            onClick={() => onSelect(item.blockId)}
            help={`${item.tone.title} · ${BLOCK_TYPE_LABEL[item.blockType]}${
              item.params.enabled ? '' : ' · Bypassed'
            }`}
            open={item.blockId === currentBlockId}
            // Bypassed blocks dim like any other off/disabled chrome in this
            // app (see ChromeIconButton's own disabled treatment) rather than
            // a bespoke color, so it reads as "off" at a glance without
            // fighting the open/idle chrome underneath it.
            style={item.params.enabled ? undefined : { opacity: DISABLED_OPACITY }}
          >
            {BLOCK_TYPE_LABEL[item.blockType]}
          </ChromeTextButton>
        )
      )}
    </div>
  );
};
