import * as actionReview from '../../../composables/useConfirm';
import { flushPromises, mount } from '@vue/test-utils';
import { afterEach, beforeEach, describe, expect, it, vi, type MockInstance } from 'vitest';
import MachinePage from './MachinePage.vue';
import { fetchHostedFleet, fetchMachine, machineAction } from '../../../api/admin';
import { fetchLogs } from '../../../api/logs';
import { fetchProducts } from '../../../api/products';
import { fetchUsage } from '../../../api/usage';
import { fleetResponse, fleetServer, machine, machineDetail } from '../../../test/adminFixtures';
import { productsResponse } from '../../../test/productFixtures';
import { NOW, logPage, machineUsage } from '../../../test/usageFixtures';
import { testRouter } from '../../../test/router';

vi.mock('../../../api/admin', () => ({ fetchHostedFleet: vi.fn(), fetchMachine: vi.fn(), machineAction: vi.fn() }));
vi.mock('../../../api/products', () => ({ fetchProducts: vi.fn() }));
vi.mock('../../../api/usage', () => ({ fetchUsage: vi.fn() }));
vi.mock('../../../api/logs', () => ({ fetchLogs: vi.fn() }));

const read = vi.mocked(fetchMachine);
const fleet = vi.mocked(fetchHostedFleet);
const action = vi.mocked(machineAction);
const products = vi.mocked(fetchProducts);
const usage = vi.mocked(fetchUsage);
const logs = vi.mocked(fetchLogs);

describe('MachinePage', () => {
  let wrapper: ReturnType<typeof mount> | null = null;
  let confirm: MockInstance<typeof actionReview.confirmAction>;

  async function mountPage(path = '/admin/machines/3') {
    const router = await testRouter(path, ['/admin/machines/:id']);
    wrapper = mount(MachinePage, { global: { plugins: [router] } });
    await flushPromises();
    return wrapper;
  }

  beforeEach(() => {
    vi.clearAllMocks();
    vi.useFakeTimers({ toFake: ['Date'] });
    vi.setSystemTime(NOW);
    confirm = vi.spyOn(actionReview, 'confirmAction').mockResolvedValue(true);
    read.mockResolvedValue({ ok: true, value: machineDetail('3') });
    // Server 1 in slot 3 is owner1's JO server; server 2 is not in the fleet's rows.
    fleet.mockResolvedValue({ ok: true, value: fleetResponse({ servers: [fleetServer(1)] }) });
    products.mockResolvedValue({ ok: true, value: productsResponse() });
    usage.mockResolvedValue({ ok: true, value: machineUsage() });
    logs.mockResolvedValue({ ok: true, value: logPage({ subject: 'machine', id: '3', generation: null, group: '/opennova/prod/machines', stream: 'hostd/hostd/task3' }) });
  });

  afterEach(() => {
    wrapper?.unmount();
    wrapper = null;
    confirm.mockRestore();
    vi.useRealTimers();
  });

  it('heads the page with the machine, its crumbs and its facts in a line', async () => {
    const page = await mountPage();
    expect(read).toHaveBeenCalledWith('3', expect.anything());
    expect(page.get('h1').text()).toBe('Machine 3');
    expect(page.findAll('.page-crumbs a').map((a) => a.attributes('href'))).toEqual(['/admin/servers', '/admin/servers/machines']);
    const meta = page.get('.page-meta').text();
    expect(meta).toContain('Ready');
    expect(meta).toContain('Shared machine');
    expect(meta).toContain('t3a.large');
    expect(page.get('.machine-report').text()).toBe('Last report 4 s ago');
    const tiles = page.get('.machine-tiles').text();
    expect(tiles).toContain('2/12');
    expect(tiles).toContain('22 %');
    expect(tiles).toContain('13 GB');
    // Sent this month once, in its tile.
    expect(page.text().match(/Sent this month/g)).toHaveLength(1);
  });

  it("names its facts against the release's", async () => {
    let page = await mountPage();
    const facts = page.get('.machine-facts').text();
    expect(facts).toContain('opennova-hostd@sha256:111111111111');
    expect(facts).toContain("Release's");
    expect(facts).toContain('0.0.20, agent protocol 3');
    expect(facts).toContain('hostd/hostd/task3');
    expect(page.findAll('.machine-flag')).toHaveLength(0);
    page.unmount();

    read.mockResolvedValue({ ok: true, value: machineDetail('3', { machine: machine('3', { image_id: 'ami-0old', outdated: true }) }) });
    page = await mountPage();
    expect(page.get('.machine-facts').text()).toContain('Not the release');
    expect(page.get('.machine-flag').text()).toBe('Older than the release: machine image ami-0old');
  });

  it('lists its slots as a table, each server with its game, players and owner', async () => {
    const page = await mountPage();
    expect(fleet).toHaveBeenCalledWith({ state: 'live' }, expect.anything());
    const rows = page.findAll('tr.machine-server');
    expect(rows).toHaveLength(2);
    expect(rows[0].get('th a').text()).toBe('Ranked TDM');
    expect(rows[0].get('th a').attributes('href')).toBe('/admin/servers/1');
    expect(rows[0].text()).toContain('Joint Operations');
    expect(rows[0].text()).toContain('5/32');
    expect(rows[0].findAll('a').map((a) => a.attributes('href'))).toContain('/admin/players/101');
    expect(rows[0].text()).toContain('Running');
    expect(rows[0].findAll('td').at(-1)!.text()).toBe('3');
    expect(rows[1].get('th a').text()).toBe('Server 2');
    expect(rows[1].text()).toContain('Booting');
  });

  it('names the games it holds where it can, with their progress, and its timeline', async () => {
    const page = await mountPage();
    const trees = page.findAll('tr.machine-tree');
    expect(trees).toHaveLength(2);
    // Tree 10 is server 1's pinned base; tree 12 matches nothing.
    expect(trees[0].get('th').text()).toBe('Joint Operations 1.0.5');
    expect(trees[0].text()).toContain('Ready, 1.2 GB');
    expect(trees[1].get('th').text()).toBe('Tree 12');
    expect(trees[1].text()).toContain('Fetching: 300 MB of 600 MB (50 %), try 2');
    expect(trees[1].get('[role="progressbar"]').attributes('aria-valuenow')).toBe('50');
    const events = page.findAll('li.machine-event').map((e) => e.text());
    expect(events).toHaveLength(4);
    expect(events[3]).toContain('Booting to ready: first report');
  });

  it("shows its usage with its disk, and hostd's log", async () => {
    const page = await mountPage();
    expect(usage).toHaveBeenCalledWith('machine', '3', '48h', { task: undefined, signal: expect.anything() });
    expect(page.get('.usage-now').text()).toContain('Machine 3: 22 % CPU');
    expect(page.text()).toContain('Disk free');
    expect(logs).toHaveBeenCalledWith({ subject: 'machine', id: '3', limit: 200 }, expect.anything());
    expect(page.get('.log-source').text()).toBe('CloudWatch /opennova/prod/machines, stream hostd/hostd/task3');
  });

  it('drains and ends it after a confirmation', async () => {
    action.mockResolvedValue({ ok: true, value: undefined });
    const page = await mountPage();
    await page.findAll('button').find((b) => b.text() === 'Drain…')!.trigger('click');
    await flushPromises();
    expect(confirm).toHaveBeenCalledWith('Drain machine 3? Its 2 servers move to other machines one at a time.');
    expect(action).toHaveBeenCalledWith('3', 'drain');
    expect(page.get('[role="status"]').text()).toBe('Machine 3 draining.');
    confirm.mockResolvedValueOnce(false);
    await page.findAll('button').find((b) => b.text() === 'End…')!.trigger('click');
    await flushPromises();
    expect(action).toHaveBeenCalledTimes(1);
  });

  it('says so when no machine has the id', async () => {
    read.mockResolvedValue({ ok: false, error: { code: 'not_found', status: 404, message: 'It no longer exists. Refresh the list.' } });
    const page = await mountPage('/admin/machines/99');
    expect(page.get('h1').text()).toBe('Machine 99');
    expect(page.get('[role="alert"]').text()).toBe('No machine has this id.');
    expect(page.find('.machine-tiles').exists()).toBe(false);
  });
});
