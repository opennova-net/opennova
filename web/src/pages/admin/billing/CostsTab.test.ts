import { flushPromises, mount } from '@vue/test-utils';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import CostsTab from './CostsTab.vue';
import { fetchCosts } from '../../../api/costs';
import { combinedCosts, costs, localCosts } from '../../../test/costFixtures';
import { testRouter } from '../../../test/router';

vi.mock('../../../api/costs', async (importOriginal) => ({
  ...(await importOriginal<typeof import('../../../api/costs')>()),
  fetchCosts: vi.fn(),
}));

const read = vi.mocked(fetchCosts);

describe('CostsTab', () => {
  let wrapper: ReturnType<typeof mount> | null = null;

  async function mountTab() {
    const router = await testRouter('/admin/billing/costs');
    wrapper = mount(CostsTab, { global: { plugins: [router] } });
    await flushPromises();
    return wrapper;
  }

  beforeEach(() => {
    vi.clearAllMocks();
    read.mockResolvedValue({ ok: true, value: costs() });
  });

  afterEach(() => {
    wrapper?.unmount();
    wrapper = null;
    vi.useRealTimers();
  });

  it("is the Billing section's AWS costs tab: the month's bill against the budget, and no footnote", async () => {
    const page = await mountTab();
    expect(read).toHaveBeenCalledTimes(1);
    expect(page.get('h1').text()).toBe('Billing');
    expect(page.get('.tab-nav a[aria-current="page"]').text()).toBe('AWS costs');
    expect(page.get('.page-meta').text()).toContain('Read from AWS');
    expect(page.get('.page-meta').text()).toContain('Estimate');
    expect(page.get('.cost-budget-state').text()).toContain('Within the budget');
    expect(page.findAll('.cost-service')).toHaveLength(5);
    expect(page.text()).not.toContain('6 hours');
    expect(page.text()).not.toContain('Cost Explorer');
  });

  it('names the accounts that add up in the meta line', async () => {
    read.mockResolvedValue({ ok: true, value: combinedCosts() });
    const page = await mountTab();
    expect(page.get('.cost-scope').text()).toBe('Production + CI');
    expect(page.findAll('.cost-account')).toHaveLength(2);
  });

  it('reads once and on Refresh, never on a timer', async () => {
    vi.useFakeTimers({ toFake: ['setTimeout', 'clearTimeout'] });
    const page = await mountTab();
    await vi.advanceTimersByTimeAsync(10 * 60_000);
    expect(read).toHaveBeenCalledTimes(1);
    await page.findAll('header button').find((b) => b.text() === 'Refresh')!.trigger('click');
    await flushPromises();
    expect(read).toHaveBeenCalledTimes(2);
  });

  it('says the local stack reads none', async () => {
    read.mockResolvedValue({ ok: true, value: localCosts() });
    const page = await mountTab();
    expect(page.get('.cost-unavailable').text()).toBe('AWS costs are not read on this stack.');
    expect(page.find('.page-meta').exists()).toBe(false);
  });

  it('shows why the read failed and no number', async () => {
    read.mockResolvedValue({ ok: false, error: { code: 'network', status: null, message: 'Could not reach the server.' } });
    const page = await mountTab();
    expect(page.get('[role="alert"]').text()).toBe('Could not reach the server.');
    expect(page.find('.cost-summary').exists()).toBe(false);
  });
});
