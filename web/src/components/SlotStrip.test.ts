import { mount } from '@vue/test-utils';
import { describe, expect, it } from 'vitest';
import SlotStrip from './SlotStrip.vue';
import { fleetServer } from '../test/adminFixtures';
import { testRouter } from '../test/router';

describe('SlotStrip', () => {
  async function mountStrip(servers: ReturnType<typeof fleetServer>[], slots = 6) {
    const router = await testRouter('/admin/fleet');
    return mount(SlotStrip, { props: { machineId: '3', slots, servers }, global: { plugins: [router] } });
  }

  it('shows every slot, free or the server in it, in a word and a colour', async () => {
    const strip = await mountStrip([
      fleetServer(1, { slot: 0 }),
      fleetServer(2, { slot: 2, tick: { p99_ms: 31, late: 12 } }),
      fleetServer(3, { slot: 4, state: 'booting' }),
    ]);
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

  it('reads no tick as on time', async () => {
    const strip = await mountStrip([fleetServer(1, { slot: 0, tick: null })], 1);
    expect(strip.get('.slot-cell').text()).toContain('Running');
    expect(strip.get('.slot-cell').text()).not.toContain('late');
  });
});
