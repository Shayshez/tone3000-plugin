/**
 * Chain state model, mirroring the native `getChainState` payload.
 *
 * Design notes:
 * - Tone metadata is *nested* under `tone` (never spread into the block), so
 *   runtime fields can't collide with API fields and the tone object stays a
 *   verbatim copy of what TONE3000 returned.
 * - User-editable settings live under `params`, separate from runtime status
 *   (`loaded`, `modelLoading`). A future shareable chain preset is just
 *   `{ tone, activeModelId, params }` per block.
 * - `revision` is a monotonic counter bumped by native on every mutation;
 *   pollers pass it back to `getChainState` and get a tiny
 *   `{ revision, unchanged: true }` reply when nothing changed.
 */

/**
 * Per-block 8-band EQ. Runs on the block's wet signal after its model by
 * default (before the dry/wet mix), or between the block's
 * input gain and its model when `pre` is on.
 *
 * Fixed channel-strip roles by index, no user-facing type selector (see
 * `roleForBandIndex`): band 0 is always Low Cut, band 1 Low Shelf, the last
 * two bands are High Shelf then High Cut, everything between is a Bell.
 * Low/High Cut get a discrete Pole count (see `EQ_POLE_OPTIONS`) instead of
 * Gain. Mirrors `BlockEq::Band`/`BlockEq::roleForIndex` on the native side.
 */
export type EqBandRole = 'lowcut' | 'lowshelf' | 'bell' | 'highshelf' | 'highcut';

export interface EqBand {
  freqHz: number;
  gainDb: number;
  q: number;
  /** Pole count (1/3/4/6/8, see `EQ_POLE_OPTIONS`), meaningful only for
      Low/High Cut bands; present on every band for a uniform shape. */
  poles: number;
  /** Per-band bypass (the band's own icon doubles as this toggle). Low/High
      Cut default off; every other band defaults on. */
  on: boolean;
}

export interface BlockEqParams {
  /** EQ power/bypass. Band settings persist while disabled. */
  enabled: boolean;
  /** Position: true = before the block's model (after its input gain),
      false = after the model on the wet path (default). */
  pre: boolean;
  bands: EqBand[];
}

/** Which sides of lane item `index` already have an empty "+" slot right
    next to it - the Add Slot Left/Right menu rows hide on those sides
    (adding a second empty slot beside an existing one is pointless). */
export function adjacentInsertSlots(
  items: ChainItem[],
  index: number
): { left: boolean; right: boolean } {
  const isSlot = (i: number) => i >= 0 && i < items.length && isInsertSlot(items[i]);
  return { left: isSlot(index - 1), right: isSlot(index + 1) };
}

/** Positional MIDI target for lane item `index`'s power ("block3Power" =
    the lane's third non-slot block, matching the native MidiMapper's own
    count), or null past the mappable range / on an empty slot. */
export const BLOCK_POWER_MIDI_TARGETS = 12;
export function blockPowerMidiTarget(items: ChainItem[], index: number): string | null {
  if (isInsertSlot(items[index])) return null;
  const position = items.slice(0, index + 1).filter((i) => !isInsertSlot(i)).length;
  return position <= BLOCK_POWER_MIDI_TARGETS ? `block${position}Power` : null;
}

export const EQ_NUM_BANDS = 8;
export const EQ_MIN_FREQ_HZ = 20;
export const EQ_MAX_FREQ_HZ = 20000;
export const EQ_MAX_ABS_GAIN_DB = 24;
export const EQ_MIN_Q = 0.1;
export const EQ_MAX_Q = 20;

/** Supported Low/High Cut pole counts and their dB/oct slope. */
export const EQ_POLE_OPTIONS = [1, 3, 4, 6, 8] as const;
export const EQ_POLE_DB_PER_OCT: Record<number, number> = { 1: 6, 3: 18, 4: 24, 6: 36, 8: 48 };

/** Fixed channel-strip role by position (mirrors native `roleForIndex`). */
export function roleForBandIndex(index: number, numBands: number = EQ_NUM_BANDS): EqBandRole {
  if (index === 0) return 'lowcut';
  if (index === 1) return 'lowshelf';
  if (index === numBands - 2) return 'highshelf';
  if (index === numBands - 1) return 'highcut';
  return 'bell';
}

/** A bell/shelf band at ~0 dB is inert; cuts shape by nature once on. A band
    with `on === false` is always inert regardless of its other settings. */
export function isEqBandActive(band: EqBand, index: number): boolean {
  if (!band.on) return false;
  const role = roleForBandIndex(index);
  if (role === 'lowcut' || role === 'highcut') return true;
  return Math.abs(band.gainDb) >= 0.05;
}

/** True when the EQ has no audible effect (all bands inert). */
export function isEqFlat(eq: BlockEqParams): boolean {
  return !eq.bands.some((band, i) => isEqBandActive(band, i));
}

/**
 * NAM slimmable-size requests (the native SetSlimmableSize domain): 0 asks
 * for the smallest tier of an A2 container ("lite"), 1 for the largest
 * ("full"). NAM assigns a boundary value to the tier above it, so on a
 * two-tier container everything below 0.5 runs lite and 0.5 up runs full —
 * which is what `isSlimSizeFull` mirrors for display.
 */
export const SLIM_SIZE_LITE = 0;
export const SLIM_SIZE_FULL = 1;

export function isSlimSizeFull(slimSize: number): boolean {
  return slimSize >= 0.5;
}

/** Per-block user-editable parameters (all persisted with the plugin state). */
export interface BlockParams {
  /** Block participates in processing (per-block on/off). */
  enabled: boolean;
  /** Loudness normalization toggle, NAM blocks only (off = the capture's
      raw level). IR blocks are always normalized natively; this flag is
      inert for them. */
  normalize: boolean;
  /** NAM A2 size in NAM's slimmable-size domain (0..1): the CPU/quality
      request the block's engine runs at. Only the endpoints are set today
      (see SLIM_SIZE_LITE/FULL); inert for IR blocks. Set via
      `setBlockSlimSize`, not `setBlockParam`: changing it retiers the
      loaded engine natively. */
  slimSize: number;
  /** Normalized 0..1; 0.5 = unity, ±24 dB. Drives the block's DSP. */
  inputGain: number;
  /** Normalized 0..1; 0.5 = unity, ±24 dB. */
  outputGain: number;
  /** Dry/wet: 0 = dry, 1 = wet. */
  mix: number;
  /** Normalized 0..1 -> 0-1000ms, delay before the wet signal enters the
      IR's convolver. IR blocks only; inert for NAM blocks. */
  predelay: number;
  /** IR envelope: a 2-segment Attack/Decay shape (Space Designer-style) over
      the block's own detected content (irContentLengthMs, frozen at load).
      IR blocks only; inert for NAM blocks. Set via `setBlockIrDecay`, not
      `setBlockParam`: unlike this list's other continuous params (real-time
      smoothers), it rebuilds the convolver engine off-thread, and all six
      values travel together in one call so a drag on one can't clobber
      another's in-flight value.

      `decayLength` is the TOTAL truncated length - the real "End" position,
      a fraction of the full detected content, matching the old standalone
      Length knob's own convention. `attackLength` is NOT an independent
      length - it's a fraction *of that total*, marking where the envelope's
      peak (the Attack/Decay boundary) sits within it, naturally bounded to
      [0, the total] by construction: dragging Attack Length alone can never
      change the total window length, only Decay Length does that.

      `initLevel` is the level at sample 0 (the origin, not part of either
      segment). The Attack segment runs from there up to unity/0dB - pinned,
      not adjustable: standard AD-envelope semantics (Attack always reaches
      full level; only Decay's target level is adjustable) - at
      `attackLength`'s position; the Decay segment continues from that peak
      to the truncated content's end (`decayLevel`). Both adjustable levels
      are normalized 0..1, unipolar attenuation-only (via percentScale: 1.0
      = unity/0dB, 0.0 = genuine silence - see decayEnvelope.ts's levelToDb,
      mirroring prepareIrShapeRebuild's own native formula). Defaults
      (attackLength 0.0, decayLength 1.0, every level 1.0) are a genuine
      no-op: no attack ramp, decay spans the full content, flat/unity
      envelope. */
  initLevel: number;
  attackLength: number;
  /** Continuous per-segment envelope shape, normalized 0..1: 0.5 (default)
      is a linear-in-dB ramp, sweeping toward more front-loaded below and
      more back-loaded above - see knobScale.ts's curveScale and
      decayEnvelope.ts's shared curve formula. */
  attackCurve: number;
  decayLength: number;
  decayLevel: number;
  decayCurve: number;
  /** IR Size: vari-speed duration/pitch over the block's frozen source,
      normalized 0..1, 0.5 = 100%/unchanged (see sizePercentScale in
      knobScale.ts for the exact 10%-1000% taper). IR blocks only. Set via
      `setBlockIrSize`, not `setBlockParam`: same off-thread rebuild shape as
      the envelope params above, committed once per drag gesture (on
      release) rather than continuously. Applied upstream of the envelope
      natively (a declared-sample-rate scale, not a change to the trimmed
      buffer's sample count), so every envelope fraction above keeps landing
      at the same relative position with no special-casing needed here. */
  size: number;
  /** IR Width: stereo-image control, normalized 0..1, 0.5 = 0%/mono (see
      widthPercentScale in knobScale.ts for the -200%..+200% mapping). IR
      blocks only. Default/reset is 0.75 (100%/full original stereo), NOT
      the bipolar center - 0.5 is genuinely mono, and every stereo IR
      played its true recorded image before this control existed, so the
      no-op starting point has to be 100%, not the knob's visual center.
      Locked at that default in the UI whenever the loaded IR is mono (see
      ToneBlock.irNumChannels). Set via `setBlockIrWidth`, same off-thread/
      commit-on-release shape as `size`. */
  width: number;
  /** Trim Init: manually-toggled leading-silence removal, off by default -
      never applied automatically (an RMS-threshold heuristic could clip an
      intentionally quiet start). When on, native shifts where the trimmed/
      enveloped kernel starts to the block's detected onset (past any
      leading silence in the source file), so it stops masquerading as
      unwanted Predelay. IR blocks only. Set via `setBlockIrTrimInit`, same
      off-thread rebuild shape as `size`/`width` but a plain toggle, not a
      drag gesture. */
  trimInit: boolean;
  /** Which onset detection Trim Init uses when `trimInit` is on: the
      standard threshold (false) or the relaxed, less-sensitive one (true,
      see native's ChainBlock::irOnsetSamplesRelaxed) for sources whose
      audible transient builds up slowly enough that the standard threshold
      finds "onset" too early. Meaningless while `trimInit` is false; set
      together with it via `setBlockIrTrimInit`'s third argument so a click
      through Off/Std/Lax is always exactly one undo step. */
  trimRelaxed: boolean;
  /** Reverse: manually-toggled backward playback of the fully-shaped kernel
      (applied last, after Trim Init and the envelope), off by default. IR
      blocks only. Set via `setBlockIrReverse`, same off-thread rebuild
      shape as `trimInit` - a plain toggle, not a drag gesture. */
  reverse: boolean;
  /** Per-block 8-band EQ. Flat = skipped entirely on the audio thread. */
  eq: BlockEqParams;
  /** Active channel, 0-3 = A-D (see ScenesState). A channel is a full
      version of the block: model and every setting except bypass. */
  channel?: number;
  /** Per channel slot: whether it holds settings yet (the active one always
      does; an unused slot starts as a copy of the current channel). */
  channelsUsed?: boolean[];
  /** DUAL_MONO only: recombine controls for the block's two fixed child
      slots (see ToneBlock.dualLeft/dualRight). Normalized 0..1, constant-
      power pan (0 = hard left, 1 = hard right); defaults hard-left/hard-
      right/full-width - each side keeps its own place until the user dials
      in something else. Set via `setDualImage`, called continuously while a
      knob drags (native-side smoothing handles click-avoidance). Inert for
      every other block type. */
  dualLeftPan: number;
  dualRightPan: number;
  dualWidth: number;
  /** DUAL_MONO only: Link toggle - real per-block state (see
      `setDualLinked`'s own native comment), but the actual mirror/sync
      behavior when linked (Pan reflected around center, Mix/Vol matched)
      runs client-side - this just tells the UI whether to do it. */
  dualLinked: boolean;
  /** DUAL_MONO only: exclusive per-side solo (see `setDualSolo`) - at most
      one is ever true. */
  dualSoloLeft: boolean;
  dualSoloRight: boolean;
  /** DUAL_MONO only: per-side Mute (see `setDualMuted`) - true silence via
      a recombine-level gain, unconditional whether the side is empty or
      loaded. Deliberately NOT the child's own `enabled` (that's bypass,
      which crossfades to the dry unprocessed input - audible, not
      silent). */
  dualLeftMuted: boolean;
  dualRightMuted: boolean;
  /** DUAL_MONO only: per-side polarity flip (Ø) - two amp captures don't
      share a polarity convention, so one side can arrive 180 degrees out
      against the other. Independent per side (unlike Solo). Set via
      `setDualInvert`. */
  dualLeftInvert: boolean;
  dualRightInvert: boolean;
  /** DUAL_MONO only: Align - corrective delay + advanced deck (Wobble/
      Crossover/Diffuse) between the two sides' raw output, before the Pan/
      Width recombine above (see native StereoOffset.h / `setDualAlign`).
      offset/wobble/crossover are normalized 0..1 (offset is bipolar, 0.5 =
      center = 0 ms), same encoding as the global chain-level Align. Off and
      centered by default, same no-op-until-asked-for reasoning as the
      global one. */
  dualAlignEnabled: boolean;
  dualAlignOffset: number;
  dualAlignWobble: number;
  dualAlignWobbleEnabled: boolean;
  dualAlignCrossover: number;
  dualAlignCrossoverEnabled: boolean;
  dualAlignDiffuseEnabled: boolean;
  /** Master bypass for the whole Stereo Processing screen (Align + Ø) -
      forces both neutral without touching any of the dialed-in values
      above (see `setDualStereoProcessingEnabled`). Default true (not
      bypassed). */
  dualStereoProcessingEnabled: boolean;
}

/**
 * An insert placeholder (pass-through slot where new tones are added).
 * Native keeps each lane at its minimum slot layout (at least 5 tiles and
 * always one trailing insert once every minimum slot holds a tone), so a
 * lane can carry several of these, each independently reorderable.
 */
export interface InsertSlot {
  blockId: string;
  kind: 'insert';
}

/**
 * Slim tone projection shipped by native (see makeToneSummary in
 * ProcessorChain.cpp). Only what the UI renders; the full API payload
 * (model URLs, tags, …) stays native-side.
 */
export interface ToneSummary {
  id: number;
  title: string;
  format?: string;
  gear?: string;
  /** Drop-loaded local file(s) (no catalog metadata): the tile shows a file
      glyph and the detail card drops share / counts; the model picker feeds
      off `models` instead of the API. */
  local?: boolean;
  /** First image only (block artwork). */
  images?: string[];
  user?: { username: string; avatar_url: string };
  /** When the tone was published; absent on older stored tones. */
  published_at?: string;
  /** Only the active model for catalog tones (the picker pages the catalog
      from the API). Local tones carry all their dropped files, each with
      its stash model_url: that's what a switch call needs, and there is no
      catalog to fetch it from. */
  models: { id: number; name: string; model_url?: string }[];
  /** Catalog totals. NAM uses `a2_models_count` (the plugin only loads v2);
      IR and other formats use `models_count`. */
  models_count: number;
  a2_models_count: number;
  /** Public tallies for the tone-info stats row (downloads, then bookmarks). */
  downloads_count: number;
  favorites_count: number;
  /** Whether the signed-in user has favorited this tone. Set by the
      expand-time /tones/{id} sync; omitted when unknown (signed out, or
      a payload from before the field existed). */
  is_favorite?: boolean;
  /** Canonical public page URL (title slug + id) for the share action.
      Absent on tones stored before native shipped it. */
  url?: string;
}

/** A real tone block in the chain. */
export interface ToneBlock {
  blockId: string;
  kind: 'tone';
  /** Real native block type. 'cab' is a genuine ChainBlockType::CAB block
      (site tones tagged gear === "cab"): structurally minimal, no predelay/
      envelope/waveform fields (see ChainBlock.tsx's own isCab branches).
      'ir'/'nam' render the existing full card; tone.format still reports
      "ir" for a cab block (that's the catalog's format, not the native
      split), so this field - not tone.format - is the thing to branch on.
      'eq' is a standalone ChainBlockType::EQ block (see addEqBlock): no
      model/tone at all, just this block's own eq processing everything
      that passes through - ChainBlock.tsx renders it as a completely
      separate, much simpler card (see its own isEq early return).
      'dualMono' is a fixed, non-extensible pair (ChainBlockType::DUAL_MONO,
      see addDualMonoBlock): also no tone/model of its own - dualLeft/
      dualRight below carry its two fixed child slots instead - rendered as
      its own separate card too (ChainBlock.tsx's isDualMono branch). */
  blockType: 'nam' | 'ir' | 'cab' | 'eq' | 'dualMono';
  /** Tone metadata for rendering (slim projection of the API tone). */
  tone: ToneSummary;
  activeModelId: number;
  /** True when a model is loaded and processing. During a model switch this
      stays true: the previous model keeps playing until the new one is
      spliced in natively (with a short fade). On a failed load it drops to
      false: the block falls out of processing (matching the new tone/model
      already shown in the UI) until a retry succeeds. */
  loaded: boolean;
  /** True when the last download/prepare of the active model failed (network
      down, TONE3000 unreachable). The block renders a retry affordance
      instead of loading dots; retry re-queues via `retryModelLoad`. */
  loadFailed: boolean;
  /** True while a download/prepare of the active model is in flight. Drives
      the loading overlays (not `loaded`, which stays true mid-switch so the
      old model keeps playing). */
  modelLoading: boolean;
  /** Engine-selection signal only (uniform vs non-uniform convolution); no
      audible meaning - see irCategory below. */
  irLong: boolean;
  /** Detected IR content length in ms (native, RMS-threshold based): where
      the waveform display's auto-fit trims to. Already scaled by Size's
      clamped ratio (see irRawContentLengthMs below), so it moves with the
      Size knob. 0 for NAM blocks and IR blocks not yet loaded. */
  irContentLengthMs: number;
  /** Load-time IR content length in ms, *not* scaled by Size - fixed for
      the life of the loaded file. This is what the Size knob's own display
      clamps against (see sizePercentScale in knobScale.ts); using
      irContentLengthMs there instead would be circular, since that value
      already reflects the current clamped Size ratio. 0 for NAM blocks and
      IR blocks not yet loaded. */
  irRawContentLengthMs: number;
  /** Detected onset (see computeIrOnsetSamples), as a fraction 0..1 of the
      exact span the waveform backdrop (getIrWaveform's mins/maxs) was
      downsampled over - deliberately NOT relative to irRawContentLengthMs
      above, which is already trim-adjusted once Trim Init is on and would
      make this circular. Always shipped regardless of BlockParams.trimInit;
      the waveform display crops with it only when that's on. 0 for NAM
      blocks and IR blocks not yet loaded. */
  irOnsetFraction: number;
  /** Explicit IR content category (IR blocks only; meaningless for NAM).
      'cab' = real cabinet content: 100% default mix.
      'irPlayer' = anything else (space/reverb/outboard/experimental/generic
      IR): 50% default mix. Drives the Mix knob's default/Alt-click
      reset and the Out knob help. Editable via setBlockIrCategory. */
  irCategory: 'cab' | 'irPlayer';
  /** Channels in the loaded IR file (1 or 2, native ChainBlock::
      irNumChannels). The sole source of truth for whether Width has a real
      stereo image to work with; the Width knob locks to mono/disabled
      whenever this is 1. 1 for NAM blocks and IR blocks not yet loaded. */
  irNumChannels: number;
  /** NAM calibration metadata (dBu) off the loaded model; absent when the
      model carries none (or nothing is loaded yet). `inputLevelDbu` feeds
      input calibration; `outputLevelDbu` feeds the mid-chain calibrated
      hand-off that supersedes normalization (see the post-model gain stage
      in Processor.cpp). Both are guaranteed finite by native. */
  inputLevelDbu?: number;
  outputLevelDbu?: number;
  params: BlockParams;
  /** DUAL_MONO only: the block's two fixed child slots, each 0 or 1 real
      block (never an extensible chain of its own - see blockType's own
      comment). Reuses ChainItem's own shape (insert slot when empty, a
      full ToneBlock when filled) rather than a bespoke type, so the same
      tile-preview rendering works for either state with no special-casing. */
  dualLeft?: ChainItem[];
  dualRight?: ChainItem[];
  /** DUAL_MONO only: true when the enclosing lane currently has no spare
      physical channel to widen into, so Pan/Width are momentarily inert
      (native folds to mono regardless of their setting) - mirrors
      runDualMono's own widen-vs-fold decision. Drives whether the detail
      view dims those three knobs. */
  dualChannelLimited?: boolean;
}

export type ChainItem = InsertSlot | ToneBlock;

export function isInsertSlot(item: ChainItem): item is InsertSlot {
  return item.kind === 'insert';
}

/** Abbreviated label per native block type - shared by ChainMapStrip's own
    strip chips and helpText.ts's toneTileHelp (gallery tile hover hint), so
    both read the same abbreviation for a given type. Lives here (not in
    either component file) since ChainMapStrip.tsx already imports from
    helpText.ts - a shared const avoids the circular import that would
    otherwise create. */
export const BLOCK_TYPE_LABEL: Record<ToneBlock['blockType'], string> = {
  nam: 'NAM',
  ir: 'IR',
  cab: 'CAB',
  eq: 'EQ',
  dualMono: 'DUAL',
};

/** One entry in the native preset store (see getPresetList). */
export interface PresetInfo {
  id: string;
  name: string;
  /** Bundled TONE3000 preset; read-only (no rename/delete). */
  factory: boolean;
}

/** The preset shown in the top-bar pill. */
export interface ActivePreset {
  id: string;
  name: string;
}

export interface ChainState {
  revision: number;
  /** Chain edit history (undo/redo). Native flips these together with a
      revision bump, so pollers always see them fresh. */
  canUndo: boolean;
  canRedo: boolean;
  /** Whether the native block clipboard holds a copied block (Paste enabled
      on insert slots). The clipboard is a self-contained snapshot, so this
      survives preset switches and deleting the copied block. */
  canPasteBlock?: boolean;
  /** Whether the native EQ clipboard holds copied bands (EQ Paste enabled). */
  canPasteEq?: boolean;
  /** The 4 scenes (see ScenesState). */
  scenes?: ScenesState;
  /** True when nothing distinguishes the state from a fresh instance: empty
      mono chain, faceplate params at defaults, no active preset. Greys out
      the top bar's New button. */
  atDefault: boolean;
  /** Active preset, absent when none is loaded. Changes with revision bumps. */
  preset?: ActivePreset;
  /** True when a real stereo source feeds the plugin (stereo host bus or a
      stereo standalone input device). Drives the faceplate input-mode button
      and the dual input meters. */
  stereoInput: boolean;
  /** True when the plugin can drive two distinct output channels (stereo
      host bus, 2+ channel standalone output device). False greys the
      Spread group (native keeps it idle: a double can't be heard on one
      channel), while stereo chains keep running and native sums them to
      mono; the pan rail dims its pans and shows the MONO chip. */
  stereoOutput: boolean;
  /** True in the standalone app; gates standalone-only settings. */
  standalone: boolean;
  /** Which channels of a stereo source feed the plugin (faceplate button):
      both, or one mirrored onto both. */
  inputMode: InputMode;
  /** Default NAM A2 size stamped on newly added blocks (machine-wide user
      setting; existing blocks keep their own `params.slimSize`). Set via
      `setNamSlimSizeDefault`. */
  namSlimSizeDefault: number;
  /** Multi-core processing (machine-wide user setting). When true, oversampled
      NAM models split their phase instances across cores; set via
      `setMultiCore`. */
  multiCore: boolean;
  /** The chain-domain processing rate (fixed 48000: the whole chain runs at
      48 kHz behind one resampling boundary). The EQ curve math needs it to
      mirror the audio exactly. */
  sampleRate: number;
  chain: ChainItem[];
}

/** Input channel mode (mirrors Processor::InputMode). */
export type InputMode = 'stereo' | 'left' | 'right';

/** Minimal reply when the caller's revision is still current. */
export interface ChainStateUnchanged {
  revision: number;
  unchanged: true;
}

export type ChainStateResponse = ChainState | ChainStateUnchanged;

export function isUnchanged(res: ChainStateResponse): res is ChainStateUnchanged {
  return 'unchanged' in res && res.unchanged === true;
}

/** Param names accepted by the native `setBlockParam` function. */
export type BlockParamName =
  | 'enabled'
  | 'normalize'
  | 'inputGain'
  | 'outputGain'
  | 'mix'
  | 'predelay';

/** Payload of the native `getMeterLevels` function (all values dB, -60 floor).
    Main meters ship as [L, R] pairs; mono sources report L == R. */
export interface MeterLevels {
  input: [number, number];
  output: [number, number];
  blocks: Record<string, { in: number; out: number; alignCorrelation?: number }>;
  /** Audio-callback load, 0..1 proportion of the real-time budget. */
  cpu: number;
}

/** One block's state in one scene. */
export interface SceneBlockState {
  enabled: boolean;
  /** Channel 0-3 = A-D. */
  channel: number;
}

/** Scenes & channels (see the native SCENES & CHANNELS section in
    Processor.h), Fractal-style: every block has four channels (A-D), each a
    full version of the block; a scene picks, per block, bypass + channel,
    plus its own name and output level. Editing a block edits its active
    channel, so the change reaches every scene using that channel. */
export interface ScenesState {
  /** Active scene, 0-3. */
  active: number;
  /** Per-slot names ('' = unnamed; show the number). */
  names: string[];
  /** Per-slot output level in dB (-24..+12). */
  levels: number[];
  /** Per scene: blockId -> state. A block missing from a scene (added while
      another scene was active) takes its live state when that scene is
      first selected. The active scene mirrors the live chain. */
  blocks: Record<string, SceneBlockState>[];
}

export const NUM_SCENES = 4;
export const NUM_CHANNELS = 4;
export const CHANNEL_LETTERS = ['A', 'B', 'C', 'D'] as const;

export const EMPTY_SCENES: ScenesState = {
  active: 0,
  names: Array(NUM_SCENES).fill(''),
  levels: Array(NUM_SCENES).fill(0),
  blocks: Array.from({ length: NUM_SCENES }, () => ({})),
};
