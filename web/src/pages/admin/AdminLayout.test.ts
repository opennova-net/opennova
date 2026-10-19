import { flushPromises, mount } from '@vue/test-utils';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import AdminLayout from './AdminLayout.vue';
import { fetchAttention } from '../../api/attention';
import { attention } from '../../test/usageFixtures';
import { testRouter } from '../../test/router';

// Whether the session the mock answers is an admin's.
const session = vi.hoisted(() => ({ admin: true }));

vi.mock('../../composables/useSession', async () => {
  const { computed } = await import('vue');
  return {
    useSession: () => ({
      isAdmin: computed(() => session.admin),
      user: computed(() => (session.admin ? { username: 'admin', role: 'admin' } : null)),
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

  async function mountLayout() {
    const router = await testRouter('/admin/fleet');
    wrapper = mount(AdminLayout, { global: { plugins: [router] } });
    await flushPromises();
    return wrapper;
  }

  beforeEach(() => {
    vi.clearAllMocks();
    session.admin = true;
    read.mockResolvedValue({ ok: true, value: attention() });
  });

  afterEach(() => {
    wrapper?.unmount();
    wrapper = null;
    vi.useRealTimers();
  });

  it('counts what needs an admin on the Overview item, in words for a screen reader', async () => {
    const page = await mountLayout();
    expect(read).toHaveBeenCalledTimes(1);
    const overview = page.findAll('nav a').find((a) => a.text().startsWith('Overview'))!;
    expect(overview.get('.admin-attention-count').text()).toBe('3 things need you');
    expect(page.findAll('.admin-attention-count')).toHaveLength(1);
  });

  it('groups the pages by area, each named, and marks the one open', async () => {
    const page = await mountLayout();
    const areas = page.findAll('.admin-nav-area').map((area) => [area.get('p').text(), area.findAll('a').map((a) => a.text())]);
    expect(areas).toEqual([
      ['NovaWorld', ['Status and sessions', 'CD keys']],
      ['Hosting', ['Fleet', 'Server usage', 'Static addresses', 'Game data']],
      ['People and money', ['Users', 'Subscriptions', 'AWS costs']],
      ['Operations', ['Infrastructure', 'Background jobs']],
    ]);
    // Each area is a group its name labels, for a screen reader.
    const hosting = page.findAll('.admin-nav-area')[1];
    expect(hosting.attributes('role')).toBe('group');
    expect(hosting.attributes('aria-labelledby')).toBe(hosting.get('p').attributes('id'));
    const current = page.findAll('nav a[aria-current="page"]');
    expect(current.map((a) => a.text())).toEqual(['Fleet']);
  });

  it('marks a section for the pages under it', async () => {
    const router = await testRouter('/admin/fleet/machines/3');
    wrapper = mount(AdminLayout, { global: { plugins: [router] } });
    await flushPromises();
    expect(wrapper.findAll('nav a[aria-current="page"]').map((a) => a.text())).toEqual(['Fleet']);
  });

  it('shows no count when nothing needs anyone', async () => {
    read.mockResolvedValue({ ok: true, value: attention({ findings: [], alarms: [] }) });
    const page = await mountLayout();
    expect(page.find('.admin-attention-count').exists()).toBe(false);
  });

  it('shows a question mark rather than a stale count while the read fails', async () => {
    vi.useFakeTimers({ toFake: ['setTimeout', 'clearTimeout'] });
    const page = await mountLayout();
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

  it('sends no admin request while its gate is closed', async () => {
    session.admin = false;
    await mountLayout();
    expect(read).not.toHaveBeenCalled();
  });
});
