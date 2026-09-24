import React, { createContext, useContext, useEffect, useMemo, useRef } from 'react';
import { useMidiMap } from './useMidiMap';
import type { MidiMapping } from '../types/midiMap';
import { useToast } from '../components/Toast';
import { sourceLabel, targetById } from '../components/midiCatalog';
import type { TileMenuItem } from '../components/TileMenu';
import { MidiPort, Unlink, X } from '../components/icons';
import { HELP } from '../components/helpText';

/**
 * MIDI Learn straight from a control's right-click menu (see
 * useMidiMenuItems), instead of only from Settings > MIDI. One provider at
 * the plugin root owns a useMidiMap subscription; every control reads the
 * same snapshot.
 *
 * Feedback rides the toast: arming a learn pins "Move a MIDI control…" until
 * the hardware control is moved (native commits the mapping and pushes
 * midiMapChanged), then flashes what got mapped.
 */
interface MidiLearnContextValue {
  mappingFor: (targetId: string) => MidiMapping | undefined;
  learningTargetId: string;
  startLearn: (targetId: string) => void;
  cancelLearn: () => void;
  clearMapping: (targetId: string) => void;
}

const MidiLearnContext = createContext<MidiLearnContextValue | null>(null);

const targetName = (targetId: string) => targetById.get(targetId)?.name ?? targetId;

export const MidiLearnProvider: React.FC<{ children: React.ReactNode }> = ({ children }) => {
  const { state, actions } = useMidiMap(true);
  const toast = useToast();

  // Toast the outcome when an armed learn resolves: the target leaves
  // learnTargetId and (if the user moved a control) now has a mapping.
  const armedRef = useRef('');
  useEffect(() => {
    const learning = state?.learnTargetId ?? '';
    const armed = armedRef.current;
    if (armed && learning !== armed) {
      const mapping = state?.mappings.find((m) => m.targetId === armed);
      if (mapping) toast.show(`${targetName(armed)} → ${sourceLabel(mapping)}`);
      else toast.clear();
    }
    armedRef.current = learning;
  }, [state, toast]);

  const value = useMemo<MidiLearnContextValue>(
    () => ({
      mappingFor: (targetId) => state?.mappings.find((m) => m.targetId === targetId),
      learningTargetId: state?.learnTargetId ?? '',
      startLearn: (targetId) => {
        armedRef.current = targetId;
        toast.pin(`${targetName(targetId)}: move a MIDI control…`);
        void actions.startLearn(targetId);
      },
      cancelLearn: () => {
        armedRef.current = '';
        toast.clear();
        void actions.cancelLearn();
      },
      clearMapping: (targetId) => void actions.removeMapping(targetId),
    }),
    [state, actions, toast]
  );

  return <MidiLearnContext.Provider value={value}>{children}</MidiLearnContext.Provider>;
};

/**
 * Right-click menu rows for a MIDI-mappable control: MIDI Learn (or Cancel
 * while this target is armed) and, when mapped, Clear MIDI naming the
 * current assignment. `targetId` is an APVTS parameter id or a positional
 * block power ("block3Power"); null renders nothing (unmappable here).
 * `subject` names what's mapped when the menu isn't obviously about it
 * (a block's menu: "MIDI Learn Power" / "Clear Power MIDI (CC 20)").
 */
export function useMidiMenuItems(targetId: string | null, subject?: string): TileMenuItem[] {
  const ctx = useContext(MidiLearnContext);
  if (!ctx || targetId === null) return [];
  const mapping = ctx.mappingFor(targetId);
  const learning = ctx.learningTargetId === targetId;
  return [
    learning
      ? {
          label: 'Cancel MIDI Learn',
          icon: <X size={16} />,
          help: HELP.midiMenuCancel,
          onSelect: ctx.cancelLearn,
        }
      : {
          label: subject ? `MIDI Learn ${subject}` : 'MIDI Learn',
          icon: <MidiPort size={16} />,
          help: HELP.midiMenuLearn,
          onSelect: () => ctx.startLearn(targetId),
        },
    ...(mapping
      ? [
          {
            label: `Clear ${subject ? `${subject} ` : ''}MIDI (${sourceLabel(mapping)})`,
            icon: <Unlink size={16} />,
            help: HELP.midiMenuClear,
            onSelect: () => ctx.clearMapping(targetId),
          },
        ]
      : []),
  ];
}
