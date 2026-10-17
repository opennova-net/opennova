import { flushPromises, mount } from '@vue/test-utils';
import { afterEach, describe, expect, it, vi } from 'vitest';
import LobbyServerPage from './LobbyServerPage.vue';
import { fetchLobbyServer, LobbyGone } from '../api/lobbies';
import { testRouter } from '../test/router';
import type { LobbyServer } from '../types/lobby';

vi.mock('../api/lobbies', async (importOriginal) => ({
  ...(await importOriginal<typeof import('../api/lobbies')>()),
  fetchLobbyServer: vi.fn(),
}));

const read = vi.mocked(fetchLobbyServer);

function listed(overrides: Partial<LobbyServer> = {}): LobbyServer {
  return {
    id: 167772173,
    serverName: 'Retail Joint Ops: Escalation',
    hostIp: '3.232.55.36',
    hostPort: 32768,
    players: 2,
    maxPlayers: 32,
    mission: 'AS - Kombang Valley',
    gameType: 'AAS',
    expansion: 'Escalation',
    locked: false,
    game: { slug: 'jop_2_consumer', displayName: 'Joint Operations' },
    timeLeft: 23,
    timeOfDay: 'Unknown',
    message: '!Put your message here',
    roster: ['biggy', 'Ann'],
    scoreboard: null,
    ...overrides,
  };
}

describe('LobbyServerPage', () => {
  let wrapper: ReturnType<typeof mount> | null = null;
  afterEach(() => {
    wrapper?.unmount();
    wrapper = null;
  });

  async function open() {
    const router = await testRouter('/lobby/167772173', ['/lobby/:id']);
    wrapper = mount(LobbyServerPage, { global: { plugins: [router] } });
    await flushPromises();
    return wrapper;
  }

  it("shows the game info, with the host's default message and an unknown time of day left out", async () => {
    read.mockResolvedValue(listed());
    const page = await open();
    expect(read).toHaveBeenCalledWith(167772173, expect.anything());
    expect(page.get('h1').text()).toBe('Retail Joint Ops: Escalation');
    const info = page.get('dl').text();
    expect(info).toContain('AS - Kombang Valley (AAS)');
    expect(info).toContain('23 min');
    expect(info).toContain('Escalation');
    expect(info).toContain('3.232.55.36:32768');
    expect(page.text()).not.toContain('Put your message here');
    expect(info).not.toContain('Unknown');
    // No scores: the names who joined.
    expect(page.findAll('li').map((li) => li.text())).toEqual(['biggy', 'Ann']);
  });

  it('shows a hosted server its scoreboard, most kills first', async () => {
    read.mockResolvedValue(
      listed({
        scoreboard: [
          { name: 'Ann', team: 1, kills: 5, deaths: 2 },
          { name: 'Bo', team: 2, kills: 9, deaths: 1 },
        ],
      }),
    );
    const page = await open();
    const rows = page.findAll('tbody tr').map((tr) => tr.findAll('th, td').map((c) => c.text()));
    expect(rows).toEqual([
      ['Bo', '2', '9', '1'],
      ['Ann', '1', '5', '2'],
    ]);
  });

  it('says so once the server is no longer listed', async () => {
    read.mockRejectedValue(new LobbyGone());
    const page = await open();
    expect(page.text()).toContain('This server is no longer listed.');
  });
});
