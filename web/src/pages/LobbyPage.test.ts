import { flushPromises, mount } from '@vue/test-utils';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import LobbyPage from './LobbyPage.vue';
import { fetchLobbies, fetchNovaLogicLobbies } from '../api/lobbies';
import { testRouter } from '../test/router';
import type { LobbyResponse, NovaLogicLobbies, NovaLogicServer } from '../types/lobby';

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

function nlServer(overrides: Partial<NovaLogicServer> = {}): NovaLogicServer {
  return {
    id: 167782351,
    serverName: '<c00FF00>Café © Ops</c>',
    mission: 'AS - Kombang Valley',
    gameType: 'AAS',
    players: 12,
    maxPlayers: 32,
    locked: false,
    password: false,
    message: '',
    country: 'US',
    region: 'North America',
    timeLeft: '29',
    timeOfDay: 'Unknown',
    expansion: '',
    punkBuster: false,
    dedicated: true,
    tracers: true,
    roster: [],
    ...overrides,
  };
}

function novalogic(overrides: Partial<NovaLogicLobbies> = {}): NovaLogicLobbies {
  return {
    enabled: true,
    servers: [
      nlServer(),
      nlServer({
        id: 167782352,
        serverName: 'Bravo',
        mission: '',
        gameType: 'TDM',
        players: 0,
        maxPlayers: 16,
        locked: true,
        password: true,
        punkBuster: true,
        expansion: 'jox01',
      }),
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
    readOurs.mockReset(); readNovaLogic.mockReset();
    readOurs.mockResolvedValue(ours(1)); readNovaLogic.mockResolvedValue(novalogic());
  });
  afterEach(() => { wrapper?.unmount(); wrapper = null; });
  async function open(path = '/lobby') {
    const router = await testRouter(path);
    wrapper = mount(LobbyPage, { global: { plugins: [router] } });
    await flushPromises();
    return { page: wrapper, router };
  }
  const names = (page: ReturnType<typeof mount>) => page.findAll('tbody tr.server-row th a').map((a) => a.text());

  it('combines both sources in one table under a page header, with no source cards', async () => {
    const { page } = await open();
    expect(page.get('h1').text()).toBe('Servers');
    expect(page.find('[aria-label="Listing sources"]').exists()).toBe(false);
    expect(page.find('.lobby-trouble').exists()).toBe(false);
    expect(page.findAll('.server-search')).toHaveLength(1);
    expect(page.findAll('tbody tr.server-row')).toHaveLength(3);
    expect(names(page)[1]).toBe('Our Server 0');
    expect(page.findAll('tbody tr.server-row')[0].text()).toContain('NovaLogic');
    expect(page.findAll('tbody tr.server-row')[1].text()).toContain('OpenNova');
    expect(page.get('.page-meta').text()).toContain('3 servers');
    expect(page.get('.novalogic-age').text()).toBe("NovaLogic's list from 3 min ago");
    expect(page.findAll('.server-mobile > li')).toHaveLength(3);
    expect(page.get('.server-count').text()).toBe('3 of 3 servers');
    expect(page.findAll('tbody a').map((a) => a.attributes('href'))).toEqual(['/lobby/novalogic/167782351', '/lobby/1000', '/lobby/novalogic/167782352']);
    expect(page.find('a[href="/play#online"]').exists()).toBe(true);
  });

  it('names the ONE build of each server NovaWorld hosts, and none for any other', async () => {
    const own = ours(2);
    own.games[0].hosts[0].build = { version: '0.0.22.123', commit: 'a1b2c3d' };
    own.games[0].hosts[1].build = null;
    readOurs.mockResolvedValue(own);
    const { page } = await open();
    const rows = page.findAll('tbody tr.server-row');
    const built = rows.find((r) => r.text().includes('Our Server 0'))!;
    expect(built.get('.server-build').text()).toBe('ONE 0.0.22.123 a1b2c3d');
    expect(built.get('th').text()).toContain('Joint Operations · OpenNova · ONE 0.0.22.123 a1b2c3d');
    // A server its owner hosts, and NovaLogic's, name no build.
    expect(rows.filter((r) => r.find('.server-build').exists())).toHaveLength(1);
    // The phone list says the same.
    expect(page.findAll('.server-mobile .server-build').map((b) => b.text())).toEqual(['ONE 0.0.22.123 a1b2c3d']);
  });

  it('restores URL filters together and changes them without dropping unrelated URL state', async () => {
    const { page, router } = await open('/lobby?source=novalogic&game=jop_2_consumer&type=TDM&hideFull=1&hidePassworded=1&campaign=community');
    expect(names(page)).toEqual([]);
    await page.get('.server-hide-passworded').setValue(false);
    await flushPromises();
    expect(names(page)).toEqual(['Bravo']);
    expect(router.currentRoute.value.query).toMatchObject({ source: 'novalogic', game: 'jop_2_consumer', type: 'TDM', hideFull: '1', campaign: 'community' });
    expect(router.currentRoute.value.query.hidePassworded).toBeUndefined();
    await page.get('.server-clear').trigger('click');
    await flushPromises();
    expect(router.currentRoute.value.query).toEqual({ campaign: 'community' });
    expect(names(page)).toHaveLength(3);
    await page.get('.server-search').setValue('Our Server');
    await flushPromises();
    expect(names(page)).toEqual(['Our Server 0']);
    expect(router.currentRoute.value.query.q).toBe('Our Server');
    await router.push('/lobby?source=novalogic&hideEmpty=1');
    await flushPromises();
    expect(names(page)).toHaveLength(1);
    expect(names(page)[0]).not.toBe('Our Server 0');
  });

  it('preserves a shared game filter across both services and URL sort order', async () => {
    const own = ours(1);
    own.games.push({ slug: 'dfx2', displayName: 'Delta Force Xtreme 2', hosts: [{ id: 2000, serverName: 'DFX2 server', players: 3, maxPlayers: 16 }] });
    readOurs.mockResolvedValue(own);
    const { page, router } = await open('/lobby?game=jop_2_consumer&sort=server&order=asc');
    expect(names(page)).toHaveLength(3);
    expect(names(page)[0]).toBe('Bravo');
    expect(names(page)).not.toContain('DFX2 server');
    const players = page.findAll('thead button').find((button) => button.text().startsWith('Players'))!;
    await players.trigger('click'); await flushPromises();
    expect(router.currentRoute.value.query.sort).toBeUndefined();
    expect(router.currentRoute.value.query.order).toBeUndefined();
    expect(names(page).at(-1)).toBe('Bravo');
  });

  it('does not let either failing source hide the other, and says which failed in one notice', async () => {
    readOurs.mockRejectedValue(new Error('offline'));
    const { page } = await open();
    expect(names(page)).toHaveLength(2);
    expect(page.get('.lobby-trouble').text()).toBe('Our server list could not be read.');
    wrapper?.unmount();
    readOurs.mockResolvedValue(ours(1)); readNovaLogic.mockRejectedValue(new Error('offline'));
    const other = (await open()).page;
    expect(names(other)).toEqual(['Our Server 0']);
    expect(other.get('.lobby-trouble').text()).toBe("NovaLogic's list could not be read.");
  });

  it('keeps the last lists on a refresh failure and says so', async () => {
    const { page } = await open();
    readOurs.mockRejectedValue(new Error('offline')); readNovaLogic.mockRejectedValue(new Error('offline'));
    await page.findAll('button').find((b) => b.text() === 'Refresh')!.trigger('click');
    await flushPromises();
    expect(names(page)).toHaveLength(3);
    expect(page.findAll('.notice')).toHaveLength(1);
    expect(page.get('.lobby-trouble').text()).toBe("Our server list could not be refreshed. NovaLogic's list could not be refreshed.");
  });

  it('does not call a still-loading list empty', async () => {
    let resolve: (value: LobbyResponse) => void = () => {};
    readOurs.mockReturnValue(new Promise<LobbyResponse>((done) => { resolve = done; }));
    readNovaLogic.mockResolvedValue(novalogic({ enabled: false, servers: [], ageSeconds: null, fetchedAt: null }));
    const { page } = await open();
    expect(page.text()).toContain('Checking for servers');
    expect(page.text()).not.toContain('No servers right now.');
    resolve(ours());
    await flushPromises();
    expect(page.text()).toContain('No servers right now.');
  });

  it("says when NovaLogic's NovaWorld is unreachable, and nothing for a source this deployment does not list", async () => {
    readNovaLogic.mockResolvedValue(novalogic({ failing: true, ageSeconds: 7200 }));
    let page = (await open()).page;
    expect(page.get('.novalogic-age').text()).toBe("NovaLogic's list from 2 hours ago");
    expect(page.get('.lobby-trouble').text()).toBe("NovaLogic's NovaWorld can't be reached right now.");
    expect(names(page)).toHaveLength(3);
    wrapper?.unmount();
    readNovaLogic.mockResolvedValue(novalogic({ enabled: false, servers: [], ageSeconds: null, fetchedAt: null }));
    page = (await open()).page;
    expect(page.find('.lobby-trouble').exists()).toBe(false);
    expect(page.find('.novalogic-age').exists()).toBe(false);
    expect(names(page)).toEqual(['Our Server 0']);
    wrapper?.unmount();
    readOurs.mockResolvedValue(ours(0));
    readNovaLogic.mockResolvedValue(novalogic({ servers: [], ageSeconds: null, fetchedAt: null }));
    page = (await open()).page;
    expect(page.text()).toContain("Waiting for NovaLogic's list.");
  });
});
