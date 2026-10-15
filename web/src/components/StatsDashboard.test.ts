import { flushPromises, mount } from '@vue/test-utils';
import { afterEach, describe, expect, it, vi } from 'vitest';
import StatsDashboard from './StatsDashboard.vue';
import { parseStats } from '../api/hostedStats';
import { serverStats } from '../test/statsFixtures';
import type { StatsRange } from '../types/hostedStats';

describe('StatsDashboard', () => {
  let wrapper: ReturnType<typeof mount> | null = null;
  afterEach(() => {
    wrapper?.unmount();
    wrapper = null;
  });

  async function mountWith(load = vi.fn((range: StatsRange) => Promise.resolve({ ok: true as const, value: serverStats({ range }) }))) {
    wrapper = mount(StatsDashboard, {
      props: { load, baseId: 'test-stats', heading: 'Stats', variant: 'server', follow: false, initialRange: '1h' },
    });
    await flushPromises();
    return { wrapper, load };
  }

  it('shows the headline tiles in words', async () => {
    const { wrapper } = await mountWith();
    const tiles = wrapper.findAll('dl > div');
    const byLabel = Object.fromEntries(tiles.map((t) => [t.get('dt').text(), t.text()]));
    expect(byLabel['Availability']).toContain('83.3%');
    expect(byLabel['Availability']).toContain('Running 50 minutes of the 1 hour it was live, 1 restart');
    expect(byLabel['Peak players']).toContain('11');
    expect(byLabel['Peak players']).toContain('Average 5.3 in the last hour');
    expect(byLabel['Unique players']).toContain('about 17');
    expect(byLabel['Data sent']).toContain('1.3 GB');
    expect(byLabel['CPU']).toContain('21%');
    expect(byLabel['CPU']).toContain('Memory 180 MB');
    // Right now.
    expect(wrapper.text()).toContain('Running, up for 3 hours 5 minutes');
    expect(wrapper.text()).toContain('Time left in the round');
    expect(wrapper.text()).toContain('10 minutes');
    // No raw units, no em dash.
    expect(wrapper.text()).not.toMatch(/\bms\b/);
    expect(wrapper.text()).not.toContain('—');
  });

  it('draws the players with a gap where the server was not running, and a table of every value', async () => {
    const { wrapper } = await mountWith();
    const chart = wrapper.get('#test-stats-players');
    expect(chart.element.tagName).toBe('FIGURE');
    expect(chart.get('figcaption').text()).toContain('Players over time');
    // Two series: the legend names both.
    expect(chart.findAll('li').map((li) => li.text())).toEqual(['Players', 'Max players']);
    const line = chart.findAll('path').find((p) => p.classes().includes('stroke-chart-series'));
    expect(line?.attributes('d')?.match(/M/g)).toHaveLength(2);
    const rows = chart.findAll('tbody tr');
    expect(rows).toHaveLength(12);
    expect(rows[4].text()).toContain('Not running');
    expect(rows[11].text()).toContain('11');
  });

  it('reads again when the range changes', async () => {
    const { wrapper, load } = await mountWith();
    expect(load).toHaveBeenCalledTimes(1);
    await wrapper.get('#test-stats-range-7d').setValue(true);
    await flushPromises();
    expect(load).toHaveBeenCalledTimes(2);
    expect(load.mock.calls[1][0]).toBe('7d');
  });

  it('parses a reply and refuses a malformed one', () => {
    const good = serverStats();
    expect(parseStats(good)).toEqual(good);
    expect(parseStats({ ...good, range: '2h' })).toBeNull();
    expect(parseStats({ ...good, summary: { ...good.summary, peak_players: '11' } })).toBeNull();
    expect(parseStats({ ...good, series: { ...good.series, points: [{ t: 'later', running: true }] } })).toBeNull();
    const { server_id: _id, ...aggregate } = good;
    expect(parseStats({ ...aggregate, live: null })?.server_id).toBeUndefined();
  });
});
