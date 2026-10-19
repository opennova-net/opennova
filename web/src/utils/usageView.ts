// The usage views' words and lines (components/UsageCharts.vue, the program
// and machine cards): what a subject uses now, named, in readable units ("web:
// 41 % CPU of 0.5 vCPU, 612 MiB of 1 GiB, 1.2 MB/s out"), and its samples as
// chart series, a gap wherever the samples skip.

import type { ChartSeries } from './chartScale';
import { formatByteRate, formatBytes, formatCount, formatCpus, formatFrameTime, formatMemory, formatPercent, timeMs } from './format';
import type { Usage, UsageNow, UsagePoint, UsageRange } from '../types/usage';

export const RANGE_MS: Record<UsageRange, number> = {
  '48h': 48 * 60 * 60 * 1000,
  '400d': 400 * 24 * 60 * 60 * 1000,
};

export const RANGE_LABELS: Record<UsageRange, string> = {
  '48h': 'Last 48 hours',
  '400d': 'Last 400 days',
};

// "a point a minute over the last 48 hours".
export function rangeWords(range: UsageRange): string {
  return range === '48h'
    ? 'a point a minute over the last 48 hours'
    : "a point an hour over the last 400 days (the hour's mean CPU and its peak memory)";
}

// The parts of what a subject uses now that it has a figure for: its CPU of its
// CPUs, its memory of its limit, its network in and out.
export function usageParts(n: UsageNow): string[] {
  const parts: string[] = [];
  if (n.cpu_percent !== null) {
    parts.push(n.cpu_limit !== null ? `${formatPercent(n.cpu_percent)} CPU of ${formatCpus(n.cpu_limit)}` : `${formatPercent(n.cpu_percent)} CPU`);
  }
  if (n.memory_bytes !== null) {
    parts.push(n.memory_limit_bytes !== null ? `${formatMemory(n.memory_bytes)} of ${formatMemory(n.memory_limit_bytes)}` : formatMemory(n.memory_bytes));
  }
  if (n.net_in_bps !== null) parts.push(`${formatByteRate(n.net_in_bps)} in`);
  if (n.net_out_bps !== null) parts.push(`${formatByteRate(n.net_out_bps)} out`);
  return parts;
}

// "web: 41 % CPU of 0.5 vCPU, 612 MiB of 1 GiB, 300 kB/s in, 1.2 MB/s out",
// with `extra` parts after; null `now` for a subject with no report in the
// last 3 minutes.
export function usageLine(name: string, now: UsageNow | null, extra: string[] = []): string {
  if (now === null) return `${name}: no report in the last 3 minutes.`;
  const parts = [...usageParts(now), ...extra];
  return parts.length ? `${name}: ${parts.join(', ')}.` : `${name}: no figure reported.`;
}

// A usage reply's newest minute with its limits; null when it has none.
export function usageNow(usage: Usage): UsageNow | null {
  if (usage.now === null) return null;
  return {
    cpu_percent: usage.now.cpu_percent,
    cpu_limit: usage.limit.cpu,
    memory_bytes: usage.now.memory_bytes,
    memory_limit_bytes: usage.limit.memory_bytes,
    net_in_bps: usage.now.net_in_bps,
    net_out_bps: usage.now.net_out_bps,
  };
}

// A server's tick health and a machine's disk, as parts of its line.
export function usageExtras(usage: Usage): string[] {
  const now = usage.now;
  if (now === null) return [];
  const parts: string[] = [];
  if (now.tick_p99_ms != null) parts.push(`ticks ${formatFrameTime(now.tick_p99_ms)} at the 99th percentile`);
  if (now.late_ticks != null) parts.push(`${formatCount(now.late_ticks)} late tick${now.late_ticks === 1 ? '' : 's'} in the minute`);
  if (now.disk_free_bytes != null) {
    parts.push(
      usage.disk_total_bytes !== null
        ? `${formatBytes(now.disk_free_bytes)} of ${formatBytes(usage.disk_total_bytes)} disk free`
        : `${formatBytes(now.disk_free_bytes)} disk free`,
    );
  }
  return parts;
}

// One instant of the charts: a sample, or a gap (null) where the samples skip
// more than two steps (the subject was not reporting).
export interface UsageSlot {
  t: number;
  p: UsagePoint | null;
}

export function usageTimeline(usage: Pick<Usage, 'series' | 'step_seconds'>): UsageSlot[] {
  const step = usage.step_seconds * 1000;
  const out: UsageSlot[] = [];
  let prev: number | null = null;
  for (const p of usage.series) {
    const t = timeMs(p.at);
    if (Number.isNaN(t)) continue;
    if (prev !== null && t - prev > 2 * step) out.push({ t: prev + step, p: null });
    out.push({ t, p });
    prev = t;
  }
  return out;
}

// A series of one figure over the timeline.
export function usageSeries(
  timeline: readonly UsageSlot[],
  label: string,
  kind: ChartSeries['kind'],
  value: (p: UsagePoint) => number | null | undefined,
): ChartSeries {
  return { label, kind, points: timeline.map((s) => ({ t: s.t, value: s.p ? (value(s.p) ?? null) : null })) };
}

// A flat context line at `value` (a limit) wherever there is a sample.
export function limitSeries(timeline: readonly UsageSlot[], label: string, value: number): ChartSeries {
  return { label, kind: 'context', points: timeline.map((s) => ({ t: s.t, value: s.p ? value : null })) };
}
