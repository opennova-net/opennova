import * as actionReview from '../../composables/useConfirm';
import { flushPromises, mount } from '@vue/test-utils';
import { afterEach, beforeEach, describe, expect, it, vi, type MockInstance } from 'vitest';
import ServerOverviewTab from './ServerOverviewTab.vue';
import { fetchProducts } from '../../api/products';
import { fetchMyServers, updateHostedServer } from '../../api/hostedServer';
import { productsResponse, JO_PRODUCT } from '../../test/productFixtures';
import { hostedServer, JO, serversResponse, settings } from '../../test/hostedFixtures';
import { testRouter } from '../../test/router';
import type { HostedServer } from '../../types/hostedServer';

vi.mock('../../api/hostedServer', async (importOriginal) => ({
  ...(await importOriginal<typeof import('../../api/hostedServer')>()),
  fetchMyServers: vi.fn(),
  updateHostedServer: vi.fn(),
}));
vi.mock('../../api/products', () => ({ fetchProducts: vi.fn() }));

const mine = vi.mocked(fetchMyServers);
const update = vi.mocked(updateHostedServer);
const adminProducts = vi.mocked(fetchProducts);

const NOW = Date.parse('2026-10-14T13:00:00Z');

describe('ServerOverviewTab', () => {
  let wrapper: ReturnType<typeof mount> | null = null;
  let confirm: MockInstance<typeof actionReview.confirmAction>;

  async function mountTab(server: HostedServer, scope: 'owner' | 'admin' = 'owner') {
    const router = await testRouter(`/me/servers/${server.id}`);
    wrapper = mount(ServerOverviewTab, { props: { server, scope, now: NOW }, global: { plugins: [router] } });
    await flushPromises();
    return wrapper;
  }

  beforeEach(() => {
    vi.clearAllMocks();
    confirm = vi.spyOn(actionReview, 'confirmAction').mockResolvedValue(true);
    // The game holds a newer version than the server runs.
    mine.mockResolvedValue({ ok: true, value: serversResponse([], { products: [{ ...JO, version: '1.0.9' }] }) });
    adminProducts.mockResolvedValue({
      ok: true,
      value: productsResponse({ products: [{ ...JO_PRODUCT, current_tree: { id: 15, version: '1.0.9', ready_at: null } }] }),
    });
  });

  afterEach(() => {
    wrapper?.unmount();
    wrapper = null;
    confirm.mockRestore();
  });

  it('shows the data the server runs on', async () => {
    const page = await mountTab(
      hostedServer(1, {
        expansion: { id: 2, code: 'jox01', display_name: 'Escalation' },
        data: { base: { tree_id: 10, version: '1.0.5' }, expansion: { tree_id: 12, version: '' } },
      }),
    );
    const section = page.get('#hosted-server-1-data').element.closest('section')!;
    expect(section.textContent).toContain('Joint Operations');
    expect(section.textContent).toContain('1.0.5');
    expect(section.textContent).toContain('Escalation');
    expect(section.textContent).toContain('No version');
    // The protocol its game's title speaks.
    expect(page.get('#hosted-server-protocol').text()).toBe('JO protocol 16');
    expect(page.find('#hosted-server-update').exists()).toBe(false);
  });

  it('folds in the settings it was started with, its rotation by mission name and its password as set or not', async () => {
    const page = await mountTab(
      hostedServer(1, { settings: settings({ game_name: 'Server 1', game_password: 'hunter2', kill_limit: 500, missions: ['jo_one.bms', 'jo_two.bms'] }) }),
    );
    const dl = page.get('.server-settings').text();
    expect(dl).toContain('Max players32');
    expect(dl).toContain('Game passwordSet');
    expect(dl).not.toContain('hunter2');
    expect(dl).toContain('Kill limitNo limit');
    expect(page.findAll('.server-rotation li').map((li) => li.text())).toEqual(['River Patrol', 'jo_two.bms']);
  });

  it("names the newer version and updates the owner's server after a confirmation", async () => {
    update.mockResolvedValue({ ok: true, value: hostedServer(1, { update_available: true, update_requested: true }) });
    const page = await mountTab(hostedServer(1, { update_available: true }));
    expect(mine).toHaveBeenCalledTimes(1);
    const callout = page.get('#hosted-server-update');
    expect(callout.text()).toContain('Joint Operations 1.0.9 available');
    await callout.get('button').trigger('click');
    await flushPromises();
    expect(confirm).toHaveBeenCalledWith('Update Server 1? It restarts and its players drop.');
    expect(update).toHaveBeenCalledWith(1);
    expect(page.get('[role="status"]').text()).toBe('Server 1 is updating.');
    expect(page.emitted('changed')?.[0]).toEqual([hostedServer(1, { update_available: true, update_requested: true })]);
  });

  it('says an update was requested, with no button', async () => {
    const page = await mountTab(hostedServer(1, { update_available: true, update_requested: true }));
    expect(page.text()).toContain('Update requested');
    expect(page.find('#hosted-server-update').exists()).toBe(false);
  });

  it('shows the owner the refusal, and tells the page when the server is gone', async () => {
    update.mockResolvedValue({ ok: false, error: { code: 'up_to_date', status: 409, message: 'Your server already runs the current data.' } });
    const page = await mountTab(hostedServer(1, { update_available: true }));
    await page.get('#hosted-server-update button').trigger('click');
    await flushPromises();
    expect(page.get('[role="alert"]').text()).toBe('Your server already runs the current data.');
    expect(page.emitted('gone')).toHaveLength(1);
  });

  it("reads the admin's games for the version, and offers the admin no update here", async () => {
    const page = await mountTab(hostedServer(1, { update_available: true }), 'admin');
    expect(mine).not.toHaveBeenCalled();
    expect(adminProducts).toHaveBeenCalledTimes(1);
    const callout = page.get('#hosted-server-update');
    expect(callout.text()).toContain('Joint Operations 1.0.9 available');
    expect(callout.find('button').exists()).toBe(false);
    // The admin's rotation lists the files.
    expect(page.findAll('.server-rotation li').map((li) => li.text())).toEqual(['jo_one.bms']);
  });

  it('links its listing in the server browser, with where joiners go', async () => {
    const page = await mountTab(
      hostedServer(1, {
        listing: { rid: 77, server_name: 'Server 1', mission_name: 'River Patrol', game_type: 'TDM', players: 4, max_players: 32, host_ip: '203.0.113.7', host_port: 32771 },
      }),
    );
    const listing = page.get('.server-listing');
    expect(listing.get('a').attributes('href')).toBe('/lobby/77');
    expect(listing.text()).toContain('203.0.113.7:32771');
    page.unmount();
    const unlisted = await mountTab(hostedServer(1));
    expect(unlisted.get('.server-listing').text()).toContain('Not listed');
  });

  it('says where the start stands under its progress, and when it is slower than usual', async () => {
    // The data's been downloading since 12:00 (an hour before NOW): far past its usual few minutes.
    const page = await mountTab(
      hostedServer(1, {
        state: 'booting',
        updated_at: '2026-10-14 12:00:00',
        phase: { name: 'downloading_data', bytes_done: 1_200_000_000, bytes_total: 2_300_000_000 },
      }),
    );
    expect(page.get('.server-phase').text()).toBe("Downloading the game's release: 1.2 of 2.3 GB.");
    expect(page.get('.server-phase-slow').text()).toBe('This is taking longer than usual.');
  });

  it("says the game's release could not be fetched, the machine's reason on an admin's page alone", async () => {
    const waiting = hostedServer(1, {
      state: 'provisioning',
      phase: { name: 'data_unavailable', line: 'its data could not be fetched: aws s3 sync exited 1: AccessDenied' },
    });
    let page = await mountTab(waiting, 'admin');
    expect(page.get('.server-phase').text()).toBe("The game's release could not be fetched; the service is retrying.");
    expect(page.get('.server-phase-line').text()).toContain('aws s3 sync exited 1: AccessDenied');
    wrapper?.unmount();
    page = await mountTab(waiting);
    expect(page.find('.server-phase-line').exists()).toBe(false);
  });

  it('counts its restarts', async () => {
    const page = await mountTab(hostedServer(1, { state: 'running', restarts: 2 }));
    expect(page.get('.server-restarts').text()).toBe('Restarted 2 times');
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
    expect(why.text()).toContain('It crashed (exit code 3).');
    expect(why.text()).toContain('fatal: mission jo_two.bms is missing');
    expect(why.findAll('p')[0].classes()).toContain('text-danger');
    expect(page.text()).toContain('it crashed 3 times in an hour');
    expect(page.find('.server-phase').exists()).toBe(false);
    // An ended server has no listing section.
    expect(page.find('.server-listing').exists()).toBe(false);
  });

  it('says who stopped it, to its owner and to an admin', async () => {
    const stopped = hostedServer(1, { state: 'stopped', stopped_because: { cause: 'owner_stop', limit: null, exit_code: 0, oom: false, line: '' } });
    let page = await mountTab(stopped);
    expect(page.get('.server-stopped-because').text()).toContain('You stopped it.');
    wrapper?.unmount();
    page = await mountTab(stopped, 'admin');
    expect(page.get('.server-stopped-because').text()).toContain('Its owner stopped it.');
  });

  it("says where its listing on NovaLogic's NovaWorld stands, for an owner who asked", async () => {
    const asked = settings({ game_name: 'Server 1', list_on_novalogic: true });
    const refused = { state: 'not_listed', reason: 'the service did not accept the server name', tag: 'NWEC57', player_slots: [3, 4] };
    let page = await mountTab(hostedServer(1, { settings: asked }));
    expect(page.find('.server-novalogic').exists()).toBe(false);
    wrapper?.unmount();
    page = await mountTab(hostedServer(1, { settings: asked, novalogic: refused }));
    const line = page.get('.server-novalogic');
    expect(line.text()).toContain('Not listed for this run: NovaLogic did not accept the server name.');
    expect(line.findAll('p')[0].classes()).toContain('text-danger');
    expect(line.get('.server-novalogic-players').text()).toBe('2 players through NovaLogic');
    // The NovaWorld error tag is the admin's alone.
    expect(line.text()).not.toContain('NWEC57');
    wrapper?.unmount();
    page = await mountTab(hostedServer(1, { settings: asked, novalogic: refused }), 'admin');
    expect(page.get('.server-novalogic').text()).toContain('NWEC57');
    wrapper?.unmount();
    // Not asked: no line, whatever the reply carries.
    page = await mountTab(hostedServer(1, { novalogic: { state: 'listed', player_slots: [] } }));
    expect(page.find('.server-novalogic').exists()).toBe(false);
  });
});
