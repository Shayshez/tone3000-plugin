import { useSyncExternalStore } from 'react';

/**
 * Per-machine UI preferences that aren't part of the plugin state (they must
 * not ride presets/undo or APVTS automation), so they live in the webview's
 * localStorage with a tiny external store for reactive reads.
 */

const listeners = new Set<() => void>();
const emit = () => listeners.forEach((listener) => listener());
const subscribe = (listener: () => void) => {
  listeners.add(listener);
  return () => listeners.delete(listener);
};

/** Boolean preference backed by localStorage (off unless `fallback`). */
function boolPref(key: string, fallback = false) {
  let value = (() => {
    try {
      const stored = localStorage.getItem(key);
      return stored === null ? fallback : stored === 'true';
    } catch {
      return fallback;
    }
  })();

  const set = (enabled: boolean) => {
    if (value === enabled) return;
    value = enabled;
    try {
      localStorage.setItem(key, String(enabled));
    } catch {
      // Storage unavailable. The toggle still works for this session.
    }
    emit();
  };

  const useValue = () => useSyncExternalStore(subscribe, () => value);
  return { set, useValue, get: () => value };
}

// Whether NAM block cards expose the (=) per-block normalization toggle.
// Off by default: every block simply stays normalized (the block flag itself
// defaults to on and lives in the chain state, not here).
const blockNormalizeControl = boolPref('t3k.showBlockNormalizeControl');
export const setBlockNormalizeControlEnabled = blockNormalizeControl.set;
export const useBlockNormalizeControlEnabled = blockNormalizeControl.useValue;

// Whether NAM block cards expose the LITE/FULL size toggle (off shows a
// read-only chip instead, and only on blocks whose size differs from the
// new-block default). A view preference only: the size itself is per-block
// chain state (`params.slimSize`), and the default for new blocks lives
// natively (`namSlimSizeDefault`).
const blockSizeControl = boolPref('t3k.showBlockSizeControl');
export const setBlockSizeControlEnabled = blockSizeControl.set;
export const useBlockSizeControlEnabled = blockSizeControl.useValue;

// Whether the preset browser shows each row's MIDI program-change number.
// Off by default: most players don't program PCs, and the numbers are noise
// until they do.
const presetPcNumbers = boolPref('t3k.showPresetPcNumbers');
export const setPresetPcNumbersEnabled = presetPcNumbers.set;
export const usePresetPcNumbersEnabled = presetPcNumbers.useValue;

/** Typed preference backed by localStorage: `parse` validates what's
    stored (bad/missing -> `fallback`). `get` reads outside React (the
    tuner's poll loop), `useValue` subscribes. */
function typedPref<T>(key: string, fallback: T, parse: (raw: string) => T | undefined) {
  let value = (() => {
    try {
      const raw = localStorage.getItem(key);
      return raw === null ? fallback : (parse(raw) ?? fallback);
    } catch {
      return fallback;
    }
  })();

  const set = (next: T) => {
    if (Object.is(value, next)) return;
    value = next;
    try {
      localStorage.setItem(key, String(next));
    } catch {
      // Storage unavailable. The setting still works for this session.
    }
    emit();
  };

  const useValue = () => useSyncExternalStore(subscribe, () => value);
  const get = () => value;
  return { set, useValue, get };
}

const intIn = (lo: number, hi: number) => (raw: string) => {
  const n = Number(raw);
  return Number.isInteger(n) && n >= lo && n <= hi ? n : undefined;
};

// --- Tuner (see TunerView / useTunerReading) --------------------------------

/** Reference pitch for A4 in Hz. 440 standard; 432 and the baroque/orchestral
    415-446 range are the common alternatives. */
export const TUNER_REF_MIN = 415;
export const TUNER_REF_MAX = 466;
export const TUNER_REF_DEFAULT = 440;
const tunerRefHz = typedPref(
  't3k.tunerRefHz',
  TUNER_REF_DEFAULT,
  intIn(TUNER_REF_MIN, TUNER_REF_MAX)
);
export const setTunerRefHz = tunerRefHz.set;
export const useTunerRefHz = tunerRefHz.useValue;
export const getTunerRefHz = tunerRefHz.get;

/** Transposed tuning in semitones (-1 = E♭ standard): the tuner still names
    notes as if in standard (EADGBE) while targeting the shifted pitches. */
export const TUNER_OFFSET_MIN = -7;
export const TUNER_OFFSET_MAX = 5;
const tunerOffset = typedPref('t3k.tunerOffset', 0, intIn(TUNER_OFFSET_MIN, TUNER_OFFSET_MAX));
export const setTunerOffset = tunerOffset.set;
export const useTunerOffset = tunerOffset.useValue;
export const getTunerOffset = tunerOffset.get;

/** Mute the plugin output while the full tuner is open (restored on close). */
const tunerMute = typedPref('t3k.tunerMute', false, (raw) =>
  raw === 'true' ? true : raw === 'false' ? false : undefined
);
export const setTunerMute = tunerMute.set;
export const useTunerMute = tunerMute.useValue;

export type TunerDisplay = 'bars' | 'needle' | 'strobe';
const tunerDisplay = typedPref<TunerDisplay>('t3k.tunerDisplay', 'bars', (raw) =>
  // 'combo' was merged into 'strobe' (which now carries the needle overlay).
  raw === 'combo'
    ? 'strobe'
    : raw === 'bars' || raw === 'needle' || raw === 'strobe'
      ? raw
      : undefined
);
export const setTunerDisplay = tunerDisplay.set;
export const useTunerDisplay = tunerDisplay.useValue;

// Global keyboard shortcuts while the plugin has keyboard focus (scenes
// 1-4 and [ ], presets ⇧[ ⇧], undo, Esc - see useGlobalShortcuts), and Space handing
// the transport to the host without giving up that focus. On by default;
// the switch exists for hosts where a plain key must always reach the DAW.
const keyboardShortcuts = boolPref('t3k.keyboardShortcuts', true);
export const setKeyboardShortcutsEnabled = keyboardShortcuts.set;
export const useKeyboardShortcutsEnabled = keyboardShortcuts.useValue;
/** Non-reactive read for plain event listeners (keyPassthrough). */
export const keyboardShortcutsEnabled = () => keyboardShortcuts.get();
