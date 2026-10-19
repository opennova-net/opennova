// Support tiers and the account's billing, as the NovaWorld service's billing
// routes answer them (snake_case; apps/novaworld_common/billing/
// billing_routes.cpp). Amounts are in the currency's smallest unit (cents);
// times are ISO 8601 UTC.

import type { SoldCapacity } from './users';

export type TierNumber = 1 | 2 | 3;

// What a checkout buys: a support tier, a dedicated machine, or a static
// address for a dedicated machine.
export type BillingItem = 'tier1' | 'tier2' | 'tier3' | 'own_machine' | 'static_address';

// One support tier on offer. `amount` is null while the service could not
// read its price from Stripe. `available` is false while the fleet has sold
// all it holds of what the tier adds: new subscriptions to it are paused.
export interface TierOffer {
  tier: TierNumber;
  // The hosted servers it lets the account run at once.
  max_servers: number;
  amount: number | null;
  interval: 'month';
  available: boolean;
}

// A dedicated machine on offer: one more hosted server, on a machine of its
// own, with or without a support tier; each its own subscription.
// `available` is false while the fleet sells no more of them.
export interface OwnMachineOffer {
  amount: number | null;
  interval: string;
  available: boolean;
}

// The static address add-on on offer: one fixed address and name for a
// server on a dedicated machine, each its own subscription.
export interface StaticAddressOffer {
  amount: number | null;
  interval: string;
  available: boolean;
}

// GET /api/billing/tiers (public).
export interface TiersResponse {
  // Support tiers are on (Stripe is configured and its prices checked).
  enabled: boolean;
  // ISO 4217, lower case as Stripe sends it ('usd').
  currency: string;
  tiers: TierOffer[];
  own_machine: OwnMachineOffer | null;
  // Null when this deployment sells no static address.
  static_address: StaticAddressOffer | null;
}

// Stripe's subscription statuses; another one shows as it is.
export type SubscriptionStatus =
  | 'active'
  | 'trialing'
  | 'past_due'
  | 'canceled'
  | 'unpaid'
  | 'incomplete'
  | 'incomplete_expired'
  | 'paused'
  | (string & {});

// The account's support tier subscription.
export interface TierSubscription {
  tier: TierNumber;
  status: SubscriptionStatus;
  // It grants hosting now (active, trialing, or past due while Stripe
  // retries).
  entitled: boolean;
  current_period_end: string;
  // It ends at the period's end (canceled in the billing portal).
  cancel_at_period_end: boolean;
}

// One of the account's dedicated machine subscriptions (each checkout makes
// another), while not over.
export interface OwnMachineSubscription {
  stripe_subscription_id: string;
  // The dedicated machines it pays for.
  quantity: number;
  status: SubscriptionStatus;
  current_period_end: string;
  cancel_at_period_end: boolean;
}

// One static address's own subscription, while open.
export interface AddressSubscription {
  stripe_subscription_id: string;
  status: SubscriptionStatus;
  current_period_end: string;
  cancel_at_period_end: boolean;
  // Its address's row; null while bought and not named yet.
  address_id: number | null;
}

// GET /api/me/billing, and POST /api/me/billing/sync.
export interface MyBilling {
  enabled: boolean;
  // The hosting entitlement: its servers on shared machines plus its
  // dedicated machines (one server each).
  max_servers: number;
  // The servers it may run on shared machines: max(tier servers,
  // comp_servers).
  shared_servers: number;
  // An admin's grants, 0 for none: servers on shared machines, dedicated
  // machines, and static addresses.
  comp_servers: number;
  comp_own_machines: number;
  comp_addresses: number;
  // Any of them: the account's hosting is the grant, and it buys no tier,
  // dedicated machine or static address (the checkout refuses one).
  granted: boolean;
  // Its dedicated machines: every granting subscription's quantity (no tier
  // needed) plus the admin's.
  own_machines: number;
  // How many of its hosted servers are live now, and how many of those are
  // stopping.
  used: number;
  stopping: number;
  // The same on shared machines alone: a lower tier stops among the rest.
  shared_used: number;
  shared_stopping: number;
  can_start: boolean;
  subscription: TierSubscription | null;
  // Its dedicated machine subscriptions, newest first.
  own_machine_subscriptions: OwnMachineSubscription[];
  // The static addresses its subscriptions and an admin's grants pay for (at
  // most its dedicated machines), and each address subscription still open.
  static_addresses: number;
  address_subscriptions: AddressSubscription[];
  // The account has a Stripe customer: the billing portal opens.
  can_manage: boolean;
  // A tier checkout may start (billing on, the account active, no live tier,
  // no admin grant).
  can_subscribe: boolean;
}

// ---------------------------------------------------------------------------
// The admin's view (GET /api/admin/billing, and a user's admin page).

// What a subscription is for.
export type SubscriptionItem = 'tier' | 'own_machine' | 'static_address';

// One subscription as NovaWorld last read it from Stripe.
export interface AdminSubscription {
  stripe_subscription_id: string;
  item: SubscriptionItem;
  // The tier, for a tier subscription; null for a dedicated machine or a
  // static address.
  tier: TierNumber | null;
  quantity: number;
  status: SubscriptionStatus;
  entitled: boolean;
  current_period_end: string;
  cancel_at_period_end: boolean;
}

// A subscription in the billing overview, with whose it is.
export interface BillingSubscriptionRow extends AdminSubscription {
  user_id: number;
  username: string;
  stripe_customer_id: string;
}

// A Stripe subscription no account here owns (another deployment's, or one
// whose customer lost its link): an admin relinks it.
export interface BillingOrphan {
  stripe_customer_id: string;
  stripe_subscription_id: string;
  // The username Stripe's metadata names, '' when none.
  username_hint: string;
  status: SubscriptionStatus;
}

// One webhook event Stripe sent.
export interface BillingEvent {
  event_id: string;
  type: string;
  received_at: string;
  // What the service made of it: 'processed', 'ignored', 'failed: ...'.
  outcome: string;
}

// An account an admin grants hosting (comp_grants): its grant of servers on
// shared machines, dedicated machines and static addresses (its comp
// addresses that count).
export interface AdminCompGrant {
  user_id: number;
  username: string;
  servers: number;
  own_machines: number;
  addresses: number;
}

// GET /api/admin/billing. With billing off, `enabled` is false and only the
// limits hold, the sold capacity and the admin grants are read (the rest
// stays empty).
export interface AdminBillingResponse {
  enabled: boolean;
  // The hosted servers' limits hold: a pass would have stopped (or moved off
  // their dedicated machines) the servers of more owners at once than a real
  // lapse would, so no server stops or moves for its owner's limits until a
  // clean read from Stripe confirms them or an admin applies the stops.
  limits_hold: boolean;
  limits_hold_reason: string | null;
  // The stops and moves it keeps back, as the supervisor's last pass counted
  // them.
  limits_hold_owners: number;
  limits_hold_servers: number;
  // The Stripe key's mode.
  mode: 'test' | 'live';
  // Every configured price is active, monthly and in the key's mode.
  prices_ok: boolean;
  last_reconcile_at: string | null;
  last_reconcile_error: string | null;
  // Customers marked for a sync not done yet.
  pending_events: number;
  subscriptions: BillingSubscriptionRow[];
  // Accounts with more than one live tier subscription.
  duplicates: number[];
  // Prices our subscriptions sell that no STRIPE_PRICE_* setting names (a
  // sold price missing from the configuration): their subscribers get no
  // hosting, and no read from Stripe releases the limits hold meanwhile.
  unknown_prices: string[];
  orphans: BillingOrphan[];
  recent_events: BillingEvent[];
  // The service's sold capacity against what the fleet sells (the
  // fleet-health oversold finding links to this page); null when the reply
  // named none.
  capacity: SoldCapacity | null;
  // Every account an admin grants hosting, by id.
  comp_grants: AdminCompGrant[];
}
