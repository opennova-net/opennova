import * as actionReview from '../../../composables/useConfirm';
import { flushPromises, mount } from '@vue/test-utils';
import { afterEach, beforeEach, describe, expect, it, vi, type MockInstance } from 'vitest';
import MachinesTab from './MachinesTab.vue';
import { fetchHostedFleet, machineAction } from '../../../api/admin';
import { fetchInfrastructure } from '../../../api/infra';
import { fetchSettings, saveSettings } from '../../../api/settings';
import { fleetResponse, fleetServer, machine as fleetMachine } from '../../../test/adminFixtures';
import { cloud, localOverview, overview } from '../../../test/infraFixtures';
import { serviceSettings } from '../../../test/settingsFixtures';
import { NOW } from '../../../test/usageFixtures';
import { testRouter } from '../../../test/router';

vi.mock('../../../api/admin', () => ({ fetchHostedFleet: vi.fn(), machineAction: vi.fn() }));
vi.mock('../../../api/infra', () => ({ fetchInfrastructure: vi.fn() }));
vi.mock('../../../api/settings', () => ({ fetchSettings: vi.fn(), saveSettings: vi.fn() }));

const fleet = vi.mocked(fetchHostedFleet);
const machine = vi.mocked(machineAction);
const infra = vi.mocked(fetchInfrastructure);
const readSettings = vi.mocked(fetchSettings);
const save = vi.mocked(saveSettings);

describe('MachinesTab', () => {
  let wrapper: ReturnType<typeof mount> | null = null;
  let confirm: MockInstance<typeof actionReview.confirmAction>;

  async function mountTab() {
    const router = await testRouter('/admin/servers/machines');
    wrapper = mount(MachinesTab, { global: { plugins: [router] } });
    await flushPromises();
    return wrapper;
  }

  beforeEach(() => {
    vi.clearAllMocks();
    vi.useFakeTimers({ toFake: ['Date'] });
    vi.setSystemTime(NOW);
    confirm = vi.spyOn(actionReview, 'confirmAction').mockResolvedValue(true);
    fleet.mockResolvedValue({ ok: true, value: fleetResponse({ servers: [fleetServer(1), fleetServer(2, { state: 'booting', players: null })] }) });
    infra.mockResolvedValue({ ok: true, value: overview() });
    readSettings.mockResolvedValue({ ok: true, value: serviceSettings() });
  });

  afterEach(() => {
    wrapper?.unmount();
    wrapper = null;
    confirm.mockRestore();
    vi.useRealTimers();
  });

  it("shows each machine as a card whose slots name the servers in them", async () => {
    const page = await mountTab();
    expect(fleet).toHaveBeenCalledWith({ state: 'live' }, expect.anything());
    expect(page.get('[aria-current="page"]').text()).toBe('Machines');
    const cards = page.findAll('article.machine-card');
    expect(cards).toHaveLength(2);
    expect(cards[0].get('h3 a').attributes('href')).toBe('/admin/machines/3');
    const cells = cards[0].findAll('.slot-cell');
    expect(cells[3].text()).toContain('Server 1');
    expect(cells[3].text()).toContain('JO · 5/32');
    expect(cells[3].get('a').attributes('href')).toBe('/admin/servers/1');
    expect(cells[5].text()).toContain('Server 2');
    expect(cells[5].text()).toContain('Starting');
    expect(cards[1].text()).toContain('Dedicated machine');
  });

  it("shows ECS's view of the fleet cluster, each instance to its machine", async () => {
    const page = await mountTab();
    const panel = page.get('.fleet-cluster');
    expect(panel.text()).toContain('Steady');
    expect(panel.text()).toContain('hostd 1/1 · opennova-prod-fleet · opennova-prod-hostd:5');
    const row = panel.get('tr.fleet-cluster-instance');
    expect(row.get('a').attributes('href')).toBe('/admin/machines/1');
    expect(row.text()).toContain('Connected');
    expect(row.text()).toContain('ami-0aaa');
    expect(row.text()).not.toContain('own');
  });

  it("says no machine has joined, ECS unread on the local stack, and a failed read's reason", async () => {
    const read = cloud();
    infra.mockResolvedValue({
      ok: true,
      value: overview({ cloud: { ...read, fleet: { ...read.fleet!, hostd: { ...read.fleet!.hostd!, desired: 0, running: 0 }, instances: [] } } }),
    });
    let page = await mountTab();
    expect(page.get('.fleet-cluster').text()).toContain('No machines');
    expect(page.find('.fleet-cluster table').exists()).toBe(false);
    page.unmount();

    infra.mockResolvedValue({ ok: true, value: localOverview() });
    page = await mountTab();
    expect(page.get('.fleet-cluster').text()).toContain('Not read:');
    page.unmount();

    infra.mockResolvedValue({ ok: false, error: { code: 'db_error', status: 500, message: 'The server hit a database error. Try again shortly.' } });
    page = await mountTab();
    expect(page.get('.fleet-cluster').text()).toContain('The server hit a database error.');
    expect(page.findAll('article.machine-card')).toHaveLength(2);
  });

  it('drains and ends a machine after a confirmation', async () => {
    machine.mockResolvedValue({ ok: true, value: undefined });
    const page = await mountTab();
    const card = page.findAll('article.machine-card')[0];
    await card.findAll('button').find((b) => b.text().startsWith('Drain'))!.trigger('click');
    await flushPromises();
    expect(confirm).toHaveBeenCalledWith('Drain machine 3? Its 2 servers move to other machines one at a time.');
    expect(machine).toHaveBeenCalledWith('3', 'drain');
    expect(page.get('[role="status"]').text()).toBe('Machine 3 draining.');

    confirm.mockResolvedValueOnce(false);
    await card.findAll('button').find((b) => b.text().startsWith('End'))!.trigger('click');
    await flushPromises();
    expect(machine).toHaveBeenCalledTimes(1);
  });

  it('keeps replaced machines behind the history toggle', async () => {
    fleet.mockResolvedValue({
      ok: true,
      value: fleetResponse({ machines: [fleetMachine('5', { state: 'failed', slots_used: 0 }), fleetMachine('6', { state: 'ended', slots_used: 0 }), fleetMachine('7', { state: 'lost' })] }),
    });
    const page = await mountTab();
    expect(page.findAll('article.machine-card')).toHaveLength(1);
    await page.findAll('button').find((b) => b.text() === 'Show history (2)')!.trigger('click');
    const cards = page.findAll('article.machine-card');
    expect(cards).toHaveLength(3);
    expect(cards[0].findAll('button')).toHaveLength(0);
    expect(cards[2].findAll('button').map((b) => b.text())).toEqual(['End machine 7']);
  });

  it('sizes the fleet behind Capacity, the subscriptions switch kept as it was', async () => {
    save.mockResolvedValue({ ok: true, value: serviceSettings({ fleet: { max_shared: 5, shared_slots: 12, max_own: 2, max_addresses: 4 } }) });
    const page = await mountTab();
    const capacity = page.get('details.capacity');
    expect(capacity.get('.capacity-sold').text()).toContain('Sold: 5 of 24 shared places');
    expect((page.get('#capacity-max_shared').element as HTMLInputElement).value).toBe('2');
    expect(page.get('#capacity-max_shared').attributes('title')).toBe('Deployment default 2');
    const saveButton = () => capacity.findAll('button').find((b) => b.text() === 'Save')!;
    expect(saveButton().attributes('disabled')).toBeDefined();

    await page.get('#capacity-shared_slots').setValue(0);
    expect(page.get('#capacity-shared_slots').attributes('aria-invalid')).toBe('true');
    expect(saveButton().attributes('disabled')).toBeDefined();
    await page.get('#capacity-shared_slots').setValue(12);

    await page.get('#capacity-max_shared').setValue(5);
    await capacity.get('form').trigger('submit');
    await flushPromises();
    expect(save).toHaveBeenCalledWith({ subscriptions_enabled: false, max_shared: 5, shared_slots: 12, max_own: 2, max_addresses: 4 });
    expect(page.get('[role="status"]').text()).toBe('Capacity saved.');
  });

  it('keeps static addresses fixed where the deployment runs none', async () => {
    readSettings.mockResolvedValue({ ok: true, value: serviceSettings({ addresses_available: false }) });
    const page = await mountTab();
    expect(page.get('#capacity-max_addresses').attributes('disabled')).toBeDefined();
  });
});
