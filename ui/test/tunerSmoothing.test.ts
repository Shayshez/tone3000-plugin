/**
 * The tuner's display smoothing: attack gate, outlier median, glide, snap.
 *
 *   node --test ui/test/
 */
import test from 'node:test';
import assert from 'node:assert/strict';

import { ATTACK_MS, TunerSmoother } from '../src/types/tunerSmoothing.ts';

const E2 = 40;
const cents = (c: number) => c / 100;

/** Feeds readings every 50 ms from t0; returns [t, displayed pitch] pairs. */
const run = (s: TunerSmoother, t0: number, readings: [number, number | null][]) =>
  readings.map(([level, pitch], i) => {
    const t = t0 + i * 50;
    return [t, s.push(t, level, pitch)] as const;
  });

test('pluck attack: the sharp transient never reaches the display', () => {
  const s = new TunerSmoother();
  // Pluck from silence: level climbs, pitch starts 15 c sharp and settles
  // to +1 c within ~150 ms.
  const out = run(s, 0, [
    [-30, E2 + cents(15)],
    [-24, E2 + cents(10)],
    [-21, E2 + cents(4)],
    [-21, E2 + cents(1)],
    [-22, E2 + cents(1)],
    [-22, E2 + cents(1)],
    [-23, E2 + cents(1)],
  ]);
  for (const [t, p] of out) {
    if (p === null) continue;
    assert.ok(t >= ATTACK_MS, `displayed during the attack at ${t} ms`);
    assert.ok(Math.abs(p - E2) < cents(4), `showed ${((p - E2) * 100).toFixed(1)} c`);
  }
  assert.ok(out.at(-1)![1] !== null);
});

test('single-reading outlier is ignored while a note rings', () => {
  const s = new TunerSmoother();
  const steady: [number, number][] = Array.from({ length: 8 }, () => [-20, E2]);
  run(s, 0, steady);
  const out = run(s, 400, [
    [-20.5, E2 + cents(25)], // one glitch
    [-21, E2],
    [-21.5, E2],
  ]);
  for (const [, p] of out) assert.ok(Math.abs(p! - E2) < cents(1));
});

test('a slow tuning-peg move is followed', () => {
  const s = new TunerSmoother();
  let p = E2 - cents(20);
  const readings: [number, number][] = [];
  for (let i = 0; i < 40; i++) {
    readings.push([-20 - i * 0.2, p]);
    p = Math.min(E2, p + cents(1));
  }
  const out = run(s, 0, readings);
  assert.ok(Math.abs(out.at(-1)![1]! - E2) < cents(1));
});

test('new note snaps instead of sliding across the scale', () => {
  const s = new TunerSmoother();
  run(
    s,
    0,
    Array.from({ length: 8 }, () => [-20, E2] as [number, number])
  );
  // A string (MIDI 45) without a detected level jump (e.g. hammer-on).
  const out = run(s, 400, [
    [-21, 45],
    [-21, 45],
    [-21, 45],
  ]);
  assert.equal(out[0][1], E2); // held on the first disagreeing reading
  assert.equal(out[1][1], 45); // snapped once confirmed
});

test('after silence the previous note is not flashed', () => {
  const s = new TunerSmoother();
  run(
    s,
    0,
    Array.from({ length: 8 }, () => [-20, E2] as [number, number])
  );
  const out = run(s, 2000, [[-20, 45]]);
  assert.equal(out[0][1], null);
});
