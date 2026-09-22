import { useCallback, useEffect, useState } from 'react';
import { useNativeFunction } from './useFunction';
import { useToast } from '../components/Toast';

/** matchedMs is the measured inter-chain lag (positive = the right side
    gets delayed), mirroring the Offset knob move; below the native
    "already aligned" floor no delay correction was applied. polarityFlipped
    reports Ø toggled alongside. Two decimals: the probe measures to
    sub-sample precision. */
export interface DualAutoAlignResult {
  state: string;
  matchedMs?: number;
  polarityFlipped?: boolean;
}

const doneMessage = ({ matchedMs = 0, polarityFlipped = false }: DualAutoAlignResult): string => {
  const delay =
    Math.abs(matchedMs) < 0.05
      ? null
      : `${matchedMs > 0 ? 'R' : 'L'} +${Math.abs(matchedMs).toFixed(2)} ms`;
  return ['Aligned', delay, polarityFlipped ? 'Ø flipped' : null]
    .filter((part) => part != null)
    .join(' · ');
};

/**
 * One-shot Align probe measurement (a brief output mute while an internal
 * sweep drives both sides), scoped to a single Dual Mono block's own two
 * sides - see armDualAutoAlign/pollDualAutoAlign (Processor.cpp). A
 * separate hook (rather than the shared useAutoMeasure) so blockId can be
 * baked into the native calls. cancelAutoOffset is state-agnostic (one
 * shared AutoOffset engine instance, native-side), so it cancels a probe
 * armed here the same way it would one armed anywhere else that reuses the
 * same engine.
 */
export function useDualAutoAlign(blockId: string) {
  const arm = useNativeFunction<boolean>('armDualAutoAlign');
  const cancel = useNativeFunction<boolean>('cancelAutoOffset');
  const poll = useNativeFunction<DualAutoAlignResult>('pollDualAutoAlign');
  const toast = useToast();
  const [listening, setListening] = useState(false);

  useEffect(() => {
    if (!listening) return;
    const id = setInterval(async () => {
      const res = await poll(blockId);
      if (!res || res.state === 'listening') return;
      setListening(false);
      if (res.state === 'done') toast.show(doneMessage(res));
      else toast.clear();
    }, 200);
    return () => clearInterval(id);
  }, [listening, poll, blockId, toast]);

  const toggle = useCallback(async () => {
    if (listening) {
      await cancel();
      setListening(false);
      toast.clear();
    } else {
      // Unlike the global case, arm can genuinely fail here (another
      // measurement already owns the engine) - only pin "Measuring" once
      // it actually took, or a failed arm would leave that pinned forever.
      const armed = await arm(blockId);
      if (armed) {
        setListening(true);
        toast.pin('Measuring');
      }
    }
  }, [listening, arm, cancel, blockId, toast]);

  return { listening, toggle };
}
