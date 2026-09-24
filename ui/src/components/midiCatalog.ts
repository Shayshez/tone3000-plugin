import type { MidiMapping } from '../types/midiMap';

/**
 * Display catalog for the MIDI mapping UI: which targets are mappable and
 * how to present them. The native engine accepts any APVTS parameter id (or
 * block-power id); this list is the UI's curation (setup-domain params like
 * calibration stay out; they describe the rig, not something you perform
 * with).
 */

export interface MappableTarget {
  /** APVTS parameter id, positional block power ("block1Power"), or a
      virtual action id ("presetNext"). */
  id: string;
  name: string;
  /** Section subtitle, mirroring the faceplate's layout. */
  group: string;
  /** Drives the behavior display: continuous knobs are absolute via CC,
      toggles flip on/off, triggers fire an action once per press. */
  kind: 'continuous' | 'toggle' | 'trigger' | 'select';
}

/** Block-power targets are positional ("Block 1" is the chain's first tone
    block whatever it currently holds), so a mapping survives tone swaps and
    preset loads, like switches on a pedalboard. Display-only cap (the
    engine takes up to 64): enough for any realistic pedalboard without
    burying the picker. */
const BLOCK_POWER_TARGETS = 12;

export const MAPPABLE_TARGETS: MappableTarget[] = [
  // Virtual actions (native resolves the ids itself): step through the
  // preset list in browser order, wrapping at the ends, for footswitches
  // programmed with CC / note buttons instead of program changes.
  { id: 'presetPrevious', name: 'Previous Preset', group: 'Presets', kind: 'trigger' },
  { id: 'presetNext', name: 'Next Preset', group: 'Presets', kind: 'trigger' },
  { id: 'inputLevel', name: 'Input Gain', group: 'Global', kind: 'continuous' },
  { id: 'outputLevel', name: 'Output Level', group: 'Global', kind: 'continuous' },
  { id: 'outputPan', name: 'Output Pan', group: 'Global', kind: 'continuous' },
  { id: 'bypass', name: 'Bypass', group: 'Global', kind: 'toggle' },
  { id: 'scenePrevious', name: 'Previous Scene', group: 'Scenes', kind: 'trigger' },
  { id: 'sceneNext', name: 'Next Scene', group: 'Scenes', kind: 'trigger' },
  // One control for all scenes: CC value 0-3 = Scene 1-4, or a note mapping
  // covering four consecutive notes (the mapped note = Scene 1).
  { id: 'sceneSelect', name: 'Scene Select (all scenes)', group: 'Scenes', kind: 'select' },
  ...Array.from(
    { length: 4 },
    (_, i): MappableTarget => ({
      id: `scene${i + 1}`,
      name: `Scene ${i + 1}`,
      group: 'Scenes',
      kind: 'trigger',
    })
  ),
  { id: 'outputMute', name: 'Mute', group: 'Global', kind: 'toggle' },
  { id: 'gateEnabled', name: 'Gate Power', group: 'Noise Gate', kind: 'toggle' },
  { id: 'gateThreshold', name: 'Gate Threshold', group: 'Noise Gate', kind: 'continuous' },
  { id: 'toneEqEnabled', name: 'Tone Stack Power', group: 'Tone Stack', kind: 'toggle' },
  { id: 'toneBass', name: 'Bass', group: 'Tone Stack', kind: 'continuous' },
  { id: 'toneMid', name: 'Mid', group: 'Tone Stack', kind: 'continuous' },
  { id: 'toneTreble', name: 'Treble', group: 'Tone Stack', kind: 'continuous' },
  ...Array.from(
    { length: BLOCK_POWER_TARGETS },
    (_, i): MappableTarget => ({
      id: `block${i + 1}Power`,
      name: `Block ${i + 1} Power`,
      group: 'Chain',
      kind: 'toggle',
    })
  ),
];

export const targetById = new Map(MAPPABLE_TARGETS.map((t) => [t.id, t]));

const NOTE_NAMES = ['C', 'C#', 'D', 'D#', 'E', 'F', 'F#', 'G', 'G#', 'A', 'A#', 'B'];

/** 60 → "C4" (scientific pitch, middle C = C4). */
export const midiNoteName = (note: number) =>
  `${NOTE_NAMES[note % 12]}${Math.floor(note / 12) - 1}`;

/** "CC 64" / "Note C2": the mapping row's source column. */
export const sourceLabel = (mapping: MidiMapping) => {
  if (mapping.source === 'cc') return `CC ${mapping.number}`;
  // Scene Select answers to four consecutive notes.
  if (mapping.targetId === 'sceneSelect')
    return `Notes ${midiNoteName(mapping.number)}-${midiNoteName(mapping.number + 3)}`;
  return `Note ${midiNoteName(mapping.number)}`;
};

/** Typed source: "64" = CC 64, a note name ("C2", "F#3", "Db1") = that note. */
export const parseTypedSource = (
  text: string
): { source: 'cc' | 'note'; number: number } | null => {
  const t = text.trim();
  if (/^\d{1,3}$/.test(t)) {
    const n = Number(t);
    return n <= 127 ? { source: 'cc', number: n } : null;
  }
  const m = /^([A-Ga-g])([#b]?)(-?\d)$/.exec(t);
  if (!m) return null;
  const base = { C: 0, D: 2, E: 4, F: 5, G: 7, A: 9, B: 11 }[m[1].toUpperCase() as 'C'];
  const n = (Number(m[3]) + 1) * 12 + base + (m[2] === '#' ? 1 : m[2] === 'b' ? -1 : 0);
  return n >= 0 && n <= 127 ? { source: 'note', number: n } : null;
};

/** How the pairing behaves, mirroring the native derivation: trigger
    targets fire per press, toggle targets (and any note source) flip
    on/off, everything else tracks the CC value absolutely. */
export const behaviorLabel = (mapping: MidiMapping) => {
  const kind = targetById.get(mapping.targetId)?.kind;
  if (kind === 'select') return mapping.source === 'note' ? 'Note → Scene' : 'Value 0-3 → Scene';
  if (kind === 'trigger') return 'Trigger';
  if (kind === 'toggle' || mapping.source === 'note') return 'Toggle';
  return 'Absolute';
};
