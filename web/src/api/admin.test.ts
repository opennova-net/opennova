import { AxiosError, AxiosHeaders, type AxiosResponse } from 'axios';
import { beforeEach, describe, expect, it, vi } from 'vitest';
import { adminClient } from './client';
import {
  describeAdminError,
  drainLobby,
  endConnection,
  fetchListedHosts,
  fetchLobbies,
  pickMachine,
  pickMachineDetail,
  pickRollPlan,
  rollFleet,
  stopListedServer,
} from './admin';
import { HOSTD_IMAGE, machine, machineDetail, rollPlan } from '../test/adminFixtures';

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

describe('ending a session and stopping a listed server', () => {
  beforeEach(() => vi.clearAllMocks());

  it('ends a session by its row id', async () => {
    post.mockResolvedValue(reply({ status: 'queued', session_id: 57, id: 2147483649, addr: '198.51.100.21:32769' }, 202));
    const result = await endConnection(57);
    expect(result.ok).toBe(true);
    expect(post).toHaveBeenCalledWith('/connections/57/end', undefined);
  });

  it('maps the end refusals to words, never the server text', async () => {
    post.mockRejectedValue(refusal(409, { error: 'hosting', rid: 167772161, message: 'raw server text' }));
    const hosting = await endConnection(41);
    expect(!hosting.ok && hosting.error.code).toBe('hosting');
    expect(!hosting.ok && hosting.error.message).toContain('stop the server instead');
    expect(!hosting.ok && hosting.error.message).not.toContain('raw server text');

    post.mockRejectedValue(refusal(404, { error: 'not_found' }));
    const gone = await endConnection(41);
    expect(!gone.ok && gone.error.code).toBe('not_found');
  });

  it('reads the lobby instances, and drains one by its id', async () => {
    get.mockResolvedValue(
      reply({
        lobbies: [
          { id: 3, endpoint: '203.0.113.20:64206', state: 'active', task_id: '7a1e3c0ffee0', started_ms: 1792100000000, last_seen_ms: 1792120000000, sessions: 4 },
          { id: 4, endpoint: '203.0.113.20:64206', state: 'draining', started_ms: 1792100000000, last_seen_ms: 1792120000000, sessions: 0 },
        ],
      }),
    );
    const result = await fetchLobbies();
    expect(get).toHaveBeenCalledWith('/lobbies', expect.anything());
    expect(result.ok && result.value.lobbies).toEqual([
      { id: 3, endpoint: '203.0.113.20:64206', state: 'active', task_id: '7a1e3c0ffee0', started_ms: 1792100000000, last_seen_ms: 1792120000000, sessions: 4 },
      { id: 4, endpoint: '203.0.113.20:64206', state: 'draining', task_id: '', started_ms: 1792100000000, last_seen_ms: 1792120000000, sessions: 0 },
    ]);
    get.mockResolvedValue(reply({ lobbies: [{ endpoint: 'x' }] }));
    expect((await fetchLobbies()).ok).toBe(false);

    post.mockResolvedValue(reply({ status: 'draining', id: 3 }));
    expect((await drainLobby(3)).ok).toBe(true);
    expect(post).toHaveBeenCalledWith('/lobbies/3/drain', undefined);
    post.mockRejectedValue(refusal(404, { error: 'not_found' }));
    const gone = await drainLobby(9);
    expect(!gone.ok && gone.error.code).toBe('not_found');
  });

  it("maps an account delete's refusals to words: a subscription that grants, an open checkout", () => {
    const granting = describeAdminError(refusal(409, { error: 'active_subscription', message: 'raw server text' }));
    expect(granting.code).toBe('active_subscription');
    expect(granting.message).toContain('a support tier, a dedicated machine or a static address');
    expect(granting.message).toContain('once its subscriptions end');
    const open = describeAdminError(refusal(409, { error: 'checkout_open', message: 'raw server text' }));
    expect(open.code).toBe('checkout_open');
    expect(open.message).toContain('A checkout of this account is open');
    expect(open.message).not.toContain('raw server text');
  });

  it('stops a listed server by its RID', async () => {
    post.mockResolvedValue(reply({ status: 'queued', rid: 167772161 }, 202));
    expect((await stopListedServer(167772161)).ok).toBe(true);
    expect(post).toHaveBeenCalledWith('/hosts/167772161/stop', undefined);

    post.mockRejectedValue(refusal(409, { error: 'not_hosting' }));
    const stopped = await stopListedServer(167772161);
    expect(!stopped.ok && stopped.error.message).toContain('no longer listed');
  });

  it('reads every listed server with its owner and hosted server', async () => {
    get.mockResolvedValue(
      reply({
        count: 2,
        hosts: [
          {
            rid: 7, gsid: 'g7', game: 'jop_2_consumer', server_name: 'Night Raid', mission_name: 'River Patrol', game_type: 'TDM',
            players: 4, max_players: 32, host_ip: '198.51.100.9', host_port: 32768, peer_ip: '198.51.100.9',
            created_at: '2026-10-18 11:00:00', updated_at: '2026-10-18 11:59:00', hosted_server_id: null, owner: { id: 5, username: 'biggy' },
          },
          { rid: 8, game: 'dfx2_consumer', hosted_server_id: 12, owner: null },
        ],
      }),
    );
    const result = await fetchListedHosts();
    expect(get).toHaveBeenCalledWith('/hosts', { signal: undefined });
    expect(result.ok && result.value.hosts[0]).toMatchObject({ rid: 7, game_type: 'TDM', players: 4, owner: { id: 5, username: 'biggy' }, hosted_server_id: null });
    expect(result.ok && result.value.hosts[1]).toMatchObject({ rid: 8, server_name: '', players: 0, owner: null, hosted_server_id: 12 });

    get.mockResolvedValue(reply({ hosts: [{ game: 'jo' }] }));
    expect((await fetchListedHosts()).ok).toBe(false);
  });
});

describe('the fleet machines', () => {
  const wire = (value: unknown): unknown => JSON.parse(JSON.stringify(value));

  it('reads a machine with its agent, its use and the release as machine_json sends them', () => {
    // The route writes the row's id and the static address's as numbers.
    const sent = { ...(wire(machine('3', { address: { id: 7, name: 'smoke' } })) as object), id: 3 };
    expect(pickMachine(sent)).toEqual(machine('3', { address: { id: 7, name: 'smoke' } }));
  });

  it('reads a machine before its first report, and the fields a route leaves out', () => {
    const m = pickMachine({ id: 9, kind: 'own', state: 'launching', slots: 1, slots_used: 0, stats: null, agent_protocol: null });
    expect(m?.stats).toBeNull();
    expect(m?.agent_protocol).toBeNull();
    expect(m?.release).toEqual({ image_id: null, agent_image: null });
    expect(m?.outdated).toBeNull();
    expect(m?.address).toBeNull();
    expect(m?.lost_held).toBe(false);
    expect(pickMachine({ id: 9, kind: 'own', state: 'ready', slots: 1, slots_used: 0, stats: { cpu_percent: 'x' } })?.stats?.cpu_percent).toBeNull();
  });

  it("reads a machine's page and a roll's preview", () => {
    const detail = wire(machineDetail('3')) as { machine: Record<string, unknown> };
    detail.machine.id = 3;
    expect(pickMachineDetail(detail)).toEqual(machineDetail('3'));
    expect(pickMachineDetail({ machine: wire(machine('3')), events: [{ to: 'ready' }], trees: [] })).toBeNull();
    expect(pickMachineDetail({ machine: null, events: [], trees: [] })).toBeNull();

    const plan = wire(rollPlan()) as { machines: { id: unknown }[]; replace: unknown[] };
    plan.machines.forEach((m) => (m.id = Number(m.id)));
    plan.replace = [3];
    expect(pickRollPlan(plan)).toEqual(rollPlan());
    expect(pickRollPlan({ machines: [{ id: 3, kind: 'shared', action: 'drain' }] })).toBeNull();
    expect(HOSTD_IMAGE).toContain('opennova-hostd@sha256:');
  });

  it('rolls the machines named, as numbers', async () => {
    post.mockResolvedValue(reply({ draining: 2, machines: [3, 4] }, 202));
    expect(await rollFleet(['3', '4'])).toEqual({ ok: true, value: 2 });
    expect(post).toHaveBeenCalledWith('/fleet/roll', { machines: [3, 4] });
  });
});
