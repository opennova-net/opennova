import { flushPromises, mount } from '@vue/test-utils';
import { afterEach, beforeEach, describe, expect, it, vi, type MockInstance } from 'vitest';
import NovaWorldSessions from './NovaWorldSessions.vue';
import { endConnection, fetchConnections, stopListedServer } from '../api/admin';
import type { AdminConnection, AdminConnectionHost, ConnectionsResponse } from '../types/admin';
import { testRouter } from '../test/router';

vi.mock('../api/admin', () => ({ fetchConnections: vi.fn(), endConnection: vi.fn(), stopListedServer: vi.fn() }));

const connections = vi.mocked(fetchConnections);
const end = vi.mocked(endConnection);
const stop = vi.mocked(stopListedServer);

// Production's read of 2026-10-16 09:39 UTC, its handles and addresses replaced.
const NOW = Date.UTC(2026, 9, 16, 9, 39, 2);
const JO = { slug: 'jop_2_consumer', display_name: 'Joint Operations' };
const TASK = '0f3c9a1b2d4e4f6a8b0c1d2e3f405162';

function session(id: number, over: Partial<AdminConnection> = {}): AdminConnection {
  return {
    session_id: 1000 + id,
    lobby_id: 1,
    lobby_task_id: TASK,
    id,
    state: 'active',
    pn: 'NOVAWORLDUDP',
    identity: 'jop:cus2',
    created_ms: NOW - 3 * 60_000,
    last_seen_ms: NOW - 5_000,
    addr: `198.51.100.${id % 250}:32768`,
    kind: 'client',
    title: JO,
    account: null,
    host: null,
    playing: null,
    ...over,
  };
}

function host(rid: number, over: Partial<AdminConnectionHost> = {}): AdminConnectionHost {
  return {
    rid,
    server_name: 'Retail Joint Ops',
    mission_name: 'River Patrol',
    players: 2,
    max_players: 32,
    listed: true,
    hosted_server_id: null,
    server_kind: 'owned',
    dedicated: false,
    ...over,
  };
}

// A server's session: it hosts the listing `h`.
function server(id: number, h: AdminConnectionHost, over: Partial<AdminConnection> = {}): AdminConnection {
  return session(id, { kind: 'server', host: h, ...over });
}

function response(list: AdminConnection[]): ConnectionsResponse {
  return { count: list.length, heartbeat_timeout_ms: 240_000, connections: list };
}

describe('NovaWorldSessions', () => {
  let wrapper: ReturnType<typeof mount> | null = null;
  let confirm: MockInstance<typeof window.confirm>;

  async function mountSection(list: AdminConnection[]) {
    connections.mockResolvedValue({ ok: true, value: response(list) });
    const router = await testRouter('/admin/novaworld');
    wrapper = mount(NovaWorldSessions, { global: { plugins: [router] } });
    await flushPromises();
    return wrapper;
  }

  const clientRows = () => wrapper!.findAll('.sessions-clients tr.session-row');
  const serverRows = () => wrapper!.findAll('.sessions-servers tr.session-row');

  beforeEach(() => {
    vi.clearAllMocks();
    vi.useFakeTimers({ toFake: ['Date'] });
    vi.setSystemTime(NOW);
    confirm = vi.spyOn(window, 'confirm').mockReturnValue(true);
    end.mockResolvedValue({ ok: true, value: undefined });
    stop.mockResolvedValue({ ok: true, value: undefined });
  });

  afterEach(() => {
    wrapper?.unmount();
    wrapper = null;
    confirm.mockRestore();
    vi.useRealTimers();
  });

  it('splits the sessions into clients and servers, each its own table', async () => {
    const page = await mountSection([session(11), server(12, host(167772161)), session(13, { created_ms: NOW - 60_000 })]);
    expect(page.get('h2').text()).toBe('Signed in now');
    expect(page.findAll('h3').map((h) => h.text())).toEqual(['Clients', 'Servers']);
    expect(page.get('.sessions-summary').text()).toBe(
      'Game clients and servers signed in to NovaWorld. 3 signed in: 2 clients and 1 server; a session closes after 4 minutes without a keepalive.',
    );
    const headers = (table: string) =>
      page.findAll(`.sessions-${table} thead th`).map((th) => th.text().replace(/[▲▼]/g, '').trim());
    expect(headers('clients')).toEqual(['Session', 'Account', 'Game', 'Lobby', 'Playing on', 'Address', 'Connected', 'Last heard', 'Actions']);
    expect(headers('servers')).toEqual(['Session', 'Account', 'Game', 'Lobby', 'Server', 'Players', 'Address', 'Connected', 'Last heard', 'Actions']);
    // Newest first in each: 13 connected a minute ago, 11 three.
    expect(clientRows().map((r) => r.get('th span').text())).toEqual(['13', '11']);
    expect(serverRows().map((r) => r.get('th span').text())).toEqual(['12']);
    expect(page.text()).not.toMatch(/\bms\b/);
    expect(page.text()).not.toContain('240000');
  });

  it('says so when a table is empty', async () => {
    const page = await mountSection([]);
    expect(page.get('.sessions-clients tbody').text()).toBe('No game client is signed in.');
    expect(page.get('.sessions-servers tbody').text()).toBe('No server is signed in.');
    expect(page.get('.sessions-summary').text()).toContain('0 signed in: 0 clients and 0 servers;');
  });

  it('names each session in words: its state, game, lobby and times', async () => {
    await mountSection([
      session(11),
      session(12, { state: 'handshaking', pn: 'JOINTOPERATIONS', identity: '', title: null, lobby_task_id: '', created_ms: NOW - 30_000, last_seen_ms: NOW - 30_000 }),
      session(13, { state: 'closing', pn: 'NOVAWORLDUDP', identity: 'http:push', title: null, lobby_id: 0, created_ms: 0, last_seen_ms: 0 }),
    ]);
    const [handshaking, signedIn, closing] = clientRows();
    expect(handshaking.get('th').text()).toBe('12Signing in');
    expect(signedIn.get('th').text()).toBe('11Signed in');
    expect(closing.get('th').text()).toBe('13Signing out');
    // The game: its games row, else the title its protocol name says; its game tag beneath.
    expect(signedIn.get('.session-game').text()).toBe('Joint Operationsjop:cus2');
    expect(handshaking.get('.session-game').text()).toBe('Joint Operations');
    expect(closing.get('.session-game').text()).toBe('Not knownhttp:push');
    // The lobby, linked to its task's usage on the lobby program's page.
    const lobby = signedIn.get('.session-lobby a');
    expect(lobby.text()).toBe('Lobby 1');
    expect(lobby.attributes('href')).toBe(`/admin/infrastructure/programs/lobby?task=${TASK}`);
    expect(signedIn.get('.session-lobby').text()).toContain(TASK.slice(0, 12));
    expect(handshaking.get('.session-lobby a').attributes('href')).toBe('/admin/infrastructure/programs/lobby');
    expect(closing.get('.session-lobby').text()).toBe('None');
    // Its account, times and address.
    expect(signedIn.get('.session-account').text()).toBe('Not known');
    expect(handshaking.get('.session-account').text()).toBe('Not signed in yet');
    expect(signedIn.get('.session-connected').text()).toContain('2026-10-16 09:36 UTC');
    expect(signedIn.get('.session-connected').text()).toContain('for 3 minutes');
    expect(signedIn.get('.session-heard').text()).toBe('just now');
    expect(signedIn.get('.session-addr').text()).toBe('198.51.100.11:32768');
    expect(closing.get('.session-connected').text()).toBe('Unknown');
    expect(closing.get('.session-heard').text()).toBe('Unknown');
  });

  it('reads production times as minutes and hours, never as a day count', async () => {
    await mountSection([
      // The host: connected 02:27:21 UTC, heard 25 s ago.
      session(3879272710, { created_ms: Date.UTC(2026, 9, 16, 2, 27, 21), last_seen_ms: NOW - 25_000 }),
      // The steady-clock stamps the service once sent (27514593 ms since the machine's boot),
      // which read as January 1970: 20,741 days ago.
      session(2147483649, { created_ms: 27_514_593, last_seen_ms: 27_634_827 }),
    ]);
    const rows = clientRows();
    expect(rows[0].get('.session-connected').text()).toContain('2026-10-16 02:27 UTC');
    expect(rows[0].get('.session-connected').text()).toContain('for 7 hours 11 minutes');
    expect(rows[0].get('.session-heard').text()).toBe('just now');
    expect(rows[1].get('.session-connected').text()).toBe('Unknown');
    expect(rows[1].get('.session-heard').text()).toBe('Unknown');
    expect(wrapper!.text()).not.toMatch(/\d+ days?/);
    expect(wrapper!.text()).not.toContain('1970');
  });

  it('names the server each client plays on, linked to its listing or its hosted server', async () => {
    await mountSection([
      session(1, {
        created_ms: NOW - 10_000,
        account: { id: 9, username: 'gunner', via: 'login' },
        playing: { rid: 167772161, addr: '203.0.113.7:32768', since_ms: NOW - 120_000, listed: true, server_name: 'Retail Joint Ops', hosted_server_id: null },
      }),
      session(2, {
        created_ms: NOW - 20_000,
        playing: { rid: 167772162, addr: '203.0.113.8:32771', since_ms: NOW - 60_000, listed: true, server_name: 'Fleet Box', hosted_server_id: 4 },
      }),
      session(3, {
        created_ms: NOW - 30_000,
        playing: { rid: null, addr: '192.168.1.20:32768', since_ms: NOW - 30_000, listed: false, server_name: null, hosted_server_id: null },
      }),
      session(4, { created_ms: NOW - 40_000 }),
    ]);
    const [retail, fleet, lan, idle] = clientRows();
    expect(retail.get('.session-account a').attributes('href')).toBe('/admin/users/9');
    expect(retail.get('.session-account').text()).toContain('By its game login');
    expect(retail.get('.session-playing a').attributes('href')).toBe('/lobby/167772161');
    expect(retail.get('.session-playing').text()).toContain('Retail Joint Ops');
    expect(retail.get('.session-playing').text()).toContain('203.0.113.7:32768, for 2 minutes');
    expect(fleet.get('.session-playing a').attributes('href')).toBe('/admin/fleet/4');
    expect(fleet.get('.session-playing').text()).toContain('Fleet Box');
    // A server no lobby lists: its address alone.
    expect(lan.get('.session-playing').find('a').exists()).toBe(false);
    expect(lan.get('.session-playing').text()).toContain('192.168.1.20:32768');
    expect(lan.get('.session-playing').text()).toContain('Not listed, for 30 seconds');
    expect(idle.get('.session-playing').text()).toBe('Not playing');
    expect(retail.text()).toContain('End');
  });

  it('names each server’s listing, what it is and its players, each linked to its page', async () => {
    await mountSection([
      server(1, host(167772161, { server_kind: 'hosted', hosted_server_id: 4, dedicated: true }), {
        created_ms: NOW - 10_000,
        account: { id: 7, username: 'pilot', via: 'host_key' },
      }),
      server(2, host(167772162, { server_name: 'Den Box' }), { created_ms: NOW - 20_000 }),
      server(3, host(167772163, { server_name: 'Garage Box', mission_name: '', players: 0, max_players: 16, listed: false, server_kind: 'anonymous', dedicated: null }), {
        created_ms: NOW - 30_000,
        title: null,
        pn: 'NOVAWORLDUDP',
      }),
      // Two clients on the Den Box, one on the fleet's.
      session(4, { playing: { rid: 167772162, addr: '203.0.113.9:32768', since_ms: NOW - 5_000, listed: true, server_name: 'Den Box', hosted_server_id: null } }),
      session(5, { playing: { rid: 167772162, addr: '203.0.113.9:32768', since_ms: NOW - 5_000, listed: true, server_name: 'Den Box', hosted_server_id: null } }),
      session(6, { playing: { rid: 167772161, addr: '203.0.113.7:32768', since_ms: NOW - 5_000, listed: true, server_name: 'Retail Joint Ops', hosted_server_id: 4 } }),
    ]);
    const [fleet, den, garage] = serverRows();
    expect(fleet.get('.session-account a').attributes('href')).toBe('/admin/users/7');
    expect(fleet.get('.session-account').text()).toContain('By its host key');
    expect(fleet.get('.session-server a').attributes('href')).toBe('/admin/fleet/4');
    expect(fleet.get('.session-server').text()).toContain('River Patrol');
    expect(fleet.get('.session-server').text()).toContain('Hosted server, Serve Only');
    expect(fleet.get('.session-players').text()).toBe('2 of 32 1 client above');
    expect(den.get('.session-server a').attributes('href')).toBe('/lobby/167772162');
    expect(den.get('.session-server').text()).toContain("A player's own, Listen (plays too)");
    expect(den.get('.session-players').text()).toContain('2 clients above');
    // One held out of the browser has no page; nor players.
    expect(garage.get('.session-server').find('a').exists()).toBe(false);
    expect(garage.get('.session-server').text()).toContain('Garage Box');
    expect(garage.get('.session-server').text()).toContain('Not listed: waiting for a host key that names an account');
    expect(garage.get('.session-server').text()).toContain('Anonymous');
    expect(garage.get('.session-players').text()).toBe('None listed');
    expect(garage.get('.session-game').text()).toContain('Not known');
    expect(fleet.text()).toContain('Stop server');
  });

  it('sorts both tables by a header, the same header again reversing it', async () => {
    const page = await mountSection([
      session(5, { created_ms: NOW - 50_000, last_seen_ms: NOW - 1_000, account: { id: 1, username: 'zed', via: 'login' } }),
      session(6, { created_ms: NOW - 10_000, last_seen_ms: NOW - 9_000 }),
      session(7, { created_ms: NOW - 90_000, last_seen_ms: NOW - 4_000, account: { id: 2, username: 'Amy', via: 'login' } }),
      server(8, host(1), { created_ms: NOW - 70_000 }),
      server(9, host(2), { created_ms: NOW - 20_000 }),
    ]);
    const order = () => clientRows().map((r) => r.get('th span').text());
    const servers = () => serverRows().map((r) => r.get('th span').text());
    const header = (label: string) => page.findAll('.sessions-clients thead th').find((th) => th.text().startsWith(label))!;
    expect(order()).toEqual(['6', '5', '7']);
    expect(servers()).toEqual(['9', '8']);
    expect(header('Connected').attributes('aria-sort')).toBe('descending');
    await header('Connected').get('button').trigger('click');
    expect(order()).toEqual(['7', '5', '6']);
    expect(servers()).toEqual(['8', '9']);
    expect(header('Connected').attributes('aria-sort')).toBe('ascending');
    // Names ascending, ignoring case; a session with no account last.
    await header('Account').get('button').trigger('click');
    expect(order()).toEqual(['7', '5', '6']);
    expect(header('Connected').attributes('aria-sort')).toBeUndefined();
    await header('Account').get('button').trigger('click');
    expect(order()).toEqual(['5', '7', '6']);
    await header('Last heard').get('button').trigger('click');
    expect(order()).toEqual(['5', '7', '6']);
    await header('Session').get('button').trigger('click');
    expect(order()).toEqual(['5', '6', '7']);
  });

  it('ends a client’s session once confirmed, by its row id', async () => {
    const page = await mountSection([session(11, { account: { id: 9, username: 'gunner', via: 'login' } })]);
    confirm.mockReturnValueOnce(false);
    await page.get('.sessions-clients tr.session-row button').trigger('click');
    expect(end).not.toHaveBeenCalled();
    await page.get('.sessions-clients tr.session-row button').trigger('click');
    await flushPromises();
    expect(confirm).toHaveBeenLastCalledWith(expect.stringContaining("End gunner's NovaWorld session 11 (198.51.100.11:32768)?"));
    expect(end).toHaveBeenCalledWith(expect.objectContaining({ session_id: 1011, id: 11 }));
    expect(page.text()).toContain('Session 11 is ending.');
    expect(connections).toHaveBeenCalledTimes(2);
  });

  it('stops a server instead, and shows a refusal', async () => {
    const page = await mountSection([server(1, host(167772161))]);
    stop.mockResolvedValueOnce({ ok: false, error: { code: 'not_hosting', status: 409, message: 'That server is no longer listed. Refresh the list.' } });
    await page.get('.sessions-servers tr.session-row button').trigger('click');
    await flushPromises();
    expect(confirm).toHaveBeenLastCalledWith(expect.stringContaining('The match ends for the 2 players on it.'));
    expect(stop).toHaveBeenCalledWith(167772161);
    expect(end).not.toHaveBeenCalled();
    expect(page.text()).toContain('That server is no longer listed. Refresh the list.');
  });

  it('says what happened once: the live region is the box itself', async () => {
    const page = await mountSection([session(11)]);
    await page.get('.sessions-clients tr.session-row button').trigger('click');
    await flushPromises();
    expect(page.text().split('Session 11 is ending.')).toHaveLength(2);
    expect(page.get('[role="status"]').text()).toBe('Session 11 is ending.');
  });

  it("reads again when asked (the page's Refresh)", async () => {
    const page = await mountSection([session(11)]);
    expect(connections).toHaveBeenCalledTimes(1);
    await (page.vm as unknown as { refresh: () => Promise<void> }).refresh();
    expect(connections).toHaveBeenCalledTimes(2);
  });
});
