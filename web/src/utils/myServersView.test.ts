import { describe, expect, it } from 'vitest';
import { hostedServer, listedServer, serversResponse } from '../test/hostedFixtures';
import {
  biggerTierOffered,
  canStartAnother,
  endedServers,
  hostingView,
  listingsOutrunHosted,
  liveServers,
  ownMachinesFree,
  sharedFree,
  splitListings,
  startBlock,
} from './myServersView';

describe('hostingView', () => {
  it('loads, then fails or shows off, locked or ready', () => {
    expect(hostingView(null, false)).toBe('loading');
    expect(hostingView(null, true)).toBe('error');
    expect(hostingView(serversResponse([], { enabled: false }), false)).toBe('off');
    expect(hostingView(serversResponse([], { data_sets: [] }), false)).toBe('off');
    expect(hostingView(serversResponse([], { entitlement: { max_servers: 0, source: 'none' } }), false)).toBe('locked');
    expect(hostingView(serversResponse([]), false)).toBe('ready');
    // Hosting off wins over no entitlement: nothing to sell.
    expect(hostingView(serversResponse([], { enabled: false, entitlement: { max_servers: 0 } }), false)).toBe('off');
  });
});

describe('startBlock and canStartAnother', () => {
  it('allows a start while the entitlement has room', () => {
    const data = serversResponse([hostedServer(1), hostedServer(2)]);
    expect(data.entitlement.used).toBe(2);
    expect(startBlock(data)).toBeNull();
    expect(canStartAnother(data)).toBe(true);
    expect(canStartAnother(null)).toBe(false);
  });

  it('names why no start is allowed', () => {
    expect(startBlock(serversResponse([], { enabled: false }))).toBe('off');
    expect(startBlock(serversResponse([], { entitlement: { max_servers: 0 } }))).toBe('locked');
    expect(startBlock(serversResponse([], { entitlement: { can_start: false } }))).toBe('past_due');
    // A stopping server still counts.
    const three = [hostedServer(1), hostedServer(2), hostedServer(3, { state: 'stopping' })];
    expect(startBlock(serversResponse(three))).toBe('limit');
    expect(startBlock(serversResponse([], { full: true }))).toBe('full');
    // Past due outranks the limit: the payment is what to fix first.
    expect(startBlock(serversResponse(three, { entitlement: { can_start: false } }))).toBe('past_due');
  });

  it('reaches the limit only once both pools are full', () => {
    // Tier 2 and a dedicated machine: both shared servers live, the machine free.
    const shared = [hostedServer(1), hostedServer(2)];
    const room = { max_servers: 3, shared_servers: 2, own_machines: 1 };
    expect(startBlock(serversResponse(shared, { entitlement: room }))).toBeNull();
    // The dedicated machine busy too.
    const all = [...shared, hostedServer(3, { own_machine: true })];
    expect(startBlock(serversResponse(all, { entitlement: room }))).toBe('limit');
    // A dedicated machine alone, no tier: its one server, then the limit.
    const alone = { max_servers: 1, shared_servers: 0, own_machines: 1, source: 'none' as const, tier: null };
    expect(startBlock(serversResponse([], { entitlement: alone }))).toBeNull();
    expect(startBlock(serversResponse([hostedServer(4, { own_machine: true })], { entitlement: alone }))).toBe('limit');
  });

  it('offers a bigger tier only below the biggest, and never with an admin grant', () => {
    expect(biggerTierOffered(serversResponse([], { entitlement: { tier: 2, max_servers: 2 } }).entitlement)).toBe(true);
    expect(biggerTierOffered(serversResponse([], { entitlement: { tier: 3 } }).entitlement)).toBe(false);
    // A grant over a tier, or a grant alone: it buys no tier.
    expect(biggerTierOffered(serversResponse([], { entitlement: { tier: 1, granted: true } }).entitlement)).toBe(false);
    expect(
      biggerTierOffered(serversResponse([], { entitlement: { tier: null, source: 'comp', granted: true } }).entitlement),
    ).toBe(false);
  });

  it('counts the free places on shared machines', () => {
    expect(sharedFree(serversResponse([hostedServer(1)]).entitlement)).toBe(2);
    // A stopping server still holds its place.
    const stopping = serversResponse([hostedServer(1), hostedServer(2, { state: 'stopping' })], { entitlement: { shared_servers: 2 } });
    expect(sharedFree(stopping.entitlement)).toBe(0);
    // Past the allowance (a lowered tier, its stops pending): none, never fewer.
    expect(sharedFree(serversResponse([], { entitlement: { shared_servers: 1, shared_used: 3 } }).entitlement)).toBe(0);
  });

  it('counts the free dedicated machines', () => {
    expect(ownMachinesFree(serversResponse([]).entitlement)).toBe(0);
    expect(ownMachinesFree(serversResponse([], { entitlement: { own_machines: 2, own_used: 1 } }).entitlement)).toBe(1);
    expect(ownMachinesFree(serversResponse([], { entitlement: { own_machines: 1, own_used: 2 } }).entitlement)).toBe(0);
  });
});

describe('the server lists', () => {
  it('splits live from ended', () => {
    const servers = [hostedServer(1), hostedServer(2, { state: 'stopping' }), hostedServer(3, { state: 'stopped' })];
    expect(liveServers(servers).map((s) => s.id)).toEqual([1, 2]);
    expect(endedServers(servers).map((s) => s.id)).toEqual([3]);
  });

  it("matches each live hosted server to its own listing, and keeps the others apart", () => {
    const servers = [
      hostedServer(1, {
        listing: { rid: 10, server_name: 'a', mission_name: '', players: 0, max_players: 32, host_ip: '1.2.3.4', host_port: 1 },
      }),
      hostedServer(2),
      hostedServer(3, { state: 'stopped' }),
    ];
    const hosts = [
      listedServer(10, { hosted_server_id: 1 }),
      listedServer(11, { hosted_server_id: 2 }),
      listedServer(12),
      // A server this page does not show yet: it waits.
      listedServer(13, { hosted_server_id: 4 }),
    ];
    const { hosted, others } = splitListings(hosts, servers);
    expect(hosted.get(1)?.rid).toBe(10);
    expect(hosted.get(2)?.rid).toBe(11);
    expect(hosted.has(3)).toBe(false);
    expect(others?.map((h) => h.rid)).toEqual([12]);
    expect(splitListings(null, servers).others).toBeNull();
    expect(listingsOutrunHosted(hosts, servers)).toBe(true);
    expect(listingsOutrunHosted(hosts.slice(0, 3), servers)).toBe(false);
  });
});
