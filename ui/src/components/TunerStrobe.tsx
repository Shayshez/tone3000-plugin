import React, { useEffect, useRef } from 'react';
import { getUiScale } from '../hooks/useUiScale';
import { SURFACE_RAISED } from './theme';

/**
 * Strobe tuner display: bands of stripes that drift sideways at a speed
 * proportional to the pitch error - right when sharp, left when flat - and
 * stand still when the note is in tune. Far more precise to read than a
 * needle: even a cent or two shows as a slow, unmistakable crawl.
 *
 * Like a mechanical strobe's harmonic rings, each band below the first
 * halves the stripe period and doubles the speed, so the finest band makes
 * tiny errors obvious while the coarse top band stays readable when far
 * off. Animated on requestAnimationFrame from the latest reading (kept in a
 * ref), so motion stays smooth although readings arrive at ~20 Hz.
 */

/** Design-px stripe period of the top (coarsest) band. */
const BASE_PERIOD = 44;
/** Design px per second of drift per cent of error, top band. */
const SPEED_PER_CENT = 5;
/** Errors beyond this read as the maximum speed (the note name has moved on). */
const MAX_CENTS = 50;
const BAND_GAP = 6;

const LIT_BLUE = '#3D7BFF';

/** Manual rounded-rect path: CanvasRenderingContext2D.roundRect only exists
    from Safari 16, and the UI still boots on older system WebKits (see
    vite.config.ts's low build target). */
const roundedRectPath = (
  ctx: CanvasRenderingContext2D,
  x: number,
  y: number,
  w: number,
  h: number,
  r: number
) => {
  ctx.beginPath();
  ctx.moveTo(x + r, y);
  ctx.arcTo(x + w, y, x + w, y + h, r);
  ctx.arcTo(x + w, y + h, x, y + h, r);
  ctx.arcTo(x, y + h, x, y, r);
  ctx.arcTo(x, y, x + w, y, r);
  ctx.closePath();
};

export const TunerStrobe: React.FC<{
  cents: number;
  hasSignal: boolean;
  inTune: boolean;
  width: number;
  height: number;
  /** Stripe bands, coarsest first (3 on its own, 2 under the needle). */
  bands?: number;
}> = ({ cents, hasSignal, inTune, width, height, bands = 3 }) => {
  const canvasRef = useRef<HTMLCanvasElement>(null);
  const live = useRef({ cents, hasSignal, inTune });
  live.current = { cents, hasSignal, inTune };

  useEffect(() => {
    let frame = 0;
    let last = performance.now();
    let phase = 0; // design px, top band
    const draw = (now: number) => {
      frame = requestAnimationFrame(draw);
      const dt = Math.min(0.1, (now - last) / 1000);
      last = now;
      const canvas = canvasRef.current;
      const ctx = canvas?.getContext('2d');
      if (!canvas || !ctx) return;

      const { cents: c, hasSignal: signal, inTune: locked } = live.current;
      if (signal) {
        const clamped = Math.max(-MAX_CENTS, Math.min(MAX_CENTS, c));
        phase += clamped * SPEED_PER_CENT * dt;
      }

      // Real pixels: design size × UI scale × device pixel ratio.
      const scale = getUiScale() * (window.devicePixelRatio || 1);
      const pxW = Math.round(width * scale);
      const pxH = Math.round(height * scale);
      if (canvas.width !== pxW || canvas.height !== pxH) {
        canvas.width = pxW;
        canvas.height = pxH;
      }
      ctx.setTransform(scale, 0, 0, scale, 0, 0);
      ctx.clearRect(0, 0, width, height);

      const bandH = (height - BAND_GAP * (bands - 1)) / bands;
      const color = !signal ? SURFACE_RAISED : locked ? LIT_BLUE : 'rgba(255, 255, 255, 0.88)';
      for (let b = 0; b < bands; b++) {
        const period = BASE_PERIOD / 2 ** b;
        const offset = (((phase * 2 ** b) % period) + period) % period;
        const y = b * (bandH + BAND_GAP);
        ctx.save();
        roundedRectPath(ctx, 0, y, width, bandH, 6);
        ctx.clip();
        ctx.fillStyle = '#0c0c0e';
        ctx.fillRect(0, y, width, bandH);
        ctx.fillStyle = color;
        for (let x = offset - period; x < width; x += period) ctx.fillRect(x, y, period / 2, bandH);
        ctx.restore();
      }

      // Fade both ends into the background so stripes glide in and out
      // rather than popping at a hard edge.
      const fade = Math.min(90, width * 0.15);
      const left = ctx.createLinearGradient(0, 0, fade, 0);
      left.addColorStop(0, 'rgba(0,0,0,1)');
      left.addColorStop(1, 'rgba(0,0,0,0)');
      ctx.fillStyle = left;
      ctx.fillRect(0, 0, fade, height);
      const right = ctx.createLinearGradient(width - fade, 0, width, 0);
      right.addColorStop(0, 'rgba(0,0,0,0)');
      right.addColorStop(1, 'rgba(0,0,0,1)');
      ctx.fillStyle = right;
      ctx.fillRect(width - fade, 0, fade, height);
    };
    frame = requestAnimationFrame(draw);
    return () => cancelAnimationFrame(frame);
  }, [width, height, bands]);

  return (
    <canvas
      ref={canvasRef}
      aria-label="Strobe tuner"
      style={{ width: `${width}rem`, height: `${height}rem`, display: 'block' }}
    />
  );
};
