/**
 * Snapshot of the standalone app's audio device state, produced by the
 * native StandaloneAudioSettings controller (getAudioDeviceState). The UI
 * treats this as read-only truth: every field is post-open readback. A
 * requested rate/buffer is a wish; these values are what the device actually
 * runs at. Never available in hosted builds (the DAW owns devices).
 */
export interface AudioDeviceState {
  /** All registered driver types; a driver picker renders only when > 1
      (macOS has a single CoreAudio type; never show a one-option select). */
  deviceTypes: string[];
  currentType: string;
  /** False only for ASIO-style backends where one driver owns both
      directions; those render a single device picker. */
  separateIO: boolean;
  inputDevices: string[];
  outputDevices: string[];
  /** Currently selected device names ('' = no device, a real option). */
  inputDevice: string;
  outputDevice: string;
  /** True when a device is actually open and running. */
  deviceOpen: boolean;
  inputChannels: AudioInputChannel[];
  /** Stereo-pair labels for multi-out interfaces (empty when the device has
      2 or fewer outputs and no picker is needed). */
  outputPairs: string[];
  /** Index into outputPairs of the active pair; -1 when not applicable. */
  activeOutputPair: number;
  sampleRates: number[];
  sampleRate: number;
  bufferSizes: number[];
  bufferSize: number;
  /** Vendor control panel exists (ASIO); buffer/clock may live there. */
  hasControlPanel: boolean;
  /** Output monitoring (inverse of the standalone holder's input mute). */
  hearYourself: boolean;
  /** Built-in mic feeding speakers; monitoring would squeal. */
  feedbackRisk: boolean;
  /** Windows only: an ASIO driver reports devices while another type is
      current (drives the "lower latency available" nudge). */
  asioAvailable: boolean;
  /** OS microphone gate. 'denied' means the OS is blocking audio input (macOS
      privacy), so input is silent until re-enabled and the app relaunched.
      'granted' on platforms without a queryable per-app mic gate. */
  micPermission: 'granted' | 'denied' | 'unknown';
  /** MIDI hardware, re-enumerated per pull (polling while the settings tab is
      open doubles as hot-plug detection). Enabled inputs are merged into one
      stream feeding the plugin; what each control does is the MIDI Mapping
      tab's business (see MidiMapState). */
  midiInputs: MidiInputDevice[];
  /** OS Bluetooth MIDI pairing dialog exists (macOS). */
  btMidiAvailable: boolean;
  /** iOS only: the audio session's current route is Bluetooth (HFP, A2DP or
      LE). Always false on desktop, where the OS does not force a route on
      us. Drives the Bluetooth tip (see shouldShowBluetoothTip). */
  bluetoothRoute: boolean;
}

/**
 * Should the Bluetooth tip show? Pure, so it can be tested without React.
 *
 * Two ways in: the route itself is Bluetooth, or the session came up below
 * 44.1 kHz, which on iOS effectively only happens on a headset (HFP) route
 * capped at 16 or 24 kHz. The rate arm is the safety net for a route the
 * port-type list does not name; the caller gates the whole thing to iOS.
 */
export const shouldShowBluetoothTip = (state: AudioDeviceState): boolean =>
  state.deviceOpen && (state.bluetoothRoute || (state.sampleRate > 0 && state.sampleRate < 44100));

/**
 * The headline the tip can stand behind. Only a route the session reports as
 * Bluetooth gets named as Bluetooth; a low rate on any other route (a USB
 * interface opened at 32 kHz, say) is described as what it is, a low rate,
 * so the banner never claims a cause it cannot see. Pure, tested.
 */
export const bluetoothTipHeadline = (state: AudioDeviceState): string => {
  const kHz = `${Math.round(state.sampleRate / 1000)} kHz`;
  const capped = state.sampleRate > 0 && state.sampleRate < 44100;
  if (state.bluetoothRoute)
    return capped
      ? `Bluetooth headphones are limiting audio to ${kHz} and add latency.`
      : 'Bluetooth headphones add latency.';
  return `This audio route is running at ${kHz}, which limits fidelity and adds latency.`;
};

export interface MidiInputDevice {
  /** OS device identifier (stable key for enable/disable). */
  id: string;
  name: string;
  enabled: boolean;
}

export interface AudioInputChannel {
  /** Device channel index (stable key for selection + metering). */
  index: number;
  name: string;
  active: boolean;
}

/** Result shape of every audio settings mutation. */
export interface AudioDeviceResult {
  ok: boolean;
  error: string;
}
