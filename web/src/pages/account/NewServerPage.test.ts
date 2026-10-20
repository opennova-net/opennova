import { flushPromises, mount, type VueWrapper } from '@vue/test-utils';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import NewServerPage from './NewServerPage.vue';
import { fetchMyAddresses } from '../../api/addresses';
import { fetchMyServer, fetchMyServers, startHostedServer } from '../../api/hostedServer';
import { address, addressesResponse } from '../../test/addressFixtures';
import { hostedServer, serversResponse } from '../../test/hostedFixtures';
import { testRouter } from '../../test/router';
import type { Address } from '../../types/addresses';
import type { HostedServer, HostedServersResponse } from '../../types/hostedServer';

vi.mock('../../api/hostedServer', async (importOriginal) => ({
  ...(await importOriginal<typeof import('../../api/hostedServer')>()),
  fetchMyServers: vi.fn(),
  fetchMyServer: vi.fn(),
  startHostedServer: vi.fn(),
}));
vi.mock('../../api/addresses', async (importOriginal) => ({
  ...(await importOriginal<typeof import('../../api/addresses')>()),
  fetchMyAddresses: vi.fn(),
}));

const servers = vi.mocked(fetchMyServers);
const one = vi.mocked(fetchMyServer);
const addresses = vi.mocked(fetchMyAddresses);
const start = vi.mocked(startHostedServer);

// A server on a dedicated machine that ran at the static address fleettest
// (id 3), stopped.
function stoppedAtAddress(overrides: Partial<HostedServer> = {}): HostedServer {
  return hostedServer(7, {
    state: 'stopped',
    own_machine: true,
    address: {
      id: 3,
      name: 'fleettest',
      hostname: 'fleettest.play.opennova.net',
      public_ip: '203.0.113.3',
      state: 'ready',
    },
    ...overrides,
  });
}

let wrapper: VueWrapper | null = null;

async function mountPage(from: HostedServer, owned: Address[], ownMachines = 1) {
  servers.mockResolvedValue({ ok: true, value: serversResponse([from], { entitlement: { own_machines: ownMachines } }) });
  addresses.mockResolvedValue({ ok: true, value: addressesResponse(owned) });
  const router = await testRouter(`/me/servers/new?from=${from.id}`);
  wrapper = mount(NewServerPage, { global: { plugins: [router] } });
  await flushPromises();
  return wrapper;
}

// The page with no server to start again, the account's servers `live`.
async function mountBlocked(live: HostedServer[], e: Partial<HostedServersResponse['entitlement']>) {
  servers.mockResolvedValue({ ok: true, value: serversResponse(live, { entitlement: e }) });
  addresses.mockResolvedValue({ ok: true, value: addressesResponse([]) });
  const router = await testRouter('/me/servers/new');
  wrapper = mount(NewServerPage, { global: { plugins: [router] } });
  await flushPromises();
  return wrapper;
}

function linkTexts(page: VueWrapper): string[] {
  return page.findAll('a').map((a) => a.text());
}

function selectedAddress(page: VueWrapper): string {
  const select = page.get('#new-server-address_id').element as HTMLSelectElement;
  return select.options[select.selectedIndex].text;
}

describe('NewServerPage', () => {
  beforeEach(() => {
    vi.clearAllMocks();
    start.mockResolvedValue({ ok: true, value: hostedServer(10, { state: 'requested', own_machine: true }) });
  });

  afterEach(() => {
    wrapper?.unmount();
    wrapper = null;
  });

  it('starts a server that ran on a dedicated machine again on one, at its static address', async () => {
    // Another free address listed first: the old server's is the one picked.
    const page = await mountPage(stoppedAtAddress(), [address(4, 'arena'), address(3, 'fleettest')]);
    expect(one).not.toHaveBeenCalled();
    expect(page.get('h1').text()).toBe('Start Server 7 again');
    expect((page.get('#new-server-own_machine').element as HTMLInputElement).checked).toBe(true);
    expect(selectedAddress(page)).toBe('fleettest.play.opennova.net');
    await page.get('form').trigger('submit');
    await flushPromises();
    expect(start).toHaveBeenCalledWith(expect.objectContaining({ game_name: 'Server 7' }), true, 3);
  });

  it("keeps the dedicated machine but not an address that is no longer the owner's and ready", async () => {
    const lapsed = await mountPage(stoppedAtAddress(), [address(3, 'fleettest', { state: 'lapsed' })]);
    expect((lapsed.get('#new-server-own_machine').element as HTMLInputElement).checked).toBe(true);
    expect(lapsed.find('#new-server-address_id').exists()).toBe(false);
    lapsed.unmount();

    // Gone from the account's list: another free one, never the old one.
    const gone = await mountPage(stoppedAtAddress(), [address(4, 'arena')]);
    expect(selectedAddress(gone)).toBe('arena.play.opennova.net');
  });

  it('says why no server starts, offering a bigger tier only to an account that can buy one', async () => {
    const past = await mountBlocked([], { can_start: false, source: 'comp', tier: null, granted: true });
    expect(past.get('#new-server-blocked').element.closest('section')!.textContent).toContain(
      "A payment of yours did not go through. Your servers keep running while Stripe retries the card; new servers can't start.",
    );
    expect(linkTexts(past)).toContain('Update your payment method');
    past.unmount();

    const locked = await mountBlocked([], { max_servers: 0, source: 'none', tier: null });
    expect(locked.text()).toContain('Hosting a server needs a support tier, a dedicated machine or an admin grant.');
    expect(linkTexts(locked)).toContain('See the support tiers');
    locked.unmount();

    const one = [hostedServer(1)];
    const tier1 = await mountBlocked(one, { max_servers: 1, tier: 1 });
    expect(tier1.text()).toContain('Stop one first, or move to a bigger support tier or add a dedicated machine.');
    expect(linkTexts(tier1)).toContain('See the support tiers');
    tier1.unmount();
    const granted = await mountBlocked(one, { max_servers: 1, tier: 1, granted: true });
    expect(granted.text()).toContain('All the servers you may run at once are live. Stop one first.');
    expect(linkTexts(granted)).not.toContain('See the support tiers');
    granted.unmount();
    // The biggest tier: a dedicated machine still adds a server.
    const top = await mountBlocked([hostedServer(1), hostedServer(2), hostedServer(3)], { tier: 3 });
    expect(top.text()).toContain('Stop one first, or add a dedicated machine.');
    expect(linkTexts(top)).toContain('See the support tiers');
  });

  it('starts on the dedicated machine once every server on shared machines is live', async () => {
    // Tier 2 and a dedicated machine, both shared servers live: a stopped shared
    // server starts again on the dedicated machine.
    const stopped = hostedServer(7, { state: 'stopped' });
    servers.mockResolvedValue({
      ok: true,
      value: serversResponse([hostedServer(1), hostedServer(2), stopped], {
        entitlement: { max_servers: 3, tier: 2, own_machines: 1 },
      }),
    });
    addresses.mockResolvedValue({ ok: true, value: addressesResponse([]) });
    wrapper = mount(NewServerPage, { global: { plugins: [await testRouter('/me/servers/new?from=7')] } });
    await flushPromises();
    const page = wrapper;
    expect(page.find('#new-server-blocked').exists()).toBe(false);
    expect(page.find('#new-server-shared_machine').exists()).toBe(false);
    expect(page.get('#new-server-pool').text()).toBe('On your dedicated machine');
    await page.get('form').trigger('submit');
    await flushPromises();
    expect(start).toHaveBeenCalledWith(expect.anything(), true, null);
  });

  it('says a server goes to a shared machine while every dedicated machine is in use', async () => {
    const stopped = hostedServer(7, { state: 'stopped' });
    servers.mockResolvedValue({
      ok: true,
      value: serversResponse([hostedServer(1, { own_machine: true }), stopped], {
        entitlement: { max_servers: 3, tier: 2, own_machines: 1 },
      }),
    });
    addresses.mockResolvedValue({ ok: true, value: addressesResponse([]) });
    wrapper = mount(NewServerPage, { global: { plugins: [await testRouter('/me/servers/new?from=7')] } });
    await flushPromises();
    expect(wrapper.find('#new-server-own_machine').exists()).toBe(false);
    expect(wrapper.get('#new-server-pool').text()).toBe('On a shared machine: your dedicated machines are all in use.');
    await wrapper.get('form').trigger('submit');
    await flushPromises();
    expect(start).toHaveBeenCalledWith(expect.anything(), false, null);
  });

  it('starts a shared server again on a shared machine, and offers no dedicated machine with none free', async () => {
    const shared = await mountPage(hostedServer(7, { state: 'stopped' }), [address(3, 'fleettest')]);
    expect((shared.get('#new-server-shared_machine').element as HTMLInputElement).checked).toBe(true);
    expect((shared.get('#new-server-own_machine').element as HTMLInputElement).checked).toBe(false);
    expect(shared.find('#new-server-address_id').exists()).toBe(false);
    shared.unmount();

    const busy = await mountPage(stoppedAtAddress(), [address(3, 'fleettest')], 0);
    expect(busy.find('#new-server-own_machine').exists()).toBe(false);
    await busy.get('form').trigger('submit');
    await flushPromises();
    expect(start).toHaveBeenCalledWith(expect.anything(), false, null);
  });
});
