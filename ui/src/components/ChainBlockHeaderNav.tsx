import React from 'react';
import { ArrowLeft } from './icons';
import { ChainMapStrip, type ChainMapChildTab } from './ChainMapStrip';
import { HELP, helpProps } from './helpText';
import { WHITE } from './theme';
import type { ChainItem } from '../types/chain';

interface ChainBlockHeaderNavProps {
  /** Back-arrow click: pop one level (useDetailViewStack's `pop`). */
  onPop: () => void;
  /** The chain strip's own persistent Home chip: jump straight to the
      gallery regardless of how deep the detail-view stack currently is
      (unlike `onPop`, which only steps back one level). For a top-level
      block this is the exact same closure `onBack` already is. */
  onGoHome: () => void;
  /** Only affects which hover-help text the arrow shows. */
  isDualChild: boolean;
  chainStripItems: ChainItem[];
  blockId: string;
  /** ChainMapStrip's plain chip jump: lands on the target block's own
      plain view (a lateral move, not a deeper push - see
      useDetailViewStack's `reset`). */
  onSelect: (targetBlockId: string) => void;
  /** ChainMapStrip's EQ-mark jump: same destination, straight into its EQ. */
  onSelectEq: (targetBlockId: string) => void;
  onAddBlockAt: (insertBlockId: string) => void;
  onPasteBlockAt: ((index: number) => void) | null;
  /** L/R child tabs that expand the currently-open chip inline (only a
      Dual Mono block populates this today) - omitted/empty for every other
      block type, which has no sibling sub-screens worth surfacing here yet
      (its own EQ mark already covers the one it has). See ChainMapStrip's
      own doc comment. */
  currentChildTabs?: ChainMapChildTab[];
}

/**
 * The "← BLOCK"-row header shared by all three of ChainBlock's render
 * branches (isEq, isDualMono, main): a bare back arrow, then the chain
 * strip, left-aligned right after it (see ChainMapStrip's own doc comment
 * for why it isn't centered). No breadcrumb text anymore - the strip itself
 * is the map now (its persistent Home chip, and the currently-open chip's
 * own inline sub-view tabs), which is also what fixed the strip getting
 * crowded out by a long, ever-growing text trail. Stays visible even for a
 * Dual Mono child's own recursed editor (via ChainBlock's
 * `dualStripContext`, which swaps in the wrapper's own identity/tabs/home
 * target so the child's render of this same strip still points at a real
 * chip). One shared component instead of three hand-kept copies.
 */
export const ChainBlockHeaderNav: React.FC<ChainBlockHeaderNavProps> = ({
  onPop,
  onGoHome,
  isDualChild,
  chainStripItems,
  blockId,
  onSelect,
  onSelectEq,
  onAddBlockAt,
  onPasteBlockAt,
  currentChildTabs,
}) => (
  <div
    style={{
      display: 'flex',
      flexDirection: 'row',
      alignItems: 'center',
      // Tighter than the old 16rem: with the strip left-aligned (see
      // ChainMapStrip's own doc comment) right after the arrow, its first
      // chip - the persistent Home chip - reads as a companion to Back
      // rather than a separate row segment, so it sits close the same way
      // the strip's own chips sit close to each other (6rem).
      gap: '8rem',
      marginBottom: '16rem',
      flexShrink: 0,
    }}
  >
    <button
      type="button"
      onClick={onPop}
      {...helpProps(isDualChild ? HELP.backToDual : HELP.backToChain)}
      style={{
        display: 'flex',
        alignItems: 'center',
        flexShrink: 0,
        background: 'transparent',
        border: 'none',
        outline: 'none',
        padding: 0,
        cursor: 'pointer',
        color: WHITE,
      }}
    >
      <ArrowLeft size={16} style={{ display: 'block', flexShrink: 0 }} />
    </button>

    <ChainMapStrip
      items={chainStripItems}
      currentBlockId={blockId}
      onSelect={onSelect}
      onSelectEq={onSelectEq}
      onAdd={onAddBlockAt}
      onPasteBlockAt={onPasteBlockAt}
      onGoHome={onGoHome}
      currentChildTabs={currentChildTabs}
    />
  </div>
);
