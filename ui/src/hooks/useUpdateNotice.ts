import { useCallback, useEffect, useState } from 'react';
import { UPDATE_NOTICE_ENABLED } from '../t3k/config';
import type { T3KClient } from '../t3k/tone3000-client';
import { isNativeFunctionRegistered } from '../backend/JuceBackend';
import { useAudioBackend } from './useAudioBackend';

/**
 * Startup update check. Pings the version endpoint on mount (and again when
 * the session appears or disappears) and surfaces an "update available"
 * notice when the published version is newer than the running build.
 * Auth is optional: a signed-in Bearer lets the server return a
 * user-specific payload (beta builds for select accounts); signed-out
 * users still get the public release. Every check also sends `X-Device-Id`
 * (JUCE's stable machine hash) so installs can be targeted independently
 * of the account. Deliberately best-effort:
 *
 * - Disabled entirely unless `VITE_T3K_UPDATE_NOTICE=true` (forks skip it).
 * - Never blocks UI load; fires in an effect with a 5s timeout.
 * - Any failure (offline, 404, bad payload) is silently ignored.
 * - Skipped in dev / on native builds that don't expose `getPluginVersion`
 *   (the JUCE bridge lists registered functions at startup).
 *
 * Dismissing the modal snoozes it for 1, 7 or 30 days (localStorage, same
 * convention as the other UI prefs). There is no "skip this version": every
 * snooze expires and the notice comes back until the user updates. A snoozed
 * update still shows in Settings (`update` is exposed regardless of snooze).
 *
 * The plugin's own version (`localVersion`) is read unconditionally so
 * Settings can display it even when the update check is disabled.
 */

export interface UpdateNoticeData {
  version: string;
  messageHtml: string;
  url: string;
}

const STORAGE_KEY = 't3k.updateNotice';

function readSnoozeUntil(): number {
  try {
    const parsed = JSON.parse(localStorage.getItem(STORAGE_KEY) ?? '{}') as {
      snoozeUntil?: number;
    };
    return typeof parsed.snoozeUntil === 'number' ? parsed.snoozeUntil : 0;
  } catch {
    return 0;
  }
}

function writeSnoozeUntil(timestamp: number): void {
  try {
    localStorage.setItem(STORAGE_KEY, JSON.stringify({ snoozeUntil: timestamp }));
  } catch {
    // Storage unavailable; worst case the notice reappears next open.
  }
}

/**
 * Compare dot-separated numeric versions ("1.2.3", tolerates a leading "v"
 * and non-numeric suffixes). Positive when a > b. It's a strictly-newer check, so
 * dev builds and forks ahead of the public release are never prompted.
 */
export function compareVersions(a: string, b: string): number {
  const parse = (v: string) =>
    v
      .trim()
      .replace(/^v/i, '')
      .split('.')
      .map((seg) => parseInt(seg, 10) || 0);
  const [pa, pb] = [parse(a), parse(b)];
  for (let i = 0; i < Math.max(pa.length, pb.length); i++) {
    const diff = (pa[i] ?? 0) - (pb[i] ?? 0);
    if (diff !== 0) return diff;
  }
  return 0;
}

export function useUpdateNotice(client: T3KClient): {
  /** Update to show in the startup modal; null once snoozed/dismissed. */
  notice: UpdateNoticeData | null;
  /** Available update regardless of snooze, for the Settings screen. */
  update: UpdateNoticeData | null;
  /** The running build's version ("" until resolved / outside the plugin). */
  localVersion: string;
  /** sandbox/experiments checkpoint counter ("" once merged/unresolved). */
  sandboxBuild: string;
  remindLater: (days: number) => void;
} {
  const backend = useAudioBackend();
  const [notice, setNotice] = useState<UpdateNoticeData | null>(null);
  const [update, setUpdate] = useState<UpdateNoticeData | null>(null);
  const [localVersion, setLocalVersion] = useState('');
  const [sandboxBuild, setSandboxBuild] = useState('');
  // Re-check when the session appears or disappears so a just-signed-in
  // beta tester gets their payload, and logout drops a beta-only notice.
  const authenticated = client.isAuthenticated();

  // Independent of the update-check flow below (no auth/UPDATE_NOTICE_ENABLED
  // gating) - this is just a dev-facing marker, see SANDBOX_LOG.md.
  useEffect(() => {
    if (!isNativeFunctionRegistered('getSandboxBuild')) return;
    let cancelled = false;
    (async () => {
      const build = await backend.getPluginFunction('getSandboxBuild')();
      if (!cancelled && typeof build === 'string' && build.length > 0) setSandboxBuild(build);
    })();
    return () => {
      cancelled = true;
    };
  }, [backend]);

  useEffect(() => {
    if (!isNativeFunctionRegistered('getPluginVersion')) return;

    let cancelled = false;
    (async () => {
      const version = await backend.getPluginFunction('getPluginVersion')();
      if (cancelled || typeof version !== 'string' || version.length === 0) return;
      setLocalVersion(version);

      if (!UPDATE_NOTICE_ENABLED) return;

      let deviceId = '';
      if (isNativeFunctionRegistered('getUniqueDeviceID')) {
        const id = await backend.getPluginFunction('getUniqueDeviceID')();
        if (typeof id === 'string') deviceId = id;
      }

      // AbortController + setTimeout instead of `AbortSignal.timeout()`: the
      // system WebKit before Safari 16 (any macOS 10.15 install) lacks the
      // latter, and the machines running old WebKit are exactly the ones
      // that most need to hear about updates (same rationale as
      // probeSecureConnection in useConnectionGate).
      const controller = new AbortController();
      const timer = setTimeout(() => controller.abort(), 5000);
      let res: Response;
      try {
        res = await client.fetchPluginVersion({
          signal: controller.signal,
          // The response is CDN-cached server-side; never let a stale local
          // webview cache hide a new release for days.
          cache: 'no-cache',
          headers: deviceId ? { 'X-Device-Id': deviceId } : undefined,
        });
      } finally {
        clearTimeout(timer);
      }
      if (!res.ok) return;

      const body: unknown = await res.json();
      const remote = (body ?? {}) as Record<string, unknown>;
      // The payload is remote input: require the exact shape and an http(s)
      // link (anything else, e.g. a javascript: URL, is dropped).
      if (typeof remote.version !== 'string' || typeof remote.message_html !== 'string') return;
      if (typeof remote.url !== 'string' || !/^https?:\/\//i.test(remote.url)) return;

      if (compareVersions(remote.version, version) <= 0) {
        // A previous (likely beta-gated) payload is no longer offered.
        if (!cancelled) {
          setUpdate(null);
          setNotice(null);
        }
        return;
      }

      const data: UpdateNoticeData = {
        version: remote.version,
        messageHtml: remote.message_html,
        url: remote.url,
      };
      if (cancelled) return;
      setUpdate(data);
      if (Date.now() >= readSnoozeUntil()) setNotice(data);
    })().catch(() => {
      // Best-effort by design: offline, timeout, bad JSON are all ignored.
    });

    return () => {
      cancelled = true;
    };
  }, [authenticated, backend, client]);

  const remindLater = useCallback((days: number) => {
    writeSnoozeUntil(Date.now() + days * 24 * 60 * 60 * 1000);
    setNotice(null);
  }, []);

  return { notice, update, localVersion, sandboxBuild, remindLater };
}
