import { flushPromises, mount } from '@vue/test-utils';
import { beforeEach, describe, expect, it, vi } from 'vitest';
import HostingPage from './HostingPage.vue';
import { fetchTiers } from '../api/billing';
import { tiersResponse } from '../test/billingFixtures';
import { testRouter } from '../test/router';

vi.mock('../api/billing', async (importOriginal) => ({
  ...(await importOriginal<typeof import('../api/billing')>()),
  fetchTiers: vi.fn(),
}));

const tiers = vi.mocked(fetchTiers);

async function mountPage() {
  const router = await testRouter('/hosting');
  const wrapper = mount(HostingPage, { global: { plugins: [router] } });
  await flushPromises();
  return { wrapper, router };
}

describe('HostingPage', () => {
  beforeEach(() => vi.clearAllMocks());

  it('shows the tiers, the dedicated machines, the way to run it yourself and the fine print', async () => {
    tiers.mockResolvedValue({ ok: true, value: tiersResponse() });
    const { wrapper } = await mountPage();
    expect(wrapper.get('h1').text()).toBe('Hosting');
    expect(wrapper.findAll('article')).toHaveLength(3);
    expect(wrapper.text()).toContain('$5 a month');
    expect(wrapper.text()).toContain('$15 a month');
    expect(wrapper.get('#public-own-machine').text()).toContain('$45 a month each');
    expect(wrapper.get('#run-it-yourself a').attributes('href')).toMatch(/apps\/serve\/README\.md$/);
    // "Not the sale of any game" once, in the fine print with the terms and refunds.
    expect(wrapper.text().match(/not the sale of any game/g)).toHaveLength(1);
    const hrefs = wrapper.findAll('a').map((a) => a.attributes('href'));
    expect(hrefs).toContain('/terms');
    expect(hrefs).toContain('/refunds');
    // No CLI flags or protocol terms on a player page.
    expect(wrapper.text()).not.toMatch(/--master-host|opennova-serve|—/);
  });

  it('sends a visitor not logged in to log in, then to Billing with the tier', async () => {
    tiers.mockResolvedValue({ ok: true, value: tiersResponse() });
    const { wrapper, router } = await mountPage();
    await wrapper.get('#public-support-tier-3 button').trigger('click');
    await flushPromises();
    expect(router.currentRoute.value.path).toBe('/login');
    expect(router.currentRoute.value.query.redirect).toBe('/me/billing?tier=3');
  });

  it('says when hosting is not open, and an error in its words', async () => {
    tiers.mockResolvedValue({ ok: true, value: tiersResponse({ enabled: false }) });
    let { wrapper } = await mountPage();
    expect(wrapper.text()).toContain('Hosting is not open yet.');
    expect(wrapper.findAll('article')).toHaveLength(0);
    wrapper.unmount();
    tiers.mockResolvedValue({ ok: false, error: { code: 'network', status: null, message: 'Could not reach the server.' } });
    ({ wrapper } = await mountPage());
    expect(wrapper.get('[role="alert"]').text()).toBe('Could not reach the server.');
  });
});
