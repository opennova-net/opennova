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
  });

  it('counts what needs an admin on the Overview item, in words for a screen reader', async () => {
    const page = await mountLayout();
    expect(read).toHaveBeenCalledTimes(1);
    const overview = page.findAll('nav a').find((a) => a.text().startsWith('Overview'))!;
    expect(overview.get('.admin-attention-count').text()).toBe('3 things need you');
    expect(page.findAll('.admin-attention-count')).toHaveLength(1);
  });

  it('shows no count when nothing needs anyone', async () => {
    read.mockResolvedValue({ ok: true, value: attention({ findings: [], alarms: [] }) });
    const page = await mountLayout();
    expect(page.find('.admin-attention-count').exists()).toBe(false);
  });

  it('sends no admin request while its gate is closed', async () => {
    session.admin = false;
    await mountLayout();
    expect(read).not.toHaveBeenCalled();
  });
});
