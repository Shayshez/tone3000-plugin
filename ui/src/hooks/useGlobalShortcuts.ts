import { useEffect, useRef } from 'react';
import { isKeyboardOwned, isTypingTarget } from '../keyPassthrough';
import { resolveShortcut } from './shortcutKeys';

/**
 * Global keyboard shortcuts ("layer 3"), active while the plugin window has
 * keyboard focus and the Settings switch is on:
 *
 *   1-4        select scene           ⇧[ / ⇧]   previous / next scene
 *   [ / ]      previous / next preset
 *   ⌘Z / ⇧⌘Z   plugin undo / redo     (Ctrl on Windows/Linux; ⌘Y also redoes)
 *   Esc        back (block view -> gallery; the tuner and the Scene Manager
 *              handle their own Esc)
 *
 * Only keys handled here are consumed (preventDefault); every other combo
 * falls through to the host, so e.g. ⌘S still saves the DAW project. Open
 * lists/menus and text fields keep their keys (isKeyboardOwned /
 * isTypingTarget), as do keyboard drags.
 */

/** Esc targets: the most recently mounted open view steps back (a Dual
    Mono side's view opened inside its wrapper's goes first). */
const backHandlers: Array<{ current: () => void }> = [];

/** Register a view's "back" for Esc while it is mounted. */
export function useShortcutBack(handler: () => void): void {
  const ref = useRef(handler);
  ref.current = handler;
  useEffect(() => {
    backHandlers.push(ref);
    return () => {
      const index = backHandlers.indexOf(ref);
      if (index >= 0) backHandlers.splice(index, 1);
    };
  }, []);
}

export interface GlobalShortcutActions {
  undo: () => void;
  redo: () => void;
  presetStep: (direction: 1 | -1) => void;
  selectScene: (index: number) => void;
  sceneStep: (direction: 1 | -1) => void;
}

export function useGlobalShortcuts(enabled: boolean, actions: GlobalShortcutActions): void {
  const actionsRef = useRef(actions);
  actionsRef.current = actions;

  useEffect(() => {
    if (!enabled) return;
    const onKey = (e: KeyboardEvent) => {
      if (e.defaultPrevented || e.repeat) return;
      if (isKeyboardOwned() || isTypingTarget(e.target)) return;
      if (document.querySelector('[data-dnd-dragging]') != null) return;
      const action = resolveShortcut(e);
      if (action === null) return;
      if (action.kind === 'back' && backHandlers.length === 0) return;
      const a = actionsRef.current;
      switch (action.kind) {
        case 'undo':
          a.undo();
          break;
        case 'redo':
          a.redo();
          break;
        case 'preset':
          a.presetStep(action.direction);
          break;
        case 'sceneStep':
          a.sceneStep(action.direction);
          break;
        case 'scene':
          a.selectScene(action.index);
          break;
        case 'back':
          backHandlers[backHandlers.length - 1].current();
          break;
      }
      e.preventDefault();
      e.stopPropagation();
    };
    window.addEventListener('keydown', onKey);
    return () => window.removeEventListener('keydown', onKey);
  }, [enabled]);
}
