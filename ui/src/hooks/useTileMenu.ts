import { useCallback, useEffect, useRef, useState } from 'react';
import type React from 'react';
import type { TileMenuAnchor } from '../components/TileMenu';
import { IS_IOS } from './useUiScale';

/** How long a touch is held before the tile menu opens: the system's own
    long-press delay (same as useTouchHold). */
const LONG_PRESS_MS = 500;

/** Real-px drift that abandons the long press. Kept under the drag sensor's
    activation distance, so a press that starts travelling becomes a drag and
    never also a menu. */
const LONG_PRESS_SLOP_PX = 5;

/** Real px the menu drops below the touch point. The menu opens while the
    finger is still down, so the release, and the mouse pair WebKit replays
    at it, must land outside the menu or letting go would pick whatever row
    sat under the finger. Dropping the sheet also keeps it readable past the
    fingertip. */
const LONG_PRESS_MENU_DROP_PX = 24;

/** How long a set suppression stays valid. A ctrl-click's synthetic click
    follows within the same task, but the mouse pair WebKit replays after a
    touch can land much later or not at all, and an unconsumed flag must
    expire rather than swallow the next honest tap. Matches the replay
    window in helpText.ts. */
const SUPPRESS_CLICK_MS = 700;

/** Exported so ChainMapStrip's own "+" chip (a block's chain-map strip, not
    the gallery) can carry the identical right-click menu/long-press
    behavior instead of duplicating this iOS-long-press machinery. */
export const useTileMenu = () => {
  const [menuAnchor, setMenuAnchor] = useState<TileMenuAnchor | null>(null);
  // Deadline (performance.now ms) under which the next click is swallowed.
  const suppressClickUntilRef = useRef(0);
  const openMenu = useCallback((e: React.MouseEvent) => {
    e.preventDefault();
    e.stopPropagation();
    suppressClickUntilRef.current = performance.now() + SUPPRESS_CLICK_MS;
    // Viewport coords: TileMenu portals to body and positions with
    // position:fixed at these real-px coordinates.
    setMenuAnchor({ clientX: e.clientX, clientY: e.clientY });
  }, []);
  const closeMenu = useCallback(() => setMenuAnchor(null), []);

  // iOS only: WKWebView never delivers `contextmenu` for a long press, so a
  // touch hold has to open the sheet itself. Every other engine fires the
  // native event and lands in openMenu above.
  //
  // The menu fires on its own timer, like the system's, while the finger is
  // still down. Travel past the slop cancels it: past there the press is a
  // drag (the sensor's distance activation), never also a menu; if the
  // finger drags on after the menu already opened, the menu yields and the
  // drag proceeds. The release is watched on window in the capture phase,
  // because a drag that did start takes pointer capture and no pointerup
  // reaches the tile at all.
  const pressStart = useRef<{ x: number; y: number; id: number; fired: boolean } | null>(null);
  const holdTimer = useRef<number | undefined>(undefined);
  const releaseListener = useRef<((e: PointerEvent) => void) | null>(null);

  const cancelLongPress = useCallback(() => {
    pressStart.current = null;
    if (holdTimer.current !== undefined) window.clearTimeout(holdTimer.current);
    holdTimer.current = undefined;
    if (releaseListener.current) {
      window.removeEventListener('pointerup', releaseListener.current, true);
      window.removeEventListener('pointercancel', releaseListener.current, true);
      releaseListener.current = null;
    }
  }, []);

  // The release listener lives on window; a tile can unmount mid-press (undo,
  // a preset load), so drop it on unmount.
  useEffect(() => cancelLongPress, [cancelLongPress]);

  const longPressProps = {
    onPointerDown: (e: React.PointerEvent) => {
      if (!IS_IOS || e.pointerType !== 'touch') return;
      cancelLongPress();
      const start = { x: e.clientX, y: e.clientY, id: e.pointerId, fired: false };
      pressStart.current = start;

      holdTimer.current = window.setTimeout(() => {
        holdTimer.current = undefined;
        if (pressStart.current !== start) return;
        start.fired = true;
        // The release that follows can fire a click on the tile; swallow it
        // exactly as the ctrl-click path does.
        suppressClickUntilRef.current = performance.now() + SUPPRESS_CLICK_MS;
        setMenuAnchor({ clientX: start.x, clientY: start.y + LONG_PRESS_MENU_DROP_PX });
      }, LONG_PRESS_MS);

      const onRelease = (ev: PointerEvent) => {
        if (pressStart.current === start && ev.pointerId === start.id) cancelLongPress();
      };
      releaseListener.current = onRelease;
      window.addEventListener('pointerup', onRelease, true);
      window.addEventListener('pointercancel', onRelease, true);
    },
    onPointerMove: (e: React.PointerEvent) => {
      const start = pressStart.current;
      if (start == null || e.pointerId !== start.id) return;
      if (
        Math.abs(e.clientX - start.x) <= LONG_PRESS_SLOP_PX &&
        Math.abs(e.clientY - start.y) <= LONG_PRESS_SLOP_PX
      )
        return;
      if (start.fired) closeMenu();
      cancelLongPress();
    },
  };
  /** True when a tile click should be ignored (followed a contextmenu or a
      touch hold, is a modifier-click, or the menu is already open, in which
      case it closes). */
  const shouldIgnoreClick = useCallback(
    (e: React.MouseEvent) => {
      if (performance.now() < suppressClickUntilRef.current) {
        suppressClickUntilRef.current = 0;
        return true;
      }
      if (e.ctrlKey || e.metaKey) return true;
      if (menuAnchor) {
        closeMenu();
        return true;
      }
      return false;
    },
    [menuAnchor, closeMenu]
  );
  return { menuAnchor, openMenu, closeMenu, shouldIgnoreClick, longPressProps };
};
