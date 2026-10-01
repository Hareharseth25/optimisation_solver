// Presentation of record values. Every formatter takes null/undefined to mean
// "absent" and says so; none of them turns an absent value into 0.

export const NOT_RUN = 'Not run';
export const UNKNOWN = 'Unknown';

export const isAbsent = (value) => value === null || value === undefined;

export function count(value) {
  if (isAbsent(value)) return UNKNOWN;
  return Number(value).toLocaleString('en-US');
}

// Objective values and other reals: up to 10 significant digits.
export function real(value) {
  if (isAbsent(value)) return UNKNOWN;
  if (value === 0) return '0';
  const magnitude = Math.abs(value);
  if (magnitude >= 1e-4 && magnitude < 1e12) return String(Number(value.toPrecision(10)));
  return value.toExponential(6);
}

// Residuals: exact zero stays "0", anything else in scientific notation.
export function residual(value) {
  if (isAbsent(value)) return UNKNOWN;
  if (value === 0) return '0';
  return value.toExponential(2);
}

export function seconds(value) {
  if (isAbsent(value)) return NOT_RUN;
  if (value < 1e-3) return `${(value * 1e6).toFixed(1)} µs`;
  if (value < 1) return `${(value * 1e3).toFixed(2)} ms`;
  return `${value.toFixed(3)} s`;
}

export function bytes(value) {
  if (value < 1024) return `${value} B`;
  if (value < 1024 * 1024) return `${(value / 1024).toFixed(1)} KiB`;
  return `${(value / (1024 * 1024)).toFixed(1)} MiB`;
}

// snake_case record vocabulary -> words, for statuses and enum names.
export function words(value) {
  if (isAbsent(value)) return UNKNOWN;
  return String(value).replace(/_/g, ' ');
}

export function shortHash(value, length = 12) {
  return isAbsent(value) ? UNKNOWN : String(value).slice(0, length);
}
