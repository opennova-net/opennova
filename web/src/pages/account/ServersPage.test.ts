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
    expect(wrapper.text()).toContain('From your support tier.');
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

  it('says why a past-due account cannot start', async () => {
    const wrapper = await mountPage(serversResponse([hostedServer(1)], { entitlement: { can_start: false } }));
    unmount = () => wrapper.unmount();
    expect(wrapper.findAll('a').some((a) => a.text() === 'Start a server')).toBe(false);
    expect(wrapper.text()).toContain("new servers can't start");
  });

  it('shows the support tier callout to an account with no entitlement', async () => {
    const wrapper = await mountPage(
      serversResponse([hostedServer(5, { state: 'stopped' })], { entitlement: { max_servers: 0, source: 'none' } }),
    );
    unmount = () => wrapper.unmount();
    expect(wrapper.find('#servers-locked-heading').exists()).toBe(true);
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
});
