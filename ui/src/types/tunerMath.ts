/**
 * Pure pitch -> note math for the tuner (no React/JUCE imports, so
 * `node --test` can load it directly - see ui/test/tunerMath.test.ts).
 */

/** Cents window the tuners call "in tune" (big tuner, mini tuner, needle
    zone). 2 cents: about where a careful ear starts to hear the beat - the
    earlier 5 lit the in-tune marks on notes that were audibly off. */
export const IN_TUNE_CENTS = 2;
/** Extra cents a held lock tolerates before releasing (hysteresis, so the
    marks don't flicker as a sustained note wobbles at the edge). */
export const IN_TUNE_RELEASE = 1;

const NOTE_NAMES = ['C', 'C♯', 'D', 'D♯', 'E', 'F', 'F♯', 'G', 'G♯', 'A', 'A♯', 'B'];

export const noteName = (midi: number) => NOTE_NAMES[((midi % 12) + 12) % 12];

/** Detected pitch -> note, against the A4 reference (`refHz`). `offset`
    transposes the naming: a guitar tuned down a half step (offset -1)
    playing its low string (E♭2) reads as E2, in tune. `sounding` keeps the
    real note for the readout. */
export const frequencyToNote = (frequency: number, refHz: number, offset: number) => {
  const soundingMidi = 69 + 12 * Math.log2(frequency / refHz);
  const midi = soundingMidi - offset;
  const nearest = Math.round(midi);
  return {
    name: noteName(nearest),
    midi: nearest,
    sounding: noteName(nearest + offset),
    cents: (midi - nearest) * 100,
  };
};
