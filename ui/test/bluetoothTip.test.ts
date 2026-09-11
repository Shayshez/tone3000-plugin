/**
 * The pure predicate behind the iOS Bluetooth tip.
 *
 * The ui has no test runner, so this is a plain `node --test` file; Node
 * strips the TypeScript types on import. Run it with:
 *
 *   node --test ui/test/
 */
import test from 'node:test';
import assert from 'node:assert/strict';

import {
  bluetoothTipHeadline,
  shouldShowBluetoothTip,
  type AudioDeviceState,
} from '../src/types/audioDevice.ts';

/** A healthy USB-interface snapshot: open, 48 kHz, no Bluetooth. Only the
    three fields the predicate reads matter. */
const base = {
  deviceOpen: true,
  sampleRate: 48000,
  bluetoothRoute: false,
} as unknown as AudioDeviceState;

const withState = (over: Partial<AudioDeviceState>) =>
  shouldShowBluetoothTip({ ...base, ...over } as AudioDeviceState);

test('quiet on a normal 48 kHz route', () => {
  assert.equal(withState({}), false);
});

test('fires on a Bluetooth route even at a normal rate (A2DP output)', () => {
  assert.equal(withState({ bluetoothRoute: true }), true);
});

test('fires on a 24 kHz HFP session, route flag or not', () => {
  assert.equal(withState({ sampleRate: 24000 }), true);
  assert.equal(withState({ sampleRate: 24000, bluetoothRoute: true }), true);
});

test('44.1 kHz is a normal rate, not a capped one', () => {
  assert.equal(withState({ sampleRate: 44100 }), false);
});

test('stays quiet while no device is open (0 Hz is not a capped rate)', () => {
  assert.equal(withState({ deviceOpen: false, sampleRate: 0 }), false);
  assert.equal(withState({ deviceOpen: false, bluetoothRoute: true }), false);
  assert.equal(withState({ sampleRate: 0 }), false);
});

test('names Bluetooth only when the route says so', () => {
  const headline = (over: Partial<AudioDeviceState>) =>
    bluetoothTipHeadline({ ...base, ...over } as AudioDeviceState);
  // A2DP at a normal rate: latency is the only true claim.
  assert.equal(headline({ bluetoothRoute: true }), 'Bluetooth headphones add latency.');
  // HFP: the cap is named with the rate the session actually has, not a fixed 24.
  assert.match(headline({ sampleRate: 24000, bluetoothRoute: true }), /limiting audio to 24 kHz/);
  assert.match(headline({ sampleRate: 16000, bluetoothRoute: true }), /limiting audio to 16 kHz/);
  // A USB interface at 32 kHz is a low rate, not Bluetooth.
  assert.doesNotMatch(headline({ sampleRate: 32000 }), /Bluetooth/);
  assert.equal(
    headline({ sampleRate: 32000 }),
    'This audio route is running at 32 kHz, which limits fidelity and adds latency.'
  );
});
