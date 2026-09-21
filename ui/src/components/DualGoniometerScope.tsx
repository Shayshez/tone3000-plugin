import React, { useEffect, useRef } from 'react';
import { useAudioBackend } from '../hooks/useAudioBackend';
import { rem, getUiScale } from '../hooks/useUiScale';
import { BORDER } from './theme';

/**
 * X-Y scope (goniometer) of a Dual Mono block's true final output (post
 * Align/Ø/Pan/Width/EQ - see BlockGoniometer.h) - the in-plugin equivalent
 * of the external correlation meter the user was reading while dialing in
 * Align/Ø/Pan. Mid (L+R) plots up the center, Side (L-R) across it (the
 * standard 45-degree-rotated convention): fully correlated/mono content
 * reads as a vertical line, decorrelated/out-of-phase content spreads into
 * a wide or horizontal blob.
 *
 * Canvas, not SVG (unlike every other graphic in this app - WaveformDisplay,
 * IrEnvelopeGraph): this redraws a few hundred points per frame at ~30 fps
 * with a fading trail, which SVG's retained-mode DOM can't do cheaply.
 * Drawing is fully imperative (a plain useEffect + rAF loop, no React state
 * for the points themselves) so the poll/redraw cycle never re-renders this
 * component's own subtree.
 *
 * Enables the native capture on mount, disables on unmount - same
 * "only capture while the view showing it is actually open" contract as
 * BlockSpectrum/useBlockSpectrum.ts.
 */
export const DualGoniometerScope: React.FC<{ blockId: string; size?: number }> = ({
  blockId,
  size = 220,
}) => {
  const backend = useAudioBackend();
  const canvasRef = useRef<HTMLCanvasElement | null>(null);

  useEffect(() => {
    const setEnabled = backend.getPluginFunction('setDualGoniometerEnabled');
    const getPoints = backend.getPluginFunction('getDualGoniometer');
    const canvas = canvasRef.current;
    if (!canvas) return;
    const ctx = canvas.getContext('2d');
    if (!ctx) return;

    // Drawing math below stays in design-space units (0..size, matching
    // every other rem-authored dimension in this app - see useUiScale.ts);
    // one combined scale factor (current UI scale * device pixel ratio)
    // maps that space onto the canvas's actual physical pixel buffer, same
    // as CSS rem already does for every other element.
    const dpr = window.devicePixelRatio || 1;
    const pixelsPerDesignPx = getUiScale() * dpr;
    canvas.width = size * pixelsPerDesignPx;
    canvas.height = size * pixelsPerDesignPx;
    ctx.scale(pixelsPerDesignPx, pixelsPerDesignPx);

    Promise.resolve(setEnabled(blockId, true)).catch((error) =>
      console.error('setDualGoniometerEnabled failed:', error)
    );

    const CENTER = size / 2;
    // Trail persistence: how much of the previous frame survives each
    // redraw (0 = no trail/instant clear, closer to 1 = long trail). Low,
    // matching a real scope's long phosphor-style decay (DigiCheck's own
    // reference look) rather than a fast wipe.
    const FADE_ALPHA = 0.045;
    // Auto-gain: typical program material peaks well under 1.0 (guitar amp
    // captures commonly sit around 0.2-0.4), so a fixed scale assuming
    // near-unity content left the trace small and dim in the middle of the
    // frame - DigiCheck auto-ranges to fill its own display, this should
    // too. Tracks a slow-release envelope of the recent peak magnitude and
    // scales so that envelope reaches MAX_RADIUS_FRACTION of the frame -
    // fast attack (a loud transient should snap the scale out immediately),
    // slow release (quiet content shouldn't visibly shrink/rescale on every
    // frame, only ease back down once it's genuinely stayed quiet).
    // Backed off from 0.46: at that fraction, loud transients' auto-gain
    // pushed points close enough to the frame edge to read as clipping
    // against the border - this leaves real headroom instead.
    const MAX_RADIUS_FRACTION = 0.38;
    const MIN_PEAK = 0.03;
    const ENV_ATTACK = 0.35;
    const ENV_RELEASE = 0.01;
    let peakEnvelope = 0.3;

    // The per-frame alpha fade never actually reaches a true-black pixel:
    // once a channel decays to canvas's 8-bit quantum 1, 1 * (1-FADE_ALPHA)
    // rounds back up to 1 forever (any alpha <= 0.5 has this floor), so old
    // content "burns in" as a faint stain that outlives its own trail by
    // minutes. A periodic hard clear bounds how long any stuck pixel can
    // possibly survive, instead of relying on the exponential fade alone.
    const HARD_CLEAR_INTERVAL_MS = 2000;
    let lastHardClear = 0;

    let alive = true;
    let polling = false;
    let rafId = 0;

    const drawGrid = () => {
      ctx.strokeStyle = BORDER;
      ctx.lineWidth = 1;
      ctx.beginPath();
      // Crosshair.
      ctx.moveTo(CENTER, 0);
      ctx.lineTo(CENTER, size);
      ctx.moveTo(0, CENTER);
      ctx.lineTo(size, CENTER);
      // Diagonal guides at the un-rotated L/R axes.
      ctx.moveTo(0, 0);
      ctx.lineTo(size, size);
      ctx.moveTo(size, 0);
      ctx.lineTo(0, size);
      ctx.stroke();
    };

    const draw = (flat: number[]) => {
      // Fade the previous frame instead of clearing outright - the trail is
      // what makes this read as a continuous scope rather than a sparse,
      // flickering dot cloud. Plain 'source-over' for the fade itself (not
      // 'lighter' - fading must darken, not brighten).
      ctx.globalCompositeOperation = 'source-over';
      const now = performance.now();
      if (now - lastHardClear > HARD_CLEAR_INTERVAL_MS) {
        lastHardClear = now;
        ctx.clearRect(0, 0, size, size);
      } else {
        ctx.fillStyle = `rgba(0, 0, 0, ${FADE_ALPHA})`;
        ctx.fillRect(0, 0, size, size);
      }
      drawGrid();

      if (flat.length >= 2) {
        let framePeak = 0;
        for (let i = 0; i < flat.length; ++i) framePeak = Math.max(framePeak, Math.abs(flat[i]));
        peakEnvelope +=
          (framePeak - peakEnvelope) * (framePeak > peakEnvelope ? ENV_ATTACK : ENV_RELEASE);
        const scale = (size * MAX_RADIUS_FRACTION) / Math.max(peakEnvelope, MIN_PEAK);

        // 'lighter' (additive) blending with a semi-transparent dot: dense
        // clusters of overlapping points genuinely brighten rather than
        // just re-painting the same flat opaque color, matching a real
        // scope's glow (and DigiCheck's own reference look) instead of a
        // dim, uniform dot cloud.
        ctx.globalCompositeOperation = 'lighter';
        ctx.fillStyle = 'rgba(255, 255, 0, 0.55)';
        for (let i = 0; i + 1 < flat.length; i += 2) {
          const l = flat[i];
          const r = flat[i + 1];
          // Mid up the center, Side across it - the standard rotated
          // goniometer convention (mono collapses to a vertical line).
          const mid = (l + r) * 0.70710678;
          const side = (l - r) * 0.70710678;
          // Negated (not "+ side") to match DigiCheck's own left/right
          // orientation - confirmed side by side against it.
          const x = CENTER - side * scale;
          const y = CENTER - mid * scale;
          ctx.fillRect(x, y, 2, 2);
        }
      }
    };

    const tick = async () => {
      if (!alive) return;
      if (!polling) {
        polling = true;
        try {
          const res = await getPoints(blockId);
          if (alive && Array.isArray(res)) draw(res as number[]);
        } catch (error) {
          console.error('getDualGoniometer failed:', error);
        } finally {
          polling = false;
        }
      }
      if (alive) rafId = requestAnimationFrame(() => void tick());
    };
    void tick();

    return () => {
      alive = false;
      cancelAnimationFrame(rafId);
      Promise.resolve(setEnabled(blockId, false)).catch(() => undefined);
    };
  }, [backend, blockId, size]);

  return (
    <canvas
      ref={canvasRef}
      style={{
        width: rem(size),
        height: rem(size),
        borderRadius: '8rem',
        border: BORDER,
        display: 'block',
      }}
    />
  );
};
