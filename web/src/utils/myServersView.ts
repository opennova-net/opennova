// What the My servers page (pages/MyServersPage.vue) shows: its hosted server
// section, from GET /api/me/server, and the account's other listed servers,
// from GET /api/me/hosts.

import type { HostedServer, MyHostedServerResponse } from '../types/hostedServer';
import type { MyServer } from '../types/myServers';

// The hosted server section:
//   loading - the first read has not answered;
//   error   - the first read failed;
//   hidden  - the account is not approved to host and has no live server;
//   live    - the account's server is live (its status, console and Stop);
//   off     - approved, but the service runs no hosted servers;
//   start   - approved and nothing live: the one Start (Try again after a
//             failure), above the last server when there was one.
// A live server shows whatever the approval or the service say: the service
// stops it on its own when either is withdrawn.
export type HostedView = 'loading' | 'error' | 'hidden' | 'live' | 'off' | 'start';

export function hostedView(data: MyHostedServerResponse | null, failed: boolean): HostedView {
  if (!data) return failed ? 'error' : 'loading';
  if (data.server?.live) return 'live';
  if (!data.can_host) return 'hidden';
  if (!data.enabled) return 'off';
  return 'start';
}

// The section's one start control: a new start, or another try after the
// last server failed. The service allows one live server per account (409
// already_live), so the page never offers one while a server is live.
export type StartKind = 'start' | 'retry';

export function startKind(last: HostedServer | null): StartKind {
  return last?.state === 'failed' ? 'retry' : 'start';
}

// The account's listed servers split in two: the live hosted server's own
// listing, which the hosted section shows with its controls, and every server
// the account hosts itself. The hosted listing is the row with the RID the
// hosted read names, else the row naming the live server's id. A hosted
// server's listing never shows among the others: a listing's
// hosted_server_id is set only while that server is live, and an account has
// at most one, so a listing whose server the hosted section does not show
// yet (the two reads a poll apart) waits for the next read.
export function splitListings(
  hosts: readonly MyServer[] | null,
  hosted: HostedServer | null,
): { hostedListing: MyServer | null; others: MyServer[] | null } {
  if (hosts === null) return { hostedListing: null, others: null };
  let hostedListing: MyServer | null = null;
  if (hosted?.live) {
    const rid = hosted.listing?.rid;
    hostedListing =
      (rid !== undefined ? hosts.find((h) => h.rid === rid) : undefined) ??
      hosts.find((h) => h.hosted_server_id === hosted.id) ??
      null;
  }
  return { hostedListing, others: hosts.filter((h) => h !== hostedListing && h.hosted_server_id == null) };
}

// Whether the listings name a live hosted server the hosted read does not
// show (another tab or device started one): the hosted read is then stale.
export function listingsOutrunHosted(hosts: readonly MyServer[], hosted: HostedServer | null): boolean {
  const shownId = hosted?.live ? hosted.id : null;
  return hosts.some((h) => h.hosted_server_id != null && h.hosted_server_id !== shownId);
}
