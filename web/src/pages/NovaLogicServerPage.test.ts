import { flushPromises, mount } from '@vue/test-utils';
import { afterEach, describe, expect, it, vi } from 'vitest';
import NovaLogicServerPage from './NovaLogicServerPage.vue';
import { fetchNovaLogicLobbies } from '../api/lobbies';
import { testRouter } from '../test/router';
import type { NovaLogicLobbies, NovaLogicServer } from '../types/lobby';

vi.mock('../api/lobbies', async (importOriginal) => ({
  ...(await importOriginal<typeof import('../api/lobbies')>()),
  fetchNovaLogicLobbies: vi.fn(),
}));

const read = vi.mocked(fetchNovaLogicLobbies);

function server(overrides: Partial<NovaLogicServer> = {}): NovaLogicServer {
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

function list(overrides: Partial<NovaLogicLobbies> = {}): NovaLogicLobbies {
  return {
    enabled: true,
    servers: [server({ id: 1, serverName: 'Other' }), server()],
    fetchedAt: '2026-10-25T11:57:00Z',
    ageSeconds: 30,
    failing: false,
    ...overrides,
  };
}

describe('NovaLogicServerPage', () => {
  let wrapper: ReturnType<typeof mount> | null = null;
  afterEach(() => {
    wrapper?.unmount();
    wrapper = null;
    read.mockReset();
  });

  async function open(id = 167782351) {
    const router = await testRouter(`/lobby/novalogic/${id}`, ['/lobby/novalogic/:id']);
    wrapper = mount(NovaLogicServerPage, { global: { plugins: [router] } });
    await flushPromises();
    return wrapper;
  }

  it("shows the server's game info in the game's markup, and its players", async () => {
    read.mockResolvedValue(list());
    const page = await open();
    expect(page.get('.eyebrow').text()).toBe("NovaLogic's NovaWorld");
    expect(page.get('h1').text()).toBe('Nice-Evil<-c');
    expect(page.get<HTMLElement>('h1 span span').element.style.color).toBe('rgb(0, 255, 0)');
    expect(page.text()).toContain('Welcome all');
    expect(page.text()).toContain('2 / 32');
    const info = page.get('dl').text();
    expect(info).toContain('AS - Kombang Valley (COOP/AAS)');
    expect(info).toContain('29 min');
    expect(info).toContain('US');
    expect(info).toContain('North America');
    expect(info).toContain('jox01');
    expect(info).not.toContain('Unknown');
    const facts = Object.fromEntries(
      page.findAll('dt').map((dt) => [dt.text(), (dt.element.nextElementSibling as HTMLElement).textContent?.trim()]),
    );
    expect(facts).toMatchObject({ Password: 'Yes', Locked: 'Yes', PunkBuster: 'Yes', Dedicated: 'Yes', Tracers: 'Off' });
    // Names are plain text.
    expect(page.findAll('li').map((li) => li.text())).toEqual(['biggy', '<Ann>']);
    expect(page.text()).toContain("connect your game to NovaLogic's NovaWorld");
    // Player-facing copy: no em dash.
    expect(page.text()).not.toContain(String.fromCharCode(0x2014));
  });

  it('leaves out a time left of NA', async () => {
    read.mockResolvedValue(list({ servers: [server({ timeLeft: 'NA', roster: [] })] }));
    const page = await open();
    expect(page.get('dl').text()).not.toContain('Time left');
    expect(page.text()).toContain('No one is playing.');
  });

  it('says so once the server is no longer listed', async () => {
    read.mockResolvedValue(list({ servers: [server({ id: 1 })] }));
    const page = await open();
    expect(page.text()).toContain('This server is no longer listed.');
    expect(page.find('h1').exists()).toBe(false);
  });

  it('waits for the first list, and says when NovaLogic has not answered', async () => {
    read.mockResolvedValue(list({ servers: [], fetchedAt: null, ageSeconds: null }));
    let page = await open();
    expect(page.text()).toContain("Checking NovaLogic's NovaWorld...");
    expect(page.text()).not.toContain('no longer listed');
    wrapper?.unmount();

    read.mockResolvedValue(list({ servers: [], fetchedAt: null, ageSeconds: null, failing: true }));
    page = await open();
    expect(page.text()).toContain("NovaLogic's NovaWorld can't be reached right now.");
  });

  it('says when it shows the last list it got', async () => {
    read.mockResolvedValue(list({ failing: true }));
    const page = await open();
    expect(page.get('h1').text()).toBe('Nice-Evil<-c');
    expect(page.text()).toContain('this is from the last list we got');
  });

  it('says so when the list cannot be read', async () => {
    read.mockRejectedValue(new Error('Network Error'));
    const page = await open();
    expect(page.get('[role="alert"]').text()).toContain('The server could not be read.');
  });
});
