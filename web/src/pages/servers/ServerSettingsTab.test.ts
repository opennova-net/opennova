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

  it("shows the listing on NovaLogic's NovaWorld while it is on, or while the service offers it", async () => {
    const label = "Also list on NovaLogic's NovaWorld";
    async function listingRow(server: ReturnType<typeof hostedServer>, scope: 'owner' | 'admin') {
      const router = await testRouter('/me/servers/1/settings');
      const page = mount(ServerSettingsTab, { props: { server, scope, now: 0 }, global: { plugins: [router] } });
      await flushPromises();
      const terms = page.findAll('dt');
      const term = terms.find((dt) => dt.text() === label);
      const value = term ? (term.element.nextElementSibling?.textContent ?? '').trim() : null;
      page.unmount();
      return value;
    }

    // Not offered and off: no row.
    mine.mockResolvedValue({ ok: true, value: serversResponse([], { data_sets: [JO] }) });
    expect(await listingRow(hostedServer(1), 'owner')).toBeNull();
    // Offered: the owner sees it off.
    mine.mockResolvedValue({ ok: true, value: serversResponse([], { data_sets: [JO], novalogic_listing: true }) });
    expect(await listingRow(hostedServer(1), 'owner')).toBe('Off');
    // On: shown to the owner and the admin alike.
    const listed = hostedServer(1);
    listed.settings.list_on_novalogic = true;
    expect(await listingRow(listed, 'owner')).toBe('On');
    expect(await listingRow(listed, 'admin')).toBe('On');
    expect(await listingRow(hostedServer(1), 'admin')).toBeNull();
  });
});
