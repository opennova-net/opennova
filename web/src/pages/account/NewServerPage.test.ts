import { flushPromises, mount, type VueWrapper } from '@vue/test-utils';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import NewServerPage from './NewServerPage.vue';
import { fetchMyAddresses } from '../../api/addresses';
import { fetchMyServer, fetchMyServers, startHostedServer } from '../../api/hostedServer';
import { address, addressesResponse } from '../../test/addressFixtures';
import { hostedServer, serversResponse } from '../../test/hostedFixtures';
import { testRouter } from '../../test/router';
import type { Address } from '../../types/addresses';
import type { HostedServer } from '../../types/hostedServer';

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

// An own-machine server that ran at the static address fleettest (id 3), stopped.
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

  it('starts an own-machine server again on an own machine at its static address', async () => {
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

  it("keeps the own machine but not an address that is no longer the owner's and ready", async () => {
    const lapsed = await mountPage(stoppedAtAddress(), [address(3, 'fleettest', { state: 'lapsed' })]);
    expect((lapsed.get('#new-server-own_machine').element as HTMLInputElement).checked).toBe(true);
    expect(lapsed.find('#new-server-address_id').exists()).toBe(false);
    lapsed.unmount();

    // Gone from the account's list: another free one, never the old one.
    const gone = await mountPage(stoppedAtAddress(), [address(4, 'arena')]);
    expect(selectedAddress(gone)).toBe('arena.play.opennova.net');
  });

  it('starts a shared server again on a shared machine, and offers no own machine with none free', async () => {
    const shared = await mountPage(hostedServer(7, { state: 'stopped' }), [address(3, 'fleettest')]);
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
