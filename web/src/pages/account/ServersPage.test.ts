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

  it('shows the allowance and a start while there is room', async () => {
    const wrapper = await mountPage(serversResponse([hostedServer(1), hostedServer(2)]));
    unmount = () => wrapper.unmount();
    expect(wrapper.get('#servers-allowance').text()).toBe('2 of 3 servers used');
    const start = wrapper.findAll('a').find((a) => a.text() === 'Start a server');
    expect(start?.attributes('href')).toBe('/me/servers/new');
    expect(start?.attributes('aria-describedby')).toBe('servers-allowance');
    expect(wrapper.text()).toContain('On shared machines, from your support tier.');
    expect(wrapper.find('#servers-own-machines').exists()).toBe(false);
  });

  it('meters the servers on shared machines and the dedicated machines apart', async () => {
    // Tier 2 and a dedicated machine: both shared servers live, the machine free.
    const wrapper = await mountPage(
      serversResponse([hostedServer(1), hostedServer(2)], { entitlement: { max_servers: 3, tier: 2, own_machines: 1 } }),
    );
    unmount = () => wrapper.unmount();
    expect(wrapper.get('#servers-allowance').text()).toBe('2 of 2 servers used');
    expect(wrapper.get('#servers-own-machines').text()).toBe('0 of 1 dedicated machine used');
    expect(wrapper.text()).toContain('One more server each, on a machine of its own.');
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
    expect(wrapper.get('#servers-own-machines').text()).toBe('1 of 2 dedicated machines used');
    expect(wrapper.findAll('a').some((a) => a.text() === 'Start a server')).toBe(true);
  });

  it("gives each server's card ids of its own", async () => {
    const listing = {
      rid: 21,
      server_name: 'Two',
      mission_name: 'jo_two.bms',
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
    expect(two.get('#hosted-server-2-heading').text()).toBe('Server 2');
    expect(two.text()).toContain('5 of 24');
    // Every id on the page is unique.
    const ids = Array.from(document.body.querySelectorAll('[id]')).map((el) => el.id);
    expect(new Set(ids).size).toBe(ids.length);
    // The hosted server's own listing is not among the games hosted yourself.
    expect(wrapper.text()).toContain('Listed 30');
    expect(wrapper.text()).not.toContain('Listed 21');
  });

  it('offers no start once every server it may run is live, a stopping one included', async () => {
    const wrapper = await mountPage(
      serversResponse([hostedServer(1), hostedServer(2), hostedServer(3, { state: 'stopping' })]),
    );
    unmount = () => wrapper.unmount();
    expect(wrapper.get('#servers-allowance').text()).toBe('3 of 3 servers used');
    expect(wrapper.findAll('a').some((a) => a.text() === 'Start a server')).toBe(false);
    expect(wrapper.text()).toContain('All 3 servers you may run are live.');
    expect(wrapper.text()).toContain('counts toward your limit until it has stopped, in about a minute');
  });

  it('says why a past-due account cannot start, whatever payment it is', async () => {
    // An admin's grant with a dedicated machine's payment past due: no tier to blame.
    const wrapper = await mountPage(
      serversResponse([hostedServer(1)], { entitlement: { can_start: false, source: 'comp', tier: null, granted: true } }),
    );
    unmount = () => wrapper.unmount();
    expect(wrapper.findAll('a').some((a) => a.text() === 'Start a server')).toBe(false);
    expect(wrapper.text()).toContain("A payment of yours did not go through. Your servers keep running while Stripe retries the card; new servers can't start.");
    expect(wrapper.text()).not.toContain('support tier');
  });

  it('offers more servers at the limit to an account that can buy them', async () => {
    const two = [hostedServer(1), hostedServer(2)];
    const tier2 = await mountPage(serversResponse(two, { entitlement: { max_servers: 2, tier: 2 } }));
    expect(tier2.text()).toContain('Stop one to start another, or move to a bigger support tier or add a dedicated machine.');
    expect(tier2.findAll('a').some((a) => a.text() === 'See the support tiers')).toBe(true);
    tier2.unmount();

    // Tier 3 has none bigger, and a dedicated machine still adds one.
    const three = [...two, hostedServer(3)];
    const top = await mountPage(serversResponse(three, { entitlement: { max_servers: 3, tier: 3 } }));
    expect(top.text()).toContain('All 3 servers you may run are live. Stop one to start another, or add a dedicated machine.');
    expect(top.text()).not.toContain('bigger support tier');
    expect(top.findAll('a').some((a) => a.text() === 'See the support tiers')).toBe(true);
    top.unmount();

    // A granted account buys none.
    const granted = await mountPage(serversResponse(three, { entitlement: { max_servers: 3, tier: 1, granted: true } }));
    expect(granted.text()).toContain('All 3 servers you may run are live. Stop one to start another.');
    expect(granted.text()).not.toContain('dedicated machine');
    expect(granted.findAll('a').some((a) => a.text() === 'See the support tiers')).toBe(false);
    granted.unmount();
  });

  it('shows the support tier callout to an account with no entitlement', async () => {
    const wrapper = await mountPage(
      serversResponse([hostedServer(5, { state: 'stopped' })], {
        entitlement: { max_servers: 0, source: 'none', tier: null },
      }),
    );
    unmount = () => wrapper.unmount();
    expect(wrapper.find('#servers-locked-heading').exists()).toBe(true);
    // A dedicated machine and an admin's grant are the other ways to host.
    const callout = wrapper.get('#servers-locked-heading').element.closest('section')!.textContent;
    expect(callout).toContain('A dedicated machine runs one more on a machine of its own, with or without a tier.');
    expect(callout).toContain('An admin can also grant your account hosted servers');
    expect(wrapper.find('#servers-allowance').exists()).toBe(false);
    // Its ended server still shows, with no start again.
    expect(wrapper.get('#hosted-server-5').text()).not.toContain('Start again');
  });

  it('says so when hosting is off', async () => {
    const wrapper = await mountPage(serversResponse([], { enabled: false }));
    unmount = () => wrapper.unmount();
    expect(wrapper.text()).toContain('Hosting is off');
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
});
