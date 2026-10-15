import { flushPromises, mount } from '@vue/test-utils';
import { afterEach, beforeEach, describe, expect, it, vi, type MockInstance } from 'vitest';
import FleetSection from './FleetSection.vue';
import { fetchHostedFleet, machineAction, rollFleet, stopAllFleetServers, stopFleetServer } from '../../api/admin';
import { fleetResponse, fleetServer, machine as fleetMachine } from '../../test/adminFixtures';
import { testRouter } from '../../test/router';

vi.mock('../../api/admin', () => ({
  fetchHostedFleet: vi.fn(),
  machineAction: vi.fn(),
  rollFleet: vi.fn(),
  stopAllFleetServers: vi.fn(),
  stopFleetServer: vi.fn(),
}));

const fleet = vi.mocked(fetchHostedFleet);
const machine = vi.mocked(machineAction);
const roll = vi.mocked(rollFleet);
const stopAll = vi.mocked(stopAllFleetServers);
const stopOne = vi.mocked(stopFleetServer);

describe('FleetSection', () => {
  let wrapper: ReturnType<typeof mount> | null = null;
  let confirm: MockInstance<typeof window.confirm>;

  async function mountSection() {
    const router = await testRouter('/admin/fleet');
    wrapper = mount(FleetSection, { global: { plugins: [router] } });
    await flushPromises();
    return wrapper;
  }

  beforeEach(() => {
    vi.clearAllMocks();
    confirm = vi.spyOn(window, 'confirm').mockReturnValue(true);
    fleet.mockResolvedValue({ ok: true, value: fleetResponse() });
  });

  afterEach(() => {
    wrapper?.unmount();
    wrapper = null;
    confirm.mockRestore();
  });

  it('lists the machines in words', async () => {
    const page = await mountSection();
    const rows = page.findAll('tr.fleet-machine-row');
    expect(rows).toHaveLength(2);
    // The machine by its row, the cloud's id beneath.
    expect(rows[0].get('th').text()).toContain('3');
    expect(rows[0].get('th').text()).toContain('i-0abc000000000003');
    expect(rows[0].text()).toContain('Shared machine');
    expect(rows[0].text()).toContain('Ready');
    expect(rows[0].text()).toContain('2 of 12');
    expect(rows[0].text()).toContain('13 GB');
    expect(rows[1].text()).toContain('Own machine');
    expect(rows[1].text()).toContain('c7a.medium');
    expect(page.text()).toContain('1 shared, 1 own');
    expect(page.text()).toContain('1 of 24');
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

  it('filters by owner and state', async () => {
    const page = await mountSection();
    await page.get('#fleet-filter-owner').setValue(' owner1 ');
    await page.get('#fleet-filter-state').setValue('ended');
    await page.get('#fleet-filter-owner').element.closest('form')!.dispatchEvent(new Event('submit'));
    await flushPromises();
    expect(fleet).toHaveBeenLastCalledWith({ owner: 'owner1', state: 'ended' }, expect.anything());
  });

  it('drains and ends a machine, and rolls the fleet, after a confirmation', async () => {
    machine.mockResolvedValue({ ok: true, value: undefined });
    roll.mockResolvedValue({ ok: true, value: undefined });
    const page = await mountSection();
    const row = page.findAll('tr.fleet-machine-row')[0];
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

    await page.findAll('button').find((b) => b.text() === 'Roll the fleet…')!.trigger('click');
    await flushPromises();
    expect(roll).toHaveBeenCalledTimes(1);
  });

  it('offers no end for a machine that has ended or failed', async () => {
    fleet.mockResolvedValue({
      ok: true,
      value: fleetResponse({
        machines: [fleetMachine('5', { state: 'failed', slots_used: 0 }), fleetMachine('6', { state: 'ended', slots_used: 0 }), fleetMachine('7', { state: 'lost' })],
      }),
    });
    const page = await mountSection();
    const rows = page.findAll('tr.fleet-machine-row');
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
