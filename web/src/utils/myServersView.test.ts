import { describe, expect, it } from 'vitest';
import { hostedServer, listedServer, serversResponse } from '../test/hostedFixtures';
import {
  allowanceParts,
  blockLine,
  blockLink,
  hostedSince,
  hostingView,
  listingGames,
  listingsOutrunHosted,
  liveServers,
  myServerRows,
  ownMachinesFree,
  sharedFree,
  sourceTitle,
  splitListings,
  startBlock,
  stateOf,
} from './myServersView';

describe('hostingView', () => {
  it('loads, then fails or shows off, locked or ready', () => {
    expect(hostingView(null, false)).toBe('loading');
    expect(hostingView(null, true)).toBe('error');
    expect(hostingView(serversResponse([], { enabled: false }), false)).toBe('off');
    expect(hostingView(serversResponse([], { products: [] }), false)).toBe('off');
    expect(hostingView(serversResponse([], { entitlement: { max_servers: 0, source: 'none' } }), false)).toBe('locked');
    expect(hostingView(serversResponse([]), false)).toBe('ready');
    // Hosting off wins over no entitlement: nothing to sell.
    expect(hostingView(serversResponse([], { enabled: false, entitlement: { max_servers: 0 } }), false)).toBe('off');
  });
});

describe('startBlock', () => {
  it('allows a start while the entitlement has room', () => {
    const data = serversResponse([hostedServer(1), hostedServer(2)]);
    expect(data.entitlement.used).toBe(2);
    expect(startBlock(data)).toBeNull();
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


  it('says why no server may start in one sentence, and where that is lifted', () => {
    const e = serversResponse([]).entitlement;
    expect(blockLine('limit')).toBe('Every server you may run is live: stop one to start another.');
    expect(blockLine('limit', true)).toContain('a stopping one counts until it stops');
    expect(blockLine('past_due')).toContain("new servers can't start");
    expect(blockLink('past_due', e)).toEqual({ to: '/me/billing', label: 'Update payment method' });
    expect(blockLink('limit', e)).toEqual({ to: '/me/billing', label: 'Get more' });
    // A granted account buys none.
    expect(blockLink('limit', serversResponse([], { entitlement: { granted: true, source: 'comp', tier: null } }).entitlement)).toBeNull();
    expect(blockLink('full', e)).toBeNull();
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
  it('keeps the live ones', () => {
    const servers = [hostedServer(1), hostedServer(2, { state: 'stopping' }), hostedServer(3, { state: 'stopped' })];
    expect(liveServers(servers).map((s) => s.id)).toEqual([1, 2]);
  });

  it("matches each live hosted server to its own listing, and keeps the others apart", () => {
    const servers = [
      hostedServer(1, {
        listing: { rid: 10, server_name: 'a', mission_name: '', game_type: '', players: 0, max_players: 32, host_ip: '1.2.3.4', host_port: 1 },
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

describe('the allowance', () => {
  it('says each pool the entitlement holds in words, and where the shared one comes from', () => {
    const e = serversResponse([hostedServer(1)], { entitlement: { max_servers: 3, own_machines: 1 } }).entitlement;
    expect(allowanceParts(e).map((p) => p.text)).toEqual(['1 of 2 shared servers in use', '0 of 1 dedicated machine in use']);
    expect(allowanceParts({ ...e, shared_servers: 0 }).map((p) => p.key)).toEqual(['own']);
    expect(allowanceParts({ ...e, own_used: 2 })[1].over).toBe(true);
    expect(sourceTitle(e)).toBe('From your tier');
    expect(sourceTitle({ ...e, source: 'comp' })).toBe('Granted by an admin');
    expect(sourceTitle({ ...e, source: 'none' })).toBeUndefined();
  });
});

describe('the table', () => {
  it('reads the state from the URL, live by default', () => {
    expect(stateOf({})).toBe('live');
    expect(stateOf({ state: 'ended' })).toBe('ended');
    expect(stateOf({ state: 'all' })).toBe('all');
    expect(stateOf({ state: 'nonsense' })).toBe('live');
  });

  it('lists the live hosted servers, then the games hosted yourself, then the ended ones', () => {
    const servers = [hostedServer(1), hostedServer(2, { state: 'stopped' }), hostedServer(3, { state: 'stopped' })];
    const hosts = [listedServer(10, { hosted_server_id: 1, server_name: 'Night Raid', players: 5, max_players: 24, mission_name: 'River Patrol' }), listedServer(12)];
    const games = new Map([[12, 'Joint Operations']]);
    const all = myServerRows(servers, hosts, 'all', games, new Set([3]));
    expect(all.map((r) => r.key)).toEqual(['hosted-1', 'listed-12', 'hosted-2']);
    expect(all[0]).toMatchObject({ name: 'Night Raid', game: 'Joint Operations', gameType: 'TDM', map: 'River Patrol', players: '5/24', since: '2026-10-14 12:02:00' });
    expect(all[1]).toMatchObject({ server: null, name: 'Listed 12', game: 'Joint Operations', players: '2/32', since: '2026-10-14 12:00:00' });
    expect(all[2]).toMatchObject({ name: 'Server 2', map: '', players: '', since: '2026-10-14 13:00:00' });
    expect(myServerRows(servers, hosts, 'live', games).map((r) => r.key)).toEqual(['hosted-1', 'listed-12']);
    expect(myServerRows(servers, hosts, 'ended', games).map((r) => r.key)).toEqual(['hosted-2', 'hosted-3']);
    // The listings unread: the hosted servers alone.
    expect(myServerRows(servers, null, 'live').map((r) => r.key)).toEqual(['hosted-1']);
  });

  it('dates a hosted server from when it entered its state', () => {
    expect(hostedSince(hostedServer(1))).toBe('2026-10-14 12:02:00');
    expect(hostedSince(hostedServer(1, { state: 'booting' }))).toBe('2026-10-14 12:05:00');
    expect(hostedSince(hostedServer(1, { state: 'failed' }))).toBe('2026-10-14 13:00:00');
  });

  it("names each listing's game from the lobby's groups", () => {
    const games = listingGames([
      { slug: 'jop_2_consumer', displayName: 'Joint Operations', hosts: [{ id: 12, serverName: 'a', players: 0, maxPlayers: 32 }] },
      { slug: 'dfx2', displayName: 'Delta Force Xtreme 2', hosts: [{ id: 13, serverName: 'b', players: 0, maxPlayers: 32 }] },
    ]);
    expect([...games]).toEqual([
      [12, 'Joint Operations'],
      [13, 'Delta Force Xtreme 2'],
    ]);
  });
});
