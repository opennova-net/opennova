import { flushPromises, mount } from '@vue/test-utils';
import { afterEach, describe, expect, it, vi } from 'vitest';
import ConnectionsSection from './ConnectionsSection.vue';
import { fetchConnections } from '../../api/admin';

vi.mock('../../api/admin', () => ({ fetchConnections: vi.fn() }));

const connections = vi.mocked(fetchConnections);

describe('ConnectionsSection', () => {
  let wrapper: ReturnType<typeof mount> | null = null;
  afterEach(() => {
    wrapper?.unmount();
    wrapper = null;
  });

  it('names the sessions in words: no raw milliseconds, no protocol codes', async () => {
    const now = Date.now();
    connections.mockResolvedValue({
      ok: true,
      value: {
        count: 2,
        heartbeat_timeout_ms: 240_000,
        connections: [
          { id: 11, state: 'active', pn: 'NOVAWORLDUDP', identity: 'pilot', created_ms: now - 3 * 60_000, last_seen_ms: now - 5_000, addr: '198.51.100.4:7597' },
          { id: 12, state: 'handshaking', pn: 'JOINTOPERATIONS', identity: '', created_ms: now - 30_000, last_seen_ms: now - 30_000, addr: '198.51.100.5:7597' },
          { id: 13, state: 'closing', pn: 'SOMETHINGELSE', identity: '', created_ms: 0, last_seen_ms: 0, addr: '198.51.100.6:7597' },
        ],
      },
    });
    wrapper = mount(ConnectionsSection);
    await flushPromises();
    const text = wrapper.text();
    expect(wrapper.get('h1').text()).toBe('NovaWorld sessions');
    expect(text).toContain('Game clients signed in to NovaWorld. 2 signed in; a session closes after 4 minutes without a keepalive.');
    // No raw milliseconds and no bare abbreviations anywhere.
    expect(text).not.toMatch(/\bms\b/);
    expect(text).not.toContain('240000');
    expect(text).not.toMatch(/\bPN\b/);
    const headers = wrapper.findAll('thead th').map((th) => th.text());
    expect(headers).toEqual(['Session', 'State', 'Game', 'Account', 'Address', 'Connected', 'Last heard']);
    const rows = wrapper.findAll('tr.session-row');
    expect(rows[0].text()).toContain('Signed in');
    expect(rows[0].text()).toContain('NovaWorld lobby (any game)');
    expect(rows[0].text()).toContain('3 min ago');
    expect(rows[0].text()).toContain('just now');
    // The absolute UTC time on hover.
    expect(rows[0].findAll('time')[0].attributes('title')).toMatch(/UTC$/);
    expect(rows[1].text()).toContain('Signing in');
    expect(rows[1].text()).toContain('Joint Operations');
    expect(rows[1].text()).toContain('Not signed in yet');
    // An unknown protocol name shows as it is, with a hint.
    const unknown = rows[2].findAll('td')[1].get('span');
    expect(unknown.text()).toBe('SOMETHINGELSE');
    expect(unknown.attributes('title')).toBe('A protocol name this site does not know');
    expect(rows[2].text()).toContain('Unknown');
  });
});
