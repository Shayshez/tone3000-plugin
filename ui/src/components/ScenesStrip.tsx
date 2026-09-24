import React, { useEffect, useRef, useState } from 'react';
import { useTileMenu } from '../hooks/useTileMenu';
import { useMidiMenuItems } from '../hooks/useMidiLearn';
import { TileMenu } from './TileMenu';
import type { TileMenuItem } from './TileMenu';
import { Copy, LayoutGrid, Pencil } from './icons';
import { HELP, helpProps } from './helpText';
import {
  BLACK,
  BORDER,
  BRAND_YELLOW,
  FONT_MONO,
  GRAY,
  KNOB_LABEL_GAP,
  KNOB_SIZE_PRIMARY,
  MUTED,
  SUBTLE,
  WHITE,
} from './theme';
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
/** Knob label metrics (KnobControl's LABEL_SIZE and its label slot). */
const LABEL_SIZE = 14;
const LABEL_ROW_H = Math.round(LABEL_SIZE * 1.2);

const sceneLabel = (names: string[], i: number) => names[i] || `Scene ${i + 1}`;

export const ScenesStrip: React.FC<{
  scenes: ScenesState;
  onSelect: (index: number) => void;
  onRename: (index: number, name: string) => void;
  onCopy: (from: number, to: number) => void;
  /** Open/close the Scene Manager. */
  onToggleManager: () => void;
  managerOpen: boolean;
}> = ({ scenes, onSelect, onRename, onCopy, onToggleManager, managerOpen }) => {
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
      label: managerOpen ? 'Close Scene Manager' : 'Scene Manager',
      icon: <LayoutGrid size={16} />,
      help: HELP.sceneManager,
      onSelect: onToggleManager,
    },
  ];

  return (
    <div
      style={{
        display: 'flex',
        flexDirection: 'column',
        alignItems: 'stretch',
        // Same geometry as a faceplate knob: the switches sit centered on
        // the knobs' center line, the name sits on the knob-label line.
        gap: `${KNOB_LABEL_GAP}rem`,
        width: `${BTN_W * 5 + GAP * 4}rem`,
      }}
    >
      <div
        style={{
          height: `${KNOB_SIZE_PRIMARY}rem`,
          display: 'flex',
          alignItems: 'center',
          justifyContent: 'center',
        }}
      >
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
            onClick={onToggleManager}
            aria-pressed={managerOpen}
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
              color: managerOpen ? BLACK : WHITE,
              background: managerOpen ? WHITE : 'transparent',
              border: BORDER,
            }}
          >
            <LayoutGrid size={13} />
          </button>
        </div>
      </div>
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
            height: `${LABEL_ROW_H}rem`,
            boxSizing: 'border-box',
            background: '#0a0a0a',
            border: BORDER,
            borderRadius: '4rem',
            color: WHITE,
            fontSize: '12rem',
            textAlign: 'center',
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
            height: `${LABEL_ROW_H}rem`,
            display: 'flex',
            alignItems: 'center',
            justifyContent: 'center',
            gap: '5rem',
            fontSize: `${LABEL_SIZE}rem`,
            color: GRAY,
            whiteSpace: 'nowrap',
            overflow: 'hidden',
          }}
        >
          <span style={{ overflow: 'hidden', textOverflow: 'ellipsis' }}>
            {sceneLabel(scenes.names, active)}
          </span>
          <span style={{ color: SUBTLE, fontSize: '10rem' }}>▾</span>
        </button>
      )}
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
