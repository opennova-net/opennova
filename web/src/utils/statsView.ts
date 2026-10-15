// What the stats views (components/StatsDashboard.vue) draw from a stats
// reply (types/hostedStats.ts): the window the series covers, and the chart
// series, each a gap wherever the server was not running.

import type { HostedStats, StatsRange } from '../types/hostedStats';
import type { ChartSeries } from './chartScale';

const HOUR = 3_600_000;

export const RANGE_MS: Record<StatsRange, number> = {
  '1h': HOUR,
  '24h': 24 * HOUR,
  '7d': 7 * 24 * HOUR,
  '30d': 30 * 24 * HOUR,
};

// "in the last 24 hours".
export const RANGE_WORDS: Record<StatsRange, string> = {
  '1h': 'in the last hour',
  '24h': 'in the last 24 hours',
  '7d': 'in the last 7 days',
  '30d': 'in the last 30 days',
};

// The x domain: the range, ending with the newest bucket (or `now` when the
// series is empty).
export function statsWindow(stats: HostedStats, now: number): { start: number; end: number } {
  const points = stats.series.points;
  const last = points.length ? Date.parse(points[points.length - 1].t) + stats.series.step_seconds * 1000 : now;
  const end = Number.isFinite(last) ? last : now;
  return { start: end - RANGE_MS[stats.range], end };
}

function times(stats: HostedStats): number[] {
  return stats.series.points.map((p) => Date.parse(p.t));
}

// Players over time, with the most allowed beside them as context (when the
// series carries it).
export function playerSeries(stats: HostedStats): ChartSeries[] {
  const t = times(stats);
  const points = stats.series.points;
  const players: ChartSeries = {
    label: 'Players',
    kind: 'main',
    points: points.map((p, i) => ({ t: t[i], value: p.running ? p.players : null })),
  };
  if (points.every((p) => p.max_players === null)) return [players];
  const max: ChartSeries = {
    label: 'Max players',
    kind: 'context',
    points: points.map((p, i) => ({ t: t[i], value: p.running ? p.max_players : null })),
  };
  return [players, max];
}

// Bytes sent to players in each bucket.
export function egressSeries(stats: HostedStats): ChartSeries[] {
  const t = times(stats);
  return [
    {
      label: 'Data sent',
      kind: 'main',
      points: stats.series.points.map((p, i) => ({ t: t[i], value: p.running ? p.egress_bytes : null })),
    },
  ];
}

// The CPU use the machine reported for the server, per bucket.
export function cpuValues(stats: HostedStats): (number | null)[] {
  return stats.series.points.map((p) => (p.running ? p.cpu_percent : null));
}

// Players per bucket (a sparkline's values).
export function playerValues(stats: HostedStats): (number | null)[] {
  return stats.series.points.map((p) => (p.running ? p.players : null));
}

// The step in words, for a chart's description: "each point is 5 minutes".
export function stepWords(stats: HostedStats): string {
  const minutes = Math.round(stats.series.step_seconds / 60);
  if (minutes < 60) return minutes === 1 ? 'each point is a minute' : `each point is ${minutes} minutes`;
  const hours = Math.round(minutes / 60);
  return hours === 1 ? 'each point is an hour' : `each point is ${hours} hours`;
}
