import { useSyncExternalStore } from 'react';
import { IS_COARSE_POINTER } from '../hooks/useUiScale';
import { BLOCK_TYPE_LABEL } from '../types/chain';
import type { ToneBlock } from '../types/chain';

/**
 * Central help system: every control publishes a one-line hint here while
 * hovered (or mid-interaction), and the faceplate's pinned readout renders
 * whatever is current, Native Instruments style, instead of browser
 * tooltips. All copy lives in this file so wording stays consistent.
 *
 * Copy conventions:
 * - `Name: what it does.` then a shortcut legend of `key: effect` pairs
 *   joined with middots. Keep it terse: the bar shares its row with the
 *   CPU readout, and long lines get ellipsized.
 * - Modifier keys are OS-correct: glyphs on macOS (⇧ ⌥, hyphen-joined per
 *   Apple convention), spelled out with `+` elsewhere (Shift+drag).
 * - Toggles describe the control, not the current state (the control's own
 *   visual state already says which way it's set).
 */

// --- store -----------------------------------------------------------------

// Hover tracking is delegated: elements carry a data-help attribute and
// document-level mouseover + pointerdown listeners resolve the nearest hint
// under the pointer. Compared to per-element enter/leave handlers this
// survives nesting (button inside a hoverable tile) and elements unmounting
// mid-hover (removing a block never strands its hint on screen).
//
// `pinned` overrides hover for the duration of an interaction: a knob drag
// can wander off the knob without releasing, so its hint stays pinned until
// mouseup.

const HELP_ATTR = 'data-help';

let hoverText: string | null = null;
let pinned: string | null = null;
const listeners = new Set<() => void>();
const emit = () => listeners.forEach((listener) => listener());

/** Pin a hint for the duration of an interaction (drag/edit). */
export const pinHelp = (text: string) => {
  if (pinned === text) return;
  pinned = text;
  emit();
};

/** Release a pinned hint (no-op if something else pinned since). */
export const unpinHelp = (text: string) => {
  if (pinned !== text) return;
  pinned = null;
  emit();
};

let delegationInstalled = false;
const installDelegation = () => {
  if (delegationInstalled || typeof document === 'undefined') return;
  delegationInstalled = true;

  const update = (text: string | null) => {
    if (hoverText === text) return;
    hoverText = text;
    emit();
  };

  // Touch engines replay a mouse event pair (mouseover, mousemove,
  // mousedown, mouseup, click) after a tap, aimed at the element just
  // tapped. The replay lands *after* pointerup, so it would restore the hint
  // the release below has just cleared: the bar kept captioning the last
  // thing touched, exactly the behaviour the release is there to remove.
  // Ignoring it for a beat is narrower than dropping `mouseover` on touch
  // devices, which would also kill the genuine hover an iPad trackpad
  // produces.
  const MOUSE_REPLAY_MS = 700;
  let lastTouchRelease = -Infinity;

  const resolve = (e: Event) => {
    if (e.type === 'mouseover' && performance.now() - lastTouchRelease < MOUSE_REPLAY_MS) return;
    const el = e.target instanceof Element ? e.target.closest(`[${HELP_ATTR}]`) : null;
    update(el?.getAttribute(HELP_ATTR) ?? null);
  };

  document.addEventListener('mouseover', resolve);
  // Touch-only devices never hover, so pressing a control is the hint
  // trigger there (harmless for mouse users; press implies hover).
  document.addEventListener('pointerdown', resolve);
  // A touch press is the hover equivalent, so the release is the un-hover:
  // the bar shows the control's help for exactly as long as the finger is
  // down, then clears. Leaving stale help pinned to the last thing tapped
  // was the desktop behaviour of a stationary mouse, which on touch just
  // reads as a wrong caption. Mouse and pen releases are ignored, so
  // desktop hover is untouched.
  //
  // On `window` in the capture phase: a control that took pointer capture
  // (knobs, the tile lift) retargets its release, and a bubbling document
  // listener can miss it entirely.
  const releaseTouch = (e: PointerEvent) => {
    if (e.pointerType !== 'touch') return;
    lastTouchRelease = performance.now();
    update(null);
  };
  window.addEventListener('pointerup', releaseTouch, true);
  window.addEventListener('pointercancel', releaseTouch, true);
  // Pointer left the window entirely.
  document.addEventListener('mouseout', (e) => {
    if (e.relatedTarget === null) update(null);
  });
};

const subscribe = (listener: () => void) => {
  installDelegation();
  listeners.add(listener);
  return () => listeners.delete(listener);
};

const snapshot = () => pinned ?? hoverText;

/** Current help line for the pinned readout (null = nothing hovered). */
export const useHelpText = () => useSyncExternalStore(subscribe, snapshot);

/** Spread onto any element to drive the readout from hover. */
export const helpProps = (text: string) => ({ [HELP_ATTR]: text });

// --- visibility preference ---------------------------------------------------

// Whether the hint bar shows at all. A per-machine UI preference, not part
// of the plugin state, so it lives in webview localStorage rather than an
// APVTS parameter (presets/undo shouldn't touch it).
const HINTS_KEY = 't3k.showHints';

let hintsEnabled = (() => {
  try {
    return localStorage.getItem(HINTS_KEY) !== 'false';
  } catch {
    return true;
  }
})();

export const setHintsEnabled = (enabled: boolean) => {
  if (hintsEnabled === enabled) return;
  hintsEnabled = enabled;
  try {
    localStorage.setItem(HINTS_KEY, String(enabled));
  } catch {
    // Storage unavailable; the toggle still works for this session.
  }
  emit();
};

export const useHintsEnabled = () => useSyncExternalStore(subscribe, () => hintsEnabled);

// --- copy ------------------------------------------------------------------

/** OS-correct modifier chords: glyphs + hyphen on macOS (Apple convention),
    spelled out + plus elsewhere. */
const IS_MAC = /Mac|iP(hone|ad|od)/i.test(
  (navigator as { userAgentData?: { platform?: string } }).userAgentData?.platform ??
    navigator.platform
);
const chord = (macGlyph: string, name: string) => (gesture: string) =>
  IS_MAC ? `${macGlyph}-${gesture}` : `${name}+${gesture}`;
const shift = chord('\u21e7', 'Shift');
const alt = chord('\u2325', 'Alt');

/** Shared legend for every KnobControl (they all support these gestures).
    Touch has no modifier keys and no separate click button, so it gets the
    gestures it actually has (see KnobControl). */
const KNOB_KEYS = IS_COARSE_POINTER
  ? 'drag up or down: adjust · double tap: reset · tap the name: type'
  : `${shift('drag')}: fine · double-click: type · ${alt('click')}: reset`;

export const knobHelp = (name: string, desc: string) => `${name}: ${desc} ${KNOB_KEYS}`;

/**
 * Desktop copy. Touch devices re-word it through `touchify` below rather
 * than branching every line: only the entries whose *gesture* differs
 * (knobs, EQ faders and dots) are branched by hand, and everything else
 * differs only in the noun for "press this", which one pass can do without
 * letting the two wordings drift apart.
 */
const HELP_DESKTOP = {
  // Faceplate: gains
  inputLevel: knobHelp('Input', 'chain input level, up to +24 dB, mutes at zero.'),
  inputMode: 'Input Mode: source channels. Stereo: both · L/R: one. Click: choose.',
  outputLevel: knobHelp('Output', 'master output level, up to +24 dB, mutes at zero.'),
  outputPan: knobHelp('Pan', 'output stereo position. Center: off.'),

  // Faceplate: gate, tone stack
  gate: knobHelp('Gate', 'noise gate threshold, -100 to 0 dB.'),
  gatePower: 'Gate Power: noise gate on/off.',
  toneBass: knobHelp('Bass', 'tone stack lows, 0-10: ±20 dB shelf at 150 Hz.'),
  toneMiddle: knobHelp('Middle', 'tone stack mids, 0-10: ±15 dB bell at 425 Hz.'),
  toneTreble: knobHelp('Treble', 'tone stack highs, 0-10: ±10 dB shelf at 1.8 kHz.'),
  tonePower: 'Tone Stack Power: Bass/Middle/Treble on/off.',

  // Top bar
  miniTuner:
    'Tuner, always on: flat/sharp/in-tune. Click: full tuner with note detail. Again: back.',
  bypass: 'Bypass: plugin power. Off: dry input passes through (same as the host bypass).',
  mute: 'Mute: silence the output. The tuner keeps working.',
  undo: 'Undo: revert last chain edit.',
  redo: 'Redo: re-apply undone edit.',
  settings: 'Settings: plugin and audio options.',
  account: 'Account: settings and TONE3000 sign-out.',

  // Presets
  presetPrev: 'Previous Preset: step back through the list.',
  presetNext: 'Next Preset: step forward through the list.',
  presetBrowse: 'Presets: browse factory and user presets.',
  presetSave: 'Save Preset: store the current chain. Same name: overwrite.',
  presetNew: 'New: clear the chain and reset every control to its default.',
  presetRename: 'Rename: edit name. Enter: commit · Esc: cancel.',
  presetDelete: 'Delete: remove this preset.',
  presetDuplicate: 'Duplicate: save the current sound as a copy of this preset.',
  presetReorder: 'Reorder: drag presets into a custom order. Prev/Next and MIDI follow it.',
  presetDrag: 'Drag: move this preset within its section.',
  presetPcToggle:
    'MIDI PC: show each preset\u2019s program change number. Prev/Next and PC follow the list order.',
  presetPc: 'PC: the MIDI program change number that loads this preset.',

  // Chain gallery
  addTile:
    'Add Tone: browse TONE3000 for this slot, or drop a .nam or IR .wav file (or a folder of them). Right-click: paste / load file · drag: move.',
  chainMapEqMark: 'EQ: jump to this block’s EQ view. Orange: EQ shaping the sound.',
  closeToneBrowser: 'Close: back to the chain.',
  copyBlock: 'Copy: copy this block (tone, model and all settings).',
  duplicateBlock: 'Duplicate: add a copy of this block right after it (same as ⌥-drag).',
  eqBandMenuReset: 'Reset Band: this band back to its default Freq, Gain/Slope and Q.',
  irPointMenuReset: 'Reset: this envelope point or curve back to its default (same as ⌥-click).',
  midiMenuLearn:
    'MIDI Learn: then move a knob, fader or switch on your MIDI controller to map it here.',
  midiMenuCancel: 'Cancel MIDI Learn: stop waiting for a MIDI control.',
  midiMenuClear: "Clear MIDI: remove this control's MIDI mapping.",
  tunerRef:
    'A4 reference pitch (standard 440 Hz). Wheel or arrows to step, right-click for quick picks.',
  tunerOffset:
    'Tuning offset: target N semitones down/up while still reading EADGBE (e.g. E♭ Std). Right-click for presets.',
  tunerMute: 'Mute while tuning: silence the output whenever this tuner screen is open.',
  sceneName: 'Scene: click to pick a scene from the list, or rename the active one.',
  sceneButton: 'click: switch scene (gapless). Right-click: rename, copy, MIDI.',
  sceneSelect: 'Switch to this scene.',
  sceneRename: 'Rename: give this scene a name (Enter to save, Esc to cancel).',
  sceneCopy: "Copy To: overwrite another scene with this one's settings (keeps its name).",
  sceneLevel:
    'Scene level: output boost/cut for this scene only. Drag or wheel to change, double-click to type, ⌥-click: 0 dB.',
  blockChannel:
    'Channel A-D: four full versions of this block (model, knobs, EQ). Scenes pick a channel per block.',
  blockChannelCopy: "Copy this channel's settings onto another channel of the block.",
  sceneManager:
    'Scene Manager: every scene against every block - bypass and channel at a glance. Click again to close.',
  sceneCellPower: 'Click: bypass/enable this block in this scene.',
  sceneCellChannel: 'Click: pick the channel this block uses in this scene.',
  sceneColumnOpen: 'Click: open this block.',
  resizeGrip: 'Resize: drag the corner or the right/bottom edge to scale the window (1x to 2x).',
  knobMenuReset: 'Reset to Default: put this knob back to its default value (same as ⌥-click).',
  knobMenuType: 'Type Value: enter an exact value (same as double-click).',
  chipBypass: 'Bypass: toggle this block on/off (same as ⌥-click on the chip).',
  chipReplace: 'Replace: pick a new tone for this block, keeping its slot.',
  chipAddSlotLeft: 'Add Slot Left: an empty + slot just before this block.',
  chipAddSlotRight: 'Add Slot Right: an empty + slot just after this block.',
  chipDelete: 'Delete: remove this block from the chain.',
  chipDeleteSlot: 'Delete: remove this empty slot.',
  pasteBlock: 'Paste: add a copy of the copied block in this slot.',
  addCabTile: 'Cab: load a local .wav as a cabinet IR - hover for Load File / Load Folder.',
  addIrTile: 'IR: load a local .wav as an impulse response - hover for Load File / Load Folder.',
  addEqTile: 'EQ: add a standalone 8-band EQ block here - no model, always 100% wet.',
  addDualMonoTile:
    'Dual Mono: a fixed pair of two blocks, one per channel - pick each side’s tone from its own detail view.',
  convertToDualMono:
    'Convert to Dual Mono: wraps this block as the Left side of a new Dual Mono pair, keeping its tone and settings. Right starts empty.',
  collapseDualMonoToSingle:
    'Collapse to Single: replaces this block with its one loaded side, keeping its tone and settings.',
  loadFileTile: 'Load File: pick a local file to load here. No account needed.',
  loadFolderTile: 'Load Folder: pick a folder of files; loads as one multi-model block.',
  blockPower: 'Power: bypass this block.',
  retryLoad: 'Retry: re-download this model.',
  galleryEqShortcut: 'EQ: jump straight to this block’s EQ view. Yellow: EQ shaping the sound.',
  galleryStereoShortcut:
    'Stereo: jump straight to this block’s Stereo Processing view. Yellow: Align/Ø shaping the sound.',
  swapTone: 'Swap: replace this tone, keeping its slot.',
  removeBlock: 'Remove: delete this block.',
  dualPanLeft: knobHelp('Pan L', 'Left slot’s position in the recombined stereo image.'),
  dualPanRight: knobHelp('Pan R', 'Right slot’s position in the recombined stereo image.'),
  addDualSlotLeft: 'Left: pick a tone for this slot.',
  addDualSlotRight: 'Right: pick a tone for this slot.',
  copyDualSlotFromLeft: 'Copy from Left: fill this side with a copy of Left’s tone and settings.',
  copyDualSlotFromRight:
    'Copy from Right: fill this side with a copy of Right’s tone and settings.',
  dualSideOpen: 'Open: this side’s full editor.',
  dualLink: 'Link: mirror Pan, match Mix and Vol between both sides.',
  dualMute: 'Mute: silence this side.',
  dualSolo: 'Solo: silence the other side.',
  dualInvertLeft: 'Ø Left: flip this side’s polarity.',
  dualInvertRight: 'Ø Right: flip this side’s polarity.',
  dualAutoAlign:
    'Auto Align: a ½ s internal sweep time-aligns this block’s two sides and fixes inverted polarity. Click again: cancel.',
  dualAutoBalance:
    'Auto Balance: a ½ s internal sweep matches this block’s two sides to the same loudness. Click again: cancel.',
  dualAlignToggle:
    'Stereo: Align/phase controls between the two sides. Outline: Align shaping the sound.',
  dualStereoProcessingPower:
    'Stereo Power: bypasses Align and Ø without clearing the dialed-in values.',
  dualAlignOffset: knobHelp(
    'Offset',
    'delays one side to correct timing/phase drift against the other, up to ±24 ms.'
  ),
  dualAlignReset: 'Reset all Align controls to default (off, centered).',
  dualAlignCorrelation:
    'Mono safety: dim: safe · yellow: caution · red: cancellation when the two sides sum to mono.',
  dualAlignWobble: knobHelp('Wobble', 'humanizing drift of the align delay, up to ±1.2 ms.'),
  dualAlignWobblePower: 'Wobble Power: drifts the delayed side like an ADT double-track.',
  dualAlignCrossover: knobHelp('Crossover', 'lows below the cutoff skip the deck, 33-520 Hz.'),
  dualAlignCrossoverPower: 'Crossover Power: on keeps lows out of the delay and diffusion.',
  dualAlignDiffuse: 'Diffuse Power: phase-decorrelates the delayed side for width.',

  // Block card
  blockIn: knobHelp('In', 'block input gain, up to +24 dB, mutes at zero.'),
  blockOut: knobHelp('Out', 'block output gain, up to +24 dB, mutes at zero.'),
  blockOutIr: knobHelp(
    'Out',
    'block output gain (IR level-normalized), up to +24 dB, mutes at zero.'
  ),
  blockMix: knobHelp('Mix', 'dry/wet blend.'),
  blockPredelay: knobHelp('Delay', 'delay before the IR player starts, up to 1s.'),
  // Not `blockSize`/`blockWidth` - those names are taken by the NAM A2
  // Lite/Full size chip above and would silently collide (TS won't allow a
  // duplicate key, but nothing stops the reverse ordering from doing so
  // quietly elsewhere - `Ir` disambiguates on purpose).
  blockIrSize: knobHelp(
    'Size',
    'vari-speed length/pitch, 10%-1000%. Center: original. Faster = higher pitch, slower = lower.'
  ),
  blockIrWidth: knobHelp(
    'Width',
    'stereo image, -200% to +200%. 0-100%: mono to full recorded stereo. Beyond: artificial widening.'
  ),
  blockIrWidthMono: knobHelp('Width', 'unavailable, the loaded IR has no stereo image.'),
  // IR shaping row: a 2-segment Attack/Decay envelope (Space Designer-style)
  // over the truncated content. Decay Length sets the TOTAL trimmed length
  // (the real "End" position); Attack Length is a position *within* that
  // total marking the envelope's peak, not an independent length. Init
  // Level is the level at the very start (not part of either segment);
  // Attack ramps from there up to unity/0dB (its peak is pinned, not
  // adjustable) at that position; Decay continues from the peak to the
  // trimmed end. Levels are 0-100% attenuation-only (100% = unity, 0% =
  // genuine silence).
  blockInitLevel: knobHelp('Init', 'level at the very start of the IR. 100%: no change.'),
  blockAttackLength: knobHelp(
    'Attack',
    'position of the envelope’s peak within the trimmed length.'
  ),
  blockAttackCurve: knobHelp(
    'Attack Curve',
    'Attack segment’s envelope shape, front-loaded to back-loaded. Center: linear.'
  ),
  blockDecayLength: knobHelp('Decay', 'trims the IR’s tail - the envelope’s total length.'),
  blockDecayLevel: knobHelp('Decay Level', 'level at the IR’s trimmed end. 100%: no change.'),
  blockDecayCurve: knobHelp(
    'Decay Curve',
    'Decay segment’s envelope shape, front-loaded to back-loaded. Center: linear.'
  ),
  // Plain toggle, not a knob/chip-with-a-value - no knobHelp (its gesture
  // legend describes dragging/typing a number, neither of which applies
  // here; click is the only affordance).
  blockTrimInit:
    'Trim Init: removes detected leading silence from the source so it can’t be mistaken for Delay. Off by default. Click to cycle Off → Std → Lax; Lax uses a less sensitive threshold for sources whose Std cut lands too early.',
  blockReverse: 'Reverse: plays the shaped IR backward. Off by default. Click to toggle.',
  // Envelope graph (IrEnvelopeGraph.tsx): the same six values as the chips
  // above, shaped directly on the waveform instead of typed in. Touch has no
  // Shift/Option modifiers, so those variants drop the fine-drag and
  // click-to-reset mentions in favor of a plain double tap (touchify's blunt
  // find/replace can't safely rewrite a modifier chord like "\u2325-click").
  envelopeInitPoint: IS_COARSE_POINTER
    ? 'Init: drag to set level at the IR\u2019s start. Double tap: reset.'
    : `Init: drag to set level at the IR\u2019s start. ${shift(
        'drag'
      )}: fine \u00b7 ${alt('click')} / double-click: reset.`,
  envelopePeakPoint: IS_COARSE_POINTER
    ? 'Attack: drag to set the envelope\u2019s peak position. Double tap: reset.'
    : `Attack: drag to set the envelope\u2019s peak position. ${shift(
        'drag'
      )}: fine \u00b7 ${alt('click')} / double-click: reset.`,
  envelopeEndPoint: IS_COARSE_POINTER
    ? 'Decay: drag to set trim length + end level. Double tap: reset.'
    : `Decay: drag to set trim length + end level. ${shift(
        'drag'
      )}: fine \u00b7 ${alt('click')} / double-click: reset.`,
  envelopeAttackCurve: IS_COARSE_POINTER
    ? 'Attack Curve: drag the line to bow the rise.'
    : `Attack Curve: drag the line to bow the rise. ${shift('drag')}: fine.`,
  envelopeDecayCurve: IS_COARSE_POINTER
    ? 'Decay Curve: drag the line to bow the fall.'
    : `Decay Curve: drag the line to bow the fall. ${shift('drag')}: fine.`,
  blockNormalize: 'Normalize: level this block\u2019s loudness. Off: raw capture level.',
  blockNormalizeOverridden:
    'Normalize: overridden \u2014 calibration hands this model\u2019s true output level to the next NAM block.',
  blockSize: 'NAM Size: LITE saves CPU · FULL is highest quality. Sets this block only.',
  blockSizeChip:
    'NAM Size: this block\u2019s size differs from your default. To choose per block, enable it in Settings.',
  blockIrCategory:
    'Cab / IR Player: converts this block \u2014 the sample carries over, truncated to 500ms going into Cab, full length going back. Cab \u2014 100% mix by default. IR Player \u2014 25% mix by default.',
  blockCalibrated: 'Calibration: active \u2014 levels set from this model\u2019s calibration data.',
  blockUncalibrated: 'Calibration: inactive \u2014 this model has no calibration data.',
  eqToggle: 'EQ: 8-band EQ editor. Outline: EQ shaping the sound.',
  blockResetShape:
    'Reset: restores Init/Attack/Decay, Size, Width, Trim Init and Reverse to default. Not EQ.',
  toneInfo: 'Info: tone description, makes, and tags from TONE3000.',
  toneInfoLogin: 'Log In: sign in to TONE3000 to see tone details.',
  viewOnT3k: 'View on TONE3000: open this tone in your browser.',
  favoriteTone: 'Favorite: save this tone to your TONE3000 favorites.',
  unfavoriteTone: 'Favorited: click to remove from your TONE3000 favorites.',
  eqReset: 'Reset EQ: all bands flat, position post.',
  eqPre: 'PRE: EQ before the model. Off: after the model (wet only).',
  eqPower: 'EQ Power: bypass EQ, keep settings.',
  eqCopy: 'Copy EQ: copy all 8 bands, to paste into any other EQ.',
  eqPaste: 'Paste EQ: replace these bands with the copied ones and turn this EQ on.',
  shareTone: 'Share: copy TONE3000 link.',
  modelSelectSignedOut: 'Models: sign in to TONE3000 to switch models.',
  backToChain: 'Back: chain overview.',
  backToDual: 'Back: this Dual Mono block.',
  chainMapHome: 'Gallery: jump straight back, from any depth.',

  // EQ editor
  eqBandBypass: 'Band: click the icon to bypass/enable. Low/High Cut start off.',
  eqPoleControl: IS_COARSE_POINTER
    ? 'Slope: drag: step 6/18/24/36/48 dB/oct.'
    : `Slope: drag: step 6/18/24/36/48 dB/oct · ${shift('drag')}: fine.`,
  eqDot: IS_COARSE_POINTER
    ? 'Band Dot: drag: freq + gain · double tap: reset. Q: use the strip below.'
    : `Band Dot: drag: freq + gain · scroll: Q · ${shift('drag')}: fine · ${alt('click')}: reset.`,
  eqFreqChip: IS_COARSE_POINTER
    ? 'Freq: drag: adjust · tap: type (\u201c800\u201d, \u201c1.2k\u201d).'
    : `Freq: drag: adjust (${shift('drag')}: fine) · click: type (\u201c800\u201d, \u201c1.2k\u201d).`,
  eqGainChip: IS_COARSE_POINTER
    ? 'Gain: drag: adjust, ±24 dB · tap: type.'
    : `Gain: drag: adjust, ±24 dB (${shift('drag')}: fine) · click: type.`,
  eqQChip: IS_COARSE_POINTER
    ? 'Q: drag: adjust · tap: type.'
    : `Q: drag: adjust (${shift('drag')}: fine) · click: type.`,

  // Meters
  clipDot: 'Clip: latches on clipping. Click: clear.',

  // The hint bar itself
  cpuLoad: 'CPU: audio engine load.',
  hideHints: 'Hide Info Bar: hide this bar. Re-enable in Settings.',
} as const;

/**
 * Desktop pointer vocabulary rewritten for touch. `Right-click` first, since
 * it contains `click`; everything a right-click reaches (context menus,
 * advanced decks) answers a touch and hold on a touch screen.
 */
const TOUCH_WORDING: readonly (readonly [RegExp, string])[] = [
  [/Right-click/g, 'Touch and hold'],
  [/right-click/g, 'touch and hold'],
  [/Click/g, 'Tap'],
  [/click/g, 'tap'],
];

const touchify = (copy: Record<string, string>): Record<string, string> =>
  Object.fromEntries(
    Object.entries(copy).map(([key, text]) => [
      key,
      TOUCH_WORDING.reduce(
        (acc, [pattern, replacement]) => acc.replace(pattern, replacement),
        text
      ),
    ])
  );

export const HELP = (IS_COARSE_POINTER ? touchify(HELP_DESKTOP) : HELP_DESKTOP) as Record<
  keyof typeof HELP_DESKTOP,
  string
>;

/** Gallery tile: leads with the tone's own name, then its block type
    (NAM/IR/CAB/EQ/DUAL - same abbreviation ChainMapStrip's own chips use). */
export const toneTileHelp = (title: string, blockType: ToneBlock['blockType']) =>
  IS_COARSE_POINTER
    ? `${title} · ${BLOCK_TYPE_LABEL[blockType]}. Tap: open · drag: reorder · touch and hold: menu.`
    : `${title} · ${BLOCK_TYPE_LABEL[blockType]}. Click: open · drag: reorder · ${alt('drag')}: duplicate · right-click: copy / load file.`;
