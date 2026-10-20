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
  updateHostedServer,
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
    // One server on a shared machine and a dedicated machine free.
    const body = serversResponse([hostedServer(1), hostedServer(2, { state: 'stopped' })], {
      entitlement: { max_servers: 2, shared_servers: 1, own_machines: 1 },
    });
    expect(body.entitlement).toMatchObject({ used: 1, shared_used: 1, own_used: 0 });
    get.mockResolvedValue(reply(body));
    const result = await fetchMyServers();
    expect(get).toHaveBeenCalledWith('/me/servers', expect.anything());
    expect(result).toEqual({ ok: true, value: body });
  });

  it("reads the entitlement's tier and grant, none when left out", async () => {
    const body = serversResponse([], { entitlement: { source: 'comp', tier: 2, granted: true } });
    get.mockResolvedValue(reply(body));
    const granted = await fetchMyServers();
    expect(granted.ok && granted.value.entitlement).toMatchObject({ tier: 2, granted: true });
    const { tier: _tier, granted: _granted, ...older } = body.entitlement;
    get.mockResolvedValue(reply({ ...body, entitlement: { ...older } }));
    const plain = await fetchMyServers();
    expect(plain.ok && plain.value.entitlement).toMatchObject({ tier: null, granted: false });
  });

  it('refuses a reply whose entitlement or any server is malformed', async () => {
    const good = serversResponse([hostedServer(1)]);
    get.mockResolvedValue(reply({ ...good, entitlement: { ...good.entitlement, max_servers: '3' } }));
    expect((await fetchMyServers()).ok).toBe(false);
    get.mockResolvedValue(reply({ ...good, entitlement: { ...good.entitlement, used: -1 } }));
    expect((await fetchMyServers()).ok).toBe(false);
    get.mockResolvedValue(reply({ ...good, entitlement: { ...good.entitlement, source: 'gift' } }));
    expect((await fetchMyServers()).ok).toBe(false);
    // The shared machines' counts are what a start is judged by.
    const { shared_servers: _shared, ...uncounted } = good.entitlement;
    get.mockResolvedValue(reply({ ...good, entitlement: uncounted }));
    expect((await fetchMyServers()).ok).toBe(false);
    get.mockResolvedValue(reply({ ...good, entitlement: { ...good.entitlement, shared_used: 'one' } }));
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
    get.mockResolvedValue(reply({ ...good, data_sets: [good.data_sets[0], { id: 'x' }, { ...good.data_sets[1], limits: { max_players: { min: 5, max: 2 } } }] }));
    const result = await fetchMyServers();
    expect(result.ok && result.value.data_sets.map((d) => d.id)).toEqual([1]);
  });

  it('reads the data a server runs on, with defaults for what a reply leaves out', async () => {
    const server = hostedServer(3, {
      expansion: { id: 2, code: 'jox01', display_name: 'Escalation' },
      data: { base: { tree_id: 10, version: '1.7.5.5' }, expansion: { tree_id: 12, version: '1.7.5.5' } },
      update_available: true,
    });
    get.mockResolvedValue(reply(serversResponse([server])));
    const result = await fetchMyServers();
    expect(result.ok && result.value.servers[0]).toEqual(server);

    const { data_set: _d, expansion: _e, data: _t, update_available: _a, update_requested: _r, address: _s, ...bare } =
      hostedServer(4);
    get.mockResolvedValue(reply(serversResponse([bare as typeof server])));
    const filled = await fetchMyServers();
    expect(filled.ok && filled.value.servers[0]).toMatchObject({
      data_set: null,
      expansion: null,
      data: { base: null, expansion: null },
      update_available: false,
      update_requested: false,
      address: null,
    });
  });

  it('reads the static address a server runs at', async () => {
    const at = { id: 5, name: 'smoke', hostname: 'smoke.play.opennova.net', public_ip: '203.0.113.9', state: 'ready' };
    const server = hostedServer(3, { own_machine: true, address: at });
    get.mockResolvedValue(reply(serversResponse([server, hostedServer(4, { address: { ...at, id: 'x' } as never })])));
    const result = await fetchMyServers();
    expect(result.ok && result.value.servers[0]).toEqual(server);
    // A malformed one reads as none; the server still shows.
    expect(result.ok && result.value.servers[1].address).toBeNull();
    // A service with no zone names no host.
    get.mockResolvedValue(reply(serversResponse([hostedServer(3, { address: { ...at, hostname: null, public_ip: null } })])));
    const bare = await fetchMyServers();
    expect(bare.ok && bare.value.servers[0].address).toEqual({ ...at, hostname: null, public_ip: null });
  });

  it("reads where a start stands and why a server's last run ended", async () => {
    const server = hostedServer(3, {
      state: 'booting',
      phase: { name: 'downloading_data', bytes_done: 5, bytes_total: 10 },
    });
    const stopped = hostedServer(4, {
      state: 'failed',
      stopped_because: { cause: 'crash', limit: null, exit_code: 3, oom: true, line: 'out of memory' },
    });
    const limited = hostedServer(8, {
      state: 'stopped',
      stopped_because: { cause: 'limits', limit: { kind: 'shared_servers', count: 1 }, exit_code: null, oom: false, line: '' },
    });
    get.mockResolvedValue(reply(serversResponse([server, stopped, limited])));
    const result = await fetchMyServers();
    expect(result.ok && result.value.servers[0].phase).toEqual({ name: 'downloading_data', bytes_done: 5, bytes_total: 10 });
    expect(result.ok && result.value.servers[1].stopped_because).toEqual({ cause: 'crash', limit: null, exit_code: 3, oom: true, line: 'out of memory' });
    // A limits stop's limit reads; a malformed one reads as none.
    expect(result.ok && result.value.servers[2].stopped_because?.limit).toEqual({ kind: 'shared_servers', count: 1 });
    const malformed = hostedServer(9, {
      state: 'stopped',
      stopped_because: { cause: 'limits', limit: { kind: 'shared_servers', count: -1 }, exit_code: null, oom: false, line: '' },
    });
    get.mockResolvedValue(reply(serversResponse([malformed])));
    const unread = await fetchMyServers();
    expect(unread.ok && unread.value.servers[0].stopped_because).toEqual({ cause: 'limits', limit: null, exit_code: null, oom: false, line: '' });

    // A restart's cause reads.
    get.mockResolvedValue(
      reply(serversResponse([hostedServer(7, { state: 'provisioning', phase: { name: 'restarting', restart: 1, restart_limit: 3, cause: 'timeout' } })])),
    );
    const restarting = await fetchMyServers();
    expect(restarting.ok && restarting.value.servers[0].phase).toEqual({ name: 'restarting', restart: 1, restart_limit: 3, cause: 'timeout' });

    // A phase's line (the agent's reason, which the admin's server reply alone
    // carries) reads too: every server reply parses alike.
    const unfetched = hostedServer(6, {
      state: 'provisioning',
      phase: { name: 'data_unavailable', line: 'its data could not be fetched: aws s3 sync exited 1' },
    });
    get.mockResolvedValue(reply(serversResponse([unfetched])));
    const withLine = await fetchMyServers();
    expect(withLine.ok && withLine.value.servers[0].phase).toEqual({
      name: 'data_unavailable',
      line: 'its data could not be fetched: aws s3 sync exited 1',
    });

    // A malformed or missing one reads as none; the server still shows.
    get.mockResolvedValue(reply(serversResponse([hostedServer(5, { phase: { bytes_done: 1 } as never, stopped_because: undefined as never })])));
    const bare = await fetchMyServers();
    expect(bare.ok && bare.value.servers[0].phase).toBeNull();
    expect(bare.ok && bare.value.servers[0].stopped_because).toBeNull();
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

    // A static address goes with a dedicated machine alone.
    await startHostedServer({ ...settings(), game_name: 'Night Ops' }, true, 5);
    expect(post).toHaveBeenLastCalledWith('/me/servers', expect.objectContaining({ own_machine: true, address_id: 5 }));
    await startHostedServer({ ...settings(), game_name: 'Night Ops' }, false, 5);
    expect(post.mock.lastCall?.[1]).not.toHaveProperty('address_id');
    await startHostedServer({ ...settings(), game_name: 'Night Ops' }, true);
    expect(post.mock.lastCall?.[1]).not.toHaveProperty('address_id');

    post.mockResolvedValue(reply({ server: hostedServer(8, { state: 'stopping' }) }, 202));
    await stopHostedServer(8);
    expect(post).toHaveBeenLastCalledWith('/me/servers/8/stop');

    post.mockResolvedValue(reply({ server: hostedServer(8, { update_requested: true }) }, 202));
    const updated = await updateHostedServer(8);
    expect(post).toHaveBeenLastCalledWith('/me/servers/8/update');
    expect(updated.ok && updated.value.update_requested).toBe(true);
    post.mockRejectedValue(refusal(409, { error: 'up_to_date' }));
    const current = await updateHostedServer(8);
    expect(!current.ok && current.error.code).toBe('up_to_date');
    expect(!current.ok && current.error.message).toContain('already runs the current data');
  });

  it("reads server_limit with the shared machines' counts, and links it to the support tiers", async () => {
    post.mockRejectedValue(refusal(409, { error: 'server_limit', shared_servers: 2, shared_used: 2 }));
    const result = await startHostedServer(settings());
    expect(result.ok).toBe(false);
    if (result.ok) return;
    expect(result.error).toMatchObject({ code: 'server_limit', status: 409, sharedServers: 2, sharedUsed: 2 });
    expect(result.error.message).toContain('You may run 2 servers at once on shared machines');
    expect(result.error.message).toContain('counts toward your limit until it has stopped, in about a minute');
    expect(linksToSupportTier(result.error.code)).toBe(true);

    // An account with dedicated machines alone runs no server on a shared one.
    post.mockRejectedValue(refusal(409, { error: 'server_limit', shared_servers: 0, shared_used: 0 }));
    const dedicatedOnly = await startHostedServer(settings());
    expect(!dedicatedOnly.ok && dedicatedOnly.error.message).toBe(
      'Your account may run no servers on shared machines. Start this one on a dedicated machine.',
    );
  });

  it('reads no_own_machine with its counts', async () => {
    post.mockRejectedValue(refusal(409, { error: 'no_own_machine', own_machines: 2, own_used: 2 }));
    const busy = await startHostedServer(settings(), true);
    expect(!busy.ok && busy.error).toMatchObject({ code: 'no_own_machine', ownMachines: 2, ownUsed: 2 });
    expect(!busy.ok && busy.error.message).toContain('Each of your dedicated machines already runs a server.');
    expect(linksToSupportTier('no_own_machine')).toBe(true);
    post.mockRejectedValue(refusal(409, { error: 'no_own_machine', own_machines: 0, own_used: 0 }));
    const none = await startHostedServer(settings(), true);
    expect(!none.ok && none.error.message).toBe('You have no dedicated machine. Start this one on a shared machine.');
  });

  it('maps the entitlement refusals, never showing the server text', async () => {
    post.mockRejectedValue(refusal(403, { error: 'payment_past_due', message: 'raw server text' }));
    const pastDue = await startHostedServer(settings());
    expect(!pastDue.ok && pastDue.error.message).toContain('Stripe retries the card');
    // A dedicated machine's payment counts too: no tier assumed.
    expect(!pastDue.ok && pastDue.error.message).toContain('A payment of yours did not go through');
    expect(!pastDue.ok && pastDue.error.message).not.toContain('raw server text');

    post.mockRejectedValue(refusal(403, { error: 'not_entitled' }));
    const locked = await startHostedServer(settings());
    expect(!locked.ok && locked.error.code).toBe('not_entitled');
    expect(!locked.ok && locked.error.message).toBe(
      'Hosting a server needs a support tier, a dedicated machine or an admin grant.',
    );
    expect(linksToSupportTier('not_entitled')).toBe(true);

    post.mockRejectedValue(refusal(409, { error: 'at_capacity' }));
    const full = await startHostedServer(settings());
    expect(!full.ok && full.error.code).toBe('at_capacity');
    expect(linksToSupportTier('at_capacity')).toBe(false);

    post.mockRejectedValue(refusal(409, { error: 'address_unavailable', message: 'raw server text' }));
    const taken = await startHostedServer(settings(), true, 5);
    expect(!taken.ok && taken.error.code).toBe('address_unavailable');
    expect(!taken.ok && taken.error.message).toContain('That static address is not ready');
    expect(!taken.ok && taken.error.message).not.toContain('raw server text');

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
