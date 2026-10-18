import { describe, expect, it } from 'vitest';
import { metricView } from './resourceView';

describe('metricView', () => {
  it("names each metric with its unit, a Sum of bytes a period a second", () => {
    expect(metricView('rds', 'CPUUtilization').title).toBe('CPU');
    expect(metricView('rds', 'CPUUtilization').format(41.2)).toBe('41 %');
    expect(metricView('rds', 'FreeableMemory').format(512 * 1024 * 1024)).toBe('512 MiB');
    expect(metricView('cache', 'NetworkBytesIn').perSecond).toBe(true);
    expect(metricView('rds', 'NetworkReceiveThroughput').perSecond).toBeUndefined();
    expect(metricView('nlb', 'ProcessedBytes').format(1_200_000)).toBe('1.2 MB/s');
  });

  it("reads a target group's healthy hosts by its listener, and an unknown metric as a count", () => {
    expect(metricView('nlb', 'HealthyHostCount:https').title).toBe('Healthy targets: The website (TCP 443)');
    expect(metricView('nlb', 'HealthyHostCount:https').integer).toBe(true);
    expect(metricView('cache', 'Evictions').title).toBe('Evictions');
  });
});
