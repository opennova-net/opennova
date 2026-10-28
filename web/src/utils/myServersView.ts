// What the account's Servers page (pages/account/ServersPage.vue) and its
// start page show: from GET /api/me/servers its hosting entitlement and its
// hosted servers, and from GET /api/me/hosts the servers it lists, each
// hosted server's own listing matched to it.

import type { HostedServer, HostedServersResponse, HostingEntitlement } from '../types/hostedServer';
import type { MyServer } from '../types/myServers';

// The hosting area at the top of the page:
//   loading - the first read has not answered;
//   error   - the first read failed;
//   off     - the service runs no hosted servers (or offers no game to start
//             one of);
//   locked  - the account's entitlement is 0 servers: hosting needs a support
//             tier, a dedicated machine or an admin's grant (the callout to
//             the tiers);
//   ready   - the allowance meter, with a start when one is allowed.
// The account's servers show in every view but loading and error: a live one
// shows whatever the entitlement or the service say (the service stops it on
// its own when either is withdrawn).
export type HostingView = 'loading' | 'error' | 'off' | 'locked' | 'ready';

function offered(data: HostedServersResponse): boolean {
  return data.enabled && data.products.length > 0;
}

export function hostingView(data: HostedServersResponse | null, failed: boolean): HostingView {
  if (!data) return failed ? 'error' : 'loading';
  if (!offered(data)) return 'off';
  if (data.entitlement.max_servers === 0) return 'locked';
  return 'ready';
}

// Why no new server may start now, or null when one may:
//   off      - the service runs no hosted servers;
//   locked   - no entitlement;
//   past_due - a payment of the account's (a tier's or a dedicated
//              machine's) is past due (the live ones keep running);
//   limit    - every server the entitlement covers is live (stopping ones
//              included), on shared machines and on dedicated machines alike;
//   full     - no hosted server place the account could take is free.
export type StartBlock = 'off' | 'locked' | 'past_due' | 'limit' | 'full';

export function startBlock(data: HostedServersResponse): StartBlock | null {
  if (!offered(data)) return 'off';
  const e = data.entitlement;
  if (e.max_servers === 0) return 'locked';
  if (!e.can_start) return 'past_due';
  if (sharedFree(e) === 0 && ownMachinesFree(e) === 0) return 'limit';
  if (data.full) return 'full';
  return null;
}

export function canStartAnother(data: HostedServersResponse | null): boolean {
  return data !== null && startBlock(data) === null;
}

// The biggest support tier: none past it to move to.
const TOP_TIER = 3;

// Whether the account can move to a bigger support tier for more servers: not
// with an admin's grant (it buys none), nor on the biggest tier.
export function biggerTierOffered(e: HostingEntitlement): boolean {
  return !e.granted && (e.tier ?? 0) < TOP_TIER;
}

// Whether the account can buy more servers at all: a dedicated machine adds
// one with or without a tier, so every account but a granted one can.
export function moreServersOffered(e: HostingEntitlement): boolean {
  return !e.granted;
}

// Why no new server may start, in one sentence; `stopping`: one of the live
// ones is stopping (it still counts).
export function blockLine(block: StartBlock, stopping = false): string {
  switch (block) {
    case 'off':
      return 'Hosting is off right now.';
    case 'locked':
      return 'Hosting needs a tier, a dedicated machine or an admin grant.';
    case 'past_due':
      return "A payment did not go through, so new servers can't start.";
    case 'limit':
      return stopping
        ? 'Every server you may run is live; a stopping one counts until it stops.'
        : 'Every server you may run is live: stop one to start another.';
    case 'full':
      return 'Hosting is full right now; try again later.';
  }
}

// Where a block is lifted, or null: the payment, or more servers on the
// Billing page (none for a granted account, which buys none).
export function blockLink(block: StartBlock, e: HostingEntitlement): { to: string; label: string } | null {
  if (block === 'past_due') return { to: '/me/billing', label: 'Update payment method' };
  if (block === 'locked') return { to: '/hosting', label: 'See hosting' };
  if (block === 'limit' && moreServersOffered(e)) return { to: '/me/billing', label: 'Get more' };
  return null;
}

// How many more servers the account may start on shared machines.
export function sharedFree(e: HostingEntitlement): number {
  return Math.max(0, e.shared_servers - e.shared_used);
}

// How many of the account's dedicated machines are free for a new server.
export function ownMachinesFree(e: HostingEntitlement): number {
  return Math.max(0, e.own_machines - e.own_used);
}

export function liveServers(servers: readonly HostedServer[]): HostedServer[] {
  return servers.filter((s) => s.live);
}

export function endedServers(servers: readonly HostedServer[]): HostedServer[] {
  return servers.filter((s) => !s.live);
}

// The account's listed servers split in two: each live hosted server's own
// listing, by the server's id (the row with the RID its read names, else the
// row naming its id), and every server the account hosts itself. A hosted
// server's listing never shows among the others: a listing's
// hosted_server_id is set only while that server is live, so one whose
// server this page does not show yet (the two reads a poll apart) waits for
// the next read.
export function splitListings(
  hosts: readonly MyServer[] | null,
  servers: readonly HostedServer[],
): { hosted: Map<number, MyServer>; others: MyServer[] | null } {
  const hosted = new Map<number, MyServer>();
  if (hosts === null) return { hosted, others: null };
  for (const server of servers) {
    if (!server.live) continue;
    const rid = server.listing?.rid;
    const row =
      (rid !== undefined ? hosts.find((h) => h.rid === rid) : undefined) ??
      hosts.find((h) => h.hosted_server_id === server.id);
    if (row) hosted.set(server.id, row);
  }
  return { hosted, others: hosts.filter((h) => h.hosted_server_id == null) };
}

// Whether the listings name a live hosted server the hosted read does not
// show as live (another tab or device started one): the hosted read is then
// stale.
export function listingsOutrunHosted(hosts: readonly MyServer[], servers: readonly HostedServer[]): boolean {
  const shown = new Set(liveServers(servers).map((s) => s.id));
  return hosts.some((h) => h.hosted_server_id != null && !shown.has(h.hosted_server_id));
}
