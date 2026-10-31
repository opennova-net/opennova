import * as actionReview from '../../../composables/useConfirm';
import { flushPromises, mount } from '@vue/test-utils';
import { afterEach, beforeEach, describe, expect, it, vi, type MockInstance } from 'vitest';
import type { Router } from 'vue-router';
import ServersTab from './ServersTab.vue';
import { fetchHostedFleet, fetchListedHosts, stopAllFleetServers, stopFleetServer, stopListedServer } from '../../../api/admin';
import { fetchFleetUsage, parseFleetUsage } from '../../../api/hostedStats';
import { fetchProducts, updateAllServers } from '../../../api/products';
import { fleetResponse, fleetServer, listedHost } from '../../../test/adminFixtures';
import { DFX2_PRODUCT, productsResponse } from '../../../test/productFixtures';
import { fleetUsageReply } from '../../../test/statsFixtures';
import { NOW } from '../../../test/usageFixtures';
import { testRouter } from '../../../test/router';
import type { FleetServer } from '../../../types/hostedServer';

vi.mock('../../../api/admin', () => ({
  fetchHostedFleet: vi.fn(),
  fetchListedHosts: vi.fn(),
  stopAllFleetServers: vi.fn(),
  stopFleetServer: vi.fn(),
  stopListedServer: vi.fn(),
}));
vi.mock('../../../api/hostedStats', async (importOriginal) => ({
  ...(await importOriginal<typeof import('../../../api/hostedStats')>()),
  fetchFleetUsage: vi.fn(),
}));
vi.mock('../../../api/products', () => ({ fetchProducts: vi.fn(), updateAllServers: vi.fn() }));

const fleet = vi.mocked(fetchHostedFleet);
const listed = vi.mocked(fetchListedHosts);
const usage = vi.mocked(fetchFleetUsage);
const products = vi.mocked(fetchProducts);
const stopAll = vi.mocked(stopAllFleetServers);
const stopOne = vi.mocked(stopFleetServer);
const stopListed = vi.mocked(stopListedServer);
const updateAll = vi.mocked(updateAllServers);

// A server behind its game's current release, as the service answers it.
const OUTDATED: Partial<FleetServer> = { outdated: true, update_available: true, update_kinds: ['content'] };

// Server 1, JO on machine 3 slot 3, listed on River Patrol in TDM; a DFX2
// server of owner7's; a player's own JO server (rid 21), an anonymous DFX2 one
// (rid 22), and the listing hosted server 1 holds (rid 20), which is no row of
// its own.
function serverOne(overrides: Partial<FleetServer> = {}): FleetServer {
  return fleetServer(1, {
    listing: { rid: 20, server_name: 'Night Raid', mission_name: 'River Patrol', game_type: 'TDM', players: 5, max_players: 32, host_ip: '203.0.113.7', host_port: 32771 },
    ...overrides,
  });
}
const dfx2Server = () =>
  fleetServer(7, {
    product: { id: DFX2_PRODUCT.id, key: 'DFX2', title: 'dfx2', display_name: 'Delta Force Xtreme 2' },
    owner: { id: 107, username: 'owner7', tier: null, granted: true },
    machine_ref: '4',
    slot: 0,
  });

describe('ServersTab', () => {
  let wrapper: ReturnType<typeof mount> | null = null;
  let router: Router;
  let confirm: MockInstance<typeof actionReview.confirmAction>;

  async function mountTab(path = '/admin/servers') {
    router = await testRouter(path);
    wrapper = mount(ServersTab, { global: { plugins: [router] } });
    await flushPromises();
    return wrapper;
  }

  const rows = () => wrapper!.findAll('tr.server-row');

  beforeEach(() => {
    vi.clearAllMocks();
    vi.useFakeTimers({ toFake: ['Date'] });
    vi.setSystemTime(NOW);
    confirm = vi.spyOn(actionReview, 'confirmAction').mockResolvedValue(true);
    fleet.mockResolvedValue({ ok: true, value: fleetResponse({ servers: [serverOne(), dfx2Server()] }) });
    listed.mockResolvedValue({
      ok: true,
      value: {
        count: 3,
        hosts: [
          listedHost(20, { hosted_server_id: 1, owner: { id: 101, username: 'owner1' } }),
          listedHost(21),
          listedHost(22, { game: 'dfx2_consumer', owner: null, server_name: '<C255,0,0>Red Base', game_type: 'CTF', players: 1 }),
        ],
      },
    });
    const reply = fleetUsageReply();
    (reply.servers as Record<string, unknown>[])[1].id = 1;
    usage.mockResolvedValue({ ok: true, value: parseFleetUsage(reply)! });
    products.mockResolvedValue({ ok: true, value: productsResponse() });
  });

  afterEach(() => {
    wrapper?.unmount();
    wrapper = null;
    confirm.mockRestore();
    vi.useRealTimers();
  });

  it('lists every live server in one table: the hosted ones, then the ones players host', async () => {
    await mountTab();
    expect(fleet).toHaveBeenCalledWith({ state: 'live' }, expect.anything());
    expect(rows().map((r) => r.classes().includes('hosted-row'))).toEqual([true, true, false, false]);

    const hosted = rows()[0];
    expect(hosted.get('th a').text()).toBe('Night Raid');
    expect(hosted.get('th a').attributes('href')).toBe('/admin/servers/1');
    // Its game and game type under its name, its map in a column.
    expect(hosted.get('.server-game').text()).toBe('Joint Operations · TDM');
    expect(hosted.get('.server-map').text()).toBe('River Patrol');
    expect(hosted.text()).toContain('5/32');
    const links = hosted.findAll('a').map((a) => a.attributes('href'));
    expect(links).toContain('/admin/players/101');
    expect(links).toContain('/admin/machines/3');
    expect(links).toContain('/admin/products/JO');
    expect(links).toContain('/admin/servers/1/console');
    expect(hosted.get('.server-runs-on').text()).toContain('slot 3');
    expect(hosted.get('.server-release').text()).toContain('1.0.5');
    expect(hosted.get('.server-usage').text()).toBe('25 % · 200 MiB');
    expect(hosted.text()).toContain('Running');

    const own = rows()[2];
    expect(own.get('th a').text()).toBe('Listed 21');
    expect(own.get('th a').attributes('href')).toBe('/lobby/21');
    expect(own.get('.server-game').text()).toBe('Joint Operations · TDM');
    expect(own.get('.server-map').text()).toBe('River Patrol');
    expect(own.text()).toContain('3/16');
    expect(own.findAll('a').map((a) => a.attributes('href'))).toContain('/admin/players/71');
    expect(own.get('.server-runs-on').text()).toContain('Player-hosted');
    expect(own.get('.server-runs-on').text()).toContain('198.51.100.9:32768');
    expect(own.text()).toContain('Listed');
    expect(own.get('.server-usage').text()).toBe('—');

    const anonymous = rows()[3];
    expect(anonymous.get('.server-game').text()).toBe('Delta Force Xtreme 2 · CTF');
    expect(anonymous.text()).toContain('Anonymous');
    expect(anonymous.get('th').text()).toContain('Red Base');
  });

  it('counts the servers, their players and machines in tiles', async () => {
    const page = await mountTab();
    const tiles = page.get('.servers-tiles');
    expect(tiles.text()).toContain('Live servers');
    expect(tiles.text()).toContain('2 hosted, 2 player-hosted');
    // Every listed server's players: 3 + 3 + 1.
    expect(tiles.findAll('dd')[2].text()).toBe('7');
    expect(tiles.findAll('a').map((a) => a.attributes('href'))).toContain('/admin/servers/machines');
    expect(tiles.text()).toContain('On');
  });

  it("takes the owner from the URL, for the fleet's read and the listings alike", async () => {
    const page = await mountTab('/admin/servers?owner=host21');
    expect(fleet).toHaveBeenCalledWith({ state: 'live', owner: 'host21' }, expect.anything());
    expect((page.get('#servers-owner').element as HTMLInputElement).value).toBe('host21');
    const shown = rows().filter((r) => r.classes().includes('listed-row'));
    expect(shown).toHaveLength(1);
    expect(shown[0].text()).toContain('Listed 21');

    await page.get('#servers-owner').setValue('owner7');
    await page.get('#servers-owner').element.closest('form')!.dispatchEvent(new Event('submit'));
    await flushPromises();
    expect(router.currentRoute.value.query.owner).toBe('owner7');
    expect(fleet).toHaveBeenLastCalledWith({ state: 'live', owner: 'owner7' }, expect.anything());
  });

  it("takes the game from the URL: its hosted servers and its lobby's listings, and its update alone", async () => {
    updateAll.mockResolvedValue({ ok: true, value: 1 });
    fleet.mockResolvedValue({ ok: true, value: fleetResponse({ servers: [serverOne(), { ...dfx2Server(), ...OUTDATED }] }) });
    const page = await mountTab(`/admin/servers?product=${DFX2_PRODUCT.id}`);
    expect((page.get('#servers-product').element as HTMLSelectElement).value).toBe(String(DFX2_PRODUCT.id));
    expect(rows().map((r) => r.get('th a').text())).toEqual(['Server 7', 'Red Base']);
    expect(rows()[0].findAll('a').map((a) => a.attributes('href'))).toContain('/admin/players/107');

    await page.findAll('button').find((b) => b.text() === 'Update outdated…')!.trigger('click');
    await flushPromises();
    expect(confirm).toHaveBeenCalledWith('Update 1 outdated server? Each restarts and drops its players.');
    expect(updateAll).toHaveBeenCalledWith(DFX2_PRODUCT.id);
    expect(page.get('[role="status"]').text()).toBe('1 server updating.');

    await page.get('#servers-product').setValue('');
    await flushPromises();
    expect(router.currentRoute.value.query.product).toBeUndefined();
    expect(rows()).toHaveLength(4);
  });

  it('filters by state and kind with chips kept in the URL', async () => {
    const page = await mountTab();
    const chips = page.findAll('[role="group"] a');
    expect(chips.map((c) => c.text())).toEqual(['Live', 'Ended', 'All', 'All', 'Hosted', 'Player-hosted']);
    expect(chips[0].attributes('aria-current')).toBe('true');
    expect(chips[1].attributes('href')).toBe('/admin/servers?state=ended');
    expect(chips[5].attributes('href')).toBe('/admin/servers?kind=player');
    page.unmount();

    await mountTab('/admin/servers?kind=player');
    expect(rows().every((r) => r.classes().includes('listed-row'))).toBe(true);
    wrapper!.unmount();

    fleet.mockResolvedValue({ ok: true, value: fleetResponse({ servers: [fleetServer(2, { state: 'stopped' })] }) });
    await mountTab('/admin/servers?state=ended');
    expect(fleet).toHaveBeenLastCalledWith({ state: 'ended' }, expect.anything());
    // The ones players host are live by nature.
    expect(rows()).toHaveLength(1);
    expect(rows()[0].text()).toContain('Stopped');
  });

  it('stops a hosted server and a player-hosted one, each after a confirmation', async () => {
    stopOne.mockResolvedValue({ ok: true, value: null });
    stopListed.mockResolvedValue({ ok: true, value: undefined });
    const page = await mountTab();
    await rows()[0].findAll('button').find((b) => b.text().startsWith('Stop'))!.trigger('click');
    await flushPromises();
    expect(confirm).toHaveBeenCalledWith('Stop Night Raid? Its match ends for everyone on it.');
    expect(stopOne).toHaveBeenCalledWith(1);
    expect(page.get('[role="status"]').text()).toBe('Night Raid is stopping.');

    await rows()[2].findAll('button').find((b) => b.text().startsWith('Stop'))!.trigger('click');
    await flushPromises();
    expect(stopListed).toHaveBeenCalledWith(21);

    confirm.mockResolvedValueOnce(false);
    await rows()[3].findAll('button').find((b) => b.text().startsWith('Stop'))!.trigger('click');
    await flushPromises();
    expect(stopListed).toHaveBeenCalledTimes(1);
  });

  it('stops all behind the typed phrase', async () => {
    stopAll.mockResolvedValue({ ok: true, value: 2 });
    const page = await mountTab();
    const submit = page.get('.stop-all button[type="submit"]');
    await page.get('#stop-all-confirm').setValue('STOP AL');
    expect((submit.element as HTMLButtonElement).disabled).toBe(true);
    await page.get('#stop-all-confirm').setValue('STOP ALL');
    await page.get('.stop-all form').trigger('submit');
    await flushPromises();
    expect(stopAll).toHaveBeenCalledTimes(1);
    expect(page.get('[role="status"]').text()).toBe('2 servers stopping.');
  });

  it('keeps the update off while an owner filter is on, or none is outdated', async () => {
    const page = await mountTab();
    const button = () => page.findAll('button').find((b) => b.text() === 'Update outdated…')!;
    expect((button().element as HTMLButtonElement).disabled).toBe(true);
    page.unmount();
    fleet.mockResolvedValue({ ok: true, value: fleetResponse({ servers: [serverOne(OUTDATED)] }) });
    await mountTab('/admin/servers?owner=owner1');
    const update = wrapper!.findAll('button').find((b) => b.text() === 'Update outdated…')!;
    expect((update.element as HTMLButtonElement).disabled).toBe(true);
  });

  it("names a release's state and the server's restarts and failure", async () => {
    fleet.mockResolvedValue({
      ok: true,
      value: fleetResponse({
        servers: [
          serverOne({ ...OUTDATED, restarts: 2 }),
          fleetServer(3, { ...OUTDATED, update_requested: true }),
          fleetServer(4, { state: 'failed', failure_reason: 'it crashed 3 times in an hour' }),
        ],
      }),
    });
    await mountTab('/admin/servers?state=all');
    expect(rows()[0].get('.server-release').text()).toContain('Outdated');
    expect(rows()[0].text()).toContain('2 restarts');
    expect(rows()[1].get('.server-release').text()).toContain('Update requested');
    expect(rows()[1].get('.server-release').text()).not.toContain('Outdated');
    expect(rows().at(-1)!.text()).toContain('it crashed 3 times in an hour');
  });

  it('shows the build a server runs and a newer one waiting, which the update takes', async () => {
    updateAll.mockResolvedValue({ ok: true, value: 1 });
    fleet.mockResolvedValue({
      ok: true,
      value: fleetResponse({
        servers: [
          serverOne({
            update_available: true,
            update_kinds: ['server_build'],
            build: { version: '0.0.23.6053', commit: '9eabb5d' },
            build_available: { version: '0.0.23.6055', commit: 'ebc5aeb' },
          }),
          fleetServer(3, { build: { version: '0.0.23.6055', commit: 'ebc5aeb' } }),
        ],
      }),
    });
    const page = await mountTab();
    const release = rows()[0].get('.server-release');
    expect(release.get('.server-build').text()).toBe('ONE 0.0.23.6053 9eabb5d');
    expect(release.get('.server-build-update').text()).toBe('New server build: ONE 0.0.23.6053 → ONE 0.0.23.6055');
    expect(release.text()).not.toContain('Outdated');
    expect(rows()[1].get('.server-build').text()).toBe('ONE 0.0.23.6055 ebc5aeb');
    expect(rows()[1].find('.server-update').exists()).toBe(false);

    await page.findAll('button').find((b) => b.text() === 'Update outdated…')!.trigger('click');
    await flushPromises();
    expect(confirm).toHaveBeenCalledWith('Update 1 outdated server? Each restarts and drops its players.');
    expect(updateAll).toHaveBeenCalledWith();
  });

  it('says so when the listings could not be read, and keeps the fleet', async () => {
    listed.mockResolvedValue({ ok: false, error: { code: 'db_error', status: 500, message: 'The server hit a database error. Try again shortly.' } });
    const page = await mountTab();
    expect(page.get('.listed-trouble').text()).toContain('The server hit a database error.');
    expect(rows()).toHaveLength(2);
  });
});
