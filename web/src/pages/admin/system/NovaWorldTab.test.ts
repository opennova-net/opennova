import { flushPromises, mount } from '@vue/test-utils';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import NovaWorldTab from './NovaWorldTab.vue';
import { fetchLobbies, fetchServerStatus } from '../../../api/admin';
import { testRouter } from '../../../test/router';

vi.mock('../../../api/admin', () => ({
  fetchLobbies: vi.fn(),
  fetchServerStatus: vi.fn(),
  updateServerStatus: vi.fn(),
  drainLobby: vi.fn(),
}));

const lobbies = vi.mocked(fetchLobbies);
const status = vi.mocked(fetchServerStatus);

describe('NovaWorldTab', () => {
  let wrapper: ReturnType<typeof mount> | null = null;

  async function mountTab() {
    const router = await testRouter('/admin/system');
    wrapper = mount(NovaWorldTab, { global: { plugins: [router] } });
    await flushPromises();
    return wrapper;
  }

  beforeEach(() => {
    vi.clearAllMocks();
    status.mockResolvedValue({ ok: true, value: { maintenance_enabled: false, message: '' } });
    lobbies.mockResolvedValue({
      ok: true,
      value: {
        lobbies: [
          { id: 3, endpoint: '203.0.113.20:64206', state: 'active', task_id: '7a1e3c0ffee0123456789abcdef', started_ms: Date.now() - 60_000, last_seen_ms: Date.now(), sessions: 0 },
        ],
      },
    });
  });

  afterEach(() => {
    wrapper?.unmount();
    wrapper = null;
  });

  it("is the System section's NovaWorld tab: its sign-in and its lobbies, no sessions and no prose", async () => {
    const page = await mountTab();
    expect(page.get('h1').text()).toBe('System');
    expect(page.findAll('.tab-nav a').map((a) => a.text())).toEqual(['NovaWorld', 'Infrastructure', 'Jobs']);
    expect(page.get('.tab-nav a[aria-current="page"]').text()).toBe('NovaWorld');
    expect(page.findAll('h2').map((h) => h.text().replace(/\s+/g, ' '))).toEqual(['Sign-in Open', 'Lobbies']);
    expect(page.find('#sessions').exists()).toBe(false);
    expect(page.findAll('p').filter((p) => p.text().length > 80)).toHaveLength(0);
    expect(page.get('#lobbies tr.lobby-row a').attributes('href')).toBe('/admin/programs/lobby?task=7a1e3c0ffee0123456789abcdef');
  });

  it('reads both again on Refresh', async () => {
    const page = await mountTab();
    expect(status).toHaveBeenCalledTimes(1);
    expect(lobbies).toHaveBeenCalledTimes(1);
    await page.findAll('header button').find((b) => b.text() === 'Refresh')!.trigger('click');
    await flushPromises();
    expect(status).toHaveBeenCalledTimes(2);
    expect(lobbies).toHaveBeenCalledTimes(2);
  });
});
