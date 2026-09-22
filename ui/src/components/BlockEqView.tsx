import React, { useCallback, useEffect, useMemo, useRef, useState } from 'react';
import type { EqBand } from '../types/chain';
import {
  EQ_MAX_ABS_GAIN_DB,
  EQ_MAX_Q,
  EQ_MIN_Q,
  EQ_MIN_FREQ_HZ,
  EQ_MAX_FREQ_HZ,
  EQ_POLE_OPTIONS,
  EQ_POLE_DB_PER_OCT,
  roleForBandIndex,
} from '../types/chain';
import { eqResponseDb, formatFreq, freqToNorm, normToFreq } from './eqMath';
import {
  BAND_ROLE_LABELS,
  EQ_BAND_COLORS,
  EQ_DEFAULT_FREQ_HZ,
  EQ_DEFAULT_Q,
  EQ_STRIP_H,
  GRAPH_H,
  GRAPH_W,
  TYPE_GLYPHS,
  clamp,
  createDoubleTapDetector,
  gainToY,
  hasGain,
  yToGain,
} from './eqShared';
import { getUiScale, rem } from '../hooks/useUiScale';
import { SpectrumBackdrop } from './SpectrumBackdrop';
import { HELP, helpProps, pinHelp, unpinHelp } from './helpText';
import { DISABLED_OPACITY, FONT_MONO, WHITE, uiOffClass } from './theme';

/**
 * 8-band EQ editor shown in the card body while the header EQ toggle is
 * active. Fixed channel-strip roles by index (see roleForBandIndex): band 0
 * is Low Cut, band 1 Low Shelf, bands 2-5 are Bells, band 6 High Shelf,
 * band 7 High Cut - no user-facing type selector, and no neighbor-lock on
 * frequency (bands can freely cross each other; identity/color is fixed by
 * index, never by sorted position).
 *
 * One editor, two coupled surfaces:
 * - Graph: curve + 8 draggable dots. Drag = freq+gain (or freq+Q on cut
 *   bands, which have no gain).
 * - Band strip (bottom, one thin row): every band's icon + Freq + Gain-or-
 *   Pole + Q live and directly manipulable in place - no separate "select a
 *   band, then edit it elsewhere" step. A band's icon doubles as its own
 *   bypass toggle (Low/High Cut start off); hovering a band (chip or dot)
 *   highlights its working frequency range on the graph.
 *
 * Shared interaction conventions (mirroring KnobControl):
 * - Every readout (Freq/Gain/Pole/Q, on the dot or in the strip) is a
 *   fader: vertical drag adjusts it in place, Shift = 8x finer, a plain
 *   click (no drag) opens type-in entry (Enter commits, Escape cancels).
 * - Alt/Option-click a dot resets the band (gain to 0, or Q to default on
 *   cut bands) without touching its frequency or Pole.
 *
 * The spectrum backdrop (SpectrumBackdrop.tsx) is its own leaf so its
 * ~30 fps updates never re-render the editor.
 */

// Controls stay black/white/gray (like the knobs); color only appears on
// the band strip/dots/hover-highlight to bind them together, never on the
// summed curve.
const CURVE_COLOR = '#8E8E93';

/** Q reset target for Low/High Cut bands (the Butterworth-flat reference). */
const CUT_DEFAULT_Q = Math.SQRT1_2;

// One sample per horizontal pixel so even a Q=20 spike is drawn cleanly
// (coarse sampling clips the narrow notch into a flat-bottomed wedge).
const CURVE_POINTS = GRAPH_W;
const CURVE_FREQS = Array.from({ length: CURVE_POINTS }, (_, i) =>
  normToFreq(i / (CURVE_POINTS - 1))
);

const GRID_FREQS = [50, 100, 200, 500, 1000, 2000, 5000, 10000];
const GRID_LABELS: Record<number, string> = {
  50: '50',
  100: '100',
  200: '200',
  500: '500',
  1000: '1k',
  2000: '2k',
  5000: '5k',
  10000: '10k',
};

/** Parse a typed frequency: plain Hz ("800") or k-notation ("1.2k"). */
const parseFreqInput = (raw: string): number | null => {
  const cleaned = raw.trim().toLowerCase().replace(',', '.').replace(/hz$/, '').trim();
  const hasK = cleaned.includes('k');
  const value = Number.parseFloat(cleaned.replace('k', ''));
  if (!Number.isFinite(value)) return null;
  return hasK ? value * 1000 : value;
};

/** A band's approximate working frequency range for the hover highlight:
    half-width of 1/(2*Q) octaves each side of its own frequency - narrower
    as Q rises, wide (most of the visible spectrum) at the lowest Q. Not
    meant to be psychoacoustically exact, just visually proportional. */
const bandRangeHz = (freqHz: number, q: number): [number, number] => {
  const halfOctaves = 1 / (2 * Math.max(q, 0.01));
  return [freqHz * Math.pow(2, -halfOctaves), freqHz * Math.pow(2, halfOctaves)];
};

interface BlockEqViewProps {
  blockId: string;
  bands: EqBand[];
  /** EQ power state; a bypassed EQ renders its curve/dots/controls dimmed
      and inert (the header power button is the way back in). */
  eqEnabled: boolean;
  sampleRate: number;
  /** Fire-and-forget whole-band setter (safe at drag rates). */
  onSetBand: (blockId: string, bandIndex: number, band: EqBand) => void;
}

// --- FaderValue: double-click-to-type + drag-to-scrub, shared by every
// readout in the strip (Freq/Gain/Pole/Q, all 8 bands) - the same convention
// KnobControl uses (double-click opens the editor, Option/Alt-click resets),
// not a separate one just for text.
const DRAG_SLOP_PX = 4;

const FaderValue: React.FC<{
  text: string;
  /** Omit both together to drop the double-click-to-type editor entirely -
      for Pole, where the displayed unit (dB/oct) and the underlying stored
      value (a pole count) don't match, so free-text entry would force
      picking one unit to type in while showing the other. Drag-to-scrub and
      Alt-click-reset stay available either way. */
  editText?: string;
  onCommitText?: (raw: string) => void;
  /** Called on every move once the gesture has committed to a drag; deltaY
      is in design px (already UI-scale-corrected), positive = dragged up. */
  onDrag: (deltaY: number, fine: boolean) => void;
  /** Option/Alt-click resets this value instead of starting a drag, matching
      KnobControl's and the graph dot's reset convention. Omit where there's
      no well-defined default to reset to. */
  onAltReset?: () => void;
  color: string;
  disabled?: boolean;
  help?: string;
  fontSize?: number;
  width?: number;
}> = ({
  text,
  editText,
  onCommitText,
  onDrag,
  onAltReset,
  color,
  disabled,
  help,
  fontSize = 10,
  width,
}) => {
  const editable = editText !== undefined && onCommitText !== undefined;
  const [draft, setDraft] = useState<string | null>(null);
  const editing = draft !== null;
  const inputRef = useRef<HTMLInputElement>(null);
  const draggingRef = useRef(false);
  const movedRef = useRef(false);
  const startRef = useRef({ x: 0, y: 0 });
  const lastYRef = useRef(0);

  useEffect(() => {
    if (editing) {
      inputRef.current?.focus();
      inputRef.current?.select();
    }
  }, [editing]);

  const commit = () => {
    if (draft !== null && draft.trim() !== '') onCommitText?.(draft);
    setDraft(null);
  };
  const lastPointerTypeRef = useRef<string>('mouse');

  const handlePointerDown = (e: React.PointerEvent) => {
    if (disabled || editing) return;
    e.stopPropagation();
    lastPointerTypeRef.current = e.pointerType;
    // Option/Alt-click resets instead of starting a drag, matching
    // KnobControl's and the graph dot's reset convention - skip the drag
    // entirely rather than let a reset be immediately overwritten by a few
    // pixels of pointer travel before release.
    if (e.altKey && onAltReset) {
      onAltReset();
      return;
    }
    e.currentTarget.setPointerCapture(e.pointerId);
    draggingRef.current = true;
    movedRef.current = false;
    startRef.current = { x: e.clientX, y: e.clientY };
    lastYRef.current = e.clientY;
    pinHelp(help ?? '');
  };
  const handlePointerMove = (e: React.PointerEvent) => {
    if (!draggingRef.current) return;
    if (!movedRef.current) {
      const dx = e.clientX - startRef.current.x;
      const dy = e.clientY - startRef.current.y;
      if (Math.hypot(dx, dy) < DRAG_SLOP_PX) return;
      movedRef.current = true;
      lastYRef.current = e.clientY; // drop the slop distance from the first delta
    }
    const deltaY = (lastYRef.current - e.clientY) / getUiScale();
    lastYRef.current = e.clientY;
    onDrag(deltaY, e.shiftKey);
  };
  const handlePointerUp = () => {
    if (!draggingRef.current) return;
    draggingRef.current = false;
    if (help) unpinHelp(help);
  };

  return (
    <div
      style={{ position: 'relative', lineHeight: 1.2, opacity: disabled ? DISABLED_OPACITY : 1 }}
    >
      {editing ? (
        <input
          ref={inputRef}
          value={draft}
          onChange={(e) => setDraft(e.target.value)}
          onBlur={commit}
          onPointerDown={(e) => e.stopPropagation()}
          onKeyDown={(e) => {
            e.stopPropagation();
            if (e.key === 'Enter') commit();
            else if (e.key === 'Escape') setDraft(null);
          }}
          inputMode="decimal"
          placeholder={editText}
          style={{
            width: width ? `${width}rem` : '100%',
            background: 'transparent',
            border: 'none',
            borderBottom: `1rem solid ${color}`,
            color: WHITE,
            fontSize: `${fontSize}rem`,
            fontFamily: FONT_MONO,
            textAlign: 'center',
            outline: 'none',
            padding: 0,
          }}
        />
      ) : (
        <>
          <span
            style={{
              display: 'block',
              width: width ? `${width}rem` : undefined,
              fontSize: `${fontSize}rem`,
              fontFamily: FONT_MONO,
              color: WHITE,
              textAlign: 'center',
              whiteSpace: 'nowrap',
              userSelect: 'none',
            }}
          >
            {text}
          </span>
          {/* Invisible, larger pointer target - the row above is one thin
              line of text, fiddly to grab/double-click precisely. Absolutely
              positioned so it adds no height to the stacked Freq/Gain-or-
              Pole/Q rows; only rendered while not editing, so it never sits
              over (and steals focus/typing from) the input above. */}
          <div
            {...(help && !disabled ? helpProps(help) : {})}
            onPointerDown={handlePointerDown}
            onPointerMove={handlePointerMove}
            onPointerUp={handlePointerUp}
            onPointerCancel={handlePointerUp}
            // The dblclick some engines synthesize from a touch double-tap
            // must not also open the editor here (same guard KnobControl
            // uses).
            onDoubleClick={() => {
              if (editable && !disabled && lastPointerTypeRef.current !== 'touch') setDraft('');
            }}
            style={{
              position: 'absolute',
              inset: '-4rem 0',
              touchAction: 'none',
              cursor: disabled ? undefined : 'ns-resize',
            }}
          />
        </>
      )}
    </div>
  );
};

export const BlockEqView: React.FC<BlockEqViewProps> = ({
  blockId,
  bands: bandsProp,
  eqEnabled,
  sampleRate,
  onSetBand,
}) => {
  // Optimistic local bands; native converges via chain-state resyncs. Skip
  // prop syncs mid-drag so a stale snapshot can't fight the pointer.
  const [bands, setBands] = useState<EqBand[]>(bandsProp);
  const [selected, setSelected] = useState(1);
  const [hovered, setHovered] = useState<number | null>(null);
  const draggingRef = useRef(false);
  const dragStateRef = useRef<{ index: number; lastX: number; lastY: number } | null>(null);
  const graphRef = useRef<SVGSVGElement | null>(null);
  // Accumulates drag distance for the discrete Pole stepper (see
  // handlePoleDrag) - one option per POLE_STEP_PX of travel.
  const poleDragAccumRef = useRef(0);

  useEffect(() => {
    if (!draggingRef.current) setBands(bandsProp);
  }, [bandsProp]);

  const roleOf = useCallback((index: number) => roleForBandIndex(index, bands.length), [bands]);

  const updateBand = useCallback(
    (index: number, patch: Partial<EqBand>) => {
      setBands((prev) => {
        const next = prev.map((b, i) => (i === index ? { ...b, ...patch } : b));
        onSetBand(blockId, index, next[index]);
        return next;
      });
    },
    [blockId, onSetBand]
  );

  const toggleBandOn = useCallback(
    (index: number) => {
      const band = bands[index];
      if (!band) return;
      setSelected(index);
      updateBand(index, { on: !band.on });
    },
    [bands, updateBand]
  );

  // --- dot dragging -------------------------------------------------------
  const graphPointFromEvent = useCallback((e: PointerEvent | React.PointerEvent) => {
    const rect = graphRef.current?.getBoundingClientRect();
    if (!rect) return { x: 0, y: 0 };
    return {
      x: ((e.clientX - rect.left) / rect.width) * GRAPH_W,
      y: ((e.clientY - rect.top) / rect.height) * GRAPH_H,
    };
  }, []);

  /** Alt/Option-click (touch: double tap) reset: neutralize the band's
      effect (gain or Q) while keeping its frequency and Pole, matching the
      knobs' reset convention. */
  const resetBand = useCallback(
    (index: number) => {
      const band = bands[index];
      if (!band) return;
      const role = roleOf(index);
      updateBand(index, hasGain(role) ? { gainDb: 0 } : { q: CUT_DEFAULT_Q });
    },
    [bands, roleOf, updateBand]
  );

  const [doubleTap] = useState(createDoubleTapDetector);

  const handleDotPointerDown = useCallback(
    (index: number) => (e: React.PointerEvent<SVGCircleElement>) => {
      e.preventDefault();
      setSelected(index);
      // Touch: second tap of a double tap resets, and ends the gesture there.
      if (e.pointerType === 'touch' && doubleTap.tap(index, e)) {
        resetBand(index);
        return;
      }
      if (e.altKey) {
        resetBand(index);
        return;
      }
      e.currentTarget.setPointerCapture(e.pointerId);
      draggingRef.current = true;
      // Keep the hint up while the (captured) drag runs across the graph.
      pinHelp(HELP.eqDot);
      const { x, y } = graphPointFromEvent(e);
      dragStateRef.current = { index, lastX: x, lastY: y };
    },
    [doubleTap, graphPointFromEvent, resetBand]
  );

  // Delta-based dragging (not absolute pointer position) so Shift = 8x finer
  // control works and can toggle mid-drag without the dot jumping.
  const handleDotPointerMove = useCallback(
    (index: number) => (e: React.PointerEvent<SVGCircleElement>) => {
      if (e.pointerType === 'touch') doubleTap.move(e);
      const drag = dragStateRef.current;
      if (!draggingRef.current || drag?.index !== index) return;
      const { x, y } = graphPointFromEvent(e);
      const fine = e.shiftKey ? 1 / 8 : 1;
      const dX = (x - drag.lastX) * fine;
      const dGain = (yToGain(y) - yToGain(drag.lastY)) * fine;
      const dY = (y - drag.lastY) * fine;
      drag.lastX = x;
      drag.lastY = y;

      const band = bands[index];
      // No neighbor-lock: every band's frequency is free across the whole
      // range, and bands can cross each other - identity is fixed by index
      // (and its color), never by sorted position.
      const freqNorm = clamp(freqToNorm(band.freqHz) + dX / GRAPH_W, 0, 1);
      const freqHz = clamp(normToFreq(freqNorm), EQ_MIN_FREQ_HZ, EQ_MAX_FREQ_HZ);
      const role = roleOf(index);
      if (hasGain(role)) {
        const gainDb = clamp(band.gainDb + dGain, -EQ_MAX_ABS_GAIN_DB, EQ_MAX_ABS_GAIN_DB);
        updateBand(index, { freqHz, gainDb });
      } else {
        // Cuts have no gain, so vertical drag tunes Q instead (up = tighter).
        const q = clamp(band.q * Math.exp(-dY * 0.02), EQ_MIN_Q, EQ_MAX_Q);
        updateBand(index, { freqHz, q });
      }
    },
    [bands, doubleTap, graphPointFromEvent, roleOf, updateBand]
  );

  const handleDotPointerUp = useCallback(() => {
    draggingRef.current = false;
    dragStateRef.current = null;
    unpinHelp(HELP.eqDot);
  }, []);

  // --- wheel = Q of the selected band (non-passive so the page can't scroll).
  // Redundant with the strip's own drag-scrubbable Q now, but a real
  // shortcut while the pointer is already over the graph - not removed.
  const containerRef = useRef<HTMLDivElement | null>(null);
  const wheelStateRef = useRef({ bands, selected, eqEnabled });
  wheelStateRef.current = { bands, selected, eqEnabled };
  useEffect(() => {
    const el = containerRef.current;
    if (!el) return;
    const onWheel = (e: WheelEvent) => {
      const { bands: current, selected: index, eqEnabled: on } = wheelStateRef.current;
      if (!on) return; // bypassed EQ is inert (matching the dimmed dots)
      const band = current[index];
      if (!band || !band.on) return;
      // A single-pole cut has no 2nd-order section, so Q has no effect there.
      if (roleForBandIndex(index, current.length) !== 'bell' && band.poles === 1) return;
      e.preventDefault();
      const fine = e.shiftKey ? 1 / 8 : 1;
      const q = clamp(band.q * Math.exp(-e.deltaY * 0.003 * fine), EQ_MIN_Q, EQ_MAX_Q);
      updateBand(index, { q });
    };
    el.addEventListener('wheel', onWheel, { passive: false });
    return () => el.removeEventListener('wheel', onWheel);
  }, [updateBand]);

  // --- curve --------------------------------------------------------------
  const curve = useMemo(() => {
    const response = eqResponseDb(bands, sampleRate, CURVE_FREQS);
    const points = response.map((db, i) => {
      const x = (i / (CURVE_POINTS - 1)) * GRAPH_W;
      const y = clamp(gainToY(db), -8, GRAPH_H + 8);
      return `${x.toFixed(1)} ${y.toFixed(1)}`;
    });
    const line = `M${points.join(' L')}`;
    const zeroY = gainToY(0).toFixed(1);
    const area = `${line} L${GRAPH_W} ${zeroY} L0 ${zeroY} Z`;
    return { line, area };
  }, [bands, sampleRate]);

  // Strip readout -> band updates (clamped like their drag equivalents).
  const commitFreq = useCallback(
    (index: number, raw: string) => {
      const parsed = parseFreqInput(raw);
      if (parsed === null) return;
      updateBand(index, { freqHz: clamp(parsed, EQ_MIN_FREQ_HZ, EQ_MAX_FREQ_HZ) });
    },
    [updateBand]
  );
  const commitGain = useCallback(
    (index: number, raw: string) => {
      const parsed = Number.parseFloat(raw.replace(',', '.'));
      if (!Number.isFinite(parsed)) return;
      updateBand(index, { gainDb: clamp(parsed, -EQ_MAX_ABS_GAIN_DB, EQ_MAX_ABS_GAIN_DB) });
    },
    [updateBand]
  );
  const commitQ = useCallback(
    (index: number, raw: string) => {
      const parsed = Number.parseFloat(raw.replace(',', '.'));
      if (!Number.isFinite(parsed)) return;
      updateBand(index, { q: clamp(parsed, EQ_MIN_Q, EQ_MAX_Q) });
    },
    [updateBand]
  );

  const dragFreq = useCallback(
    (index: number, deltaY: number, fine: boolean) => {
      const band = bands[index];
      if (!band) return;
      const norm = clamp(freqToNorm(band.freqHz) + (deltaY * (fine ? 1 / 8 : 1)) / GRAPH_W, 0, 1);
      updateBand(index, { freqHz: clamp(normToFreq(norm), EQ_MIN_FREQ_HZ, EQ_MAX_FREQ_HZ) });
    },
    [bands, updateBand]
  );
  const dragGain = useCallback(
    (index: number, deltaY: number, fine: boolean) => {
      const band = bands[index];
      if (!band) return;
      const gainDb = clamp(
        yToGain(gainToY(band.gainDb) - deltaY * (fine ? 1 / 8 : 1)),
        -EQ_MAX_ABS_GAIN_DB,
        EQ_MAX_ABS_GAIN_DB
      );
      updateBand(index, { gainDb });
    },
    [bands, updateBand]
  );
  const dragQ = useCallback(
    (index: number, deltaY: number, fine: boolean) => {
      const band = bands[index];
      if (!band) return;
      const q = clamp(band.q * Math.exp(deltaY * (fine ? 0.02 / 8 : 0.02)), EQ_MIN_Q, EQ_MAX_Q);
      updateBand(index, { q });
    },
    [bands, updateBand]
  );
  /** Discrete stepper: accumulates drag distance and steps one Pole option
      per POLE_STEP_PX of travel (fine = 4x that distance per step), so a
      deliberate flick moves one step, not the whole set. */
  const POLE_STEP_PX = 10;
  const dragPole = useCallback(
    (index: number, deltaY: number, fine: boolean) => {
      const band = bands[index];
      if (!band) return;
      poleDragAccumRef.current += deltaY;
      const stepPx = POLE_STEP_PX * (fine ? 4 : 1);
      let steps = 0;
      while (poleDragAccumRef.current >= stepPx) {
        poleDragAccumRef.current -= stepPx;
        steps += 1;
      }
      while (poleDragAccumRef.current <= -stepPx) {
        poleDragAccumRef.current += stepPx;
        steps -= 1;
      }
      if (steps === 0) return;
      const currentIdx = EQ_POLE_OPTIONS.indexOf(band.poles as (typeof EQ_POLE_OPTIONS)[number]);
      const nextIdx = clamp(
        (currentIdx < 0 ? 2 : currentIdx) + steps,
        0,
        EQ_POLE_OPTIONS.length - 1
      );
      updateBand(index, { poles: EQ_POLE_OPTIONS[nextIdx] });
    },
    [bands, updateBand]
  );

  const chipColW = 100 / 8;

  /** One band's compact readout in the always-visible strip: an icon that
      doubles as its bypass toggle, then Freq / Gain-or-Pole / Q, every
      value directly drag-scrubbable and click-to-type in place. */
  const renderStripChip = (band: EqBand, i: number) => {
    const role = roleOf(i);
    const color = EQ_BAND_COLORS[i % EQ_BAND_COLORS.length];
    const isOn = band.on;

    return (
      <div
        key={i}
        onPointerEnter={() => setHovered(i)}
        onPointerLeave={() => setHovered((h) => (h === i ? null : h))}
        style={{
          width: `${chipColW}%`,
          minWidth: 0,
          display: 'flex',
          flexDirection: 'row',
          alignItems: 'center',
          gap: '4rem',
          padding: '0 3rem',
          boxSizing: 'border-box',
          borderRadius: '4rem',
          // Same hover moment as the graph dot (setHovered is shared) - a
          // tint here makes it obvious which band a hovered dot belongs to
          // without having to trace it back to its color by eye.
          backgroundColor: hovered === i ? `${color}26` : 'transparent',
          transition: 'background-color 0.1s ease',
        }}
      >
        <button
          onClick={() => toggleBandOn(i)}
          {...helpProps(HELP.eqBandBypass)}
          aria-label={`${BAND_ROLE_LABELS[role]}: ${isOn ? 'on' : 'off'}`}
          style={{
            flexShrink: 0,
            width: rem(16),
            height: rem(16),
            border: 'none',
            background: 'transparent',
            padding: 0,
            cursor: 'pointer',
            opacity: isOn ? 1 : 0.35,
          }}
        >
          <svg viewBox="0 0 16 14" style={{ width: '100%', height: '100%', display: 'block' }}>
            <path
              d={TYPE_GLYPHS[role]}
              fill="none"
              stroke={color}
              strokeWidth={1.8}
              strokeLinecap="round"
            />
          </svg>
        </button>
        <div
          style={{
            flex: 1,
            minWidth: 0,
            display: 'flex',
            flexDirection: 'column',
            gap: '1rem',
            opacity: isOn ? 1 : DISABLED_OPACITY,
          }}
        >
          <FaderValue
            text={formatFreq(band.freqHz)}
            editText={Math.round(band.freqHz).toString()}
            onCommitText={(raw) => commitFreq(i, raw)}
            onDrag={(dy, fine) => dragFreq(i, dy, fine)}
            onAltReset={() => updateBand(i, { freqHz: EQ_DEFAULT_FREQ_HZ[i] })}
            color={color}
            help={HELP.eqFreqChip}
          />
          {role === 'lowcut' || role === 'highcut' ? (
            <FaderValue
              text={`${EQ_POLE_DB_PER_OCT[band.poles] ?? 24}dB/oct`}
              onDrag={(dy, fine) => dragPole(i, dy, fine)}
              onAltReset={() => updateBand(i, { poles: 4 })}
              color={color}
              help={HELP.eqPoleControl}
            />
          ) : (
            <FaderValue
              text={`${band.gainDb >= 0 ? '+' : ''}${band.gainDb.toFixed(1)}dB`}
              editText={band.gainDb.toFixed(1)}
              onCommitText={(raw) => commitGain(i, raw)}
              onDrag={(dy, fine) => dragGain(i, dy, fine)}
              onAltReset={() => updateBand(i, { gainDb: 0 })}
              color={color}
              help={HELP.eqGainChip}
            />
          )}
          <FaderValue
            text={band.q.toFixed(2)}
            editText={band.q.toFixed(2)}
            onCommitText={(raw) => commitQ(i, raw)}
            onDrag={(dy, fine) => dragQ(i, dy, fine)}
            onAltReset={() => updateBand(i, { q: EQ_DEFAULT_Q[i] })}
            color={color}
            disabled={(role === 'lowcut' || role === 'highcut') && band.poles === 1}
            help={HELP.eqQChip}
          />
        </div>
      </div>
    );
  };

  // --- hover range highlight -----------------------------------------------
  const hoverHighlight = useMemo(() => {
    if (hovered === null) return null;
    const band = bands[hovered];
    if (!band) return null;
    const [loHz, hiHz] = bandRangeHz(band.freqHz, band.q);
    const x1 = clamp(freqToNorm(loHz), 0, 1) * GRAPH_W;
    const x2 = clamp(freqToNorm(hiHz), 0, 1) * GRAPH_W;
    return { x: Math.min(x1, x2), width: Math.max(2, Math.abs(x2 - x1)) };
  }, [hovered, bands]);

  return (
    <div
      ref={containerRef}
      style={{
        flex: 1,
        height: '100%',
        minHeight: 0,
        position: 'relative',
        display: 'flex',
        flexDirection: 'column',
        backgroundColor: '#000000',
      }}
    >
      {/* Plain div takes the flex:1 share, not the <svg> itself - some
          WebKit versions don't correctly flex-size a replaced element like
          svg (it can fall back to an intrinsic/viewBox-derived size instead
          of its flex-basis), silently starving the strip below it. A plain
          block div flexes reliably everywhere; the svg just fills it via
          ordinary 100%/100% percentage sizing. */}
      <div style={{ flex: 1, minHeight: 0, position: 'relative' }}>
        <svg
          ref={graphRef}
          width="100%"
          height="100%"
          viewBox={`0 0 ${GRAPH_W} ${GRAPH_H}`}
          preserveAspectRatio="none"
          style={{ display: 'block' }}
        >
          {/* Grid */}
          {GRID_FREQS.map((f) => {
            const x = freqToNorm(f) * GRAPH_W;
            return (
              <g key={f}>
                <line
                  x1={x}
                  y1={0}
                  x2={x}
                  y2={GRAPH_H}
                  stroke="rgba(235, 235, 245, 0.07)"
                  strokeWidth={1}
                />
                {GRID_LABELS[f] && (
                  <text x={x + 4} y={GRAPH_H - 4} fill="rgba(235, 235, 245, 0.35)" fontSize={9}>
                    {GRID_LABELS[f]}
                  </text>
                )}
              </g>
            );
          })}
          {[-EQ_MAX_ABS_GAIN_DB / 2, EQ_MAX_ABS_GAIN_DB / 2].map((db) => (
            <line
              key={db}
              x1={0}
              y1={gainToY(db)}
              x2={GRAPH_W}
              y2={gainToY(db)}
              stroke="rgba(235, 235, 245, 0.05)"
              strokeWidth={1}
            />
          ))}
          {/* 0 dB line */}
          <line
            x1={0}
            y1={gainToY(0)}
            x2={GRAPH_W}
            y2={gainToY(0)}
            stroke="rgba(235, 235, 245, 0.18)"
            strokeWidth={1}
          />

          <SpectrumBackdrop blockId={blockId} />

          {/* Hover range highlight: a translucent, full-height band in the
            hovered band's own color, behind the curve/dots (Logic-style
            focus visualization). Width narrows with Q. */}
          {hoverHighlight && (
            <rect
              x={hoverHighlight.x}
              y={0}
              width={hoverHighlight.width}
              height={GRAPH_H}
              fill={EQ_BAND_COLORS[hovered! % EQ_BAND_COLORS.length]}
              opacity={0.14}
              style={{ pointerEvents: 'none' }}
            />
          )}

          {/* EQ curve + dots: dimmed AND inert while the EQ is bypassed
            (pointer-events off so the dots can't be grabbed). */}
          <g
            opacity={eqEnabled ? 1 : DISABLED_OPACITY}
            style={{ pointerEvents: eqEnabled ? undefined : 'none' }}
          >
            <path d={curve.area} fill={CURVE_COLOR} opacity={0.1} />
            <path d={curve.line} fill="none" stroke={CURVE_COLOR} strokeWidth={1.25} />

            {/* Band dots */}
            {bands.map((band, i) => {
              const role = roleOf(i);
              const cx = freqToNorm(band.freqHz) * GRAPH_W;
              const cy = hasGain(role) ? gainToY(band.gainDb) : gainToY(0);
              const isSelected = i === selected;
              const color = EQ_BAND_COLORS[i % EQ_BAND_COLORS.length];
              return (
                <g key={i}>
                  {isSelected && (
                    <circle
                      cx={cx}
                      cy={cy}
                      r={9}
                      fill="none"
                      stroke="#ffffff"
                      strokeWidth={1.5}
                      opacity={0.9}
                    />
                  )}
                  <circle
                    cx={cx}
                    cy={cy}
                    r={5.5}
                    fill={band.on ? (isSelected ? color : '#B8B8BE') : 'transparent'}
                    stroke={band.on ? '#000000' : color}
                    strokeWidth={1.5}
                    strokeDasharray={band.on ? undefined : '2 1.5'}
                    opacity={band.on ? 1 : 0.6}
                    style={{ pointerEvents: 'none' }}
                  />
                  {/* Invisible, larger hit target - the visible dot above is
                      deliberately small to stay legible with 8 of them on
                      one curve, but a hit area that size is fiddly to grab.
                      Double-click toggles bypass (same action as the strip
                      icon), matching KnobControl's own double-click
                      convention elsewhere in this editor. */}
                  <circle
                    cx={cx}
                    cy={cy}
                    r={12}
                    fill="transparent"
                    style={{ cursor: 'grab', touchAction: 'none' }}
                    {...helpProps(HELP.eqDot)}
                    onPointerEnter={() => setHovered(i)}
                    onPointerLeave={() => setHovered((h) => (h === i ? null : h))}
                    onPointerDown={handleDotPointerDown(i)}
                    onPointerMove={handleDotPointerMove(i)}
                    onPointerUp={handleDotPointerUp}
                    onPointerCancel={handleDotPointerUp}
                    onDoubleClick={() => toggleBandOn(i)}
                  />
                </g>
              );
            })}
          </g>
        </svg>
      </div>

      {/* Always-visible 8-band strip: every band's icon (bypass toggle) and
          Freq/Gain-or-Pole/Q, all drag-scrubbable in place, color-coded to
          its dot. No separate expanded per-band panel. */}
      <div
        className={uiOffClass(!eqEnabled)}
        style={{
          height: `${EQ_STRIP_H}rem`,
          flexShrink: 0,
          display: 'flex',
          flexDirection: 'row',
          alignItems: 'center',
          borderTop: '1rem solid rgba(235, 235, 245, 0.08)',
          padding: '4rem 1rem',
          boxSizing: 'border-box',
          transition: 'opacity 0.2s ease',
        }}
      >
        {bands.map((band, i) => renderStripChip(band, i))}
      </div>
    </div>
  );
};
