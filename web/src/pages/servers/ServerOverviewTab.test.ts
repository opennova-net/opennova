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

  it("says where the start stands under its progress, and when it is slower than usual", async () => {
    // The data's been downloading since 12:00 (an hour before NOW): far past its usual few minutes.
    const page = await mountTab(
      hostedServer(1, {
        state: 'booting',
        updated_at: '2026-10-14 12:00:00',
        phase: { name: 'downloading_data', bytes_done: 1_200_000_000, bytes_total: 2_300_000_000 },
      }),
    );
    expect(page.get('.server-phase').text()).toBe('Downloading game data: 1.2 of 2.3 GB.');
    expect(page.get('.server-phase-slow').text()).toBe('This is taking longer than usual.');
    expect(page.find('.launches-held').exists()).toBe(false);
  });

  it('shows the fleet notice while launches are held, and nothing slow about the hold', async () => {
    const page = await mountTab(
      hostedServer(1, { state: 'provisioning', updated_at: '2026-10-14 12:00:00', phase: { name: 'launches_held' } }),
    );
    expect(page.get('.server-phase').text()).toBe('Waiting for a machine: new machines are held for a moment.');
    expect(page.get('.launches-held').text()).toContain('A server waiting for a machine starts as soon as launches resume');
    expect(page.find('.server-phase-slow').exists()).toBe(false);
  });

  it("says the game data could not be fetched, the machine's reason on an admin's page alone", async () => {
    const waiting = hostedServer(1, {
      state: 'provisioning',
      phase: { name: 'data_unavailable', line: 'its data could not be fetched: aws s3 sync exited 1: AccessDenied' },
    });
    let page = await mountTab(waiting, 'admin');
    expect(page.get('.server-phase').text()).toBe('The game data could not be fetched; the service is retrying.');
    expect(page.get('.server-phase-line').text()).toContain('aws s3 sync exited 1: AccessDenied');
    wrapper?.unmount();
    page = await mountTab(waiting);
    expect(page.get('.server-phase').text()).toBe('The game data could not be fetched; the service is retrying.');
    expect(page.find('.server-phase-line').exists()).toBe(false);
  });

  it('counts its restarts without calling a start that timed out a crash', async () => {
    const page = await mountTab(hostedServer(1, { state: 'running', restarts: 2 }));
    expect(page.get('.server-restarts').text()).toBe(
      'It was restarted 2 times after a crash or a start that did not come up in time.',
    );
    expect(page.text()).not.toContain('crashed and was restarted');
  });

  it("says why it stopped, with the server's own last line", async () => {
    const page = await mountTab(
      hostedServer(1, {
        state: 'failed',
        failure_reason: 'it crashed 3 times in an hour',
        stopped_because: { cause: 'crash', limit: null, exit_code: 3, oom: false, line: 'fatal: mission jo_two.bms is missing' },
      }),
    );
    const why = page.get('.server-stopped-because');
    expect(why.text()).toContain('Why it stopped');
    expect(why.text()).toContain('It crashed (exit code 3).');
    expect(why.text()).toContain('fatal: mission jo_two.bms is missing');
    expect(why.findAll('p')[1].classes()).toContain('text-danger');
    expect(page.find('.server-phase').exists()).toBe(false);
  });

  it('says who stopped it, to its owner and to an admin', async () => {
    const stopped = hostedServer(1, { state: 'stopped', stopped_because: { cause: 'owner_stop', limit: null, exit_code: 0, oom: false, line: '' } });
    let page = await mountTab(stopped);
    expect(page.get('.server-stopped-because').text()).toContain('You stopped it.');
    wrapper?.unmount();
    page = await mountTab(stopped, 'admin');
    expect(page.get('.server-stopped-because').text()).toContain('Its owner stopped it.');
    wrapper?.unmount();
    page = await mountTab(
      hostedServer(1, { state: 'stopped', stopped_because: { cause: 'admin_stop', limit: null, exit_code: 0, oom: false, line: '' } }),
    );
    expect(page.get('.server-stopped-because').text()).toContain('An admin stopped it.');
  });

  it('says which hosting limit stopped it', async () => {
    const page = await mountTab(
      hostedServer(1, {
        state: 'stopped',
        stopped_because: { cause: 'limits', limit: { kind: 'no_shared_server', count: 0 }, exit_code: null, oom: false, line: '' },
      }),
    );
    const why = page.get('.server-stopped-because');
    expect(why.text()).toContain('Your dedicated machines went down to 0 and you had no shared server free for it, so it was stopped.');
    expect(why.findAll('p')[1].classes()).not.toContain('text-danger');
  });
});
