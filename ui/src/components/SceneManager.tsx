import React, { useEffect, useRef, useState } from 'react';
import { Copy, Minus, Plus, Power, X as XIcon } from './icons';
import { ChromeIconButton } from './ChromeIconButton';
import { TileMenu } from './TileMenu';
import { useTileMenu } from '../hooks/useTileMenu';
import { HELP, helpProps } from './helpText';
import { isKeyboardOwned, isTypingTarget } from '../keyPassthrough';
import {
  BLACK,
  BORDER,
  BRAND_YELLOW,
  FONT_MONO,
  GRAY,
  MUTED,
  SEGMENTED_TRACK,
  SUBTLE,
  SURFACE,
  WHITE,
} from './theme';
import {
  BLOCK_TYPE_LABEL,
  CHANNEL_LETTERS,
  NUM_CHANNELS,
  NUM_SCENES,
  isInsertSlot,
} from '../types/chain';
import type { ChainItem, SceneBlockState, ScenesState, ToneBlock } from '../types/chain';
import { channelUsed } from './channels';

/**
 * Scene Manager takeover (Fractal-style): one row per scene, one column per
 * block. Each cell is that block's state in that scene - bypass and channel
 * - editable in place without visiting the scene. The row head holds the
 * scene's number (click: switch to it; right-click: copy), name and level.
 *
 * The active scene's row IS the live chain (native reports it that way), so
 * editing it is the same as editing the blocks directly.
 */

export interface SceneManagerActions {
  selectScene: (index: number) => void;
  renameScene: (index: number, name: string) => void;
  setSceneLevel: (index: number, levelDb: number) => void;
  copyScene: (from: number, to: number) => void;
  setSceneBlockEnabled: (scene: number, blockId: string, enabled: boolean) => void;
  setSceneBlockChannel: (scene: number, blockId: string, channel: number) => void;
}

interface Column {
  block: ToneBlock;
  /** Type label (NAM/IR/...), prefixed L/R for a Dual Mono side. */
  label: string;
  title: string;
}

/** Every block a scene can address, in chain order; a Dual Mono block is
    followed by its sides' blocks. */
const sceneColumns = (items: ChainItem[], prefix = ''): Column[] =>
  items.flatMap((item): Column[] => {
    if (isInsertSlot(item)) return [];
    const own: Column = {
      block: item,
      label: `${prefix}${BLOCK_TYPE_LABEL[item.blockType]}`,
      title: item.blockType === 'dualMono' ? 'Dual Mono' : item.tone.title,
    };
    if (item.blockType !== 'dualMono') return [own];
    return [
      own,
      ...sceneColumns(item.dualLeft ?? [], 'L '),
      ...sceneColumns(item.dualRight ?? [], 'R '),
    ];
  });

const LEVEL_MIN = -24;
const LEVEL_MAX = 12;
const levelLabel = (db: number) => (db === 0 ? '0 dB' : `${db > 0 ? '+' : ''}${db} dB`);

const HEAD_W = 236;
const COL_MIN_W = 104;
const ROW_H = 58;

const SceneRowHead: React.FC<{
  index: number;
  active: boolean;
  name: string;
  level: number;
  names: string[];
  actions: SceneManagerActions;
}> = ({ index, active, name, level, names, actions }) => {
  const [draft, setDraft] = useState(name);
  useEffect(() => setDraft(name), [name]);
  const commit = () => {
    if (draft.trim() !== name) actions.renameScene(index, draft.trim());
  };
  const menu = useTileMenu();
  const stepLevel = (delta: number) =>
    actions.setSceneLevel(index, Math.max(LEVEL_MIN, Math.min(LEVEL_MAX, level + delta)));
  const stepButton = (delta: number) => (
    <button
      type="button"
      onClick={() => stepLevel(delta)}
      {...helpProps(HELP.sceneLevel)}
      style={{
        all: 'unset',
        cursor: 'pointer',
        width: '16rem',
        height: '18rem',
        display: 'grid',
        placeItems: 'center',
        color: MUTED,
      }}
    >
      {delta < 0 ? <Minus size={12} /> : <Plus size={12} />}
    </button>
  );

  return (
    <div
      style={{
        display: 'flex',
        alignItems: 'center',
        gap: '10rem',
        padding: '0 12rem',
        height: '100%',
        boxSizing: 'border-box',
        borderLeft: `3rem solid ${active ? BRAND_YELLOW : 'transparent'}`,
      }}
    >
      <button
        type="button"
        onClick={() => actions.selectScene(index)}
        onContextMenu={menu.openMenu}
        aria-pressed={active}
        {...helpProps(HELP.sceneButton)}
        style={{
          width: '30rem',
          height: '26rem',
          flexShrink: 0,
          padding: 0,
          borderRadius: '4rem',
          cursor: 'pointer',
          fontFamily: FONT_MONO,
          fontSize: '13rem',
          fontWeight: 600,
          color: active ? BLACK : WHITE,
          background: active ? BRAND_YELLOW : 'transparent',
          border: active ? `1rem solid ${BRAND_YELLOW}` : BORDER,
        }}
      >
        {index + 1}
      </button>
      <input
        className="t3k-touch-field"
        value={draft}
        maxLength={24}
        placeholder={`Scene ${index + 1}`}
        onChange={(e) => setDraft(e.target.value)}
        onBlur={commit}
        onKeyDown={(e) => {
          e.stopPropagation();
          if (e.key === 'Enter') (e.target as HTMLInputElement).blur();
          if (e.key === 'Escape') {
            setDraft(name);
            (e.target as HTMLInputElement).blur();
          }
        }}
        {...helpProps(HELP.sceneRename)}
        style={{
          flex: 1,
          minWidth: 0,
          height: '26rem',
          boxSizing: 'border-box',
          background: 'transparent',
          border: 'none',
          borderBottom: BORDER,
          color: WHITE,
          fontSize: '13rem',
          padding: '0 2rem',
          outline: 'none',
        }}
      />
      <div
        style={{ display: 'flex', alignItems: 'center', flexShrink: 0 }}
        {...helpProps(HELP.sceneLevel)}
      >
        {stepButton(-1)}
        <button
          type="button"
          onClick={() => actions.setSceneLevel(index, 0)}
          {...helpProps(`${HELP.sceneLevel} Click: back to 0 dB.`)}
          style={{
            all: 'unset',
            cursor: 'pointer',
            width: '46rem',
            textAlign: 'center',
            fontFamily: FONT_MONO,
            fontSize: '11rem',
            color: level === 0 ? SUBTLE : WHITE,
          }}
        >
          {levelLabel(level)}
        </button>
        {stepButton(1)}
      </div>
      {menu.menuAnchor && (
        <TileMenu
          anchor={menu.menuAnchor}
          onClose={menu.closeMenu}
          items={[
            {
              label: 'Copy To',
              icon: <Copy size={16} />,
              help: HELP.sceneCopy,
              submenu: Array.from({ length: NUM_SCENES }, (_, to) => to)
                .filter((to) => to !== index)
                .map((to) => ({
                  label: names[to] || `Scene ${to + 1}`,
                  icon: <span style={{ width: '16rem', fontFamily: FONT_MONO }}>{to + 1}</span>,
                  help: HELP.sceneCopy,
                  onSelect: () => actions.copyScene(index, to),
                })),
            },
          ]}
        />
      )}
    </div>
  );
};

const SceneCell: React.FC<{
  block: ToneBlock;
  state: SceneBlockState;
  onEnabled: (enabled: boolean) => void;
  onChannel: (channel: number) => void;
}> = ({ block, state, onEnabled, onChannel }) => (
  <div
    style={{
      display: 'flex',
      alignItems: 'center',
      justifyContent: 'center',
      gap: '6rem',
      height: '100%',
      opacity: state.enabled ? 1 : 0.55,
    }}
  >
    <ChromeIconButton
      tone="power"
      on={state.enabled}
      help={HELP.sceneCellPower}
      onClick={() => onEnabled(!state.enabled)}
    >
      <Power />
    </ChromeIconButton>
    <div style={{ display: 'flex', gap: '1rem' }}>
      {Array.from({ length: NUM_CHANNELS }, (_, c) => {
        const isActive = c === state.channel;
        return (
          <button
            key={c}
            type="button"
            onClick={() => onChannel(c)}
            {...helpProps(`Channel ${CHANNEL_LETTERS[c]} - ${HELP.sceneCellChannel}`)}
            style={{
              all: 'unset',
              cursor: 'pointer',
              width: '15rem',
              height: '20rem',
              display: 'grid',
              placeItems: 'center',
              borderRadius: '2rem',
              fontFamily: FONT_MONO,
              fontSize: '11rem',
              fontWeight: 600,
              color: isActive ? BLACK : channelUsed(block, c) ? WHITE : SUBTLE,
              background: isActive ? BRAND_YELLOW : SEGMENTED_TRACK,
            }}
          >
            {CHANNEL_LETTERS[c]}
          </button>
        );
      })}
    </div>
  </div>
);

export const SceneManager: React.FC<{
  chain: ChainItem[];
  scenes: ScenesState;
  actions: SceneManagerActions;
  onClose: () => void;
}> = ({ chain, scenes, actions, onClose }) => {
  const columns = sceneColumns(chain);

  // Optimistic cell edits, dropped whenever native reports new scenes.
  const [pending, setPending] = useState<Record<string, SceneBlockState>>({});
  useEffect(() => setPending({}), [scenes]);
  const cellState = (s: number, block: ToneBlock): SceneBlockState =>
    pending[`${s}:${block.blockId}`] ??
    scenes.blocks[s]?.[block.blockId] ?? {
      // Not captured by this scene yet: it takes the live state when
      // first selected.
      enabled: block.params.enabled,
      channel: block.params.channel ?? 0,
    };
  const edit = (s: number, block: ToneBlock, next: SceneBlockState) =>
    setPending((p) => ({ ...p, [`${s}:${block.blockId}`]: next }));

  // Esc closes - unless a menu/list or a text field has the key.
  const closeRef = useRef(onClose);
  closeRef.current = onClose;
  useEffect(() => {
    const onKey = (e: KeyboardEvent) => {
      if (e.key !== 'Escape' || isKeyboardOwned() || isTypingTarget(e.target)) return;
      e.preventDefault();
      closeRef.current();
    };
    window.addEventListener('keydown', onKey);
    return () => window.removeEventListener('keydown', onKey);
  }, []);

  const gridColumns = `${HEAD_W}rem repeat(${columns.length}, minmax(${COL_MIN_W}rem, 1fr))`;

  return (
    <div
      style={{
        position: 'relative',
        flex: 1,
        width: '100%',
        minHeight: 0,
        display: 'flex',
        flexDirection: 'column',
        backgroundColor: '#000000',
        padding: '16rem 24rem 20rem',
        boxSizing: 'border-box',
        gap: '12rem',
      }}
    >
      <div style={{ display: 'flex', alignItems: 'center', justifyContent: 'space-between' }}>
        <span
          {...helpProps(HELP.sceneManager)}
          style={{ fontFamily: FONT_MONO, fontSize: '13rem', color: WHITE, letterSpacing: '1rem' }}
        >
          SCENE MANAGER
        </span>
        <button
          onClick={onClose}
          aria-label="Close scene manager"
          style={{
            background: 'transparent',
            border: 'none',
            color: WHITE,
            cursor: 'pointer',
            display: 'flex',
            alignItems: 'center',
            padding: '4rem',
          }}
        >
          <XIcon size={20} />
        </button>
      </div>

      <div
        style={{
          flex: 1,
          minHeight: 0,
          overflow: 'auto',
          border: BORDER,
          borderRadius: '8rem',
          backgroundColor: SURFACE,
        }}
      >
        <div
          style={{
            display: 'grid',
            gridTemplateColumns: gridColumns,
            // Rows share the height (never below ROW_H; scrolls past that).
            gridTemplateRows: `44rem repeat(${NUM_SCENES}, minmax(${ROW_H}rem, 1fr))`,
            minWidth: 'min-content',
            minHeight: '100%',
          }}
        >
          {/* Header row: block columns. */}
          <div
            style={{
              height: '44rem',
              display: 'flex',
              alignItems: 'center',
              padding: '0 15rem',
              fontFamily: FONT_MONO,
              fontSize: '10rem',
              color: GRAY,
              borderBottom: BORDER,
              position: 'sticky',
              left: 0,
              backgroundColor: SURFACE,
              zIndex: 1,
            }}
          >
            SCENE
          </div>
          {columns.map((col) => (
            <div
              key={col.block.blockId}
              {...helpProps(`${col.label} · ${col.title}`)}
              style={{
                height: '44rem',
                display: 'flex',
                flexDirection: 'column',
                justifyContent: 'center',
                alignItems: 'center',
                gap: '3rem',
                padding: '0 8rem',
                borderBottom: BORDER,
                borderLeft: BORDER,
                minWidth: 0,
              }}
            >
              <span style={{ fontFamily: FONT_MONO, fontSize: '10rem', color: GRAY }}>
                {col.label}
              </span>
              <span
                style={{
                  fontSize: '11rem',
                  color: WHITE,
                  maxWidth: '100%',
                  overflow: 'hidden',
                  textOverflow: 'ellipsis',
                  whiteSpace: 'nowrap',
                }}
              >
                {col.title}
              </span>
            </div>
          ))}

          {Array.from({ length: NUM_SCENES }, (_, s) => (
            <React.Fragment key={s}>
              <div
                style={{
                  borderBottom: s < NUM_SCENES - 1 ? BORDER : undefined,
                  position: 'sticky',
                  left: 0,
                  backgroundColor: s === scenes.active ? '#1f1f10' : SURFACE,
                  zIndex: 1,
                }}
              >
                <SceneRowHead
                  index={s}
                  active={s === scenes.active}
                  name={scenes.names[s] ?? ''}
                  level={scenes.levels[s] ?? 0}
                  names={scenes.names}
                  actions={actions}
                />
              </div>
              {columns.map((col) => {
                const state = cellState(s, col.block);
                return (
                  <div
                    key={col.block.blockId}
                    style={{
                      borderBottom: s < NUM_SCENES - 1 ? BORDER : undefined,
                      borderLeft: BORDER,
                      backgroundColor: s === scenes.active ? '#16160c' : undefined,
                    }}
                  >
                    <SceneCell
                      block={col.block}
                      state={state}
                      onEnabled={(enabled) => {
                        edit(s, col.block, { ...state, enabled });
                        actions.setSceneBlockEnabled(s, col.block.blockId, enabled);
                      }}
                      onChannel={(channel) => {
                        edit(s, col.block, { ...state, channel });
                        actions.setSceneBlockChannel(s, col.block.blockId, channel);
                      }}
                    />
                  </div>
                );
              })}
            </React.Fragment>
          ))}
        </div>
        {columns.length === 0 && (
          <div style={{ padding: '24rem', color: MUTED, fontSize: '12rem' }}>
            Add blocks to the chain to set their bypass and channel per scene.
          </div>
        )}
      </div>
    </div>
  );
};
