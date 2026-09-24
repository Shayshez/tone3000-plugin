import React, { useEffect, useRef } from 'react';
import { BRAND_BLUE, GRAY, MUTED, WHITE } from './theme';

/**
 * Analog needle meter: a ±50 cent arc with ticks, a blue in-tune zone, and
 * a needle that swings with a critically-damped spring (rAF, written straight
 * to the SVG transform, no React re-render per frame) - so it glides like a
 * real meter instead of stepping at the ~20 Hz reading rate. Only the outer
 * segment of the needle is drawn: the pivot sits below the visible area, so
 * the meter stays short and nothing crosses the note name under it.
 */

const MAX_CENTS = 50;
/** Arc half-span in degrees for ±MAX_CENTS. */
const SPREAD_DEG = 45;
/** Inner end of the drawn needle, as a fraction of the radius. */
const NEEDLE_INNER = 0.74;
/** Spring stiffness (1/s): higher = snappier. */
const SPRING = 14;

const LIT_BLUE = '#3D7BFF';

const polar = (cx: number, cy: number, r: number, deg: number) => {
  const rad = (deg * Math.PI) / 180;
  return { x: cx + r * Math.sin(rad), y: cy - r * Math.cos(rad) };
};

export const TunerNeedle: React.FC<{
  cents: number;
  hasSignal: boolean;
  inTune: boolean;
  inTuneCents: number;
  /** Arc radius in design px; the meter's size follows from it. */
  radius: number;
  width: number;
}> = ({ cents, hasSignal, inTune, inTuneCents, radius: R, width: W }) => {
  const pad = 26; // room for the tick labels above the arc
  const cx = W / 2;
  const cy = pad + R;
  const height = Math.ceil(pad + R * (1 - NEEDLE_INNER) + 6);

  const needleRef = useRef<SVGGElement>(null);
  const target = useRef(0);
  target.current = hasSignal ? Math.max(-MAX_CENTS, Math.min(MAX_CENTS, cents)) : 0;

  useEffect(() => {
    let frame = 0;
    let last = performance.now();
    let angle = 0;
    const tick = (now: number) => {
      frame = requestAnimationFrame(tick);
      const dt = Math.min(0.1, (now - last) / 1000);
      last = now;
      const goal = (target.current / MAX_CENTS) * SPREAD_DEG;
      angle += (goal - angle) * (1 - Math.exp(-SPRING * dt));
      needleRef.current?.setAttribute('transform', `rotate(${angle.toFixed(3)} ${cx} ${cy})`);
    };
    frame = requestAnimationFrame(tick);
    return () => cancelAnimationFrame(frame);
  }, [cx, cy]);

  const arcPath = (fromDeg: number, toDeg: number, r: number) => {
    const a = polar(cx, cy, r, fromDeg);
    const b = polar(cx, cy, r, toDeg);
    return `M ${a.x} ${a.y} A ${r} ${r} 0 0 1 ${b.x} ${b.y}`;
  };
  const zoneDeg = (inTuneCents / MAX_CENTS) * SPREAD_DEG;

  const ticks: React.ReactNode[] = [];
  for (let c = -MAX_CENTS; c <= MAX_CENTS; c += 5) {
    const deg = (c / MAX_CENTS) * SPREAD_DEG;
    const major = c % 25 === 0;
    const a = polar(cx, cy, R - (major ? 14 : 7), deg);
    const b = polar(cx, cy, R, deg);
    ticks.push(
      <line
        key={c}
        x1={a.x}
        y1={a.y}
        x2={b.x}
        y2={b.y}
        stroke={c === 0 ? WHITE : MUTED}
        strokeWidth={major ? 2 : 1}
        strokeLinecap="round"
      />
    );
    if (major) {
      const t = polar(cx, cy, R + 13, deg);
      ticks.push(
        <text
          key={`l${c}`}
          x={t.x}
          y={t.y}
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

  const needleColor = !hasSignal ? 'rgba(255,255,255,0.25)' : inTune ? LIT_BLUE : WHITE;
  const inner = polar(cx, cy, R * NEEDLE_INNER, 0);
  const outer = polar(cx, cy, R + 4, 0);

  return (
    <svg
      viewBox={`0 0 ${W} ${height}`}
      style={{ width: `${W}rem`, height: `${height}rem`, display: 'block', overflow: 'visible' }}
      aria-label="Needle tuner"
    >
      <path
        d={arcPath(-SPREAD_DEG, SPREAD_DEG, R)}
        stroke="rgba(235,235,245,0.18)"
        strokeWidth={2}
        fill="none"
      />
      <path
        d={arcPath(-zoneDeg, zoneDeg, R)}
        stroke={BRAND_BLUE}
        strokeWidth={6}
        strokeLinecap="round"
        fill="none"
        opacity={hasSignal && inTune ? 1 : 0.55}
      />
      {ticks}
      <g ref={needleRef}>
        <line
          x1={inner.x}
          y1={inner.y}
          x2={outer.x}
          y2={outer.y}
          stroke={needleColor}
          strokeWidth={3}
          strokeLinecap="round"
          style={{ transition: 'stroke 90ms linear' }}
        />
      </g>
    </svg>
  );
};
