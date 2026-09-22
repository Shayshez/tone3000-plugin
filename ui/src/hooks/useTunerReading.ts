import { useEffect, useRef, useState } from 'react';
import { useNativeFunction } from './useFunction';

interface TunerReading {
  frequency: number;
  confidence: number;
  level: number;
}

const NOTE_NAMES = ['C', 'C♯', 'D', 'D♯', 'E', 'F', 'F♯', 'G', 'G♯', 'A', 'A♯', 'B'];
const POLL_MS = 50;
// Hold the last note on screen briefly after the signal decays.
const HOLD_MS = 900;

const frequencyToNote = (frequency: number) => {
  const midi = 69 + 12 * Math.log2(frequency / 440);
  const nearest = Math.round(midi);
  return {
    name: NOTE_NAMES[((nearest % 12) + 12) % 12],
    octave: Math.floor(nearest / 12) - 1,
    cents: (midi - nearest) * 100,
  };
};

export interface TunerState {
  note: string | null;
  cents: number;
  frequency: number;
  hasSignal: boolean;
}

/**
 * Polls the native pitch detector at 20 Hz and smooths the result into a
 * stable note/cents reading. Shared by the full TunerView and the always-on
 * mini tuner in the header - both just read whatever the native detector
 * (enabled once at startup, see Plugin.tsx) is already producing.
 */
export const useTunerReading = (): TunerState => {
  const getTunerReading = useNativeFunction<TunerReading>('getTunerReading');
  const [note, setNote] = useState<string | null>(null);
  const [cents, setCents] = useState(0);
  const [hasSignal, setHasSignal] = useState(false);
  const [frequency, setFrequency] = useState(0);
  const smoothedCentsRef = useRef(0);
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

        if (freq > 0 && confidence > 0.5) {
          const detected = frequencyToNote(freq);
          // Light exponential smoothing so the display doesn't jitter.
          smoothedCentsRef.current = smoothedCentsRef.current * 0.6 + detected.cents * 0.4;
          setNote(detected.name);
          setCents(smoothedCentsRef.current);
          setFrequency(freq);
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

  return { note, cents, frequency, hasSignal };
};
