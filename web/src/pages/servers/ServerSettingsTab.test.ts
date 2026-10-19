import { flushPromises, mount } from '@vue/test-utils';
import { describe, expect, it, vi } from 'vitest';
import ServerSettingsTab from './ServerSettingsTab.vue';
import { fetchMyServers } from '../../api/hostedServer';
import { hostedServer, JO, serversResponse } from '../../test/hostedFixtures';
import { testRouter } from '../../test/router';

vi.mock('../../api/hostedServer', async (importOriginal) => ({
  ...(await importOriginal<typeof import('../../api/hostedServer')>()),
  fetchMyServers: vi.fn(),
}));

const mine = vi.mocked(fetchMyServers);

describe('ServerSettingsTab', () => {
  it("lists the rotation by its missions' titles, the file beside each (the file alone when untitled)", async () => {
    mine.mockResolvedValue({ ok: true, value: serversResponse([], { data_sets: [JO] }) });
    const server = hostedServer(1);
    server.settings.missions = ['JO_ONE.bms', 'jo_two.bms'];
    const router = await testRouter('/me/servers/1/settings');
    const page = mount(ServerSettingsTab, { props: { server, scope: 'owner', now: 0 }, global: { plugins: [router] } });
    await flushPromises();
    const rows = page.findAll('ol li').map((li) => li.text().replace(/\s+/g, ' '));
    expect(rows).toEqual(['River Patrol JO_ONE.bms', 'jo_two.bms']);
    expect(page.text()).not.toContain('Static address');
    page.unmount();
  });

  it('shows the static address the server runs at', async () => {
    mine.mockResolvedValue({ ok: true, value: serversResponse([], { data_sets: [JO] }) });
    const server = hostedServer(1, {
      own_machine: true,
      address: { id: 5, name: 'smoke', hostname: 'smoke.play.opennova.net', public_ip: '203.0.113.9', state: 'ready' },
    });
    const router = await testRouter('/me/servers/1/settings');
    const page = mount(ServerSettingsTab, { props: { server, scope: 'owner', now: 0 }, global: { plugins: [router] } });
    await flushPromises();
    const text = page.get('dl').text().replace(/\s+/g, ' ');
    expect(text).toContain('Static address');
    expect(text).toContain('smoke.play.opennova.net 203.0.113.9:32768');
    page.unmount();
  });
});
