import { flushPromises, mount } from '@vue/test-utils';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import ServerOverviewTab from './ServerOverviewTab.vue';
import { fetchDataSets } from '../../api/dataSets';
import { fetchMyServers, updateHostedServer } from '../../api/hostedServer';
import { listMyServers } from '../../api/myServers';
import { dataSetsResponse, JO_SET } from '../../test/dataSetFixtures';
import { hostedServer, JO, serversResponse } from '../../test/hostedFixtures';
import { testRouter } from '../../test/router';
import type { HostedServer } from '../../types/hostedServer';

vi.mock('../../api/hostedServer', async (importOriginal) => ({
  ...(await importOriginal<typeof import('../../api/hostedServer')>()),
  fetchMyServers: vi.fn(),
  stopHostedServer: vi.fn(),
  updateHostedServer: vi.fn(),
}));
vi.mock('../../api/myServers', () => ({ listMyServers: vi.fn() }));
vi.mock('../../api/admin', () => ({ stopFleetServer: vi.fn() }));
vi.mock('../../api/dataSets', () => ({ fetchDataSets: vi.fn() }));

const mine = vi.mocked(fetchMyServers);
const listed = vi.mocked(listMyServers);
const update = vi.mocked(updateHostedServer);
const adminSets = vi.mocked(fetchDataSets);

const NOW = Date.parse('2026-10-14T13:00:00Z');

describe('ServerOverviewTab', () => {
  let wrapper: ReturnType<typeof mount> | null = null;

  async function mountTab(server: HostedServer, scope: 'owner' | 'admin' = 'owner') {
    const router = await testRouter(`/me/servers/${server.id}`);
    wrapper = mount(ServerOverviewTab, { props: { server, scope, now: NOW }, global: { plugins: [router] } });
    await flushPromises();
    return wrapper;
  }

  beforeEach(() => {
    vi.clearAllMocks();
    listed.mockResolvedValue({ ok: true, value: [] });
    // The data set holds a newer version than the server runs.
    mine.mockResolvedValue({ ok: true, value: serversResponse([], { data_sets: [{ ...JO, version: '1.7.5.9' }] }) });
    adminSets.mockResolvedValue({
      ok: true,
      value: dataSetsResponse({ data_sets: [{ ...JO_SET, current_tree: { id: 15, version: '1.7.5.9', ready_at: null } }] }),
    });
  });

  afterEach(() => {
    wrapper?.unmount();
    wrapper = null;
  });

  it('shows the data the server runs on', async () => {
    const page = await mountTab(
      hostedServer(1, {
        expansion: { id: 2, code: 'jox01', display_name: 'Escalation' },
        data: { base: { tree_id: 10, version: '1.7.5.5' }, expansion: { tree_id: 12, version: '1.7.5.5' } },
      }),
    );
    const section = page.get('#hosted-server-1-data').element.closest('section')!;
    expect(section.textContent).toContain('Joint Operations');
    expect(section.textContent).toContain('1.7.5.5');
    expect(section.textContent).not.toContain('version');
    expect(section.textContent).toContain('Escalation');
    expect(page.find('#hosted-server-update').exists()).toBe(false);
    expect(mine).not.toHaveBeenCalled();
  });

  it('says No version for data that names none, and offers Start again once the server ended', async () => {
    const page = await mountTab(
      hostedServer(1, { state: 'stopped', live: false, data: { base: { tree_id: 10, version: '' }, expansion: null } }),
    );
    const section = page.get('#hosted-server-1-data').element.closest('section')!;
    expect(section.textContent).toContain('No version');
    const again = page.findAll('a').find((a) => a.text() === 'Start again');
    expect(again?.attributes('href')).toBe('/me/servers/new?from=1');
  });

  it("names the newer version and updates the server behind a warning, on the owner's page", async () => {
    update.mockResolvedValue({ ok: true, value: hostedServer(1, { update_available: true, update_requested: true }) });
    const page = await mountTab(hostedServer(1, { update_available: true }));
    expect(mine).toHaveBeenCalledTimes(1);
    const callout = page.get('#hosted-server-update');
    expect(callout.text()).toContain('Joint Operations 1.7.5.9 is available.');
    expect(callout.text()).toContain('Your server keeps the data it started on until you update it.');

    await callout.get('button').trigger('click');
    expect(page.get('#hosted-server-update').text()).toContain('Updating restarts the server and drops its players.');
    expect(update).not.toHaveBeenCalled();
    await page.get('#hosted-server-update form').trigger('submit');
    await flushPromises();
    expect(update).toHaveBeenCalledWith(1);
    expect(page.get('[role="status"]').text()).toBe('Update requested: Server 1 restarts on the current data shortly.');
    expect(page.emitted('changed')?.[0]).toEqual([hostedServer(1, { update_available: true, update_requested: true })]);
  });

  it('says an update was requested, with no second button', async () => {
    const page = await mountTab(hostedServer(1, { update_available: true, update_requested: true }));
    expect(page.text()).toContain('Update requested.');
    expect(page.find('#hosted-server-update').exists()).toBe(false);
  });

  it("shows the owner the refusal, and tells the page when the server is gone", async () => {
    update.mockResolvedValue({ ok: false, error: { code: 'up_to_date', status: 409, message: 'Your server already runs the current data.' } });
    const page = await mountTab(hostedServer(1, { update_available: true }));
    await page.get('#hosted-server-update button').trigger('click');
    await page.get('#hosted-server-update form').trigger('submit');
    await flushPromises();
    expect(page.get('[role="alert"]').text()).toBe('Your server already runs the current data.');
    expect(page.emitted('gone')).toHaveLength(1);
  });

  it("reads the admin's data sets for the version, and offers the admin no update here", async () => {
    const page = await mountTab(hostedServer(1, { update_available: true }), 'admin');
    expect(mine).not.toHaveBeenCalled();
    expect(adminSets).toHaveBeenCalledTimes(1);
    const callout = page.get('#hosted-server-update');
    expect(callout.text()).toContain('Joint Operations 1.7.5.9 is available.');
    expect(callout.text()).toContain('an admin updates every outdated server');
    expect(callout.find('button').exists()).toBe(false);
  });
});
