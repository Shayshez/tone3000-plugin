import type { CSSProperties } from 'react';
import { rem } from '../hooks/useUiScale';

/**
 * Shared theme tokens. The palette is deliberately tiny: black surfaces,
 * white/gray chrome, and three brand accents (pure blue / yellow / red) for
 * audio visuals and UI "attention" states. Components import these instead of
 * re-declaring the same hex literals.
 *
 * Icon / chrome box system (Lucide glyphs; custom SVGs use `currentColor`,
 * round caps, and a stroke weight that matches Lucide at the rendered size):
 * - Glyph in a box: ICON_SIZE (14). Square box: ICON_BOX_SIZE (20) ×
 *   ICON_BOX_RADIUS (2). Text / non-square chrome (EQ, PRE, LITE/FULL,
 *   segmented strips): TEXT_BOX_HEIGHT (20), 12px monospace, 4px L/R padding.
 * - Interactive icons are white; GRAY only for off/disabled states.
 * - State patterns (see ChromeIconButton / ChromeTextButton):
 *   1. Power (on/off): on = white icon, no fill; off = GRAY icon + HIGHLIGHT.
 *   2. Open / panel showing (EQ editor, tone info): WHITE fill + BLACK label/icon.
 *   3. Armed / listening / shaping (auto-balance, active EQ while closed,
 *      PRE, normalize): BRAND_YELLOW fill + BLACK glyph/label.
 *   4. Link (pan link): on = white icon; off = GRAY icon, never a fill.
 */

/** The app's one monospace stack. Roboto Mono ships with the plugin
    (@fontsource imports in main.tsx); the generic keyword is only a
    fallback for the instant before the bundled face registers. */
export const FONT_MONO = "'Roboto Mono', monospace";

/** Lucide / custom glyph size inside ICON_BOX_SIZE chrome boxes. */
export const ICON_SIZE = 14;
/** Square hit-target for icon buttons beside knobs and in card headers. */
export const ICON_BOX_SIZE = 20;
/** Corner radius for every icon/text chrome box. */
export const ICON_BOX_RADIUS = 2;
/** Height for text chrome (EQ, PRE, LITE/FULL segments). */
export const TEXT_BOX_HEIGHT = 20;

/** The two knob footprints (see KnobInner). Every knob in the UI is one of
    these two sizes: primary for a section's headline control, secondary for
    its companion trims. */
export const KNOB_SIZE_PRIMARY = 48;
export const KNOB_SIZE_SECONDARY = 36;

/** The one dim applied to every disabled/off control in the app: powered-off
    groups (`.ui-off`), disabled buttons, unavailable cards. */
export const DISABLED_OPACITY = 0.45;

/**
 * Powered-off / bypassed treatment for a group of controls (see `.ui-off`
 * in index.css): dims the group to DISABLED_OPACITY, shows the not-allowed
 * cursor, and makes every descendant inert (no clicks, drags, or hover
 * hints). The section's power button must sit OUTSIDE the dimmed wrapper so
 * the feature can be switched back on. Pair with an inline
 * `transition: 'opacity 0.2s ease'` so the dim fades both ways.
 */
export const uiOffClass = (off: boolean): string | undefined => (off ? 'ui-off' : undefined);

/** Brand accents, the only chromatic UI colors outside gray/white/black. */
export const BRAND_BLUE = '#0000FF';
export const BRAND_YELLOW = '#FFFF00';
export const BRAND_RED = '#FF0000';
/** A secondary, non-error "shaped away from default" signal (e.g. the chain
    map strip's EQ-modified mark) that has to read clearly on its own against
    black - unlike BRAND_RED (already reads as error/destructive elsewhere)
    or BRAND_YELLOW (already reads as active/armed), and unlike BRAND_BLUE,
    whose low luminance nearly disappears as a small solid mark on black. */
export const BRAND_ORANGE = '#FF8000';
/** Inline doc / “Learn More” links in settings. */
export const LINK_BLUE = '#40A6FF';

export const WHITE = '#ffffff';
export const BLACK = '#000000';

/** Primary muted text/icon color. */
export const MUTED = 'rgba(235, 235, 245, 0.60)';
/** Secondary labels (axis marks, fine print). */
export const SUBTLE = 'rgba(235, 235, 245, 0.40)';
/** Disabled/idle icon gray. */
export const GRAY = '#8D8D93';
/** Pressed/active fill behind white icons and segmented buttons. */
export const HIGHLIGHT = 'rgba(235, 235, 245, 0.18)';
/** Hairline used by every card/section/segment border. */
export const BORDER = '1rem solid rgba(84, 84, 88, 0.65)';
/** Card body background. */
export const SURFACE = '#151517';
/** Raised chrome (card headers, faceplate, pills). */
export const SURFACE_RAISED = '#1C1C1E';

/**
 * Outline pill CTA: white border + white label on a transparent fill.
 * Used for chrome actions (Browse, Spread) and as the secondary button on
 * edge-case screens (Dismiss beside a filled primary).
 */
export const pillButtonStyle: CSSProperties = {
  display: 'flex',
  alignItems: 'center',
  justifyContent: 'center',
  gap: '8rem',
  padding: '7rem 16rem',
  fontSize: '13rem',
  fontWeight: 400,
  borderRadius: '9999rem',
  border: `1rem solid ${WHITE}`,
  backgroundColor: 'transparent',
  color: WHITE,
  cursor: 'pointer',
  whiteSpace: 'nowrap',
  flexShrink: 0,
};

/**
 * Filled pill CTA: solid white background + black label at weight 400.
 * The highest-priority action on edge-case / gated screens (sign-in, Try
 * again, Download update, Reload). Pair with `pillButtonStyle` for any
 * secondary action beside it.
 */
export const filledPillButtonStyle: CSSProperties = {
  display: 'flex',
  alignItems: 'center',
  justifyContent: 'center',
  padding: '10rem 20rem',
  fontSize: '14rem',
  fontWeight: 400,
  borderRadius: '9999rem',
  border: 'none',
  backgroundColor: WHITE,
  color: BLACK,
  cursor: 'pointer',
  whiteSpace: 'nowrap',
};

/** Base style for a square icon chrome box (faceplate / card / tile).
 *  Grid + placeItems centers glyphs reliably; flex+inline-SVG baseline
 *  quirks are what made Power look low in the expanded block header. */
export const iconButtonStyle = (size = ICON_BOX_SIZE): CSSProperties => ({
  background: 'transparent',
  border: '1rem solid transparent',
  outline: 'none',
  color: MUTED,
  cursor: 'pointer',
  width: `${size}rem`,
  height: `${size}rem`,
  borderRadius: rem(ICON_BOX_RADIUS),
  display: 'grid',
  placeItems: 'center',
  padding: 0,
  flexShrink: 0,
  boxSizing: 'border-box',
  lineHeight: 0,
  fontSize: 0,
});

/** Text chrome box (EQ, PRE, static LITE/FULL label): fixed height, mono. */
export const textBoxStyle = (): CSSProperties => ({
  height: `${TEXT_BOX_HEIGHT}rem`,
  borderRadius: rem(ICON_BOX_RADIUS),
  display: 'flex',
  alignItems: 'center',
  justifyContent: 'center',
  padding: '0 4rem',
  fontSize: '12rem',
  fontWeight: 400,
  fontFamily: FONT_MONO,
  lineHeight: 1,
  boxSizing: 'border-box',
  flexShrink: 0,
  cursor: 'pointer',
  background: 'transparent',
});

/** Shared fill behind LITE/FULL and the EQ view switcher. */
export const SEGMENTED_TRACK = 'rgba(120, 120, 128, 0.36)';

/**
 * Segmented control shell (LITE/FULL, EQ view). Borderless track fill;
 * selection is white vs MUTED text/icons, not a cell highlight.
 */
export const segmentedGroupStyle = (): CSSProperties => ({
  display: 'flex',
  flexDirection: 'row',
  alignItems: 'stretch',
  height: `${TEXT_BOX_HEIGHT}rem`,
  borderRadius: rem(ICON_BOX_RADIUS),
  border: 'none',
  backgroundColor: SEGMENTED_TRACK,
  overflow: 'hidden',
  flexShrink: 0,
  boxSizing: 'border-box',
});

/**
 * One cell inside a segmented group. Non-square chrome (text or icon
 * strips) always gets 4px left/right padding, never a tight ICON_BOX square.
 */
export const segmentedCellStyle = (icon = false): CSSProperties => ({
  height: '100%',
  display: 'flex',
  alignItems: 'center',
  justifyContent: 'center',
  border: 'none',
  cursor: 'pointer',
  backgroundColor: 'transparent',
  padding: '0 4rem',
  fontSize: icon ? 0 : '12rem',
  fontWeight: icon ? undefined : 400,
  fontFamily: icon ? undefined : FONT_MONO,
  lineHeight: icon ? 0 : 1,
  flexShrink: 0,
  boxSizing: 'border-box',
});

/** Knob-to-label gap. faceplateChromeLift and the Spread/Align advert
    vertical centering are built around this. */
export const KNOB_LABEL_GAP = 8;

/**
 * Vertical lift for a chrome icon box sitting in a bottom-aligned faceplate
 * row: from the shared label baseline up to the center of a secondary knob.
 * (gap + 14px label + radius minus half the box.)
 */
export const faceplateChromeLift = (secondaryKnobSize: number) =>
  -(KNOB_LABEL_GAP + 14 + secondaryKnobSize / 2 - ICON_BOX_SIZE / 2);
