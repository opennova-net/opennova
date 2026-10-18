import { flushPromises, mount } from '@vue/test-utils';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import AddressesPage from './AddressesPage.vue';
import { fetchMyAddresses, nameAddress } from '../../api/addresses';
import { fetchMyBilling, openPortal } from '../../api/billing';
import { address, addressesResponse } from '../../test/addressFixtures';
import { addressSubscription, myBilling } from '../../test/billingFixtures';
import { testRouter } from '../../test/router';
import { navigateToStripe } from '../../utils/navigate';

vi.mock('../../api/addresses', async (importOriginal) => ({
  ...(await importOriginal<typeof import('../../api/addresses')>()),
  fetchMyAddresses: vi.fn(),
  nameAddress: vi.fn(),
}));
vi.mock('../../api/billing', async (importOriginal) => ({
  ...(await importOriginal<typeof import('../../api/billing')>()),
  fetchMyBilling: vi.fn(),
  openPortal: vi.fn(),
}));
vi.mock('../../utils/navigate', () => ({ navigateToStripe: vi.fn(() => true) }));

const addresses = vi.mocked(fetchMyAddresses);
const name = vi.mocked(nameAddress);
const billing = vi.mocked(fetchMyBilling);
const portal = vi.mocked(openPortal);
const navigate = vi.mocked(navigateToStripe);

const PORTAL_URL = 'https://billing.stripe.com/p/session/test_cancel';

let wrapper: ReturnType<typeof mount> | null = null;

async function mountPage() {
  const router = await testRouter('/me/addresses');
  wrapper = mount(AddressesPage, { global: { plugins: [router] } });
  await flushPromises();
  return wrapper;
}

function card(id: number) {
  return wrapper!.get(`#address-${id}-heading`).element.closest('article')!;
}

describe('AddressesPage', () => {
  beforeEach(() => {
    vi.clearAllMocks();
    billing.mockResolvedValue({
      ok: true,
      value: myBilling({
        static_addresses: 1,
        address_subscriptions: [addressSubscription({ stripe_subscription_id: 'sub_address_1', address_id: 1 })],
      }),
    });
  });

  afterEach(() => {
    wrapper?.unmount();
    wrapper = null;
    vi.useRealTimers();
  });

  it('lists each address with its name, where players join it, its state and its server', async () => {
    addresses.mockResolvedValue({
      ok: true,
      value: addressesResponse([
        address(1, 'smoke', { server: { id: 31, game_name: 'Night Ops', state: 'running' } }),
        address(2, 'spare', { source: 'comp', stripe_subscription_id: null }),
        address(3, 'broken', { state: 'failed', public_ip: null, port: null, reason: 'the cloud has no address left' }),
      ]),
    });
    const page = await mountPage();
    const smoke = card(1).textContent!;
    expect(smoke).toContain('smoke.play.opennova.net');
    expect(smoke).toContain('203.0.113.1:32768');
    expect(smoke).toContain('Ready');
    expect(smoke).toContain('Its own subscription (active), renews on');
    expect(page.findAll('a').find((a) => a.text() === 'Night Ops')?.attributes('href')).toBe('/me/servers/31');
    // An admin's grant: no cancel.
    expect(card(2).textContent).toContain('Granted by an admin');
    expect(Array.from(card(2).querySelectorAll('button')).some((b) => b.textContent!.trim().startsWith('Cancel'))).toBe(false);
    expect(Array.from(card(1).querySelectorAll('button')).some((b) => b.textContent!.trim().startsWith('Cancel'))).toBe(true);
    expect(card(3).textContent).toContain('Could not be set up');
    expect(card(3).textContent).toContain('the cloud has no address left');
    expect(card(3).textContent).toContain('Not given yet');
    expect(page.find('#address-name').exists()).toBe(false);
  });

  it('copies the address players join', async () => {
    const writeText = vi.fn(() => Promise.resolve());
    Object.defineProperty(navigator, 'clipboard', { value: { writeText }, configurable: true });
    addresses.mockResolvedValue({ ok: true, value: addressesResponse([address(1, 'smoke')]) });
    const page = await mountPage();
    await page.findAll('button').find((b) => b.text().startsWith('Copy'))!.trigger('click');
    await flushPromises();
    expect(writeText).toHaveBeenCalledWith('203.0.113.1:32768');
    expect(page.get('[role="status"]').text()).toBe('Copied 203.0.113.1:32768.');
    Object.defineProperty(navigator, 'clipboard', { value: undefined, configurable: true });
  });

  it('names a bought address, checking the name first', async () => {
    addresses.mockResolvedValue({ ok: true, value: addressesResponse([], { unnamed: 1 }) });
    const page = await mountPage();
    expect(page.get('#addresses-name').element.parentElement!.textContent).toContain('One static address you bought waits');
    expect(page.text()).toContain('.play.opennova.net');
    expect(page.text()).not.toContain('You have no static address yet');

    await page.get('#address-name').setValue('Night_Ops');
    await page.get('form').trigger('submit');
    await flushPromises();
    expect(name).not.toHaveBeenCalled();
    expect(page.get('#address-name').attributes('aria-invalid')).toBe('true');
    expect(page.get('#address-name-hint').text()).toContain('lower-case letters');

    name.mockResolvedValueOnce({
      ok: false,
      error: { code: 'name_taken', status: 409, message: 'Another static address already has that name. Pick another.' },
    });
    await page.get('#address-name').setValue('smoke');
    await page.get('form').trigger('submit');
    await flushPromises();
    expect(name).toHaveBeenCalledWith('smoke');
    expect(page.get('#address-name-hint').text()).toBe('Another static address already has that name. Pick another.');

    name.mockResolvedValueOnce({ ok: true, value: address(4, 'night-ops', { state: 'allocating', public_ip: null }) });
    addresses.mockResolvedValue({
      ok: true,
      value: addressesResponse([address(4, 'night-ops', { state: 'allocating', public_ip: null })]),
    });
    await page.get('#address-name').setValue('night-ops');
    await page.get('form').trigger('submit');
    await flushPromises();
    expect(page.get('[role="status"]').text()).toBe('night-ops.play.opennova.net is being set up. It is ready in a minute or two.');
    expect(card(4).textContent).toContain('Being set up');
    expect(page.find('#address-name').exists()).toBe(false);
  });

  it("cancels a subscription's address through the portal's cancel flow", async () => {
    addresses.mockResolvedValue({ ok: true, value: addressesResponse([address(1, 'smoke')]) });
    portal.mockResolvedValue({ ok: true, value: PORTAL_URL });
    const page = await mountPage();
    await page.findAll('button').find((b) => b.text().startsWith('Cancel'))!.trigger('click');
    await flushPromises();
    expect(portal).toHaveBeenCalledWith('sub_address_1');
    expect(navigate).toHaveBeenCalledWith(PORTAL_URL);
  });

  it('offers no cancel once the subscription is canceled, saying when it ends', async () => {
    billing.mockResolvedValue({
      ok: true,
      value: myBilling({ static_addresses: 1, address_subscriptions: [addressSubscription({ cancel_at_period_end: true })] }),
    });
    addresses.mockResolvedValue({ ok: true, value: addressesResponse([address(1, 'smoke')]) });
    const page = await mountPage();
    expect(card(1).textContent).toContain('Its own subscription, canceled: it ends on');
    expect(page.findAll('button').some((b) => b.text().startsWith('Cancel'))).toBe(false);
  });

  it('reads again while an address is being set up', async () => {
    vi.useFakeTimers({ toFake: ['setTimeout', 'clearTimeout'] });
    addresses.mockResolvedValueOnce({
      ok: true,
      value: addressesResponse([address(1, 'smoke', { state: 'allocating', public_ip: null })]),
    });
    addresses.mockResolvedValue({ ok: true, value: addressesResponse([address(1, 'smoke')]) });
    await mountPage();
    expect(card(1).textContent).toContain('Being set up');
    await vi.advanceTimersByTimeAsync(5_000);
    await flushPromises();
    expect(card(1).textContent).toContain('Ready');
    // Nothing moves on its own now: no further read.
    const reads = addresses.mock.calls.length;
    await vi.advanceTimersByTimeAsync(60_000);
    expect(addresses.mock.calls.length).toBe(reads);
  });

  it('says when the account has none, and when the service offers none', async () => {
    addresses.mockResolvedValue({ ok: true, value: addressesResponse([]) });
    const page = await mountPage();
    expect(page.text()).toContain('You have no static address yet.');
    page.unmount();

    addresses.mockResolvedValue({ ok: true, value: addressesResponse([], { enabled: false }) });
    const off = await mountPage();
    expect(off.text()).toContain('NovaWorld does not offer static addresses right now.');
  });
});
