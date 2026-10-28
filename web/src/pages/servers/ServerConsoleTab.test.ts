import { flushPromises, mount } from '@vue/test-utils';
import { beforeEach, describe, expect, it, vi } from 'vitest';
import ServerConsoleTab from './ServerConsoleTab.vue';
import { listMyServers } from '../../api/myServers';
import { hostedServer, listedServer } from '../../test/hostedFixtures';

vi.mock('../../api/myServers', async (importOriginal) => ({
  ...(await importOriginal<typeof import('../../api/myServers')>()),
  listMyServers: vi.fn(),
}));

const list = vi.mocked(listMyServers);

// A running hosted server, listed as rid 30.
function running() {
  return hostedServer(7, {
    state: 'running',
    listing: { rid: 30, server_name: 'Night Raid', mission_name: 'River Patrol', game_type: 'TDM', players: 2, max_players: 24, host_ip: '203.0.113.7', host_port: 32769 },
  });
}

function mountTab(scope: 'owner' | 'admin', server = running()) {
  return mount(ServerConsoleTab, {
    props: { server, scope, now: Date.now() },
    global: { stubs: { HostedServerConsole: true } },
  });
}

describe('ServerConsoleTab', () => {
  beforeEach(() => {
    vi.clearAllMocks();
    list.mockResolvedValue({ ok: true, value: [listedServer(30, { hosted_server_id: 7, server_name: 'Night Raid', msg: 'Welcome' })] });
  });

  it("gives the owner of a running, listed server its name, message, mission and weather beside the console", async () => {
    const page = mountTab('owner');
    await flushPromises();
    expect(page.find('.server-commands').exists()).toBe(true);
    expect(page.findAll('.server-commands h2').map((h) => h.text())).toEqual(['Name and message', 'Mission', 'Weather and time']);
    expect((page.get('#console-commands-name').element as HTMLInputElement).value).toBe('Night Raid');
  });

  it('gives an admin, or a server not running, the console alone', async () => {
    const admin = mountTab('admin');
    await flushPromises();
    expect(admin.find('.server-commands').exists()).toBe(false);
    const starting = mountTab('owner', hostedServer(7, { state: 'booting' }));
    await flushPromises();
    expect(starting.find('.server-commands').exists()).toBe(false);
    expect(list).not.toHaveBeenCalled();
  });
});
