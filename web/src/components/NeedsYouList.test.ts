import { mount } from '@vue/test-utils';
import { describe, expect, it } from 'vitest';
import NeedsYouList, { type NeedsYouItem } from './NeedsYouList.vue';
import { testRouter } from '../test/router';

const NOW = Date.parse('2026-10-27T12:00:00Z');

async function render(items: NeedsYouItem[] | null, empty?: string) {
  const router = await testRouter('/me');
  return mount(NeedsYouList, { props: { items, now: NOW, ...(empty === undefined ? {} : { empty }) }, global: { plugins: [router] } });
}

describe('NeedsYouList', () => {
  it('says it is checking before the first read, nothing needs you with none, and nothing when told so', async () => {
    expect((await render(null)).text()).toContain('Checking…');
    expect((await render([])).get('.needs-you-empty').text()).toBe('Nothing needs you.');
    const quiet = await render([], '');
    expect(quiet.find('.needs-you-empty').exists()).toBe(false);
    expect(quiet.find('ul').exists()).toBe(false);
  });

  it('draws a row each: its badge, its subject linked (a server through ServerLink), its text and since when', async () => {
    const view = await render([
      { key: 'a', tone: 'bad', label: 'Act now', subject: 'Night Raid', to: '/me/servers/9', server: { id: 9, name: 'Night Raid' }, text: 'Failed.', since: '2026-10-27 10:00:00' },
      { key: 'b', tone: 'muted', label: 'Note', subject: 'Night Ops', to: '/me/projects/NIGHTOPS', text: '0.5.0 awaits approval.' },
    ]);
    const rows = view.findAll('.needs-you-item');
    expect(rows.map((r) => [r.get('.badge').text(), r.get('a').text(), r.get('a').attributes('href'), r.get('.needs-you-text').text()])).toEqual([
      ['Act now', 'Night Raid', '/me/servers/9', 'Failed.'],
      ['Note', 'Night Ops', '/me/projects/NIGHTOPS', '0.5.0 awaits approval.'],
    ]);
    expect(rows[0].find('.server-link').exists()).toBe(true);
    expect(rows[0].get('.badge').classes()).toContain('badge-bad');
    expect(rows[0].get('time').text()).toBe('2 hours ago');
    expect(rows[1].find('time').exists()).toBe(false);
  });
});
