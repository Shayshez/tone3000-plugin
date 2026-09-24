import React, { useEffect, useLayoutEffect, useRef, useState } from 'react';
import { createPortal } from 'react-dom';
import { helpProps } from './helpText';
import { useDismissable } from '../hooks/useDismissable';
import { BORDER, DISABLED_OPACITY, HIGHLIGHT, MUTED, WHITE } from './theme';
import { KEYBOARD_OWNER_ATTR } from '../keyPassthrough';

/**
 * Right-click action sheet for gallery tiles, in the house floating-panel
 * style (see the faceplate's input-mode menu): #141416 panel, hairline
 * border, 14px radius, icon + label rows with the shared hover highlight.
 *
 * Portaled to document.body with position:fixed at the click's viewport
 * coords (numeric left/top = real px; the rem-denominated sizes scale with
 * the UI like everything else). Dismissed on outside press, Escape, or
 * picking a row.
 *
 * Rows can carry a `submenu` (see addTileMenuItems in GalleryBlock.tsx: a
 * block-type row like Cab/IR opens its own Load File/Load Folder choice) -
 * hovering such a row flies out a nested panel to its right, one level deep.
 * A row with a submenu has no `onSelect` of its own; only leaf rows commit.
 */

export interface TileMenuItem {
  label: string;
  icon: React.ReactNode;
  /** One-line hint for the faceplate help readout. */
  help: string;
  disabled?: boolean;
  /** Leaf row: fires on click, then closes the whole sheet. Omit when
      `submenu` is set - a parent row only opens its flyout, it never
      commits anything itself. */
  onSelect?: () => void;
  /** Nested rows shown in a flyout to this row's right while it's
      hovered. */
  submenu?: TileMenuItem[];
}

/** Click point in viewport (client) coordinates. */
export interface TileMenuAnchor {
  clientX: number;
  clientY: number;
}

const MENU_WIDTH = 148;
const PANEL_PADDING = 6;
/** Visual-px nudge so the panel's top-left sits clearly past the cursor tip. */
const CURSOR_OFFSET = 6;
/** Real-px breathing room kept between the panel and the window edges. */
const VIEWPORT_MARGIN = 8;
/** Flyout submenu's own nudge past its parent row's right edge, and up past
    the panel's own top padding so a flyout opened from the first row lines
    up with it rather than sitting a few px low. */
const SUBMENU_OFFSET_X = 4;

const rowStyle = (disabled?: boolean): React.CSSProperties => ({
  display: 'flex',
  alignItems: 'center',
  gap: '12rem',
  width: '100%',
  padding: '9rem 12rem',
  background: 'transparent',
  border: 'none',
  borderRadius: '8rem',
  color: disabled ? MUTED : WHITE,
  opacity: disabled ? DISABLED_OPACITY : 1,
  fontSize: '13rem',
  fontWeight: 400,
  textAlign: 'left',
  cursor: disabled ? 'not-allowed' : 'pointer',
  whiteSpace: 'nowrap',
  boxSizing: 'border-box',
});

/** A submenu flyout off its parent row: opens to the right by default, but
    flips to the left when that would run the panel past the viewport edge
    (the tile menu can open anywhere along the chain, including near the
    right edge of the plugin window). Measured after mount
    (useLayoutEffect, before paint) against the flyout's own rendered rect
    rather than computed from rem/px conversions, so it's correct
    regardless of UI scale. Vertically it slides up just enough to stay
    inside the window when its parent row sits low (never above the top
    margin). */
const SubmenuFlyout: React.FC<{
  items: TileMenuItem[];
  onCommit: () => void;
  /** Opened from the keyboard (→): focus its first row. */
  autoFocus: boolean;
  /** ← from inside: close and return focus to the parent row. */
  onBack: () => void;
}> = ({ items, onCommit, autoFocus, onBack }) => {
  const ref = useRef<HTMLDivElement>(null);
  const [openLeft, setOpenLeft] = useState(false);
  /** Real px the flyout moves up to fit the window. */
  const [liftPx, setLiftPx] = useState(0);

  useLayoutEffect(() => {
    const rect = ref.current?.getBoundingClientRect();
    if (!rect) return;
    if (rect.right > window.innerWidth) setOpenLeft(true);
    const overflow = rect.bottom - (window.innerHeight - VIEWPORT_MARGIN);
    if (overflow > 0) setLiftPx(Math.max(0, Math.min(overflow, rect.top - VIEWPORT_MARGIN)));
  }, []);

  return (
    <div
      ref={ref}
      style={{
        position: 'absolute',
        top: `calc(-${PANEL_PADDING}rem - ${liftPx}px)`,
        ...(openLeft
          ? { right: `calc(100% + ${SUBMENU_OFFSET_X}rem)` }
          : { left: `calc(100% + ${SUBMENU_OFFSET_X}rem)` }),
      }}
    >
      <MenuPanel
        items={items}
        onCommit={onCommit}
        style={{}}
        autoFocus={autoFocus}
        onBack={onBack}
      />
    </div>
  );
};

/** The panel's own visual chrome, shared by the root sheet and every
    flyout submenu - only the positioning (fixed at the cursor vs. absolute
    off a parent row) differs between them. */
const MenuPanel: React.FC<{
  items: TileMenuItem[];
  onCommit: () => void;
  style: React.CSSProperties;
  /** Focus the first enabled row on mount (root sheet, keyboard flyouts). */
  autoFocus?: boolean;
  /** Set on a flyout: ← closes it back to the parent row. */
  onBack?: () => void;
}> = ({ items, onCommit, style, autoFocus = false, onBack }) => {
  const [openSubmenu, setOpenSubmenu] = useState<number | null>(null);
  const [submenuByKey, setSubmenuByKey] = useState(false);
  const rowRefs = useRef<(HTMLButtonElement | null)[]>([]);

  const focusRow = (from: number, dir: 1 | -1) => {
    for (let step = 1; step <= items.length; step++) {
      const i = (from + dir * step + items.length * step) % items.length;
      if (!items[i]?.disabled && rowRefs.current[i]) {
        rowRefs.current[i]?.focus();
        return;
      }
    }
  };

  useEffect(() => {
    if (autoFocus) focusRow(-1, 1);
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, []);

  // Keyboard: ↑/↓ cycle enabled rows, → opens a row's flyout (focusing its
  // first row), ← closes this flyout back to its parent row; Enter/Space
  // activate the focused row natively (they're real buttons); Esc closes
  // the whole sheet (useDismissable). stopPropagation keeps a flyout's keys
  // from also moving the parent panel.
  const onKeyDown = (e: React.KeyboardEvent) => {
    const current = rowRefs.current.findIndex((el) => el === document.activeElement);
    if (e.key === 'ArrowDown' || e.key === 'ArrowUp') {
      focusRow(current, e.key === 'ArrowDown' ? 1 : -1);
    } else if (e.key === 'ArrowRight' && current >= 0 && items[current]?.submenu) {
      setSubmenuByKey(true);
      setOpenSubmenu(current);
    } else if (e.key === 'ArrowLeft' && onBack) {
      onBack();
    } else return;
    e.preventDefault();
    e.stopPropagation();
  };

  return (
    <div
      onKeyDown={onKeyDown}
      style={{
        width: `${MENU_WIDTH}rem`,
        backgroundColor: '#141416',
        border: BORDER,
        borderRadius: '14rem',
        padding: `${PANEL_PADDING}rem`,
        boxSizing: 'border-box',
        ...style,
      }}
    >
      {items.map((item, index) => (
        <div
          key={item.label}
          style={{ position: 'relative' }}
          onMouseEnter={() => {
            setSubmenuByKey(false);
            setOpenSubmenu(item.submenu ? index : null);
          }}
        >
          <button
            ref={(el) => {
              rowRefs.current[index] = el;
            }}
            type="button"
            className="tile-menu-item"
            disabled={item.disabled}
            {...helpProps(item.help)}
            onClick={() => {
              if (item.submenu) return;
              onCommit();
              item.onSelect?.();
            }}
            style={rowStyle(item.disabled)}
          >
            {item.icon}
            {item.label}
          </button>
          {item.submenu && openSubmenu === index && (
            <SubmenuFlyout
              items={item.submenu}
              onCommit={onCommit}
              autoFocus={submenuByKey}
              onBack={() => {
                setOpenSubmenu(null);
                rowRefs.current[index]?.focus();
              }}
            />
          )}
        </div>
      ))}
    </div>
  );
};

export const TileMenu: React.FC<{
  anchor: TileMenuAnchor;
  items: TileMenuItem[];
  onClose: () => void;
}> = ({ anchor, items, onClose }) => {
  const rootRef = useRef<HTMLDivElement>(null);
  const [pos, setPos] = useState({
    left: anchor.clientX + CURSOR_OFFSET,
    top: anchor.clientY + CURSOR_OFFSET,
  });

  useDismissable(true, rootRef, onClose);

  // Keep the panel inside the window (long menus - a tone tile's - opened
  // near the bottom or right edge would otherwise run off it): measured
  // before paint, so it never visibly jumps. Past the bottom it slides up;
  // past the right edge it opens to the cursor's left instead.
  useLayoutEffect(() => {
    const rect = rootRef.current?.getBoundingClientRect();
    if (!rect) return;
    const margin = VIEWPORT_MARGIN;
    let { left, top } = {
      left: anchor.clientX + CURSOR_OFFSET,
      top: anchor.clientY + CURSOR_OFFSET,
    };
    if (top + rect.height > window.innerHeight - margin)
      top = Math.max(margin, window.innerHeight - margin - rect.height);
    if (left + rect.width > window.innerWidth - margin)
      left = Math.max(margin, anchor.clientX - CURSOR_OFFSET - rect.width);
    setPos({ left, top });
  }, [anchor]);

  // A resize reflows the content under the fixed menu: just dismiss;
  // keeping it at the old client point would look wrong anyway.
  useEffect(() => {
    window.addEventListener('resize', onClose);
    return () => window.removeEventListener('resize', onClose);
  }, [onClose]);

  return createPortal(
    <div
      ref={rootRef}
      {...{ [KEYBOARD_OWNER_ATTR]: '' }}
      // Keep every gesture inside the panel: clicks must not open the tile's
      // detail view, presses must not arm a drag under the menu.
      onClick={(e) => e.stopPropagation()}
      onPointerDown={(e) => e.stopPropagation()}
      onContextMenu={(e) => {
        e.preventDefault();
        e.stopPropagation();
      }}
      style={{
        // Fixed to the viewport at the click point: pointer coords are real
        // px, so left/top stay numeric (px), never rem.
        position: 'fixed',
        left: pos.left,
        top: pos.top,
        zIndex: 1000,
      }}
    >
      <style>{`.tile-menu-item:hover:not(:disabled), .tile-menu-item:focus-visible { background-color: ${HIGHLIGHT}; outline: none; }`}</style>
      <MenuPanel items={items} onCommit={onClose} style={{}} autoFocus />
    </div>,
    document.body
  );
};
