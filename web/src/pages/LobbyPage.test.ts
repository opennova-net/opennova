import { flushPromises, mount } from '@vue/test-utils';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import LobbyPage from './LobbyPage.vue';
import { fetchLobbies, fetchNovaLogicLobbies } from '../api/lobbies';
import { testRouter } from '../test/router';
import type { LobbyResponse, NovaLogicLobbies } from '../types/lobby';

vi.mock('../api/lobbies', async (importOriginal) => ({
  ...(await importOriginal<typeof import('../api/lobbies')>()),
  fetchLobbies: vi.fn(),
  fetchNovaLogicLobbies: vi.fn(),
}));

const readOurs = vi.mocked(fetchLobbies);
const readNovaLogic = vi.mocked(fetchNovaLogicLobbies);

function ours(hosts = 0): LobbyResponse {
  return {
    games: [
      {
        slug: 'jop_2_consumer',
        displayName: 'Joint Operations',
        hosts: Array.from({ length: hosts }, (_, i) => ({
          id: 1000 + i,
          serverName: `Our Server ${i}`,
          players: 1,
          maxPlayers: 16,
        })),
      },
    ],
  };
}

function novalogic(overrides: Partial<NovaLogicLobbies> = {}): NovaLogicLobbies {
  return {
    enabled: true,
    servers: [
      {
        id: 167782351,
        serverName: 'Café © Ops',
        mission: 'AS - Kombang Valley',
        gameType: 'AAS',
        players: 12,
        maxPlayers: 32,
        locked: false,
      },
      { id: 167782352, serverName: 'Bravo', mission: '', gameType: 'TDM', players: 0, maxPlayers: 16, locked: true },
    ],
    fetchedAt: '2026-10-25T11:57:00Z',
    ageSeconds: 180,
    failing: false,
    ...overrides,
  };
}

describe('LobbyPage', () => {
  let wrapper: ReturnType<typeof mount> | null = null;
  beforeEach(() => {
    readOurs.mockReset();
    readNovaLogic.mockReset();
  });
  afterEach(() => {
    wrapper?.unmount();
    wrapper = null;
  });

  async function open() {
    const router = await testRouter('/lobby');
    wrapper = mount(LobbyPage, { global: { plugins: [router] } });
    await flushPromises();
    return wrapper;
  }

  function section(page: ReturnType<typeof mount>) {
    return page.find('section[aria-labelledby="novalogic-servers"]');
  }

  it("lists NovaLogic's servers in their own section, with the list's age", async () => {
    readOurs.mockResolvedValue(ours(1));
    readNovaLogic.mockResolvedValue(novalogic());
    const page = await open();
    expect(readNovaLogic).toHaveBeenCalled();
    // Ours as ever.
    expect(page.text()).toContain('Our Server 0');
    const own = section(page);
    expect(own.exists()).toBe(true);
    expect(own.get('h2').text()).toBe("NovaLogic's NovaWorld");
    expect(own.text()).toContain('2 servers');
    const rows = own.findAll('tbody tr');
    expect(rows).toHaveLength(2);
    expect(rows[0].text()).toContain('Café © Ops');
    expect(rows[0].text()).toContain('AS - Kombang Valley (AAS)');
    expect(rows[0].text()).toContain('12 / 32');
    expect(rows[0].text()).not.toContain('Password');
    expect(rows[1].text()).toContain('Bravo');
    expect(rows[1].text()).toContain('TDM');
    expect(rows[1].text()).toContain('Password');
    expect(own.text()).toContain('Updated 3 min ago');
    // A row is no link: the site has no page for another service's server.
    expect(own.findAll('a')).toHaveLength(0);
    // Player-facing copy: no em dash.
    expect(own.text()).not.toContain(String.fromCharCode(0x2014));
  });

  it('shows no section while the service does not list them', async () => {
    readOurs.mockResolvedValue(ours(1));
    readNovaLogic.mockResolvedValue(novalogic({ enabled: false, servers: [], fetchedAt: null, ageSeconds: null }));
    const page = await open();
    expect(section(page).exists()).toBe(false);
    expect(page.text()).not.toContain('NovaLogic');
  });

  it('says so before the first list, and when NovaLogic has not answered', async () => {
    readOurs.mockResolvedValue(ours(1));
    readNovaLogic.mockResolvedValue(novalogic({ servers: [], fetchedAt: null, ageSeconds: null }));
    let page = await open();
    expect(section(page).text()).toContain("Checking NovaLogic's NovaWorld...");
    expect(section(page).find('table').exists()).toBe(false);
    wrapper?.unmount();

    readNovaLogic.mockResolvedValue(novalogic({ servers: [], fetchedAt: null, ageSeconds: null, failing: true }));
    page = await open();
    expect(section(page).text()).toContain("NovaLogic's NovaWorld can't be reached right now.");
  });

  it('keeps an older list when the last fetch failed, and says when none are listed', async () => {
    readOurs.mockResolvedValue(ours(1));
    readNovaLogic.mockResolvedValue(novalogic({ failing: true, ageSeconds: 2 * 3600 }));
    let page = await open();
    expect(section(page).findAll('tbody tr')).toHaveLength(2);
    expect(section(page).text()).toContain('Updated 2 hours ago');
    wrapper?.unmount();

    readNovaLogic.mockResolvedValue(novalogic({ servers: [], ageSeconds: 10 }));
    page = await open();
    expect(section(page).text()).toContain('No servers are listed there right now.');
    expect(section(page).text()).toContain('Updated just now');
  });

  it("never lets NovaLogic's list break ours", async () => {
    readOurs.mockResolvedValue(ours(0));
    readNovaLogic.mockRejectedValue(new Error('Network Error'));
    const page = await open();
    expect(page.text()).toContain('No games are being hosted on our NovaWorld right now');
    expect(page.text()).not.toContain('Unable to load lobbies');
    expect(section(page).exists()).toBe(false);
  });

  it("shows NovaLogic's list when ours fails to load", async () => {
    readOurs.mockRejectedValue(new Error('Network Error'));
    readNovaLogic.mockResolvedValue(novalogic());
    const page = await open();
    expect(page.text()).toContain('Unable to load lobbies right now.');
    expect(section(page).findAll('tbody tr')).toHaveLength(2);
  });
});
