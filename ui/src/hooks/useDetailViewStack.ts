import { useRef, useState } from 'react';

/**
 * A block detail's sub-view: the block's own plain view, or one of its
 * toggleable panels.
 */
export type DetailView = 'main' | 'eq' | 'info' | 'stereo';

export interface DetailViewStack {
  /** The currently-shown view (top of the internal stack). Named
      `activeView`, not `current` - eslint's react-hooks plugin flags any
      `.current` property access in a dependency array as a likely ref
      mistake, which this isn't. */
  activeView: DetailView;
  /** Pill/icon onClick: toggle a sub-view. Toggling the one already showing
      pops it. From 'main' this pushes (a genuine deepen); switching
      directly between sibling sub-views (e.g. EQ -> Info without ever
      returning to 'main') replaces the top entry instead of stacking
      deeper. */
  toggle: (view: DetailView) => void;
  /** Back-arrow onClick: pop one level. Popping past the stack's last
      entry calls `onExit` instead of leaving it empty. */
  pop: () => void;
  /** Unconditionally replace the whole stack - a fresh open, a shortcut
      open straight into a sub-view (deliberately seeded without 'main'
      beneath it, since that view was never actually visited), or a
      lateral jump to a sibling block. */
  reset: (seed: DetailView[]) => void;
}

/**
 * One reusable "how did I get here" stack, used identically at every
 * nesting level a block detail can appear at: ChainView's own top-level
 * block detail, an ordinary ChainBlock instance's sub-view, and a recursed
 * Dual Mono child's own ChainBlock instance. Fixes the class of bug where
 * a sub-view entered via a shortcut (skipping the intermediate 'main' view)
 * closes onto that skipped intermediate view instead of retracing fully
 * back to wherever the user actually came from - `pop`/`toggle`-off both
 * call the same `onExit` once the stack is down to its last entry, rather
 * than each independently guessing a default destination.
 */
export function useDetailViewStack(initialView: DetailView, onExit: () => void): DetailViewStack {
  const [stack, setStack] = useState<DetailView[]>([initialView]);
  const stackRef = useRef(stack);
  stackRef.current = stack;
  const onExitRef = useRef(onExit);
  onExitRef.current = onExit;

  const pop = () => {
    if (stackRef.current.length > 1) setStack((s) => s.slice(0, -1));
    else onExitRef.current();
  };

  const toggle = (view: DetailView) => {
    const s = stackRef.current;
    const top = s[s.length - 1];
    if (top === view) {
      pop();
      return;
    }
    if (top === 'main') setStack((prev) => [...prev, view]);
    else setStack((prev) => [...prev.slice(0, -1), view]);
  };

  const reset = (seed: DetailView[]) => setStack(seed);

  return { activeView: stack[stack.length - 1] ?? 'main', toggle, pop, reset };
}
