import { flushPromises, mount } from '@vue/test-utils';
import { afterEach, describe, expect, it } from 'vitest';
import ServerTable from './ServerTable.vue';
import { testRouter } from '../test/router';
import type { ServerRow } from '../types/lobby';

function row(n: number, overrides: Partial<ServerRow> = {}): ServerRow {
  return {
    key: String(n),
    to: `/lobby/${n}`,
    serverName: `Server ${n}`,
    mission: 'AS - Kombang Valley',
    gameType: 'AAS',
    players: 4,
    maxPlayers: 32,
    ...overrides,
  };
}

const ROWS: ServerRow[] = [
  row(1, { serverName: '<c00FF00>Zulu</c>', players: 4, country: 'US', punkBuster: true }),
  row(2, { serverName: 'alpha', players: 12, mission: '<cFF0000>TDM - Fire</c>', gameType: '<c00FF00>TDM</c>', password: true }),
  row(3, { serverName: 'Bravo', players: 0, country: 'DE', expansion: 'jox01' }),
  row(4, { serverName: 'Charlie', players: 32, mission: '', gameType: 'COOP', locked: true }),
  row(5, { serverName: 'Delta', players: 4 }),
];

describe('ServerTable', () => {
  let wrapper: ReturnType<typeof mount> | null = null;
  afterEach(() => {
    wrapper?.unmount();
    wrapper = null;
  });

  async function open(rows: ServerRow[] = ROWS) {
    const router = await testRouter('/lobby');
    wrapper = mount(ServerTable, { props: { rows, empty: 'No servers right now.' }, global: { plugins: [router] } });
    await flushPromises();
    return { table: wrapper, router };
  }

  function names(table: ReturnType<typeof mount>): string[] {
    return table.findAll('tbody tr.server-row th').map((th) => th.find('a').text());
  }

  it('lists the most players first, then by name, each row in the game markup', async () => {
    const { table } = await open();
    expect(names(table)).toEqual(['Charlie', 'alpha', 'Delta', 'Zulu', 'Bravo']);
    const alpha = table.findAll('tbody tr.server-row')[1];
    expect(alpha.get('th').text()).toContain('Password');
    expect(alpha.findAll('td').map((c) => c.text())).toEqual(['TDM - Fire (TDM)', '12 / 32', '', '']);
    const zulu = table.findAll('tbody tr.server-row')[3];
    expect(zulu.text()).toContain('PunkBuster');
    expect(zulu.text()).toContain('US');
    expect(zulu.find('a span span').element.getAttribute('style')).toContain('color');
    expect(table.findAll('tbody tr.server-row')[0].text()).toContain('Locked');
    expect(table.findAll('tbody tr.server-row')[0].findAll('td')[0].text()).toBe('COOP');
    // No filter yet: no count.
    expect(table.find('.server-count').exists()).toBe(false);
  });

  it('sorts by a header, and turns it around on a second click', async () => {
    const { table } = await open();
    const header = (label: string) => table.findAll('thead th').find((th) => th.text().startsWith(label))!;
    await header('Server').get('button').trigger('click');
    expect(names(table)).toEqual(['alpha', 'Bravo', 'Charlie', 'Delta', 'Zulu']);
    expect(header('Server').attributes('aria-sort')).toBe('ascending');
    await header('Server').get('button').trigger('click');
    expect(names(table)).toEqual(['Zulu', 'Delta', 'Charlie', 'Bravo', 'alpha']);
    expect(header('Server').attributes('aria-sort')).toBe('descending');
    await header('Players').get('button').trigger('click');
    expect(names(table)).toEqual(['Charlie', 'alpha', 'Delta', 'Zulu', 'Bravo']);
    await header('Players').get('button').trigger('click');
    expect(names(table)).toEqual(['Bravo', 'Delta', 'Zulu', 'alpha', 'Charlie']);
    await header('Mission').get('button').trigger('click');
    expect(names(table)[0]).toBe('Charlie');
    expect(names(table)[1]).toBe('Bravo');
    expect(names(table).at(-1)).toBe('alpha');
    await header('Country').get('button').trigger('click');
    expect(names(table).slice(-2)).toEqual(['Bravo', 'Zulu']);
  });

  it('filters by the visible text, the game type and the switches, and counts what it shows', async () => {
    const { table } = await open();
    await table.get('.server-search').setValue('tdm');
    expect(names(table)).toEqual(['alpha']);
    expect(table.get('.server-count').text()).toBe('1 of 5 servers');
    // The markup is no text to match.
    await table.get('.server-search').setValue('c00FF00');
    expect(names(table)).toEqual([]);
    expect(table.text()).toContain('No server matches these filters.');
    await table.get('.server-search').setValue('');

    const types = table.findAll('.server-game-type option').map((o) => o.text());
    expect(types).toEqual(['All', 'AAS', 'COOP', 'TDM']);
    await table.get('.server-game-type').setValue('AAS');
    expect(names(table)).toEqual(['Delta', 'Zulu', 'Bravo']);
    await table.get('.server-game-type').setValue('');

    await table.get('.server-hide-empty').setValue(true);
    expect(names(table)).not.toContain('Bravo');
    await table.get('.server-hide-full').setValue(true);
    expect(names(table)).not.toContain('Charlie');
    await table.get('.server-hide-passworded').setValue(true);
    expect(names(table)).toEqual(['Delta', 'Zulu']);
    expect(table.get('.server-count').text()).toBe('2 of 5 servers');
  });

  it("opens a row's page from its name or anywhere on the row", async () => {
    const { table, router } = await open();
    const first = table.findAll('tbody tr.server-row')[0];
    expect(first.get('a').attributes('href')).toBe('/lobby/4');
    await first.findAll('td')[1].trigger('click');
    await flushPromises();
    expect(router.currentRoute.value.path).toBe('/lobby/4');
  });

  it('says its empty text with no rows, and names a server with none', async () => {
    let { table } = await open([]);
    expect(table.text()).toBe('No servers right now.');
    expect(table.find('table').exists()).toBe(false);
    wrapper?.unmount();
    ({ table } = await open([row(9, { serverName: '<c00FF00></c>' })]));
    expect(names(table)).toEqual(['Unnamed server']);
  });
});
