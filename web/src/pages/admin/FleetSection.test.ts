import { flushPromises, mount } from '@vue/test-utils';
import { afterEach, beforeEach, describe, expect, it, vi, type MockInstance } from 'vitest';
import FleetSection from './FleetSection.vue';
import { fetchHostedFleet, fetchRollPlan, machineAction, stopAllFleetServers, stopFleetServer } from '../../api/admin';
import { updateAllServers } from '../../api/dataSets';
import { fleetResponse, fleetServer, machine as fleetMachine } from '../../test/adminFixtures';
import { NOW } from '../../test/usageFixtures';
import { testRouter } from '../../test/router';

vi.mock('../../api/admin', () => ({
  fetchHostedFleet: vi.fn(),
  fetchRollPlan: vi.fn(),
  machineAction: vi.fn(),
  rollFleet: vi.fn(),
  stopAllFleetServers: vi.fn(),
  stopFleetServer: vi.fn(),
}));
vi.mock('../../api/dataSets', () => ({ updateAllServers: vi.fn() }));
vi.mock('../../api/attention', async (importOriginal) => ({
  ...(await importOriginal<typeof import('../../api/attention')>()),
  fetchAttention: vi.fn(async () => ({ ok: true, value: { findings: [], alarms: [], alarms_read: true, checked_at: '' } })),
}));

const fleet = vi.mocked(fetchHostedFleet);
const machine = vi.mocked(machineAction);
const rollPlan = vi.mocked(fetchRollPlan);
const stopAll = vi.mocked(stopAllFleetServers);
const stopOne = vi.mocked(stopFleetServer);
const updateAll = vi.mocked(updateAllServers);

describe('FleetSection', () => {
  let wrapper: ReturnType<typeof mount> | null = null;
  let confirm: MockInstance<typeof window.confirm>;

  async function mountSection(path = '/admin/fleet') {
    const router = await testRouter(path);
    wrapper = mount(FleetSection, { global: { plugins: [router] } });
    await flushPromises();
    return wrapper;
  }

  beforeEach(() => {
    vi.clearAllMocks();
    vi.useFakeTimers({ toFake: ['Date'] });
    vi.setSystemTime(NOW);
    confirm = vi.spyOn(window, 'confirm').mockReturnValue(true);
    fleet.mockResolvedValue({ ok: true, value: fleetResponse() });
  });

  afterEach(() => {
    wrapper?.unmount();
    wrapper = null;
    confirm.mockRestore();
    vi.useRealTimers();
  });

  it('sends the background jobs to their own page', async () => {
    const page = await mountSection();
    expect(page.find('.fleet-job-row').exists()).toBe(false);
    expect(page.get('#fleet-roll-hint a').attributes('href')).toBe('/admin/jobs');
  });

  it('shows each machine as a card, in words, each to its own page', async () => {
    const page = await mountSection();
    const cards = page.findAll('article.machine-card');
    expect(cards).toHaveLength(2);
    // The machine by its row, the cloud's id beneath.
    expect(cards[0].get('h3').text()).toBe('Machine 3');
    expect(cards[0].get('h3 a').attributes('href')).toBe('/admin/fleet/machines/3');
    expect(cards[0].text()).toContain('i-0abc000000000003');
    expect(cards[0].text()).toContain('Shared machine');
    expect(cards[0].text()).toContain('Ready');
    expect(cards[0].text()).toContain('2 of 12 slots in use');
    expect(cards[0].text()).toContain('13 GB sent this month');
    expect(cards[0].get('.machine-report').text()).toBe('Last report 4 s ago');
    expect(cards[1].text()).toContain('Own machine');
    expect(cards[1].text()).toContain('c7a.medium');
    expect(page.text()).toContain('1 shared, 1 own');
    expect(page.text()).toContain('1 of 24 shared');
    expect(page.text()).toContain('0 of 2 own');
  });

  it("puts each server in its machine's slot from the machine's own slot holders, whatever the list shows", async () => {
    const page = await mountSection();
    const cells = page.findAll('article.machine-card')[0].findAll('.slot-cell');
    expect(cells).toHaveLength(12);
    expect(cells[3].get('a').attributes('href')).toBe('/admin/fleet/1');
    expect(cells[3].text()).toContain('Running');
    expect(cells[5].text()).toContain('Starting');
    expect(cells[0].text()).toContain('Free');
    expect(fleet).toHaveBeenCalledTimes(1);

    // A filter that leaves the server out changes no slot.
    fleet.mockResolvedValue({ ok: true, value: fleetResponse({ servers: [] }) });
    await page.get('#fleet-filter-owner').setValue('nobody');
    await page.get('#fleet-filter-owner').element.closest('form')!.dispatchEvent(new Event('submit'));
    await flushPromises();
    expect(fleet).toHaveBeenCalledTimes(2);
    expect(page.findAll('tr.fleet-server-row')).toHaveLength(0);
    const cell = page.findAll('article.machine-card')[0].findAll('.slot-cell')[3];
    expect(cell.text()).toContain('Running');
    expect(cell.get('.sr-only').text()).toBe(': Server 1');
  });

  it('lists the servers with owner, tier, players and slot, each to its own page', async () => {
    const page = await mountSection();
    const rows = page.findAll('tr.fleet-server-row');
    expect(rows).toHaveLength(2);
    expect(rows[0].text()).toContain('owner1');
    expect(rows[0].text()).toContain('Tier 2');
    expect(rows[0].text()).toContain('5 of 32');
    expect(rows[0].text()).toContain('Slot 3, game port 32771');
    expect(rows[0].get('th a').attributes('href')).toBe('/admin/fleet/1');
    expect(rows[1].text()).toContain('Not placed');
    expect(rows[1].text()).toContain('Not listed');
  });

  it("shows each server's data, and whether newer waits", async () => {
    fleet.mockResolvedValue({
      ok: true,
      value: fleetResponse({
        servers: [
          fleetServer(1, { outdated: true }),
          fleetServer(2, {
            expansion: { id: 2, code: 'jox01', display_name: 'Escalation' },
            data: { base: { tree_id: 10, version: '1.7.5.5' }, expansion: { tree_id: 12, version: '1.7.5.5' } },
            expansion_tree_id: 12,
            update_requested: true,
            outdated: true,
          }),
          fleetServer(3, { base_tree_id: null, machine_ref: null, state: 'requested' }),
        ],
      }),
    });
    const page = await mountSection();
    const servers = page.findAll('tr.fleet-server-row');
    expect(servers[0].get('.fleet-server-data').text()).toContain('1.7.5.5');
    expect(servers[0].get('.fleet-server-data').text()).toContain('Outdated');
    expect(servers[1].get('.fleet-server-data').text()).toContain('Escalation 1.7.5.5');
    expect(servers[1].get('.fleet-server-data').text()).toContain('Update requested');
    expect(servers[1].get('.fleet-server-data').text()).not.toContain('Outdated');
    expect(servers[2].get('.fleet-server-data').text()).toBe('Not pinned');
  });

  it('updates every outdated server after a confirmation, and offers it only while one is', async () => {
    updateAll.mockResolvedValue({ ok: true, value: 1 });
    const page = await mountSection();
    const button = () => page.findAll('button').find((b) => b.text() === 'Update outdated servers…')!;
    expect((button().element as HTMLButtonElement).disabled).toBe(true);

    fleet.mockResolvedValue({ ok: true, value: fleetResponse({ servers: [fleetServer(1, { outdated: true })] }) });
    await page.findAll('button').find((b) => b.text() === 'Refresh')!.trigger('click');
    await flushPromises();
    expect((button().element as HTMLButtonElement).disabled).toBe(false);
    await button().trigger('click');
    await flushPromises();
    expect(confirm).toHaveBeenCalledWith(expect.stringContaining('Update 1 outdated server?'));
    expect(confirm).toHaveBeenCalledWith(expect.stringContaining('its players drop'));
    expect(updateAll).toHaveBeenCalledWith();
    expect(page.get('[role="status"]').text()).toBe('1 server is updating.');
  });

  it('filters by owner and state', async () => {
    const page = await mountSection();
    await page.get('#fleet-filter-owner').setValue(' owner1 ');
    await page.get('#fleet-filter-state').setValue('ended');
    await page.get('#fleet-filter-owner').element.closest('form')!.dispatchEvent(new Event('submit'));
    await flushPromises();
    expect(fleet).toHaveBeenLastCalledWith({ owner: 'owner1', state: 'ended' }, expect.anything());
  });

  it('drains and ends a machine after a confirmation', async () => {
    machine.mockResolvedValue({ ok: true, value: undefined });
    const page = await mountSection();
    const row = page.findAll('article.machine-card')[0];
    await row.findAll('button').find((b) => b.text().startsWith('Drain'))!.trigger('click');
    await flushPromises();
    // The servers move now, one at a time, each down while it moves.
    expect(confirm).toHaveBeenCalledWith(expect.stringContaining('Drain machine 3?'));
    expect(confirm).toHaveBeenCalledWith(expect.stringContaining('one at a time; each server is down while it moves'));
    expect(machine).toHaveBeenCalledWith('3', 'drain');
    expect(page.get('[role="status"]').text()).toBe('Machine 3 is draining.');

    confirm.mockReturnValueOnce(false);
    await row.findAll('button').find((b) => b.text().startsWith('End'))!.trigger('click');
    await flushPromises();
    expect(machine).toHaveBeenCalledTimes(1);
  });

  it('sends a roll to its preview, on its own tab', async () => {
    rollPlan.mockResolvedValue({ ok: true, value: { image_id: 'ami-0bbb', machines: [], replace: [] } });
    const page = await mountSection();
    const link = page.findAll('a').find((a) => a.text() === 'Roll the fleet…')!;
    expect(link.attributes('href')).toBe('/admin/fleet?tab=roll');
    expect(rollPlan).not.toHaveBeenCalled();
    page.unmount();

    const roll = await mountSection('/admin/fleet?tab=roll');
    expect(rollPlan).toHaveBeenCalledTimes(1);
    expect(roll.get('#fleet-roll-heading').text()).toBe('Roll the fleet');
    expect(roll.find('article.machine-card').exists()).toBe(false);
    expect(roll.get('nav[aria-label="Fleet pages"] [aria-current="page"]').text()).toBe('Roll');
  });

  it('offers no end for a machine that has ended or failed', async () => {
    fleet.mockResolvedValue({
      ok: true,
      value: fleetResponse({
        machines: [fleetMachine('5', { state: 'failed', slots_used: 0 }), fleetMachine('6', { state: 'ended', slots_used: 0 }), fleetMachine('7', { state: 'lost' })],
      }),
    });
    const page = await mountSection();
    // Only the lost one counts as a machine: a failed one never ran.
    expect(page.text()).toContain('1 shared, 0 own');
    // The failed and the ended one are history once a later machine launched.
    expect(page.findAll('article.machine-card')).toHaveLength(1);
    await page.findAll('button').find((b) => b.text() === 'Show history (2)')!.trigger('click');
    const rows = page.findAll('article.machine-card');
    expect(rows).toHaveLength(3);
    expect(rows[0].findAll('button')).toHaveLength(0);
    expect(rows[1].findAll('button')).toHaveLength(0);
    expect(rows[2].findAll('button').map((b) => b.text())).toEqual(['End machine 7']);
  });

  it('stops a server, and stops all behind the typed phrase', async () => {
    stopOne.mockResolvedValue({ ok: true, value: null });
    stopAll.mockResolvedValue({ ok: true, value: 1 });
    fleet.mockResolvedValue({ ok: true, value: fleetResponse({ servers: [fleetServer(1)] }) });
    const page = await mountSection();
    await page.get('tr.fleet-server-row').findAll('button').find((b) => b.text().startsWith('Stop'))!.trigger('click');
    await flushPromises();
    expect(stopOne).toHaveBeenCalledWith(1);

    await page.findAll('button').find((b) => b.text() === 'Stop all live servers…')!.trigger('click');
    await page.get('#stop-all-confirm').setValue('STOP AL');
    const submit = page.get('#stop-all-confirm').element.closest('form')!.querySelector('button[type="submit"]') as HTMLButtonElement;
    expect(submit.disabled).toBe(true);
    await page.get('#stop-all-confirm').setValue('STOP ALL');
    await page.get('#stop-all-confirm').element.closest('form')!.dispatchEvent(new Event('submit'));
    await flushPromises();
    expect(stopAll).toHaveBeenCalledTimes(1);
    expect(page.get('[role="status"]').text()).toBe('1 server is stopping.');
  });
});
