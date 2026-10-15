// The arithmetic under the hand-written SVG charts (components/charts/): nice
// value ticks, linear scales, time ticks, and the line and area paths, which
// break wherever a value is missing (a bucket in which the server was not
// running), so a gap shows as a gap and never as a slope.

// 1, 2 or 5 times a power of ten: the step for about `count` ticks over
// `span`, the round step nearest the raw one (on a log scale: the cut-offs
// are the geometric means, as d3's tickIncrement has them).
export function niceStep(span: number, count: number): number {
  if (!(span > 0) || !(count >= 1)) return 1;
  const raw = span / count;
  const power = 10 ** Math.floor(Math.log10(raw));
  const fraction = raw / power;
  const nice = fraction >= Math.sqrt(50) ? 10 : fraction >= Math.sqrt(10) ? 5 : fraction >= Math.sqrt(2) ? 2 : 1;
  return nice * power;
}

function roundTo(value: number, step: number): number {
  const decimals = Math.max(0, -Math.floor(Math.log10(step)));
  return Number(value.toFixed(decimals));
}

export interface NiceTicks {
  // The domain the ticks span: min floored and max ceiled to the step.
  domain: [number, number];
  ticks: number[];
}

// About `count` ticks covering [min, max] on round values. `integer` keeps
// the step a whole number (counts of players). An empty or flat span still
// gets a domain: 0 to 1, or the value to one step above it.
export function niceTicks(min: number, max: number, count = 4, integer = false): NiceTicks {
  let lo = Number.isFinite(min) ? min : 0;
  let hi = Number.isFinite(max) ? max : lo + 1;
  if (hi < lo) [lo, hi] = [hi, lo];
  if (hi === lo) hi = lo + (lo === 0 ? 1 : Math.abs(lo));
  let step = niceStep(hi - lo, count);
  if (integer) step = Math.max(1, Math.round(step));
  const start = Math.floor(lo / step) * step;
  const end = Math.ceil(hi / step) * step;
  const ticks: number[] = [];
  for (let v = start; v <= end + step / 2; v += step) ticks.push(roundTo(v, step));
  return { domain: [roundTo(start, step), roundTo(end, step)], ticks };
}

// A linear scale from `domain` onto `range`; a flat domain maps to the
// range's middle.
export function scaleLinear(domain: [number, number], range: [number, number]): (value: number) => number {
  const [d0, d1] = domain;
  const [r0, r1] = range;
  if (d1 === d0) return () => (r0 + r1) / 2;
  return (value) => r0 + ((value - d0) / (d1 - d0)) * (r1 - r0);
}

// One series of a line chart (components/charts/LineChart.vue): the first is
// the main one, a `context` one a limit drawn beside it.
export interface ChartSeries {
  label: string;
  kind: 'main' | 'context';
  // One per time, in the times' order; null for a gap.
  points: { t: number; value: number | null }[];
}

// One plotted point; y null where the value is missing.
export interface PlotPoint {
  x: number;
  y: number | null;
}

// The runs of points that have a value, in order: a missing value ends a run.
export function segments(points: readonly PlotPoint[]): { x: number; y: number }[][] {
  const runs: { x: number; y: number }[][] = [];
  let run: { x: number; y: number }[] = [];
  for (const p of points) {
    if (p.y === null || !Number.isFinite(p.y)) {
      if (run.length) runs.push(run);
      run = [];
    } else {
      run.push({ x: p.x, y: p.y });
    }
  }
  if (run.length) runs.push(run);
  return runs;
}

function fmt(n: number): string {
  return String(Math.round(n * 100) / 100);
}

// The line through the points, one subpath per run; a lone point is a
// zero-length subpath (a dot under a round cap).
export function linePath(points: readonly PlotPoint[]): string {
  return segments(points)
    .map((run) => {
      const [first, ...rest] = run;
      const head = `M${fmt(first.x)},${fmt(first.y)}`;
      return rest.length === 0 ? `${head}h0` : `${head}${rest.map((p) => `L${fmt(p.x)},${fmt(p.y)}`).join('')}`;
    })
    .join('');
}

// The area under the line down to `baseline`, one closed shape per run.
export function areaPath(points: readonly PlotPoint[], baseline: number): string {
  return segments(points)
    .filter((run) => run.length > 1)
    .map((run) => {
      const first = run[0];
      const last = run[run.length - 1];
      return `M${fmt(first.x)},${fmt(baseline)}${run.map((p) => `L${fmt(p.x)},${fmt(p.y)}`).join('')}L${fmt(last.x)},${fmt(baseline)}Z`;
    })
    .join('');
}

const MINUTE = 60_000;
const HOUR = 60 * MINUTE;
const DAY = 24 * HOUR;
const TIME_STEPS = [
  5 * MINUTE,
  10 * MINUTE,
  15 * MINUTE,
  30 * MINUTE,
  HOUR,
  2 * HOUR,
  3 * HOUR,
  6 * HOUR,
  12 * HOUR,
  DAY,
  2 * DAY,
  7 * DAY,
];

// About `count` round times in [start, end] (epoch ms): every 15 minutes, 6
// hours, a day, a week... Steps under a day fall on the hour's round minutes
// (UTC, which shares them with every whole-hour zone); steps of days on the
// visitor's own midnight.
export function timeTicks(start: number, end: number, count = 5): number[] {
  if (!(end > start)) return [];
  const span = end - start;
  const step = TIME_STEPS.find((s) => span / s <= count) ?? TIME_STEPS[TIME_STEPS.length - 1];
  const ticks: number[] = [];
  if (step < DAY) {
    for (let t = Math.ceil(start / step) * step; t <= end; t += step) ticks.push(t);
    return ticks;
  }
  const first = new Date(start);
  first.setHours(0, 0, 0, 0);
  if (first.getTime() < start) first.setDate(first.getDate() + 1);
  const days = step / DAY;
  for (const d = first; d.getTime() <= end; d.setDate(d.getDate() + days)) ticks.push(d.getTime());
  return ticks;
}

// A time tick's label: the clock under a day's span ("14:00"), else the date
// ("14 Oct").
export function timeLabel(t: number, span: number): string {
  const d = new Date(t);
  if (span <= 2 * DAY) return d.toLocaleTimeString(undefined, { hour: 'numeric', minute: '2-digit' });
  return d.toLocaleDateString(undefined, { day: 'numeric', month: 'short' });
}

// The index of the point nearest `x` (points in x order), or -1 for none.
export function nearestIndex(xs: readonly number[], x: number): number {
  if (xs.length === 0) return -1;
  let lo = 0;
  let hi = xs.length - 1;
  while (hi - lo > 1) {
    const mid = (lo + hi) >> 1;
    if (xs[mid] <= x) lo = mid;
    else hi = mid;
  }
  return Math.abs(xs[lo] - x) <= Math.abs(xs[hi] - x) ? lo : hi;
}
