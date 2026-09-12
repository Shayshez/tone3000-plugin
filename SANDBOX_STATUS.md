# sandbox/experiments — status checkpoint

Written at end of session on 2026-09-12. Branch head at time of writing:
**`a552e8bfc1a75ee125f6bc4286b558f63c8ccb24`**, pushed to `myfork/sandbox/experiments`
(`https://github.com/Shayshez/tone3000-plugin`). This file exists so a future session
(even with zero memory of this one) can get oriented in a few minutes.

## What shipped today

Three fixes, all in `ui/`, verified via `tsc -b` / `eslint` / `prettier --check` / the
full `test/` GoogleTest suite (161 tests) / a full 6-format Release build
(VST3, AU, AAX, CLAP, LV2, Standalone):

1. **`7201321` — Restored IR-card creator/counts, fixed info-panel image.**
   The compact IR block card (waveform-strip layout) had lost its downloads/
   favorites/creator readout in an earlier layout refactor (`d88e30d`). Restored it
   *inline* into the existing gear+badge row's unused horizontal width (not as a new
   row — an earlier attempt at this that added a row shrank the waveform display,
   which was explicitly called out as wrong and reverted). Also fixed the info panel
   ("i" button) showing the envelope/waveform graphic instead of the real TONE3000
   preview image — it now always shows the real image there, preferring the
   freshly-fetched `infoTone`'s image when available.

2. **`ef6f04f` — Fixed the gallery scrolling to the far left on add.**
   Two distinct bugs, both found via a **real execution trace** (not static
   reasoning — two earlier guesses were wrong and had to be discarded; see below).
   Root causes:
   - ChainMapStrip's "+" (add from within a block's detail view): a fresh
     `ChainView` mount landed with `detailBlockId` already set to the new block, but
     `chain`/`chainRight` props hadn't caught up yet (a separate `chainChanged`
     native round trip lags behind the `loadTone` call). The render still fell
     through to the gallery return in the meantime — a real, visible flash.
   - Gallery's own "+" tiles: opening the tone browser unmounts `ChainView`
     entirely, discarding its scroll container with nothing capturing the position
     first. **The actual root-cause mechanism, worth remembering**: a `useEffect`
     cleanup runs *after* React has already detached refs (called `ref(null)`)
     during the synchronous unmount commit — so reading `scrollLeft` from the ref in
     a `useEffect` cleanup is silently a no-op on unmount, every time. Fixed by
     capturing the value directly inside the ref callback's own `el === null`
     branch instead, which has no such ordering hazard.

3. **`a552e8b` — Fixed gallery-tile swap forcing its way into the block's detail
   view.** `swapBlock` had a hardcoded `OPEN_DETAIL_ON_SWAP = true` (from `#114`)
   assuming a swap always targets an already-open block. A trace showed the gallery
   tile's own swap action shares that same handler and was wrongly forcing
   navigation into detail. Gave `swapBlock` the same per-call-site
   `navigateToDetail` option `addModel` already had.

**Verification status, honestly stated:** #1 and the ChainMapStrip-add half of #2
were manually confirmed by the user in a running Standalone build. The gallery-"+"
half of #2 was implied-confirmed (the user's next message described swap as "another
instance of the same class of bug," i.e. a *different* bug from the one just fixed,
rather than a continuation of it) but never explicitly stated as verified. **#3 was
built, typechecked, linted, and passed the full test suite, but has *not yet* been
manually re-verified in a running app before this checkpoint — that's the first
thing to do on resume if picking this back up.**

Diagnostic `console.debug` logging added during the trace-based investigation
(forwarded to the native logger via the existing `[webview:log]` console shim,
readable live from `~/Library/Logs/TONE3000/TONE3000.log`) has been fully removed —
confirmed by grep, see the "How to re-run the same trace technique" note below.

## Current branch state — what's here and how safe it is to extract

`sandbox/experiments` is `29` commits ahead of `origin/main` (merge-base
`ce486d7`). Roughly, oldest to newest:

**Mature, low-risk, could be extracted/PR'd with little further work:**
- IR Predelay (`BlockPredelay` DSP stage + knob) — has dedicated isolation/timing
  tests.
- EQ "FLAT" reset button.
- Model/IR dropdown scroll-to-loaded-item fix.
- Out Gain post-dry/wet-mix bug fix.
- IR loudness/classification fix (`#89`) — explicit persisted `IrCategory` field
  (`Cab` / `IrPlayer`) replacing duration-based classification. This is the fix
  `#121` (below) tracks further work beyond.

**Real feature, reasonably mature, but larger surface / not yet reviewed upstream:**
- IR envelope shaping: 2-segment Attack/Decay curve DSP, the interactive
  `IrEnvelopeGraph` editor, `WaveformDisplay`, precision-entry `EditableChip`s. Went
  through several real polish passes (curve reshaping for acoustic material, readout
  clipping fixes, double-click reset) — feels solid, but sizable enough that it
  deserves its own review pass before extracting.

**Still explicitly experimental / early / in-flight:**
- **Cab Block v1** (`ChainBlockType::CAB`, commit `7c37aa2`) — a real, separate
  native block type (not just IR-with-a-flag), but single-slot only; the Pan knob
  is inert until a second slot exists (see `ChainBlock::cabPanNormalized`'s own
  comment). This is what tracking issue **`#121`** is for — read it before touching
  Cab further, it lays out the intended direction (see "Open threads" below).
- **ChainMapStrip navigation** (commits `ceab167` through `2ba93cf`, plus today's
  fixes on top) — replaces the old Prev/Next chevrons with a persistent
  chain-position strip in the block detail header. Several real iteration passes
  today (chip sizing/centering, active-chip color, scroll-restore interactions) —
  feels close to done, but **is explicitly pending a maintainer decision** before
  being extracted onto its own clean branch (see PR `#117` below). Don't assume
  it's ready to ship as-is; it hasn't had outside eyes on it yet.
- Today's three fixes (above) — solid trace-based root-cause work, but fresh
  (same-day) and only partially manually re-verified per the honest note above.

## Open threads

- **PR #117** — `https://github.com/tone-3000/tone3000-plugin/pull/117`
  ("Prev/Next navigation in expanded block view"). Status: **changes requested** by
  `woodybury` (rebase onto his scroll-position edits; hide the chevrons while
  viewing EQ). Instead of addressing those, a comment was posted proposing
  ChainMapStrip (this branch) as a different, more complete direction, and asking
  whether to extract it cleanly onto its own branch off `main` for a fresh PR, or
  leave #117 as-is with the requested chevron fixes. **Awaiting woodybury's
  response** — nothing to do here until he replies.
- **Issue #121** — `https://github.com/tone-3000/tone3000-plugin/issues/121`
  ("Split IR Block into a dedicated Cab Block") — the tracking issue for
  Cab-Block-beyond-`#89` work. Cab Block v1 on this branch is a first step toward
  it, not the full scope described in the issue (re-read the issue body for what's
  still intentionally deferred, e.g. multi-slot cab support).
- **Pending: extract ChainMapStrip onto a clean branch off `origin/main`.** Blocked
  on woodybury's response to the PR #117 comment above. When it's time to do this:
  the relevant commits are `ceab167`, `f05841a`, `1752506`, `9da0095`, `350e63f`,
  `2ba93cf`, plus (if the scroll-restore fixes are considered part of the same
  feature rather than general bugfixes worth their own PR) `ef6f04f` and `a552e8b`
  from today. `ChainMapStrip.tsx` is the main new file; the rest of the diff lives
  in `ChainBlock.tsx` (mounting it) and `ChainView.tsx` (scroll-restore
  interactions it exposed). Cherry-picking cleanly will take some care since later
  commits touch the same regions as earlier ones in the list.

## How to re-run the same trace technique, if a similar bug shows up again

Console output from the webview (`console.log`/`warn`/`error`/`debug`) is forwarded
to the native logger via a `console.*` shim injected at document start (see
`plugin/src/EditorWebViewSetup.cpp`'s `webLog` native function and the
`withUserScript` block around it), landing in the on-disk log as
`[webview:<level>] <message>` lines — this works in Release builds too, not just
Debug. To use it: add `console.debug(...)` calls at the points you need to trace,
rebuild just the Standalone target (`cmake --build build --target
TONE3000_Standalone`, much faster than the full 6-format build), clear
`~/Library/Logs/TONE3000/TONE3000.log`, relaunch, reproduce, then `grep
'\[webview:debug\]' ~/Library/Logs/TONE3000/TONE3000.log`. This is what actually
found both root causes today after two wrong static-reasoning guesses — worth
reaching for early next time rather than late.
