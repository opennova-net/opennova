// Player accounts as the admin routes answer them (snake_case;
// apps/novaworld_web/admin_user_routes.cpp).

import type { AdminAddress } from './addresses';
import type { AdminSubscription, TierNumber } from './billing';
import type { EntitlementSource, HostedServer } from './hostedServer';

// A website role (players.role): every account is a `player`; `admin` opens
// the /api/admin/* routes to the account's session.
export type Role = 'player' | 'admin';

export interface User {
  id: number;
  username: string;
  pcid: string;
  nwh: string;
  nwhandle: string;
  account_status: string;
  // The account's email address (players.email, lowercased), null for none:
  // read-only on the admin pages.
  email: string | null;
  role: Role;
  // An admin's grants (comp_grants, 0 to 16 each): servers on shared
  // machines, and dedicated machines (one more server each).
  comp_servers: number;
  comp_own_machines: number;
  // The account's hosting entitlement: its servers on shared machines (the
  // larger of its tier's and the grant's) plus its dedicated machines.
  max_servers: number;
}

// The entitlement a users list row carries: how many servers, how many of
// them on shared machines and from where (the tier, the admin's grant: with
// both, both show), and its dedicated machines with the grant's among them.
export interface UserEntitlementSummary {
  max_servers: number;
  shared_servers: number;
  source: EntitlementSource;
  tier: TierNumber | null;
  comp_servers: number;
  // Every granting subscription's quantity (no tier needed) plus
  // comp_own_machines.
  own_machines: number;
  comp_own_machines: number;
}

// GET /api/admin/users's rows.
export interface UserListEntry extends User {
  entitlement: UserEntitlementSummary;
  // The account's live hosted servers.
  live_servers: number;
  // Its support tier subscription's Stripe status, null for none.
  subscription_status: string | null;
}

// The account's hosting entitlement in full, on its admin page.
export interface UserEntitlement extends UserEntitlementSummary {
  // The servers its support tier alone grants (0 with none): its servers on
  // shared machines are the larger of this and comp_servers.
  tier_servers: number;
  can_start: boolean;
}

// PUT /api/admin/users/<id>/comp's body: the servers on shared machines and
// the dedicated machines granted, which add up.
export interface CompGrants {
  servers: number;
  own_machines: number;
}

// PUT /api/admin/users/<id>/comp's reply: the account, and the sold capacity
// with the grant counted.
export interface CompGrantsReply {
  user: User;
  capacity: SoldCapacity | null;
}

// What the service has sold of one item (admin grants, subscriptions and
// open checkouts together) against what the fleet sells of it.
export interface SoldItem {
  sold: number;
  sellable: number;
}

// The service's sold capacity (hosted/model/sold_capacity.h): shared places,
// dedicated machines and static addresses. `oversold` when any is past what
// the fleet sells (an admin's grant can take it there): new subscriptions of
// it are paused.
export interface SoldCapacity {
  shared: SoldItem;
  own_machines: SoldItem;
  addresses: SoldItem;
  oversold: boolean;
}

// GET /api/admin/users/<id>.
export interface UserDetail {
  user: User;
  entitlement: UserEntitlement;
  // The service's sold capacity; null when the reply named none.
  capacity: SoldCapacity | null;
  // Its hosted servers: the live ones, then the newest ended ones.
  servers: HostedServer[];
  // Its static addresses, the held ones first.
  addresses: AdminAddress[];
  billing: {
    // The Stripe mode the ids belong to (the dashboard links follow it).
    livemode: boolean;
    stripe_customer_id: string | null;
    subscriptions: AdminSubscription[];
  };
}

export interface CreateUserPayload {
  username: string;
  password: string;
  pcid: string;
  nwhandle: string;
  nwh?: string;
  email?: string;
}

export interface UpdateUserPayload {
  username?: string;
  password?: string;
  pcid?: string;
  nwhandle?: string;
  nwh?: string;
  role?: Role;
}

export interface RegisterPayload {
  username: string;
  password: string;
  nwhandle?: string;
  // Required: the welcome mail and the password reset links go here.
  email: string;
}

// The public registration's reply.
export interface UserResponse {
  user: User;
}

// The most an admin may grant of each (PUT /api/admin/users/<id>/comp).
export const COMP_SERVERS_MAX = 16;
export const COMP_OWN_MACHINES_MAX = 16;
