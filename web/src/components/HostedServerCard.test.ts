import { mount } from '@vue/test-utils';
import { describe, expect, it } from 'vitest';
import HostedServerCard from './HostedServerCard.vue';
import { hostedServer, listedServer } from '../test/hostedFixtures';
import { testRouter } from '../test/router';

describe('HostedServerCard', () => {
  it('names the server to its page, with its state, game, players and map, and Manage and Console', async () => {
    const router = await testRouter('/me/servers');
    const card = mount(HostedServerCard, {
      props: {
        server: hostedServer(1),
        listing: listedServer(21, { hosted_server_id: 1, players: 5, max_players: 24, mission_name: 'Night Raid' }),
        now: Date.now(),
      },
      global: { plugins: [router] },
    });
    expect(card.get('h3 a').attributes('href')).toBe('/me/servers/1');
    expect(card.get('.badge').text()).toBe('Running');
    const text = card.text();
    expect(text).toContain('5 of 24 players');
    expect(text).toContain('Night Raid');
    expect(card.findAll('a.btn-secondary').map((a) => [a.text().split(' ')[0], a.attributes('href')])).toEqual([
      ['Manage', '/me/servers/1'],
      ['Console', '/me/servers/1/console'],
    ]);
  });

  it("says where a start stands, or why the server's last run ended, with a start again", async () => {
    const router = await testRouter('/me/servers');
    const starting = mount(HostedServerCard, {
      props: {
        server: hostedServer(1, { state: 'provisioning', updated_at: '2026-10-14 12:00:00', phase: { name: 'launching_machine' } }),
        now: Date.parse('2026-10-14T12:30:00Z'),
      },
      global: { plugins: [router] },
    });
    expect(starting.get('.hosted-phase').text()).toBe('Starting a machine for it. This is taking longer than usual.');

    const ended = mount(HostedServerCard, {
      props: {
        server: hostedServer(2, { state: 'stopped', stopped_because: { cause: 'update', limit: null, exit_code: 0, oom: false, line: '' } }),
        now: Date.now(),
        canStart: true,
      },
      global: { plugins: [router] },
    });
    expect(ended.find('.hosted-phase').exists()).toBe(false);
    expect(ended.get('.hosted-stopped-because').text()).toBe('It was restarted onto a newer release.');
    expect(ended.findAll('a').find((a) => a.text().startsWith('Start again'))?.attributes('href')).toBe('/me/servers/new?from=2');
  });
});
