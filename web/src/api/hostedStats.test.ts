import { AxiosError, AxiosHeaders } from 'axios';
import { describe, expect, it, vi } from 'vitest';
import { adminClient } from './client';
import { describeStatsError, fetchFleetUsage, parseFleetUsage } from './hostedStats';
import { fleetUsageReply } from '../test/statsFixtures';

vi.mock('./client', () => ({
  apiClient: { get: vi.fn() },
  adminClient: { get: vi.fn() },
}));

const adminGet = vi.mocked(adminClient.get);

function refusal(status: number, data: unknown): AxiosError {
  const config = { headers: new AxiosHeaders() };
  return new AxiosError('refused', 'ERR_BAD_REQUEST', config, null, {
    status,
    statusText: '',
    headers: {},
    config,
    data,
  });
}

describe('describeStatsError', () => {
  it("maps the stats routes' range refusal (invalid_field) to its message", () => {
    const error = describeStatsError(refusal(400, { error: 'invalid_field', message: 'raw server text' }));
    expect(error.code).toBe('invalid_field');
    expect(error.message).toContain('could not read the request');
    expect(error.message).not.toContain('raw server text');
  });
});

describe('parseFleetUsage', () => {
  it('reads the totals, the machines and the servers, machine ids as text', () => {
    const usage = parseFleetUsage(fleetUsageReply());
    expect(usage).not.toBeNull();
    expect(usage!.totals).toMatchObject({ servers_live: 4, machines_live: 2, slots: 13, slots_used: 3, reporting: 1, players: 6 });
    expect(usage!.machines.map((m) => [m.id, m.kind, m.slots_used])).toEqual([
      ['3', 'shared', 2],
      ['4', 'own', 1],
    ]);
    expect(usage!.servers.map((s) => [s.id, s.machine_ref, s.slot])).toEqual([
      [12, '3', 0],
      [7, '3', 1],
      [9, '4', 0],
      [15, null, null],
    ]);
    const running = usage!.servers[1];
    expect(running).toMatchObject({ owner: { id: 1, username: 'owner1' }, cpu_percent: 25, players: 6, max_players: 32 });
    expect(running.window).toEqual({ minutes: 60, cpu_percent_mean: 18.4, cpu_percent_peak: 61, memory_bytes_peak: 310 * 1024 * 1024, egress_bytes: 520_000_000 });
    // No sample: every figure null, the window still read.
    expect(usage!.servers[2]).toMatchObject({ owner: null, sample_at: null, memory_bytes: null, egress_bytes_per_second: null });
    expect(usage!.servers[2].window.egress_bytes).toBe(20_000_000);
  });

  it('refuses a reply with a malformed total, machine or server', () => {
    const noTotal = fleetUsageReply();
    delete (noTotal.totals as Record<string, unknown>).memory_bytes;
    expect(parseFleetUsage(noTotal)).toBeNull();
    const badMachine = fleetUsageReply();
    (badMachine.machines as Record<string, unknown>[])[0].kind = 'tiny';
    expect(parseFleetUsage(badMachine)).toBeNull();
    const badServer = fleetUsageReply();
    delete (badServer.servers as Record<string, unknown>[])[1].window;
    expect(parseFleetUsage(badServer)).toBeNull();
    expect(parseFleetUsage(null)).toBeNull();
  });
});

describe('fetchFleetUsage', () => {
  it('reads GET /api/admin/hosted/usage', async () => {
    adminGet.mockResolvedValueOnce({ status: 200, data: fleetUsageReply() });
    const result = await fetchFleetUsage();
    expect(adminGet).toHaveBeenLastCalledWith('/hosted/usage', { signal: undefined });
    expect(result.ok && result.value.totals.servers_live).toBe(4);
  });

  it('answers a refusal in words, never the server text', async () => {
    adminGet.mockRejectedValueOnce(refusal(403, { error: 'forbidden', message: 'raw server text' }));
    const result = await fetchFleetUsage();
    expect(result.ok).toBe(false);
    if (!result.ok) {
      expect(result.error.code).toBe('forbidden');
      expect(result.error.message).toBe('This account is not an admin.');
    }
    adminGet.mockResolvedValueOnce({ status: 200, data: { at: 'now' } });
    const malformed = await fetchFleetUsage();
    expect(!malformed.ok && malformed.error.code).toBe('unknown');
  });
});
