import { flushPromises, mount } from '@vue/test-utils';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import AddressesTab from './AddressesTab.vue';
import { fetchAdminAddresses, releaseAddress, renameAddress, retryAddress } from '../../../api/adminAddresses';
import { adminAddress, adminAddressesResponse } from '../../../test/addressFixtures';
import { testRouter } from '../../../test/router';

vi.mock('../../../api/adminAddresses', () => ({
  fetchAdminAddresses: vi.fn(),
  releaseAddress: vi.fn(),
  renameAddress: vi.fn(),
  retryAddress: vi.fn(),
}));

const list = vi.mocked(fetchAdminAddresses);
const release = vi.mocked(releaseAddress);
const rename = vi.mocked(renameAddress);
const retry = vi.mocked(retryAddress);

let wrapper: ReturnType<typeof mount> | null = null;

async function mountTab() {
  const router = await testRouter('/admin/billing/addresses');
  wrapper = mount(AddressesTab, { global: { plugins: [router] } });
  await flushPromises();
  return wrapper;
}

function rows() {
  return wrapper!.findAll('tr.address-row');
}

function submit(selector: string) {
  wrapper!.get(selector).element.closest('form')!.dispatchEvent(new Event('submit'));
}

describe('AddressesTab', () => {
  beforeEach(() => {
    vi.clearAllMocks();
    list.mockResolvedValue({
      ok: true,
      value: adminAddressesResponse([
        adminAddress(1, 'smoke', { server: { id: 31, game_name: 'Night Ops', state: 'running' } }),
        adminAddress(2, 'spare', { source: 'comp', stripe_subscription_id: null, owner: null }),
        adminAddress(3, 'old', { state: 'lapsed', lapse_cause: 'grant', dns_state: 'pending' }),
      ]),
    });
  });

  afterEach(() => {
    wrapper?.unmount();
    wrapper = null;
  });

  it('is a Billing tab with what is held in its meta line, and one table of every address', async () => {
    const page = await mountTab();
    expect(page.get('h1').text()).toBe('Billing');
    expect(page.get('.tab-nav a[aria-current="page"]').text()).toBe('Static addresses');
    expect(page.get('#addresses-held').text()).toBe('3 of 10 held');
    expect(page.findAll('thead th').map((th) => th.text())).toEqual(['Name', 'Owner', 'IP', 'State', 'Paid by', 'Server', 'Actions']);
    const [smoke, spare, old] = rows().map((r) => r.text().replace(/\s+/g, ' '));
    expect(smoke).toContain('smoke.play.opennova.net');
    expect(smoke).toContain('203.0.113.1:32768');
    expect(smoke).toContain('eipalloc-0abc1');
    expect(smoke).toContain('Ready');
    expect(smoke).toContain('Subscription sub_address_1');
    expect(spare).toContain('Deleted account');
    expect(spare).toContain('Admin grant');
    expect(old).toContain('Ending: its subscription ended');
    expect(old).toContain('Record being removed');
    // The owner and the server link to their pages.
    expect(rows()[0].get('.player-link').attributes('href')).toBe('/admin/players/101');
    expect(rows()[0].get('.server-link a').attributes('href')).toBe('/admin/servers/31');
    // Actions are ghost buttons; an address on its way out has no release.
    expect(rows()[0].findAll('button').every((b) => b.classes('btn-ghost'))).toBe(true);
    expect(rows()[2].findAll('button').map((b) => b.text())).toEqual(['Rename address old']);
    expect(page.find('#addresses-orphans').exists()).toBe(false);
  });

  it('adds a Machine column linking the machine when the reply names one', async () => {
    list.mockResolvedValue({ ok: true, value: adminAddressesResponse([adminAddress(1, 'smoke', { machine_id: 12 }), adminAddress(2, 'spare')]) });
    const page = await mountTab();
    expect(page.findAll('thead th').map((th) => th.text())).toContain('Machine');
    expect(rows()[0].get('.machine-link').attributes('href')).toBe('/admin/machines/12');
    expect(rows()[1].find('.machine-link').exists()).toBe(false);
  });

  it("says why an address is ending from its own cause, never a comp address's subscription", async () => {
    list.mockResolvedValue({
      ok: true,
      value: adminAddressesResponse([
        adminAddress(1, 'smoke', { state: 'lapsed', lapse_cause: 'grant' }),
        adminAddress(2, 'fleettest', { source: 'comp', stripe_subscription_id: null, state: 'lapsed', lapse_cause: 'admin' }),
        adminAddress(3, 'gone', { state: 'released', lapse_cause: 'admin', reason: 'released by an admin' }),
      ]),
    });
    await mountTab();
    const [sold, comp, gone] = rows().map((r) => r.get('td.address-state').findAll('span').map((line) => line.text()));
    expect(sold).toEqual(['Ending: its subscription ended']);
    expect(comp).toEqual(['Ending: an admin released it']);
    expect(rows()[1].text()).not.toMatch(/subscription/i);
    expect(gone).toEqual(['Released', 'An admin released it']);
  });

  it('renames an address in its row, checking the name first', async () => {
    const page = await mountTab();
    await rows()[0].findAll('button').find((b) => b.text().startsWith('Rename'))!.trigger('click');
    const input = page.get('#address-rename-1');
    expect((input.element as HTMLInputElement).value).toBe('smoke');
    await input.setValue('Blaze!');
    submit('#address-rename-1');
    await flushPromises();
    expect(rename).not.toHaveBeenCalled();
    expect(page.get('#address-rename-1-hint').text()).toContain('lower-case letters');

    rename.mockResolvedValue({ ok: true, value: adminAddress(1, 'blaze') });
    await input.setValue('blaze');
    submit('#address-rename-1');
    await flushPromises();
    expect(rename).toHaveBeenCalledWith(1, 'blaze');
    expect(page.get('[role="status"]').text()).toBe('Renamed smoke to blaze.');
    expect(page.find('#address-rename-1').exists()).toBe(false);
  });

  it('releases an address only once its name is typed', async () => {
    const page = await mountTab();
    await rows()[0].findAll('button').find((b) => b.text().startsWith('Release'))!.trigger('click');
    expect(page.get('label[for="address-release-1"]').text()).toContain('Release smoke.play.opennova.net? Its server moves to another address.');
    const input = page.get('#address-release-1');
    const go = () => page.findAll('button').find((b) => b.text() === 'Release now')!;
    expect(go().attributes('disabled')).toBeDefined();
    await input.setValue('smok');
    expect(go().attributes('disabled')).toBeDefined();
    await input.setValue('smoke');
    expect(go().attributes('disabled')).toBeUndefined();

    release.mockResolvedValue({ ok: true, value: adminAddress(1, 'smoke', { state: 'lapsed', lapse_cause: 'admin' }) });
    submit('#address-release-1');
    await flushPromises();
    expect(release).toHaveBeenCalledWith(1, 'smoke');
    expect(page.get('[role="status"]').text()).toBe('smoke.play.opennova.net is being released.');
  });

  it('retries an address the cloud refused, and says when it retries by itself', async () => {
    list.mockResolvedValue({
      ok: true,
      value: adminAddressesResponse([
        adminAddress(1, 'smoke'),
        adminAddress(4, 'quota', { state: 'failed', public_ip: null, port: null, allocation_id: null, reason: 'AddressLimitExceeded', retry_at: '2099-01-01 00:00:00' }),
      ]),
    });
    const page = await mountTab();
    const [smoke, quota] = rows();
    expect(smoke.findAll('button').map((b) => b.text())).not.toContain('Retry address smoke');
    expect(quota.text()).toContain('AddressLimitExceeded');
    expect(quota.text()).toContain('Retries');
    retry.mockResolvedValue({ ok: true, value: adminAddress(4, 'quota', { state: 'allocating' }) });
    await quota.findAll('button').find((b) => b.text().startsWith('Retry'))!.trigger('click');
    await flushPromises();
    expect(retry).toHaveBeenCalledWith(4);
    expect(page.get('[role="status"]').text()).toBe('quota.play.opennova.net is being set up again.');
  });

  it("shows a refusal in the service's words", async () => {
    release.mockResolvedValue({
      ok: false,
      error: { code: 'not_releasable', status: 409, message: 'That address is on its way out already. Refresh the list.' },
    });
    const page = await mountTab();
    await rows()[0].findAll('button').find((b) => b.text().startsWith('Release'))!.trigger('click');
    await page.get('#address-release-1').setValue('smoke');
    submit('#address-release-1');
    await flushPromises();
    expect(page.get('[role="alert"]').text()).toBe('That address is on its way out already. Refresh the list.');
  });

  it('lists what the cloud holds that no address names in a small table', async () => {
    list.mockResolvedValue({
      ok: true,
      value: adminAddressesResponse([], {
        held: 0,
        orphans: [{ allocation_id: 'eipalloc-0dead', public_ip: '203.0.113.50', address_id: 7, seen_at: '2026-10-18 09:00:00' }],
      }),
    });
    const page = await mountTab();
    expect(page.get('#addresses-orphans').text()).toBe('Held by the cloud, not by an address');
    const row = page.get('tr.address-orphan');
    expect(row.findAll('th, td').slice(0, 3).map((c) => c.text())).toEqual(['eipalloc-0dead', '203.0.113.50', 'Address 7']);
    expect(page.text()).toContain('No static addresses yet.');
  });

  it('says in one sentence when the deployment sells none', async () => {
    list.mockResolvedValue({ ok: true, value: adminAddressesResponse([], { enabled: false, sellable: 0 }) });
    const page = await mountTab();
    expect(page.get('#addresses-held').text()).toBe('0 of 0 held');
    expect(page.get('.addresses-off').text()).toBe('Static addresses are off on this deployment.');
  });
});
