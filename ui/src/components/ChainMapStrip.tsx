import React from 'react';
import { Plus } from './icons';
import { ChromeIconButton, ChromeTextButton } from './ChromeIconButton';
import { HELP } from './helpText';
import { useHorizontalWheelScroll } from '../hooks/useHorizontalWheelScroll';
import type { ToneBlock } from '../types/chain';

/** Abbreviated strip label per native block type (ToneBlock.blockType):
    mirrors the header's own NAM/IR split, plus the newer CAB type. */
const BLOCK_TYPE_LABEL: Record<ToneBlock['blockType'], string> = {
  nam: 'NAM',
  ir: 'IR',
  cab: 'CAB',
};

interface ChainMapStripProps {
  /** Every tone block in the open block's lane, in chain order. Lane-local
      like the Prev/Next chevrons it replaces (issue #83) — a branch only
      taps the other lane's signal, it never merges the two arrays, so this
      stays correct in stereo too, branched or not (see ChainView). */
  blocks: ToneBlock[];
  currentBlockId: string;
  /** Jump straight into another block's detail view — the same mechanism
      the gallery's own tap-to-open already uses (ChainView's
      setDetailBlockId), just addressable from any block, not only the
      lane-adjacent one. */
  onSelect: (blockId: string) => void;
  /** Reuses the existing add-tone flow (TONE3000 browser or local file),
      targeting the nearest insert slot at/after this block's position (see
      ChainView) — true "insert between two blocks" is out of scope for this
      pass, so this rides the same slot-targeted mechanism the gallery's own
      "+" tiles use rather than splicing the chain array. Null when the lane
      has no insert slot left to target (shouldn't happen given the lane's
      always-one-trailing-insert invariant — kMinLaneSlots — but the control
      disables rather than silently no-opping). */
  onAdd: (() => void) | null;
}

/**
 * Persistent chain-map strip at the top of the block detail view (replaces
 * the old Prev/Next chevrons, issue #83): every block in this lane rendered
 * as a compact NAM/IR/CAB chip, left to right in chain order, with the open
 * block highlighted. Hovering a chip surfaces the tone's name via the
 * shared faceplate help readout (see helpText.ts); clicking one jumps
 * straight to that block. The trailing + adds a new block via the existing
 * add-tone flow. Chips never shrink or wrap — the row scrolls horizontally
 * instead once a chain overflows the card width.
 */
export const ChainMapStrip: React.FC<ChainMapStripProps> = ({
  blocks,
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
        marginBottom: '16rem',
        flexShrink: 0,
        overflowX: 'auto',
        overflowY: 'hidden',
      }}
    >
      {blocks.map((block) => (
        <ChromeTextButton
          key={block.blockId}
          onClick={() => onSelect(block.blockId)}
          help={`${block.tone.title} · ${BLOCK_TYPE_LABEL[block.blockType]}`}
          open={block.blockId === currentBlockId}
        >
          {BLOCK_TYPE_LABEL[block.blockType]}
        </ChromeTextButton>
      ))}
      <ChromeIconButton
        help={HELP.addBlockAfter}
        onClick={() => onAdd?.()}
        disabled={onAdd == null}
      >
        <Plus />
      </ChromeIconButton>
    </div>
  );
};
