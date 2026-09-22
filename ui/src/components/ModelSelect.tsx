import React, { useState, useRef, useCallback, useEffect, useLayoutEffect } from 'react';
import { createPortal } from 'react-dom';
import { ChevronLeft, ChevronRight, FolderClosed } from './icons';
import { useDismissable } from '../hooks/useDismissable';
import { LoadingDots } from './LoadingDots';
import { DISABLED_OPACITY } from './theme';
import { getUiScale } from '../hooks/useUiScale';

interface Option {
  id: string;
  name: string;
}

/** One dropdown row: 12px vertical padding ×2 + ~17px text line. */
const OPTION_ROW_HEIGHT = 41;
/** The dropdown shows at most this many options before scrolling. */
const MAX_VISIBLE_OPTIONS = 5;

interface ModelSelectProps {
  options: Option[];
  value: string;
  onChange: (id: string) => void;
  /** The dropdown just opened. Lets the owner retry a failed catalog fetch
      (the `loading` dots row covers the retry while it runs). */
  onOpen?: () => void;
  height?: number;
  /** Grays out and blocks all interaction (e.g. signed out; switching
      models needs an authenticated native download). */
  disabled?: boolean;
  /** The catalog fetch is in flight (renders a dots row in the dropdown). */
  loading?: boolean;
  /** Catalog total for the "n/N" display. */
  totalCount: number;
}

export const ModelSelect: React.FC<ModelSelectProps> = ({
  options,
  value,
  onChange,
  onOpen,
  height = 46,
  disabled = false,
  loading = false,
  totalCount,
}) => {
  const [isOpen, setIsOpen] = useState(false);
  const containerRef = useRef<HTMLDivElement>(null);
  const dropdownRef = useRef<HTMLDivElement>(null);
  const activeOptionRef = useRef<HTMLDivElement | null>(null);

  // Fixed-position (viewport px), computed fresh each open: the trigger can
  // sit anywhere in the card - a Dual Mono side card in particular has far
  // less headroom above it than an ordinary block's bottom-anchored picker
  // (see the call site's own "opens upward" comment) - so a dropdown that
  // always opens up and always assumes 5 rows' worth of clearance can run
  // past the top of the window with nothing below it to scroll to (issue:
  // the top options become permanently unreachable). Flips direction and
  // clamps to whichever side actually has more room, in real (unscaled) px
  // since position:fixed coordinates are viewport-relative regardless of the
  // rem-per-design-px UI scale.
  const [dropdownStyle, setDropdownStyle] = useState<React.CSSProperties | null>(null);

  useLayoutEffect(() => {
    if (!isOpen || !containerRef.current) {
      setDropdownStyle(null);
      return;
    }
    const scale = getUiScale();
    const rowPx = OPTION_ROW_HEIGHT * scale;
    const fullListPx =
      (MAX_VISIBLE_OPTIONS * OPTION_ROW_HEIGHT + (MAX_VISIBLE_OPTIONS - 1)) * scale;
    const marginPx = 4 * scale;

    const place = () => {
      if (!containerRef.current) return;
      const rect = containerRef.current.getBoundingClientRect();
      const spaceAbove = rect.top - marginPx;
      const spaceBelow = window.innerHeight - rect.bottom - marginPx;
      // Prefer up (matches every call site's own card layout) whenever it
      // can fit the whole list or simply has more room than down; only
      // flip when down is genuinely roomier - a tight-but-usable "up" stays
      // put rather than flipping over a marginal difference.
      const openUp = spaceAbove >= fullListPx || spaceAbove >= spaceBelow;
      const available = Math.max(rowPx, openUp ? spaceAbove : spaceBelow);
      setDropdownStyle({
        position: 'fixed',
        left: rect.left,
        width: rect.width,
        maxHeight: Math.min(fullListPx, available),
        ...(openUp
          ? { bottom: window.innerHeight - rect.top + marginPx }
          : { top: rect.bottom + marginPx }),
      });
    };

    place();
    // A scroll (now that the card itself can scroll - see the "bottom gets
    // cut off" fix) or a window resize both invalidate the trigger's
    // measured rect; just close rather than track it live, same as every
    // other floating panel in this app (see TileMenu's own resize handler).
    // Scrolling the option list itself is exempt - that's a scroll event
    // too (capture:true sees it), but it isn't the trigger moving.
    const onInvalidate = (e: Event) => {
      if (
        dropdownRef.current &&
        e.target instanceof Node &&
        dropdownRef.current.contains(e.target)
      ) {
        return;
      }
      setIsOpen(false);
    };
    window.addEventListener('resize', onInvalidate);
    document.addEventListener('scroll', onInvalidate, true);
    return () => {
      window.removeEventListener('resize', onInvalidate);
      document.removeEventListener('scroll', onInvalidate, true);
    };
  }, [isOpen, options]);

  const currentIndex = options.findIndex((opt) => opt.id === value);
  const selectedOption = options[currentIndex];

  // Land on the loaded item instead of the top of the list (issue #85) - in
  // a tone pack with dozens/hundreds of files, always opening at the top
  // means scrolling to find where you already are. Keyed on `options` too,
  // not just `isOpen`: the catalog fetch (see ChainBlock.tsx's
  // handleModelsOpen) resolves *after* the dropdown opens, so the first
  // render only has the stored single-item fallback - this re-fires once
  // the real list lands and the active row actually exists to scroll to.
  // Also keyed on `dropdownStyle`: the portaled list itself (and so
  // activeOptionRef) doesn't exist yet on the render where `isOpen` first
  // flips true - it only mounts once the position effect above computes a
  // style, one render later - so this must re-fire on that render too, or
  // it fires early against a still-empty ref and silently does nothing.
  useEffect(() => {
    if (isOpen) activeOptionRef.current?.scrollIntoView({ block: 'nearest' });
  }, [isOpen, options, dropdownStyle]);

  const handlePrev = (e: React.MouseEvent) => {
    e.stopPropagation();
    if (currentIndex > 0) {
      onChange(options[currentIndex - 1].id);
    }
  };

  const handleNext = (e: React.MouseEvent) => {
    e.stopPropagation();
    if (currentIndex < options.length - 1) {
      onChange(options[currentIndex + 1].id);
    }
  };

  const handleSelect = (id: string) => {
    onChange(id);
    setIsOpen(false);
  };

  const close = useCallback(() => setIsOpen(false), []);
  useDismissable(isOpen, containerRef, close);

  return (
    <div
      ref={containerRef}
      style={{
        position: 'relative',
        width: '100%',
        opacity: disabled ? DISABLED_OPACITY : 1,
        pointerEvents: disabled ? 'none' : 'auto',
      }}
    >
      <div
        style={{
          borderRadius: '8rem',
          background: 'rgba(120, 120, 128, 0.36)',
          height: `${height}rem`,
          padding: '0 12rem',
          display: 'flex',
          alignItems: 'center',
          width: '100%',
          boxSizing: 'border-box',
          gap: '10rem',
          userSelect: 'none',
        }}
      >
        {/* Previous button */}
        <button
          onClick={handlePrev}
          disabled={currentIndex <= 0}
          style={{
            background: 'none',
            border: 'none',
            padding: '12rem 0',
            cursor: currentIndex > 0 ? 'pointer' : 'not-allowed',
            opacity: currentIndex > 0 ? 1 : DISABLED_OPACITY,
            display: 'flex',
            alignItems: 'center',
            color: 'white',
          }}
        >
          <ChevronLeft size={20} />
        </button>

        {/* Name / Dropdown trigger */}
        <div
          onClick={() => {
            if (!isOpen) onOpen?.();
            setIsOpen(!isOpen);
          }}
          style={{
            flex: 1,
            minWidth: 0,
            display: 'flex',
            alignItems: 'center',
            justifyContent: 'center',
            gap: '6rem',
            overflow: 'hidden',
            cursor: 'pointer',
            padding: '12rem 0',
          }}
        >
          <span
            style={{
              overflow: 'hidden',
              whiteSpace: 'nowrap',
              textOverflow: 'ellipsis',
              color: 'white',
              fontSize: '14rem',
              fontWeight: '400',
            }}
          >
            {selectedOption?.name ?? 'Select models...'}
          </span>
        </div>

        {/* Next button */}
        <button
          onClick={handleNext}
          disabled={currentIndex >= options.length - 1}
          style={{
            background: 'none',
            border: 'none',
            padding: '12rem 0',
            cursor: currentIndex < options.length - 1 ? 'pointer' : 'not-allowed',
            opacity: currentIndex < options.length - 1 ? 1 : DISABLED_OPACITY,
            display: 'flex',
            alignItems: 'center',
            color: 'white',
          }}
        >
          <ChevronRight size={20} />
        </button>

        {/* Divider + model count */}
        <div
          style={{
            width: '1rem',
            alignSelf: 'stretch',
            margin: '8rem 0',
            backgroundColor: 'rgba(84, 84, 88, 0.65)',
            flexShrink: 0,
          }}
        />
        <span
          style={{
            display: 'flex',
            alignItems: 'center',
            gap: '6rem',
            color: 'rgba(255, 255, 255, 0.6)',
            fontSize: '13rem',
            fontWeight: '400',
            whiteSpace: 'nowrap',
            flexShrink: 0,
            width: '70rem',
            justifyContent: 'center',
          }}
        >
          <FolderClosed size={14} />
          <span style={{ display: 'flex', fontVariantNumeric: 'tabular-nums' }}>
            {currentIndex + 1}/{totalCount}
          </span>
        </span>
      </div>

      {/* Portaled to document.body, fixed at the trigger's own viewport
          rect (see dropdownStyle above): any ancestor card - a Dual Mono
          side card especially - can clip an absolutely-positioned child
          with overflow: hidden, which made the top rows of a long list
          unreachable when there wasn't enough room above. Escaping to
          document.body sidesteps every such ancestor; the position/
          direction/height are all computed fresh per open instead. */}
      {isOpen &&
        dropdownStyle &&
        createPortal(
          <div
            ref={dropdownRef}
            className="hide-scrollbar"
            onPointerDown={(e) => e.stopPropagation()}
            style={{
              borderRadius: '8rem',
              background: '#39393D',
              overflowY: 'auto',
              zIndex: 1000,
              ...dropdownStyle,
            }}
          >
            {options.map((option, index) => (
              <div
                key={option.id}
                ref={option.id === value ? activeOptionRef : undefined}
                onClick={() => handleSelect(option.id)}
                style={{
                  padding: '12rem 16rem',
                  cursor: 'pointer',
                  color: 'white',
                  fontSize: '14rem',
                  lineHeight: '17rem',
                  fontWeight: '400',
                  overflow: 'hidden',
                  textOverflow: 'ellipsis',
                  whiteSpace: 'nowrap',
                  background: option.id === value ? 'rgba(255, 255, 255, 0.1)' : 'transparent',
                  borderBottom:
                    index < options.length - 1 ? '1rem solid rgba(84, 84, 88, 0.65)' : 'none',
                }}
                onMouseEnter={(e) => {
                  if (option.id !== value) {
                    e.currentTarget.style.background = 'rgba(255, 255, 255, 0.1)';
                  }
                }}
                onMouseLeave={(e) => {
                  if (option.id !== value) {
                    e.currentTarget.style.background = 'transparent';
                  }
                }}
              >
                {option.name}
              </div>
            ))}
            {loading && (
              <div
                style={{
                  display: 'flex',
                  justifyContent: 'center',
                  padding: '10rem 16rem',
                  borderTop: '1rem solid rgba(84, 84, 88, 0.65)',
                }}
              >
                <LoadingDots />
              </div>
            )}
          </div>,
          document.body
        )}
    </div>
  );
};
