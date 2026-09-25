import { useEffect, useRef, useState } from 'react';
import { useNativeFunction } from './useFunction';
import { getTunerOffset, getTunerRefHz } from '../components/uiPreferences';
import { frequencyToPitch, pitchToFrequency, pitchToNote } from '../types/tunerMath';
import { TunerSmoother } from '../types/tunerSmoothing';

interface TunerReading {
  frequency: number;
  confidence: number;
  level: number;
}

const POLL_MS = 50;
// Hold the last note on screen briefly after the signal decays.
const HOLD_MS = 900;

export interface TunerState {
  /** Note name in the (offset-adjusted) standard frame, e.g. "E". */
  note: string | null;
  /** That note's MIDI number (same frame) - picks the guitar string. */
  midi: number | null;
  /** The pitch actually sounding (differs from `note` when offset != 0). */
  sounding: string | null;
  cents: number;
  frequency: number;
  hasSignal: boolean;
}

/**
 * Polls the native pitch detector at 20 Hz and smooths the result into a
 * stable note/cents reading (attack gate + median + glide, see
 * TunerSmoother). Shared by the full TunerView and the always-on
 * mini tuner in the header - both just read whatever the native detector
 * (enabled once at startup, see Plugin.tsx) is already producing.
 */
export const useTunerReading = (): TunerState => {
  const getTunerReading = useNativeFunction<TunerReading>('getTunerReading');
  const [note, setNote] = useState<string | null>(null);
  const [midi, setMidi] = useState<number | null>(null);
  const [sounding, setSounding] = useState<string | null>(null);
  const [cents, setCents] = useState(0);
  const [hasSignal, setHasSignal] = useState(false);
  const [frequency, setFrequency] = useState(0);
  const smootherRef = useRef(new TunerSmoother());
  const holdTimeoutRef = useRef<number | undefined>(undefined);

  useEffect(() => {
    let cancelled = false;
    let polling = false;

    const poll = async () => {
      if (cancelled || polling) return;
      polling = true;
      try {
        const reading = await getTunerReading();
        if (cancelled || !reading) return;

        const freq = typeof reading.frequency === 'number' ? reading.frequency : 0;
        const confidence = typeof reading.confidence === 'number' ? reading.confidence : 0;
        const level = typeof reading.level === 'number' ? reading.level : -120;
        const refHz = getTunerRefHz();
        const offset = getTunerOffset();
        const valid = freq > 0 && confidence > 0.5;

        const pitch = smootherRef.current.push(
          performance.now(),
          level,
          valid ? frequencyToPitch(freq, refHz, offset) : null
        );
        if (valid && pitch !== null) {
          const detected = pitchToNote(pitch, offset);
          setNote(detected.name);
          setMidi(detected.midi);
          setSounding(detected.sounding);
          setCents(detected.cents);
          setFrequency(pitchToFrequency(pitch, refHz, offset));
          setHasSignal(true);
          if (holdTimeoutRef.current) window.clearTimeout(holdTimeoutRef.current);
          holdTimeoutRef.current = window.setTimeout(() => setHasSignal(false), HOLD_MS);
        }
      } catch {
        // Ignore individual polling failures.
      } finally {
        polling = false;
      }
    };

    const interval = window.setInterval(poll, POLL_MS);
    poll();

    return () => {
      cancelled = true;
      window.clearInterval(interval);
      if (holdTimeoutRef.current) window.clearTimeout(holdTimeoutRef.current);
    };
  }, [getTunerReading]);

  return { note, midi, sounding, cents, frequency, hasSignal };
};
