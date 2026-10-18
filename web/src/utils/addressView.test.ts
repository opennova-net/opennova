import { describe, expect, it } from 'vitest';
import {
  ADDRESS_NAME_RULE,
  addressEndpoint,
  addressNameProblem,
  addressStateLabel,
  freeAddresses,
  preferredAddress,
  serverAddressEndpoint,
} from './addressView';
import { address } from '../test/addressFixtures';
import { hostedServer } from '../test/hostedFixtures';
import type { ServerAddress } from '../types/addresses';

describe('addressNameProblem', () => {
  it("takes the service's names and refuses the rest with the rule", () => {
    for (const ok of ['smoke', 'abc', 'a1b', 'night-ops-2', '7th-fleet', 'a'.repeat(32)]) {
      expect(addressNameProblem(ok)).toBe('');
    }
    for (const bad of ['', 'ab', 'a'.repeat(33), 'Smoke', 'night_ops', '-smoke', 'smoke-', 'a--b', '1234', 'smoke.ops', 'café']) {
      expect(addressNameProblem(bad)).toBe(ADDRESS_NAME_RULE);
    }
  });
});

describe('the words and the endpoint', () => {
  it('names each state plainly, an unknown one as it is', () => {
    expect(addressStateLabel('allocating')).toBe('Being set up');
    expect(addressStateLabel('ready')).toBe('Ready');
    expect(addressStateLabel('lapsed')).toBe('Ending: its subscription ended');
    expect(addressStateLabel('releasing')).toBe('Being released');
    expect(addressStateLabel('released')).toBe('Released');
    expect(addressStateLabel('failed')).toBe('Could not be set up');
    expect(addressStateLabel('parked')).toBe('parked');
  });

  it('joins the IP and the port, and says nothing before an IP', () => {
    expect(addressEndpoint(address(9))).toBe('203.0.113.9:32768');
    expect(addressEndpoint(address(9, 'x', { public_ip: null, port: null }))).toBe('');
    const at: ServerAddress = { id: 9, name: 'x', hostname: null, public_ip: '203.0.113.9', state: 'ready' };
    expect(serverAddressEndpoint(at)).toBe('203.0.113.9:32768');
    expect(serverAddressEndpoint({ ...at, public_ip: null })).toBe('');
  });
});

describe('the picker', () => {
  const running = { id: 40, game_name: 'Night Ops', state: 'running' };
  const list = [
    address(1, 'smoke'),
    address(2, 'arena', { server: running }),
    address(3, 'spare'),
    address(4, 'gone', { state: 'releasing' }),
    address(5, 'fresh', { state: 'allocating', public_ip: null }),
  ];

  it('offers the ready addresses no live server uses', () => {
    expect(freeAddresses(list).map((a) => a.id)).toEqual([1, 3]);
  });

  it('starts on the address the server started again from, else the newest server', () => {
    const free = freeAddresses(list);
    const at = (id: number): ServerAddress => ({ id, name: `a${id}`, hostname: null, public_ip: null, state: 'ready' });
    const servers = [hostedServer(10, { address: at(1) }), hostedServer(12, { address: at(3) }), hostedServer(11)];
    // The newest server that ran at one: 12, at address 3.
    expect(preferredAddress(free, servers)).toBe(3);
    // Started again from 10: its address.
    expect(preferredAddress(free, servers, servers[0])).toBe(1);
    // An address another server holds now: the first free one.
    expect(preferredAddress(free, [hostedServer(13, { address: at(2) })])).toBe(1);
    expect(preferredAddress(free, [])).toBe(1);
    expect(preferredAddress([], servers)).toBeNull();
  });
});
