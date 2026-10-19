// Static addresses, as the NovaWorld service's address routes answer them
// (snake_case; apps/novaworld_web/hosted/address_routes.cpp). A static address
// is an own machine's: a fixed public IP its servers list at on every start,
// named <name>.<zone> in the deployment's play zone. Times are UTC
// 'YYYY-MM-DD HH:MM:SS'.

import type { HostedState } from './hostedServer';

// allocating -> ready -> lapsed (its grant ended, or an admin released it) ->
// releasing -> released; failed when the cloud would not give one, and
// allocating again once an admin retries it (or, for an exhausted quota, by
// itself at its retry_at).
export type AddressState = 'allocating' | 'ready' | 'lapsed' | 'releasing' | 'released' | 'failed';

// What pays for it: its own subscription, or an admin's grant.
export type AddressSource = 'subscription' | 'comp';

// The server running at an address now.
export interface AddressServer {
  id: number;
  game_name: string;
  state: HostedState | (string & {});
}

// One static address, as its owner sees it.
export interface Address {
  id: number;
  name: string;
  // <name>.<zone> ('smoke.play.opennova.net').
  hostname: string;
  state: AddressState | (string & {});
  // Null until the cloud gave it one.
  public_ip: string | null;
  // The game port its servers list at (an own machine's one slot), with the IP.
  port: number | null;
  source: AddressSource;
  // The subscription that pays for it; null for an admin's grant.
  stripe_subscription_id: string | null;
  // Its live server, or null.
  server: AddressServer | null;
  // Why it failed, in the service's words; null unless it did.
  reason: string | null;
  // Why it lapsed: 'grant' (its subscription stopped granting) or 'admin' (an
  // admin released it); null before it lapsed.
  lapse_cause: string | null;
  created_at: string | null;
  ready_at: string | null;
  lapsed_at: string | null;
  released_at: string | null;
}

// GET /api/me/addresses.
export interface AddressesResponse {
  // The service sells static addresses.
  enabled: boolean;
  // The zone the names live in ('play.opennova.net').
  zone: string;
  // How many of the account's bought addresses wait for a name.
  unnamed: number;
  // The account's, the held ones first.
  addresses: Address[];
}

// The address a hosted server runs at (its server JSON's `address`).
export interface ServerAddress {
  id: number;
  name: string;
  // Null when the service has no zone set.
  hostname: string | null;
  public_ip: string | null;
  state: AddressState | (string & {});
}

// ---------------------------------------------------------------------------
// The admin's view (GET /api/admin/addresses, and a user's admin page).

export interface AdminAddress extends Address {
  // Null once the account is gone.
  owner: { id: number; username: string } | null;
  // The cloud's handle of it (an Elastic IP allocation id); null before one.
  allocation_id: string | null;
  // The name its DNS record has now; null for none.
  dns_name: string | null;
  // 'none', 'pending' (a delete waits to be in sync) or 'insync'.
  dns_state: string;
  // A failed one's next automatic try (an exhausted quota's); null for none.
  retry_at: string | null;
}

// An address the cloud holds for this deployment that no address row names
// (what fleet.sweep found): an admin releases it by hand.
export interface AddressOrphan {
  allocation_id: string;
  public_ip: string;
  // The address row its tag names, null for none.
  address_id: number | null;
  seen_at: string;
}

// GET /api/admin/addresses.
export interface AdminAddressesResponse {
  enabled: boolean;
  zone: string;
  // The addresses the cloud holds now (allocating, ready, lapsed, releasing).
  held: number;
  // How many the deployment sells (ONNET_FLEET_MAX_ADDRESSES).
  sellable: number;
  addresses: AdminAddress[];
  orphans: AddressOrphan[];
}
