import React, { useState, useCallback, useEffect, useMemo, useRef } from 'react';
import { useNativeFunction } from '../hooks/useFunction';
import { useParameter } from '../hooks/useParameter';
import { useChainState } from '../hooks/useChainState';
import { ChainActionsProvider } from '../hooks/useChainActions';
import type { ChainActions } from '../hooks/useChainActions';
import { usePresets } from '../hooks/usePresets';
import { useAudioDevice } from '../hooks/useAudioDevice';
import { useConnectionGate } from '../hooks/useConnectionGate';
import { useToneSession } from '../hooks/useToneSession';
import { useToneLoadFlow } from '../hooks/useToneLoadFlow';
import { useUpdateNotice } from '../hooks/useUpdateNotice';
import { useUiScale, DESIGN_WIDTH, DESIGN_HEIGHT } from '../hooks/useUiScale';
import { shouldRestoreToneBrowser } from '../hooks/useT3kSelect';
import { CHAIN_SCROLL_STORAGE_KEY, ChainView, DETAIL_BLOCK_STORAGE_KEY } from './ChainView';
import { Faceplate, PLATE_HEIGHT } from './Faceplate';
import { HintBar, HINT_HEIGHT } from './HintBar';
import { ResizeGrip } from './ResizeGrip';
import { ToastProvider } from './Toast';
import { MidiLearnProvider } from '../hooks/useMidiLearn';
import { PluginHeader } from './PluginHeader';
import { useHintsEnabled } from './helpText';
import { AppBanner, useAppBanner, type BannerAction } from './AppBanner';
import { useChromeChoreography, BANNER_ANIM_MS } from '../hooks/useChromeChoreography';
import { DbMeter } from './DbMeter';
import { TunerView } from './TunerView';
import { SceneManager } from './SceneManager';
import { OAuthOverlay } from './OAuthOverlay';
import { ConnectionModal } from './ConnectionModal';
import { ToneBrowser } from './ToneBrowser';
import { UpdateNotice } from './UpdateNotice';
import Settings, { type SettingsTab } from './Settings';
import { T3K_API } from '../t3k/config';
import type { Model } from '../types/tone';
import type { ToneBlock } from '../types/chain';
import { NUM_SCENES, isInsertSlot } from '../types/chain';
import { useGlobalShortcuts } from '../hooks/useGlobalShortcuts';
import { useKeyboardShortcutsEnabled } from './uiPreferences';

export const Plugin: React.FC = () => {
  const [showSettings, setShowSettings] = useState(false);
  // Which tab Settings opens on; banner / gear land on System (setup first).
  const settingsTabRef = useRef<SettingsTab>('system');
  const [showTuner, setShowTuner] = useState(false);
  const [showSceneManager, setShowSceneManager] = useState(false);
  // Global Bypass dims the chain and faceplate (opacity only - still
  // editable) so a bypassed plugin reads as "off" at a glance, same visual
  // language as a powered-off block. Meters stay bright: they show the dry
  // signal actually leaving the plugin.
  const [bypassed] = useParameter('bypass', 'toggle');
  const bypassDim: React.CSSProperties = {
    opacity: bypassed ? 0.45 : 1,
    transition: 'opacity 0.2s ease',
  };
  // In-plugin tone browser takeover (streams of TONE3000 tones). Opened by
  // the + when already authenticated, or right after the no-prompt login
  // flow returns. Seeded true when we're returning from a browse-intent
  // redirect without a picked tone (Browse closed/canceled) so the browser
  // is already mounted under the busy scrim; no flash of the main chain.
  const [showToneBrowser, setShowToneBrowser] = useState(shouldRestoreToneBrowser);
  // Block info view fills the center column to the header and faceplate so
  // scroll content isn't stopped by the 24px meter-band pads.
  const [fillToFaceplate, setFillToFaceplate] = useState(false);
  // Bumped on preset load so ChainView drops an open detail takeover (and a
  // remount after closing the tuner/browser doesn't restore it).
  const [returnToGallery, setReturnToGallery] = useState(0);

  // Chain state: revision-gated polling + mutation actions, owned by one hook.
  const {
    chain,
    canUndo,
    canRedo,
    canPaste,
    canPasteEq,
    scenes,
    atDefault,
    activePreset,
    stereoInput,
    stereoOutput,
    inputMode,
    namSlimSizeDefault,
    multiCore,
    standalone,
    sampleRate,
    refresh,
    actions,
  } = useChainState();

  // Audio device state (standalone only): shared by the System Settings tab
  // and the app banner so both read the same snapshot.
  const audioDevice = useAudioDevice(standalone);

  // Internal presets. Mutations resync the chain state immediately (loading a
  // preset replaces the chain; saving/renaming changes the active preset).
  const presetStore = usePresets(refresh);

  // The output carries a real stereo image only when a Dual Mono block
  // widens it (see hasWideningDualMono below) AND the rig can reproduce it
  // (stereoOutput: stereo host bus / 2+ channel output device); drives the
  // output meter's stereo form.
  // A Dual Mono block widens to independent L/R purely off buffer channel
  // count (see runDualMono, Processor.cpp). Conservative on purpose: any
  // Dual Mono block in the chain counts, not only once its two sides
  // actually diverge - showing L/R needles that happen to move together is
  // harmless, hiding a channel that can carry real content isn't.
  const hasWideningDualMono = chain.some(
    (item) => !isInsertSlot(item) && item.blockType === 'dualMono'
  );
  const stereoImage = hasWideningDualMono && stereoOutput;

  const setTunerEnabled = useNativeFunction<boolean>('setTunerEnabled');
  const copyToClipboard = useNativeFunction<boolean>('copyToClipboard');
  const setExtraContentHeight = useNativeFunction<boolean>('setExtraContentHeight');
  const pickLocalToneFile = useNativeFunction<{
    blockId?: string;
    error?: string;
    cancelled?: boolean;
  }>('pickLocalToneFile');

  const openSettings = useCallback((tab: SettingsTab) => {
    settingsTabRef.current = tab;
    setShowSettings(true);
  }, []);
  const openDefaultSettings = useCallback(() => openSettings('system'), [openSettings]);

  // App banner: one priority-picked banner over the audio device state
  // (standalone only). Both the banner (top) and the hint bar (bottom) are
  // chrome strips that grow the window rather than squish the 578px core.
  const { banner, dismiss: dismissBanner } = useAppBanner(standalone ? audioDevice.state : null);
  // Whole-UI proportional scaling: keeps the root font-size at the current
  // scale so the rem-denominated design space below tracks the window.
  useUiScale();
  const hintsVisible = useHintsEnabled();
  // Chrome choreography: reports the strip heights to native (before paint)
  // and sequences the banner mount against the window resize so existing
  // content never jumps; see useChromeChoreography for the phase machine.
  const chrome = useChromeChoreography(banner, hintsVisible, setExtraContentHeight);

  const handleBannerAction = useCallback(
    (kind: BannerAction) => {
      if (kind === 'openSettings') openSettings('system');
      else if (kind === 'switchToAsio') audioDevice.actions.setDeviceType('ASIO');
      else if (kind === 'openMicSettings') audioDevice.actions.openMicSettings();
    },
    [audioDevice.actions, openSettings]
  );

  // Native pitch detection runs continuously (the always-on MiniTuner in the
  // header needs a reading regardless of whether the full screen is open),
  // enabled once at startup rather than toggled with the screen.
  useEffect(() => {
    void setTunerEnabled(true);
  }, [setTunerEnabled]);

  // Toggle the full tuner screen; detection itself is always on (see above).
  const handleToggleTuner = useCallback((show: boolean) => {
    setShowTuner(show);
    if (show) setShowSceneManager(false);
  }, []);
  // Scene Manager takeover (same middle-band slot as the tuner).
  const toggleSceneManager = useCallback(() => {
    setShowTuner(false);
    setBlockFromSceneManager(false);
    setShowSceneManager((open) => !open);
  }, []);
  const closeSceneManager = useCallback(() => setShowSceneManager(false), []);
  // Header click in the manager: leave it with that block's view open
  // (ChainView reopens the persisted detail block when it remounts).
  // Its Back arrow then returns to the manager (Home still goes to the
  // gallery and drops that origin).
  const [blockFromSceneManager, setBlockFromSceneManager] = useState(false);
  const openBlockFromSceneManager = useCallback((blockId: string) => {
    sessionStorage.setItem(DETAIL_BLOCK_STORAGE_KEY, blockId);
    setBlockFromSceneManager(true);
    setShowSceneManager(false);
  }, []);
  const backToSceneManager = useCallback(() => {
    // ChainView unmounts in the same render, before its own effect could
    // drop the persisted detail block: drop it here, or closing the manager
    // would land back on the block.
    sessionStorage.removeItem(DETAIL_BLOCK_STORAGE_KEY);
    setBlockFromSceneManager(false);
    setShowSceneManager(true);
  }, []);
  const clearBlockOrigin = useCallback(() => setBlockFromSceneManager(false), []);
  const closeTuner = useCallback(() => handleToggleTuner(false), [handleToggleTuner]);

  // Top-bar actions whose effect lands on the main screen (undo/redo,
  // loading or saving a preset) leave the tuner first, so the result is
  // visible instead of hidden behind the tuner takeover.
  const closeTunerThen = useCallback(
    <A extends unknown[], R>(fn: (...args: A) => R) =>
      (...args: A): R => {
        if (showTuner) void handleToggleTuner(false);
        return fn(...args);
      },
    [showTuner, handleToggleTuner]
  );
  const handleUndo = useMemo(() => closeTunerThen(actions.undo), [closeTunerThen, actions]);
  const handleRedo = useMemo(() => closeTunerThen(actions.redo), [closeTunerThen, actions]);

  // Share: copy the tone's public TONE3000 page URL, the API's canonical
  // `url` (title slug + id). The plain id path is a fallback for summaries
  // that predate it. Clipboard writes go through native (webview clipboard
  // APIs are unreliable in JUCE), with the browser API as a dev-server
  // fallback.
  const handleShareBlock = useCallback(
    async (block: ToneBlock): Promise<boolean> => {
      const url = block.tone.url ?? `${T3K_API}/tones/${block.tone.id}`;
      const ok = await copyToClipboard(url);
      if (ok) return true;
      try {
        await navigator.clipboard.writeText(url);
        return true;
      } catch {
        return false;
      }
    },
    [copyToClipboard]
  );

  // First line of defence for network-dependent actions: an instant
  // `navigator.onLine` check (no probe, no latency at click time). Actions
  // are never blocked beyond that: a throttled background probe verifies
  // HTTPS to TONE3000 on the side and, only after a confirming re-probe,
  // explains a broken-TLS environment (wrong system clock, intercepting
  // proxy) that would otherwise strand OAuth on a dead page. Failures still
  // land on the recovery paths (failed-navigation recovery, stream retry,
  // block retry).
  const connectionGate = useConnectionGate();
  const { requireConnection } = connectionGate;

  // The add/swap browse flows and their pending targets.
  const loadFlow = useToneLoadFlow({
    actions,
    requireConnection,
    setShowToneBrowser,
  });

  // Loading a preset or resetting to default replaces the chain. Leave any
  // takeover (tuner, tone browser, block detail) first so the new chain is
  // visible on the gallery, matching closeTunerThen.
  const showChainThen = useCallback(
    <A extends unknown[], R>(fn: (...args: A) => R) =>
      (...args: A): R => {
        if (showTuner) void handleToggleTuner(false);
        setShowSceneManager(false);
        setBlockFromSceneManager(false);
        if (showToneBrowser) {
          loadFlow.clearPendingTargets();
          setShowToneBrowser(false);
        }
        sessionStorage.removeItem(DETAIL_BLOCK_STORAGE_KEY);
        sessionStorage.removeItem(CHAIN_SCROLL_STORAGE_KEY);
        setReturnToGallery((n) => n + 1);
        return fn(...args);
      },
    [showTuner, handleToggleTuner, showToneBrowser, loadFlow]
  );

  const handleReset = useMemo(
    () => showChainThen(actions.resetToDefault),
    [showChainThen, actions]
  );

  // Rename/delete/move only touch the preset list, so they pass through.
  const headerPresetStore = useMemo(
    () => ({
      ...presetStore,
      actions: {
        ...presetStore.actions,
        save: closeTunerThen(presetStore.actions.save),
        load: showChainThen(presetStore.actions.load),
      },
    }),
    [presetStore, closeTunerThen, showChainThen]
  );

  // Global keyboard shortcuts (layer 3, see useGlobalShortcuts).
  const shortcutsEnabled = useKeyboardShortcutsEnabled();
  useGlobalShortcuts(shortcutsEnabled, {
    undo: handleUndo,
    redo: handleRedo,
    presetStep: (direction) => {
      const list = headerPresetStore.presets;
      if (list.length === 0) return;
      const index = activePreset ? list.findIndex((p) => p.id === activePreset.id) : -1;
      const next =
        index < 0
          ? direction === 1
            ? 0
            : list.length - 1
          : (index + direction + list.length) % list.length;
      void headerPresetStore.actions.load(list[next].id);
    },
    selectScene: (index) => void actions.selectScene(index),
    sceneStep: (direction) =>
      void actions.selectScene((scenes.active + direction + NUM_SCENES) % NUM_SCENES),
  });

  const openToneBrowser = useCallback(() => setShowToneBrowser(true), []);

  // TONE3000 session: API client, OAuth flows, signed-in identity, and
  // native's copy of the access token.
  const session = useToneSession({
    onToneSelected: loadFlow.handleToneSelected,
    onAuthenticated: openToneBrowser,
  });
  const { client: t3kClient, ensureNativeAuth, startLoginFlow, startSelectFlow } = session;

  const handleLogin = useCallback(
    () => requireConnection(() => startLoginFlow()),
    [requireConnection, startLoginFlow]
  );
  // Sign-in CTAs inside the browser (gated streams / Trending's discovery
  // footer) run the no-prompt login flow and return to this same browser,
  // never the full Select catalog.
  const handleBrowserSignIn = useCallback(
    () => requireConnection(() => startLoginFlow({ openBrowser: true })),
    [requireConnection, startLoginFlow]
  );
  // Browse on TONE3000 leaves for the Select OAuth catalog, so it takes the
  // same gate as login.
  const handleBrowseTone3000 = useCallback(
    () => requireConnection(() => startSelectFlow()),
    [requireConnection, startSelectFlow]
  );

  const handleLogout = useCallback(async () => {
    loadFlow.clearPendingTargets();
    setShowToneBrowser(false);
    await session.logout();
  }, [loadFlow, session]);

  // Closing without picking abandons any pending swap/insert target.
  const handleBrowserClose = useCallback(() => {
    loadFlow.clearPendingTargets();
    setShowToneBrowser(false);
  }, [loadFlow]);

  // Switch a block's model. Native downloads the new model file itself, so
  // refresh-and-sync the token first; switching after the editor has been
  // sitting idle is exactly when the last-pushed token has expired. Local
  // (drop-loaded) models switch from the on-disk stash instead: no download,
  // no token, works signed out.
  const handleSwitchModel = useCallback(
    async (blockId: string, modelId: number, model: Pick<Model, 'id' | 'name' | 'model_url'>) => {
      if (!model.model_url.startsWith('file:')) {
        try {
          await ensureNativeAuth();
        } catch (err) {
          // Refresh token rejected: tokens are cleared, the model select
          // disables itself on the next render, and the next + re-authenticates.
          console.error('Cannot switch model: TONE3000 session expired', err);
          return;
        }
      }
      const success = await actions.switchModel(blockId, modelId, JSON.stringify(model));
      if (!success) console.error('Failed to switch model');
    },
    [actions, ensureNativeAuth]
  );

  // Retry a failed model download. Refresh the token first when signed in
  // (the failure may have left the block waiting long enough for the last
  // pushed token to expire); signed out we retry anyway, since public model
  // URLs still work anonymously.
  const handleRetryLoad = useCallback(
    async (blockId: string) => {
      if (t3kClient.isAuthenticated()) {
        try {
          await ensureNativeAuth();
        } catch {
          // Session expired; the retry below still runs and native falls
          // back to whatever token it holds.
        }
      }
      await actions.retryModelLoad(blockId);
    },
    [actions, ensureNativeAuth, t3kClient]
  );

  // Menu-driven local load (the tiles' Load File / Load Folder): native owns
  // the whole flow (OS picker, validation, stash, load) and resolves when the
  // dialog closes. This is the route that works on Linux, where OS file drags
  // never reach the embedded webview and handleDropFile can't fire. Resync
  // after: the load lands outside useChainState's mutation wrapper.
  const handlePickLocalFile = useCallback(
    async (targetBlockId: string, kind: 'file' | 'folder', category?: 'ir' | 'cab') => {
      const res = await pickLocalToneFile(kind === 'folder', targetBlockId, category ?? '');
      await refresh();
      if (res?.blockId || res?.cancelled) return null;
      return res?.error ?? "Couldn't load the file";
    },
    [pickLocalToneFile, refresh]
  );

  // Non-blocking update check (enabled via VITE_T3K_UPDATE_NOTICE); also
  // resolves the running build's version for the Settings footer.
  const {
    notice: updateNotice,
    update,
    localVersion,
    forkVersion,
    remindLater,
  } = useUpdateNotice(t3kClient);

  // Auth-dependent block actions (model switching) key off this. Reading
  // localStorage per render is fine: every login/logout transition already
  // re-renders Plugin (user / oauthPhase state), refreshing the value.
  const authenticated = t3kClient.isAuthenticated();

  // Single stable bundle of everything a block can do. ChainView and the
  // tiles/cards below it read this from context instead of threading a dozen
  // callback props (which would defeat their React.memo).
  const chainActions = useMemo<ChainActions>(
    () => ({
      addModel: loadFlow.handleAddModel,
      loadLocalFile: loadFlow.handleDropFile,
      pickLocalFile: handlePickLocalFile,
      addEqBlock: actions.addEqBlock,
      addDualMonoBlock: actions.addDualMonoBlock,
      addToDualSlot: loadFlow.handleAddToDualSlot,
      removeDualSlotContent: actions.removeDualSlotContent,
      copyDualSlotFromSibling: actions.copyDualSlotFromSibling,
      convertBlockToDualMono: actions.convertBlockToDualMono,
      collapseDualMonoToSingle: actions.collapseDualMonoToSingle,
      setDualImage: actions.setDualImage,
      setDualLinked: actions.setDualLinked,
      setDualSolo: actions.setDualSolo,
      setDualMuted: actions.setDualMuted,
      setDualInvert: actions.setDualInvert,
      setDualAlign: actions.setDualAlign,
      setDualStereoProcessingEnabled: actions.setDualStereoProcessingEnabled,
      removeBlock: actions.removeBlock,
      swapBlock: loadFlow.handleSwapBlock,
      shareBlock: handleShareBlock,
      reorderBlocks: actions.reorderBlocks,
      moveBlock: actions.moveBlockToChain,
      duplicateBlock: actions.duplicateBlock,
      copyBlock: actions.copyBlock,
      pasteBlock: actions.pasteBlock,
      addInsertSlot: actions.addInsertSlot,
      removeInsertSlot: actions.removeInsertSlot,
      switchModel: handleSwitchModel,
      retryLoad: handleRetryLoad,
      listToneModels: session.listToneModels,
      getTone: session.getTone,
      setToneFavorite: session.setToneFavorite,
      refreshToneMetadata: actions.refreshToneMetadata,
      setBlockParam: actions.setBlockParam,
      setBlockSlimSize: actions.setBlockSlimSize,
      setBlockIrDecay: actions.setBlockIrDecay,
      setBlockIrSize: actions.setBlockIrSize,
      setBlockIrWidth: actions.setBlockIrWidth,
      setBlockIrTrimInit: actions.setBlockIrTrimInit,
      setBlockIrReverse: actions.setBlockIrReverse,
      resetBlockIrShape: actions.resetBlockIrShape,
      setBlockIrCategory: actions.setBlockIrCategory,
      convertBlockType: actions.convertBlockType,
      setBlockEqBand: actions.setBlockEqBand,
      setBlockEqEnabled: actions.setBlockEqEnabled,
      setBlockEqPre: actions.setBlockEqPre,
      resetBlockEq: actions.resetBlockEq,
      copyBlockEq: actions.copyBlockEq,
      selectBlockChannel: actions.selectBlockChannel,
      copyBlockChannel: actions.copyBlockChannel,
      pasteBlockEq: actions.pasteBlockEq,
      canPasteEq,
      authenticated,
      login: handleLogin,
    }),
    [
      actions,
      authenticated,
      canPasteEq,
      handleLogin,
      handlePickLocalFile,
      handleRetryLoad,
      handleShareBlock,
      handleSwitchModel,
      loadFlow.handleAddModel,
      loadFlow.handleAddToDualSlot,
      loadFlow.handleDropFile,
      loadFlow.handleSwapBlock,
      session.getTone,
      session.setToneFavorite,
      session.listToneModels,
    ]
  );

  return (
    <div
      style={{
        position: 'relative',
        // Explicit design-space box: rem lengths track the root font-size
        // (useUiScale), so this and every dimension inside scale together.
        width: `${DESIGN_WIDTH}rem`,
        // The window grows by the chrome-strip height (see useChromeChoreography),
        // so the 578px core UI between them keeps its full space.
        // (Figma's 600 includes a 22px mock OS title bar outside JUCE setSize.)
        height: `${DESIGN_HEIGHT + chrome.rootExtraHeight}rem`,
        // While the banner slides, the root and the banner wrapper animate
        // height with the same curve, so the flex middle (root minus fixed
        // strips) stays exactly constant and nothing inside moves.
        transition: chrome.animating ? `height ${BANNER_ANIM_MS}ms ease` : undefined,
        display: 'flex',
        flexDirection: 'column',
        backgroundColor: '#000000',
        boxSizing: 'border-box',
        overflow: 'hidden',
        color: '#ffffff',
      }}
    >
      {/* One app-wide toast pill, floating above the faceplate. Everything
          that raises toasts (preset save, share, auto measure) is inside. */}
      <ToastProvider bottom={PLATE_HEIGHT + (hintsVisible ? HINT_HEIGHT : 0) + 24}>
        {/* Right-click MIDI Learn on any mappable control (useMidiMenuItems). */}
        <MidiLearnProvider>
          {chrome.renderedBanner && (
            // Slide slot: the banner is anchored to the slot's bottom edge, so
            // opening/closing the slot slides it down/up from behind the top
            // edge. The window has already grown before the slide starts.
            <div
              style={{
                height: `${chrome.bannerSlotHeight}rem`,
                overflow: 'hidden',
                flexShrink: 0,
                display: 'flex',
                flexDirection: 'column',
                justifyContent: 'flex-end',
                transition: chrome.animating ? `height ${BANNER_ANIM_MS}ms ease` : undefined,
              }}
            >
              <AppBanner
                banner={chrome.renderedBanner}
                onAction={handleBannerAction}
                onDismiss={dismissBanner}
              />
            </div>
          )}

          <PluginHeader
            presetStore={headerPresetStore}
            activePreset={activePreset}
            atDefault={atDefault}
            onReset={handleReset}
            showTuner={showTuner}
            onToggleTuner={handleToggleTuner}
            canUndo={canUndo}
            canRedo={canRedo}
            onUndo={handleUndo}
            onRedo={handleRedo}
            user={session.user}
            authenticated={authenticated}
            onOpenSettings={openDefaultSettings}
            onLogin={handleLogin}
            onLogout={handleLogout}
          />

          {/* Middle Section: Tuner (when toggled on) or Meters + Chain View.
          Horizontal inset is on this band; vertical inset lives only on the
          center column so meters always center in the full header-to-faceplate
          height (never shift when Select opens). Select drops the center's
          bottom pad and uses its own scroll padding instead. */}
          {showTuner ? (
            <TunerView onClose={closeTuner} />
          ) : showSceneManager ? (
            <SceneManager
              chain={chain}
              scenes={scenes}
              actions={actions}
              onClose={closeSceneManager}
              onOpenBlock={openBlockFromSceneManager}
            />
          ) : (
            <div
              style={{
                display: 'flex',
                flexDirection: 'row',
                flex: 1,
                width: '100%',
                backgroundColor: '#000000',
                overflow: 'hidden',
                minHeight: 0,
                padding: '0 24rem',
                boxSizing: 'border-box',
              }}
            >
              <div
                style={{
                  height: '100%',
                  display: 'flex',
                  alignItems: 'center',
                  justifyContent: 'center',
                  flexShrink: 0,
                  backgroundColor: '#000000',
                  // Above the Select Tone header scrim, so stereo columns that
                  // overflow this slot into the center aren't covered by it.
                  position: 'relative',
                  zIndex: 3,
                }}
              >
                {/* 358 matches Figma's BLOCK column (title + gap + card). */}
                <DbMeter type="input" stereo={stereoInput && inputMode === 'stereo'} height={358} />
              </div>

              {/* Center: the chain gallery, or the tone browser takeover. */}
              <div
                style={{
                  flex: 1,
                  height: '100%',
                  overflow: 'hidden',
                  minHeight: 0,
                  minWidth: 0,
                  boxSizing: 'border-box',
                  // Shared 24px under the header; 24px above the faceplate only
                  // for chain/BLOCK. Select fills to the faceplate; block-info
                  // fills to both header and faceplate, putting those pads
                  // inside the scroll content instead.
                  paddingTop: fillToFaceplate ? 0 : 24,
                  paddingBottom: showToneBrowser || fillToFaceplate ? 0 : 24,
                  ...bypassDim,
                }}
              >
                {showToneBrowser ? (
                  <ToneBrowser
                    client={t3kClient}
                    // Pre-mounted during an OAuth return ('returning'), the client
                    // has no tokens until the callback's code exchange finishes;
                    // hold the stream fetch so it doesn't fire unauthenticated.
                    authPending={session.oauthPhase === 'returning'}
                    authenticated={authenticated}
                    onPickTone={session.selectToneById}
                    onBrowseTone3000={handleBrowseTone3000}
                    onSignIn={handleBrowserSignIn}
                    onClose={handleBrowserClose}
                  />
                ) : (
                  <ChainActionsProvider value={chainActions}>
                    <ChainView
                      chain={chain}
                      canPaste={canPaste}
                      sampleRate={sampleRate}
                      namSlimSizeDefault={namSlimSizeDefault}
                      onFillToFaceplate={setFillToFaceplate}
                      returnToGallery={returnToGallery}
                      onBackToOrigin={blockFromSceneManager ? backToSceneManager : undefined}
                      onOriginCleared={clearBlockOrigin}
                    />
                  </ChainActionsProvider>
                )}
              </div>

              <div
                style={{
                  height: '100%',
                  display: 'flex',
                  alignItems: 'center',
                  justifyContent: 'center',
                  flexShrink: 0,
                  backgroundColor: '#000000',
                  // Above the Select Tone header scrim, so stereo columns that
                  // overflow this slot into the center aren't covered by it.
                  position: 'relative',
                  zIndex: 3,
                }}
              >
                <DbMeter type="output" stereo={stereoImage} height={358} labelsPosition="right" />
              </div>
            </div>
          )}

          {/* Pinned faceplate at the bottom (gains, gate, tone stack), with the
          hint strip under it (hidden entirely when hints are off). */}
          <div style={{ width: '100%', flexShrink: 0, ...bypassDim }}>
            <Faceplate
              stereoInput={stereoInput}
              inputMode={inputMode}
              onInputModeChange={actions.setInputMode}
              scenes={scenes}
              onSelectScene={actions.selectScene}
              onRenameScene={actions.renameScene}
              onCopyScene={actions.copyScene}
              onToggleSceneManager={toggleSceneManager}
              sceneManagerOpen={showSceneManager}
            />
          </div>
          <HintBar />
          <ResizeGrip />

          {/* Settings takeover, mounted only while open so its parameter
          subscriptions and screen state don't run behind the main UI. */}
          {showSettings && (
            <Settings
              onClose={() => setShowSettings(false)}
              standalone={standalone}
              device={audioDevice}
              initialTab={settingsTabRef.current}
              version={localVersion}
              forkVersion={forkVersion}
              update={update}
              namSlimSizeDefault={namSlimSizeDefault}
              onNamSlimSizeDefaultChange={actions.setNamSlimSizeDefault}
              multiCore={multiCore}
              onMultiCoreChange={actions.setMultiCore}
              chain={chain}
            />
          )}

          {/* OAuth callback overlay: covers the chain UI while we resolve the
          tokens + tone after returning from tone3000.com, and surfaces any
          OAuth error (callback failures, failed-navigation recovery) with a
          retry that restarts whichever flow actually failed. */}
          <OAuthOverlay
            phase={session.oauthPhase}
            error={session.oauthError}
            onRetry={session.retryFlow}
            onDismiss={session.clearOauthError}
          />

          {/* Connection gate for internet-dependent actions (add / swap /
          login / select). Offline gates instantly; TLS problems surface from
          a non-blocking background probe, only after confirmation. */}
          <ConnectionModal
            problem={connectionGate.problem}
            onRetry={connectionGate.retry}
            onDismiss={connectionGate.dismiss}
          />

          {/* Update available, below OAuth/connection (z 3000) so those always win. */}
          <UpdateNotice notice={updateNotice} onRemindLater={remindLater} />
        </MidiLearnProvider>
      </ToastProvider>
    </div>
  );
};
