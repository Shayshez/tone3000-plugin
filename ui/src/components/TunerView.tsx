import React, { useEffect, useRef } from 'react';
import { ChevronLeft, ChevronRight, RotateCcw, X as XIcon } from './icons';
import { useTunerReading } from '../hooks/useTunerReading';
import { useParameter } from '../hooks/useParameter';
import { TunerStrobe } from './TunerStrobe';
import { TunerNeedle } from './TunerNeedle';
import { isKeyboardOwned, isTypingTarget } from '../keyPassthrough';
import { SegmentedControl } from './controls';
import { ChromeTextButton } from './ChromeIconButton';
import { HELP, helpProps } from './helpText';
import { TileMenu } from './TileMenu';
import { useTileMenu } from '../hooks/useTileMenu';
import {
  TUNER_OFFSET_MAX,
  TUNER_OFFSET_MIN,
  TUNER_REF_MAX,
  TUNER_REF_MIN,
  setTunerDisplay,
  setTunerMute,
  setTunerOffset,
  setTunerRefHz,
  useTunerDisplay,
  useTunerMute,
  useTunerOffset,
  useTunerRefHz,
} from './uiPreferences';
import type { TunerDisplay } from './uiPreferences';
import {
  BRAND_BLUE,
  BRAND_RED,
  BRAND_YELLOW,
  FONT_MONO,
  GRAY,
  MUTED,
  SURFACE_RAISED,
  WHITE,
} from './theme';

/** Standard 6-string targets (MIDI, low E2 .. high E4). With a tuning
    offset the reading is already mapped back into this frame, so the row
    always reads EADGBE. */
const GUITAR_STRINGS = [40, 45, 50, 55, 59, 64];
const STRING_LABELS = ['E', 'A', 'D', 'G', 'B', 'E'];

/** Low-string name for each offset (flats for down-tunings, the usual
    naming: "E♭ Std", "D Std"...). */
const OFFSET_NAMES: Record<number, string> = {
  [-7]: 'A',
  [-6]: 'B♭',
  [-5]: 'B',
  [-4]: 'C',
  [-3]: 'D♭',
  [-2]: 'D',
  [-1]: 'E♭',
  0: 'E',
  1: 'F',
  2: 'F♯',
  3: 'G',
  4: 'A♭',
  5: 'A',
};
const offsetLabel = (offset: number) =>
  offset === 0 ? 'Standard' : `${OFFSET_NAMES[offset]} Std (${offset > 0 ? '+' : ''}${offset})`;

/** In-tune accents: text on a blue fill, and the note letter itself. */
const BLACK_TEXT = '#000000';
const BRAND_BLUE_TEXT = '#5B8CFF';

/** Quick picks in the reference-pitch menu. */
const REF_PRESETS = [432, 440, 442, 444];

const STROBE_WIDTH = 620;
const STROBE_HEIGHT = 132;

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

/**
 * A small value stepper for the settings bar: ‹ value › with the mouse
 * wheel stepping too (mouse-first: one hand on the guitar), and a
 * right-click menu for quick picks / reset.
 */
const Stepper: React.FC<{
  label: string;
  value: string;
  help: string;
  onStep: (delta: number) => void;
  canDec: boolean;
  canInc: boolean;
  menuItems: React.ComponentProps<typeof TileMenu>['items'];
  valueWidth: number;
}> = ({ label, value, help, onStep, canDec, canInc, menuItems, valueWidth }) => {
  const { menuAnchor, openMenu, closeMenu } = useTileMenu();
  const wheelRef = useRef<HTMLDivElement>(null);
  const stepRef = useRef(onStep);
  stepRef.current = onStep;
  useEffect(() => {
    const el = wheelRef.current;
    if (!el) return;
    // Native, non-passive: React's wheel listeners can't preventDefault.
    const onWheel = (e: WheelEvent) => {
      const d = e.deltaY !== 0 ? e.deltaY : e.deltaX;
      if (d === 0) return;
      e.preventDefault();
      stepRef.current(d < 0 ? 1 : -1);
    };
    el.addEventListener('wheel', onWheel, { passive: false });
    return () => el.removeEventListener('wheel', onWheel);
  }, []);
  const arrow = (enabled: boolean): React.CSSProperties => ({
    background: 'none',
    border: 'none',
    padding: '4rem',
    color: WHITE,
    cursor: enabled ? 'pointer' : 'not-allowed',
    opacity: enabled ? 1 : 0.35,
    display: 'flex',
    alignItems: 'center',
  });
  return (
    <div style={{ display: 'flex', alignItems: 'center', gap: '8rem' }}>
      <span style={{ fontSize: '11rem', fontWeight: 600, color: MUTED, letterSpacing: '0.06em' }}>
        {label}
      </span>
      <div
        ref={wheelRef}
        onContextMenu={openMenu}
        {...helpProps(help)}
        style={{
          display: 'flex',
          alignItems: 'center',
          background: '#0a0a0a',
          border: '1rem solid #3f3f46',
          borderRadius: '8rem',
          padding: '0 2rem',
        }}
      >
        <button disabled={!canDec} onClick={() => onStep(-1)} style={arrow(canDec)}>
          <ChevronLeft size={14} />
        </button>
        <span
          style={{
            width: `${valueWidth}rem`,
            textAlign: 'center',
            fontSize: '12rem',
            fontFamily: FONT_MONO,
            color: WHITE,
            whiteSpace: 'nowrap',
          }}
        >
          {value}
        </span>
        <button disabled={!canInc} onClick={() => onStep(1)} style={arrow(canInc)}>
          <ChevronRight size={14} />
        </button>
      </div>
      {menuAnchor && <TileMenu anchor={menuAnchor} onClose={closeMenu} items={menuItems} />}
    </div>
  );
};

export const TunerView: React.FC<{ onClose: () => void }> = ({ onClose }) => {
  const { note, midi, sounding, cents, frequency, hasSignal } = useTunerReading();
  const display = useTunerDisplay();
  const refHz = useTunerRefHz();
  const offset = useTunerOffset();
  const muteWhileTuning = useTunerMute();

  // Mute while tuning: engage the global Mute while this screen is open (if
  // it wasn't already on) and put it back on close / when the setting is
  // turned off. A mute the user had on before opening is left alone.
  const [outputMute, setOutputMute] = useParameter('outputMute', 'toggle');
  const muteAtOpen = useRef(outputMute);
  const setMuteRef = useRef(setOutputMute);
  setMuteRef.current = setOutputMute;
  useEffect(() => {
    if (!muteWhileTuning || muteAtOpen.current) return;
    setMuteRef.current(true);
    return () => setMuteRef.current(false);
  }, [muteWhileTuning]);

  // Esc closes the tuner - unless a menu/list or a text field has the key.
  const closeRef = useRef(onClose);
  closeRef.current = onClose;
  useEffect(() => {
    const onKey = (e: KeyboardEvent) => {
      if (e.key !== 'Escape' || isKeyboardOwned() || isTypingTarget(e.target)) return;
      e.preventDefault();
      closeRef.current();
    };
    window.addEventListener('keydown', onKey);
    return () => window.removeEventListener('keydown', onKey);
  }, []);
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

  // Strobe with ♭/♯ markers either side, lit toward the error.
  const strobeRow = (width: number, height: number, bands: number) => (
    <div style={{ display: 'flex', alignItems: 'center', gap: '18rem' }}>
      <span style={{ color: isFlat ? WHITE : SURFACE_RAISED, fontSize: '22rem' }}>♭</span>
      <TunerStrobe
        cents={cents}
        hasSignal={hasSignal}
        inTune={inTune}
        width={width}
        height={height}
        bands={bands}
      />
      <span style={{ color: isSharp ? WHITE : SURFACE_RAISED, fontSize: '22rem' }}>♯</span>
    </div>
  );

  // Big note letter + Hz/cents readout, shared by every display. Always
  // occupies its box (opacity 0 while idle) so nothing shifts on lock.
  const noteReadout = (fontSize: number) => (
    <div style={{ position: 'relative', opacity: hasSignal ? 1 : 0 }}>
      <div
        style={{
          fontSize: `${fontSize}rem`,
          lineHeight: 1,
          fontWeight: 700,
          color: inTune ? BRAND_BLUE_TEXT : WHITE,
          textAlign: 'center',
          userSelect: 'none',
          fontVariantNumeric: 'tabular-nums',
          transition: 'color 90ms linear',
        }}
      >
        {/* Letter wrapper is inline-block, so it stays centered; the
            accidental hangs off to the right (absolute) instead of shifting
            the letter off-center. */}
        <span style={{ position: 'relative', display: 'inline-block' }}>
          {note ? note.charAt(0) : '—'}
          {note && note.length > 1 && (
            <span style={{ position: 'absolute', left: '100%', top: '0.05em', fontSize: '0.35em' }}>
              {note.slice(1)}
            </span>
          )}
        </span>
      </div>
      {/* Pulled up into the line box's descender whitespace so it sits just
          under the visible letter; nowrap + centered so the readout can
          extend past the letter without wrapping or shifting it. */}
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
        {/* Own line: appended to the Hz/cents line it overran the center
            column into the bars. */}
        {offset !== 0 && sounding && (
          <div style={{ fontSize: '11rem', marginTop: '2rem' }}>sounding {sounding}</div>
        )}
      </div>
    </div>
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
      {/* Guitar strings, always EADGBE (the offset maps readings back into
          standard): the one being played lights up, blue once in tune. */}
      <div
        style={{
          position: 'absolute',
          top: '18rem',
          left: '50%',
          transform: 'translateX(-50%)',
          display: 'flex',
          gap: '14rem',
          fontFamily: FONT_MONO,
          fontSize: '15rem',
          fontWeight: 600,
          userSelect: 'none',
        }}
      >
        {GUITAR_STRINGS.map((target, i) => {
          const active = hasSignal && midi === target;
          return (
            <span
              key={target}
              style={{
                width: '26rem',
                height: '26rem',
                display: 'flex',
                alignItems: 'center',
                justifyContent: 'center',
                borderRadius: '13rem',
                color: active ? (inTune ? BLACK_TEXT : WHITE) : GRAY,
                background: active
                  ? inTune
                    ? BRAND_BLUE
                    : 'rgba(255,255,255,0.14)'
                  : 'transparent',
                transition: 'background 90ms linear, color 90ms linear',
              }}
            >
              {STRING_LABELS[i]}
            </span>
          );
        })}
      </div>

      {display === 'bars' ? (
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
              gap: '28rem',
              minWidth: '120rem',
            }}
          >
            {/* Top triangle points down: lit when sharp ("tune down") or in tune */}
            {triangle('down', inTune || isSharp)}
            {noteReadout(110)}
            {/* Bottom triangle points up: lit when flat ("tune up") or in tune */}
            {triangle('up', inTune || isFlat)}
          </div>

          {renderBars('right', rightLit)}
        </div>
      ) : display === 'strobe' ? (
        <div
          style={{
            display: 'flex',
            flexDirection: 'column',
            alignItems: 'center',
            gap: '30rem',
            marginTop: '-10rem',
          }}
        >
          {noteReadout(76)}
          {strobeRow(STROBE_WIDTH, STROBE_HEIGHT, 3)}
        </div>
      ) : display === 'needle' ? (
        <div
          style={{
            display: 'flex',
            flexDirection: 'column',
            alignItems: 'center',
            gap: '4rem',
            marginTop: '-16rem',
          }}
        >
          <TunerNeedle
            cents={cents}
            hasSignal={hasSignal}
            inTune={inTune}
            inTuneCents={IN_TUNE_CENTS}
            radius={300}
            width={560}
          />
          {noteReadout(84)}
        </div>
      ) : (
        // Combo (Fractal-style): the needle for the coarse approach, a
        // two-band strobe under it for the last cent.
        <div
          style={{
            display: 'flex',
            flexDirection: 'column',
            alignItems: 'center',
            gap: '4rem',
            marginTop: '-22rem',
          }}
        >
          <TunerNeedle
            cents={cents}
            hasSignal={hasSignal}
            inTune={inTune}
            inTuneCents={IN_TUNE_CENTS}
            radius={230}
            width={500}
          />
          {noteReadout(60)}
          <div style={{ marginTop: '28rem' }}>{strobeRow(500, 56, 2)}</div>
        </div>
      )}

      {/* Settings: display mode, reference pitch, tuning offset, mute. */}
      <div
        style={{
          position: 'absolute',
          bottom: '16rem',
          left: '50%',
          transform: 'translateX(-50%)',
          display: 'flex',
          alignItems: 'center',
          gap: '28rem',
          whiteSpace: 'nowrap',
        }}
      >
        <SegmentedControl<TunerDisplay>
          value={display}
          options={[
            { value: 'bars', label: 'BARS' },
            { value: 'needle', label: 'NEEDLE' },
            { value: 'strobe', label: 'STROBE' },
            { value: 'combo', label: 'COMBO' },
          ]}
          onChange={setTunerDisplay}
          ariaLabel="Tuner display"
        />
        <Stepper
          label="A4"
          value={`${refHz} Hz`}
          help={HELP.tunerRef}
          valueWidth={56}
          canDec={refHz > TUNER_REF_MIN}
          canInc={refHz < TUNER_REF_MAX}
          onStep={(d) => setTunerRefHz(Math.min(TUNER_REF_MAX, Math.max(TUNER_REF_MIN, refHz + d)))}
          menuItems={[
            ...REF_PRESETS.map((hz) => ({
              label: `${hz} Hz`,
              icon: <span style={{ width: '16rem' }}>{hz === refHz ? '✓' : ''}</span>,
              help: HELP.tunerRef,
              onSelect: () => setTunerRefHz(hz),
            })),
          ]}
        />
        <Stepper
          label="TUNING"
          value={offsetLabel(offset)}
          help={HELP.tunerOffset}
          valueWidth={112}
          canDec={offset > TUNER_OFFSET_MIN}
          canInc={offset < TUNER_OFFSET_MAX}
          onStep={(d) =>
            setTunerOffset(Math.min(TUNER_OFFSET_MAX, Math.max(TUNER_OFFSET_MIN, offset + d)))
          }
          menuItems={[
            {
              label: 'Standard',
              icon: <RotateCcw size={16} />,
              help: HELP.tunerOffset,
              onSelect: () => setTunerOffset(0),
            },
            ...[-1, -2].map((o) => ({
              label: offsetLabel(o),
              icon: <span style={{ width: '16rem' }}>{o === offset ? '✓' : ''}</span>,
              help: HELP.tunerOffset,
              onSelect: () => setTunerOffset(o),
            })),
          ]}
        />
        <ChromeTextButton
          armed={muteWhileTuning}
          help={HELP.tunerMute}
          onClick={() => setTunerMute(!muteWhileTuning)}
        >
          MUTE
        </ChromeTextButton>
      </div>
    </div>
  );
};
