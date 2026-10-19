import { flushPromises, mount } from '@vue/test-utils';
import { beforeEach, describe, expect, it, vi } from 'vitest';
import { createMemoryHistory, createRouter } from 'vue-router';
import SupportPage from './SupportPage.vue';
import { fetchTiers } from '../api/billing';
import { tiersResponse } from '../test/billingFixtures';

vi.mock('../api/billing', async (importOriginal) => ({
  ...(await importOriginal<typeof import('../api/billing')>()),
  fetchTiers: vi.fn(),
}));

const tiers = vi.mocked(fetchTiers);

async function mountPage() {
  const router = createRouter({
    history: createMemoryHistory(),
    routes: [{ path: '/:rest(.*)*', component: { render: () => null } }],
  });
  await router.push('/support');
  const wrapper = mount(SupportPage, { global: { plugins: [router] } });
  await flushPromises();
  return { wrapper, router };
}

describe('SupportPage', () => {
  beforeEach(() => vi.clearAllMocks());

  it('shows the tiers and their prices to a visitor not logged in', async () => {
    tiers.mockResolvedValue({ ok: true, value: tiersResponse() });
    const { wrapper } = await mountPage();
    expect(wrapper.text()).toContain('$5 a month');
    expect(wrapper.text()).toContain('$10 a month');
    expect(wrapper.text()).toContain('$15 a month');
    expect(wrapper.get('#public-own-machine').text()).toBe('Dedicated machines');
    expect(wrapper.text()).toContain('$45 a month for each');
    expect(wrapper.text()).toContain('No support tier needed');
    expect(wrapper.text()).toContain('not the sale of any game');
    expect(wrapper.text()).not.toContain('—');
    // The terms and the refund policy beside Subscribe.
    const hrefs = wrapper.findAll('a').map((a) => a.attributes('href'));
    expect(hrefs).toContain('/terms');
    expect(hrefs).toContain('/refunds');
  });

  it('sends a visitor not logged in to log in, then to their support tier page', async () => {
    tiers.mockResolvedValue({ ok: true, value: tiersResponse() });
    const { wrapper, router } = await mountPage();
    await wrapper.get('#public-support-tier-3 button').trigger('click');
    await flushPromises();
    expect(router.currentRoute.value.path).toBe('/login');
    expect(router.currentRoute.value.query.redirect).toBe('/me/support');
  });

  it('says when the tiers are not open', async () => {
    tiers.mockResolvedValue({ ok: true, value: tiersResponse({ enabled: false }) });
    const { wrapper } = await mountPage();
    expect(wrapper.text()).toContain('Support tiers are not open on NovaWorld yet.');
    expect(wrapper.findAll('article')).toHaveLength(0);
  });
});
