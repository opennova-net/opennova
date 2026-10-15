import { AxiosError, AxiosHeaders, type AxiosResponse } from 'axios';
import { beforeEach, describe, expect, it, vi } from 'vitest';
import { adminClient, apiClient } from './client';
import {
  consolePath,
  fetchConsolePlayers,
  fetchMyServer,
  fetchMyServers,
  linksToSupportTier,
  startHostedServer,
  stopHostedServer,
} from './hostedServer';
import { hostedServer, serversResponse, settings } from '../test/hostedFixtures';

vi.mock('./client', () => ({
  apiClient: { get: vi.fn(), post: vi.fn() },
  adminClient: { get: vi.fn(), post: vi.fn() },
}));

const get = vi.mocked(apiClient.get);
const post = vi.mocked(apiClient.post);
const adminGet = vi.mocked(adminClient.get);

function reply(data: unknown, status = 200) {
  return { status, data } as AxiosResponse;
}

// A refusal as axios throws it.
function refusal(status: number, data: unknown, headers: Record<string, string> = {}): AxiosError {
  const config = { headers: new AxiosHeaders() };
  return new AxiosError('refused', 'ERR_BAD_REQUEST', config, null, {
    status,
    statusText: '',
    data,
    headers,
    config,
  } as AxiosResponse);
}

describe('fetchMyServers', () => {
  beforeEach(() => vi.clearAllMocks());

  it('reads the entitlement, the data sets and the servers', async () => {
    const body = serversResponse([hostedServer(1), hostedServer(2, { state: 'stopped' })], {
      entitlement: { max_servers: 2, own_machines: 1, own_used: 0 },
    });
    get.mockResolvedValue(reply(body));
    const result = await fetchMyServers();
    expect(get).toHaveBeenCalledWith('/me/servers', expect.anything());
    expect(result).toEqual({ ok: true, value: body });
  });

  it('refuses a reply whose entitlement or any server is malformed', async () => {
    const good = serversResponse([hostedServer(1)]);
    get.mockResolvedValue(reply({ ...good, entitlement: { ...good.entitlement, max_servers: '3' } }));
    expect((await fetchMyServers()).ok).toBe(false);
    get.mockResolvedValue(reply({ ...good, entitlement: { ...good.entitlement, used: -1 } }));
    expect((await fetchMyServers()).ok).toBe(false);
    get.mockResolvedValue(reply({ ...good, entitlement: { ...good.entitlement, source: 'gift' } }));
    expect((await fetchMyServers()).ok).toBe(false);
    get.mockResolvedValue(reply({ ...good, entitlement: undefined }));
    expect((await fetchMyServers()).ok).toBe(false);
    const { own_machine: _drop, ...noOwnMachine } = hostedServer(2);
    get.mockResolvedValue(reply({ ...good, servers: [hostedServer(1), noOwnMachine] }));
    const result = await fetchMyServers();
    expect(result.ok).toBe(false);
    if (!result.ok) expect(result.error.code).toBe('unknown');
  });

  it('drops a malformed data set but keeps the rest', async () => {
    const good = serversResponse([]);
    get.mockResolvedValue(reply({ ...good, data_sets: [good.data_sets[0], { game: '' }] }));
    const result = await fetchMyServers();
    expect(result.ok && result.value.data_sets.map((d) => d.game)).toEqual(['jop_2_consumer']);
  });
});

describe('the start, the stop and one server', () => {
  beforeEach(() => vi.clearAllMocks());

  it('addresses each server by id', async () => {
    get.mockResolvedValue(reply({ server: hostedServer(7, { events: [] }) }));
    expect((await fetchMyServer(7)).ok).toBe(true);
    expect(get).toHaveBeenCalledWith('/me/servers/7', expect.anything());

    post.mockResolvedValue(reply({ server: hostedServer(8, { state: 'requested' }) }, 202));
    const started = await startHostedServer({ ...settings(), game_name: 'Night Ops' }, true);
    expect(started.ok && started.value.id).toBe(8);
    expect(post).toHaveBeenLastCalledWith('/me/servers', expect.objectContaining({ game_name: 'Night Ops', own_machine: true }));

    post.mockResolvedValue(reply({ server: hostedServer(8, { state: 'stopping' }) }, 202));
    await stopHostedServer(8);
    expect(post).toHaveBeenLastCalledWith('/me/servers/8/stop');
  });

  it('reads server_limit with its counts, and links it to the support tiers', async () => {
    post.mockRejectedValue(refusal(409, { error: 'server_limit', max_servers: 2, used: 2 }));
    const result = await startHostedServer(settings());
    expect(result.ok).toBe(false);
    if (result.ok) return;
    expect(result.error).toMatchObject({ code: 'server_limit', status: 409, maxServers: 2, used: 2 });
    expect(result.error.message).toContain('You may run 2 servers at once');
    expect(result.error.message).toContain('your slot frees up in about a minute');
    expect(linksToSupportTier(result.error.code)).toBe(true);
  });

  it('maps the entitlement refusals, never showing the server text', async () => {
    post.mockRejectedValue(refusal(403, { error: 'payment_past_due', message: 'raw server text' }));
    const pastDue = await startHostedServer(settings());
    expect(!pastDue.ok && pastDue.error.message).toContain('Stripe retries the card');
    expect(!pastDue.ok && pastDue.error.message).not.toContain('raw server text');

    post.mockRejectedValue(refusal(403, { error: 'not_entitled' }));
    const locked = await startHostedServer(settings());
    expect(!locked.ok && locked.error.code).toBe('not_entitled');
    expect(linksToSupportTier('not_entitled')).toBe(true);

    post.mockRejectedValue(refusal(409, { error: 'at_capacity' }));
    const full = await startHostedServer(settings());
    expect(!full.ok && full.error.code).toBe('at_capacity');
    expect(linksToSupportTier('at_capacity')).toBe(false);

    post.mockRejectedValue(refusal(400, { error: 'invalid_settings', field: 'max_players' }));
    const bad = await startHostedServer(settings());
    expect(!bad.ok && bad.error.field).toBe('max_players');

    post.mockRejectedValue(refusal(429, {}, { 'retry-after': '30' }));
    const limited = await stopHostedServer(1);
    expect(!limited.ok && limited.error).toMatchObject({ code: 'rate_limited', retryAfterSeconds: 30 });

    // A retired code reads as unknown, by its status.
    post.mockRejectedValue(refusal(409, { error: 'already_live' }));
    const retired = await startHostedServer(settings());
    expect(!retired.ok && retired.error.code).toBe('unknown');
  });
});

describe('the console routes', () => {
  beforeEach(() => vi.clearAllMocks());

  it("puts the owner's under the server's id and the admin's under the fleet", async () => {
    expect(consolePath({ kind: 'owner', id: 7 }, 'players')).toBe('/me/servers/7/players');
    expect(consolePath({ kind: 'owner', id: 7 }, 'rotation/2/next')).toBe('/me/servers/7/rotation/2/next');
    expect(consolePath({ kind: 'admin', id: 7 }, 'players')).toBe('/hosted/7/players');

    get.mockResolvedValue(reply({ players: [] }));
    await fetchConsolePlayers({ kind: 'owner', id: 7 });
    expect(get).toHaveBeenCalledWith('/me/servers/7/players', expect.anything());
    adminGet.mockResolvedValue(reply({ players: [] }));
    await fetchConsolePlayers({ kind: 'admin', id: 3 });
    expect(adminGet).toHaveBeenCalledWith('/hosted/3/players', expect.anything());
  });

  it('reads not_found and not_live as the server gone', async () => {
    get.mockRejectedValue(refusal(404, { error: 'not_found' }));
    const gone = await fetchConsolePlayers({ kind: 'owner', id: 7 });
    expect(!gone.ok && gone.error.code).toBe('not_found');
    get.mockRejectedValue(refusal(409, { error: 'no_live_server' }));
    const retired = await fetchConsolePlayers({ kind: 'owner', id: 7 });
    expect(!retired.ok && retired.error.code).toBe('unknown');
  });
});
