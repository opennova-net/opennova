import { AxiosError, AxiosHeaders, type AxiosResponse } from 'axios';
import { beforeEach, describe, expect, it, vi } from 'vitest';
import { adminClient } from './client';
import { fetchLogs, pickLogPage } from './logs';
import { logPage } from '../test/usageFixtures';

vi.mock('./client', () => ({
  apiClient: { get: vi.fn(), post: vi.fn() },
  adminClient: { get: vi.fn(), post: vi.fn() },
}));

const get = vi.mocked(adminClient.get);

const wire = (value: unknown): unknown => JSON.parse(JSON.stringify(value));

function refusal(status: number, data: unknown): AxiosError {
  const config = { headers: new AxiosHeaders() };
  return new AxiosError('refused', 'ERR_BAD_REQUEST', config, null, {
    status,
    statusText: '',
    data,
    headers: {},
    config,
  } as AxiosResponse);
}

describe('the log client', () => {
  beforeEach(() => vi.clearAllMocks());

  it('reads a page as the server sends it', () => {
    expect(pickLogPage(wire(logPage()))).toEqual(logPage());
    const program = pickLogPage({ subject: 'program', id: 'web', generation: null, task: null, source: 'file', lines: [{ at: null, text: 'x' }], before: null });
    expect(program).toEqual({
      subject: 'program',
      id: 'web',
      generation: null,
      task: null,
      source: 'file',
      group: null,
      stream: null,
      lines: [{ at: null, text: 'x' }],
      before: null,
    });
    expect(pickLogPage({ ...logPage(), lines: [{ at: 'x' }] })).toBeNull();
    expect(pickLogPage({ ...logPage(), subject: 'job' })).toBeNull();
  });

  it('asks for the start, the cursor and the filter the viewer names', async () => {
    get.mockResolvedValue({ status: 200, data: wire(logPage()) });
    const result = await fetchLogs({ subject: 'server', id: 12, generation: 2, before: 'f/123', limit: 200, q: 'joined' });
    expect(result.ok).toBe(true);
    expect(get).toHaveBeenCalledWith('/logs', {
      params: { subject: 'server', id: '12', generation: '2', before: 'f/123', limit: '200', q: 'joined' },
      signal: undefined,
    });
    await fetchLogs({ subject: 'program', id: 'web', task: 'abc' });
    expect(get).toHaveBeenLastCalledWith('/logs', { params: { subject: 'program', id: 'web', task: 'abc' }, signal: undefined });
  });

  it("tells the store's own refusals with the service's reason", async () => {
    get.mockRejectedValue(refusal(502, { error: 'logs_unavailable', message: 'AccessDeniedException: logs:GetLogEvents' }));
    const failed = await fetchLogs({ subject: 'machine', id: 3 });
    expect(failed.ok).toBe(false);
    if (failed.ok) return;
    expect(failed.error.code).toBe('logs_unavailable');
    expect(failed.error.message).toBe('The log store could not be read. Try again shortly. The service says: AccessDeniedException: logs:GetLogEvents');

    // Any other refusal keeps our words alone.
    get.mockRejectedValue(refusal(404, { error: 'not_found', message: 'no fleet machine has this id' }));
    const missing = await fetchLogs({ subject: 'machine', id: 3 });
    expect(missing.ok).toBe(false);
    if (missing.ok) return;
    expect(missing.error.message).toBe('It no longer exists. Refresh the list.');
  });
});
