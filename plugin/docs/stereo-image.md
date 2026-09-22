# Stereo image: Align (Dual Mono's per-block Stereo Processing)

Align is a Dual Mono block's own per-block corrective delay, on that block's
Stereo Processing screen, time-aligning its two children before they
recombine. It carries an advanced "deck" of sections (wobble, crossover,
diffusion, correlation meter) shared with `ImageDeck.h`'s primitives - the
same deck a now-removed global "Spread" doubler used to mount, which is why
these primitives are named generically rather than after either feature.

Implementation:

- Align: `plugin/include/StereoOffset.h` / `plugin/src/StereoOffset.cpp`
- Shared deck primitives: `plugin/include/ImageDeck.h`
- Tests: `test/src/stereo_offset_tests.cpp`

## Align

Align applies a short corrective delay (up to 24 ms, sub-sample precise) to
one side of a Dual Mono block, for children that land a few ms apart:
captures of the same performance, or NAM models / IRs with different
baked-in latency. The block's own auto-align control measures the lag
between its Left and Right children with an internal sweep and writes the
correction into the offset (see `AutoOffset.h`, and `armDualAutoAlign`/
`pollDualAutoAlign` in `Processor.cpp` for the per-block call site).

```
Left child out ----+
                    +--> corrective delay (up to 24 ms, sub-sample) --> Pan/Width recombine
Right child out ----+
```

On top of the corrective delay sits the shared deck, with two deliberate
differences from a generic doubler (rationale in `StereoOffset.h`):

- **No precedence trim.** These are two real signal paths; a corrective tool
  must not color levels (`StereoOffsetTest.DiffusePreservesMagnitudeWithNoTrim`).
- **The crossover splits both channels**, so both sides get the identical
  LR4 phase rotation and the inter-side alignment (the whole point of the
  engine) is preserved. Flip side: with the crossover on, lows are no longer
  corrected by the delay; it is a creative width control, not part of the
  corrective path.

Every Align transition (power, side swap, knob through center) glides the
delay to zero and blends the deck out first, so it passes through identity
and never clicks; the interpolator-tap clamp that protects the first samples
after a delay-line clear is documented in `StereoOffset.h`.

## Controls

Align's musical control lives on the Dual Mono block's Stereo Processing
screen, plus an advanced panel (right-click the group) with the shared deck
sections:

- **Offset** (bipolar, ±24 ms): the sign picks which channel is delayed, the
  magnitude sets the delay. The detent window is tight (~1 sample at
  48 kHz): auto-align corrections of a few samples must not vanish into the
  detent. Alt-click resets the offset and the whole deck.
- **Wobble** (0-100% + power): depth of the random-walk drift, up to ±1.2 ms
  around the dialed offset. That is roughly ±2-4 cents of continuous pitch
  wander (pitch shift is the derivative of delay time). Depth is absolute,
  not relative to the offset, so a small offset can still carry a full-depth
  wobble. The power switch folds to zero depth through the depth smoother,
  so it never clicks. The delay time is modulated by a random walk (white
  noise through two cascaded 0.3 Hz one-poles - one pole alone leaves
  audible noise variance above 20 Hz, which FMs the delayed channel into
  broadband fizz, pinned by `StereoOffsetTest.WobbleAddsNoBroadbandFizz`).
  Interpolation is 4-point Lagrange because linear interpolation under a
  time-varying fractional offset low-passes in rhythm with the wobble.
- **Crossover** (32.5-520 Hz + power, default 130 Hz): the cutoff below
  which lows skip the deck. Off treats the full band.
- **Diffuse** (power only): six first-order allpasses with fixed, log-spaced
  corner frequencies decorrelate phase without touching magnitude. Off
  leaves the delayed side a pure delay: more coherent, more comb-like on a
  mono sum. The coefficients are static on purpose: movement comes from the
  delay wobble, and modulating allpass coefficients would reintroduce
  phasiness. The approach follows the allpass decorrelators evaluated in
  O. Das, "An Open-Source Stereo Widening Plugin", Proc. 27th Int. Conf. on
  Digital Audio Effects (DAFx24), Guildford, UK, 2024
  ([paper](https://www.dafx.de/paper-archive/2024/papers/DAFx24_paper_92.pdf)).

Align's default is **off**: it stays a pure corrective tool until the deck
is asked for, at which point the delayed side reads as an ADT-style second
take.

The section switches are ~25 ms blends rather than hard toggles (both
endpoints are magnitude-flat but differ in phase, so an instant switch would
step the waveform), and the bypassed filters keep running so re-engaging
lands on warm state.

## Mono outputs

A rig that can't reproduce stereo (a mono host bus, or a standalone output
device with a single channel; the latter still hands the processor a stereo
buffer but plays only channel 0) is reported to the UI as the `stereoOutput`
capability flag in `getChainState` (the output-side twin of `stereoInput`).
The chain's own output Pan knob (`outputPan`) stays identity on such a rig
regardless of its dialed value, pinned by
`ProcessorTest.PanStaysIdentityWithoutAStereoOutput`.

**A Dual Mono block** responds instead to how many physical channels its own
buffer carries (`runDualMono` in `Processor.cpp`): with two, each side keeps
its own output channel (Pan/Width); pinned to one, Pan/Width go inert and
the two sides average via a true `½(l + r)` fold - Align keeps running
either way, since the two sides still need to line up before they combine.
Pinned by `DualMonoBlockTest.WidenCaseGivesEachSideItsOwnOutputChannel` and
the `...Fold...` tests in the same file.

## Mono safety

Align's panel carries a correlation LED fed by a ~300 ms running normalized
L/R cross-correlation of the block's own output (`DeckCorrelation`,
published through an atomic for the UI). Below 0.5 a mono fold-down audibly
thins; below 0 it actively cancels. The wobble also keeps the mono-sum comb
moving, so fold-down reads as gentle chorus rather than a stationary notch.

## Threading

Align runs `process()` on the audio thread only and allocates nothing after
`prepare()`. The correlation atomic is the single cross-thread output.
