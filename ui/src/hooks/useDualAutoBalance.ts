import { useCallback, useEffect, useState } from 'react';
import { useNativeFunction } from './useFunction';
import { useToast } from '../components/Toast';

/** gainDeltaDb is always >= 0 - how much the quieter side (boostedSide) got
    raised. Below the native "already even" floor, no Vol change was
    applied at all (matching Auto Align's own "effectively zero" case). */
export interface DualAutoBalanceResult {
  state: string;
  gainDeltaDb?: number;
  boostedSide?: 'L' | 'R';
}

const doneMessage = ({ gainDeltaDb = 0, boostedSide }: DualAutoBalanceResult): string =>
  gainDeltaDb < 0.05
    ? 'Balanced · already even'
    : `Balanced · ${boostedSide} +${gainDeltaDb.toFixed(1)} dB`;

/**
 * One-shot Balance probe measurement, scoped to a single Dual Mono block's
 * own two sides - see armDualAutoBalance/pollDualAutoBalance (Processor.cpp).
 * Shares the exact same underlying sweep engine useDualAutoAlign's own
 * arm/poll pair does (one AutoOffset instance, two independent consumers -
 * see AutoOffset::Result's gainDeltaDb/boostRight), just reading a different
 * part of the result: Balance never touches Align's own offset/Ø fields,
 * and vice versa. A separate hook (rather than the shared useAutoMeasure) so
 * blockId can be baked into the native calls, same reasoning
 * useDualAutoAlign gives for itself. cancelAutoOffset is state-agnostic (one
 * shared engine instance, native-side), so it cancels a probe armed here the
 * same way it would one armed by Align.
 */
export function useDualAutoBalance(blockId: string) {
  const arm = useNativeFunction<boolean>('armDualAutoBalance');
  const cancel = useNativeFunction<boolean>('cancelAutoOffset');
  const poll = useNativeFunction<DualAutoBalanceResult>('pollDualAutoBalance');
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
      // measurement - Align included, same shared engine - already owns
      // it) - only pin "Measuring" once it actually took, or a failed arm
      // would leave that pinned forever.
      const armed = await arm(blockId);
      if (armed) {
        setListening(true);
        toast.pin('Measuring');
      }
    }
  }, [listening, arm, cancel, blockId, toast]);

  return { listening, toggle };
}
