/**
 * Pure key -> shortcut mapping for the global shortcuts (see
 * useGlobalShortcuts). No imports, so node --test can load it directly.
 */

/** What a key press means (pure, see resolveShortcut); null = not ours. */
export type ShortcutAction =
  | { kind: 'undo' }
  | { kind: 'redo' }
  | { kind: 'preset'; direction: 1 | -1 }
  | { kind: 'sceneStep'; direction: 1 | -1 }
  | { kind: 'scene'; index: number }
  | { kind: 'back' };

export interface ShortcutKey {
  code: string;
  key: string;
  metaKey: boolean;
  ctrlKey: boolean;
  altKey: boolean;
  shiftKey: boolean;
}

/** Key -> shortcut (layout-independent: physical codes, except Escape). */
export function resolveShortcut(e: ShortcutKey): ShortcutAction | null {
  const command = e.metaKey || e.ctrlKey;
  if (command && !e.altKey && e.code === 'KeyZ') return { kind: e.shiftKey ? 'redo' : 'undo' };
  if (command && !e.altKey && !e.shiftKey && e.code === 'KeyY') return { kind: 'redo' };
  if (command || e.altKey) return null; // every other modifier combo belongs to the host
  if (e.code === 'BracketLeft' || e.code === 'BracketRight') {
    const direction = e.code === 'BracketLeft' ? -1 : 1;
    return e.shiftKey ? { kind: 'preset', direction } : { kind: 'sceneStep', direction };
  }
  if (!e.shiftKey && /^Digit[1-4]$/.test(e.code))
    return { kind: 'scene', index: Number(e.code.slice(5)) - 1 };
  if (e.key === 'Escape') return { kind: 'back' };
  return null;
}
