import React from 'react';
import { Undo2, Redo2, Power, VolumeX } from './icons';
import { AccountMenu } from './AccountMenu';
import { IconButton } from './IconButton';
import { chromeIcon } from './ChromeIconButton';
import { useParameter } from '../hooks/useParameter';
import { MiniTuner } from './MiniTuner';
import { PresetBar, PRESET_BAR_TRAILING_WIDTH } from './PresetBar';
import { HELP, helpProps } from './helpText';
import { BORDER, BRAND_RED, GRAY, HIGHLIGHT, MUTED, WHITE, iconButtonStyle } from './theme';
import type { usePresets } from '../hooks/usePresets';
import type { ActivePreset } from '../types/chain';
import type { User } from '../types/tone';

type PresetStore = ReturnType<typeof usePresets>;

/** Top-bar sized (28px box, 18px glyph) switch with explicit colors per
    state; see GlobalSwitches for the two looks. */
const GlobalSwitch: React.FC<{
  pressed: boolean;
  look: Pick<React.CSSProperties, 'color' | 'background'>;
  help: string;
  onClick: () => void;
  children: React.ReactNode;
}> = ({ pressed, look, help, onClick, children }) => (
  <button
    type="button"
    onClick={onClick}
    aria-pressed={pressed}
    {...helpProps(help)}
    style={{ ...iconButtonStyle(28), ...look }}
  >
    {chromeIcon(children, 18)}
  </button>
);

/** Global Bypass + Mute pair (APVTS bools `bypass` / `outputMute`, so they
    automate, MIDI-map and follow the host's own bypass button). Own
    component so their parameter subscriptions don't re-render the header.
    Bypass is a power switch with the app's usual power semantics - white =
    plugin running, HIGHLIGHT fill + gray = bypassed (Plugin.tsx also dims
    the chain and faceplate then). Mute follows mixer convention instead:
    lit red while muted. */
const GlobalSwitches: React.FC = () => {
  const [bypass, setBypass] = useParameter('bypass', 'toggle');
  const [mute, setMute] = useParameter('outputMute', 'toggle');
  return (
    <div style={{ display: 'flex', alignItems: 'center', gap: '16rem' }}>
      <GlobalSwitch
        pressed={bypass}
        look={bypass ? { color: GRAY, background: HIGHLIGHT } : { color: WHITE }}
        help={HELP.bypass}
        onClick={() => setBypass(!bypass)}
      >
        <Power size={18} />
      </GlobalSwitch>
      <GlobalSwitch
        pressed={mute}
        look={mute ? { color: WHITE, background: BRAND_RED } : { color: MUTED }}
        help={HELP.mute}
        onClick={() => setMute(!mute)}
      >
        <VolumeX size={18} />
      </GlobalSwitch>
    </div>
  );
};

interface PluginHeaderProps {
  presetStore: PresetStore;
  activePreset: ActivePreset | null;
  /** Greys out the preset bar's New button (see PresetBar). */
  atDefault: boolean;
  onReset: () => void;
  showTuner: boolean;
  onToggleTuner: (show: boolean) => void;
  canUndo: boolean;
  canRedo: boolean;
  onUndo: () => void;
  onRedo: () => void;
  user: User | null;
  authenticated: boolean;
  onOpenSettings: () => void;
  onLogin: () => void;
  onLogout: () => void;
}

/**
 * Full-width top bar: logo, preset controls, tuner, undo/redo and the
 * account menu. Memoized because Plugin re-renders on every chain
 * poll tick while nothing up here changes.
 */
export const PluginHeader = React.memo(function PluginHeader({
  presetStore,
  activePreset,
  atDefault,
  onReset,
  showTuner,
  onToggleTuner,
  canUndo,
  canRedo,
  onUndo,
  onRedo,
  user,
  authenticated,
  onOpenSettings,
  onLogin,
  onLogout,
}: PluginHeaderProps) {
  // Three columns, the outer two sharing the leftover space equally (same
  // trick as the faceplate): the preset pill lands dead-center on the window
  // no matter how wide either side's controls are. Each side then spreads
  // its own groups edge to edge (space-between), so the spare room becomes
  // even breathing space instead of piling up on one side.
  const sideColumn: React.CSSProperties = {
    display: 'flex',
    alignItems: 'center',
    justifyContent: 'space-between',
    minWidth: 0,
  };
  return (
    <div
      style={{
        width: '100%',
        height: '64rem',
        flexShrink: 0,
        display: 'grid',
        gridTemplateColumns: '1fr auto 1fr',
        columnGap: '36rem',
        alignItems: 'center',
        backgroundColor: '#000000',
        padding: '0 24rem',
        boxSizing: 'border-box',
        borderBottom: BORDER,
      }}
    >
      <div style={sideColumn}>
        <a
          href="https://www.tone3000.com"
          target="_blank"
          rel="noopener noreferrer"
          style={{ display: 'flex', alignItems: 'center', justifyContent: 'center', gap: '16rem' }}
        >
          <img src="/t3k.svg" alt="T3K" style={{ width: '160rem' }} />
        </a>
        <GlobalSwitches />
      </div>
      {/* Leading pad = the bar's trailing Save/New buttons, so the ‹ name ›
          pill itself (not pill + buttons) is what sits on the center line. */}
      <div style={{ paddingLeft: `${PRESET_BAR_TRAILING_WIDTH}rem` }}>
        <PresetBar
          active={activePreset}
          presets={presetStore.presets}
          atDefault={atDefault}
          onSave={presetStore.actions.save}
          onLoad={presetStore.actions.load}
          onRename={presetStore.actions.rename}
          onDelete={presetStore.actions.remove}
          onMove={presetStore.actions.move}
          onReset={onReset}
        />
      </div>
      <div style={sideColumn}>
        {/* Always-on readout, doubling as the full tuner's entry point. */}
        <MiniTuner open={showTuner} onClick={() => onToggleTuner(!showTuner)} />
        <div style={{ display: 'flex', alignItems: 'center', gap: '16rem' }}>
          <IconButton onClick={onUndo} disabled={!canUndo} help={HELP.undo} size={28}>
            <Undo2 size={18} />
          </IconButton>
          <IconButton onClick={onRedo} disabled={!canRedo} help={HELP.redo} size={28}>
            <Redo2 size={18} />
          </IconButton>
        </div>
        <AccountMenu
          user={user}
          authenticated={authenticated}
          onOpenSettings={onOpenSettings}
          onLogin={onLogin}
          onLogout={onLogout}
        />
      </div>
    </div>
  );
});
