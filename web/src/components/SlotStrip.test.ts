import { mount } from '@vue/test-utils';
import { describe, expect, it } from 'vitest';
import SlotStrip from './SlotStrip.vue';
import { fleetServer } from '../test/adminFixtures';
import { testRouter } from '../test/router';
import type { FleetServer, SlotHolder } from '../types/hostedServer';

describe('SlotStrip', () => {
  async function mountStrip(holders: SlotHolder[], servers: FleetServer[] = [], slots = 6) {
    const router = await testRouter('/admin/fleet');
    return mount(SlotStrip, { props: { machineId: '3', slots, holders, servers }, global: { plugins: [router] } });
  }

  it('shows every slot, free or the server in it, in a word and a colour', async () => {
    const strip = await mountStrip(
      [
        { slot: 0, server_id: 1, state: 'running' },
        { slot: 2, server_id: 2, state: 'running' },
        { slot: 4, server_id: 3, state: 'booting' },
      ],
      [fleetServer(1, { slot: 0 }), fleetServer(2, { slot: 2, tick: { p99_ms: 31, late: 12 } })],
    );
    const cells = strip.findAll('.slot-cell');
    expect(cells).toHaveLength(6);
    expect(cells[0].text()).toContain('Running');
    expect(cells[0].get('a').attributes('href')).toBe('/admin/fleet/1');
    expect(cells[0].get('a').classes()).toContain('text-online');
    expect(cells[1].text()).toContain('Free');
    expect(cells[2].text()).toContain('Running late');
    expect(cells[2].get('a').classes()).toContain('text-warn');
    expect(cells[4].text()).toContain('Starting');
    // The server's name, for a screen reader.
    expect(cells[0].get('.sr-only').text()).toBe(': Server 1');
  });

  it('takes the name and ticks a holder carries (the machine page) over the fleet list', async () => {
    const strip = await mountStrip(
      [
        { slot: 0, server_id: 1, state: 'running', name: 'Ranked TDM', game_port: 32768, tick: { p99_ms: 31, late: 4 } },
        { slot: 1, server_id: 2, state: 'running', name: '', game_port: 32769, tick: null },
      ],
      [],
      2,
    );
    const cells = strip.findAll('.slot-cell');
    expect(cells[0].text()).toContain('Running late');
    expect(cells[0].get('.sr-only').text()).toBe(': Ranked TDM');
    expect(cells[1].text()).not.toContain('late');
    expect(cells[1].get('.sr-only').text()).toBe(': Server 2');
  });

  it("holds a slot the fleet list does not show, named by its server's id", async () => {
    const strip = await mountStrip([{ slot: 1, server_id: 150, state: 'running' }]);
    const cell = strip.findAll('.slot-cell')[1];
    expect(cell.text()).toContain('Running');
    expect(cell.get('a').attributes('href')).toBe('/admin/fleet/150');
    expect(cell.get('.sr-only').text()).toBe(': Server 150');
    // No tick read for it: on time.
    expect(cell.text()).not.toContain('late');
  });
});
