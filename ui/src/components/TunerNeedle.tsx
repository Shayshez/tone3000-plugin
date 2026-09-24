import React, { useEffect, useRef } from 'react';
import { BRAND_RED, GRAY, MUTED, WHITE } from './theme';

/**
 * Flat, full-width needle meter (modern hardware-tuner style rather than a
 * skeuomorphic VU arc, matching the rest of the plugin's flat language): a
 * ±50 cent linear scale with ticks, a blue in-tune zone around center, and a
 * red needle - the one analog cue kept on purpose - that glides on a
 * critically-damped spring (rAF, written straight to the SVG transform, no
 * React re-render per frame) so it moves like a real meter between the
 * ~20 Hz readings.
 *
 * `overlay` draws only the needle and center line (transparent, no scale),
 * for laying it over the strobe band in the COMBO display, the way a
 * Fractal-style tuner rides its marker across the strobe.
 */

const MAX_CENTS = 50;
/** Spring stiffness (1/s): higher = snappier. */
const SPRING = 14;
/** Label row above the scale (the pointer head sits between it and the ticks). */
const LABEL_H = 24;
/** Keeps the ±50 end ticks and their labels inside the box. */
const SIDE_PAD = 12;
const NEEDLE_RED = BRAND_RED;
/** The tuner's shared in-tune blue (strobe stripes, note letter, strings):
    lighter than BRAND_BLUE, which read as a heavy navy block here. */
const LIT_BLUE = '#3D7BFF';
/** Overlay: how far the needle pokes past the band's top and bottom edges. */
const OVERHANG = 6;

export const TunerNeedle: React.FC<{
  cents: number;
  hasSignal: boolean;
  inTune: boolean;
  inTuneCents: number;
  width: number;
  /** Height of the scale (overlay: of the band it sits on). */
  height: number;
  overlay?: boolean;
}> = ({ cents, hasSignal, inTune, inTuneCents, width: W, height: H, overlay = false }) => {
  const top = overlay ? 0 : LABEL_H;
  const totalH = top + H;
  const half = W / 2 - SIDE_PAD;
  const xOf = (c: number) => W / 2 + (c / MAX_CENTS) * half;

  const needleRef = useRef<SVGGElement>(null);
  const target = useRef(0);
  target.current = hasSignal ? Math.max(-MAX_CENTS, Math.min(MAX_CENTS, cents)) : 0;

  useEffect(() => {
    let frame = 0;
    let last = performance.now();
    let pos = 0; // cents
    const tick = (now: number) => {
      frame = requestAnimationFrame(tick);
      const dt = Math.min(0.1, (now - last) / 1000);
      last = now;
      pos += (target.current - pos) * (1 - Math.exp(-SPRING * dt));
      const dx = (pos / MAX_CENTS) * half;
      needleRef.current?.setAttribute('transform', `translate(${dx.toFixed(2)} 0)`);
    };
    frame = requestAnimationFrame(tick);
    return () => cancelAnimationFrame(frame);
  }, [half]);

  const ticks: React.ReactNode[] = [];
  if (!overlay) {
    for (let c = -MAX_CENTS; c <= MAX_CENTS; c += 5) {
      const major = c % 25 === 0;
      const center = c === 0;
      const len = center ? H : major ? H * 0.62 : H * 0.34;
      const x = xOf(c);
      ticks.push(
        <line
          key={c}
          x1={x}
          x2={x}
          y1={top + H - len}
          y2={top + H}
          stroke={center ? WHITE : major ? MUTED : 'rgba(235,235,245,0.32)'}
          strokeWidth={center ? 2 : major ? 1.5 : 1}
        />
      );
      if (major)
        ticks.push(
          <text
            key={`l${c}`}
            x={x}
            y={8}
            fill={GRAY}
            fontSize={11}
            textAnchor="middle"
            dominantBaseline="middle"
            style={{ fontFamily: 'inherit' }}
          >
            {c > 0 ? `+${c}` : c}
          </text>
        );
    }
  }

  const zoneX = xOf(-inTuneCents);
  const zoneW = xOf(inTuneCents) - zoneX;

  return (
    <svg
      viewBox={`0 0 ${W} ${totalH}`}
      style={{ width: `${W}rem`, height: `${totalH}rem`, display: 'block', overflow: 'visible' }}
      aria-label="Needle tuner"
    >
      {!overlay && (
        <>
          {/* In-tune zone: always marked, lit when locked. */}
          <rect
            x={zoneX}
            y={top}
            width={zoneW}
            height={H}
            rx={3}
            fill={LIT_BLUE}
            opacity={hasSignal && inTune ? 0.4 : 0.12}
            style={{ transition: 'opacity 120ms linear' }}
          />
          <line x1={xOf(-MAX_CENTS)} x2={xOf(MAX_CENTS)} y1={top + H} y2={top + H} stroke={MUTED} />
          {ticks}
        </>
      )}
      {overlay && (
        // Center reference as notches just outside the band: a line drawn
        // across the strobe vanished among its white stripes.
        <>
          <path
            d={`M ${W / 2 - 6} ${-OVERHANG - 8} L ${W / 2 + 6} ${-OVERHANG - 8} L ${W / 2} ${-OVERHANG - 1} Z`}
            fill={WHITE}
          />
          <path
            d={`M ${W / 2 - 6} ${H + OVERHANG + 8} L ${W / 2 + 6} ${H + OVERHANG + 8} L ${W / 2} ${H + OVERHANG + 1} Z`}
            fill={WHITE}
          />
        </>
      )}
      <g ref={needleRef}>
        <g
          opacity={hasSignal ? 1 : 0.3}
          style={{
            filter: hasSignal ? `drop-shadow(0 0 4rem ${NEEDLE_RED})` : 'none',
            transition: 'opacity 150ms linear',
          }}
        >
          <rect
            x={W / 2 - 1.5}
            y={overlay ? -OVERHANG : top}
            width={3}
            height={overlay ? H + OVERHANG * 2 : H}
            rx={1.5}
            fill={NEEDLE_RED}
          />
          {/* Pointer head just above the scale, under the labels. */}
          {!overlay && (
            <path
              d={`M ${W / 2 - 6} ${top - 9} L ${W / 2 + 6} ${top - 9} L ${W / 2} ${top - 1} Z`}
              fill={NEEDLE_RED}
            />
          )}
        </g>
      </g>
    </svg>
  );
};
