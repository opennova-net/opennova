import { flushPromises, mount } from '@vue/test-utils';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import type { Router } from 'vue-router';
import ServersPage from './ServersPage.vue';
import { fetchMyServers, stopHostedServer } from '../../api/hostedServer';
import { fetchAccountStats } from '../../api/hostedStats';
import { fetchListingGames, listMyServers, stopHosting } from '../../api/myServers';
import { confirmAction } from '../../composables/useConfirm';
import { hostedServer, listedServer, serversResponse } from '../../test/hostedFixtures';
import { testRouter } from '../../test/router';
import type { HostedServersResponse } from '../../types/hostedServer';

vi.mock('../../api/hostedServer', async (importOriginal) => ({
  ...(await importOriginal<typeof import('../../api/hostedServer')>()),
  fetchMyServers: vi.fn(),
  stopHostedServer: vi.fn(),
}));
vi.mock('../../api/myServers', () => ({ listMyServers: vi.fn(), stopHosting: vi.fn(), fetchListingGames: vi.fn() }));
vi.mock('../../api/hostedStats', () => ({ fetchAccountStats: vi.fn() }));
vi.mock('../../composables/useConfirm', () => ({ confirmAction: vi.fn() }));

const servers = vi.mocked(fetchMyServers);
const hosts = vi.mocked(listMyServers);

// Server 2 is listed as rid 21 (its own listing joins its row); rid 30 is a
// game the account hosts itself.
const LISTING_21 = {
  rid: 21,
  server_name: 'Two',
  mission_name: 'jo_two.bms',
  game_type: '',
  players: 3,
  max_players: 24,
  host_ip: '203.0.113.9',
  host_port: 32770,
};

let wrapper: ReturnType<typeof mount> | null = null;
let router: Router;

async function mountPage(data: HostedServersResponse, path = '/me/servers') {
  servers.mockResolvedValue({ ok: true, value: data });
  router = await testRouter(path);
  wrapper = mount(ServersPage, { global: { plugins: [router] }, attachTo: document.body });
  await flushPromises();
  return wrapper;
}

const rows = () => wrapper!.findAll('tr.server-row');
const meta = () => wrapper!.get('.page-meta');
const link = (text: string) => wrapper!.findAll('a').find((a) => a.text() === text);
const button = (text: string) => wrapper!.findAll('button').find((b) => b.text() === text);

describe('ServersPage', () => {
  beforeEach(() => {
    vi.clearAllMocks();
    hosts.mockResolvedValue({ ok: true, value: [] });
    vi.mocked(fetchListingGames).mockResolvedValue(new Map([[30, 'Joint Operations']]));
    vi.mocked(confirmAction).mockResolvedValue(true);
  });

  afterEach(() => {
    wrapper?.unmount();
    wrapper = null;
  });

  it('heads the page with its tabs, the allowance in words and a start while there is room', async () => {
    await mountPage(serversResponse([hostedServer(1), hostedServer(2)]));
    expect(wrapper!.get('h1').text()).toBe('Servers');
    expect(wrapper!.findAll('.tab-nav a').map((a) => [a.text(), a.attributes('href')])).toEqual([
      ['Servers', '/me/servers'],
      ['Stats', '/me/servers/stats'],
    ]);
    expect(meta().text()).toBe('2 of 3 shared servers in use');
    expect(wrapper!.get('#servers-allowance-shared').attributes('title')).toBe('From your tier');
    const start = link('Start a server');
    expect(start?.attributes('href')).toBe('/me/servers/new');
    expect(start?.attributes('aria-describedby')).toBe('servers-allowance-shared');
    expect(wrapper!.find('.servers-block').exists()).toBe(false);
  });

  it('says both pools in one line when the account holds a dedicated machine', async () => {
    await mountPage(serversResponse([hostedServer(1), hostedServer(2)], { entitlement: { max_servers: 3, tier: 2, own_machines: 1 } }));
    expect(meta().findAll('span').map((s) => s.text())).toEqual(['2 of 2 shared servers in use', '·', '0 of 1 dedicated machine in use']);
    expect(link('Start a server')?.attributes('aria-describedby')).toBe('servers-allowance-shared servers-allowance-own');
  });

  it('shows no stats: they have a tab of their own', async () => {
    await mountPage(serversResponse([hostedServer(1)]));
    expect(wrapper!.find('#servers-stats-heading').exists()).toBe(false);
    expect(fetchAccountStats).not.toHaveBeenCalled();
  });

  it('lists the hosted servers and the games hosted yourself in one table', async () => {
    hosts.mockResolvedValue({
      ok: true,
      value: [listedServer(21, { hosted_server_id: 2, server_name: 'Two', players: 5, max_players: 24, mission_name: 'River Patrol' }), listedServer(30)],
    });
    await mountPage(serversResponse([hostedServer(1), hostedServer(2, { listing: LISTING_21 })]));
    expect(wrapper!.findAll('thead th').map((th) => th.text())).toEqual(['Server', 'Map', 'Players', 'Runs on', 'State', 'Since', 'Actions']);
    expect(rows()).toHaveLength(3);
    const [one, two, listed] = rows();
    expect(one.get('th a').attributes('href')).toBe('/me/servers/1');
    expect(two.get('th a').attributes('href')).toBe('/me/servers/2');
    // A hosted server's own listing names it, its map and players.
    expect(two.get('th a').text()).toBe('Two');
    expect(two.get('.server-game').text()).toBe('Joint Operations · TDM');
    expect(two.get('.server-map').text()).toBe('River Patrol');
    expect(two.text()).toContain('5/24');
    expect(two.get('.server-runs-on').text()).toBe('Shared machine');
    expect(two.get('.badge').text()).toBe('Running');
    expect(two.findAll('a').find((a) => a.text().startsWith('Console'))?.attributes('href')).toBe('/me/servers/2/console');
    // The game hosted yourself, to its own page; its listing is not a row twice.
    expect(listed.get('th a').attributes('href')).toBe('/me/servers/hosts/30');
    expect(listed.get('.server-game').text()).toBe('Joint Operations · TDM');
    expect(listed.get('.server-runs-on').text()).toBe('Your game');
    expect(listed.get('.badge').text()).toBe('Listed');
  });

  it('says where a dedicated server runs, with its static address', async () => {
    const address = { id: 4, name: 'raid-host', hostname: null, public_ip: '198.51.100.4', state: 'ready' };
    await mountPage(serversResponse([hostedServer(1, { own_machine: true, address })], { entitlement: { own_machines: 1 } }));
    expect(rows()[0].get('.server-runs-on').text().replace(/\s+/g, ' ')).toBe('Dedicated machine raid-host');
  });

  it('says where a start stands under its state', async () => {
    await mountPage(serversResponse([hostedServer(1, { state: 'provisioning', phase: { name: 'launching_machine' } })]));
    expect(rows()[0].get('.badge').text()).toBe('Starting');
    expect(rows()[0].get('.server-phase').text()).toBe('Starting a machine for it.');
  });

  it('filters by the state in the URL, the ended servers with a start again', async () => {
    hosts.mockResolvedValue({ ok: true, value: [listedServer(30)] });
    const data = serversResponse([hostedServer(1), hostedServer(5, { state: 'stopped' })]);
    await mountPage(data);
    expect(rows().map((r) => r.get('th a').text())).toEqual(['Server 1', 'Listed 30']);
    const chips = wrapper!.get('[aria-label="State"]').findAll('a');
    expect(chips.map((a) => [a.text(), a.attributes('href'), a.attributes('aria-current')])).toEqual([
      ['Live', '/me/servers', 'true'],
      ['Ended', '/me/servers?state=ended', undefined],
      ['All', '/me/servers?state=all', undefined],
    ]);
    wrapper!.unmount();

    await mountPage(data, '/me/servers?state=ended');
    expect(rows().map((r) => r.get('th a').text())).toEqual(['Server 5']);
    expect(rows()[0].get('.badge').text()).toBe('Stopped');
    expect(link('Start again: Server 5')?.attributes('href')).toBe('/me/servers/new?from=5');
    wrapper!.unmount();

    await mountPage(data, '/me/servers?state=all');
    expect(rows().map((r) => r.get('th a').text())).toEqual(['Server 1', 'Listed 30', 'Server 5']);
  });

  it('keeps an ended server started again since behind its history', async () => {
    const again = hostedServer(6, { settings: { ...hostedServer(6).settings, game_name: 'Server 5' } });
    await mountPage(serversResponse([again, hostedServer(5, { state: 'stopped' })]), '/me/servers?state=ended');
    expect(wrapper!.find('tr.table-state').text()).toBe('None ended.');
    await button('Show history (1)')!.trigger('click');
    expect(rows()).toHaveLength(1);
  });

  it('stops a hosted server after a one-sentence confirmation', async () => {
    vi.mocked(stopHostedServer).mockResolvedValue({ ok: true, value: hostedServer(1, { state: 'stopping' }) });
    await mountPage(serversResponse([hostedServer(1)]));
    await button('Stop Server 1')!.trigger('click');
    await flushPromises();
    expect(confirmAction).toHaveBeenCalledWith('Stop Server 1? Its match ends for everyone.');
    expect(stopHostedServer).toHaveBeenCalledWith(1);
    expect(wrapper!.get('[role="status"]').text()).toBe('Server 1 is stopping.');
  });

  it('stops hosting a game hosted yourself after a confirmation, and drops its row', async () => {
    hosts.mockResolvedValue({ ok: true, value: [listedServer(30)] });
    vi.mocked(stopHosting).mockResolvedValue({ ok: true, value: { rid: 30, status: 'queued', statement: 'ServerStopHosting' } });
    await mountPage(serversResponse([]));
    await button('Stop Listed 30')!.trigger('click');
    await flushPromises();
    expect(confirmAction).toHaveBeenCalledWith('Stop hosting Listed 30? Its match ends for everyone.');
    expect(stopHosting).toHaveBeenCalledWith(30);
    expect(rows()).toHaveLength(0);
    expect(wrapper!.get('[role="status"]').text()).toBe('Stopped hosting Listed 30.');
  });

  it('stops nothing when the stop is not confirmed', async () => {
    vi.mocked(confirmAction).mockResolvedValue(false);
    hosts.mockResolvedValue({ ok: true, value: [listedServer(30)] });
    await mountPage(serversResponse([hostedServer(1)]));
    await button('Stop Server 1')!.trigger('click');
    await button('Stop Listed 30')!.trigger('click');
    await flushPromises();
    expect(stopHostedServer).not.toHaveBeenCalled();
    expect(stopHosting).not.toHaveBeenCalled();
  });

  it('offers no start once every server it may run is live, a stopping one included', async () => {
    await mountPage(serversResponse([hostedServer(1), hostedServer(2), hostedServer(3, { state: 'stopping' })]));
    expect(link('Start a server')).toBeUndefined();
    expect(wrapper!.get('.servers-block').text()).toContain('a stopping one counts until it stops');
    // A stopping server takes no second stop.
    expect(button('Stop Server 3')).toBeUndefined();
  });

  it('says in one line why a past-due account cannot start, linking billing', async () => {
    await mountPage(serversResponse([hostedServer(1)], { entitlement: { can_start: false, source: 'comp', tier: null, granted: true } }));
    expect(link('Start a server')).toBeUndefined();
    const block = wrapper!.get('.servers-block');
    expect(block.text()).toContain("new servers can't start");
    expect(block.get('a').attributes('href')).toBe('/me/billing');
  });

  it('offers more servers at the limit to an account that can buy them', async () => {
    const two = [hostedServer(1), hostedServer(2)];
    await mountPage(serversResponse(two, { entitlement: { max_servers: 2, tier: 2 } }));
    expect(wrapper!.get('.servers-block a').attributes('href')).toBe('/me/billing');
    wrapper!.unmount();

    // A granted account buys none.
    await mountPage(serversResponse([...two, hostedServer(3)], { entitlement: { max_servers: 3, tier: 1, granted: true } }));
    expect(wrapper!.get('.servers-block').text()).toContain('stop one to start another');
    expect(wrapper!.find('.servers-block a').exists()).toBe(false);
  });

  it('points an account with no entitlement at the hosting page in one line', async () => {
    await mountPage(serversResponse([hostedServer(5, { state: 'stopped' })], { entitlement: { max_servers: 0, source: 'none', tier: null } }), '/me/servers?state=all');
    const callout = wrapper!.get('#servers-locked');
    expect(callout.get('a').attributes('href')).toBe('/hosting');
    expect(callout.findAll('span')).toHaveLength(1);
    expect(wrapper!.find('.page-meta').exists()).toBe(false);
    // Its ended server still shows, with no start again.
    expect(rows()).toHaveLength(1);
    expect(rows()[0].text()).not.toContain('Start again');
  });

  it('says so when hosting is off', async () => {
    await mountPage(serversResponse([], { enabled: false }));
    expect(wrapper!.get('.servers-off').text()).toBe('Hosting is off right now.');
    expect(wrapper!.find('.page-meta').exists()).toBe(false);
  });

  it('says in one line while launches are held for one of the live servers', async () => {
    await mountPage(serversResponse([hostedServer(1, { state: 'provisioning', phase: { name: 'launches_held' } })]));
    expect(wrapper!.get('.launches-held').text()).toContain('New machines are held for a moment');
    wrapper!.unmount();

    await mountPage(serversResponse([hostedServer(1)]));
    expect(wrapper!.find('.launches-held').exists()).toBe(false);
  });

  it('says so in one line when the account runs nothing', async () => {
    await mountPage(serversResponse([]));
    expect(wrapper!.get('tr.table-state').text()).toBe('No servers yet.');
  });
});
