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
export const frequencyToNote = (frequency: number, refHz: number, offset: number) =>
  pitchToNote(frequencyToPitch(frequency, refHz, offset), offset);

/** Detected pitch -> continuous (fractional) MIDI number in the
    offset-adjusted naming frame. The smoother works in this space, so a
    note change is just a jump rather than a cents wrap from +49 to -49. */
export const frequencyToPitch = (frequency: number, refHz: number, offset: number) =>
  69 + 12 * Math.log2(frequency / refHz) - offset;

/** Inverse of frequencyToPitch (for showing the smoothed pitch in Hz). */
export const pitchToFrequency = (pitch: number, refHz: number, offset: number) =>
  refHz * Math.pow(2, (pitch + offset - 69) / 12);

/** Fractional pitch (offset frame) -> nearest note + cents off it. */
export const pitchToNote = (pitch: number, offset: number) => {
  const nearest = Math.round(pitch);
  return {
    name: noteName(nearest),
    midi: nearest,
    sounding: noteName(nearest + offset),
    cents: (pitch - nearest) * 100,
  };
};
