import { flushPromises, mount } from '@vue/test-utils';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import { createMemoryHistory, createRouter } from 'vue-router';
import ServersPage from './ServersPage.vue';
import { fetchMyServers } from '../../api/hostedServer';
import { listMyServers } from '../../api/myServers';
import { hostedServer, listedServer, serversResponse } from '../../test/hostedFixtures';
import type { HostedServersResponse } from '../../types/hostedServer';

vi.mock('../../api/hostedServer', async (importOriginal) => ({
  ...(await importOriginal<typeof import('../../api/hostedServer')>()),
  fetchMyServers: vi.fn(),
}));
vi.mock('../../api/myServers', () => ({ listMyServers: vi.fn(), sendServerCommand: vi.fn(), stopHosting: vi.fn() }));
vi.mock('../../api/hostedStats', () => ({
  fetchAccountStats: vi.fn(() => Promise.resolve({ ok: false, error: { code: 'network', status: null, message: 'offline' } })),
}));

const servers = vi.mocked(fetchMyServers);
const hosts = vi.mocked(listMyServers);

function makeRouter() {
  return createRouter({
    history: createMemoryHistory(),
    routes: [{ path: '/:rest(.*)*', component: { render: () => null } }],
  });
}

async function mountPage(data: HostedServersResponse) {
  servers.mockResolvedValue({ ok: true, value: data });
  const router = makeRouter();
  await router.push('/me/servers');
  const wrapper = mount(ServersPage, { global: { plugins: [router] }, attachTo: document.body });
  await flushPromises();
  return wrapper;
}

describe('ServersPage', () => {
  let unmount: (() => void) | null = null;

  beforeEach(() => {
    vi.clearAllMocks();
    hosts.mockResolvedValue({ ok: true, value: [] });
  });

  afterEach(() => {
    unmount?.();
    unmount = null;
  });

  it('shows the allowance as one row and a start in the header while there is room', async () => {
    const wrapper = await mountPage(serversResponse([hostedServer(1), hostedServer(2)]));
    unmount = () => wrapper.unmount();
    expect(wrapper.get('h1').text()).toBe('Servers');
    expect(wrapper.get('#servers-allowance').text()).toBe('2 of 3 shared servers');
    const start = wrapper.get('.page-header').findAll('a').find((a) => a.text() === 'Start a server');
    expect(start?.attributes('href')).toBe('/me/servers/new');
    expect(start?.attributes('aria-describedby')).toBe('servers-allowance');
    expect(wrapper.get('.servers-allowance [title]').attributes('title')).toBe('From your tier');
    expect(wrapper.find('#servers-own-machines').exists()).toBe(false);
    expect(wrapper.find('.servers-block').exists()).toBe(false);
  });

  it('meters the servers on shared machines and the dedicated machines apart', async () => {
    // Tier 2 and a dedicated machine: both shared servers live, the machine free.
    const wrapper = await mountPage(
      serversResponse([hostedServer(1), hostedServer(2)], { entitlement: { max_servers: 3, tier: 2, own_machines: 1 } }),
    );
    unmount = () => wrapper.unmount();
    expect(wrapper.get('#servers-allowance').text()).toBe('2 of 2 shared servers');
    expect(wrapper.get('#servers-own-machines').text()).toBe('0 of 1 dedicated machine');
    // The machine is free: a start goes there.
    const start = wrapper.findAll('a').find((a) => a.text() === 'Start a server');
    expect(start?.attributes('aria-describedby')).toBe('servers-allowance servers-own-machines');
  });

  it('shows the dedicated machines alone for an account with no tier', async () => {
    const wrapper = await mountPage(
      serversResponse([hostedServer(1, { own_machine: true })], {
        entitlement: { max_servers: 2, own_machines: 2, source: 'none', tier: null },
      }),
    );
    unmount = () => wrapper.unmount();
    expect(wrapper.find('#servers-allowance').exists()).toBe(false);
    expect(wrapper.get('#servers-own-machines').text()).toBe('1 of 2 dedicated machines');
    expect(wrapper.findAll('a').some((a) => a.text() === 'Start a server')).toBe(true);
  });

  it("gives each server's card ids of its own", async () => {
    const listing = {
      rid: 21,
      server_name: 'Two',
      mission_name: 'jo_two.bms',
      game_type: '',
      players: 3,
      max_players: 24,
      host_ip: '203.0.113.9',
      host_port: 32770,
    };
    hosts.mockResolvedValue({
      ok: true,
      value: [listedServer(21, { hosted_server_id: 2, players: 5, max_players: 24 }), listedServer(30)],
    });
    const wrapper = await mountPage(serversResponse([hostedServer(1), hostedServer(2, { listing })]));
    unmount = () => wrapper.unmount();
    const one = wrapper.get('#hosted-server-1');
    const two = wrapper.get('#hosted-server-2');
    expect(one.get('#hosted-server-1-heading').text()).toBe('Server 1');
    expect(two.get('#hosted-server-2-heading a').attributes('href')).toBe('/me/servers/2');
    expect(two.text()).toContain('5 of 24');
    // Every id on the page is unique.
    const ids = Array.from(document.body.querySelectorAll('[id]')).map((el) => el.id);
    expect(new Set(ids).size).toBe(ids.length);
    // The hosted server's own listing is not among the games hosted yourself.
    expect(wrapper.text()).toContain('Listed 30');
    expect(wrapper.text()).not.toContain('Listed 21');
  });

  it('keeps the ended servers in a collapsed list', async () => {
    const wrapper = await mountPage(serversResponse([hostedServer(1), hostedServer(5, { state: 'stopped' })]));
    unmount = () => wrapper.unmount();
    const ended = wrapper.get('details.servers-ended');
    expect(ended.attributes('open')).toBeUndefined();
    expect(ended.get('summary').text()).toBe('Recently ended (1)');
    expect(ended.find('#hosted-server-5').exists()).toBe(true);
    expect(wrapper.get('#servers-live-heading').text()).toBe('Live');
  });

  it('offers no start once every server it may run is live, a stopping one included', async () => {
    const wrapper = await mountPage(
      serversResponse([hostedServer(1), hostedServer(2), hostedServer(3, { state: 'stopping' })]),
    );
    unmount = () => wrapper.unmount();
    expect(wrapper.get('#servers-allowance').text()).toBe('3 of 3 shared servers');
    expect(wrapper.findAll('a').some((a) => a.text() === 'Start a server')).toBe(false);
    expect(wrapper.get('.servers-block').text()).toContain('a stopping one counts until it stops');
  });

  it('says in one line why a past-due account cannot start, linking billing', async () => {
    const wrapper = await mountPage(
      serversResponse([hostedServer(1)], { entitlement: { can_start: false, source: 'comp', tier: null, granted: true } }),
    );
    unmount = () => wrapper.unmount();
    expect(wrapper.findAll('a').some((a) => a.text() === 'Start a server')).toBe(false);
    const block = wrapper.get('.servers-block');
    expect(block.text()).toContain("new servers can't start");
    expect(block.get('a').attributes('href')).toBe('/me/billing');
  });

  it('offers more servers at the limit to an account that can buy them', async () => {
    const two = [hostedServer(1), hostedServer(2)];
    const tier2 = await mountPage(serversResponse(two, { entitlement: { max_servers: 2, tier: 2 } }));
    expect(tier2.get('.servers-block').text()).toContain('stop one to start another');
    expect(tier2.get('.servers-block a').attributes('href')).toBe('/me/billing');
    tier2.unmount();

    // A granted account buys none.
    const three = [...two, hostedServer(3)];
    const granted = await mountPage(serversResponse(three, { entitlement: { max_servers: 3, tier: 1, granted: true } }));
    expect(granted.get('.servers-block').text()).toContain('stop one to start another');
    expect(granted.find('.servers-block a').exists()).toBe(false);
    granted.unmount();
  });

  it('points an account with no entitlement at the hosting page in one line', async () => {
    const wrapper = await mountPage(
      serversResponse([hostedServer(5, { state: 'stopped' })], {
        entitlement: { max_servers: 0, source: 'none', tier: null },
      }),
    );
    unmount = () => wrapper.unmount();
    const callout = wrapper.get('#servers-locked');
    expect(callout.get('a').attributes('href')).toBe('/hosting');
    expect(callout.findAll('span')).toHaveLength(1);
    expect(wrapper.find('#servers-allowance').exists()).toBe(false);
    // Its ended server still shows, with no start again.
    expect(wrapper.get('#hosted-server-5').text()).not.toContain('Start again');
  });

  it('says so when hosting is off', async () => {
    const wrapper = await mountPage(serversResponse([], { enabled: false }));
    unmount = () => wrapper.unmount();
    expect(wrapper.get('.servers-off').text()).toBe('Hosting is off right now.');
    expect(wrapper.find('#servers-allowance').exists()).toBe(false);
  });

  it('shows the fleet notice while launches are held for one of the live servers', async () => {
    const held = await mountPage(serversResponse([hostedServer(1, { state: 'provisioning', phase: { name: 'launches_held' } })]));
    expect(held.get('.launches-held').text()).toContain('New machines are held for a moment');
    held.unmount();

    const wrapper = await mountPage(serversResponse([hostedServer(1)]));
    unmount = () => wrapper.unmount();
    expect(wrapper.find('.launches-held').exists()).toBe(false);
  });

  it('shows the stats under a heading with no description', async () => {
    const wrapper = await mountPage(serversResponse([hostedServer(1)]));
    unmount = () => wrapper.unmount();
    expect(wrapper.get('#servers-stats-heading').text()).toBe("Your account's stats");
    expect(wrapper.get('#servers-stats-heading').element.nextElementSibling?.tagName).not.toBe('P');
  });

  it('says so in one line when the account hosts no game itself', async () => {
    const wrapper = await mountPage(serversResponse([]));
    unmount = () => wrapper.unmount();
    expect(wrapper.get('#servers-hosted-yourself').element.nextElementSibling?.textContent?.trim()).toBe('None right now.');
  });
});
