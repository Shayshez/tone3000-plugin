import React, { useEffect, useRef, useState } from 'react';
import { useTileMenu } from '../hooks/useTileMenu';
import { useMidiMenuItems } from '../hooks/useMidiLearn';
import { TileMenu } from './TileMenu';
import type { TileMenuItem } from './TileMenu';
import { Copy, LayoutGrid, Pencil } from './icons';
import { HELP, helpProps } from './helpText';
import { BLACK, BORDER, BRAND_YELLOW, FONT_MONO, GRAY, MUTED, WHITE } from './theme';
import { NUM_SCENES } from '../types/chain';
import type { ScenesState } from '../types/chain';

/**
 * Faceplate scene selector (see ScenesState): the active scene's name on top
 * (click: pick a scene from a list, or open the Scene Manager), and four
 * numbered switches in a row - the one-hand, mouse-first way to flip scenes
 * while playing - plus the Scene Manager button. Right-click a number for
 * Rename / Copy To / MIDI Learn; levels live in the Scene Manager.
 *
 * Active scene is optimistic: the pressed number lights at once, native
 * converges through the chain resync.
 */

const BTN_W = 26;
const BTN_H = 22;
const GAP = 4;

const sceneLabel = (names: string[], i: number) => names[i] || `Scene ${i + 1}`;
const levelLabel = (db: number) => (db === 0 ? '0 dB' : `${db > 0 ? '+' : ''}${db} dB`);

export const ScenesStrip: React.FC<{
  scenes: ScenesState;
  onSelect: (index: number) => void;
  onRename: (index: number, name: string) => void;
  onCopy: (from: number, to: number) => void;
  onOpenManager: () => void;
}> = ({ scenes, onSelect, onRename, onCopy, onOpenManager }) => {
  const [active, setActive] = useState(scenes.active);
  useEffect(() => setActive(scenes.active), [scenes.active]);
  const select = (i: number) => {
    setActive(i);
    onSelect(i);
  };

  // Inline rename (no browser dialogs inside a plugin webview).
  const [renaming, setRenaming] = useState<number | null>(null);
  const [draft, setDraft] = useState('');
  const inputRef = useRef<HTMLInputElement>(null);
  useEffect(() => {
    if (renaming !== null) inputRef.current?.select();
  }, [renaming]);
  const startRename = (i: number) => {
    setDraft(scenes.names[i] ?? '');
    setRenaming(i);
  };
  const commitRename = () => {
    if (renaming !== null) onRename(renaming, draft.trim());
    setRenaming(null);
  };

  const nameMenu = useTileMenu();
  const numberMenu = useTileMenu();
  const [menuScene, setMenuScene] = useState(0);
  const midiItems = useMidiMenuItems(`scene${menuScene + 1}`);

  const numberMenuItems = (i: number): TileMenuItem[] => [
    {
      label: 'Rename',
      icon: <Pencil size={16} />,
      help: HELP.sceneRename,
      onSelect: () => startRename(i),
    },
    {
      label: 'Copy To',
      icon: <Copy size={16} />,
      help: HELP.sceneCopy,
      submenu: Array.from({ length: NUM_SCENES }, (_, to) => to)
        .filter((to) => to !== i)
        .map((to) => ({
          label: sceneLabel(scenes.names, to),
          icon: <span style={{ width: '16rem', fontFamily: FONT_MONO }}>{to + 1}</span>,
          help: HELP.sceneCopy,
          onSelect: () => onCopy(i, to),
        })),
    },
    ...midiItems,
  ];

  const nameMenuItems: TileMenuItem[] = [
    ...Array.from({ length: NUM_SCENES }, (_, i) => ({
      label: sceneLabel(scenes.names, i),
      icon: (
        <span
          style={{ width: '16rem', fontFamily: FONT_MONO, color: i === active ? WHITE : MUTED }}
        >
          {i + 1}
        </span>
      ),
      help: HELP.sceneSelect,
      onSelect: () => select(i),
    })),
    {
      label: 'Rename',
      icon: <Pencil size={16} />,
      help: HELP.sceneRename,
      onSelect: () => startRename(active),
    },
    {
      label: 'Scene Manager',
      icon: <LayoutGrid size={16} />,
      help: HELP.sceneManager,
      onSelect: onOpenManager,
    },
  ];

  const level = scenes.levels[active] ?? 0;

  return (
    <div
      style={{
        display: 'flex',
        flexDirection: 'column',
        alignItems: 'stretch',
        gap: '5rem',
        width: `${BTN_W * 5 + GAP * 4}rem`,
      }}
    >
      {renaming !== null ? (
        <input
          ref={inputRef}
          className="t3k-touch-field"
          value={draft}
          maxLength={24}
          placeholder={`Scene ${renaming + 1}`}
          onChange={(e) => setDraft(e.target.value)}
          onBlur={commitRename}
          onKeyDown={(e) => {
            e.stopPropagation();
            if (e.key === 'Enter') commitRename();
            if (e.key === 'Escape') setRenaming(null);
          }}
          style={{
            height: '18rem',
            boxSizing: 'border-box',
            background: '#0a0a0a',
            border: BORDER,
            borderRadius: '4rem',
            color: WHITE,
            fontSize: '11rem',
            padding: '0 6rem',
            outline: 'none',
          }}
        />
      ) : (
        <button
          type="button"
          onClick={nameMenu.openMenu}
          onContextMenu={nameMenu.openMenu}
          {...helpProps(HELP.sceneName)}
          style={{
            all: 'unset',
            cursor: 'pointer',
            height: '18rem',
            display: 'flex',
            alignItems: 'center',
            gap: '5rem',
            fontSize: '11rem',
            color: WHITE,
            whiteSpace: 'nowrap',
            overflow: 'hidden',
          }}
        >
          <span style={{ color: GRAY, fontFamily: FONT_MONO, fontSize: '10rem' }}>SCENE</span>
          <span style={{ overflow: 'hidden', textOverflow: 'ellipsis' }}>
            {sceneLabel(scenes.names, active)}
          </span>
          {level !== 0 && (
            <span style={{ color: MUTED, fontSize: '10rem' }}>{levelLabel(level)}</span>
          )}
          <span style={{ color: MUTED, fontSize: '9rem' }}>▾</span>
        </button>
      )}
      <div
        style={{
          display: 'grid',
          gridTemplateColumns: `repeat(5, ${BTN_W}rem)`,
          gap: `${GAP}rem`,
        }}
      >
        {Array.from({ length: NUM_SCENES }, (_, i) => {
          const isActive = i === active;
          return (
            <button
              key={i}
              type="button"
              onClick={() => select(i)}
              onContextMenu={(e) => {
                setMenuScene(i);
                numberMenu.openMenu(e);
              }}
              aria-pressed={isActive}
              {...helpProps(`${sceneLabel(scenes.names, i)} - ${HELP.sceneButton}`)}
              style={{
                width: `${BTN_W}rem`,
                height: `${BTN_H}rem`,
                padding: 0,
                borderRadius: '4rem',
                cursor: 'pointer',
                fontFamily: FONT_MONO,
                fontSize: '11rem',
                fontWeight: 600,
                color: isActive ? BLACK : scenes.names[i] ? WHITE : MUTED,
                background: isActive ? BRAND_YELLOW : 'transparent',
                border: isActive ? `1rem solid ${BRAND_YELLOW}` : BORDER,
              }}
            >
              {i + 1}
            </button>
          );
        })}
        <button
          type="button"
          onClick={onOpenManager}
          aria-label="Scene Manager"
          {...helpProps(HELP.sceneManager)}
          style={{
            width: `${BTN_W}rem`,
            height: `${BTN_H}rem`,
            padding: 0,
            borderRadius: '4rem',
            cursor: 'pointer',
            display: 'grid',
            placeItems: 'center',
            color: WHITE,
            background: 'transparent',
            border: BORDER,
          }}
        >
          <LayoutGrid size={13} />
        </button>
      </div>
      {nameMenu.menuAnchor && (
        <TileMenu anchor={nameMenu.menuAnchor} onClose={nameMenu.closeMenu} items={nameMenuItems} />
      )}
      {numberMenu.menuAnchor && (
        <TileMenu
          anchor={numberMenu.menuAnchor}
          onClose={numberMenu.closeMenu}
          items={numberMenuItems(menuScene)}
        />
      )}
    </div>
  );
};
