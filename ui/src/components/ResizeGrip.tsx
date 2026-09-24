import React from 'react';
import { useNativeFunction } from '../hooks/useFunction';
import { IS_IOS } from '../hooks/useUiScale';
import { HELP, helpProps } from './helpText';
import { GRAY } from './theme';

/** Corner hit area (design px): generous and invisible around the small
    glyph, so the corner is easy to grab - kept clear of the hint bar's X. */
const CORNER_HIT = 24;
/** Glyph size drawn in the very corner. */
const GLYPH = 14;
/** Invisible strips along the right and bottom edges: where a hand reaches
    for a window edge (Logic draws its own resize cursor there, but JUCE's
    AUv2 wrapper rejects host-driven resizes, so these do the work). */
const EDGE = 6;

/**
 * Window resize handles: a bottom-right corner grip plus right/bottom edge
 * strips. JUCE's own corner resizer is a lightweight component the native
 * WebView always paints over, and JUCE's AUv2 wrapper snaps host-initiated
 * resizes back, so in Logic these are the only way to resize. A press just
 * hands off to native (beginEditorResizeDrag), which follows the real mouse
 * until release; its dominant-axis math lets an edge drag along one axis
 * grow and shrink the (aspect-locked) window naturally.
 */
export const ResizeGrip: React.FC = () => {
  const beginResizeDrag = useNativeFunction<boolean>('beginEditorResizeDrag');
  if (IS_IOS) return null;

  const onPointerDown = (e: React.PointerEvent<HTMLDivElement>) => {
    if (e.pointerType === 'mouse' && e.button !== 0) return;
    e.preventDefault();
    void beginResizeDrag();
  };
  const handle = (style: React.CSSProperties): React.CSSProperties => ({
    position: 'absolute',
    zIndex: 2000,
    touchAction: 'none',
    ...style,
  });

  return (
    <>
      <div
        {...helpProps(HELP.resizeGrip)}
        onPointerDown={onPointerDown}
        style={handle({
          right: 0,
          top: 0,
          bottom: `${CORNER_HIT}rem`,
          width: `${EDGE}rem`,
          cursor: 'ew-resize',
        })}
      />
      <div
        {...helpProps(HELP.resizeGrip)}
        onPointerDown={onPointerDown}
        style={handle({
          left: 0,
          right: `${CORNER_HIT}rem`,
          bottom: 0,
          height: `${EDGE}rem`,
          cursor: 'ns-resize',
        })}
      />
      <div
        {...helpProps(HELP.resizeGrip)}
        onPointerDown={onPointerDown}
        style={handle({
          right: 0,
          bottom: 0,
          width: `${CORNER_HIT}rem`,
          height: `${CORNER_HIT}rem`,
          cursor: 'nwse-resize',
          display: 'flex',
          alignItems: 'flex-end',
          justifyContent: 'flex-end',
        })}
      >
        {/* Two diagonal ridges, the classic resize-corner mark. */}
        <svg
          viewBox="0 0 14 14"
          style={{ width: `${GLYPH}rem`, height: `${GLYPH}rem`, display: 'block' }}
        >
          <path
            d="M13 5 L5 13 M13 9 L9 13"
            stroke={GRAY}
            strokeWidth={1.2}
            strokeLinecap="round"
            fill="none"
          />
        </svg>
      </div>
    </>
  );
};
