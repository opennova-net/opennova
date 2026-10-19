import { flushPromises, mount } from '@vue/test-utils';
import { afterEach, beforeEach, describe, expect, it, vi, type MockInstance } from 'vitest';
import NovaWorldLobbies from './NovaWorldLobbies.vue';
import { drainLobby, fetchLobbies } from '../api/admin';
import type { AdminLobby } from '../types/admin';
import { testRouter } from '../test/router';

vi.mock('../api/admin', () => ({ fetchLobbies: vi.fn(), drainLobby: vi.fn() }));

const lobbies = vi.mocked(fetchLobbies);
const drain = vi.mocked(drainLobby);

const NOW = Date.UTC(2026, 9, 19, 21, 0, 0);

function lobby(id: number, over: Partial<AdminLobby> = {}): AdminLobby {
  return {
    id,
    endpoint: '203.0.113.20:64206',
    state: 'active',
    task_id: `7a1e${id}c0ffee0123456789abcdef`,
    started_ms: NOW - 2 * 3_600_000,
    last_seen_ms: NOW - 3_000,
    sessions: 4,
    ...over,
  };
}

describe('NovaWorldLobbies', () => {
  let wrapper: ReturnType<typeof mount> | null = null;
  let confirm: MockInstance<typeof window.confirm>;

  async function mountBlock(list: AdminLobby[]) {
    lobbies.mockResolvedValue({ ok: true, value: { lobbies: list } });
    const router = await testRouter('/admin/novaworld');
    wrapper = mount(NovaWorldLobbies, { global: { plugins: [router] } });
    await flushPromises();
    return wrapper;
  }

  beforeEach(() => {
    vi.clearAllMocks();
    vi.useFakeTimers({ toFake: ['Date'] });
    vi.setSystemTime(NOW);
    confirm = vi.spyOn(window, 'confirm').mockReturnValue(true);
    drain.mockResolvedValue({ ok: true, value: undefined });
  });

  afterEach(() => {
    wrapper?.unmount();
    wrapper = null;
    confirm.mockRestore();
    vi.useRealTimers();
  });

  it('lists each lobby with its state, sessions and task, linked to its usage', async () => {
    const page = await mountBlock([
      lobby(3),
      lobby(4, { state: 'draining', sessions: 1 }),
      lobby(5, { last_seen_ms: NOW - 60_000, task_id: '' }),
    ]);
    expect(page.get('h2').text()).toBe('Lobbies');
    const headers = page.findAll('thead th').map((th) => th.text());
    expect(headers).toEqual(['Lobby', 'State', 'Sessions', 'Task', 'Started', 'Last seen', 'Actions']);
    const [taking, draining, unseen] = page.findAll('tr.lobby-row');
    expect(taking.get('th').text()).toBe('Lobby 3 203.0.113.20:64206');
    expect(taking.findAll('td')[0].text()).toBe('Taking clients');
    expect(taking.findAll('td')[1].text()).toBe('4 sessions');
    const usage = taking.get('td a');
    expect(usage.text()).toBe('7a1e3c0ffee0');
    expect(usage.attributes('href')).toBe('/admin/infrastructure/programs/lobby?task=7a1e3c0ffee0123456789abcdef');
    expect(taking.findAll('td')[3].text()).toBe('2 hours ago');
    expect(taking.findAll('td')[4].text()).toBe('just now');
    expect(taking.text()).toContain('Drain');
    expect(draining.findAll('td')[0].text()).toBe('Draining');
    expect(draining.findAll('td')[1].text()).toBe('1 session');
    expect(draining.find('button').exists()).toBe(false);
    // Unseen for a minute: its process may be gone; with no task named, the program's usage.
    expect(unseen.findAll('td')[0].text()).toBe('Not seen');
    expect(unseen.get('td a').text()).toBe('Its usage');
    expect(unseen.get('td a').attributes('href')).toBe('/admin/infrastructure/programs/lobby');
  });

  it('says so when no lobby is registered', async () => {
    const page = await mountBlock([]);
    expect(page.get('tbody').text()).toBe('No lobby instance is registered.');
  });

  it('drains a lobby once confirmed, then reads again', async () => {
    const page = await mountBlock([lobby(3)]);
    confirm.mockReturnValueOnce(false);
    await page.get('tr.lobby-row button').trigger('click');
    expect(drain).not.toHaveBeenCalled();
    await page.get('tr.lobby-row button').trigger('click');
    await flushPromises();
    expect(confirm).toHaveBeenLastCalledWith(expect.stringContaining('Drain lobby 3? The gates hand it no new client'));
    expect(confirm).toHaveBeenLastCalledWith(expect.stringContaining('the 4 sessions it holds stay until they leave'));
    expect(drain).toHaveBeenCalledWith(3);
    expect(page.get('[role="status"]').text()).toBe('Lobby 3 is draining.');
    expect(lobbies).toHaveBeenCalledTimes(2);
  });

  it('shows a refusal in words', async () => {
    const page = await mountBlock([lobby(3)]);
    drain.mockResolvedValueOnce({ ok: false, error: { code: 'not_found', status: 404, message: 'It no longer exists. Refresh the list.' } });
    await page.get('tr.lobby-row button').trigger('click');
    await flushPromises();
    expect(page.get('[role="alert"]').text()).toBe('It no longer exists. Refresh the list.');
  });

  it('says so when the read fails', async () => {
    lobbies.mockResolvedValue({ ok: false, error: { code: 'db_error', status: 500, message: 'The server hit a database error. Try again shortly.' } });
    const router = await testRouter('/admin/novaworld');
    const page = mount(NovaWorldLobbies, { global: { plugins: [router] } });
    wrapper = page;
    await flushPromises();
    expect(page.get('[role="alert"]').text()).toBe('The server hit a database error. Try again shortly.');
    expect(page.get('tbody').text()).toBe('Not loaded.');
  });
});
