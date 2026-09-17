import React, { useEffect, useRef, useState } from 'react';
import { helpProps } from './helpText';
import { FONT_MONO, SUBTLE } from './theme';

/** Readout chip that doubles as text entry: click to type, Enter commits,
    Escape cancels, blur commits (same conventions as the knobs). The value
    area is a fixed width (sized to the longest possible reading) so the chip
    never resizes while values change or while editing. Shared by BlockEqView
    (Freq/Gain/Q) and ChainBlock's IR envelope row (Init/A Len/A Crv/D Len/
    D Lvl/D Crv) - the "graph/sliders for by-ear, chip for exact value"
    split used by both editors. */
export const EditableChip: React.FC<{
  label: string;
  text: string;
  /** Prefill for the editor (number only, unit-free where possible). */
  editText: string;
  /** Fixed width of the value area in px: the widest reading the chip shows. */
  valueWidth: number;
  onCommit: (raw: string) => void;
  disabled?: boolean;
  /** One-line hint for the faceplate help readout (see helpText.ts). */
  help?: string;
  style?: React.CSSProperties;
  /** Label/value font size in rem units. Defaults to the EQ chips' 12; the
      IR envelope row (six chips, longer labels than EQ's Freq/Gain/Q) passes
      a smaller size so the row fits the card width. */
  fontSize?: number;
}> = ({
  label,
  text,
  editText,
  valueWidth,
  onCommit,
  disabled = false,
  help,
  style,
  fontSize = 12,
}) => {
  const [draft, setDraft] = useState<string | null>(null);
  const inputRef = useRef<HTMLInputElement>(null);
  const editing = draft !== null;

  useEffect(() => {
    if (editing) inputRef.current?.focus();
  }, [editing]);

  const commit = () => {
    if (draft !== null && draft.trim() !== '') onCommit(draft);
    setDraft(null);
  };

  return (
    <div
      {...(help && !disabled ? helpProps(help) : {})}
      onClick={() => {
        // Editing starts from an empty box (caret at the left) with the
        // current value as placeholder; committing empty is a cancel.
        if (!disabled && !editing) setDraft('');
      }}
      style={{ ...style, cursor: disabled || editing ? undefined : 'text' }}
    >
      <span style={{ fontSize: `${fontSize}rem`, fontFamily: FONT_MONO, color: SUBTLE }}>
        {label}
      </span>
      {editing ? (
        <input
          ref={inputRef}
          value={draft}
          onChange={(e) => setDraft(e.target.value)}
          onBlur={commit}
          onKeyDown={(e) => {
            e.stopPropagation();
            if (e.key === 'Enter') commit();
            else if (e.key === 'Escape') setDraft(null);
          }}
          inputMode="decimal"
          placeholder={editText}
          style={{
            width: `${valueWidth}rem`,
            background: 'transparent',
            border: 'none',
            color: '#ffffff',
            fontSize: `${fontSize}rem`,
            fontFamily: FONT_MONO,
            textAlign: 'left',
            outline: 'none',
            padding: 0,
          }}
        />
      ) : (
        <span
          style={{
            width: `${valueWidth}rem`,
            fontSize: `${fontSize}rem`,
            fontFamily: FONT_MONO,
            color: '#ffffff',
            textAlign: 'left',
            whiteSpace: 'nowrap',
          }}
        >
          {text}
        </span>
      )}
    </div>
  );
};

/** Same visual family as EditableChip (same track/label/value layout, meant
    to sit in the same row) but for a plain boolean, not a typed value: click
    anywhere on the chip to flip it, no text-entry mode. Used by ChainBlock's
    IR envelope row for Trim Init (see setBlockIrTrimInit) - unlike the
    numeric chips, there's nothing to type, just on/off. */
export const ToggleChip: React.FC<{
  label: string;
  on: boolean;
  onToggle: () => void;
  /** Fixed width of the value area in px, matching EditableChip's own so a
      toggle chip lines up with its numeric neighbors in the same row. */
  valueWidth: number;
  disabled?: boolean;
  help?: string;
  style?: React.CSSProperties;
  fontSize?: number;
  /** Overrides the default On/Off text - for a chip that cycles through
      more than two states (see ChainBlock's Trim Off/Std/Lax cycle) while
      `on` still just drives the label color (true = anything but the first,
      "off", state). Keep any override this short - the value column's
      width is sized for "Off". */
  value?: string;
}> = ({ label, on, onToggle, valueWidth, disabled = false, help, style, fontSize = 12, value }) => (
  <div
    {...(help && !disabled ? helpProps(help) : {})}
    onClick={() => {
      if (!disabled) onToggle();
    }}
    style={{ ...style, cursor: disabled ? undefined : 'pointer' }}
  >
    <span style={{ fontSize: `${fontSize}rem`, fontFamily: FONT_MONO, color: SUBTLE }}>
      {label}
    </span>
    <span
      style={{
        width: `${valueWidth}rem`,
        fontSize: `${fontSize}rem`,
        fontFamily: FONT_MONO,
        color: on ? '#ffffff' : SUBTLE,
        textAlign: 'left',
        whiteSpace: 'nowrap',
      }}
    >
      {value ?? (on ? 'On' : 'Off')}
    </span>
  </div>
);
