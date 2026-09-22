import React from 'react';
import { useTunerReading } from '../hooks/useTunerReading';
import { HELP, helpProps } from './helpText';
import { BRAND_RED, SURFACE_RAISED } from './theme';

const IN_TUNE_CENTS = 5;
// Cents deviation at which the needle reaches the end of its throw; beyond
// this it just pins at the edge rather than tracking further.
const MAX_NEEDLE_CENTS = 30;

// Lighter than theme's BRAND_BLUE (#0000FF) - the pure brand blue read as too
// dark/saturated for this small, always-on indicator.
const LIT_BLUE = '#3D7BFF';

// Wide, flat wedges (not tall triangles) per the reference hardware tuner:
// two tapered zones flanking a needle, each lighting up independently.
const WEDGE_W = 13;
const WEDGE_H = 8;
// Needle's half-throw from center, in rem; cents map linearly onto this.
const NEEDLE_THROW = 7;
const NEEDLE_W = 2;
const NEEDLE_H = 15;

// Same clipPath-over-track technique as TunerView's own `triangle` helper: a
// dim track shape stays put, the lit color sits on top and fades in/out, so
// an unlit direction still has a stable footprint.
const Wedge: React.FC<{ direction: 'left' | 'right'; lit: boolean }> = ({ direction, lit }) => {
  // Tip points at the needle: the left wedge's point faces right, the right
  // wedge's faces left.
  const clipPath =
    direction === 'left' ? 'polygon(0 0, 100% 50%, 0 100%)' : 'polygon(100% 0, 0 50%, 100% 100%)';
  return (
    <div style={{ position: 'relative', width: `${WEDGE_W}rem`, height: `${WEDGE_H}rem` }}>
      <div style={{ position: 'absolute', inset: 0, backgroundColor: SURFACE_RAISED, clipPath }} />
      <div
        style={{
          position: 'absolute',
          inset: 0,
          backgroundColor: LIT_BLUE,
          clipPath,
          opacity: lit ? 1 : 0,
          transition: 'opacity 90ms linear',
        }}
      />
    </div>
  );
};

/**
 * Always-on tuning-direction readout in the header, next to the button that
 * opens the full TunerView. No note name, no numbers - just a needle sliding
 * between two flanking wedges (reference: a hardware tuner's mini display):
 * the right wedge lights sharp, the left lights flat, both light in tune.
 * Native pitch detection runs continuously once enabled at startup (see
 * Plugin.tsx's setTunerEnabled call) - this and TunerView are just two views
 * onto the same reading via useTunerReading, independent of whether the full
 * screen is open.
 */
export const MiniTuner: React.FC = () => {
  const { cents, hasSignal } = useTunerReading();
  const inTune = hasSignal && Math.abs(cents) <= IN_TUNE_CENTS;
  const isFlat = hasSignal && cents < -IN_TUNE_CENTS;
  const isSharp = hasSignal && cents > IN_TUNE_CENTS;

  // 0 with no signal, so the needle rests dead center rather than wherever
  // the last real reading left it.
  const clampedCents = hasSignal
    ? Math.max(-MAX_NEEDLE_CENTS, Math.min(MAX_NEEDLE_CENTS, cents))
    : 0;
  const needleOffset = (clampedCents / MAX_NEEDLE_CENTS) * NEEDLE_THROW;

  return (
    <div
      {...helpProps(HELP.miniTuner)}
      style={{
        position: 'relative',
        display: 'flex',
        alignItems: 'center',
        justifyContent: 'center',
        gap: '2rem',
        width: '32rem',
        height: '28rem',
      }}
    >
      {/* Left wedge lights when flat (tune up) or in tune. */}
      <Wedge direction="left" lit={inTune || isFlat} />
      {/* Right wedge lights when sharp (tune down) or in tune. */}
      <Wedge direction="right" lit={inTune || isSharp} />
      {/* The needle itself, absolutely centered then translated by the live
          deviation so it reads as one continuous slide, not a swap between
          fixed positions. Taller than the wedges so it visibly crosses both,
          same as the reference. Always red, unlike the wedges - it's the one
          element that's always present, not an on/off state. */}
      <div
        style={{
          position: 'absolute',
          left: '50%',
          width: `${NEEDLE_W}rem`,
          height: `${NEEDLE_H}rem`,
          borderRadius: '1rem',
          backgroundColor: BRAND_RED,
          transform: `translateX(calc(-50% + ${needleOffset}rem))`,
          transition: 'transform 90ms linear',
        }}
      />
    </div>
  );
};
