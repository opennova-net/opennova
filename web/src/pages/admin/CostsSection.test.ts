import { flushPromises, mount } from '@vue/test-utils';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import CostsSection from './CostsSection.vue';
import { fetchCosts } from '../../api/costs';
import { costs, localCosts } from '../../test/costFixtures';
import { testRouter } from '../../test/router';

vi.mock('../../api/costs', async (importOriginal) => ({
  ...(await importOriginal<typeof import('../../api/costs')>()),
  fetchCosts: vi.fn(),
}));

const read = vi.mocked(fetchCosts);

describe('CostsSection', () => {
  let wrapper: ReturnType<typeof mount> | null = null;

  async function mountSection() {
    const router = await testRouter('/admin/costs');
    wrapper = mount(CostsSection, { global: { plugins: [router] } });
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

  it("shows the month's AWS bill against the budget", async () => {
    const page = await mountSection();
    expect(read).toHaveBeenCalledTimes(1);
    expect(page.get('h1').text()).toBe('AWS costs');
    expect(page.get('.cost-budget-state').text()).toContain('Within the budget');
    expect(page.findAll('.cost-service')).toHaveLength(5);
  });

  it('reads once and on Refresh, never on a timer (web keeps the read 6 hours)', async () => {
    vi.useFakeTimers({ toFake: ['setTimeout', 'clearTimeout'] });
    const page = await mountSection();
    await vi.advanceTimersByTimeAsync(10 * 60_000);
    expect(read).toHaveBeenCalledTimes(1);
    await page.get('header button').trigger('click');
    await flushPromises();
    expect(read).toHaveBeenCalledTimes(2);
  });

  it('says the local stack reads none', async () => {
    read.mockResolvedValue({ ok: true, value: localCosts() });
    const page = await mountSection();
    expect(page.get('.cost-unavailable').text()).toContain('AWS costs are not read on this stack.');
  });

  it("shows why the read failed and no number", async () => {
    read.mockResolvedValue({ ok: false, error: { code: 'network', status: null, message: 'Could not reach the server.' } });
    const page = await mountSection();
    expect(page.get('[role="alert"]').text()).toBe('Could not reach the server.');
    expect(page.find('.cost-summary').exists()).toBe(false);
  });
});
