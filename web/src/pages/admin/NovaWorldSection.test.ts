import { flushPromises, mount } from '@vue/test-utils';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import NovaWorldSection from './NovaWorldSection.vue';
import { fetchConnections, fetchLobbies, fetchServerStatus } from '../../api/admin';
import { testRouter } from '../../test/router';

vi.mock('../../api/admin', () => ({
  fetchConnections: vi.fn(),
  fetchLobbies: vi.fn(),
  fetchServerStatus: vi.fn(),
  updateServerStatus: vi.fn(),
  endConnection: vi.fn(),
  stopListedServer: vi.fn(),
  drainLobby: vi.fn(),
}));

const connections = vi.mocked(fetchConnections);
const lobbies = vi.mocked(fetchLobbies);
const status = vi.mocked(fetchServerStatus);

describe('NovaWorldSection', () => {
  let wrapper: ReturnType<typeof mount> | null = null;

  async function mountSection() {
    const router = await testRouter('/admin/novaworld');
    wrapper = mount(NovaWorldSection, { global: { plugins: [router] } });
    await flushPromises();
    return wrapper;
  }

  beforeEach(() => {
    vi.clearAllMocks();
    status.mockResolvedValue({ ok: true, value: { maintenance_enabled: false, message: '' } });
    connections.mockResolvedValue({ ok: true, value: { count: 0, heartbeat_timeout_ms: 240_000, connections: [] } });
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

  it('is the NovaWorld service in one page: its sign-in, who is signed in, then the lobbies', async () => {
    const page = await mountSection();
    expect(page.get('nav[aria-label="Breadcrumb"]').text()).toBe('NovaWorld');
    expect(page.get('h1').text()).toBe('Status and sessions');
    expect(page.findAll('h2').map((h) => h.text())).toEqual(['Sign-in', 'Signed in now', 'Lobbies']);
    expect(page.findAll('#sessions h3').map((h) => h.text())).toEqual(['Clients', 'Servers']);
    expect(page.get('.novaworld-state').text()).toContain('NovaWorld is open');
    expect(page.text()).toContain('No game client is signed in.');
    expect(page.text()).toContain('No server is signed in.');
    // The old sessions page's links land on its section; each lobby links to its task's usage.
    expect(page.find('#sessions').exists()).toBe(true);
    expect(page.find('#lobbies').exists()).toBe(true);
    expect(page.get('#lobbies tr.lobby-row a').attributes('href')).toBe(
      '/admin/infrastructure/programs/lobby?task=7a1e3c0ffee0123456789abcdef',
    );
    const links = page.findAll('header a').map((a) => a.attributes('href'));
    expect(links).toEqual(['/admin/infrastructure', '/admin/cd-keys']);
  });

  it('reads them all again on Refresh', async () => {
    const page = await mountSection();
    expect(status).toHaveBeenCalledTimes(1);
    expect(connections).toHaveBeenCalledTimes(1);
    expect(lobbies).toHaveBeenCalledTimes(1);
    await page.get('header button').trigger('click');
    await flushPromises();
    expect(status).toHaveBeenCalledTimes(2);
    expect(connections).toHaveBeenCalledTimes(2);
    expect(lobbies).toHaveBeenCalledTimes(2);
  });
});
