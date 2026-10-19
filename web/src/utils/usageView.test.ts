import { describe, expect, it } from 'vitest';
import { limitSeries, usageLine, usageSeries, usageTimeline } from './usageView';

describe('usageLine', () => {
  it('names the subject and what it uses in readable units, leaving out what it lacks', () => {
    const now = {
      cpu_percent: 41.3,
      cpu_limit: 0.5,
      memory_bytes: 612 * 1024 * 1024,
      memory_limit_bytes: 1024 * 1024 * 1024,
      net_in_bps: null,
      net_out_bps: 1_200_000,
    };
    expect(usageLine('web', now)).toBe('web: 41 % CPU of 0.5 vCPU, 612 MiB of 1 GiB, 1.2 MB/s out.');
    expect(usageLine('Machine 3', { ...now, cpu_limit: null, memory_limit_bytes: null }, ['30 GB disk free'])).toBe(
      'Machine 3: 41 % CPU, 612 MiB, 1.2 MB/s out, 30 GB disk free.',
    );
    expect(usageLine('data', null)).toBe('data: no report in the last 3 minutes.');
    expect(
      usageLine('data', { cpu_percent: null, cpu_limit: null, memory_bytes: null, memory_limit_bytes: null, net_in_bps: null, net_out_bps: null }),
    ).toBe('data: no figure reported.');
  });
});

describe('usageTimeline', () => {
  const at = (minute: number) => new Date(Date.UTC(2026, 9, 18, 12, minute)).toISOString();
  const point = (minute: number, cpu: number) => ({ at: at(minute), cpu_percent: cpu, memory_bytes: null, net_in_bps: null, net_out_bps: null });

  it('puts a gap where the samples skip more than two steps', () => {
    const timeline = usageTimeline({ step_seconds: 60, series: [point(0, 1), point(1, 2), point(2, 3), point(10, 4)] });
    expect(timeline.map((s) => (s.p ? s.p.cpu_percent : null))).toEqual([1, 2, 3, null, 4]);
    expect(timeline[3].t).toBe(Date.UTC(2026, 9, 18, 12, 3));
    // A limit is drawn where there is a sample, not across the gap.
    expect(usageSeries(timeline, 'CPU', 'main', (p) => p.cpu_percent).points.map((p) => p.value)).toEqual([1, 2, 3, null, 4]);
    expect(limitSeries(timeline, 'Limit', 100).points.map((p) => p.value)).toEqual([100, 100, 100, null, 100]);
  });
});
