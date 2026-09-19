import React, { useCallback, useEffect, useMemo, useRef, useState } from 'react';
import {
  ArrowLeft,
  ArrowLeftRight,
  Bookmark,
  Download,
  Equal,
  FolderClosed,
  Gauge,
  Info,
  Plus,
  Power,
  Share,
  Trash2,
} from './icons';
import { ChainMapStrip } from './ChainMapStrip';
import { ToneImage } from './GearIcon';
import { WaveformDisplay } from './WaveformDisplay';
import { IrEnvelopeGraph } from './IrEnvelopeGraph';
import type { EnvelopePatch } from './IrEnvelopeGraph';
import { useIrWaveform } from '../hooks/useIrWaveform';
import { rem } from '../hooks/useUiScale';
import { KnobControl } from './KnobControl';
import { EditableChip, ToggleChip } from './EditableChip';
import {
  gainDbScale,
  predelayMsScale,
  lengthMsScale,
  attackLengthMsScale,
  curveScale,
  dualPanScale,
  percentScale,
  sizePercentScale,
  widthPercentScale,
} from './knobScale';
import type { KnobScale } from './knobScale';
import { BusyOverlay, LoadingDots } from './LoadingDots';
import { ModelSelect } from './ModelSelect';
import { RetryLoadBadge } from './RetryLoadBadge';
import { BlockMeter } from './BlockMeter';
import { BlockEqView } from './BlockEqView';
import type { EqViewMode } from './BlockEqView';
import { BlockInfoPanel } from './BlockInfoPanel';
import { meterId } from '../hooks/useMeters';
import { useChainActions } from '../hooks/useChainActions';
import { useParameter } from '../hooks/useParameter';
import type { BlockParamName, ChainItem, ToneBlock, ToneSummary } from '../types/chain';
import { catalogModelCount, type Model, type Tone } from '../types/tone';
import {
  isEqFlat,
  isInsertSlot,
  isSlimSizeFull,
  SLIM_SIZE_FULL,
  SLIM_SIZE_LITE,
} from '../types/chain';
import {
  CARD_WIDTH,
  CARD_HEIGHT,
  CARD_RADIUS,
  HEADER_HEIGHT,
  BODY_HEIGHT,
  BODY_PADDING,
} from './chainLayout';
import { formatCount } from '../t3k/formatCount';
import { timeAgoShort } from '../t3k/timeAgoShort';
import { formatLabel, gearLabel } from '../t3k/labels';
import { AvatarImage } from './AvatarFallback';
import { FormatBadge } from './FormatBadge';
import { HELP, helpProps } from './helpText';
import { useBlockNormalizeControlEnabled, useBlockSizeControlEnabled } from './uiPreferences';
import { useToast } from './Toast';
import { ChromeIconButton, ChromeTextButton, chromeIcon } from './ChromeIconButton';
import { T3K_API } from '../t3k/config';
import {
  BORDER,
  GRAY,
  ICON_BOX_RADIUS,
  ICON_BOX_SIZE,
  ICON_SIZE,
  KNOB_SIZE_SECONDARY,
  FONT_MONO,
  MUTED,
  SEGMENTED_TRACK,
  TEXT_BOX_HEIGHT,
  WHITE,
  segmentedCellStyle,
  segmentedGroupStyle,
  uiOffClass,
} from './theme';

/** Tone image; matches the Figma detail mock (fits body with model select). */
const IMAGE_SIZE = 192;
/** Info view artwork; Figma detail mock is 160 beside the metadata column. */
const IMAGE_SIZE_INFO = 160;
/** Mini meter height in the side rails (meter sits centered above its knob). */
const RAIL_METER_HEIGHT = 160;
/** Centers the normalize (=) chrome box on the Out knob. */
const NORMALIZE_BUTTON_OFFSET = -(KNOB_SIZE_SECONDARY - ICON_BOX_SIZE) / 2;
// IR blocks (!isNam) add a Delay knob beside In in the Input rail, widening
// that rail (84rem: In 36 + gap 12 + Delay 36) past the Output rail (36rem:
// just Out, since the normalize button is NAM-only) - a real ~48rem
// asymmetry. A compensating spacer was tried in the Output rail to equalize
// the two rails, but the row's Center column (flex:1) is what actually pays
// for any width added to a fixed-width sibling: the spacer just shrank the
// waveform/dropdown by the same 48rem, which cost more than the asymmetry it
// fixed. True width parity would require *narrowing* the Input rail instead
// (e.g. stacking Delay under In rather than beside it) - a real layout
// change to a previously deliberate decision, not something to do as a
// side effect of a symmetry pass. Left as-is for now.
/** IR blocks (compact, non-Info view): wide waveform strip, full
    Center-column width, replacing the square artwork. Sized to 100 (up from
    an original 60) once the shaping row shrank from knobs to compact
    EditableChips (see IrEnvelopeGraph.tsx) - that freed enough of the card's
    fixed body height to make the graph meaningfully more legible without
    touching BODY_HEIGHT. */
const IR_WAVEFORM_HEIGHT = 100;
/** Abstract SVG coordinate width for the strip - NOT a literal CSS pixel
    width (the wrapper is width: 100%, and WaveformDisplay's viewBox stretches
    to fit via preserveAspectRatio="none"), just needs to be self-consistent
    for the mins/maxs/cut/decay math inside it. */
const IR_WAVEFORM_WIDTH_UNITS = 600;

/** Envelope shaping row's chip style - same track language as BlockEqView's
    own Freq/Gain/Q chips (SEGMENTED_TRACK pill, TEXT_BOX_HEIGHT tall), just
    tighter (smaller font, less padding): the six numeric chips (Init/A Len/
    A Crv/D Len/D Lvl/D Crv) don't fit the card width at EQ's own 3-chip
    sizing. The value area width is untouched by any later tightening pass,
    since that's sized for the widest real reading (Decay Length up to
    "20.00s" post-Size) and shrinking it risks visual overlap with the next
    chip, not just a tighter look. */
const ENVELOPE_CHIP_FONT_SIZE = 10;
const chipStyle: React.CSSProperties = {
  display: 'flex',
  alignItems: 'center',
  justifyContent: 'center',
  gap: '3rem',
  // Fixed outer width, not just EditableChip's own fixed value-area width:
  // the six chips' labels aren't all the same length (Init is 4 chars, the
  // rest 5), so relying on intrinsic sizing alone still left it visibly
  // narrower than A Len/A Crv/D Len/D Crv. Sized for the widest combination
  // (a 5-char label + the value area below).
  width: '78rem',
  height: `${TEXT_BOX_HEIGHT}rem`,
  padding: '0 2rem',
  borderRadius: rem(ICON_BOX_RADIUS),
  border: 'none',
  backgroundColor: SEGMENTED_TRACK,
  boxSizing: 'border-box',
  whiteSpace: 'nowrap',
};

/** Trim/Rev - the row's plain on/off toggles, not chipStyle's numeric
    readouts: "Trim"/"Rev" plus "On"/"Off" both read shorter than any
    numeric chip's label+value, so a dedicated, narrower style saves real
    row width instead of inheriting chipStyle's width sized for "20.00s".
    Identical to each other on purpose - a matched pair reads as one
    symmetric unit at the end of the row. */
const toggleChipStyle: React.CSSProperties = { ...chipStyle, width: '58rem' };
const TOGGLE_CHIP_VALUE_WIDTH = 24;

/** Downloads / bookmarks / models count with a leading icon (same pattern as ToneBrowser).
    `fontSize` defaults to the full card's size; the IR compact card's meta
    row (see CompactToneMetaRow) passes a smaller one to fit its tighter
    vertical budget. */
const CountStat: React.FC<{ icon: React.ReactNode; value: number; fontSize?: number }> = ({
  icon,
  value,
  fontSize = 14,
}) => (
  <div style={{ display: 'flex', alignItems: 'center', gap: '8rem' }}>
    <span style={{ display: 'grid', placeItems: 'center', color: GRAY }}>{icon}</span>
    <span style={{ fontSize: `${fontSize}rem`, fontWeight: 400, color: MUTED }}>
      {formatCount(value)}
    </span>
  </div>
);

/** Bookmark tally. Signed-in: a toggle (outline idle, white fill when favorited). */
const BookmarkStat: React.FC<{
  value: number;
  favorited: boolean;
  onToggle?: () => void;
  iconSize?: number;
  fontSize?: number;
}> = ({ value, favorited, onToggle, iconSize = 16, fontSize = 14 }) => {
  const icon = (
    <Bookmark size={iconSize} fill={favorited ? WHITE : 'none'} color={favorited ? WHITE : GRAY} />
  );
  const body = (
    <>
      <span style={{ display: 'grid', placeItems: 'center' }}>{icon}</span>
      <span style={{ fontSize: `${fontSize}rem`, fontWeight: 400, color: MUTED }}>
        {formatCount(value)}
      </span>
    </>
  );
  const row: React.CSSProperties = {
    display: 'flex',
    alignItems: 'center',
    gap: '8rem',
  };
  if (!onToggle) return <div style={row}>{body}</div>;
  return (
    <button
      type="button"
      onClick={onToggle}
      {...helpProps(favorited ? HELP.unfavoriteTone : HELP.favoriteTone)}
      style={{
        ...row,
        background: 'transparent',
        border: 'none',
        padding: 0,
        cursor: 'pointer',
        color: 'inherit',
      }}
    >
      {body}
    </button>
  );
};

/** Creator + downloads/favorites, condensed onto one line and dropped in
    beside the IR compact card's gear+badge row (via that row's own
    space-between) rather than as a row of its own - the IR compact layout's
    fixed body-height budget has no vertical slack to spare (see
    IR_WAVEFORM_HEIGHT's comment), only the gear+badge row's leftover
    horizontal width. A smaller avatar and icon/font sizes throughout keep it
    legible at that row's height rather than clipping or wrapping. Omitted
    for local drops (no catalog creator/counts to show), same as the full
    layout. */
const CompactToneMetaRow: React.FC<{
  tone: ToneSummary;
  favoritesCount: number;
  favorited: boolean;
  onToggleFavorite?: () => void;
}> = ({ tone, favoritesCount, favorited, onToggleFavorite }) => (
  <div
    style={{
      display: 'flex',
      flexDirection: 'row',
      alignItems: 'center',
      gap: '16rem',
      minWidth: 0,
    }}
  >
    <CountStat icon={<Download size={13} />} value={tone.downloads_count ?? 0} fontSize={12} />
    <BookmarkStat
      value={favoritesCount}
      favorited={favorited}
      onToggle={onToggleFavorite}
      iconSize={13}
      fontSize={12}
    />
    {tone.user && (
      <div
        style={{
          display: 'flex',
          alignItems: 'center',
          gap: '6rem',
          minWidth: 0,
          overflow: 'hidden',
        }}
      >
        <div
          style={{
            width: '18rem',
            height: '18rem',
            borderRadius: '50%',
            overflow: 'hidden',
            flexShrink: 0,
          }}
        >
          <AvatarImage src={tone.user.avatar_url} alt={tone.user.username} size={18} />
        </div>
        <span
          style={{
            fontSize: '12rem',
            color: GRAY,
            fontWeight: 400,
            whiteSpace: 'nowrap',
            overflow: 'hidden',
            textOverflow: 'ellipsis',
          }}
        >
          {tone.user.username}
          {tone.published_at && (
            <span style={{ color: MUTED }}> · {timeAgoShort(tone.published_at)}</span>
          )}
        </span>
      </div>
    )}
  </div>
);

/** Model list + switch-model wiring for one block's `ModelSelect` picker -
    extracted so the Dual Mono compact card's per-side picker (see
    DualSideCard) can share the exact same fetch/switch behavior as the full
    card's own picker, rather than duplicating it. `block` is null while a
    Dual Mono side is empty; every returned value degrades to an inert
    default in that case (hooks still run unconditionally either way, same
    call order every render - required by the rules of hooks). Local tones
    own their model list directly; catalog tones fetch it client-side once
    (native persists only the active model). */
function useModelPicker(block: ToneBlock | null) {
  const actions = useChainActions();
  const tone = block?.tone;
  const isLocal = tone?.local === true;
  const [models, setModels] = useState<Model[]>([]);
  const [modelsLoading, setModelsLoading] = useState(false);
  const [isSwitchingModel, setIsSwitchingModel] = useState(false);
  const modelsFetchSeq = useRef(0);

  const fetchModels = useCallback(async () => {
    if (!tone || isLocal || !actions.authenticated) return;
    const seq = ++modelsFetchSeq.current;
    setModelsLoading(true);
    try {
      const list = await actions.listToneModels(tone.id, tone.format);
      if (seq === modelsFetchSeq.current) setModels(list);
    } catch (err) {
      // No error UI: the picker keeps the stored model, and opening it
      // retries (handleModelsOpen), so a transient failure never sticks.
      console.error('Failed to load models', err);
    } finally {
      if (seq === modelsFetchSeq.current) setModelsLoading(false);
    }
    // Primitive deps deliberately, not `tone` itself: `block` (and so
    // `tone`) gets a fresh object every poll tick, and re-fetching on every
    // one of those (instead of only a genuine tone-identity change) is
    // exactly what this was written to avoid - same reasoning the original,
    // non-extracted version of this fetch used.
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [actions, isLocal, tone?.format, tone?.id]);

  // Fetch on mount, tone identity change, and auth arrival.
  useEffect(() => {
    setModels([]);
    if (tone) void fetchModels();
    return () => {
      // eslint-disable-next-line react-hooks/exhaustive-deps
      modelsFetchSeq.current++;
    };
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [fetchModels, tone?.id]);

  const handleModelsOpen = useCallback(() => {
    if (!modelsLoading && models.length === 0) void fetchModels();
  }, [fetchModels, models.length, modelsLoading]);

  const modelOptions = !tone ? [] : isLocal ? tone.models : models.length ? models : tone.models;
  const modelsTotal = tone ? catalogModelCount(tone) : 0;

  const handleModelSelect = useCallback(
    async (id: string) => {
      if (!block || !tone || isSwitchingModel) return;
      const newModelId = parseInt(id, 10);
      if (isNaN(newModelId) || newModelId === block.activeModelId) return;
      const model = (isLocal ? tone.models : models).find((m) => m.id === newModelId);
      if (!model?.model_url) return;
      setIsSwitchingModel(true);
      try {
        await actions.switchModel(block.blockId, newModelId, {
          ...model,
          model_url: model.model_url,
        });
      } finally {
        setIsSwitchingModel(false);
      }
    },
    [actions, block, isLocal, isSwitchingModel, models, tone]
  );

  return { modelOptions, modelsLoading, modelsTotal, handleModelSelect, handleModelsOpen };
}

/** EQ view glyphs: 16×16, stroke inherits selected/muted color. */
const EqSlidersIcon: React.FC = () => (
  <svg
    viewBox="0 0 16 16"
    fill="none"
    stroke="currentColor"
    strokeWidth={1.33333}
    strokeLinecap="round"
    strokeLinejoin="round"
    style={{ width: rem(16), height: rem(16), display: 'block', flexShrink: 0 }}
  >
    <path d="M11.3333 6.66669V12.6667" />
    <path d="M4.66675 3.33331V9.33331" />
    <path d="M13.3333 4.66669C13.3333 3.56212 12.4378 2.66669 11.3333 2.66669C10.2287 2.66669 9.33325 3.56212 9.33325 4.66669C9.33325 5.77126 10.2287 6.66669 11.3333 6.66669C12.4378 6.66669 13.3333 5.77126 13.3333 4.66669Z" />
    <path d="M6.66675 11.3333C6.66675 10.2287 5.77132 9.33331 4.66675 9.33331C3.56218 9.33331 2.66675 10.2287 2.66675 11.3333C2.66675 12.4379 3.56218 13.3333 4.66675 13.3333C5.77132 13.3333 6.66675 12.4379 6.66675 11.3333Z" />
  </svg>
);

const EqCurveIcon: React.FC = () => (
  <svg
    viewBox="0 0 16 16"
    fill="none"
    stroke="currentColor"
    strokeWidth={1.5}
    strokeLinecap="round"
    style={{ width: rem(16), height: rem(16), display: 'block', flexShrink: 0 }}
  >
    <path d="M1 13.5C5 13.5 5.5 2.5 8 2.5C10.5 2.5 11 13.5 15 13.5" />
  </svg>
);

/** NAM A2 size chrome next to the power button. With the Settings "choose
    per block" preference on, the LITE/FULL segmented toggle; off, a
    read-only chip of the block's size styled like the toggle's unselected
    side, so it reads as the same control, locked (its help text points at
    the setting). The caller skips rendering entirely when there's nothing
    to flag (chip mode with the block at the new-block default). */
const BlockSizeControl: React.FC<{
  full: boolean;
  interactive: boolean;
  onChange: (full: boolean) => void;
}> = ({ full, interactive, onChange }) => {
  if (!interactive) {
    return (
      <div
        {...helpProps(HELP.blockSizeChip)}
        style={{ ...segmentedGroupStyle(), cursor: 'default' }}
      >
        <span style={{ ...segmentedCellStyle(), cursor: 'default', color: MUTED }}>
          <span className="cap-trim">{full ? 'FULL' : 'LITE'}</span>
        </span>
      </div>
    );
  }
  return (
    <div {...helpProps(HELP.blockSize)} style={segmentedGroupStyle()}>
      {([false, true] as const).map((isFull) => (
        <button
          key={String(isFull)}
          type="button"
          onClick={() => onChange(isFull)}
          style={{
            ...segmentedCellStyle(),
            color: full === isFull ? WHITE : MUTED,
            transition: 'color 0.15s ease',
          }}
        >
          <span className="cap-trim">{isFull ? 'FULL' : 'LITE'}</span>
        </button>
      ))}
    </div>
  );
};

/** "Cab" / "IR Player" pill, shown on both a plain IR block and a real CAB
    block. Purely presentational - see ChainBlock's handleCabCategoryClick
    for what a click actually does: a real ChainBlockType conversion
    (convertBlockType) off a CAB block or onto a plain IR block that isn't
    already flagged IrCategory::Cab, otherwise the lighter existing
    reclassification (setBlockIrCategory), which just resets Mix (and the
    native -18 dB cab pad) to the new category's fixed default. */
const IrCategoryControl: React.FC<{
  category: 'cab' | 'irPlayer';
  onChange: (category: 'cab' | 'irPlayer') => void;
}> = ({ category, onChange }) => (
  <div {...helpProps(HELP.blockIrCategory)} style={segmentedGroupStyle()}>
    {(['cab', 'irPlayer'] as const).map((option) => (
      <button
        key={option}
        type="button"
        onClick={() => onChange(option)}
        style={{
          ...segmentedCellStyle(),
          color: category === option ? WHITE : MUTED,
          transition: 'color 0.15s ease',
        }}
      >
        <span className="cap-trim">{option === 'cab' ? 'Cab' : 'IR Player'}</span>
      </button>
    ))}
  </div>
);

/** One side of a Dual Mono block's detail view: an empty "+" socket, or -
    once filled - a compact card carrying real per-block functionality
    (icon row, thumbnail+title+tags, stats+author, a model prev/next
    browser, and this side's own Pan/Mix/Vol) instead of the bare
    thumbnail+swap/remove the block used to show. Reuses pieces the full
    single-block card already has (CompactToneMetaRow, FormatBadge,
    useModelPicker) rather than re-deriving any of it. Deliberately stays
    read-only for anything that needs the full card's own richer state
    (favoriting, the Info panel's full description/tags) - EQ/Info/the
    thumbnail all hand off to `onOpenChild`, which recurses this exact same
    side into the ordinary full `ChainBlock` card (see the isDualMono
    branch below); Share is the one action simple enough to fire directly
    from here. */
const DualSideCard: React.FC<{
  dualBlockId: string;
  isLeftSide: boolean;
  child: ToneBlock | undefined;
  /** Dims Pan (inert while the enclosing lane has no spare channel to
      widen into - see runDualMono's own fold-vs-widen decision). */
  channelLimited: boolean;
  panValue: number;
  onPanChange: (val: number) => void;
  onPanDragStateChange: (dragging: boolean) => void;
  panHelp: string;
  /** Open this side in the full single-block editor - `null` for a plain
      open (thumbnail), 'eq'/'info' to land straight in that sub-view. */
  onOpenChild: (initial: 'eq' | 'info' | null) => void;
}> = ({
  dualBlockId,
  isLeftSide,
  child,
  channelLimited,
  panValue,
  onPanChange,
  onPanDragStateChange,
  panHelp,
  onOpenChild,
}) => {
  const actions = useChainActions();
  const toast = useToast();
  const boxSize = 80;

  const { modelOptions, modelsLoading, modelsTotal, handleModelSelect, handleModelsOpen } =
    useModelPicker(child ?? null);

  const knobDragRef = useRef(false);
  const handleKnobDragState = useCallback((dragging: boolean) => {
    knobDragRef.current = dragging;
  }, []);
  const [mix, setMix] = useState(child?.params.mix ?? 1.0);
  const [vol, setVol] = useState(child?.params.outputGain ?? 0.5);
  useEffect(() => {
    if (!knobDragRef.current) setMix(child?.params.mix ?? 1.0);
  }, [child?.params.mix]);
  useEffect(() => {
    if (!knobDragRef.current) setVol(child?.params.outputGain ?? 0.5);
  }, [child?.params.outputGain]);

  if (!child) {
    return (
      <div
        style={{ display: 'flex', flexDirection: 'column', alignItems: 'center', gap: '8rem' }}
      >
        <div
          style={{
            width: `${boxSize}rem`,
            height: `${boxSize}rem`,
            borderRadius: '12rem',
            border: BORDER,
            overflow: 'hidden',
            position: 'relative',
            flexShrink: 0,
          }}
        >
          <button
            type="button"
            onClick={() => actions.addToDualSlot(dualBlockId, isLeftSide)}
            {...helpProps(isLeftSide ? HELP.addDualSlotLeft : HELP.addDualSlotRight)}
            style={{
              width: '100%',
              height: '100%',
              display: 'flex',
              alignItems: 'center',
              justifyContent: 'center',
              background: 'transparent',
              border: 'none',
              cursor: 'pointer',
              color: GRAY,
            }}
          >
            <Plus size={24} />
          </button>
        </div>
        <span style={{ fontFamily: FONT_MONO, fontSize: '12rem', color: WHITE }}>
          {isLeftSide ? 'Left' : 'Right'}
        </span>
      </div>
    );
  }

  const { tone } = child;
  const isLocal = tone.local === true;
  const formatBadge = formatLabel(tone.format);
  const eqActive = (child.params.eq?.enabled ?? false) && !isEqFlat(child.params.eq);
  const handleShare = async () => {
    if (await actions.shareBlock(child)) toast.show('Link Copied');
  };

  return (
    <div
      style={{
        display: 'flex',
        flexDirection: 'column',
        gap: '12rem',
        width: '340rem',
        minWidth: 0,
      }}
    >
      <div style={{ display: 'flex', alignItems: 'center', justifyContent: 'flex-end', gap: '8rem' }}>
        <ChromeTextButton armed={eqActive} help={HELP.eqToggle} onClick={() => onOpenChild('eq')}>
          EQ
        </ChromeTextButton>
        {!isLocal && (
          <ChromeIconButton help={HELP.toneInfo} onClick={() => onOpenChild('info')}>
            <Info size={ICON_SIZE} />
          </ChromeIconButton>
        )}
        {!isLocal && (
          <ChromeIconButton help={HELP.shareTone} onClick={() => void handleShare()}>
            <Share size={ICON_SIZE} />
          </ChromeIconButton>
        )}
        <ChromeIconButton
          help={HELP.swapTone}
          onClick={() => actions.addToDualSlot(dualBlockId, isLeftSide)}
        >
          <ArrowLeftRight size={ICON_SIZE} />
        </ChromeIconButton>
        <ChromeIconButton
          help={HELP.removeBlock}
          onClick={() => actions.removeDualSlotContent(dualBlockId, isLeftSide)}
        >
          <Trash2 size={ICON_SIZE} />
        </ChromeIconButton>
      </div>

      <div style={{ display: 'flex', flexDirection: 'row', gap: '12rem', minWidth: 0 }}>
        <button
          type="button"
          onClick={() => onOpenChild(null)}
          {...helpProps(HELP.dualSideOpen)}
          style={{
            width: `${boxSize}rem`,
            height: `${boxSize}rem`,
            borderRadius: '12rem',
            border: BORDER,
            overflow: 'hidden',
            position: 'relative',
            flexShrink: 0,
            padding: 0,
            background: 'transparent',
            cursor: 'pointer',
          }}
        >
          <ToneImage
            src={tone.images?.[0]}
            alt={tone.title}
            gear={tone.gear}
            local={tone.local}
            blockType={child.blockType}
            boxSize={boxSize}
            draggable={false}
          />
        </button>
        <div
          style={{
            display: 'flex',
            flexDirection: 'column',
            gap: '6rem',
            minWidth: 0,
            flex: 1,
          }}
        >
          <span
            style={{
              fontFamily: FONT_MONO,
              fontSize: '14rem',
              color: WHITE,
              fontWeight: 700,
              whiteSpace: 'nowrap',
              overflow: 'hidden',
              textOverflow: 'ellipsis',
            }}
          >
            {tone.title}
          </span>
          <div style={{ display: 'flex', alignItems: 'center', gap: '10rem' }}>
            {tone.gear && (
              <span style={{ fontSize: '12rem', color: MUTED, fontWeight: 400 }}>
                {gearLabel(tone.gear)}
              </span>
            )}
            {formatBadge && <FormatBadge label={formatBadge} a2={child.blockType === 'nam'} />}
          </div>
          {!isLocal && (
            <CompactToneMetaRow
              tone={tone}
              favoritesCount={tone.favorites_count ?? 0}
              favorited={tone.is_favorite === true}
            />
          )}
        </div>
      </div>

      <div style={{ cursor: isLocal || actions.authenticated ? 'default' : 'not-allowed' }}>
        <ModelSelect
          options={modelOptions.map((m) => ({ id: String(m.id), name: m.name }))}
          value={String(child.activeModelId)}
          onChange={handleModelSelect}
          onOpen={handleModelsOpen}
          height={28}
          disabled={!isLocal && !actions.authenticated}
          loading={modelsLoading}
          totalCount={isLocal ? tone.models.length : modelsTotal}
        />
      </div>

      <div
        className={uiOffClass(channelLimited)}
        style={{ display: 'flex', alignItems: 'flex-end', justifyContent: 'center', gap: '16rem' }}
      >
        <KnobControl
          label="Pan"
          value={panValue}
          onChange={onPanChange}
          onDragStateChange={onPanDragStateChange}
          size={KNOB_SIZE_SECONDARY}
          labelBottom={false}
          thumb="secondary"
          scale={dualPanScale}
          defaultValue={isLeftSide ? 0.0 : 1.0}
          help={panHelp}
        />
        <KnobControl
          label="Mix"
          value={mix}
          onChange={(val) => {
            setMix(val);
            if (!knobDragRef.current) actions.setBlockParam(child.blockId, 'mix', val);
          }}
          onDragStateChange={(dragging) => {
            handleKnobDragState(dragging);
            if (!dragging) actions.setBlockParam(child.blockId, 'mix', mix);
          }}
          size={KNOB_SIZE_SECONDARY}
          labelBottom={false}
          thumb="secondary"
          defaultValue={1.0}
          help={HELP.blockMix}
        />
        <KnobControl
          label="Vol"
          value={vol}
          onChange={(val) => {
            setVol(val);
            if (!knobDragRef.current) actions.setBlockParam(child.blockId, 'outputGain', val);
          }}
          onDragStateChange={(dragging) => {
            handleKnobDragState(dragging);
            if (!dragging) actions.setBlockParam(child.blockId, 'outputGain', vol);
          }}
          size={KNOB_SIZE_SECONDARY}
          labelBottom={false}
          thumb="secondary"
          scale={gainDbScale}
          defaultValue={0.5}
          help={HELP.blockOut}
        />
      </div>
    </div>
  );
};

interface ChainBlockProps {
  block: ToneBlock;
  /** Another enabled+loaded NAM after this block in its lane. With input
      calibration on, such a block hands off at calibrated output level
      instead of normalizing (see the post-model gain stage in
      Processor.cpp); drives the normalize control's overridden state. */
  namDownstream: boolean;
  /** Host sample rate, for the EQ curve math. */
  sampleRate: number;
  /** Default NAM A2 size for new blocks (ChainState.namSlimSizeDefault);
      the read-only size chip only shows when this block differs from it. */
  namSlimSizeDefault: number;
  /** Return to the chain gallery (← BLOCK sits above the bordered card). */
  onBack: () => void;
  /** Chain-map strip (issue #83's replacement for the old Prev/Next
      chevrons): this block's whole lane, insert slots included, in chain
      order — mirrors GalleryLane's own ChainItem[] so the strip's slot
      order matches the real chain 1:1. See ChainMapStrip. */
  chainStripItems: ChainItem[];
  /** Jump the detail view straight to another block (any block in the lane,
      not just the adjacent one — see ChainMapStrip.onSelect). */
  onJumpToBlock: (blockId: string) => void;
  /** Add a block at a specific insert slot via the existing add-tone flow
      (same targeting GalleryLane's own "+" tiles use). See
      ChainMapStrip.onAdd. */
  onAddBlockAt: (insertBlockId: string) => void;
  /** Paste the copied block into a specific insert slot in this block's own
      lane (same canPaste/actions.pasteBlock(side, index) gating GalleryLane's
      own "+" tiles use, side already bound by ChainView). Null while
      there's nothing valid to paste. See ChainMapStrip.onPasteBlockAt. */
  onPasteBlockAt: ((index: number) => void) | null;
  /** Info view fills the center column to the faceplate (Select Tone pattern). */
  onFillToFaceplate?: (fill: boolean) => void;
  /** Show the EQ panel from the moment this card mounts - the gallery
      tile's own EQ shortcut button (GalleryBlock's onOpenEq) opens straight
      into it, the same destination ChainMapStrip's own EQ mark jumps to
      from inside an already-open block. Read only as showEq's *initial*
      state below: this component isn't remounted while jumping between
      blocks via ChainMapStrip (see that component's onSelectEq, which sets
      showEq directly instead), so this prop only ever matters for a fresh
      mount - i.e. opening from the gallery, where the card was unmounted a
      moment ago. */
  initialShowEq?: boolean;
  /** Same idea as `initialShowEq`, for the Info panel - the Dual Mono
      compact card's per-side Info icon opens straight into a side's full
      editor with this set (see DualSideCard below). Only matters at mount,
      same reasoning as `initialShowEq`. */
  initialShowInfo?: boolean;
  /** True when this render is a Dual Mono side's own full editor (recursed
      into by the isDualMono branch below, not reached through ChainView's
      own detailBlockId - see that branch's own comment). A dual child has
      no lane of its own to browse (each side is a fixed single socket, not
      an insert-slot-bearing lane), so ChainMapStrip's onAdd/onPasteBlockAt
      would be meaningless here; rather than invent lane semantics for a
      child, this just swaps the strip for a plain spacer so the header row
      stays balanced. */
  hideChainStrip?: boolean;
}

/** The detail card (full block view). All mutations come from the
    ChainActions context; only the block itself and the sample rate arrive
    as props. */
export const ChainBlock: React.FC<ChainBlockProps> = ({
  block,
  namDownstream,
  sampleRate,
  namSlimSizeDefault,
  onBack,
  chainStripItems,
  onJumpToBlock,
  onAddBlockAt,
  onPasteBlockAt,
  onFillToFaceplate,
  initialShowEq = false,
  initialShowInfo = false,
  hideChainStrip = false,
}) => {
  const { blockId, tone, params } = block;
  const actions = useChainActions();

  const isNam = tone.format?.toLowerCase() === 'nam';
  // Real ChainBlockType::CAB block (site tones tagged gear === "cab", or any
  // block converted via convertBlockType; see ToneBlock.blockType) -
  // structurally minimal natively (no predelay/envelope fields, single
  // convolver), so the sections built for those fields never render for one:
  // nothing back there to show. NOT the same as IrCategory === 'cab' (a
  // still-live classification on plain IR blocks); that one keeps the full
  // IR Player feature set, just with the -18dB pad and 100% mix default.
  const isCab = block.blockType === 'cab';
  // A standalone EQ block (ChainBlockType::EQ, see addEqBlock) - no model,
  // no tone content, nothing to swap or download; this block's own eq
  // *is* its entire content, processing everything that passes through at
  // a permanent 100% mix (there is no Mix control for it at all). Renders
  // its own dedicated, much simpler card below (see the early return right
  // before this component's main JSX) rather than threading a check
  // through every NAM/IR/CAB-specific section of that one.
  const isEq = block.blockType === 'eq';
  // A Dual Mono block (ChainBlockType::DUAL_MONO, see addDualMonoBlock) -
  // also no tone/model of its own; its two fixed child slots (dualLeft/
  // dualRight below) carry the real content instead. Same "own dedicated,
  // much simpler card" treatment as isEq, for the same reason (threading
  // this through the giant NAM/IR/CAB body would be worse than a clean
  // early return).
  const isDualMono = block.blockType === 'dualMono';

  // Optional (=) normalization toggle, revealed by Per-Block Normalization
  // in Plugin Settings.
  const showNormalizeControl = useBlockNormalizeControlEnabled();
  // Whether the header's NAM size chrome is the LITE/FULL toggle or the
  // read-only chip (the "choose per block" setting).
  const sizeControlEnabled = useBlockSizeControlEnabled();

  // Optimistic local values for the controls; native converges via polling.
  const [enabled, setEnabled] = useState(params.enabled);
  const [normalizeOn, setNormalizeOn] = useState(params.normalize ?? true);
  const [slimFull, setSlimFull] = useState(isSlimSizeFull(params.slimSize ?? SLIM_SIZE_LITE));
  const [irCategory, setIrCategory] = useState(block.irCategory);
  const [inputGain, setInputGain] = useState(params.inputGain ?? 0.5);
  const [outputGain, setOutputGain] = useState(params.outputGain ?? 0.5);
  const [mix, setMix] = useState(params.mix ?? 1.0);
  const [predelay, setPredelay] = useState(params.predelay ?? 0);
  const [initLevel, setInitLevel] = useState(params.initLevel ?? 1.0);
  const [attackLength, setAttackLength] = useState(params.attackLength ?? 0.0);
  const [attackCurve, setAttackCurve] = useState(params.attackCurve ?? 0.5);
  const [decayLength, setDecayLength] = useState(params.decayLength ?? 1.0);
  const [decayLevel, setDecayLevel] = useState(params.decayLevel ?? 1.0);
  const [decayCurve, setDecayCurve] = useState(params.decayCurve ?? 0.5);
  const [irSize, setIrSize] = useState(params.size ?? 0.5);
  // 0.75 (100%/full stereo) fallback, not the knob's 0.5 center - matches
  // native's ChainBlock::widthNormalized default (see its own comment).
  const [irWidth, setIrWidth] = useState(params.width ?? 0.75);
  // DUAL_MONO only: recombine knobs. Defaults match native's own
  // ChainBlock::dualLeftPanNormalized/dualRightPanNormalized/
  // dualWidthNormalized (hard-left/hard-right/full-width).
  const [dualLeftPan, setDualLeftPan] = useState(params.dualLeftPan ?? 0.0);
  const [dualRightPan, setDualRightPan] = useState(params.dualRightPan ?? 1.0);
  const [dualWidth, setDualWidth] = useState(params.dualWidth ?? 1.0);
  // DUAL_MONO only: which side (if any) is showing its own full editor -
  // see the isDualMono branch's own comment on why this stays local rather
  // than going through ChainView's detailBlockId.
  const [openChildSide, setOpenChildSide] = useState<'left' | 'right' | null>(null);
  const [openChildInitial, setOpenChildInitial] = useState<'eq' | 'info' | null>(null);
  const [trimInit, setTrimInit] = useState(params.trimInit ?? false);
  const [trimRelaxed, setTrimRelaxed] = useState(params.trimRelaxed ?? false);
  const [reverse, setReverse] = useState(params.reverse ?? false);
  const [showEq, setShowEq] = useState(initialShowEq);
  const [showInfo, setShowInfo] = useState(initialShowInfo);
  const [infoTone, setInfoTone] = useState<Tone | null>(null);
  const [infoLoading, setInfoLoading] = useState(false);
  const [infoError, setInfoError] = useState<string | null>(null);
  // Optimistic favorite while the PUT/DELETE is in flight; native's
  // is_favorite / favorites_count catch up via refreshToneMetadata.
  const [favoriteOverride, setFavoriteOverride] = useState<{
    on: boolean;
    count: number;
  } | null>(null);
  const favoriteBusyRef = useRef(false);
  const [eqView, setEqView] = useState<EqViewMode>('sliders');
  // Optimistic EQ power/position state (native converges via polling, like
  // `enabled`).
  const [eqOn, setEqOn] = useState(params.eq?.enabled ?? true);
  const [eqPre, setEqPre] = useState(params.eq?.pre ?? false);
  const toast = useToast();
  // True while one of this card's knobs is grabbed; knob prop syncs pause
  // so a stale chain snapshot can't fight the pointer (same pattern as
  // BlockEqView). On release the deferred revision bump resyncs everyone.
  const knobDragRef = useRef(false);
  const handleKnobDragState = useCallback((dragging: boolean) => {
    knobDragRef.current = dragging;
  }, []);

  // Params can change from outside (undo/redo, state restore, other editor
  // window); follow the backend when it reports a new value.
  useEffect(() => setEnabled(params.enabled), [params.enabled]);
  useEffect(() => setNormalizeOn(params.normalize ?? true), [params.normalize]);
  useEffect(
    () => setSlimFull(isSlimSizeFull(params.slimSize ?? SLIM_SIZE_LITE)),
    [params.slimSize]
  );
  useEffect(() => setIrCategory(block.irCategory), [block.irCategory]);
  useEffect(() => {
    if (!knobDragRef.current) setInputGain(params.inputGain ?? 0.5);
  }, [params.inputGain]);
  useEffect(() => {
    if (!knobDragRef.current) setOutputGain(params.outputGain ?? 0.5);
  }, [params.outputGain]);
  useEffect(() => {
    if (!knobDragRef.current) setMix(params.mix ?? 1.0);
  }, [params.mix]);
  useEffect(() => {
    if (!knobDragRef.current) setPredelay(params.predelay ?? 0);
  }, [params.predelay]);
  useEffect(() => {
    if (!knobDragRef.current) setInitLevel(params.initLevel ?? 1.0);
  }, [params.initLevel]);
  useEffect(() => {
    if (!knobDragRef.current) setAttackLength(params.attackLength ?? 0.0);
  }, [params.attackLength]);
  useEffect(() => {
    if (!knobDragRef.current) setAttackCurve(params.attackCurve ?? 0.5);
  }, [params.attackCurve]);
  useEffect(() => {
    if (!knobDragRef.current) setDecayLength(params.decayLength ?? 1.0);
  }, [params.decayLength]);
  useEffect(() => {
    if (!knobDragRef.current) setDecayLevel(params.decayLevel ?? 1.0);
  }, [params.decayLevel]);
  useEffect(() => {
    if (!knobDragRef.current) setDecayCurve(params.decayCurve ?? 0.5);
  }, [params.decayCurve]);
  useEffect(() => {
    if (!knobDragRef.current) setIrSize(params.size ?? 0.5);
  }, [params.size]);
  useEffect(() => {
    if (!knobDragRef.current) setIrWidth(params.width ?? 0.75);
  }, [params.width]);
  useEffect(() => {
    if (!knobDragRef.current) setDualLeftPan(params.dualLeftPan ?? 0.0);
  }, [params.dualLeftPan]);
  useEffect(() => {
    if (!knobDragRef.current) setDualRightPan(params.dualRightPan ?? 1.0);
  }, [params.dualRightPan]);
  useEffect(() => {
    if (!knobDragRef.current) setDualWidth(params.dualWidth ?? 1.0);
  }, [params.dualWidth]);
  useEffect(() => setTrimInit(params.trimInit ?? false), [params.trimInit]);
  useEffect(() => setTrimRelaxed(params.trimRelaxed ?? false), [params.trimRelaxed]);
  useEffect(() => setReverse(params.reverse ?? false), [params.reverse]);
  useEffect(() => setEqOn(params.eq?.enabled ?? true), [params.eq?.enabled]);
  useEffect(() => setEqPre(params.eq?.pre ?? false), [params.eq?.pre]);

  const setParam = useCallback(
    (param: BlockParamName, value: number | boolean) =>
      actions.setBlockParam(blockId, param, value),
    [actions, blockId]
  );

  // Size/Width rebuild the convolver off-thread (not real-time smoothers),
  // so - unlike setParam's continuous params - they're committed once per
  // gesture rather than at drag rate. Unlike the envelope row's idle-
  // debounce (commitEnvelope above), these commit on the actual drag-end
  // signal: knobDragRef/handleKnobDragState already exists to pause the
  // backend->local sync mid-drag, so it doubles as the release gate here.
  // Refs track the latest optimistic value so the commit on release always
  // sends what's on screen, not a stale closure. Alt/Option-click and
  // double-tap reset (KnobControl) call onChange without ever toggling drag
  // state, so they're covered separately: onChange commits immediately
  // whenever knobDragRef isn't currently held (a real drag holds it, a
  // reset/text-edit doesn't).
  const irSizeRef = useRef(irSize);
  irSizeRef.current = irSize;
  const handleIrSizeChange = useCallback(
    (val: number) => {
      setIrSize(val);
      irSizeRef.current = val;
      if (!knobDragRef.current) actions.setBlockIrSize(blockId, val);
    },
    [actions, blockId]
  );
  const handleIrSizeDragState = useCallback(
    (dragging: boolean) => {
      handleKnobDragState(dragging);
      if (!dragging) actions.setBlockIrSize(blockId, irSizeRef.current);
    },
    [handleKnobDragState, actions, blockId]
  );

  const irWidthRef = useRef(irWidth);
  irWidthRef.current = irWidth;
  const handleIrWidthChange = useCallback(
    (val: number) => {
      setIrWidth(val);
      irWidthRef.current = val;
      if (!knobDragRef.current) actions.setBlockIrWidth(blockId, val);
    },
    [actions, blockId]
  );
  const handleIrWidthDragState = useCallback(
    (dragging: boolean) => {
      handleKnobDragState(dragging);
      if (!dragging) actions.setBlockIrWidth(blockId, irWidthRef.current);
    },
    [handleKnobDragState, actions, blockId]
  );

  // Cycles Off -> Std -> Lax -> Off. Both fields commit together in the one
  // setBlockIrTrimInit call for every step (including Lax -> Off, which
  // must also clear trimRelaxed so the next Off -> on lands on Std again,
  // not stale-resumes on Lax) - one native call per click, so a click is
  // always exactly one undo step, same shape as handleToggleEqEnabled above.
  const handleCycleTrimMode = useCallback(() => {
    const nextEnabled = !trimInit || !trimRelaxed;
    const nextRelaxed = trimInit && !trimRelaxed;
    setTrimInit(nextEnabled);
    setTrimRelaxed(nextRelaxed);
    actions.setBlockIrTrimInit(blockId, nextEnabled, nextRelaxed);
  }, [actions, blockId, trimInit, trimRelaxed]);

  const handleToggleReverse = useCallback(() => {
    setReverse((prev) => {
      actions.setBlockIrReverse(blockId, !prev);
      return !prev;
    });
  }, [actions, blockId]);

  // DUAL_MONO only: each knob sends the full current triple - setDualImage
  // takes all three together (native re-derives the recombine from them
  // every call), same shape as setBlockIrDecay's own multi-value commit.
  const handleDualLeftPanChange = useCallback(
    (val: number) => {
      setDualLeftPan(val);
      actions.setDualImage(blockId, val, dualRightPan, dualWidth);
    },
    [actions, blockId, dualRightPan, dualWidth]
  );
  const handleDualRightPanChange = useCallback(
    (val: number) => {
      setDualRightPan(val);
      actions.setDualImage(blockId, dualLeftPan, val, dualWidth);
    },
    [actions, blockId, dualLeftPan, dualWidth]
  );
  const handleDualWidthChange = useCallback(
    (val: number) => {
      setDualWidth(val);
      actions.setDualImage(blockId, dualLeftPan, dualRightPan, val);
    },
    [actions, blockId, dualLeftPan, dualRightPan]
  );

  // Resets every IR shaping field to default in one native call (a single
  // undo step - see resetBlockIrShape's own doc comment), and mirrors that
  // locally so every knob/chip/toggle this card renders snaps back
  // immediately rather than waiting on the next chain-state poll.
  const handleResetIrShape = useCallback(() => {
    setInitLevel(1.0);
    setAttackLength(0.0);
    setAttackCurve(0.5);
    setDecayLength(1.0);
    setDecayLevel(1.0);
    setDecayCurve(0.5);
    setIrSize(0.5);
    setIrWidth(0.75);
    setTrimInit(false);
    setTrimRelaxed(false);
    setReverse(false);
    actions.resetBlockIrShape(blockId);
  }, [actions, blockId]);

  // Sole source of truth for whether the loaded IR has a real stereo image
  // (see ToneBlock.irNumChannels) - Width locks to mono/disabled without it.
  const isMonoIr = (block.irNumChannels ?? 1) <= 1;

  // The envelope rebuilds the convolver off-thread (not a real-time
  // smoother like setBlockParam's continuous params), so it isn't safe to
  // fire at knob-drag rates - idle-debounced instead: the knobs and the
  // waveform overlay (both driven by local state) update live on every drag
  // frame, but the native rebuild only fires once movement pauses. All six
  // values travel together in one call (like setBlockEqBand's whole-band
  // updates), so each control's onChange passes its own new value plus the
  // other five's current local state - a drag on one can't clobber
  // another's in-flight value.
  const envelopeCommitTimerRef = useRef<ReturnType<typeof setTimeout> | null>(null);
  useEffect(
    () => () => {
      if (envelopeCommitTimerRef.current) clearTimeout(envelopeCommitTimerRef.current);
    },
    []
  );
  const commitEnvelope = useCallback(
    (
      nextInitLevel: number,
      nextAttackLength: number,
      nextAttackCurve: number,
      nextDecayLength: number,
      nextDecayLevel: number,
      nextDecayCurve: number
    ) => {
      if (envelopeCommitTimerRef.current) clearTimeout(envelopeCommitTimerRef.current);
      envelopeCommitTimerRef.current = setTimeout(() => {
        envelopeCommitTimerRef.current = null;
        actions.setBlockIrDecay(
          blockId,
          nextInitLevel,
          nextAttackLength,
          nextAttackCurve,
          nextDecayLength,
          nextDecayLevel,
          nextDecayCurve
        );
      }, 400);
    },
    [actions, blockId]
  );

  // Graph-driven envelope edit (IrEnvelopeGraph): a patch carries only the
  // axis/axes the dragged point moves (End moves two at once), merged over
  // the current local values so a diagonal End drag commits as one call
  // instead of two racing debounced ones.
  const updateEnvelope = useCallback(
    (patch: EnvelopePatch) => {
      const next = {
        initLevel: patch.initLevel ?? initLevel,
        attackLength: patch.attackLength ?? attackLength,
        attackCurve: patch.attackCurve ?? attackCurve,
        decayLength: patch.decayLength ?? decayLength,
        decayLevel: patch.decayLevel ?? decayLevel,
        decayCurve: patch.decayCurve ?? decayCurve,
      };
      if (patch.initLevel !== undefined) setInitLevel(patch.initLevel);
      if (patch.attackLength !== undefined) setAttackLength(patch.attackLength);
      if (patch.attackCurve !== undefined) setAttackCurve(patch.attackCurve);
      if (patch.decayLength !== undefined) setDecayLength(patch.decayLength);
      if (patch.decayLevel !== undefined) setDecayLevel(patch.decayLevel);
      if (patch.decayCurve !== undefined) setDecayCurve(patch.decayCurve);
      commitEnvelope(
        next.initLevel,
        next.attackLength,
        next.attackCurve,
        next.decayLength,
        next.decayLevel,
        next.decayCurve
      );
    },
    [initLevel, attackLength, attackCurve, decayLength, decayLevel, decayCurve, commitEnvelope]
  );

  // EditableChip commit for one envelope field: parses the typed display
  // value (ms / % / curve units, matching that chip's own KnobScale) back to
  // normalized 0..1 and routes it through the same updateEnvelope merge the
  // graph uses, so a chip edit and a graph drag can never race each other.
  const commitEnvelopeField = useCallback(
    (key: keyof EnvelopePatch, scale: KnobScale, raw: string) => {
      const parsed = Number.parseFloat(raw.replace(',', '.'));
      if (!Number.isFinite(parsed)) return;
      const normalized = Math.min(Math.max(scale.fromDisplay(parsed), 0), 1);
      updateEnvelope({ [key]: normalized });
    },
    [updateEnvelope]
  );

  const handleToggleEnabled = useCallback(() => {
    setEnabled((prev) => {
      setParam('enabled', !prev);
      return !prev;
    });
  }, [setParam]);

  const handleToggleNormalize = useCallback(() => {
    setNormalizeOn((prev) => {
      setParam('normalize', !prev);
      return !prev;
    });
  }, [setParam]);

  const handleSetSlimFull = useCallback(
    (full: boolean) => {
      setSlimFull(full);
      actions.setBlockSlimSize(blockId, full ? SLIM_SIZE_FULL : SLIM_SIZE_LITE);
    },
    [actions, blockId]
  );

  const handleSetIrCategory = useCallback(
    (category: 'cab' | 'irPlayer') => {
      setIrCategory(category);
      // Mirrors the native reset (setBlockIrCategory) so Mix doesn't lag a
      // poll behind; the mix param effect above picks up the converged
      // value once native's own resync lands.
      setMix(category === 'cab' ? 1 : 0.25);
      actions.setBlockIrCategory(blockId, category);
    },
    [actions, blockId]
  );

  // The header's "Cab / IR Player" control does double duty. Off a real CAB
  // block (isCab), or onto one, it's a genuine block-type conversion (see
  // convertBlockType): the loaded sample carries over, IR -> CAB applying
  // the same 500ms truncation/-18dB pad a site-loaded Cab tone gets, CAB ->
  // IR restoring the full original sample. No local optimistic state to set
  // here - blockType flips only once native's chain-state resync lands (the
  // block stays `loaded` throughout, so it keeps playing under the existing
  // loading-dots affordance meanwhile), and the card re-renders as
  // isCab/!isCab in place - nothing to navigate to, it's the same block.
  // Off a plain IR block that's merely flagged IrCategory::Cab (legacy
  // state, or a local drop guessed as cab-length - never a real CAB block),
  // clicking "IR Player" is the lighter existing reclassification instead:
  // there is no real type to leave.
  const handleCabCategoryClick = useCallback(
    (option: 'cab' | 'irPlayer') => {
      const activeOption = isCab ? 'cab' : irCategory;
      if (option === activeOption) return;
      if (isCab || option === 'cab')
        actions.convertBlockType(blockId, option === 'cab' ? 'cab' : 'ir');
      else handleSetIrCategory('irPlayer');
    },
    [actions, blockId, isCab, irCategory, handleSetIrCategory]
  );

  const handleToggleEqEnabled = useCallback(() => {
    setEqOn((prev) => {
      actions.setBlockEqEnabled(blockId, !prev);
      return !prev;
    });
  }, [actions, blockId]);

  const handleToggleEqPre = useCallback(() => {
    setEqPre((prev) => {
      actions.setBlockEqPre(blockId, !prev);
      return !prev;
    });
  }, [actions, blockId]);

  const handleResetEq = useCallback(() => {
    actions.resetBlockEq(blockId);
  }, [actions, blockId]);

  const handleShare = useCallback(async () => {
    if (await actions.shareBlock(block)) toast.show('Link Copied');
  }, [actions, block, toast]);

  // A drop-loaded local file (or folder of them): no catalog behind it, so
  // the card keeps the sound controls and drops the catalog chrome (share,
  // counts, info). The models are the dropped files themselves; the picker feeds
  // off the tone's own model list.
  const isLocal = tone.local === true;

  // Full catalog metadata: fetched from the TONE3000 API, never written into
  // saved chain/preset state by the UI. One fetch serves two purposes: it
  // pre-warms the info panel (infoTone) and re-hydrates native's stored tone
  // metadata (refreshToneMetadata merges it in and the chainChanged resync
  // updates every view). `background` (the on-expand sync) never touches the
  // loading/error UI, so offline or signed-out use is undisturbed; the
  // foreground path (info panel open) keeps its spinner and retry UI. The
  // seq ref is a stale guard (the models effect's flag, as a counter since
  // retries reuse this fetch): only the newest request may touch state, so a
  // swap mid-flight can't surface the old tone's info.
  const infoFetchSeq = useRef(0);
  const fetchInfo = useCallback(
    async (toneId: number, background = false) => {
      if (!actions.authenticated) return;
      const seq = ++infoFetchSeq.current;
      if (!background) {
        setInfoLoading(true);
        setInfoError(null);
      }
      try {
        const full = await actions.getTone(toneId);
        if (seq !== infoFetchSeq.current) return;
        // A favorite toggle in flight owns the next metadata write.
        if (favoriteBusyRef.current) return;
        setInfoTone(full);
        // Best-effort: native no-ops when nothing changed server-side.
        actions.refreshToneMetadata(JSON.stringify(full));
      } catch (err) {
        if (background) {
          // Silent by design (offline, API down, tone deleted): the cached
          // tone keeps working, and opening the info panel refetches with
          // its own visible error/retry UI.
          console.debug('Tone metadata sync skipped', err);
          return;
        }
        console.error('Failed to load tone info', err);
        if (seq !== infoFetchSeq.current) return;
        setInfoTone(null);
        setInfoError('Failed to load tone details.');
      } finally {
        if (!background && seq === infoFetchSeq.current) setInfoLoading(false);
      }
    },
    [actions]
  );

  const favorited = favoriteOverride?.on ?? infoTone?.is_favorite ?? tone.is_favorite === true;
  const favoritesCount =
    favoriteOverride?.count ?? infoTone?.favorites_count ?? tone.favorites_count ?? 0;

  const handleToggleFavorite = useCallback(async () => {
    if (!actions.authenticated || favoriteBusyRef.current) return;
    const next = !favorited;
    const nextCount = Math.max(0, favoritesCount + (next ? 1 : -1));
    setFavoriteOverride({ on: next, count: nextCount });
    favoriteBusyRef.current = true;
    try {
      await actions.setToneFavorite(tone.id, next);
      const base = infoTone?.id === tone.id ? infoTone : await actions.getTone(tone.id);
      const patched = { ...base, is_favorite: next, favorites_count: nextCount };
      setInfoTone(patched);
      actions.refreshToneMetadata(JSON.stringify(patched));
    } catch (err) {
      console.error('Failed to update favorite', err);
      setFavoriteOverride(null);
    } finally {
      favoriteBusyRef.current = false;
    }
  }, [actions, favorited, favoritesCount, infoTone, tone.id]);

  const handleToggleInfo = useCallback(() => {
    if (showInfo) {
      setShowInfo(false);
      return;
    }
    setShowEq(false);
    setShowInfo(true);
    if (actions.authenticated && infoTone?.id !== tone.id) void fetchInfo(tone.id);
  }, [actions.authenticated, fetchInfo, infoTone?.id, showInfo, tone.id]);

  // Expand and swap both land here (mount / tone identity change): orphan
  // any in-flight fetch, drop the stale payload, then fetch the latest tone
  // in the background (metadata re-sync + info pre-warm). With the info
  // panel already open (swap from the detail view) the fetch runs foreground
  // so its loading/error UI behaves as before. Local tones have no catalog
  // to sync from.
  useEffect(() => {
    infoFetchSeq.current++;
    setInfoTone(null);
    setInfoError(null);
    setInfoLoading(false);
    setFavoriteOverride(null);
    if (!isLocal && actions.authenticated) void fetchInfo(tone.id, !showInfo);
    // Only the tone identity; opening/closing the panel is handleToggleInfo.
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [tone.id]);

  // Select Tone pattern: drop the meter-band bottom pad while info is open
  // so the card can scroll to the faceplate; restore it on close/unmount.
  useEffect(() => {
    onFillToFaceplate?.(showInfo);
    return () => onFillToFaceplate?.(false);
  }, [showInfo, onFillToFaceplate]);

  // Native persists only the block's *active* model; the full catalog (tones
  // max out at 300 models) is fetched client-side in one call per tone.
  // Signed out the picker is disabled (and the API needs the token anyway).
  const { modelOptions, modelsLoading, modelsTotal, handleModelSelect, handleModelsOpen } =
    useModelPicker(block);

  // A model download/prepare is in flight (switch, swap or first load). The
  // previous model keeps playing during a switch (`loaded` stays true), so
  // loading affordances key off `modelLoading`, not `loaded`.
  const modelBusy = block.modelLoading || (!block.loaded && !block.loadFailed);

  // POC: static waveform display replaces the artwork image for IR blocks
  // only (see WaveformDisplay.tsx). `!isNam` in the gate skips the native
  // fetch entirely for NAM blocks; falls back to the image while unfetched
  // (fresh mount, still loading). activeModelId re-triggers the fetch on a
  // dropdown/arrow model switch; modelLoading closes a real race on top of
  // that (see useIrWaveform's doc comment).
  const irWaveform = useIrWaveform(
    blockId,
    !isNam && !isCab && block.loaded,
    block.activeModelId,
    block.modelLoading
  );
  // Decay Length is the TOTAL truncated length (the real "End" position) -
  // normalized against *this* block's own detected content, same convention
  // the old standalone Length knob always used - see knobScale.ts's
  // lengthMsScale. Attack Length is a fraction *of that total*, not an
  // independent length, so its own scale's max is Decay Length's current
  // real-ms value (shrinks/grows live as Decay Length moves).
  const decayLengthScale = useMemo(
    () => lengthMsScale(block.irContentLengthMs),
    [block.irContentLengthMs]
  );
  // Size's displayed % must reflect the same kIrSizeMaxEffectiveSeconds
  // clamp the engine actually applies (see sizePercentScale in
  // knobScale.ts) - otherwise a long source's knob would read "1000%" past
  // the point where the audible effect is already capped. Deliberately
  // irRawContentLengthMs, not irContentLengthMs: the latter is already
  // scaled by Size's own clamped ratio, so using it here would be circular
  // (the knob would read stuck at 100% forever once it ever clamped once).
  const irSizeScale = useMemo(
    () => sizePercentScale(block.irRawContentLengthMs),
    [block.irRawContentLengthMs]
  );
  const attackLengthScale = useMemo(
    () => attackLengthMsScale(decayLengthScale.toDisplay(decayLength)),
    [decayLengthScale, decayLength]
  );
  // Derived for the waveform overlay: cutFraction is where the truncated
  // content ends - Decay Length IS that fraction of the full content
  // directly. attackFraction is where the Attack/Decay boundary sits
  // *within* that truncated window (0..1) - Attack Length IS that fraction
  // directly, matching envelopeDbAt's own fraction convention.
  const cutFraction = decayLength;
  const attackFractionWithinWindow = attackLength;

  // Calibration state (the gauge indicator + the normalize override). Only
  // meaningful while the user's input calibration setting is on: the gauge
  // then reads white when the loaded model carries calibration data and gray
  // when it doesn't. The overridden check mirrors the DSP's calibrated
  // hand-off condition exactly (Processor.cpp): calibration on, sane
  // output_level_dbu metadata, and another NAM downstream. The last NAM
  // stays on normalization, so its control never reads overridden.
  const [calibrateInput] = useParameter('calibrateInput', 'toggle');
  const showCalibration = isNam && calibrateInput;
  const calibrationActive = block.inputLevelDbu !== undefined;
  const handOffLevelSane =
    block.outputLevelDbu !== undefined && block.outputLevelDbu >= -60 && block.outputLevelDbu <= 60;
  const normalizeOverridden = isNam && calibrateInput && namDownstream && handOffLevelSane;
  // Cab loads full wet by default, IrPlayer 25% wet (native sets the mix on
  // first load from the block's IR category); Alt-click reset on Mix must
  // agree.
  const defaultMix = isCab || block.irCategory === 'cab' ? 1 : 0.25;
  // Every NAM block in the chain is A2 (the browser filters the catalog and
  // local drops are validated), so NAM badges always carry the A2 mark.
  const formatBadge = formatLabel(tone.format);

  // EQ is shaping this block's audio: powered on and not flat (a flat or
  // bypassed EQ is skipped natively). Uses the optimistic power state so the
  // header glow reacts to the toggle immediately.
  const eqActive = eqOn && params.eq ? !isEqFlat(params.eq) : false;

  const tonePageUrl = infoTone?.url || tone.url || `${T3K_API}/tones/${tone.id}`;

  // A standalone EQ block (isEq) renders a completely separate, much
  // simpler card - no model/tone chrome to thread `isEq` checks through
  // across the rest of this component's giant NAM/IR/CAB-oriented return
  // below. Reached only after every hook above has already run (this
  // component isn't remounted while navigating between blocks via
  // ChainMapStrip - see initialShowEq's own comment - so `isEq` can change
  // between renders of the *same* instance, and conditionally skipping
  // hooks based on it would violate the rules of hooks). Duplicates the
  // small ← BLOCK/ChainMapStrip header every card shares; everything below
  // that is unique to this block type: Power, FLAT reset, the graph/sliders
  // view switcher (the exact same BlockEqView every other block's EQ panel
  // uses, just always shown instead of behind a toggle - there is nothing
  // else in this block to toggle back to), and a single Output knob (no
  // In, no Mix - see ChainBlockType::EQ's own comment for why neither
  // exists here).
  if (isEq) {
    return (
      <div
        style={{
          display: 'flex',
          flexDirection: 'column',
          width: `${CARD_WIDTH}rem`,
          height: '100%',
          boxSizing: 'border-box',
          overflowY: 'hidden',
          overflowX: 'hidden',
        }}
      >
        <div style={{ display: 'flex', flexDirection: 'column', alignItems: 'stretch' }}>
          <div
            style={{
              display: 'flex',
              flexDirection: 'row',
              alignItems: 'center',
              gap: '16rem',
              marginBottom: '16rem',
              flexShrink: 0,
            }}
          >
            <button
              type="button"
              onClick={onBack}
              {...helpProps(HELP.backToChain)}
              style={{
                display: 'flex',
                alignItems: 'center',
                gap: '16rem',
                flexShrink: 0,
                background: 'transparent',
                border: 'none',
                outline: 'none',
                padding: 0,
                cursor: 'pointer',
                color: WHITE,
              }}
            >
              <ArrowLeft size={16} style={{ display: 'block', flexShrink: 0 }} />
              <span
                style={{
                  fontFamily: FONT_MONO,
                  fontSize: '16rem',
                  fontWeight: 400,
                  textTransform: 'uppercase',
                  lineHeight: 1.4,
                }}
              >
                Block
              </span>
            </button>

            <ChainMapStrip
              items={chainStripItems}
              currentBlockId={blockId}
              onSelect={(id) => {
                onJumpToBlock(id);
                setShowEq(false);
              }}
              onSelectEq={(id) => {
                onJumpToBlock(id);
                setShowEq(true);
                setShowInfo(false);
              }}
              onAdd={onAddBlockAt}
              onPasteBlockAt={onPasteBlockAt}
            />

            <div
              aria-hidden
              style={{
                display: 'flex',
                alignItems: 'center',
                gap: '16rem',
                flexShrink: 0,
                visibility: 'hidden',
                pointerEvents: 'none',
              }}
            >
              <ArrowLeft size={16} style={{ display: 'block', flexShrink: 0 }} />
              <span
                style={{
                  fontFamily: FONT_MONO,
                  fontSize: '16rem',
                  fontWeight: 400,
                  textTransform: 'uppercase',
                  lineHeight: 1.4,
                }}
              >
                Block
              </span>
            </div>
          </div>

          <div
            style={{
              display: 'flex',
              flexDirection: 'column',
              width: '100%',
              height: `${CARD_HEIGHT}rem`,
              minHeight: `${CARD_HEIGHT}rem`,
              boxSizing: 'border-box',
              border: BORDER,
              borderRadius: `${CARD_RADIUS}rem`,
              overflow: 'hidden',
            }}
          >
            <div
              style={{
                height: `${HEADER_HEIGHT}rem`,
                flexShrink: 0,
                display: 'flex',
                alignItems: 'center',
                justifyContent: 'space-between',
                padding: `0 ${BODY_PADDING}rem`,
                boxSizing: 'border-box',
                borderBottom: BORDER,
              }}
            >
              <div style={{ display: 'flex', alignItems: 'center', gap: '16rem', flexShrink: 0 }}>
                <ChromeIconButton
                  tone="power"
                  on={enabled}
                  help={HELP.blockPower}
                  onClick={handleToggleEnabled}
                >
                  <Power />
                </ChromeIconButton>
                <span
                  style={{
                    fontFamily: FONT_MONO,
                    fontSize: '16rem',
                    fontWeight: 400,
                    color: WHITE,
                  }}
                >
                  EQ
                </span>
              </div>
              <div style={{ display: 'flex', alignItems: 'center', gap: '16rem', flexShrink: 0 }}>
                <ChromeTextButton
                  armed={false}
                  help="Reset all EQ bands to default (flat)"
                  onClick={handleResetEq}
                >
                  FLAT
                </ChromeTextButton>
                <div
                  style={{
                    ...segmentedGroupStyle(),
                    backgroundColor: 'rgba(118, 118, 128, 0.24)',
                  }}
                >
                  <button
                    onClick={() => setEqView('sliders')}
                    {...helpProps(HELP.eqSlidersView)}
                    style={{
                      ...segmentedCellStyle(true),
                      color: eqView === 'sliders' ? WHITE : GRAY,
                    }}
                  >
                    <EqSlidersIcon />
                  </button>
                  <button
                    onClick={() => setEqView('graph')}
                    {...helpProps(HELP.eqCurveView)}
                    style={{
                      ...segmentedCellStyle(true),
                      color: eqView === 'graph' ? WHITE : GRAY,
                    }}
                  >
                    <EqCurveIcon />
                  </button>
                </div>
                <ChromeIconButton
                  help={HELP.removeBlock}
                  onClick={() => actions.removeBlock(blockId)}
                >
                  <Trash2 />
                </ChromeIconButton>
              </div>
            </div>

            <div
              className={uiOffClass(!enabled)}
              style={{
                height: `${BODY_HEIGHT}rem`,
                flexShrink: 0,
                display: 'flex',
                flexDirection: 'row',
                alignItems: 'stretch',
                transition: 'opacity 0.2s ease',
              }}
            >
              <BlockEqView
                blockId={blockId}
                bands={params.eq?.bands ?? []}
                eqEnabled
                sampleRate={sampleRate}
                view={eqView}
                onSetBand={actions.setBlockEqBand}
              />
              <div
                style={{
                  display: 'flex',
                  flexDirection: 'column',
                  alignItems: 'center',
                  justifyContent: 'flex-end',
                  flexShrink: 0,
                  gap: '12rem',
                  padding: `${BODY_PADDING}rem`,
                }}
              >
                <div
                  style={{
                    flex: 1,
                    display: 'flex',
                    alignItems: 'center',
                    justifyContent: 'center',
                    minHeight: 0,
                    width: `${KNOB_SIZE_SECONDARY}rem`,
                  }}
                >
                  <BlockMeter meterId={meterId.blockOut(blockId)} length={RAIL_METER_HEIGHT} />
                </div>
                <KnobControl
                  label="Out"
                  value={outputGain}
                  onChange={(val) => {
                    setOutputGain(val);
                    setParam('outputGain', val);
                  }}
                  onDragStateChange={handleKnobDragState}
                  size={KNOB_SIZE_SECONDARY}
                  labelBottom={false}
                  thumb="secondary"
                  scale={gainDbScale}
                  defaultValue={0.5}
                  help={HELP.blockOut}
                />
              </div>
            </div>
          </div>
        </div>
      </div>
    );
  }

  if (isDualMono) {
    const rawLeftChild = block.dualLeft?.[0];
    const dualLeft = rawLeftChild && !isInsertSlot(rawLeftChild) ? rawLeftChild : undefined;
    const rawRightChild = block.dualRight?.[0];
    const dualRight = rawRightChild && !isInsertSlot(rawRightChild) ? rawRightChild : undefined;

    // Recurse into the exact same ChainBlock card every ordinary NAM/IR/CAB
    // block uses, for whichever side is open - "Navigate", not a second
    // rendering path (see this session's design discussion, recorded in
    // dual_mono_ux_punchlist.md). Local state, not ChainView's own
    // detailBlockId: a dual child is never reachable through the top-level
    // chain/chainRight arrays ChainView resolves detail views from, so
    // threading it through there would need ChainView to understand
    // nesting it doesn't today (and its awaitingDetailBlock guard would
    // render blank forever for an id it can never confirm - see the
    // research this plan was built on). onBack here only clears this local
    // state - it never touches ChainView, sessionStorage, or gallery
    // scroll-restore, because we never left this Dual Mono block.
    if (openChildSide != null) {
      const childToOpen = openChildSide === 'left' ? dualLeft : dualRight;
      if (childToOpen) {
        return (
          <ChainBlock
            block={childToOpen}
            namDownstream={false}
            sampleRate={sampleRate}
            namSlimSizeDefault={namSlimSizeDefault}
            initialShowEq={openChildInitial === 'eq'}
            initialShowInfo={openChildInitial === 'info'}
            hideChainStrip
            onBack={() => {
              setOpenChildSide(null);
              setOpenChildInitial(null);
            }}
            chainStripItems={[]}
            onJumpToBlock={() => {}}
            onAddBlockAt={() => {}}
            onPasteBlockAt={null}
          />
        );
      }
      // The side emptied out from under us (e.g. Trash from the other
      // side's own detail view is impossible, but undo/redo or a state
      // restore could still do it) - fall back to the compact card instead
      // of rendering nothing.
    }

    const openChild = (side: 'left' | 'right', initial: 'eq' | 'info' | null) => {
      setOpenChildSide(side);
      setOpenChildInitial(initial);
    };

    return (
      <div
        style={{
          display: 'flex',
          flexDirection: 'column',
          width: `${CARD_WIDTH}rem`,
          height: '100%',
          boxSizing: 'border-box',
          overflowY: 'hidden',
          overflowX: 'hidden',
        }}
      >
        <div style={{ display: 'flex', flexDirection: 'column', alignItems: 'stretch' }}>
          <div
            style={{
              display: 'flex',
              flexDirection: 'row',
              alignItems: 'center',
              gap: '16rem',
              marginBottom: '16rem',
              flexShrink: 0,
            }}
          >
            <button
              type="button"
              onClick={onBack}
              {...helpProps(HELP.backToChain)}
              style={{
                display: 'flex',
                alignItems: 'center',
                gap: '16rem',
                flexShrink: 0,
                background: 'transparent',
                border: 'none',
                outline: 'none',
                padding: 0,
                cursor: 'pointer',
                color: WHITE,
              }}
            >
              <ArrowLeft size={16} style={{ display: 'block', flexShrink: 0 }} />
              <span
                style={{
                  fontFamily: FONT_MONO,
                  fontSize: '16rem',
                  fontWeight: 400,
                  textTransform: 'uppercase',
                  lineHeight: 1.4,
                }}
              >
                Block
              </span>
            </button>

            <ChainMapStrip
              items={chainStripItems}
              currentBlockId={blockId}
              onSelect={(id) => {
                onJumpToBlock(id);
                setShowEq(false);
              }}
              onSelectEq={(id) => {
                onJumpToBlock(id);
                setShowEq(true);
                setShowInfo(false);
              }}
              onAdd={onAddBlockAt}
              onPasteBlockAt={onPasteBlockAt}
            />

            <div
              aria-hidden
              style={{
                display: 'flex',
                alignItems: 'center',
                gap: '16rem',
                flexShrink: 0,
                visibility: 'hidden',
                pointerEvents: 'none',
              }}
            >
              <ArrowLeft size={16} style={{ display: 'block', flexShrink: 0 }} />
              <span
                style={{
                  fontFamily: FONT_MONO,
                  fontSize: '16rem',
                  fontWeight: 400,
                  textTransform: 'uppercase',
                  lineHeight: 1.4,
                }}
              >
                Block
              </span>
            </div>
          </div>

          <div
            style={{
              display: 'flex',
              flexDirection: 'column',
              width: '100%',
              // Auto, not the shared CARD_HEIGHT: this card's content (two
              // per-side compact cards) is taller than the plain 3-knob row
              // it replaces - same reasoning showInfo already uses to drop
              // the fixed height elsewhere in this component.
              minHeight: `${CARD_HEIGHT}rem`,
              boxSizing: 'border-box',
              border: BORDER,
              borderRadius: `${CARD_RADIUS}rem`,
              overflow: 'hidden',
            }}
          >
            <div
              style={{
                height: `${HEADER_HEIGHT}rem`,
                flexShrink: 0,
                display: 'flex',
                alignItems: 'center',
                justifyContent: 'space-between',
                padding: `0 ${BODY_PADDING}rem`,
                boxSizing: 'border-box',
                borderBottom: BORDER,
              }}
            >
              <div style={{ display: 'flex', alignItems: 'center', gap: '16rem', flexShrink: 0 }}>
                <ChromeIconButton
                  tone="power"
                  on={enabled}
                  help={HELP.blockPower}
                  onClick={handleToggleEnabled}
                >
                  <Power />
                </ChromeIconButton>
                <span
                  style={{
                    fontFamily: FONT_MONO,
                    fontSize: '16rem',
                    fontWeight: 400,
                    color: WHITE,
                  }}
                >
                  Dual Mono
                </span>
              </div>
              <ChromeIconButton help={HELP.removeBlock} onClick={() => actions.removeBlock(blockId)}>
                <Trash2 />
              </ChromeIconButton>
            </div>

            <div
              className={uiOffClass(!enabled)}
              style={{
                flexShrink: 0,
                display: 'flex',
                flexDirection: 'row',
                alignItems: 'flex-start',
                justifyContent: 'center',
                gap: '32rem',
                padding: `${BODY_PADDING}rem`,
                boxSizing: 'border-box',
                transition: 'opacity 0.2s ease',
              }}
            >
              <DualSideCard
                dualBlockId={blockId}
                isLeftSide
                child={dualLeft}
                channelLimited={block.dualChannelLimited ?? false}
                panValue={dualLeftPan}
                onPanChange={handleDualLeftPanChange}
                onPanDragStateChange={handleKnobDragState}
                panHelp={HELP.dualPanLeft}
                onOpenChild={(initial) => openChild('left', initial)}
              />
              <div
                className={uiOffClass(block.dualChannelLimited ?? false)}
                style={{
                  display: 'flex',
                  alignItems: 'center',
                  justifyContent: 'center',
                  flexShrink: 0,
                  // Roughly centers the Width knob against the two side
                  // cards' own header rows, not their full (much taller)
                  // height.
                  minHeight: `${80 + 12 + 96}rem`,
                }}
              >
                <KnobControl
                  label="Width"
                  value={dualWidth}
                  onChange={handleDualWidthChange}
                  onDragStateChange={handleKnobDragState}
                  size={KNOB_SIZE_SECONDARY}
                  labelBottom={false}
                  thumb="secondary"
                  scale={percentScale}
                  defaultValue={1.0}
                  help={HELP.dualWidth}
                />
              </div>
              <DualSideCard
                dualBlockId={blockId}
                isLeftSide={false}
                child={dualRight}
                channelLimited={block.dualChannelLimited ?? false}
                panValue={dualRightPan}
                onPanChange={handleDualRightPanChange}
                onPanDragStateChange={handleKnobDragState}
                panHelp={HELP.dualPanRight}
                onOpenChild={(initial) => openChild('right', initial)}
              />
            </div>
          </div>
        </div>
      </div>
    );
  }

  return (
    <div
      className={showInfo ? 'hide-scrollbar' : undefined}
      style={{
        display: 'flex',
        flexDirection: 'column',
        width: `${CARD_WIDTH}rem`,
        height: '100%',
        boxSizing: 'border-box',
        overflowY: showInfo ? 'auto' : 'hidden',
        overflowX: 'hidden',
      }}
    >
      {/* 24px top/bottom pads live in the scroll content so ← BLOCK + card
          can reach the plugin header and faceplate. */}
      <div
        style={{
          display: 'flex',
          flexDirection: 'column',
          alignItems: 'stretch',
          padding: showInfo ? '24rem 0' : 0,
        }}
      >
        {/* ← BLOCK (Figma: 16px mono, gap 16) shares its row with the
            chain-map strip (issue #83's replacement for the old Prev/Next
            chevrons) so the strip doesn't add a row of its own height —
            that pushed the card down far enough to clip its bottom controls
            (e.g. Spread) in a typical-height window. Back stays fixed-width;
            the strip takes the remaining width and scrolls internally. */}
        <div
          style={{
            display: 'flex',
            flexDirection: 'row',
            alignItems: 'center',
            gap: '16rem',
            marginBottom: '16rem',
            flexShrink: 0,
          }}
        >
          <button
            type="button"
            onClick={onBack}
            {...helpProps(HELP.backToChain)}
            style={{
              display: 'flex',
              alignItems: 'center',
              gap: '16rem',
              flexShrink: 0,
              background: 'transparent',
              border: 'none',
              outline: 'none',
              padding: 0,
              cursor: 'pointer',
              color: WHITE,
            }}
          >
            <ArrowLeft size={16} style={{ display: 'block', flexShrink: 0 }} />
            <span
              style={{
                fontFamily: FONT_MONO,
                fontSize: '16rem',
                fontWeight: 400,
                textTransform: 'uppercase',
                lineHeight: 1.4,
              }}
            >
              Block
            </span>
          </button>

          {/* Every item in this block's lane, insert slots included, in
              chain order — so any block is one click away and every gap in
              the chain is a visible "+" at its real position, not just a
              trailing add button. Omitted for a Dual Mono side's own editor
              (hideChainStrip - see that prop's own comment): a side isn't a
              lane, it's a fixed single socket, so a plain spacer takes its
              place instead of inventing strip semantics for it. */}
          {hideChainStrip ? (
            <div style={{ flex: 1 }} />
          ) : (
            <ChainMapStrip
              items={chainStripItems}
              currentBlockId={blockId}
              onSelect={(id) => {
                onJumpToBlock(id);
                // Deterministic destination: a plain chip always lands on the
                // block's main content, regardless of whatever view (EQ,
                // Info) was active on screen before the click. Only
                // onSelectEq below ever turns showEq back on.
                setShowEq(false);
              }}
              onSelectEq={(id) => {
                onJumpToBlock(id);
                // Same pair the header's own EQ toggle sets (line ~1216):
                // showEq wins the body's render regardless of showInfo, but a
                // stale showInfo would still leave the Info chip reading
                // "open" underneath.
                setShowEq(true);
                setShowInfo(false);
              }}
              onAdd={onAddBlockAt}
              onPasteBlockAt={onPasteBlockAt}
            />
          )}

          {/* Invisible mirror of the ← BLOCK button: ChainMapStrip centers
              itself within its own flex:1 slot, but that slot only starts
              after the button, so without this the strip visually skews
              right (centered in the row minus the button's width, not in
              the row as a whole). A same-markup, visibility:hidden twin
              claims exactly the button's own layout width on the other
              side — self-maintaining if the label/icon ever changes,
              unlike a hardcoded width that could silently drift out of
              sync. */}
          <div
            aria-hidden
            style={{
              display: 'flex',
              alignItems: 'center',
              gap: '16rem',
              flexShrink: 0,
              visibility: 'hidden',
              pointerEvents: 'none',
            }}
          >
            <ArrowLeft size={16} style={{ display: 'block', flexShrink: 0 }} />
            <span
              style={{
                fontFamily: FONT_MONO,
                fontSize: '16rem',
                fontWeight: 400,
                textTransform: 'uppercase',
                lineHeight: 1.4,
              }}
            >
              Block
            </span>
          </div>
        </div>

        <div
          style={{
            display: 'flex',
            flexDirection: 'column',
            position: 'relative',
            width: '100%',
            height: showInfo ? undefined : `${CARD_HEIGHT}rem`,
            minHeight: `${CARD_HEIGHT}rem`,
            boxSizing: 'border-box',
            border: BORDER,
            borderRadius: `${CARD_RADIUS}rem`,
            overflow: 'hidden',
          }}
        >
          {/* Header: 16px inset, chrome centered in HEADER_HEIGHT. */}
          <div
            style={{
              height: `${HEADER_HEIGHT}rem`,
              flexShrink: 0,
              display: 'flex',
              alignItems: 'center',
              justifyContent: 'space-between',
              padding: `0 ${BODY_PADDING}rem`,
              boxSizing: 'border-box',
              borderBottom: BORDER,
            }}
          >
            <div style={{ display: 'flex', alignItems: 'center', gap: '24rem', flexShrink: 0 }}>
              <ChromeIconButton
                tone="power"
                on={enabled}
                help={HELP.blockPower}
                onClick={handleToggleEnabled}
              >
                <Power />
              </ChromeIconButton>

              {/* With per-block choice off, a block matching the new-block
                default has nothing to say: the chip only appears on a
                mismatch (e.g. a preset's FULL block under a lite default). */}
              {isNam && (sizeControlEnabled || slimFull !== isSlimSizeFull(namSlimSizeDefault)) && (
                <BlockSizeControl
                  full={slimFull}
                  interactive={sizeControlEnabled}
                  onChange={handleSetSlimFull}
                />
              )}

              {/* Also renders on a real CAB block now (isCab): the "IR
                Player" side is the reverse conversion back to a plain IR
                block (see handleCabCategoryClick), so the control has to
                stay visible there for the button to exist at all. */}
              {!isNam && (
                <IrCategoryControl
                  category={isCab ? 'cab' : irCategory}
                  onChange={handleCabCategoryClick}
                />
              )}

              {/* Calibration indicator (not a button): white = the loaded model
                carries calibration data, gray = it doesn't. Hidden entirely
                while the calibration setting is off. */}
              {showCalibration && (
                <span
                  {...helpProps(calibrationActive ? HELP.blockCalibrated : HELP.blockUncalibrated)}
                  style={{
                    width: `${ICON_BOX_SIZE}rem`,
                    height: `${ICON_BOX_SIZE}rem`,
                    display: 'grid',
                    placeItems: 'center',
                    color: calibrationActive ? WHITE : GRAY,
                  }}
                >
                  {chromeIcon(<Gauge />, ICON_SIZE)}
                </span>
              )}
            </div>

            {/* Orientation cue while the EQ view is open (issue #83 follow-up):
              the EQ body looks identical block to block, and PrevNext swaps
              which block it's showing without a mount/remount, so without
              this a step left no visible sign of which block you're now on.
              The non-EQ body already shows the title prominently, so this
              only needs to appear here. Reads straight off the `tone` prop,
              so it updates on every step automatically - no extra state. */}
            {showEq && (
              <span
                style={{
                  fontSize: '13rem',
                  color: GRAY,
                  fontWeight: 400,
                  overflow: 'hidden',
                  textOverflow: 'ellipsis',
                  whiteSpace: 'nowrap',
                  minWidth: 0,
                }}
              >
                {tone.title}
              </span>
            )}

            {/* Right cluster: EQ submenu (pill when open), info, then share/swap/trash.
              EQ stays rightmost in the submenu so opening grows left only.
              marginRight cancels the pill's right pad so EQ doesn't shift
              relative to info. */}
            <div style={{ display: 'flex', alignItems: 'center', gap: '24rem', flexShrink: 0 }}>
              <div
                style={{
                  display: 'inline-flex',
                  alignItems: 'center',
                  gap: showEq ? '16rem' : 0,
                  padding: showEq ? '4rem 12rem' : 0,
                  // Pull back by the pill's right pad so EQ stays put vs share.
                  marginRight: showEq ? -12 : 0,
                  borderRadius: showEq ? '100rem' : 0,
                  backgroundColor: showEq ? SEGMENTED_TRACK : 'transparent',
                  flexShrink: 0,
                  boxSizing: 'border-box',
                }}
              >
                {showEq && (
                  <>
                    <ChromeIconButton
                      tone="power"
                      on={eqOn}
                      help={HELP.eqPower}
                      onClick={handleToggleEqEnabled}
                    >
                      <Power />
                    </ChromeIconButton>
                    {/* PRE routes a live EQ; dims and goes inert with the
                        rest of the editor while the EQ is powered off. */}
                    <span
                      className={uiOffClass(!eqOn)}
                      style={{ display: 'inline-flex', transition: 'opacity 0.2s ease' }}
                    >
                      <ChromeTextButton armed={eqPre} help={HELP.eqPre} onClick={handleToggleEqPre}>
                        PRE
                      </ChromeTextButton>
                    </span>
                    <div
                      style={{
                        ...segmentedGroupStyle(),
                        // Nested track, slightly quieter than the outer pill.
                        backgroundColor: 'rgba(118, 118, 128, 0.24)',
                      }}
                    >
                      <button
                        onClick={() => setEqView('sliders')}
                        {...helpProps(HELP.eqSlidersView)}
                        style={{
                          ...segmentedCellStyle(true),
                          color: eqView === 'sliders' ? WHITE : GRAY,
                        }}
                      >
                        <EqSlidersIcon />
                      </button>
                      <button
                        onClick={() => setEqView('graph')}
                        {...helpProps(HELP.eqCurveView)}
                        style={{
                          ...segmentedCellStyle(true),
                          color: eqView === 'graph' ? WHITE : GRAY,
                        }}
                      >
                        <EqCurveIcon />
                      </button>
                    </div>
                    <span
                      className={uiOffClass(!eqOn)}
                      style={{ display: 'inline-flex', transition: 'opacity 0.2s ease' }}
                    >
                      <ChromeTextButton
                        armed={false}
                        help="Reset all EQ bands to default (flat)"
                        onClick={handleResetEq}
                      >
                        FLAT
                      </ChromeTextButton>
                    </span>
                  </>
                )}
                <ChromeTextButton
                  armed={eqActive}
                  open={showEq}
                  help={HELP.eqToggle}
                  onClick={() => {
                    setShowEq((prev) => !prev);
                    setShowInfo(false);
                  }}
                >
                  EQ
                </ChromeTextButton>
              </div>

              {/* Resets every IR shaping field (envelope, Size, Width, Trim
                Init, Reverse - not EQ, which has its own FLAT reset inside
                the EQ pill above) to default in one step. Gated exactly
                like the shaping UI itself (line ~1580 below): neither NAM
                nor CAB carries these fields - CAB is "structurally minimal
                by design", per ChainBlock.h. */}
              {!isNam && !isCab && (
                <ChromeTextButton help={HELP.blockResetShape} onClick={handleResetIrShape}>
                  Reset
                </ChromeTextButton>
              )}

              {!isLocal && (
                <ChromeIconButton open={showInfo} help={HELP.toneInfo} onClick={handleToggleInfo}>
                  <Info />
                </ChromeIconButton>
              )}
              {!isLocal && (
                <ChromeIconButton help={HELP.shareTone} onClick={handleShare}>
                  <Share />
                </ChromeIconButton>
              )}
              <ChromeIconButton
                help={HELP.swapTone}
                onClick={() => actions.swapBlock(blockId, { navigateToDetail: true })}
              >
                <ArrowLeftRight />
              </ChromeIconButton>
              <ChromeIconButton
                help={HELP.removeBlock}
                onClick={() => actions.removeBlock(blockId)}
              >
                <Trash2 />
              </ChromeIconButton>
            </div>
          </div>

          {/* Body: tone view uses BODY_PADDING; EQ spectrum/grid bleeds
            edge-to-edge (interactive chrome insets itself). Info view drops
            knobs/model select and lets the right column grow. While the
            block is bypassed the whole body dims and goes inert (uiOffClass);
            the header (power, EQ, info, share, swap, trash) stays live. */}
          <div
            className={uiOffClass(!enabled)}
            style={{
              height: showInfo ? undefined : `${BODY_HEIGHT}rem`,
              flexShrink: 0,
              display: 'flex',
              flexDirection: 'row',
              alignItems: 'stretch',
              // IR Player only: 20rem, not the shared 24rem - the control
              // row's own internal gap (below) is 8rem, but In/Out (this
              // Body-level gap's own neighbors) are bare 36rem rails with
              // no inset of their own, while every item across that 8rem
              // gap - Delay, Width - sits inset 12rem inside its own 60rem
              // slot (room for "Delay"/"Width" to spill without clipping,
              // see the slot comment below). So In->Delay and Mix->Out
              // need this Body-level gap to carry that missing 12rem too
              // (8 + 12 = 20) to visually match Delay->Size and Width->Mix
              // (8 + 12 + 12 = 32) - confirmed by cropping a real
              // screenshot and measuring the actual pixel gaps, not just
              // eyeballing it: at a plain 8rem this gap measured
              // ~30% narrower than the others. Widening it only grows the
              // gap on both sides of the Center column equally (In/Out's
              // own fixed-width rails don't move, and the flex:1 Center
              // column - waveform included - just gets proportionally
              // narrower, still symmetric left/right). NAM/CAB/showInfo
              // keep the original 24rem - untouched, not part of this fix.
              gap: showEq || showInfo ? 0 : !isNam && !isCab ? '20rem' : '24rem',
              padding: showEq
                ? 0
                : showInfo
                  ? `${BODY_PADDING}rem ${BODY_PADDING}rem 24rem`
                  : `${BODY_PADDING}rem`,
              boxSizing: 'border-box',
              position: 'relative',
              transition: 'opacity 0.2s ease',
              // Keep the body on its own pixel-snapped compositor layer so the
              // opacity fade (power toggle) can't promote/demote a temporary layer
              // that nudges inner content (notably the scaled EQ SVG) by a pixel.
              transform: 'translateZ(0)',
              willChange: 'opacity',
            }}
          >
            {showEq ? (
              <BlockEqView
                blockId={blockId}
                bands={params.eq?.bands ?? []}
                eqEnabled={eqOn}
                sampleRate={sampleRate}
                view={eqView}
                onSetBand={actions.setBlockEqBand}
              />
            ) : (
              <>
                {/* Input rail: meter above In knob (Figma: gap 12). Single
                  knob, matching the Output rail and every other block type
                  exactly - Delay/Size (IR Player only, no meters of their
                  own) live in the shaping area's own knob row instead (see
                  below the envelope chips), not here: widening this rail
                  per extra knob used to squeeze the Center column's flex:1
                  width, directly costing the waveform strip (see
                  IR_WAVEFORM_HEIGHT's own history above for why that's
                  worth avoiding). */}
                {!showInfo && (
                  <div
                    style={{
                      display: 'flex',
                      flexDirection: 'column',
                      alignItems: 'flex-start',
                      flexShrink: 0,
                      gap: '12rem',
                    }}
                  >
                    <div
                      style={{
                        flex: 1,
                        display: 'flex',
                        alignItems: 'center',
                        justifyContent: 'center',
                        minHeight: 0,
                        width: `${KNOB_SIZE_SECONDARY}rem`,
                      }}
                    >
                      <BlockMeter meterId={meterId.blockIn(blockId)} length={RAIL_METER_HEIGHT} />
                    </div>
                    <div
                      style={{
                        display: 'flex',
                        flexDirection: 'row',
                        alignItems: 'flex-end',
                        gap: '12rem',
                      }}
                    >
                      <KnobControl
                        label="In"
                        value={inputGain}
                        onChange={(val) => {
                          setInputGain(val);
                          setParam('inputGain', val);
                        }}
                        onDragStateChange={handleKnobDragState}
                        size={KNOB_SIZE_SECONDARY}
                        labelBottom={false}
                        thumb="secondary"
                        scale={gainDbScale}
                        defaultValue={0.5}
                        help={HELP.blockIn}
                      />
                    </div>
                  </div>
                )}

                {/* Center: NAM blocks and the Info view (either format) keep
                  the original image + full metadata layout. IR blocks
                  outside Info view get a reworked, intentionally temporary
                  v1 layout instead - see IR_WAVEFORM_HEIGHT's comment: a
                  compact identity row, a wide short waveform strip, and the
                  Start/End shaping rows, all ABOVE the model picker (which
                  stays full-width at the bottom either way, matching the
                  original layout). */}
                <div
                  style={{
                    flex: 1,
                    minWidth: 0,
                    alignSelf: 'stretch',
                    display: 'flex',
                    flexDirection: 'column',
                    // IR Player (else branch below) now stacks five items
                    // here (identity, waveform, chips, the Delay/Size/Width
                    // row, model picker) instead of three, so the gap
                    // tightens from 10 to 8 to keep the header snug against
                    // the top and everything comfortably inside the fixed
                    // body height - validated against a standalone layout
                    // mockup before landing here (see PR description).
                    gap: showInfo ? '24rem' : isNam ? '16rem' : '6rem',
                    justifyContent: 'flex-start',
                  }}
                >
                  {/* CAB takes this simpler image+title layout too - the
                    "else" branch below (waveform strip, shaping rows,
                    IrEnvelopeGraph) is the full IR Player feature set, which
                    a CAB block's data model has nothing to back (no
                    predelay/envelope/waveform fields at all; see
                    ToneBlock.blockType). */}
                  {showInfo || isNam || isCab ? (
                    <div
                      style={{
                        display: 'flex',
                        flexDirection: 'row',
                        alignItems: showInfo ? 'flex-start' : 'center',
                        gap: '24rem',
                        minWidth: 0,
                      }}
                    >
                      {/* Tone image (gear glyph fallback, like the web's ToneCard) */}
                      <div
                        style={{
                          position: 'relative',
                          width: rem(showInfo ? IMAGE_SIZE_INFO : IMAGE_SIZE),
                          height: rem(showInfo ? IMAGE_SIZE_INFO : IMAGE_SIZE),
                          borderRadius: '8rem',
                          overflow: 'hidden',
                          flexShrink: 0,
                        }}
                      >
                        <div
                          style={{
                            opacity: modelBusy || block.loadFailed ? 0.35 : 1,
                            transition: 'opacity 0.2s ease',
                            width: '100%',
                            height: '100%',
                          }}
                        >
                          {/* The envelope/waveform graphic is a compact-card
                            stand-in for the artwork (see IR_WAVEFORM_HEIGHT's
                            sibling above); the info panel always shows the
                            real TONE3000 preview image instead, since that's
                            what "i" is for. Prefers infoTone's own images
                            (the freshly fetched full tone, once loaded) over
                            the block's cached tone snapshot, so a stale
                            cached image can't outlive the fetch. */}
                          {!isNam && !showInfo && irWaveform ? (
                            <WaveformDisplay
                              mins={irWaveform.mins}
                              maxs={irWaveform.maxs}
                              width={IMAGE_SIZE}
                              height={IMAGE_SIZE}
                              contentLengthMs={block.irContentLengthMs}
                              cutFraction={cutFraction}
                              startFraction={trimInit ? block.irOnsetFraction : undefined}
                              reversed={reverse}
                              decay={{
                                initLevel,
                                attackCurve,
                                attackFraction: attackFractionWithinWindow,
                                decayLevel,
                                decayCurve,
                              }}
                            />
                          ) : (
                            <ToneImage
                              src={(showInfo && infoTone?.images?.[0]) || tone.images?.[0]}
                              alt={tone.title}
                              gear={tone.gear}
                              local={tone.local}
                              blockType={block.blockType}
                              boxSize={showInfo ? IMAGE_SIZE_INFO : IMAGE_SIZE}
                            />
                          )}
                        </div>
                        {(modelBusy || block.loadFailed) && (
                          <div
                            style={{
                              position: 'absolute',
                              inset: 0,
                              display: 'flex',
                              alignItems: 'center',
                              justifyContent: 'center',
                            }}
                          >
                            {block.loadFailed ? (
                              <RetryLoadBadge onRetry={() => actions.retryLoad(blockId)} />
                            ) : (
                              <LoadingDots />
                            )}
                          </div>
                        )}
                      </div>

                      {/* Tone info: title / gear+badge / counts / creator (Figma gaps).
                        Info view appends description / makes / tags under this. */}
                      <div
                        style={{
                          display: 'flex',
                          flexDirection: 'column',
                          gap: showInfo ? '24rem' : '16rem',
                          minWidth: 0,
                          flex: 1,
                        }}
                      >
                        <div
                          style={{
                            display: 'flex',
                            flexDirection: 'column',
                            gap: '16rem',
                            minWidth: 0,
                          }}
                        >
                          <div
                            style={{
                              display: 'flex',
                              flexDirection: 'column',
                              gap: '8rem',
                              minWidth: 0,
                            }}
                          >
                            <span
                              style={{
                                fontSize: '18rem',
                                color: WHITE,
                                fontWeight: 700,
                                lineHeight: 1.4,
                                display: '-webkit-box',
                                WebkitLineClamp: 2,
                                WebkitBoxOrient: 'vertical',
                                overflow: 'hidden',
                              }}
                            >
                              {tone.title}
                            </span>

                            <div
                              style={{
                                display: 'flex',
                                flexDirection: 'row',
                                alignItems: 'center',
                                gap: '16rem',
                              }}
                            >
                              {tone.gear && (
                                <span style={{ fontSize: '14rem', color: MUTED, fontWeight: 400 }}>
                                  {gearLabel(tone.gear)}
                                </span>
                              )}
                              {formatBadge && <FormatBadge label={formatBadge} a2={isNam} />}
                            </div>
                          </div>

                          {!isLocal && (
                            <div
                              style={{
                                display: 'flex',
                                flexDirection: 'row',
                                alignItems: 'center',
                                gap: '24rem',
                              }}
                            >
                              <CountStat
                                icon={<Download size={16} />}
                                value={tone.downloads_count ?? 0}
                              />
                              <BookmarkStat
                                value={favoritesCount}
                                favorited={favorited}
                                onToggle={
                                  actions.authenticated
                                    ? () => void handleToggleFavorite()
                                    : undefined
                                }
                              />
                              <CountStat
                                icon={<FolderClosed size={16} />}
                                value={modelsTotal ?? 0}
                              />
                            </div>
                          )}

                          {tone.user && (
                            <div style={{ display: 'flex', alignItems: 'center', gap: '12rem' }}>
                              <div
                                style={{
                                  width: '32rem',
                                  height: '32rem',
                                  borderRadius: '50%',
                                  overflow: 'hidden',
                                  flexShrink: 0,
                                }}
                              >
                                <AvatarImage
                                  src={tone.user.avatar_url}
                                  alt={tone.user.username}
                                  size={32}
                                />
                              </div>
                              <span style={{ fontSize: '14rem', color: GRAY, fontWeight: 400 }}>
                                {tone.user.username}
                                {tone.published_at && (
                                  <span style={{ color: MUTED }}>
                                    {' '}
                                    · {timeAgoShort(tone.published_at)}
                                  </span>
                                )}
                              </span>
                            </div>
                          )}
                        </div>

                        {showInfo && (
                          <BlockInfoPanel
                            loading={infoLoading}
                            error={infoError}
                            onRetry={() => void fetchInfo(tone.id)}
                            authenticated={actions.authenticated}
                            onLogin={actions.login}
                            tone={infoTone}
                            pageUrl={tonePageUrl}
                          />
                        )}
                      </div>
                    </div>
                  ) : (
                    <>
                      {/* Compact identity row: title, then gear+badge sharing
                        their row with a condensed creator/counts readout
                        (CompactToneMetaRow) - restored here after the
                        layout-refactor commit (d88e30d) that introduced the
                        waveform strip dropped them for vertical room. Fit
                        into the gear+badge row's own unused width (via
                        space-between) rather than adding a whole new row, so
                        the waveform strip below keeps its full original
                        height - see CompactToneMetaRow's own comment. */}
                      <div
                        style={{
                          display: 'flex',
                          flexDirection: 'column',
                          gap: '4rem',
                          minWidth: 0,
                        }}
                      >
                        <span
                          style={{
                            fontSize: '16rem',
                            color: WHITE,
                            fontWeight: 700,
                            lineHeight: 1.3,
                            whiteSpace: 'nowrap',
                            overflow: 'hidden',
                            textOverflow: 'ellipsis',
                          }}
                        >
                          {tone.title}
                        </span>
                        <div
                          style={{
                            display: 'flex',
                            flexDirection: 'row',
                            alignItems: 'center',
                            justifyContent: 'space-between',
                            gap: '12rem',
                            minWidth: 0,
                          }}
                        >
                          <div
                            style={{
                              display: 'flex',
                              flexDirection: 'row',
                              alignItems: 'center',
                              gap: '12rem',
                              flexShrink: 0,
                            }}
                          >
                            {tone.gear && (
                              <span style={{ fontSize: '13rem', color: MUTED, fontWeight: 400 }}>
                                {gearLabel(tone.gear)}
                              </span>
                            )}
                            {formatBadge && <FormatBadge label={formatBadge} a2={isNam} />}
                          </div>
                          {!isLocal && (
                            <CompactToneMetaRow
                              tone={tone}
                              favoritesCount={favoritesCount}
                              favorited={favorited}
                              onToggleFavorite={
                                actions.authenticated
                                  ? () => void handleToggleFavorite()
                                  : undefined
                              }
                            />
                          )}
                        </div>
                      </div>

                      {/* Waveform: full Center-column width, short strip -
                        the shape the future drag-to-shape v2 surface wants,
                        and what frees the vertical room for the rows below.
                        marginBottom overrides the column's own 6rem gap
                        down to 3rem for this one pair only (waveform ->
                        chips specifically asked to sit tighter than the
                        other gaps) without touching the gap above it
                        (identity -> waveform stays the full 6rem). */}
                      <div
                        style={{
                          position: 'relative',
                          width: '100%',
                          height: rem(IR_WAVEFORM_HEIGHT),
                          borderRadius: '8rem',
                          overflow: 'hidden',
                          flexShrink: 0,
                          marginBottom: '-3rem',
                        }}
                      >
                        <div
                          style={{
                            opacity: modelBusy || block.loadFailed ? 0.35 : 1,
                            transition: 'opacity 0.2s ease',
                            width: '100%',
                            height: '100%',
                          }}
                        >
                          {irWaveform ? (
                            <IrEnvelopeGraph
                              mins={irWaveform.mins}
                              maxs={irWaveform.maxs}
                              width={IR_WAVEFORM_WIDTH_UNITS}
                              height={IR_WAVEFORM_HEIGHT}
                              contentLengthMs={block.irContentLengthMs}
                              initLevel={initLevel}
                              attackLength={attackLength}
                              attackCurve={attackCurve}
                              decayLength={decayLength}
                              decayLevel={decayLevel}
                              decayCurve={decayCurve}
                              attackLengthScale={attackLengthScale}
                              decayLengthScale={decayLengthScale}
                              predelay={predelay}
                              startFraction={trimInit ? block.irOnsetFraction : undefined}
                              reversed={reverse}
                              onChange={updateEnvelope}
                              onDragStateChange={handleKnobDragState}
                            />
                          ) : (
                            <div
                              style={{
                                width: '100%',
                                height: '100%',
                                backgroundColor: SEGMENTED_TRACK,
                                borderRadius: '8rem',
                              }}
                            />
                          )}
                        </div>
                        {(modelBusy || block.loadFailed) && (
                          <div
                            style={{
                              position: 'absolute',
                              inset: 0,
                              display: 'flex',
                              alignItems: 'center',
                              justifyContent: 'center',
                            }}
                          >
                            {block.loadFailed ? (
                              <RetryLoadBadge onRetry={() => actions.retryLoad(blockId)} />
                            ) : (
                              <LoadingDots />
                            )}
                          </div>
                        )}
                      </div>

                      {/* Shaping row: Init/Attack (Length/Curve)/Decay
                        (Length/Level/Curve) as precision-entry chips (same
                        EditableChip pattern as the EQ's Freq/Gain/Q row) -
                        the graph above is the by-ear control now, this row
                        is for landing an exact value pointer-dragging can't
                        reliably hit. Predelay stays a knob in the Input rail
                        (it's a real-time DSP stage, not part of this
                        off-thread envelope rebuild). Idle-debounced (see
                        commitEnvelope) - chip commits and graph drags both
                        route through updateEnvelope, so they can't race.
                        Trim Init and Reverse (last two, plain ToggleChips
                        not EditableChips - see setBlockIrTrimInit/
                        setBlockIrReverse; Trim cycles Off/Std/Lax via its
                        `value` override, Rev stays a plain on/off) ride the
                        same row since they're
                        cheap, off-thread-rebuild shaping, same family as the
                        six numeric ones, just boolean - toggleChipStyle
                        gives them a narrower, matched width than the
                        numeric chips (nothing to type, shorter labels/
                        values) so the pair saves real row width rather than
                        inheriting the numeric chips' "20.00s"-sized value
                        area. flex-start + a small fixed gap, not
                        space-between: with eight chips this still doesn't
                        fill the card width, and space-between would stretch
                        the leftover into large gaps instead of a compact
                        cluster. */}
                      <div
                        style={{
                          display: 'flex',
                          flexDirection: 'row',
                          alignItems: 'center',
                          justifyContent: 'flex-start',
                          gap: '8rem',
                        }}
                      >
                        <EditableChip
                          label="Init"
                          text={percentScale.format(initLevel)}
                          editText={percentScale.editText(initLevel)}
                          valueWidth={38}
                          fontSize={ENVELOPE_CHIP_FONT_SIZE}
                          onCommit={(raw) => commitEnvelopeField('initLevel', percentScale, raw)}
                          help={HELP.blockInitLevel}
                          style={chipStyle}
                        />
                        <EditableChip
                          label="A Len"
                          text={attackLengthScale.format(attackLength)}
                          editText={attackLengthScale.editText(attackLength)}
                          valueWidth={38}
                          fontSize={ENVELOPE_CHIP_FONT_SIZE}
                          onCommit={(raw) =>
                            commitEnvelopeField('attackLength', attackLengthScale, raw)
                          }
                          help={HELP.blockAttackLength}
                          style={chipStyle}
                        />
                        <EditableChip
                          label="A Crv"
                          text={curveScale.format(attackCurve)}
                          editText={curveScale.editText(attackCurve)}
                          valueWidth={38}
                          fontSize={ENVELOPE_CHIP_FONT_SIZE}
                          onCommit={(raw) => commitEnvelopeField('attackCurve', curveScale, raw)}
                          help={HELP.blockAttackCurve}
                          style={chipStyle}
                        />
                        <EditableChip
                          label="D Len"
                          text={decayLengthScale.format(decayLength)}
                          editText={decayLengthScale.editText(decayLength)}
                          valueWidth={38}
                          fontSize={ENVELOPE_CHIP_FONT_SIZE}
                          onCommit={(raw) =>
                            commitEnvelopeField('decayLength', decayLengthScale, raw)
                          }
                          help={HELP.blockDecayLength}
                          style={chipStyle}
                        />
                        <EditableChip
                          label="D Lvl"
                          text={percentScale.format(decayLevel)}
                          editText={percentScale.editText(decayLevel)}
                          valueWidth={38}
                          fontSize={ENVELOPE_CHIP_FONT_SIZE}
                          onCommit={(raw) => commitEnvelopeField('decayLevel', percentScale, raw)}
                          help={HELP.blockDecayLevel}
                          style={chipStyle}
                        />
                        <EditableChip
                          label="D Crv"
                          text={curveScale.format(decayCurve)}
                          editText={curveScale.editText(decayCurve)}
                          valueWidth={38}
                          fontSize={ENVELOPE_CHIP_FONT_SIZE}
                          onCommit={(raw) => commitEnvelopeField('decayCurve', curveScale, raw)}
                          help={HELP.blockDecayCurve}
                          style={chipStyle}
                        />
                        <ToggleChip
                          label="Trim"
                          on={trimInit}
                          value={trimInit ? (trimRelaxed ? 'Lax' : 'Std') : 'Off'}
                          onToggle={handleCycleTrimMode}
                          valueWidth={TOGGLE_CHIP_VALUE_WIDTH}
                          fontSize={ENVELOPE_CHIP_FONT_SIZE}
                          help={HELP.blockTrimInit}
                          style={toggleChipStyle}
                        />
                        <ToggleChip
                          label="Rev"
                          on={reverse}
                          onToggle={handleToggleReverse}
                          valueWidth={TOGGLE_CHIP_VALUE_WIDTH}
                          fontSize={ENVELOPE_CHIP_FONT_SIZE}
                          help={HELP.blockReverse}
                          style={toggleChipStyle}
                        />
                      </div>

                      {/* Control row: Delay, Size, the model picker, Width,
                        Mix - the five things between the In and Out rails,
                        in that exact order (Width sits between the picker
                        and Mix, not grouped with Delay/Size, so the row
                        reads as In+Delay+Size mirroring Width+Mix+Out with
                        the picker as the pivot). Pure justifyContent:
                        'space-between' with no extra `gap` - the four knobs
                        and the picker each keep their own intrinsic width,
                        and the leftover row space becomes four *equal* gaps
                        between them, regardless of how differently sized
                        the picker is from a knob. Keeping Mix and the
                        picker here (not in their own rail/column) is what
                        lets the Center column - and the waveform/chips
                        inside it - reach all the way from the Input rail to
                        the Output rail instead of stopping short at a
                        separate Mix column (see the Input rail's own
                        comment for the general coupling, and the removed
                        standalone Mix column's comment for this specific
                        case). No meter/companion chrome needed for any of
                        these five, so they never needed their own rail. */}
                      <div
                        style={{
                          display: 'flex',
                          flexDirection: 'row',
                          alignItems: 'flex-end',
                          // Was justifyContent: 'space-between' (data-
                          // dependent leftover-space split) - an explicit
                          // gap here is the only way to guarantee every
                          // one of these five gaps is the identical CSS
                          // value. This row's own 8rem is deliberately
                          // *not* the same number as the Body row's own
                          // IR-Player gap (20rem, see the Body div's own
                          // style) - In/Out are bare rails with no inset
                          // of their own, while Delay/Size/Width/Mix each
                          // sit inset 12rem inside their own 60rem slot
                          // (room for "Delay"/"Width" to spill without
                          // clipping, see the slot comment below), so the
                          // Body row's gap has to carry that extra 12rem
                          // itself for In->Delay/Mix->Out to *look* the
                          // same width as Delay->Size/Width->Mix - a plain
                          // matching 8rem at both levels left In->Delay and
                          // Mix->Out visibly ~30% narrower, confirmed by
                          // cropping a real screenshot and measuring the
                          // actual pixel gaps. The picker needed the same
                          // treatment: it was filling its own slot flush
                          // while every knob sits inset 12rem within its
                          // own - see the picker's own box below, which
                          // now insets 12rem the same way. The picker's
                          // own width is sized so five items at fixed
                          // widths plus four 8rem gaps sums to the Center
                          // column's actual available width, same
                          // arithmetic as the two 8rem Body-level gaps
                          // (12rem still read too loose on a real check).
                          gap: '8rem',
                          // The Center column is alignSelf: stretch (full
                          // body height, matching the In/Out rails), but its
                          // own children use justifyContent: flex-start -
                          // packed from the top, not stretched - so without
                          // this, this row just lands wherever
                          // identity+waveform+chips's own heights happen to
                          // end, with unused space below it, not at the
                          // Center column's true bottom. marginTop: auto
                          // absorbs that leftover space instead, landing
                          // this row's own bottom - and via alignItems:
                          // flex-end above, every knob's bottom edge - at
                          // the Center column's actual bottom edge, the
                          // same edge the In/Out rails (full-height,
                          // flex-end internally) anchor their own knobs to.
                          // The earlier attempts at this (a translateY nudge
                          // on the picker alone) were compensating for the
                          // wrong thing: not the picker vs. its row
                          // neighbors, but the whole row vs. In/Out.
                          marginTop: 'auto',
                        }}
                      >
                        {/* Each knob sits in a fixed-width (60rem), centered
                          slot - not left as a bare KnobControl. KnobControl's
                          own label sits in a fixed 36rem-wide box (matching
                          the knob) with overflow: visible (KnobControl.tsx),
                          so a 5-letter label like "Delay"/"Width" can
                          *visually* spill past its box without widening it -
                          but the row's own space-between still measures
                          gaps from each item's true (36rem) box edges, not
                          the overflowed glyphs, and 60rem gives that spill
                          room to breathe without visually crowding the
                          neighboring item. Matching all four to the same
                          slot width is what actually mattered here: it's
                          not about any one knob being wrong-sized, it's
                          about all four needing to be *identically* sized
                          so space-between's equal-gap math (which operates
                          on box edges) produces equal-looking gaps between
                          the circles too. */}
                        <div
                          style={{
                            width: '60rem',
                            display: 'flex',
                            justifyContent: 'center',
                            flexShrink: 0,
                          }}
                        >
                          {/* Predelay before the wet signal enters the
                            convolver - a real-time DSP stage (BlockPredelay),
                            not part of the off-thread envelope rebuild. */}
                          <KnobControl
                            label="Delay"
                            value={predelay}
                            onChange={(val) => {
                              setPredelay(val);
                              setParam('predelay', val);
                            }}
                            onDragStateChange={handleKnobDragState}
                            size={KNOB_SIZE_SECONDARY}
                            labelBottom={false}
                            thumb="secondary"
                            scale={predelayMsScale}
                            defaultValue={0}
                            help={HELP.blockPredelay}
                          />
                        </div>
                        <div
                          style={{
                            width: '60rem',
                            display: 'flex',
                            justifyContent: 'center',
                            flexShrink: 0,
                          }}
                        >
                          {/* Size: vari-speed duration/pitch over the frozen
                            source, applied upstream of the envelope natively
                            (see setBlockIrSize). */}
                          <KnobControl
                            label="Size"
                            value={irSize}
                            onChange={handleIrSizeChange}
                            onDragStateChange={handleIrSizeDragState}
                            size={KNOB_SIZE_SECONDARY}
                            labelBottom={false}
                            thumb="secondary"
                            variant="bipolar"
                            scale={irSizeScale}
                            defaultValue={0.5}
                            help={HELP.blockIrSize}
                          />
                        </div>
                        {/* Model picker: inline in the control row rather
                          than its own row below - the full card height has
                          no slack left for a whole extra row (confirmed by
                          screenshotting the real running plugin, not just an
                          isolated mockup: a separate row here pushed this
                          picker entirely past the card's visible bottom
                          edge). ModelSelect's name text already ellipses
                          (see ModelSelect.tsx) and its prev/next chevrons
                          have fixed intrinsic width, so a fixed, generous
                          (not flex:1 - that would swallow the space-between
                          gaps meant for the knobs either side of it) width
                          renders cleanly at this size. NAM/CAB use the
                          separate full-width picker below instead (this row
                          doesn't exist for them). */}
                        <div
                          {...(!isLocal && !actions.authenticated
                            ? helpProps(HELP.modelSelectSignedOut)
                            : {})}
                          style={{
                            // Arithmetic, not a guess: Center column width
                            // (CARD_WIDTH 800 - 2*BODY_PADDING 16 - Input
                            // rail 36 - Output rail 36 - the Body row's own
                            // 2*20rem IR-Player gaps either side of the
                            // Center column = 656rem) minus four 60rem knob
                            // slots (240) and four 8rem gaps (32) leaves
                            // 384rem for the picker, so the five items plus
                            // their four gaps exactly fill the same width
                            // the Input/Center/Output rails already sum to -
                            // if any of those constants (or either gap
                            // value) change, this number needs to move
                            // with them.
                            width: '384rem',
                            flexShrink: 0,
                            // A knob's own visible face is 36rem, but it
                            // sits centered in a 60rem slot - 12rem of
                            // breathing room on each side before this row's
                            // own 8rem `gap` even starts (room the "Delay"/
                            // "Width" labels spill into, see the slot
                            // comment above). The picker's own inner pill
                            // fills its slot edge to edge with zero such
                            // inset, so with a bare box the *visible* gap
                            // either side of it was 24rem narrower than
                            // every knob-to-knob gap - not a height issue at
                            // all (a real screenshot, cropped/measured pixel
                            // by pixel, showed the CSS gap value was already
                            // correct; only the picker's own zero-inset
                            // pill made it look otherwise). Padding this box
                            // by the same 12rem each side - and letting
                            // ModelSelect's own width:100% shrink to fill
                            // the remainder - gives the picker the identical
                            // visible inset a knob already has, without
                            // changing this box's own 384rem footprint (so
                            // the row-width arithmetic above still holds).
                            boxSizing: 'border-box',
                            padding: '0 12rem',
                            // Matches a knob's own total column height
                            // (label slot 17rem + KNOB_LABEL_GAP 8rem +
                            // KNOB_SIZE_SECONDARY 36rem, see KnobControl.tsx)
                            // - with alignItems: flex-end on both this box
                            // and the row it sits in, only the *bottom* edge
                            // this produces is load-bearing (a shorter box
                            // still lands its content on the same baseline);
                            // matching the real total height just keeps this
                            // element's own footprint honest.
                            height: '61rem',
                            display: 'flex',
                            alignItems: 'flex-end',
                            cursor: isLocal || actions.authenticated ? 'default' : 'not-allowed',
                          }}
                        >
                          <ModelSelect
                            options={modelOptions.map((m) => ({ id: String(m.id), name: m.name }))}
                            value={String(block.activeModelId)}
                            onChange={handleModelSelect}
                            onOpen={handleModelsOpen}
                            height={KNOB_SIZE_SECONDARY}
                            disabled={!isLocal && !actions.authenticated}
                            loading={modelsLoading}
                            totalCount={isLocal ? tone.models.length : modelsTotal}
                          />
                        </div>
                        {/* Width: stereo-image crossfade/phase-inversion
                          (see setBlockIrWidth); locked/dimmed when the
                          loaded IR has no stereo image to widen. Same fixed
                          60rem slot as Delay/Size/Mix - see this row's own
                          comment on why they all need to match. */}
                        <div
                          className={uiOffClass(isMonoIr)}
                          style={{
                            width: '60rem',
                            flexShrink: 0,
                            display: 'flex',
                            justifyContent: 'center',
                            transition: 'opacity 0.2s ease',
                          }}
                        >
                          <KnobControl
                            label="Width"
                            value={irWidth}
                            onChange={handleIrWidthChange}
                            onDragStateChange={handleIrWidthDragState}
                            size={KNOB_SIZE_SECONDARY}
                            labelBottom={false}
                            thumb="secondary"
                            variant="bipolar"
                            scale={widthPercentScale}
                            // 100%/full original stereo, not the bipolar
                            // center (0%/mono) - Alt-click/double-tap should
                            // reset to "unmodified", matching the block's
                            // own load-time default (see ChainBlock::
                            // widthNormalized's comment).
                            defaultValue={0.75}
                            help={isMonoIr ? HELP.blockIrWidthMono : HELP.blockIrWidth}
                          />
                        </div>
                        <div
                          style={{
                            width: '60rem',
                            display: 'flex',
                            justifyContent: 'center',
                            flexShrink: 0,
                          }}
                        >
                          {/* Mix: dry/wet blend. Lives here (not a separate
                            column before the Output rail) so the Center
                            column reaches all the way to that rail - see
                            this row's own top comment. */}
                          <KnobControl
                            label="Mix"
                            value={mix}
                            onChange={(val) => {
                              setMix(val);
                              setParam('mix', val);
                            }}
                            onDragStateChange={handleKnobDragState}
                            size={KNOB_SIZE_SECONDARY}
                            labelBottom={false}
                            thumb="secondary"
                            defaultValue={defaultMix}
                            help={HELP.blockMix}
                          />
                        </div>
                      </div>
                    </>
                  )}

                  {/* Model picker: full width, below everything - NAM/CAB
                    only (IR Player's own picker lives inline with
                    Delay/Size/Width above instead - see that row's own
                    comment). marginTop: auto absorbs the Center column's
                    leftover height so this stays bottom-aligned with the
                    In/Mix/Out rails' knobs instead of floating above them.
                    Switching catalog models re-downloads through native
                    with a Bearer token, so the picker is inert while signed
                    out - the wrapper carries the cursor + hint, as the
                    select itself is pointer-events: none when disabled.
                    Local switches read the stash: no auth, and the picker
                    always shows (the dropped file names are the block's
                    provenance). */}
                  {!showInfo && (isNam || isCab) && (
                    <div
                      {...(!isLocal && !actions.authenticated
                        ? helpProps(HELP.modelSelectSignedOut)
                        : {})}
                      style={{
                        marginTop: 'auto',
                        cursor: isLocal || actions.authenticated ? 'default' : 'not-allowed',
                      }}
                    >
                      <ModelSelect
                        options={modelOptions.map((m) => ({ id: String(m.id), name: m.name }))}
                        value={String(block.activeModelId)}
                        onChange={handleModelSelect}
                        onOpen={handleModelsOpen}
                        height={36}
                        disabled={!isLocal && !actions.authenticated}
                        loading={modelsLoading}
                        totalCount={isLocal ? tone.models.length : modelsTotal}
                      />
                    </div>
                  )}
                </div>

                {/* Mix knob: bottom aligned, between the model select and
                  the output rail. NAM/CAB only - IR Player's Mix lives
                  inline in the shaping area's own knob row instead (see
                  above), not here: keeping it here too would reintroduce a
                  separate column between the Center column and the Output
                  rail, which is exactly what stopped the waveform strip
                  short of reaching Out (see that row's own comment). */}
                {!showInfo && (isNam || isCab) && (
                  <div
                    style={{
                      display: 'flex',
                      flexDirection: 'column',
                      alignItems: 'center',
                      justifyContent: 'flex-end',
                      flexShrink: 0,
                    }}
                  >
                    <KnobControl
                      label="Mix"
                      value={mix}
                      onChange={(val) => {
                        setMix(val);
                        setParam('mix', val);
                      }}
                      onDragStateChange={handleKnobDragState}
                      size={KNOB_SIZE_SECONDARY}
                      labelBottom={false}
                      thumb="secondary"
                      defaultValue={defaultMix}
                      help={HELP.blockMix}
                    />
                  </div>
                )}

                {/* Output rail: meter above Out (+ optional normalize). The rail
                right-aligns and the meter wrapper is knob-wide, so the meter
                stays centered over the Out knob whether or not the normalize
                button widens the bottom row to its left. */}
                {!showInfo && (
                  <div
                    style={{
                      display: 'flex',
                      flexDirection: 'column',
                      alignItems: 'flex-end',
                      flexShrink: 0,
                      gap: '12rem',
                    }}
                  >
                    <div
                      style={{
                        flex: 1,
                        display: 'flex',
                        alignItems: 'center',
                        justifyContent: 'center',
                        minHeight: 0,
                        width: `${KNOB_SIZE_SECONDARY}rem`,
                      }}
                    >
                      <BlockMeter meterId={meterId.blockOut(blockId)} length={RAIL_METER_HEIGHT} />
                    </div>
                    <div
                      style={{
                        display: 'flex',
                        flexDirection: 'row',
                        alignItems: 'flex-end',
                        gap: '12rem',
                      }}
                    >
                      {isNam && showNormalizeControl && (
                        /* The wrapper carries the vertical nudge and the overridden
                     hint: a disabled button swallows hover (no mouseover ever
                     fires), so pointer-events pass through it to this span
                     and the hint delegation resolves here instead. */
                        <span
                          {...helpProps(
                            normalizeOverridden
                              ? HELP.blockNormalizeOverridden
                              : HELP.blockNormalize
                          )}
                          style={{
                            display: 'inline-flex',
                            transform: `translateY(${NORMALIZE_BUTTON_OFFSET}rem)`,
                            // The button below is pointer-events: none while
                            // overridden, so the cursor reads from here.
                            cursor: normalizeOverridden ? 'not-allowed' : undefined,
                          }}
                        >
                          <ChromeIconButton
                            tone="power"
                            // Overridden reads as off (gray + fill) even if
                            // the stored setting is on.
                            on={normalizeOn && !normalizeOverridden}
                            help={HELP.blockNormalize}
                            onClick={handleToggleNormalize}
                            disabled={normalizeOverridden}
                            style={normalizeOverridden ? { pointerEvents: 'none' } : undefined}
                          >
                            <Equal size={ICON_SIZE} />
                          </ChromeIconButton>
                        </span>
                      )}
                      <KnobControl
                        label="Out"
                        value={outputGain}
                        onChange={(val) => {
                          setOutputGain(val);
                          setParam('outputGain', val);
                        }}
                        onDragStateChange={handleKnobDragState}
                        size={KNOB_SIZE_SECONDARY}
                        labelBottom={false}
                        thumb="secondary"
                        scale={gainDbScale}
                        defaultValue={0.5}
                        help={
                          !isNam && (isCab || block.irCategory === 'cab')
                            ? HELP.blockOutIr
                            : HELP.blockOut
                        }
                      />
                    </div>
                  </div>
                )}
              </>
            )}
            {showInfo && infoLoading && <BusyOverlay align="center" />}
          </div>
        </div>
      </div>
    </div>
  );
};
