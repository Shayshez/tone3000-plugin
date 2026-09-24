import React, { useEffect, useLayoutEffect, useRef, useState } from 'react';
import { createPortal } from 'react-dom';
import { helpProps } from './helpText';
import { useDismissable } from '../hooks/useDismissable';
import { BORDER, DISABLED_OPACITY, HIGHLIGHT, MUTED, WHITE } from './theme';

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
    regardless of UI scale. */
const SubmenuFlyout: React.FC<{ items: TileMenuItem[]; onCommit: () => void }> = ({
  items,
  onCommit,
}) => {
  const ref = useRef<HTMLDivElement>(null);
  const [openLeft, setOpenLeft] = useState(false);

  useLayoutEffect(() => {
    const rect = ref.current?.getBoundingClientRect();
    if (rect && rect.right > window.innerWidth) setOpenLeft(true);
  }, []);

  return (
    <div
      ref={ref}
      style={{
        position: 'absolute',
        top: `-${PANEL_PADDING}rem`,
        ...(openLeft
          ? { right: `calc(100% + ${SUBMENU_OFFSET_X}rem)` }
          : { left: `calc(100% + ${SUBMENU_OFFSET_X}rem)` }),
      }}
    >
      <MenuPanel items={items} onCommit={onCommit} style={{}} />
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
}> = ({ items, onCommit, style }) => {
  const [openSubmenu, setOpenSubmenu] = useState<number | null>(null);

  return (
    <div
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
          onMouseEnter={() => setOpenSubmenu(item.submenu ? index : null)}
        >
          <button
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
            <SubmenuFlyout items={item.submenu} onCommit={onCommit} />
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
      <style>{`.tile-menu-item:hover:not(:disabled) { background-color: ${HIGHLIGHT}; }`}</style>
      <MenuPanel items={items} onCommit={onClose} style={{}} />
    </div>,
    document.body
  );
};
