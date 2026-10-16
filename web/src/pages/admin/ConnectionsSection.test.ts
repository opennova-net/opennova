import { flushPromises, mount } from '@vue/test-utils';
import { afterEach, beforeEach, describe, expect, it, vi, type MockInstance } from 'vitest';
import ConnectionsSection from './ConnectionsSection.vue';
import { endConnection, fetchConnections, stopListedServer } from '../../api/admin';
import type { AdminConnection, ConnectionsResponse } from '../../types/admin';
import { testRouter } from '../../test/router';

vi.mock('../../api/admin', () => ({ fetchConnections: vi.fn(), endConnection: vi.fn(), stopListedServer: vi.fn() }));

const connections = vi.mocked(fetchConnections);
const end = vi.mocked(endConnection);
const stop = vi.mocked(stopListedServer);

// Production's read of 2026-10-16 09:39 UTC, its handles and addresses replaced.
const NOW = Date.UTC(2026, 9, 16, 9, 39, 2);

function session(id: number, over: Partial<AdminConnection> = {}): AdminConnection {
  return {
    session_id: 1000 + id,
    lobby_id: 1,
    id,
    state: 'active',
    pn: 'NOVAWORLDUDP',
    identity: 'jop:cus2',
    created_ms: NOW - 3 * 60_000,
    last_seen_ms: NOW - 5_000,
    addr: `198.51.100.${id % 250}:32768`,
    account: null,
    host: null,
    ...over,
  };
}

function response(list: AdminConnection[]): ConnectionsResponse {
  return { count: list.length, heartbeat_timeout_ms: 240_000, connections: list };
}

describe('ConnectionsSection', () => {
  let wrapper: ReturnType<typeof mount> | null = null;
  let confirm: MockInstance<typeof window.confirm>;

  async function mountSection(list: AdminConnection[]) {
    connections.mockResolvedValue({ ok: true, value: response(list) });
    const router = await testRouter('/admin/sessions');
    wrapper = mount(ConnectionsSection, { global: { plugins: [router] } });
    await flushPromises();
    return wrapper;
  }

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

  it('names the sessions in words: no raw milliseconds, no protocol codes', async () => {
    const page = await mountSection([
      session(11, { identity: 'jop:cus2' }),
      session(12, { state: 'handshaking', pn: 'JOINTOPERATIONS', identity: '', created_ms: NOW - 30_000, last_seen_ms: NOW - 30_000 }),
      session(13, { state: 'closing', pn: 'SOMETHINGELSE', identity: '', created_ms: 0, last_seen_ms: 0 }),
    ]);
    const text = page.text();
    expect(page.get('h1').text()).toBe('NovaWorld sessions');
    expect(text).toContain('Game clients and hosts signed in to NovaWorld. 3 signed in; a session closes after 4 minutes without a keepalive.');
    expect(text).not.toMatch(/\bms\b/);
    expect(text).not.toContain('240000');
    expect(text).not.toMatch(/\bPN\b/);
    const headers = page.findAll('thead th').map((th) => th.text().replace(/[▲▼]/g, '').trim());
    expect(headers).toEqual(['Session', 'Account', 'Hosting', 'Game', 'Address', 'Connected', 'Last heard', 'Actions']);
    const rows = page.findAll('tr.session-row');
    // Newest first: 12 connected 30 s ago, 11 three minutes ago, 13 at no time it can have.
    expect(rows.map((r) => r.get('th').text())).toEqual(['12Signing in', '11Signed in', '13Signing out']);
    expect(rows[1].text()).toContain('NovaWorld lobby (any game)');
    expect(rows[1].text()).toContain('jop:cus2');
    expect(rows[1].text()).toContain('Not known');
    expect(rows[1].text()).toContain('Not hosting');
    expect(rows[1].text()).toContain('2026-10-16 09:36 UTC');
    expect(rows[1].text()).toContain('for 3 minutes');
    expect(rows[1].text()).toContain('just now');
    expect(rows[1].findAll('time')[1].attributes('title')).toMatch(/UTC$/);
    expect(rows[0].text()).toContain('Joint Operations');
    expect(rows[0].text()).toContain('Not signed in yet');
    // An unknown protocol name shows as it is, with a hint; no time is Unknown.
    const unknown = rows[2].findAll('td')[2].get('span');
    expect(unknown.text()).toBe('SOMETHINGELSE');
    expect(unknown.attributes('title')).toBe('A protocol name this site does not know');
    expect(rows[2].findAll('td')[4].text()).toBe('Unknown');
    expect(rows[2].findAll('td')[5].text()).toBe('Unknown');
  });

  it('reads production times as minutes and hours, never as a day count', async () => {
    const page = await mountSection([
      // The host: connected 02:27:21 UTC, heard 25 s ago.
      session(3879272710, { created_ms: Date.UTC(2026, 9, 16, 2, 27, 21), last_seen_ms: NOW - 25_000 }),
      // The steady-clock stamps the service once sent (27514593 ms since the machine's boot),
      // which read as January 1970: 20,741 days ago.
      session(2147483649, { created_ms: 27_514_593, last_seen_ms: 27_634_827 }),
    ]);
    const rows = page.findAll('tr.session-row');
    expect(rows[0].text()).toContain('2026-10-16 02:27 UTC');
    expect(rows[0].text()).toContain('for 7 hours 11 minutes');
    expect(rows[0].findAll('td')[5].text()).toBe('just now');
    expect(rows[1].findAll('td')[4].text()).toBe('Unknown');
    expect(rows[1].findAll('td')[5].text()).toBe('Unknown');
    expect(page.text()).not.toMatch(/\d+ days?/);
    expect(page.text()).not.toContain('1970');
  });

  it('names each session’s account and hosted server, each linked to its admin page', async () => {
    const page = await mountSection([
      session(1, {
        account: { id: 7, username: 'pilot', via: 'host_key' },
        host: { rid: 167772161, server_name: 'Retail Joint Ops', mission_name: 'River Patrol', players: 2, max_players: 32, listed: true, hosted_server_id: 4 },
      }),
      session(2, { created_ms: NOW - 60_000, account: { id: 9, username: 'gunner', via: 'login' } }),
      session(3, {
        created_ms: NOW - 120_000,
        host: { rid: 167772162, server_name: 'Garage Box', mission_name: '', players: 0, max_players: 16, listed: false, hosted_server_id: null },
      }),
    ]);
    const rows = page.findAll('tr.session-row');
    const [gunner, garage, pilot] = rows;
    expect(pilot.findAll('td')[0].get('a').attributes('href')).toBe('/admin/users/7');
    expect(pilot.text()).toContain('By its host key');
    expect(pilot.findAll('td')[1].get('a').attributes('href')).toBe('/admin/fleet/4');
    expect(pilot.text()).toContain('River Patrol, 2 of 32 players');
    expect(pilot.text()).toContain('Stop server');
    expect(gunner.findAll('td')[0].get('a').attributes('href')).toBe('/admin/users/9');
    expect(gunner.text()).toContain('By its game login');
    expect(gunner.text()).toContain('Not hosting');
    expect(gunner.text()).toContain('End');
    // A listing that is no hosted server has no page of its own; one held out of the browser says so.
    expect(garage.findAll('td')[1].find('a').exists()).toBe(false);
    expect(garage.text()).toContain('Garage Box');
    expect(garage.text()).toContain('Not listed: waiting for a host key that names an account');
    expect(garage.text()).toContain('Not known');
  });

  it('sorts by a header, the same header again reversing it', async () => {
    const page = await mountSection([
      session(5, { created_ms: NOW - 50_000, last_seen_ms: NOW - 1_000, account: { id: 1, username: 'zed', via: 'login' } }),
      session(6, { created_ms: NOW - 10_000, last_seen_ms: NOW - 9_000 }),
      session(7, { created_ms: NOW - 90_000, last_seen_ms: NOW - 4_000, account: { id: 2, username: 'Amy', via: 'login' } }),
    ]);
    const order = () => page.findAll('tr.session-row').map((r) => r.get('th span').text());
    const header = (label: string) => page.findAll('thead th').find((th) => th.text().startsWith(label))!;
    expect(order()).toEqual(['6', '5', '7']);
    expect(header('Connected').attributes('aria-sort')).toBe('descending');
    await header('Connected').get('button').trigger('click');
    expect(order()).toEqual(['7', '5', '6']);
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

  it('ends a session once confirmed, by its row id', async () => {
    const page = await mountSection([session(11, { account: { id: 9, username: 'gunner', via: 'login' } })]);
    confirm.mockReturnValueOnce(false);
    await page.get('tr.session-row button').trigger('click');
    expect(end).not.toHaveBeenCalled();
    await page.get('tr.session-row button').trigger('click');
    await flushPromises();
    expect(confirm).toHaveBeenLastCalledWith(expect.stringContaining("End gunner's NovaWorld session 11 (198.51.100.11:32768)?"));
    expect(end).toHaveBeenCalledWith(expect.objectContaining({ session_id: 1011, id: 11 }));
    expect(page.text()).toContain('Session 11 is ending.');
    expect(connections).toHaveBeenCalledTimes(2);
  });

  it('stops a hosting session’s server instead, and shows a refusal', async () => {
    const page = await mountSection([
      session(1, { host: { rid: 167772161, server_name: 'Retail Joint Ops', mission_name: 'River Patrol', players: 2, max_players: 32, listed: true, hosted_server_id: null } }),
    ]);
    stop.mockResolvedValueOnce({ ok: false, error: { code: 'not_hosting', status: 409, message: 'That server is no longer listed. Refresh the list.' } });
    await page.get('tr.session-row button').trigger('click');
    await flushPromises();
    expect(confirm).toHaveBeenLastCalledWith(expect.stringContaining('The match ends for the 2 players on it.'));
    expect(stop).toHaveBeenCalledWith(167772161);
    expect(end).not.toHaveBeenCalled();
    expect(page.text()).toContain('That server is no longer listed. Refresh the list.');
  });
});
