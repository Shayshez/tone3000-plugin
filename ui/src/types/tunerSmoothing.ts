/**
 * Turns the raw ~20 Hz detector readings into the calm pitch the tuner
 * displays (pure - no React/JUCE imports, see ui/test/tunerSmoothing.test.ts).
 *
 * A plucked string starts several cents sharp and settles as it rings, and
 * the detector adds the odd single-frame outlier on top. Hardware tuners
 * (Fractal et al.) hide both; this does the same in three steps:
 *
 *  1. Attack gate: a pick (level jump, or a note after silence) freezes the
 *     display for ATTACK_MS so the sharp transient never reaches it.
 *  2. Median of the last 3 readings drops one-off outliers.
 *  3. An exponential glide (slower near the current value, quicker for big
 *     moves). A jump of a third of a semitone or more instead snaps outright
 *     once two raw readings confirm it, so switching strings doesn't slide
 *     across the scale.
 *
 * Works in continuous pitch (fractional MIDI, see frequencyToPitch) so a
 * note change is a jump, not a cents wrap.
 */

/** Display freeze after a pick, ms. */
export const ATTACK_MS = 150;
/** Level rise between consecutive readings that counts as a new pick, dB. */
export const ONSET_RISE_DB = 3;
/** No valid pitch for this long, ms, and the next one is a fresh note. */
export const GAP_MS = 250;
/** Distance (semitones) that means "different note", not a drift. */
export const JUMP_SEMITONES = 0.35;
/** Glide time constants, s: near the displayed value / for large moves. */
export const TAU_SLOW = 0.3;
export const TAU_FAST = 0.12;
/** Cents of difference at which the glide reaches TAU_FAST. */
const FAST_AT_CENTS = 20;

const median = (v: number[]) => {
  const s = [...v].sort((a, b) => a - b);
  const m = s.length >> 1;
  return s.length % 2 ? s[m] : (s[m - 1] + s[m]) / 2;
};

export class TunerSmoother {
  private smoothed: number | null = null;
  private history: number[] = [];
  private gateUntil = -Infinity;
  private lastLevel: number | null = null;
  private lastValidT = -Infinity;
  private lastUpdateT: number | null = null;
  private jumpCount = 0;

  reset() {
    this.smoothed = null;
    this.history = [];
    this.gateUntil = -Infinity;
    this.lastLevel = null;
    this.lastValidT = -Infinity;
    this.lastUpdateT = null;
    this.jumpCount = 0;
  }

  /**
   * One detector reading at time `t` (ms). `pitch` is null when the detector
   * found no reliable pitch. Returns the pitch to display, or null while
   * there is nothing to show yet (first attack of a fresh note).
   */
  push(t: number, levelDb: number, pitch: number | null): number | null {
    const rose = this.lastLevel !== null && levelDb - this.lastLevel >= ONSET_RISE_DB;
    this.lastLevel = levelDb;
    if (pitch === null) return this.smoothed;

    const fresh = t - this.lastValidT > GAP_MS;
    this.lastValidT = t;
    if (fresh) this.smoothed = null; // don't flash the previous note
    if (rose || fresh) {
      this.gateUntil = t + ATTACK_MS;
      this.history = [];
      this.jumpCount = 0;
    }
    if (t < this.gateUntil) return this.smoothed;

    const prevT = this.lastUpdateT;
    this.lastUpdateT = t;

    // A note or more away from the display: a new note, or a big outlier.
    // Hold until a second raw reading agrees, then snap (history restarts
    // so the median can't drag the old note along).
    if (this.smoothed !== null && Math.abs(pitch - this.smoothed) > JUMP_SEMITONES) {
      if (++this.jumpCount < 2) return this.smoothed;
      this.jumpCount = 0;
      this.history = [pitch];
      this.smoothed = pitch;
      return this.smoothed;
    }
    this.jumpCount = 0;

    this.history.push(pitch);
    if (this.history.length > 3) this.history.shift();
    const target = median(this.history);
    if (this.smoothed === null) {
      this.smoothed = target;
      return this.smoothed;
    }

    const diff = target - this.smoothed;
    const dt = prevT === null ? 0.05 : Math.min(0.2, Math.max(0, (t - prevT) / 1000));
    const k = Math.min(1, (Math.abs(diff) * 100) / FAST_AT_CENTS);
    const tau = TAU_SLOW + (TAU_FAST - TAU_SLOW) * k;
    this.smoothed += diff * (1 - Math.exp(-dt / tau));
    return this.smoothed;
  }
}
