import * as actionReview from '../../composables/useConfirm';
import { flushPromises, mount } from '@vue/test-utils';
import { afterEach, beforeEach, describe, expect, it, vi, type MockInstance } from 'vitest';
import ServerDetailLayout from './ServerDetailLayout.vue';
import { fetchFleetServer, stopFleetServer } from '../../api/admin';
import { fetchMyServer, stopHostedServer } from '../../api/hostedServer';
import { fleetServer } from '../../test/adminFixtures';
import { hostedServer } from '../../test/hostedFixtures';
import { testRouter } from '../../test/router';
import type { HostedListing } from '../../types/hostedServer';

vi.mock('../../api/admin', () => ({ fetchFleetServer: vi.fn(), stopFleetServer: vi.fn() }));
vi.mock('../../api/hostedServer', () => ({ fetchMyServer: vi.fn(), stopHostedServer: vi.fn() }));
// Whether the session is an admin's (the owner's page adds Logs for one).
const session = vi.hoisted(() => ({ admin: false }));
vi.mock('../../composables/useSession', () => ({ useSession: () => ({ refresh: vi.fn(), isAdmin: { value: session.admin } }) }));

const admin = vi.mocked(fetchFleetServer);
const adminStop = vi.mocked(stopFleetServer);
const own = vi.mocked(fetchMyServer);
const ownStop = vi.mocked(stopHostedServer);

const LISTING: HostedListing = {
  rid: 20,
  server_name: 'Night Raid',
  mission_name: 'River Patrol',
  game_type: 'TDM',
  players: 5,
  max_players: 32,
  host_ip: '203.0.113.7',
  host_port: 32771,
};

describe('ServerDetailLayout', () => {
  let wrapper: ReturnType<typeof mount> | null = null;
  let confirm: MockInstance<typeof actionReview.confirmAction>;

  async function mountLayout(scope: 'admin' | 'owner', path = scope === 'admin' ? '/admin/servers/1' : '/me/servers/1') {
    const router = await testRouter(path, ['/admin/servers/:id', '/me/servers/:id']);
    wrapper = mount(ServerDetailLayout, { props: { scope }, global: { plugins: [router] } });
    await flushPromises();
    return wrapper;
  }

  beforeEach(() => {
    vi.clearAllMocks();
    session.admin = false;
    confirm = vi.spyOn(actionReview, 'confirmAction').mockResolvedValue(true);
    admin.mockResolvedValue({ ok: true, value: fleetServer(1, { listing: LISTING }) });
    own.mockResolvedValue({ ok: true, value: hostedServer(1, { listing: LISTING }) });
  });

  afterEach(() => {
    wrapper?.unmount();
    wrapper = null;
    confirm.mockRestore();
  });

  it("heads the admin's page with the server, its game, map, players, owner and machine, over its four tabs", async () => {
    const page = await mountLayout('admin');
    expect(admin).toHaveBeenCalledWith(1, expect.anything());
    expect(page.findAll('.page-crumbs a').map((a) => [a.text(), a.attributes('href')])).toEqual([['Servers', '/admin/servers']]);
    expect(page.get('h1').text()).toBe('Night Raid');
    const meta = page.get('.page-meta');
    expect(meta.text()).toContain('Running');
    expect(meta.get('.server-game-type').text()).toBe('TDM');
    expect(meta.get('.server-map').text()).toBe('River Patrol');
    expect(meta.get('.server-players').text()).toBe('5/32 players');
    const links = meta.findAll('a').map((a) => a.attributes('href'));
    expect(links).toEqual(['/admin/products/JO', '/admin/players/101', '/admin/machines/3']);
    expect(page.findAll('.tab-nav a').map((a) => [a.text(), a.attributes('href')])).toEqual([
      ['Overview', '/admin/servers/1'],
      ['Stats', '/admin/servers/1/stats'],
      ['Console', '/admin/servers/1/console'],
      ['Logs', '/admin/servers/1/logs'],
    ]);
    expect(page.get('.tab-nav [aria-current="page"]').text()).toBe('Overview');
    expect(page.text()).not.toContain('Start again');
  });

  it("heads the owner's page with three tabs and a new start from its settings, no owner or machine", async () => {
    const page = await mountLayout('owner');
    expect(own).toHaveBeenCalledWith(1, expect.anything());
    expect(page.findAll('.page-crumbs a').map((a) => a.attributes('href'))).toEqual(['/me/servers']);
    expect(page.get('.page-meta').text()).toContain('Joint Operations');
    expect(page.get('.page-meta').findAll('a')).toHaveLength(0);
    expect(page.findAll('.tab-nav a').map((a) => a.text())).toEqual(['Overview', 'Stats', 'Console']);
    const again = page.findAll('a').find((a) => a.text() === 'Start again with these')!;
    expect(again.attributes('href')).toBe('/me/servers/new?from=1');
  });

  it("adds Logs to the owner's page of an admin's session, the server read as its owner's", async () => {
    session.admin = true;
    const page = await mountLayout('owner');
    expect(own).toHaveBeenCalledWith(1, expect.anything());
    expect(admin).not.toHaveBeenCalled();
    expect(page.findAll('.tab-nav a').map((a) => [a.text(), a.attributes('href')])).toEqual([
      ['Overview', '/me/servers/1'],
      ['Stats', '/me/servers/1/stats'],
      ['Console', '/me/servers/1/console'],
      ['Logs', '/me/servers/1/logs'],
    ]);
  });

  it('stops the server from its header after a confirmation', async () => {
    const stopping = hostedServer(1, { state: 'stopping', listing: LISTING });
    ownStop.mockResolvedValue({ ok: true, value: stopping });
    const page = await mountLayout('owner');
    own.mockResolvedValue({ ok: true, value: stopping });
    await page.findAll('button').find((b) => b.text() === 'Stop…')!.trigger('click');
    await flushPromises();
    expect(confirm).toHaveBeenCalledWith('Stop Night Raid? Its match ends for everyone on it.');
    expect(ownStop).toHaveBeenCalledWith(1);
    expect(page.get('[role="status"]').text()).toBe('Night Raid is stopping.');
    expect(page.findAll('button').some((b) => b.text() === 'Stop…')).toBe(false);
    page.unmount();

    adminStop.mockResolvedValue({ ok: true, value: null });
    const adminPage = await mountLayout('admin');
    await adminPage.findAll('button').find((b) => b.text() === 'Stop…')!.trigger('click');
    await flushPromises();
    expect(adminStop).toHaveBeenCalledWith(1);
  });

  it('offers no stop for a server that has ended', async () => {
    own.mockResolvedValue({ ok: true, value: hostedServer(1, { state: 'stopped' }) });
    const page = await mountLayout('owner');
    expect(page.get('h1').text()).toBe('Server 1');
    expect(page.findAll('button').some((b) => b.text().startsWith('Stop'))).toBe(false);
    expect(page.findAll('a').some((a) => a.text() === 'Start again with these')).toBe(true);
  });

  it('says so when no server has the id', async () => {
    admin.mockResolvedValue({ ok: false, error: { code: 'not_found', status: 404, message: 'It no longer exists. Refresh the list.' } });
    const page = await mountLayout('admin', '/admin/servers/99');
    expect(page.get('h1').text()).toBe('Server 99');
    expect(page.get('[role="alert"]').text()).toBe('No server has this id.');
    expect(page.find('.tab-nav').exists()).toBe(false);
  });
});
