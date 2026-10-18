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
});
