# sandbox/experiments — build log

This is the running changelog for the `sandbox/experiments` branch, keyed to the
`SANDBOX_BUILD` counter at the repo root. That counter is compiled into the plugin
(`plugin/CMakeLists.txt` → `T3K_SANDBOX_BUILD` → the `getSandboxBuild` native function →
`useUpdateNotice.ts`) and shown faintly in Settings, under the real version line
(`sandbox build N`) — so a running build can always be pointed back at the entry here that
explains what's in it.

Bump it with `./script/bump-sandbox-build.sh "what's stable now"` whenever a piece of work
reaches a stable point worth marking — it increments `SANDBOX_BUILD` and appends a dated
entry below. It does not commit; review and commit both files alongside the actual work.

Neither file is meant to survive this branch merging back to `main` — the CMake read and the
native function are both guarded to no-op cleanly if `SANDBOX_BUILD` doesn't exist.

See also `SANDBOX_STATUS.md` — an earlier, one-off end-of-session checkpoint (2026-09-12) with
more narrative detail on that day's work and the branch's risk/maturity breakdown as of then;
this file is the ongoing version of that idea.

## History before this log existed (2026-09-03 → 2026-09-17)

`sandbox/experiments` branched off `main` on 2026-09-03. The iOS enablement work in that
period (touch adaptation, App Store packaging, Bluetooth rate handling, CI) came in through
merged PRs from other contributors and isn't detailed here — this section covers the
feature work built in this collaboration, roughly chronological:

- **EQ fixes**: POST-block EQ now applies to the wet signal before the dry/wet mix (not
  after); Out Gain applies after the dry/wet mix, not to the wet term alone; a **FLAT**
  button resets a block's EQ bands to default in one action.
- **IR Predelay**: a `BlockPredelay` DSP stage + knob on IR blocks, with dedicated
  wet-only-isolation and delay-timing tests.
- **IR envelope shaping**: a 2-segment Attack/Decay curve (DSP + the interactive
  `IrEnvelopeGraph` editor, `WaveformDisplay`, precision-entry chips), through several
  real polish passes — curve reshaping for acoustic material, readout-clipping fixes,
  double-click reset.
- **ChainMapStrip navigation**: replaced the old block-detail Prev/Next chevrons with a
  persistent chain-position strip in the header, plus several iteration passes (chip
  sizing/centering, active-chip color, scroll-restore interactions).
- **Cab Block v1**: a real, separate `ChainBlockType::CAB` (not just IR-with-a-flag),
  single-slot at first.
- **2026-09-12 checkpoint** (see `SANDBOX_STATUS.md` for the full write-up): restored the
  IR card's creator/downloads/favorites readout and fixed the info-panel image; fixed two
  distinct causes of the chain gallery scrolling to the far left on add (a chain-state race
  on ChainMapStrip's own "+", and a `useEffect`-cleanup-runs-after-ref-detach ordering bug
  on the gallery's "+"); fixed gallery-tile swap forcing its way into the block's detail
  view.
- **IR shaping suite**: manual Trim Init (leading-silence trim, detection automatic, apply
  opt-in), later given a relaxed **Off/Std/Lax** threshold (Lax at −45 dB instead of −60);
  a Reverse toggle; a Reset button (next to EQ) that reverts all IR shaping params as one
  undo step; the interactive shape graph updated to reflect Trim/Reverse; a fix for
  Size/Width/envelope/Trim Init not reapplying correctly across a model swap; the IR Size
  effective-length cap lowered from 30 s to 20 s (30 s still crackled in real-world use);
  IR Size given self-loudness compensation (`sqrt(durationRatio)`, capped by the existing
  anti-clip ceiling — full parity only when the content has headroom, a deliberate
  trade-off, not a bug).
- **Local file routing**: dropping a local file on an empty/occupied gallery tile now
  splits into IR/Cab drop zones instead of always landing IR-then-guessing; this surfaced
  and fixed three related bugs in `swapTone` (stale `irCategory`, stale
  `applyDefaultMixOnLoad`, and an empty-string `forceGear` collapsing back into the
  duration guess) and gave local Cab/IR drops their own gallery-tile glyph instead of a
  generic file icon.
- **Mix defaults**: IR Player's default Mix is 25% (was 50%); Cab stays 100%.
- **ChainMapStrip chips**: an EQ shortcut, drag-to-reorder, and a bypass toggle added
  directly to the chip strip.
- **CAB/IR Player conversion**: a header toggle to convert a block between Cab and IR
  Player in place; removed the legacy Pan knob (superseded by Balance/pan elsewhere in the
  chain).
- **Tile menu goes two-level**: the right-click menu (gallery "+" tiles, and later
  replicated onto ChainMapStrip's own "+" chip) restructured so each block type gets its
  own row, with Cab/IR flying out **Load File / Load Folder** — extensible for future
  block types instead of one flat list. A `forceGear` override threaded from those rows
  down to `finishLocalToneLoad` so an explicit menu choice always wins over the
  content-duration Cab/IR guess.
- **Standalone EQ block**: a new `ChainBlockType::EQ` — no model, always 100% wet, no In
  Gain, just the existing per-block 6-band EQ with an Out knob — insertable anywhere via
  the tile menu's new "EQ" row. Also given a hover-strip quick-access button on gallery
  tiles.

That last item shipped same-day as this log's first entry below, including a layout fix
(the EQ block's graph/sliders view was collapsing to a thin strip at the top of the card —
root cause was a wrapper `<div>` around `<BlockEqView>` that wasn't itself `display: flex`,
so `BlockEqView`'s own `flex: 1` sizing had nothing to fill).

## Build log

### Build 1 — 2026-09-17 — a00decc

Everything above through `a00decc` (tile menu submenu, ChainMapStrip chip menu,
`forceGear` local-load plumbing, the standalone EQ block, and its layout fix) is the
stable point this counter starts from. This entry also introduces the counter mechanism
itself: `SANDBOX_BUILD` (repo root), the CMake read + `T3K_SANDBOX_BUILD` compile
definition (`plugin/CMakeLists.txt`), the `getSandboxBuild` native function
(`EditorWebViewSetup.cpp`), the Settings-footer display (`Settings.tsx`, wired through
`useUpdateNotice.ts` / `Plugin.tsx`), and `script/bump-sandbox-build.sh` for future
checkpoints.
