import React, { useEffect, useState } from 'react';
import { Copy, Layers } from './icons';
import { HELP, helpProps } from './helpText';
import { TileMenu } from './TileMenu';
import type { TileMenuItem } from './TileMenu';
import { useTileMenu } from '../hooks/useTileMenu';
import type { ChainActions } from '../hooks/useChainActions';
import {
  BLACK,
  BRAND_YELLOW,
  FONT_MONO,
  SEGMENTED_TRACK,
  SUBTLE,
  WHITE,
  segmentedGroupStyle,
} from './theme';
import { CHANNEL_LETTERS, NUM_CHANNELS } from '../types/chain';
import type { ToneBlock } from '../types/chain';

/**
 * Block channels (see ScenesState): four full versions of a block, A-D.
 * Shared pieces for every place a block shows up - the card header's
 * A|B|C|D selector, the letter badge on chips/tiles, and the right-click
 * menu rows.
 *
 * Visual language: the active channel is always BRAND_YELLOW (the same
 * accent as the active scene), a channel holding settings is WHITE, an
 * unused one SUBTLE (picking it starts it as a copy of the current one).
 */

export const channelLetter = (channel: number | undefined) => CHANNEL_LETTERS[channel ?? 0] ?? 'A';

export const blockChannel = (block: ToneBlock) => block.params.channel ?? 0;

/** Slot `c` holds settings (the active one always does). */
export const channelUsed = (block: ToneBlock, c: number) =>
  c === blockChannel(block) || !!block.params.channelsUsed?.[c];

/** More than one channel in play: worth a letter badge on compact views. */
export const usesChannels = (block: ToneBlock) =>
  Array.from({ length: NUM_CHANNELS }, (_, c) => channelUsed(block, c)).filter(Boolean).length > 1;

/** Right-click rows: "Channel ▸ A-D" and "Copy Channel X To ▸ ...". */
export const channelMenuItems = (
  block: ToneBlock,
  actions: Pick<ChainActions, 'selectBlockChannel' | 'copyBlockChannel'>
): TileMenuItem[] => {
  const active = blockChannel(block);
  const letterIcon = (c: number) => (
    <span
      style={{
        width: '16rem',
        fontFamily: FONT_MONO,
        fontWeight: 600,
        color: c === active ? BRAND_YELLOW : channelUsed(block, c) ? WHITE : SUBTLE,
      }}
    >
      {CHANNEL_LETTERS[c]}
    </span>
  );
  const all = Array.from({ length: NUM_CHANNELS }, (_, c) => c);
  return [
    {
      label: `Channel ${channelLetter(active)}`,
      icon: <Layers size={16} />,
      help: HELP.blockChannel,
      submenu: all.map((c) => ({
        label: `Channel ${CHANNEL_LETTERS[c]}${
          c === active ? ' ✓' : channelUsed(block, c) ? '' : ' (new copy)'
        }`,
        icon: letterIcon(c),
        help: HELP.blockChannel,
        onSelect: () => actions.selectBlockChannel(block.blockId, c),
      })),
    },
    {
      label: `Copy ${channelLetter(active)} To`,
      icon: <Copy size={16} />,
      help: HELP.blockChannelCopy,
      submenu: all
        .filter((c) => c !== active)
        .map((c) => ({
          label: `Channel ${CHANNEL_LETTERS[c]}`,
          icon: letterIcon(c),
          help: HELP.blockChannelCopy,
          onSelect: () => actions.copyBlockChannel(block.blockId, active, c),
        })),
    },
  ];
};

/**
 * Card-header A|B|C|D selector. Click a letter: switch channel (gapless for
 * warm NAM models). Right-click: copy the active channel onto another.
 * Optimistic: the clicked letter lights at once, native converges through
 * the chain resync.
 */
export const ChannelSelector: React.FC<{
  block: ToneBlock;
  actions: Pick<ChainActions, 'selectBlockChannel' | 'copyBlockChannel'>;
}> = ({ block, actions }) => {
  const nativeActive = blockChannel(block);
  const [active, setActive] = useState(nativeActive);
  useEffect(() => setActive(nativeActive), [nativeActive]);
  const menu = useTileMenu();

  return (
    <div
      {...helpProps(HELP.blockChannel)}
      onContextMenu={menu.openMenu}
      style={{ ...segmentedGroupStyle(), gap: '1rem', backgroundColor: 'transparent' }}
    >
      {Array.from({ length: NUM_CHANNELS }, (_, c) => {
        const isActive = c === active;
        const used = channelUsed(block, c);
        return (
          <button
            key={c}
            type="button"
            aria-pressed={isActive}
            onClick={() => {
              setActive(c);
              actions.selectBlockChannel(block.blockId, c);
            }}
            {...helpProps(
              `Channel ${CHANNEL_LETTERS[c]}${
                used ? '' : ' (unused: starts as a copy of the current one)'
              } - ${HELP.blockChannel}`
            )}
            style={{
              all: 'unset',
              boxSizing: 'border-box',
              cursor: 'pointer',
              width: '20rem',
              height: '100%',
              display: 'grid',
              placeItems: 'center',
              borderRadius: '2rem',
              fontFamily: FONT_MONO,
              fontSize: '12rem',
              fontWeight: 600,
              lineHeight: 1,
              color: isActive ? BLACK : used ? WHITE : SUBTLE,
              background: isActive ? BRAND_YELLOW : SEGMENTED_TRACK,
              transition: 'background 0.12s ease, color 0.12s ease',
            }}
          >
            {CHANNEL_LETTERS[c]}
          </button>
        );
      })}
      {menu.menuAnchor && (
        <TileMenu
          anchor={menu.menuAnchor}
          onClose={menu.closeMenu}
          items={channelMenuItems(
            { ...block, params: { ...block.params, channel: active } },
            actions
          )}
        />
      )}
    </div>
  );
};

/**
 * Small channel letter for compact block views (chips, gallery tiles).
 * Positioned by the caller; shown only once a block has more than one
 * channel in play, so a plain chain stays uncluttered.
 */
export const ChannelBadge: React.FC<{
  block: ToneBlock;
  /** Drawn on a light (white) surface. */
  onLight?: boolean;
  size?: number;
  style?: React.CSSProperties;
}> = ({ block, onLight = false, size = 9, style }) => {
  if (!usesChannels(block)) return null;
  return (
    <span
      aria-label={`Channel ${channelLetter(blockChannel(block))}`}
      style={{
        fontFamily: FONT_MONO,
        fontSize: `${size}rem`,
        fontWeight: 700,
        lineHeight: 1,
        color: onLight ? BLACK : BRAND_YELLOW,
        pointerEvents: 'none',
        ...style,
      }}
    >
      {channelLetter(blockChannel(block))}
    </span>
  );
};
