import { AxiosError, AxiosHeaders, type AxiosResponse } from 'axios';
import { beforeEach, describe, expect, it, vi } from 'vitest';
import { adminClient } from './client';
import { endConnection, fetchConnections, stopListedServer } from './admin';

vi.mock('./client', () => ({
  apiClient: { get: vi.fn(), post: vi.fn() },
  adminClient: { get: vi.fn(), post: vi.fn() },
}));

const get = vi.mocked(adminClient.get);
const post = vi.mocked(adminClient.post);

function reply(data: unknown, status = 200) {
  return { status, data } as AxiosResponse;
}

// A refusal as axios throws it.
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

// GET /api/admin/connections as production answered it (2026-10-16), with the
// times the service now sends (epoch ms) and its handles and addresses replaced.
const productionShape = {
  heartbeat_timeout_ms: 240000,
  count: 2,
  connections: [
    {
      session_id: 41,
      lobby_id: 1,
      addr: '198.51.100.20:32768',
      last_seen_ms: 1792120317793,
      created_ms: 1792096041000,
      identity: 'jop:cus2',
      pn: 'NOVAWORLDUDP',
      state: 'active',
      id: 3879272710,
      account: { id: 1, username: 'owner1', via: 'host_key' },
      host: {
        rid: 167772161,
        server_name: 'Retail Joint Ops',
        mission_name: 'River Patrol',
        players: 2,
        max_players: 32,
        listed: true,
        hosted_server_id: 1,
      },
    },
    {
      session_id: 57,
      lobby_id: 2,
      addr: '198.51.100.21:32769',
      last_seen_ms: 1792120335000,
      created_ms: 1792120218000,
      identity: 'jop:cus2',
      pn: 'NOVAWORLDUDP',
      state: 'active',
      id: 2147483649,
      account: null,
      host: null,
    },
  ],
};

describe('fetchConnections', () => {
  beforeEach(() => vi.clearAllMocks());

  it('reads each session with its account and listing', async () => {
    get.mockResolvedValue(reply(productionShape));
    const result = await fetchConnections();
    expect(get).toHaveBeenCalledWith('/connections', expect.anything());
    expect(result.ok).toBe(true);
    if (!result.ok) return;
    expect(result.value.count).toBe(2);
    expect(result.value.heartbeat_timeout_ms).toBe(240_000);
    const [host, player] = result.value.connections;
    expect(host).toEqual(productionShape.connections[0]);
    expect(player.account).toBeNull();
    expect(player.host).toBeNull();
    expect(player.created_ms).toBe(1792120218000);
  });

  it('reads a session without account or listing (an older service) as none', async () => {
    const { account: _a, host: _h, ...bare } = productionShape.connections[1];
    get.mockResolvedValue(reply({ ...productionShape, connections: [bare] }));
    const result = await fetchConnections();
    expect(result.ok && result.value.connections[0]).toMatchObject({ session_id: 57, id: 2147483649, account: null, host: null });
  });

  it('drops a malformed account or listing and refuses a malformed session', async () => {
    const session = productionShape.connections[0];
    get.mockResolvedValue(reply({ ...productionShape, connections: [{ ...session, account: { id: '1' }, host: { server_name: 'x' } }] }));
    const result = await fetchConnections();
    expect(result.ok && result.value.connections[0]).toMatchObject({ account: null, host: null });

    get.mockResolvedValue(reply({ ...productionShape, connections: [{ ...session, id: '3879272710' }] }));
    expect((await fetchConnections()).ok).toBe(false);
  });
});

describe('ending a session and stopping a listed server', () => {
  beforeEach(() => vi.clearAllMocks());

  it('ends a session by its row id', async () => {
    post.mockResolvedValue(reply({ status: 'queued', session_id: 57, id: 2147483649, addr: '198.51.100.21:32769' }, 202));
    const result = await endConnection({ session_id: 57 });
    expect(result.ok).toBe(true);
    expect(post).toHaveBeenCalledWith('/connections/57/end', undefined);
  });

  it('maps the end refusals to words, never the server text', async () => {
    post.mockRejectedValue(refusal(409, { error: 'hosting', rid: 167772161, message: 'raw server text' }));
    const hosting = await endConnection({ session_id: 41 });
    expect(!hosting.ok && hosting.error.code).toBe('hosting');
    expect(!hosting.ok && hosting.error.message).toContain('stop the server instead');
    expect(!hosting.ok && hosting.error.message).not.toContain('raw server text');

    post.mockRejectedValue(refusal(404, { error: 'not_found' }));
    const gone = await endConnection({ session_id: 41 });
    expect(!gone.ok && gone.error.code).toBe('not_found');
  });

  it('stops a listed server by its RID', async () => {
    post.mockResolvedValue(reply({ status: 'queued', rid: 167772161 }, 202));
    expect((await stopListedServer(167772161)).ok).toBe(true);
    expect(post).toHaveBeenCalledWith('/hosts/167772161/stop', undefined);

    post.mockRejectedValue(refusal(409, { error: 'not_hosting' }));
    const stopped = await stopListedServer(167772161);
    expect(!stopped.ok && stopped.error.message).toContain('no longer listed');
  });
});
