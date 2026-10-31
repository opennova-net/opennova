import { flushPromises, mount } from '@vue/test-utils';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import AdminLayout from './AdminLayout.vue';
import { fetchAttention } from '../../api/attention';
import { attention, finding } from '../../test/usageFixtures';
import { testRouter } from '../../test/router';

// Whether the session the mock answers is an admin's, and whose (biggy sees
// billing).
const session = vi.hoisted(() => ({ admin: true, username: 'biggy' }));

vi.mock('../../composables/useSession', async () => {
  const { computed } = await import('vue');
  return {
    useSession: () => ({
      isAdmin: computed(() => session.admin),
      user: computed(() => (session.admin ? { id: 1, username: session.username, role: 'admin' } : null)),
      ready: computed(() => true),
    }),
  };
});
vi.mock('../../api/attention', async (importOriginal) => ({
  ...(await importOriginal<typeof import('../../api/attention')>()),
  fetchAttention: vi.fn(),
}));

const read = vi.mocked(fetchAttention);

describe('AdminLayout', () => {
  let wrapper: ReturnType<typeof mount> | null = null;

  async function mountLayout(path = '/admin/servers') {
    const router = await testRouter(path);
    wrapper = mount(AdminLayout, { global: { plugins: [router] } });
    await flushPromises();
    return { page: wrapper, router };
  }

  beforeEach(() => {
    vi.clearAllMocks();
    session.admin = true;
    session.username = 'biggy';
    read.mockResolvedValue({ ok: true, value: attention() });
  });

  afterEach(() => {
    wrapper?.unmount();
    wrapper = null;
    vi.useRealTimers();
  });

  it('counts what needs an admin on the Overview item, in words for a screen reader', async () => {
    const { page } = await mountLayout();
    expect(read).toHaveBeenCalledTimes(1);
    const overview = page.findAll('nav a').find((a) => a.text().startsWith('Overview'))!;
    expect(overview.get('.admin-attention-count').text()).toBe('3 things need you');
    expect(page.findAll('.admin-attention-count')).toHaveLength(1);
  });

  it('lists the six sections and marks the one open', async () => {
    const { page } = await mountLayout();
    const aside = page.get('aside nav');
    expect(aside.findAll('a').map((a) => a.text().replace(/\d.*$/, '').trim())).toEqual(['Overview', 'Players', 'Servers', 'Products', 'Billing', 'System']);
    expect(page.findAll('aside nav a[aria-current="page"]').map((a) => a.text())).toEqual(['Servers']);
  });

  it('marks a section for the pages under it', async () => {
    const router = await testRouter('/admin/machines/3');
    wrapper = mount(AdminLayout, { global: { plugins: [router] } });
    await flushPromises();
    expect(wrapper.findAll('aside nav a[aria-current="page"]').map((a) => a.text())).toEqual(['Servers']);
  });

  it('counts the releases awaiting approval on Products', async () => {
    read.mockResolvedValue({
      ok: true,
      value: attention({ findings: [finding('warn', 'Awaits approval', { subject: 'NIGHTOPS 0.3.0', link: '/admin/products/NIGHTOPS', source: 'releases' })], alarms: [] }),
    });
    const { page } = await mountLayout();
    const products = page.findAll('aside nav a').find((a) => a.text().startsWith('Products'))!;
    expect(products.get('.admin-awaiting-count').text()).toBe('1 awaiting approval');
  });

  it('opens a modal mobile drawer and restores page scrolling when closed', async () => {
    const { page } = await mountLayout();
    const dialog = page.get('dialog').element as HTMLDialogElement;
    dialog.showModal = vi.fn(() => { dialog.setAttribute('open', ''); });
    dialog.close = vi.fn(() => { dialog.removeAttribute('open'); dialog.dispatchEvent(new Event('close')); });
    await page.get('button[aria-controls^="workspace-drawer"]').trigger('click');
    await flushPromises();
    expect(dialog.showModal).toHaveBeenCalledOnce();
    expect(page.get('button[aria-controls^="workspace-drawer"]').attributes('aria-expanded')).toBe('true');
    expect(page.get('dialog nav').findAll('a')).toHaveLength(6);
    expect(document.body.style.overflow).toBe('hidden');
    await page.get('dialog button').trigger('click');
    await flushPromises();
    expect(page.get('button[aria-controls^="workspace-drawer"]').attributes('aria-expanded')).toBe('false');
    expect(document.body.style.overflow).toBe('');
  });

  it('shows no count when nothing needs anyone', async () => {
    read.mockResolvedValue({ ok: true, value: attention({ findings: [], alarms: [] }) });
    const { page } = await mountLayout();
    expect(page.find('.admin-attention-count').exists()).toBe(false);
  });

  it('shows a question mark rather than a stale count while the read fails', async () => {
    vi.useFakeTimers({ toFake: ['setTimeout', 'clearTimeout'] });
    const { page } = await mountLayout();
    expect(page.find('.admin-attention-count').exists()).toBe(true);
    read.mockResolvedValue({ ok: false, error: { code: 'network', status: null, message: 'The service could not be reached.' } });
    await vi.advanceTimersByTimeAsync(60_000);
    await flushPromises();
    expect(read).toHaveBeenCalledTimes(2);
    expect(page.find('.admin-attention-count').exists()).toBe(false);
    expect(page.get('.admin-attention-unknown').text()).toBe('?What needs you could not be read');
    // An answer again (the retry, 5 s on): the count again.
    read.mockResolvedValue({ ok: true, value: attention() });
    await vi.advanceTimersByTimeAsync(5_000);
    await flushPromises();
    expect(page.find('.admin-attention-unknown').exists()).toBe(false);
    expect(page.get('.admin-attention-count').text()).toBe('3 things need you');
  });

  it('shows another admin no Billing section, and sends its pages to the Overview', async () => {
    session.username = 'ghost';
    const { page, router } = await mountLayout('/admin/billing/costs');
    const aside = page.get('aside nav');
    expect(aside.findAll('a').map((a) => a.text().replace(/\d.*$/, '').trim())).toEqual(['Overview', 'Players', 'Servers', 'Products', 'System']);
    expect(router.currentRoute.value.path).toBe('/admin');
  });

  it('keeps biggy on a Billing page', async () => {
    const { router } = await mountLayout('/admin/billing/costs');
    expect(router.currentRoute.value.path).toBe('/admin/billing/costs');
  });

  it('counts nothing that opens the Billing section for another admin', async () => {
    session.username = 'ghost';
    read.mockResolvedValue({
      ok: true,
      value: attention({
        findings: [
          finding('danger', 'failed to allocate.', { subject: 'address 3', link: '/admin/billing/addresses', source: 'addresses' }),
          finding('warn', 'Machine 4 runs an older hostd than the release.', { subject: 'machine 4', link: '/admin/machines/4' }),
        ],
        alarms: [{ name: 'opennova-prod-billing-sync-stale', state_since: null, description: '' }],
      }),
    });
    const { page } = await mountLayout();
    expect(page.get('.admin-attention-count').text()).toBe('1 thing needs you');
  });

  it('sends no admin request while its gate is closed', async () => {
    session.admin = false;
    await mountLayout();
    expect(read).not.toHaveBeenCalled();
  });
});
