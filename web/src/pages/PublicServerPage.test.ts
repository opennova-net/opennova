import { flushPromises, mount } from '@vue/test-utils';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import PublicServerPage from './PublicServerPage.vue';
import { fetchLobbyServer, fetchNovaLogicLobbies, LobbyGone } from '../api/lobbies';
import { listMyServers } from '../api/myServers';
import type { SessionUser } from '../api/session';
import { testRouter } from '../test/router';
import type { LobbyServer, NovaLogicLobbies, NovaLogicServer } from '../types/lobby';
import type { MyServer } from '../types/myServers';

const session = vi.hoisted(() => ({ user: null as SessionUser | null }));

vi.mock('../api/lobbies', async (importOriginal) => ({
  ...(await importOriginal<typeof import('../api/lobbies')>()),
  fetchLobbyServer: vi.fn(),
  fetchNovaLogicLobbies: vi.fn(),
}));
vi.mock('../api/myServers', () => ({ listMyServers: vi.fn() }));
vi.mock('../composables/useSession', async () => {
  const { computed } = await import('vue');
  return { useSession: () => ({ user: computed(() => session.user) }) };
});

const readOurs = vi.mocked(fetchLobbyServer);
const readNovaLogic = vi.mocked(fetchNovaLogicLobbies);
const readMine = vi.mocked(listMyServers);

function listed(overrides: Partial<LobbyServer> = {}): LobbyServer {
  return {
    id: 167772173,
    serverName: 'Retail Joint Ops: Escalation',
    hostIp: '203.0.113.7',
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

function nlServer(overrides: Partial<NovaLogicServer> = {}): NovaLogicServer {
  return {
    id: 167782351,
    serverName: '<c00FF00>Nice-Evil<-c',
    mission: 'AS - Kombang Valley',
    gameType: '<c00FF00>COOP/AAS</c>',
    players: 2,
    maxPlayers: 32,
    locked: true,
    password: true,
    message: '<b>Welcome<-b> all',
    country: 'US',
    region: 'North America',
    timeLeft: '29',
    timeOfDay: 'Unknown',
    expansion: 'jox01',
    punkBuster: true,
    dedicated: true,
    tracers: false,
    roster: ['biggy', '<Ann>'],
    ...overrides,
  };
}

function nlList(overrides: Partial<NovaLogicLobbies> = {}): NovaLogicLobbies {
  return {
    enabled: true,
    servers: [nlServer({ id: 1, serverName: 'Other' }), nlServer()],
    fetchedAt: '2026-10-25T11:57:00Z',
    ageSeconds: 30,
    failing: false,
    ...overrides,
  };
}

function mine(rid: number, hostedServerId: number | null): MyServer {
  return { rid, hosted_server_id: hostedServerId } as MyServer;
}

let wrapper: ReturnType<typeof mount> | null = null;

async function open(path: string) {
  const router = await testRouter(path, ['/lobby/novalogic/:id', '/lobby/:id']);
  wrapper = mount(PublicServerPage, { global: { plugins: [router] } });
  await flushPromises();
  return { page: wrapper, router };
}

function facts(page: ReturnType<typeof mount>): Record<string, string> {
  return Object.fromEntries(page.findAll('dt').map((dt) => [dt.text(), (dt.element.nextElementSibling as HTMLElement).textContent?.trim() ?? '']));
}

beforeEach(() => {
  vi.clearAllMocks();
  session.user = null;
  readMine.mockResolvedValue({ ok: true, value: [] });
});
afterEach(() => {
  wrapper?.unmount();
  wrapper = null;
});

describe('PublicServerPage, a server our NovaWorld lists', () => {
  it("shows its game info under a header, with the host's default message and an unknown time of day left out", async () => {
    readOurs.mockResolvedValue(listed());
    const { page } = await open('/lobby/167772173');
    expect(readOurs).toHaveBeenCalledWith(167772173, expect.anything());
    expect(readNovaLogic).not.toHaveBeenCalled();
    expect(page.get('h1').text()).toBe('Retail Joint Ops: Escalation');
    expect(page.get('.page-crumbs a').text()).toBe('All servers');
    expect(page.get('.page-crumbs a').attributes('href')).toBe('/lobby');
    expect(page.get('.page-meta').text()).toContain("OpenNova's NovaWorld");
    expect(page.get('.server-players').text()).toBe('2 / 32 players');
    expect(page.get('#join-server').get('a').attributes('href')).toBe('/play#online');
    expect(facts(page)).toEqual({
      Mission: 'AS - Kombang Valley (AAS)',
      'Time left': '23 min',
      Expansion: 'Escalation',
      Address: '203.0.113.7:32768',
    });
    expect(page.text()).not.toContain('Put your message here');
    expect(page.text()).toContain('Updated just now');
    // No scores: the names who joined.
    expect(page.findAll('[aria-labelledby="server-players"] li').map((li) => li.text())).toEqual(['biggy', 'Ann']);
    // Nobody signed in: no Manage.
    expect(readMine).not.toHaveBeenCalled();
    expect(page.findAll('a').some((a) => a.text() === 'Manage')).toBe(false);
  });

  it("draws the name, message and mission in the game's markup", async () => {
    readOurs.mockResolvedValue(
      listed({ serverName: '<c00FF00>Green</c> Ops', message: '<b>Hi<-b> all', mission: 'AS - <cFF0000>Red</c>', gameType: '<c00FF00>COOP/AAS</c>' }),
    );
    const { page } = await open('/lobby/167772173');
    expect(page.get('h1').text()).toBe('Green Ops');
    expect(page.get<HTMLElement>('h1 span span').element.style.color).toBe('rgb(0, 255, 0)');
    expect(page.get('.server-message').text()).toBe('Hi all');
    expect(facts(page).Mission).toBe('AS - Red (COOP/AAS)');
  });

  it('shows a hosted server its scoreboard, most kills first', async () => {
    readOurs.mockResolvedValue(
      listed({
        scoreboard: [
          { name: 'Ann', team: 1, kills: 5, deaths: 2 },
          { name: 'Bo', team: 2, kills: 9, deaths: 1 },
        ],
      }),
    );
    const { page } = await open('/lobby/167772173');
    expect(page.findAll('tbody tr').map((tr) => tr.findAll('th, td').map((c) => c.text()))).toEqual([
      ['Bo', '2', '9', '1'],
      ['Ann', '1', '5', '2'],
    ]);
  });

  it('says so once the server is no longer listed', async () => {
    readOurs.mockRejectedValue(new LobbyGone());
    const { page } = await open('/lobby/167772173');
    expect(page.get('.server-gone').text()).toBe('This server is no longer listed.');
    expect(page.get('h1').text()).toBe('Server');
  });

  it("gives its signed-in owner a Manage link: a hosted server's page, or their Servers page", async () => {
    session.user = { id: 4, username: 'ranger', role: 'player' } as SessionUser;
    readOurs.mockResolvedValue(listed());
    readMine.mockResolvedValue({ ok: true, value: [mine(5, null), mine(167772173, 42)] });
    let { page } = await open('/lobby/167772173');
    expect(page.findAll('a').find((a) => a.text() === 'Manage')?.attributes('href')).toBe('/me/servers/42');
    wrapper?.unmount();

    readMine.mockResolvedValue({ ok: true, value: [mine(167772173, null)] });
    ({ page } = await open('/lobby/167772173'));
    expect(page.findAll('a').find((a) => a.text() === 'Manage')?.attributes('href')).toBe('/me/servers');
    wrapper?.unmount();

    // Someone else's server.
    readMine.mockResolvedValue({ ok: true, value: [mine(5, 7)] });
    ({ page } = await open('/lobby/167772173'));
    expect(page.findAll('a').some((a) => a.text() === 'Manage')).toBe(false);
  });
});

describe("PublicServerPage, a server on NovaLogic's NovaWorld", () => {
  it("shows its row of NovaLogic's list in the game's markup, and its players", async () => {
    readNovaLogic.mockResolvedValue(nlList());
    const { page } = await open('/lobby/novalogic/167782351');
    expect(readOurs).not.toHaveBeenCalled();
    expect(page.get('h1').text()).toBe('Nice-Evil<-c');
    expect(page.get<HTMLElement>('h1 span span').element.style.color).toBe('rgb(0, 255, 0)');
    const meta = page.get('.page-meta');
    expect(meta.text()).toContain('Joint Operations');
    expect(meta.text()).toContain("NovaLogic's NovaWorld");
    expect(meta.findAll('.badge').map((b) => b.text())).toEqual(['Password', 'PunkBuster', 'Dedicated']);
    expect(page.get('.server-message').text()).toBe('Welcome all');
    expect(page.get('#join-server').text()).toContain('Original NovaWorld');
    expect(page.get('#join-server a').attributes('href')).toBe('/play?game=jointops#online');
    expect(facts(page)).toEqual({
      Mission: 'AS - Kombang Valley (COOP/AAS)',
      'Time left': '29 min',
      Expansion: 'jox01',
      Country: 'US, North America',
      Tracers: 'Off',
    });
    // Names are plain text.
    expect(page.findAll('[aria-labelledby="server-players"] li').map((li) => li.text())).toEqual(['biggy', '<Ann>']);
    expect(page.text()).not.toContain(String.fromCharCode(0x2014));
    // NovaLogic's listing is never an account's own.
    expect(page.findAll('a').some((a) => a.text() === 'Manage')).toBe(false);
  });

  it('leaves out a time left of NA', async () => {
    readNovaLogic.mockResolvedValue(nlList({ servers: [nlServer({ timeLeft: 'NA', roster: [] })] }));
    const { page } = await open('/lobby/novalogic/167782351');
    expect(facts(page)['Time left']).toBeUndefined();
    expect(page.text()).toContain('No one is playing.');
  });

  it('says so once the server is no longer listed', async () => {
    readNovaLogic.mockResolvedValue(nlList({ servers: [nlServer({ id: 1 })] }));
    const { page } = await open('/lobby/novalogic/167782351');
    expect(page.get('.server-gone').text()).toBe('This server is no longer listed.');
    expect(page.find('dl').exists()).toBe(false);
  });

  it('waits for the first list, and says when NovaLogic has not answered', async () => {
    readNovaLogic.mockResolvedValue(nlList({ servers: [], fetchedAt: null, ageSeconds: null }));
    let { page } = await open('/lobby/novalogic/167782351');
    expect(page.get('[role="status"]').text()).toBe("Checking NovaLogic's NovaWorld…");
    expect(page.find('.server-gone').exists()).toBe(false);
    wrapper?.unmount();

    readNovaLogic.mockResolvedValue(nlList({ servers: [], fetchedAt: null, ageSeconds: null, failing: true }));
    ({ page } = await open('/lobby/novalogic/167782351'));
    expect(page.text()).toContain("NovaLogic's NovaWorld can't be reached right now.");
  });

  it('says when it shows the last list it got', async () => {
    readNovaLogic.mockResolvedValue(nlList({ failing: true }));
    const { page } = await open('/lobby/novalogic/167782351');
    expect(page.get('h1').text()).toBe('Nice-Evil<-c');
    expect(page.get('.notice-warn').text()).toContain('this is the last list we got');
  });

  it('says so when the list cannot be read', async () => {
    readNovaLogic.mockRejectedValue(new Error('Network Error'));
    const { page } = await open('/lobby/novalogic/167782351');
    expect(page.get('[role="alert"]').text()).toContain('The server could not be read.');
  });

  it('reads the other source when the page moves between the two', async () => {
    readNovaLogic.mockResolvedValue(nlList());
    readOurs.mockResolvedValue(listed());
    const { page, router } = await open('/lobby/novalogic/167782351');
    expect(page.get('h1').text()).toBe('Nice-Evil<-c');
    await router.push('/lobby/167772173');
    await flushPromises();
    expect(readOurs).toHaveBeenCalledWith(167772173, expect.anything());
    expect(page.get('h1').text()).toBe('Retail Joint Ops: Escalation');
  });
});
