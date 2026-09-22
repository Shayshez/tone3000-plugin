import React from 'react';
import { X as XIcon } from './icons';
import { useTunerReading } from '../hooks/useTunerReading';
import {
  BRAND_BLUE,
  BRAND_RED,
  BRAND_YELLOW,
  FONT_MONO,
  GRAY,
  SURFACE_RAISED,
  WHITE,
} from './theme';

// Cents window considered "in tune" and the full deflection of one side.
const IN_TUNE_CENTS = 5;
const MAX_CENTS = 50;

// Bar colors from the center outward (blue → yellow → red), per screenshot.
const SIDE_COLORS = [BRAND_BLUE, BRAND_YELLOW, BRAND_YELLOW, BRAND_RED, BRAND_RED, BRAND_RED];

// Tapered panel geometry from the idle-state SVG (50×181 with the short
// inner edge running from y=30 to y=151). Gap is 16 so six bars land at
// the mockup's 377-wide track.
const BAR_WIDTH = 50;
const BAR_HEIGHT = 181;
const BAR_GAP = 16;
const BAR_TAPER_TOP = (30 / 181) * 100;
const BAR_TAPER_BOTTOM = (151 / 181) * 100;

// Number of bars lit on the deflection side: blue only near in-tune, all six
// red at a half-semitone off.
const litCountForCents = (absCents: number): number => {
  if (absCents <= IN_TUNE_CENTS) return 1;
  const t = Math.min(1, (absCents - IN_TUNE_CENTS) / (MAX_CENTS - IN_TUNE_CENTS));
  return Math.min(6, 1 + Math.floor(t * 5 + 0.5));
};

export const TunerView: React.FC<{ onClose: () => void }> = ({ onClose }) => {
  const { note, cents, frequency, hasSignal } = useTunerReading();
  const absCents = Math.abs(cents);
  const roundedCents = Math.round(cents);
  const inTune = hasSignal && absCents <= IN_TUNE_CENTS;
  const isFlat = hasSignal && cents < -IN_TUNE_CENTS;
  const isSharp = hasSignal && cents > IN_TUNE_CENTS;

  // Flat lights the left side, sharp the right; in tune lights both blues.
  const leftLit = !hasSignal ? 0 : inTune ? 1 : isFlat ? litCountForCents(absCents) : 0;
  const rightLit = !hasSignal ? 0 : inTune ? 1 : isSharp ? litCountForCents(absCents) : 0;

  const renderBars = (side: 'left' | 'right', litCount: number) => {
    // Bars ordered outermost → innermost for the left side, mirrored for right.
    const indices = side === 'left' ? [5, 4, 3, 2, 1, 0] : [0, 1, 2, 3, 4, 5];
    // Tapered panel from the idle-state SVG (50×181: full-height outer edge,
    // inner edge running 30→151). The short edge faces the center, so both
    // sides read as receding toward the note.
    const clipPath =
      side === 'left'
        ? `polygon(0 0, 100% ${BAR_TAPER_TOP}%, 100% ${BAR_TAPER_BOTTOM}%, 0 100%)`
        : `polygon(100% 0, 0 ${BAR_TAPER_TOP}%, 0 ${BAR_TAPER_BOTTOM}%, 100% 100%)`;
    return (
      <div
        style={{
          display: 'flex',
          flexDirection: 'row',
          alignItems: 'center',
          gap: `${BAR_GAP}rem`,
        }}
      >
        {indices.map((i) => {
          const lit = i < litCount;
          return (
            <div
              key={i}
              style={{
                position: 'relative',
                width: `${BAR_WIDTH}rem`,
                height: `${BAR_HEIGHT}rem`,
                flexShrink: 0,
              }}
            >
              {/* Grey track stays put; color sits on top only while that
                  segment is lit so unlit slots never flash or vanish. */}
              <div
                style={{
                  position: 'absolute',
                  inset: 0,
                  backgroundColor: SURFACE_RAISED,
                  clipPath,
                }}
              />
              <div
                style={{
                  position: 'absolute',
                  inset: 0,
                  backgroundColor: SIDE_COLORS[i],
                  clipPath,
                  opacity: lit ? 1 : 0,
                }}
              />
            </div>
          );
        })}
      </div>
    );
  };

  // 61×53 triangle per the reference SVG (wider than tall, point centered).
  const triangle = (direction: 'up' | 'down', lit: boolean) => (
    <div
      style={{
        width: '61rem',
        height: '53rem',
        backgroundColor: BRAND_BLUE,
        clipPath:
          direction === 'up'
            ? 'polygon(50% 0, 100% 100%, 0 100%)'
            : 'polygon(0 0, 100% 0, 50% 100%)',
        opacity: lit ? 1 : 0,
        transition: 'opacity 90ms linear',
      }}
    />
  );

  return (
    <div
      style={{
        position: 'relative',
        flex: 1,
        width: '100%',
        minHeight: 0,
        display: 'flex',
        alignItems: 'center',
        justifyContent: 'center',
        backgroundColor: '#000000',
        overflow: 'hidden',
      }}
    >
      {/* Redundant close affordance mirroring Settings' top-right X. */}
      <button
        onClick={onClose}
        aria-label="Close tuner"
        style={{
          position: 'absolute',
          top: '16rem',
          right: '20rem',
          background: 'transparent',
          border: 'none',
          color: '#ffffff',
          cursor: 'pointer',
          display: 'flex',
          alignItems: 'center',
          padding: '4rem',
          zIndex: 1,
        }}
      >
        <XIcon size={20} />
      </button>
      <div
        style={{
          display: 'flex',
          flexDirection: 'row',
          alignItems: 'center',
          justifyContent: 'center',
          gap: '32rem',
        }}
      >
        {renderBars('left', leftLit)}

        {/* Note + triangles always occupy the center so the grey track
            doesn't shift when a pitch locks or drops. Idle is opacity 0. */}
        <div
          style={{
            display: 'flex',
            flexDirection: 'column',
            alignItems: 'center',
            justifyContent: 'center',
            gap: '36rem',
            minWidth: '120rem',
          }}
        >
          {/* Top triangle points down: lit when sharp ("tune down") or in tune */}
          {triangle('down', inTune || isSharp)}
          <div
            style={{
              position: 'relative',
              opacity: hasSignal ? 1 : 0,
            }}
          >
            <div
              style={{
                fontSize: '110rem',
                lineHeight: 1,
                fontWeight: 700,
                color: WHITE,
                textAlign: 'center',
                userSelect: 'none',
                fontVariantNumeric: 'tabular-nums',
              }}
            >
              {/* Letter wrapper is inline-block, so it stays centered under
                  the triangles; the accidental hangs off to the right
                  (absolute) instead of shifting the letter off-center. */}
              <span style={{ position: 'relative', display: 'inline-block' }}>
                {note ? note.charAt(0) : '—'}
                {note && note.length > 1 && (
                  <span
                    style={{
                      position: 'absolute',
                      left: '100%',
                      top: '0.05em',
                      fontSize: '0.35em',
                    }}
                  >
                    {note.slice(1)}
                  </span>
                )}
              </span>
            </div>
            {/* Pulled up into the line box's descender whitespace so it sits
                just under the visible letter, not down by the triangle.
                nowrap + centered so "329.6 Hz  −12¢" can extend past the
                letter without wrapping or shifting it. */}
            <div
              style={{
                position: 'absolute',
                top: '100%',
                left: '50%',
                transform: 'translateX(-50%)',
                marginTop: '-6rem',
                fontSize: '13rem',
                fontWeight: 400,
                fontFamily: FONT_MONO,
                textAlign: 'center',
                color: GRAY,
                whiteSpace: 'nowrap',
              }}
            >
              {`${frequency.toFixed(1)} Hz  ${roundedCents >= 0 ? '+' : ''}${roundedCents}¢`}
            </div>
          </div>
          {/* Bottom triangle points up: lit when flat ("tune up") or in tune */}
          {triangle('up', inTune || isFlat)}
        </div>

        {renderBars('right', rightLit)}
      </div>
    </div>
  );
};
