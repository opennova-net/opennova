// Player accounts as the admin routes answer them (snake_case;
// apps/novaworld_server/admin_user_routes.cpp).

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
  role: Role;
  // An admin's grant of hosted servers (players.comp_servers, 0 to 16).
  comp_servers: number;
  // The account's hosting entitlement: the larger of its tier's servers and
  // the grant.
  max_servers: number;
}

// The entitlement a users list row carries: how many, from where, the tier.
export interface UserEntitlementSummary {
  max_servers: number;
  source: EntitlementSource;
  tier: TierNumber | null;
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
  comp_servers: number;
  own_machines: number;
  can_start: boolean;
}

// GET /api/admin/users/<id>.
export interface UserDetail {
  user: User;
  entitlement: UserEntitlement;
  // Its hosted servers: the live ones, then the newest ended ones.
  servers: HostedServer[];
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
}

// The public registration's reply.
export interface UserResponse {
  user: User;
}

// The most an admin may grant (PUT /api/admin/users/<id>/comp-servers).
export const COMP_SERVERS_MAX = 16;
