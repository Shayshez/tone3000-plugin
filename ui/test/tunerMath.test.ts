/**
 * The tuner's pitch -> note math (reference pitch + tuning offset).
 *
 *   node --test ui/test/
 */
import test from 'node:test';
import assert from 'node:assert/strict';

import { frequencyToNote } from '../src/types/tunerMath.ts';

const near = (actual: number, expected: number, tol = 0.01) =>
  assert.ok(Math.abs(actual - expected) < tol, `${actual} not within ${tol} of ${expected}`);

test('A4 = 440 reads A, 0 cents; standard low E is E2 (MIDI 40)', () => {
  const a = frequencyToNote(440, 440, 0);
  assert.equal(a.name, 'A');
  assert.equal(a.midi, 69);
  near(a.cents, 0);
  const lowE = frequencyToNote(82.4069, 440, 0);
  assert.equal(lowE.name, 'E');
  assert.equal(lowE.midi, 40);
  near(lowE.cents, 0, 0.1);
});

test('reference pitch moves the target: 432 Hz is in tune against A=432', () => {
  near(frequencyToNote(432, 432, 0).cents, 0);
  // ...and ~31.8 cents flat against A=440.
  const flat = frequencyToNote(432, 440, 0);
  assert.equal(flat.name, 'A');
  near(flat.cents, -31.77, 0.05);
});

test('offset -1 (E♭ standard): E♭2 reads as an in-tune E2 on the low string', () => {
  const eb2 = 77.7817; // E♭2 at A=440
  const r = frequencyToNote(eb2, 440, -1);
  assert.equal(r.name, 'E');
  assert.equal(r.midi, 40); // still the low-E string in the EADGBE row
  assert.equal(r.sounding, 'D♯');
  near(r.cents, 0, 0.1);
});

test('offset +2 maps F♯2 back to E2', () => {
  const r = frequencyToNote(92.4986, 440, 2);
  assert.equal(r.name, 'E');
  assert.equal(r.midi, 40);
  assert.equal(r.sounding, 'F♯');
});

test('sharp/flat sign and wrap across note boundaries', () => {
  near(frequencyToNote(440 * Math.pow(2, 10 / 1200), 440, 0).cents, 10);
  near(frequencyToNote(440 * Math.pow(2, -10 / 1200), 440, 0).cents, -10);
  // 60 cents sharp of A rounds to A♯, 40 cents flat.
  const r = frequencyToNote(440 * Math.pow(2, 60 / 1200), 440, 0);
  assert.equal(r.name, 'A♯');
  near(r.cents, -40);
});
