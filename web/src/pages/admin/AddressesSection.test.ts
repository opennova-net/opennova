import { flushPromises, mount } from '@vue/test-utils';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import AddressesSection from './AddressesSection.vue';
import { fetchAdminAddresses, releaseAddress, renameAddress, retryAddress } from '../../api/adminAddresses';
import { adminAddress, adminAddressesResponse } from '../../test/addressFixtures';
import { testRouter } from '../../test/router';

vi.mock('../../api/adminAddresses', () => ({
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

async function mountSection() {
  const router = await testRouter('/admin/addresses');
  wrapper = mount(AddressesSection, { global: { plugins: [router] } });
  await flushPromises();
  return wrapper;
}

function rows() {
  return wrapper!.findAll('tr.address-row');
}

describe('AddressesSection', () => {
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

  it('counts what the cloud holds of what the deployment sells, and lists every address', async () => {
    const page = await mountSection();
    expect(page.get('#addresses-held').text()).toBe('3 of 10 addresses the deployment sells held.');
    const [smoke, spare, old] = rows().map((r) => r.text().replace(/\s+/g, ' '));
    expect(smoke).toContain('smoke.play.opennova.net');
    expect(smoke).toContain('owner1');
    expect(smoke).toContain('203.0.113.1:32768');
    expect(smoke).toContain('eipalloc-0abc1');
    expect(smoke).toContain('Ready');
    expect(smoke).toContain('Subscription sub_address_1');
    expect(spare).toContain('Deleted account');
    expect(spare).toContain('Admin grant');
    expect(old).toContain('Ending: its subscription ended');
    expect(old).toContain("Its name's record is being removed");
    const links = page.findAll('a').map((a) => a.attributes('href'));
    expect(links).toContain('/admin/users/101');
    expect(links).toContain('/admin/fleet/31');
    // An address on its way out has no release.
    expect(rows()[2].findAll('button').map((b) => b.text())).toEqual(['Rename address old']);
    expect(page.find('#addresses-orphans').exists()).toBe(false);
  });

  it("says why an address is ending from its own cause, never a comp address's subscription", async () => {
    list.mockResolvedValue({
      ok: true,
      value: adminAddressesResponse([
        adminAddress(1, 'smoke', { state: 'lapsed', lapse_cause: 'grant', reason: 'its subscription ended' }),
        adminAddress(2, 'fleettest', {
          source: 'comp',
          stripe_subscription_id: null,
          state: 'lapsed',
          lapse_cause: 'admin',
          reason: 'released by an admin',
        }),
        adminAddress(3, 'gone', { state: 'released', lapse_cause: 'admin', reason: 'released by an admin' }),
      ]),
    });
    await mountSection();
    // The state cell's lines: the reason (the lapse's words) shows for a failed one alone.
    const [sold, comp, gone] = rows().map((r) => r.findAll('td')[2].findAll('span').map((line) => line.text()));
    expect(sold).toEqual(['Ending: its subscription ended']);
    expect(comp).toEqual(['Ending: an admin released it']);
    expect(rows()[1].text()).not.toMatch(/subscription/i);
    expect(gone).toEqual(['Released', 'An admin released it']);
  });

  it('renames an address in its row, checking the name first', async () => {
    const page = await mountSection();
    await rows()[0].findAll('button').find((b) => b.text().startsWith('Rename'))!.trigger('click');
    const input = page.get('#address-rename-1');
    expect((input.element as HTMLInputElement).value).toBe('smoke');
    await input.setValue('Blaze!');
    await input.element.closest('form')!.dispatchEvent(new Event('submit'));
    await flushPromises();
    expect(rename).not.toHaveBeenCalled();
    expect(page.get('#address-rename-1-hint').text()).toContain('lower-case letters');

    rename.mockResolvedValue({ ok: true, value: adminAddress(1, 'blaze') });
    await input.setValue('blaze');
    await input.element.closest('form')!.dispatchEvent(new Event('submit'));
    await flushPromises();
    expect(rename).toHaveBeenCalledWith(1, 'blaze');
    expect(page.get('[role="status"]').text()).toBe('Renamed smoke to blaze.');
    expect(page.find('#address-rename-1').exists()).toBe(false);
  });

  it('releases an address only once its name is typed', async () => {
    const page = await mountSection();
    await rows()[0].findAll('button').find((b) => b.text().startsWith('Release'))!.trigger('click');
    const input = page.get('#address-release-1');
    const go = () => page.findAll('button').find((b) => b.text() === 'Release now')!;
    expect(go().attributes('disabled')).toBeDefined();
    await input.setValue('smok');
    expect(go().attributes('disabled')).toBeDefined();
    await input.setValue('smoke');
    expect(go().attributes('disabled')).toBeUndefined();

    release.mockResolvedValue({ ok: true, value: adminAddress(1, 'smoke', { state: 'lapsed', lapse_cause: 'admin' }) });
    input.element.closest('form')!.dispatchEvent(new Event('submit'));
    await flushPromises();
    expect(release).toHaveBeenCalledWith(1, 'smoke');
    expect(page.get('[role="status"]').text()).toContain('smoke.play.opennova.net is being released');
  });

  it('retries an address the cloud refused, and says when it tries again by itself', async () => {
    list.mockResolvedValue({
      ok: true,
      value: adminAddressesResponse([
        adminAddress(1, 'smoke'),
        adminAddress(4, 'quota', {
          state: 'failed',
          public_ip: null,
          port: null,
          allocation_id: null,
          reason: 'AddressLimitExceeded',
          retry_at: '2099-01-01 00:00:00',
        }),
      ]),
    });
    const page = await mountSection();
    const [smoke, quota] = rows();
    expect(smoke.findAll('button').map((b) => b.text())).not.toContain('Retry address smoke');
    expect(quota.text()).toContain('AddressLimitExceeded');
    expect(quota.text()).toContain('Tried again by itself');
    retry.mockResolvedValue({ ok: true, value: adminAddress(4, 'quota', { state: 'allocating' }) });
    await quota.findAll('button').find((b) => b.text().startsWith('Retry'))!.trigger('click');
    await flushPromises();
    expect(retry).toHaveBeenCalledWith(4);
    expect(page.get('[role="status"]').text()).toBe('quota.play.opennova.net is being set up again.');
  });

  it("shows a refusal in the service's place", async () => {
    release.mockResolvedValue({
      ok: false,
      error: { code: 'not_releasable', status: 409, message: 'That address is on its way out already. Refresh the list.' },
    });
    const page = await mountSection();
    await rows()[0].findAll('button').find((b) => b.text().startsWith('Release'))!.trigger('click');
    await page.get('#address-release-1').setValue('smoke');
    await page.get('#address-release-1').element.closest('form')!.dispatchEvent(new Event('submit'));
    await flushPromises();
    expect(page.get('[role="alert"]').text()).toBe('That address is on its way out already. Refresh the list.');
  });

  it('lists the addresses the cloud holds that no row names', async () => {
    list.mockResolvedValue({
      ok: true,
      value: adminAddressesResponse([], {
        held: 0,
        orphans: [{ allocation_id: 'eipalloc-0dead', public_ip: '203.0.113.50', address_id: 7, seen_at: '2026-10-18 09:00:00' }],
      }),
    });
    const page = await mountSection();
    const orphans = page.get('#addresses-orphans').element.parentElement!.textContent!.replace(/\s+/g, ' ');
    expect(orphans).toContain(
      'The cloud holds these addresses for this deployment but no address row names them; release them in the AWS console after checking.',
    );
    expect(orphans).toContain('eipalloc-0dead, 203.0.113.50');
    expect(orphans).toContain('Tagged as address 7');
    expect(page.text()).toContain('No static address yet.');
  });

  it('says when the deployment sells none', async () => {
    list.mockResolvedValue({ ok: true, value: adminAddressesResponse([], { enabled: false, sellable: 0 }) });
    const page = await mountSection();
    expect(page.get('#addresses-held').text()).toBe('0 of 0 addresses the deployment sells held.');
    expect(page.text()).toContain('Static addresses are off');
  });
});
