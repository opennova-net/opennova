import { describe, expect, it, vi } from 'vitest';
import { adminClient } from './client';
import { attentionCount, fetchAttention, pickAttention } from './attention';
import { attention } from '../test/usageFixtures';

vi.mock('./client', () => ({
  apiClient: { get: vi.fn(), post: vi.fn() },
  adminClient: { get: vi.fn(), post: vi.fn() },
}));

const get = vi.mocked(adminClient.get);

const wire = (value: unknown): unknown => JSON.parse(JSON.stringify(value));

describe('the attention client', () => {
  it('reads the findings and the alarms as the server sends them', async () => {
    expect(pickAttention(wire(attention()))).toEqual(attention());
    get.mockResolvedValue({ status: 200, data: wire(attention()) });
    const result = await fetchAttention();
    expect(result.ok).toBe(true);
    expect(get).toHaveBeenCalledWith('/attention', { signal: undefined });
  });

  it('reads a level it does not know as a warning, never dropping a finding', () => {
    const read = pickAttention({ findings: [{ level: 'critical', text: 'x' }], alarms_read: false, alarms_read_failed: true });
    expect(read?.findings).toEqual([{ level: 'warn', subject: '', text: 'x', since: null, link: null, source: '' }]);
    expect(read?.alarms).toEqual([]);
    expect(read?.alarms_read).toBe(false);
    expect(read?.alarms_read_failed).toBe(true);
    expect(pickAttention({ findings: [{ level: 'warn' }], alarms_read: true, alarms_read_failed: false })).toBeNull();
    expect(pickAttention({ findings: [] })).toBeNull();
    expect(pickAttention({ findings: [], alarms_read: false })).toBeNull();
  });

  it('counts what needs someone: danger and warn findings and firing alarms', () => {
    expect(attentionCount(attention())).toBe(3);
    expect(attentionCount(attention({ findings: [], alarms: [] }))).toBe(0);
  });
});
