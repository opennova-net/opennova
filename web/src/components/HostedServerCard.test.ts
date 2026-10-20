import { mount } from '@vue/test-utils';
import { describe, expect, it } from 'vitest';
import HostedServerCard from './HostedServerCard.vue';
import { hostedServer } from '../test/hostedFixtures';
import { testRouter } from '../test/router';

describe('HostedServerCard', () => {
  it('shows the static address a server runs at, and none for a server without one', async () => {
    const router = await testRouter('/me/servers');
    const at = { id: 5, name: 'smoke', hostname: 'smoke.play.opennova.net', public_ip: '203.0.113.9', state: 'ready' };
    const card = mount(HostedServerCard, {
      props: { server: hostedServer(1, { own_machine: true, address: at }), now: Date.now() },
      global: { plugins: [router] },
    });
    const text = card.text().replace(/\s+/g, ' ');
    expect(text).toContain('Static address');
    expect(text).toContain('smoke.play.opennova.net 203.0.113.9:32768');

    // Before the cloud gave it an IP: the name alone.
    const bare = mount(HostedServerCard, {
      props: { server: hostedServer(2, { own_machine: true, address: { ...at, public_ip: null } }), now: Date.now() },
      global: { plugins: [router] },
    });
    expect(bare.text()).toContain('smoke.play.opennova.net');
    expect(bare.text()).not.toContain('32768');

    const shared = mount(HostedServerCard, { props: { server: hostedServer(3), now: Date.now() }, global: { plugins: [router] } });
    expect(shared.text()).not.toContain('Static address');
  });

  it("says where a start stands, or why the server's last run ended", async () => {
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
      },
      global: { plugins: [router] },
    });
    expect(ended.find('.hosted-phase').exists()).toBe(false);
    expect(ended.get('.hosted-stopped-because').text()).toBe('It was restarted onto newer game data.');
  });
});
