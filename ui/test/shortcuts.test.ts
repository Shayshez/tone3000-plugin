import { test } from 'node:test';
import assert from 'node:assert/strict';
import { resolveShortcut } from '../src/hooks/shortcutKeys.ts';

const key = (code: string, extra: Partial<Parameters<typeof resolveShortcut>[0]> = {}) => ({
  code,
  key: extra.key ?? '',
  metaKey: false,
  ctrlKey: false,
  altKey: false,
  shiftKey: false,
  ...extra,
});

test('digits 1-4 select scenes, other digits are not ours', () => {
  assert.deepEqual(resolveShortcut(key('Digit1')), { kind: 'scene', index: 0 });
  assert.deepEqual(resolveShortcut(key('Digit4')), { kind: 'scene', index: 3 });
  assert.equal(resolveShortcut(key('Digit5')), null);
  assert.equal(resolveShortcut(key('Digit1', { shiftKey: true })), null);
});

test('brackets step presets, shifted brackets step scenes', () => {
  assert.deepEqual(resolveShortcut(key('BracketLeft')), { kind: 'preset', direction: -1 });
  assert.deepEqual(resolveShortcut(key('BracketRight')), { kind: 'preset', direction: 1 });
  assert.deepEqual(resolveShortcut(key('BracketRight', { shiftKey: true })), {
    kind: 'sceneStep',
    direction: 1,
  });
});

test('command-Z undoes, shift-command-Z / command-Y redo', () => {
  assert.deepEqual(resolveShortcut(key('KeyZ', { metaKey: true })), { kind: 'undo' });
  assert.deepEqual(resolveShortcut(key('KeyZ', { ctrlKey: true, shiftKey: true })), {
    kind: 'redo',
  });
  assert.deepEqual(resolveShortcut(key('KeyY', { metaKey: true })), { kind: 'redo' });
});

test('every other modifier combo is left to the host', () => {
  assert.equal(resolveShortcut(key('KeyS', { metaKey: true })), null);
  assert.equal(resolveShortcut(key('Digit1', { metaKey: true })), null);
  assert.equal(resolveShortcut(key('BracketLeft', { altKey: true })), null);
  assert.equal(resolveShortcut(key('KeyZ', { metaKey: true, altKey: true })), null);
});

test('Escape means back; plain letters are not ours', () => {
  assert.deepEqual(resolveShortcut(key('Escape', { key: 'Escape' })), { kind: 'back' });
  assert.equal(resolveShortcut(key('KeyA', { key: 'a' })), null);
  assert.equal(resolveShortcut(key('Space', { key: ' ' })), null);
});
