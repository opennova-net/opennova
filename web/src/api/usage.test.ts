import { beforeEach, describe, expect, it, vi } from 'vitest';
import { adminClient } from './client';
import { fetchPrograms, fetchResourceUsage, fetchUsage, pickPrograms, pickResourceUsage, pickUsage } from './usage';
import {
  localResourceUsage,
  machineUsage,
  programUsage,
  programsResponse,
  resourceUsage,
  serverUsage,
} from '../test/usageFixtures';

vi.mock('./client', () => ({
  apiClient: { get: vi.fn(), post: vi.fn() },
  adminClient: { get: vi.fn(), post: vi.fn() },
}));

const get = vi.mocked(adminClient.get);

// The reply as the server sends it: JSON, so a round trip through text.
const wire = (value: unknown): unknown => JSON.parse(JSON.stringify(value));

describe('the usage client', () => {
  beforeEach(() => vi.clearAllMocks());

  it("reads a server's, a machine's and a program's usage as the server sends them", () => {
    expect(pickUsage(wire(serverUsage()))).toEqual(serverUsage());
    expect(pickUsage(wire(machineUsage()))).toEqual(machineUsage());
    expect(pickUsage(wire(programUsage()))).toEqual(programUsage());
    // The route writes a row's id as a number.
    expect(pickUsage({ ...(wire(serverUsage()) as object), id: 12 })?.id).toBe('12');
  });

  it('keeps each subject to its own fields, and a missing value null', () => {
    const usage = pickUsage({
      subject: 'program',
      id: 'web',
      range: '400d',
      limit: null,
      series: [{ at: '2026-10-18T11:00:00Z', cpu_percent: 'x', tick_p99_ms: 3, disk_free_bytes: 5 }],
      now: null,
    });
    expect(usage?.step_seconds).toBe(3600);
    expect(usage?.limit).toEqual({ cpu: null, memory_bytes: null });
    expect(usage?.series[0]).toEqual({
      at: '2026-10-18T11:00:00Z',
      cpu_percent: null,
      memory_bytes: null,
      net_in_bps: null,
      net_out_bps: null,
    });
    expect(usage?.now).toBeNull();
    expect(usage?.task).toBeNull();
  });

  it('refuses a reply that is no usage', () => {
    expect(pickUsage(null)).toBeNull();
    expect(pickUsage({ ...serverUsage(), subject: 'infra' })).toBeNull();
    expect(pickUsage({ ...serverUsage(), range: '7d' })).toBeNull();
    expect(pickUsage({ ...serverUsage(), series: [{ cpu_percent: 1 }] })).toBeNull();
    expect(pickUsage({ ...serverUsage(), id: null })).toBeNull();
  });

  it("reads an AWS resource's metrics, and the local stack's none", () => {
    expect(pickResourceUsage(wire(resourceUsage()))).toEqual(resourceUsage());
    expect(pickResourceUsage(wire({ ...localResourceUsage('nlb'), subject: 'infra' }))).toEqual(localResourceUsage('nlb'));
    expect(pickResourceUsage({ ...resourceUsage(), id: 'ec2' })).toBeNull();
    expect(pickResourceUsage({ ...resourceUsage(), metrics: { CPUUtilization: [{ at: 'x' }] } })).toBeNull();
  });

  it('reads the programs, with no cloud and no task reporting', () => {
    expect(pickPrograms(wire(programsResponse()))).toEqual(programsResponse());
    const local = pickPrograms({
      at: '2026-10-18T12:00:00Z',
      cloud: { configured: false, unavailable: 'AWS is not read here: no cluster', error: null },
      programs: [{ name: 'web', running: 1, desired: null, release: null, on_release: null, restarts_24h: 0, tasks: [{ id: 'abc123', reporting: true }] }],
    });
    expect(local?.cloud).toEqual({ configured: false, unavailable: 'AWS is not read here: no cluster', error: null });
    expect(local?.programs[0].desired).toBeNull();
    expect(local?.programs[0].cpu_percent).toBeNull();
    expect(local?.programs[0].tasks[0].last_status).toBeNull();
    expect(pickPrograms({ at: 'x', programs: [{ name: 'web' }] })).toBeNull();
  });

  it('asks for what the page names', async () => {
    get.mockResolvedValue({ status: 200, data: wire(programUsage({ task: 'abc' })) });
    const result = await fetchUsage('program', 'web', '48h', { task: 'abc' });
    expect(result.ok).toBe(true);
    expect(get).toHaveBeenCalledWith('/usage', { params: { subject: 'program', id: 'web', range: '48h', task: 'abc' }, signal: undefined });

    get.mockResolvedValue({ status: 200, data: wire(resourceUsage()) });
    await fetchResourceUsage('rds', '400d');
    expect(get).toHaveBeenLastCalledWith('/usage', { params: { subject: 'infra', id: 'rds', range: '400d' }, signal: undefined });

    get.mockResolvedValue({ status: 200, data: { programs: 'none' } });
    const programs = await fetchPrograms();
    expect(programs.ok).toBe(false);
    expect(get).toHaveBeenLastCalledWith('/programs', { signal: undefined });
  });
});
