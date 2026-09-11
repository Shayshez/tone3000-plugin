import React from 'react';
import { Plus } from './icons';
import { ChromeIconButton, ChromeTextButton } from './ChromeIconButton';
import { HELP } from './helpText';
import { useHorizontalWheelScroll } from '../hooks/useHorizontalWheelScroll';
import { BORDER, BRAND_YELLOW, DISABLED_OPACITY } from './theme';
import type { ChainItem, ToneBlock } from '../types/chain';
import { isInsertSlot } from '../types/chain';

/** Abbreviated strip label per native block type (ToneBlock.blockType):
    mirrors the header's own NAM/IR split, plus the newer CAB type. */
const BLOCK_TYPE_LABEL: Record<ToneBlock['blockType'], string> = {
  nam: 'NAM',
  ir: 'IR',
  cab: 'CAB',
};

/** Every chip (tone label or "+") is this exact box, regardless of label
    length, so the strip reads as a uniform row rather than ragged pill
    widths. Sized up from the shared TEXT_BOX_HEIGHT/ICON_BOX_SIZE default
    (20rem) so chips feel like a real navigation control, not a footnote. */
const CHIP_WIDTH = 44;
const CHIP_HEIGHT = 28;
const CHIP_FONT_SIZE = 13;

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
 * the open block highlighted and bypassed blocks visually flat. Hovering a
 * chip surfaces the tone's name (and bypass state) via the shared faceplate
 * help readout (see helpText.ts); clicking one jumps straight to that
 * block, or (for a "+") adds a new block right there.
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
  onAdd,
}) => {
  const wheelScrollRef = useHorizontalWheelScroll<HTMLDivElement>();
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
        {items.map((item) =>
          isInsertSlot(item) ? (
            <ChromeIconButton
              key={item.blockId}
              help={HELP.addTile}
              onClick={() => onAdd(item.blockId)}
              // Plain ChromeIconButton chrome has a transparent border (fine
              // floating beside a knob, but it read as a bare icon here,
              // next to chips that all carry a real idle border) - match the
              // tone chips' own idle frame so the "+" reads as one more
              // uniform member of the row, not a stray icon.
              style={{ width: `${CHIP_WIDTH}rem`, height: `${CHIP_HEIGHT}rem`, border: BORDER }}
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
              style={{
                width: `${CHIP_WIDTH}rem`,
                height: `${CHIP_HEIGHT}rem`,
                fontSize: `${CHIP_FONT_SIZE}rem`,
                // Active (non-bypassed), not the one currently open: a thin
                // BRAND_YELLOW outline on the plain idle chrome (no fill) -
                // enough to read as "on" at a glance without turning most of
                // the strip solid yellow (a full armed fill, tried first,
                // overwhelmed the row since most blocks in a chain are
                // active and only a couple are usually bypassed). The
                // currently-open chip already reads as unambiguously current
                // via its own white `open` fill, so it skips this accent.
                ...(item.params.enabled && item.blockId !== currentBlockId
                  ? { border: `1rem solid ${BRAND_YELLOW}` }
                  : null),
                // Bypassed: dim like any other off control in this app
                // (DISABLED_OPACITY), on top of whatever chrome above.
                ...(item.params.enabled ? null : { opacity: DISABLED_OPACITY }),
              }}
            >
              {BLOCK_TYPE_LABEL[item.blockType]}
            </ChromeTextButton>
          )
        )}
      </div>
    </div>
  );
};
