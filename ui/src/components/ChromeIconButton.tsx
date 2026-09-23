import React from 'react';
import { helpProps } from './helpText';
import {
  BLACK,
  BORDER,
  BRAND_RED,
  BRAND_YELLOW,
  DISABLED_OPACITY,
  GRAY,
  HIGHLIGHT,
  ICON_BOX_SIZE,
  ICON_SIZE,
  iconButtonStyle,
  MUTED,
  textBoxStyle,
  WHITE,
} from './theme';

/**
 * Shared chrome icon button for the faceplate, card headers, tiles, and
 * pan rail. Box is always ICON_BOX_SIZE × ICON_BOX_RADIUS; glyphs are
 * forced to ICON_SIZE and grid-centered in the box (no nested flex/span
 * that WebKit can baseline-shift).
 */
export type ChromeTone = 'plain' | 'power' | 'armed' | 'danger' | 'link';

interface ChromeIconButtonProps {
  onClick: (e: React.MouseEvent) => void;
  /** One-line hint for the faceplate help readout (see helpText.ts). */
  help: string;
  children: React.ReactNode;
  /**
   * plain: white icon (optional HIGHLIGHT via `filled`).
   * power: on = white/clear; off = GRAY + HIGHLIGHT (section power, block normalize).
   * armed: on = BRAND_YELLOW + BLACK (listening / engaged); off = BORDER + GRAY.
   * danger: on = BRAND_RED + WHITE (actively silencing something, e.g. Mute
   *   engaged); off = BORDER + GRAY. Same shape as armed, red instead of
   *   yellow - reserved for "this is actively cutting audio", never plain
   *   "engaged" (that's armed's job).
   * link:  on = white; off = GRAY, never a fill (pan link).
   */
  tone?: ChromeTone;
  /** For power/armed/link: feature on / listening / linked. */
  on?: boolean;
  /** plain only: HIGHLIGHT fill while true (e.g. non-default input mode). */
  filled?: boolean;
  /**
   * Open / panel showing (tone info): WHITE fill + BLACK icon, same as
   * ChromeTextButton.open. Wins over `tone` / `on` / `filled`.
   */
  open?: boolean;
  /** Vertical nudge from a bottom-aligned row (faceplate side buttons). */
  offsetY?: number;
  /** e.g. preventFocus on gallery tiles so the webview doesn't scroll. */
  onMouseDown?: (e: React.MouseEvent) => void;
  disabled?: boolean;
  style?: React.CSSProperties;
}

/** Always 1px so the content box never changes between tones. */
const toneChrome = (
  tone: ChromeTone,
  on: boolean,
  filled: boolean
): Pick<React.CSSProperties, 'color' | 'backgroundColor' | 'border'> => {
  switch (tone) {
    case 'power':
      return {
        color: on ? WHITE : GRAY,
        backgroundColor: on ? 'transparent' : HIGHLIGHT,
        border: '1rem solid transparent',
      };
    case 'armed':
      return {
        color: on ? BLACK : GRAY,
        backgroundColor: on ? BRAND_YELLOW : 'transparent',
        border: on ? `1rem solid ${BRAND_YELLOW}` : BORDER,
      };
    case 'danger':
      return {
        color: on ? WHITE : GRAY,
        backgroundColor: on ? BRAND_RED : 'transparent',
        border: on ? `1rem solid ${BRAND_RED}` : BORDER,
      };
    case 'link':
      return {
        color: on ? WHITE : GRAY,
        backgroundColor: 'transparent',
        border: '1rem solid transparent',
      };
    default:
      return {
        color: WHITE,
        backgroundColor: filled ? HIGHLIGHT : 'transparent',
        border: '1rem solid transparent',
      };
  }
};

/**
 * Normalize Lucide / SVG icons for chrome boxes. Stroke scales with size;
 * do NOT use absoluteStrokeWidth (at 14px that locks a 2px stroke and looks
 * bloated). `display:block` kills the inline-SVG baseline gap, and grid
 * centering on the box keeps every glyph optically centered without per-icon
 * nudges.
 *
 * Pass `size` to force a glyph size (ChromeIconButton). Omit it to keep the
 * child's own size (IconButton in the top bar uses 18px in a 28px box).
 */
export function chromeIcon(node: React.ReactNode, size?: number): React.ReactNode {
  if (!React.isValidElement(node)) return node;
  const el = node as React.ReactElement<{
    size?: number | string;
    absoluteStrokeWidth?: boolean;
    style?: React.CSSProperties;
  }>;
  return React.cloneElement(el, {
    ...(size !== undefined ? { size } : null),
    absoluteStrokeWidth: false,
    style: {
      display: 'block',
      flexShrink: 0,
      ...(el.props.style ?? {}),
    },
  });
}

export const ChromeIconButton = React.forwardRef<HTMLButtonElement, ChromeIconButtonProps>(
  (
    {
      onClick,
      help,
      children,
      tone = 'plain',
      on = true,
      filled = false,
      open = false,
      offsetY,
      onMouseDown,
      disabled = false,
      style,
    },
    ref
  ) => (
    <button
      ref={ref}
      type="button"
      onClick={onClick}
      onMouseDown={onMouseDown}
      disabled={disabled}
      {...helpProps(help)}
      style={{
        ...iconButtonStyle(ICON_BOX_SIZE),
        // Grid on the button itself: one centering context, no nested span.
        display: 'grid',
        placeItems: 'center',
        ...toneChrome(tone, on, filled),
        ...(open
          ? {
              color: BLACK,
              backgroundColor: WHITE,
              border: `1rem solid ${WHITE}`,
            }
          : {}),
        opacity: disabled ? DISABLED_OPACITY : 1,
        cursor: disabled ? 'not-allowed' : 'pointer',
        transform: offsetY !== undefined ? `translateY(${offsetY}rem)` : undefined,
        ...style,
      }}
    >
      {chromeIcon(children, ICON_SIZE)}
    </button>
  )
);
ChromeIconButton.displayName = 'ChromeIconButton';

/**
 * Text chrome button (EQ, PRE). TEXT_BOX_HEIGHT / ICON_BOX_RADIUS.
 * Priority: open (panel showing) > armed (feature shaping) > idle.
 * - open: WHITE fill + BLACK label.
 * - armed: BRAND_YELLOW fill + BLACK label.
 * - idle: BORDER + MUTED.
 */
interface ChromeTextButtonProps {
  /** Takes the event (rather than `() => void`) so a caller can branch on a
      held modifier - e.g. the chain-map strip's chip: Alt/Option-click
      bypasses the block instead of navigating (see ChainMapStrip.tsx). */
  onClick: (e: React.MouseEvent) => void;
  help: string;
  children: React.ReactNode;
  armed?: boolean;
  open?: boolean;
  style?: React.CSSProperties;
}

const textChrome = (
  armed: boolean,
  open: boolean
): Pick<React.CSSProperties, 'color' | 'backgroundColor' | 'border'> => {
  if (open) {
    return {
      color: BLACK,
      backgroundColor: WHITE,
      border: `1rem solid ${WHITE}`,
    };
  }
  if (armed) {
    return {
      color: BLACK,
      backgroundColor: BRAND_YELLOW,
      border: `1rem solid ${BRAND_YELLOW}`,
    };
  }
  return {
    color: MUTED,
    backgroundColor: 'transparent',
    border: BORDER,
  };
};

export const ChromeTextButton = React.forwardRef<HTMLButtonElement, ChromeTextButtonProps>(
  ({ onClick, help, children, armed = false, open = false, style }, ref) => (
    <button
      ref={ref}
      type="button"
      onClick={onClick}
      {...helpProps(help)}
      style={{
        ...textBoxStyle(),
        ...textChrome(armed, open),
        lineHeight: 1,
        ...style,
      }}
    >
      {/* cap-trim span: optical centering for the mono label (see index.css). */}
      <span className="cap-trim">{children}</span>
    </button>
  )
);
ChromeTextButton.displayName = 'ChromeTextButton';
