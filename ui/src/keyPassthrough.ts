import * as Juce from '@juce-framework/webview';
import { isNativeFunctionRegistered } from './backend/JuceBackend';
import { keyboardShortcutsEnabled } from './components/uiPreferences';

/**
 * Space and Enter passthrough to the host DAW.
 *
 * The webview owns keyboard focus once the user interacts with the plugin,
 * so the host's transport keys, Space (play/stop) and Enter (return to
 * start / stop, host-dependent), land in web content and either beep or
 * scroll instead of reaching the transport. Any press outside an editable
 * or interactive element is swallowed here and handed to the host via
 * `forwardKeyToHost` (plugin builds; in the dev browser the suppression
 * alone stops the beep/scroll).
 *
 * Interactive includes the chain tiles: dnd-kit marks them role="button",
 * and Space or Enter on a focused tile starts a keyboard drag (intentional,
 * since it requires tabbing to the tile first). Either key with focus
 * anywhere else still reaches the transport.
 *
 * Installed only on the plugin UI's own origin: the OAuth flows navigate
 * this webview to remote tone3000.com pages, which load without the UI
 * bundle, so they keep full keyboard behavior (the site's search box
 * needs Enter).
 */
/** Marks an open element that handles its own keys (see isKeyboardOwned). */
export const KEYBOARD_OWNER_ATTR = 'data-keyboard-owner';

/** True while some open list/menu owns the keyboard: global shortcuts and
    hover shortcuts stand down so one key never does two things. */
export const isKeyboardOwned = (): boolean =>
  document.querySelector(`[${KEYBOARD_OWNER_ATTR}]`) != null;

/** Keys typed into text fields always belong to the field. */
export const isTypingTarget = (target: EventTarget | null): boolean =>
  target instanceof Element && target.closest('input, textarea, select, [contenteditable]') != null;

export function installKeyPassthrough(): void {
  window.addEventListener(
    'keydown',
    (e) => {
      if (e.code !== 'Space' && e.code !== 'Enter') return;
      // An open keyboard-driven list (model dropdown, menus) owns Enter to
      // commit its highlighted row; Space still reaches the transport so
      // playback can be started/stopped while browsing.
      if (e.code === 'Enter' && document.querySelector(`[${KEYBOARD_OWNER_ATTR}]`) != null) return;
      // Editable and interactive elements keep the key (typing in preset
      // names/search, Enter committing a value edit, activating a focused
      // button).
      if (
        e.target instanceof Element &&
        e.target.closest(
          'input, textarea, select, button, a[href], [contenteditable], [role="button"]'
        )
      ) {
        return;
      }
      // Leave keyboard drags alone (Space/Enter/Escape end them; a focus
      // change mid-gesture would cancel the drag).
      if (document.querySelector('[data-dnd-dragging]') != null) return;
      // Always suppress: stops the caret scroll / system beep even where
      // there is no host to forward to (standalone, dev browser).
      e.preventDefault();
      // Forward the initial press only; stragglers must not spam the
      // transport. With global shortcuts on, the native side hands focus
      // straight back to the plugin afterwards (keepFocus), so scene keys
      // keep working during playback instead of landing on DAW commands
      // (Logic: 1-9 recall screensets); otherwise it leaves focus with the
      // host, and genuine repeats go there directly.
      if (e.repeat) return;
      if (isNativeFunctionRegistered('forwardKeyToHost'))
        void Juce.getNativeFunction('forwardKeyToHost')(e.code, keyboardShortcutsEnabled());
    },
    { capture: true }
  );
}
