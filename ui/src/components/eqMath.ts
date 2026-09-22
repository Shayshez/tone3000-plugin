import type { EqBand, EqBandRole } from '../types/chain';
import { EQ_MAX_FREQ_HZ, EQ_MIN_FREQ_HZ, isEqBandActive, roleForBandIndex } from '../types/chain';

/**
 * Exact TypeScript mirror of the native biquad math (plugin/src/BlockEq.cpp,
 * RBJ Audio EQ Cookbook with A = 10^(dB/40)) so the drawn curve is the audio
 * truth, not an approximation. Keep both sides in sync.
 */

interface BiquadCoeffs {
  b0: number;
  b1: number;
  b2: number;
  a1: number;
  a2: number;
}

/** Single RBJ biquad for Bell/Low Shelf/High Shelf - one stage, as before. */
function computeCoeffs(band: EqBand, role: EqBandRole, sampleRate: number): BiquadCoeffs {
  const freq = Math.min(
    Math.max(band.freqHz, EQ_MIN_FREQ_HZ),
    Math.min(EQ_MAX_FREQ_HZ, sampleRate * 0.49)
  );
  const A = Math.pow(10, band.gainDb / 40);
  const omega = (2 * Math.PI * freq) / sampleRate;
  const sn = Math.sin(omega);
  const cs = Math.cos(omega);
  const alpha = sn / (2 * band.q);
  const sqrtA = Math.sqrt(A);

  let b0 = 1;
  let b1 = 0;
  let b2 = 0;
  let a0 = 1;
  let a1 = 0;
  let a2 = 0;

  switch (role) {
    case 'bell':
      b0 = 1 + alpha * A;
      b1 = -2 * cs;
      b2 = 1 - alpha * A;
      a0 = 1 + alpha / A;
      a1 = -2 * cs;
      a2 = 1 - alpha / A;
      break;
    case 'lowshelf':
      b0 = A * (A + 1 - (A - 1) * cs + 2 * sqrtA * alpha);
      b1 = 2 * A * (A - 1 - (A + 1) * cs);
      b2 = A * (A + 1 - (A - 1) * cs - 2 * sqrtA * alpha);
      a0 = A + 1 + (A - 1) * cs + 2 * sqrtA * alpha;
      a1 = -2 * (A - 1 + (A + 1) * cs);
      a2 = A + 1 + (A - 1) * cs - 2 * sqrtA * alpha;
      break;
    case 'highshelf':
      b0 = A * (A + 1 + (A - 1) * cs + 2 * sqrtA * alpha);
      b1 = -2 * A * (A - 1 + (A + 1) * cs);
      b2 = A * (A + 1 + (A - 1) * cs - 2 * sqrtA * alpha);
      a0 = A + 1 - (A - 1) * cs + 2 * sqrtA * alpha;
      a1 = 2 * (A - 1 - (A + 1) * cs);
      a2 = A + 1 - (A - 1) * cs - 2 * sqrtA * alpha;
      break;
    case 'lowcut':
    case 'highcut':
      // Handled by computeCutStages instead.
      break;
  }

  return { b0: b0 / a0, b1: b1 / a0, b2: b2 / a0, a1: a1 / a0, a2: a2 / a0 };
}

const REFERENCE_Q = Math.SQRT1_2; // 1/sqrt(2)

/**
 * Cascaded Low/High Cut: `band.poles` poles = ceil(poles/2) stages in
 * series (an odd leftover first-order section plus one 2nd-order RBJ
 * highpass/lowpass section per pole pair, each Q following the standard
 * Butterworth pole-angle formula scaled by band.q/0.7071). Exact mirror of
 * native `BlockEq::updateCutBand` - keep both in sync.
 */
export function computeCutStages(
  band: EqBand,
  role: EqBandRole,
  sampleRate: number
): BiquadCoeffs[] {
  const freq = Math.min(
    Math.max(band.freqHz, EQ_MIN_FREQ_HZ),
    Math.min(EQ_MAX_FREQ_HZ, sampleRate * 0.49)
  );
  const omega = (2 * Math.PI * freq) / sampleRate;
  const sn = Math.sin(omega);
  const cs = Math.cos(omega);
  const isHighpass = role === 'lowcut';

  const poles = band.poles;
  const numSecondOrder = Math.floor(poles / 2);
  const hasFirstOrder = poles % 2 !== 0;
  const stages: BiquadCoeffs[] = [];

  if (hasFirstOrder) {
    const K = Math.tan(omega * 0.5);
    const b0 = isHighpass ? 1 / (1 + K) : K / (1 + K);
    const b1 = isHighpass ? -b0 : b0;
    const a1 = (K - 1) / (1 + K);
    stages.push({ b0, b1, b2: 0, a1, a2: 0 });
  }

  const qScale = band.q / REFERENCE_Q;
  for (let k = 1; k <= numSecondOrder; ++k) {
    const qBase = 1 / (2 * Math.sin(((2 * k - 1) * Math.PI) / (2 * poles)));
    const alpha = sn / (2 * qBase * qScale);

    let b0: number;
    let b1: number;
    let b2: number;
    if (isHighpass) {
      b0 = (1 + cs) * 0.5;
      b1 = -(1 + cs);
      b2 = (1 + cs) * 0.5;
    } else {
      b0 = (1 - cs) * 0.5;
      b1 = 1 - cs;
      b2 = (1 - cs) * 0.5;
    }
    const a0 = 1 + alpha;
    const a1 = -2 * cs;
    const a2 = 1 - alpha;

    stages.push({ b0: b0 / a0, b1: b1 / a0, b2: b2 / a0, a1: a1 / a0, a2: a2 / a0 });
  }

  return stages;
}

/** Every stage of a band, in processing order (1 for Bell/Shelf, 1-4 for
    Low/High Cut). */
function stagesForBand(band: EqBand, role: EqBandRole, sampleRate: number): BiquadCoeffs[] {
  if (role === 'lowcut' || role === 'highcut') return computeCutStages(band, role, sampleRate);
  return [computeCoeffs(band, role, sampleRate)];
}

/** |H(e^jω)| in dB of a normalized biquad at a single frequency. */
function biquadMagnitudeDb(c: BiquadCoeffs, freqHz: number, sampleRate: number): number {
  const omega = (2 * Math.PI * freqHz) / sampleRate;
  const cosW = Math.cos(omega);
  const cos2W = Math.cos(2 * omega);
  const num =
    c.b0 * c.b0 +
    c.b1 * c.b1 +
    c.b2 * c.b2 +
    2 * (c.b0 * c.b1 + c.b1 * c.b2) * cosW +
    2 * c.b0 * c.b2 * cos2W;
  const den = 1 + c.a1 * c.a1 + c.a2 * c.a2 + 2 * (c.a1 + c.a1 * c.a2) * cosW + 2 * c.a2 * cos2W;
  const magSq = num / Math.max(den, 1e-24);
  return 10 * Math.log10(Math.max(magSq, 1e-24));
}

/**
 * Combined EQ magnitude response (dB) at each of `freqsHz`. Inert bands are
 * skipped, matching the audio thread, which doesn't process them either.
 * Sums every stage of every active band (Low/High Cut can have 1-4 stages).
 */
export function eqResponseDb(bands: EqBand[], sampleRate: number, freqsHz: number[]): number[] {
  const activeStages: BiquadCoeffs[] = [];
  bands.forEach((band, i) => {
    if (!isEqBandActive(band, i)) return;
    const role = roleForBandIndex(i, bands.length);
    activeStages.push(...stagesForBand(band, role, sampleRate));
  });
  return freqsHz.map((f) => {
    let db = 0;
    for (const coeffs of activeStages) db += biquadMagnitudeDb(coeffs, f, sampleRate);
    return db;
  });
}

const LOG_MIN = Math.log(EQ_MIN_FREQ_HZ);
const LOG_MAX = Math.log(EQ_MAX_FREQ_HZ);

/** Frequency → 0..1 position on the log-scaled x axis (20 Hz .. 20 kHz). */
export function freqToNorm(freqHz: number): number {
  const clamped = Math.min(Math.max(freqHz, EQ_MIN_FREQ_HZ), EQ_MAX_FREQ_HZ);
  return (Math.log(clamped) - LOG_MIN) / (LOG_MAX - LOG_MIN);
}

/** 0..1 x position → frequency (inverse of freqToNorm). */
export function normToFreq(norm: number): number {
  const t = Math.min(Math.max(norm, 0), 1);
  return Math.exp(LOG_MIN + t * (LOG_MAX - LOG_MIN));
}

/** Always the full number in Hz, never a "1.31k" abbreviation - the strip
    is dense enough already without also asking the reader to convert units. */
export function formatFreq(freqHz: number): string {
  return `${Math.round(freqHz)} Hz`;
}
