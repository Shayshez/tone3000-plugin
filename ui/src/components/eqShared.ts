import type { EqBandRole } from '../types/chain';
import { EQ_MAX_ABS_GAIN_DB } from '../types/chain';
import { CARD_WIDTH } from './chainLayout';

/**
 * Geometry and glyphs shared by the EQ editor (BlockEqView's graph +
 * always-visible band strip, SpectrumBackdrop).
 */

/** Height of the always-visible 8-band value strip at the bottom of the EQ
    card body (see BlockEqView) - one row, every band's icon/Freq/Gain-or-
    Pole/Q always live and drag-scrubbable in place; no separate expanded
    per-band panel, so this is as thin as the values comfortably allow and
    the graph gets the rest of the card body's height. */
export const EQ_STRIP_H = 44;

/** Reference card body height for the graph's internal coordinate space
    only (GRAPH_H below, and the gain<->y math derived from it) - NOT a
    real layout size. ChainBlock.tsx sizes the actual EQ card with the same
    CARD_HEIGHT/BODY_HEIGHT every other card uses (same standard 24rem
    top/bottom gallery margin too - EQ doesn't get a special larger or
    flush layout), and the SVG stretches to fill whatever real pixel size
    that resolves to (preserveAspectRatio="none"). This constant only sets
    the internal proportions - how much of the coordinate space is padding
    versus curve - not how big the graph actually renders. */
export const EQ_BODY_HEIGHT = 275;

// Graph: full EQ card body minus 1px border each side, minus the band strip.
// Spectrum/grid bleed edge-to-edge; interactive chrome (the strip) is inset
// separately.
export const GRAPH_W = CARD_WIDTH - 2;
export const GRAPH_H = EQ_BODY_HEIGHT - 2 - EQ_STRIP_H;
export const GRAPH_PAD_Y = 12; // keep dots inside the frame at ±24 dB

export const clamp = (v: number, lo: number, hi: number) => Math.min(Math.max(v, lo), hi);

export const gainToY = (gainDb: number) =>
  GRAPH_H / 2 - (gainDb / EQ_MAX_ABS_GAIN_DB) * (GRAPH_H / 2 - GRAPH_PAD_Y);
export const yToGain = (y: number) =>
  ((GRAPH_H / 2 - y) / (GRAPH_H / 2 - GRAPH_PAD_Y)) * EQ_MAX_ABS_GAIN_DB;

export const hasGain = (role: EqBandRole) =>
  role === 'bell' || role === 'lowshelf' || role === 'highshelf';

/** Fixed per-role display label. Not shown on screen (the icon glyph alone
    identifies the role there) - used for the icon's help/aria text only. */
export const BAND_ROLE_LABELS: Record<EqBandRole, string> = {
  lowcut: 'Low Cut',
  lowshelf: 'Low Shelf',
  bell: 'Bell',
  highshelf: 'High Shelf',
  highcut: 'High Cut',
};

/** 16x14 curve glyphs per fixed role. */
export const TYPE_GLYPHS: Record<EqBandRole, string> = {
  lowshelf: 'M1 11 C5 11 6 3 10 3 L15 3',
  bell: 'M1 11 C4 11 5 3 8 3 C11 3 12 11 15 11',
  highshelf: 'M1 3 C5 3 6 11 10 11 L15 11',
  lowcut: 'M1 13 C4 13 5 3 9 3 L15 3',
  highcut: 'M1 3 L7 3 C11 3 12 13 15 13',
};

/** Fixed per-band color, keyed by index (0=Low Cut..7=High Cut) - never by
    frequency/position, so a band's identity in the strip and on the graph
    dot stays put even as bands cross each other in frequency. */
export const EQ_BAND_COLORS = [
  '#FF6B6B',
  '#FFA94D',
  '#FFD43B',
  '#94D82D',
  '#38D9A9',
  '#4DABF7',
  '#9775FA',
  '#F783AC',
];

/** Default Freq/Q per band index - mirrors native BlockEq::defaultBands()
    (plugin/src/BlockEq.cpp) exactly, kept here so an Option/Alt-click reset
    on the strip's Freq/Q chips has the same per-band default the FLAT
    button's native reset restores, rather than one universal value that's
    only correct for the Cut/Shelf bands. */
export const EQ_DEFAULT_FREQ_HZ = [80, 100, 250, 650, 1600, 3500, 8000, 12000];
export const EQ_DEFAULT_Q = [0.71, 0.71, 1.0, 1.0, 1.0, 1.4, 0.71, 0.71];

/** Touch double tap window and slop, same as KnobControl's recognizer. */
const DOUBLE_TAP_MS = 300;
const DOUBLE_TAP_SLOP_PX = 24;

/**
 * Pointer-stream double tap for the touch resets (fader caps and curve
 * dots). Detected from pointerdown pairs, not `dblclick`, which WKWebView
 * ties to its own double-tap handling. Keyed by band index so both taps
 * must land on the same control.
 */
export const createDoubleTapDetector = () => {
  let last: { key: number; at: number; x: number; y: number } | null = null;
  return {
    /** Feed each touch pointerdown; true when it completes a double tap. */
    tap: (key: number, e: { timeStamp: number; clientX: number; clientY: number }): boolean => {
      const prev = last;
      const isDouble =
        prev !== null &&
        prev.key === key &&
        e.timeStamp - prev.at < DOUBLE_TAP_MS &&
        Math.abs(e.clientX - prev.x) < DOUBLE_TAP_SLOP_PX &&
        Math.abs(e.clientY - prev.y) < DOUBLE_TAP_SLOP_PX;
      // A third tap starts a fresh pair, it is not another reset.
      last = isDouble ? null : { key, at: e.timeStamp, x: e.clientX, y: e.clientY };
      return isDouble;
    },
    /** Feed touch pointermoves: a press that travels is a drag, not the
        first half of a double tap, so its candidate is withdrawn. */
    move: (e: { clientX: number; clientY: number }): void => {
      if (
        last !== null &&
        (Math.abs(e.clientX - last.x) > DOUBLE_TAP_SLOP_PX ||
          Math.abs(e.clientY - last.y) > DOUBLE_TAP_SLOP_PX)
      )
        last = null;
    },
  };
};
