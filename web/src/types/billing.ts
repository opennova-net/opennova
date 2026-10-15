// Support tiers and the account's billing, as the NovaWorld service's billing
// routes answer them (snake_case; apps/novaworld_server/billing/
// billing_routes.cpp). Amounts are in the currency's smallest unit (cents);
// times are ISO 8601 UTC.

export type TierNumber = 1 | 2 | 3;

// What a checkout buys: a support tier, or the own-machine add-on.
export type BillingItem = 'tier1' | 'tier2' | 'tier3' | 'own_machine';

// One support tier on offer. `amount` is null while the service could not
// read its price from Stripe.
export interface TierOffer {
  tier: TierNumber;
  // The hosted servers it lets the account run at once.
  max_servers: number;
  amount: number | null;
  interval: 'month';
}

// The own-machine add-on on offer: one server of the account's tier on a
// machine of its own, per unit.
export interface OwnMachineOffer {
  amount: number | null;
  interval: string;
}

// GET /api/billing/tiers (public).
export interface TiersResponse {
  // Support tiers are on (Stripe is configured and its prices checked).
  enabled: boolean;
  // ISO 4217, lower case as Stripe sends it ('usd').
  currency: string;
  tiers: TierOffer[];
  own_machine: OwnMachineOffer | null;
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

// The account's own-machine add-on subscription.
export interface OwnMachineSubscription {
  quantity: number;
  status: SubscriptionStatus;
  current_period_end: string;
  cancel_at_period_end: boolean;
}

// GET /api/me/billing, and POST /api/me/billing/sync.
export interface MyBilling {
  enabled: boolean;
  // The hosting entitlement: max(tier servers, comp_servers).
  max_servers: number;
  // An admin's grant, 0 for none.
  comp_servers: number;
  own_machines: number;
  // How many of its hosted servers are live now.
  used: number;
  can_start: boolean;
  subscription: TierSubscription | null;
  own_machine_subscription: OwnMachineSubscription | null;
  // The account has a Stripe customer: the billing portal opens.
  can_manage: boolean;
  // A checkout may start (billing on, the account active, no live tier).
  can_subscribe: boolean;
}

// ---------------------------------------------------------------------------
// The admin's view (GET /api/admin/billing, and a user's admin page).

// What a subscription is for.
export type SubscriptionItem = 'tier' | 'own_machine';

// One subscription as NovaWorld last read it from Stripe.
export interface AdminSubscription {
  stripe_subscription_id: string;
  item: SubscriptionItem;
  // The tier, for a tier subscription; null for the add-on.
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

// GET /api/admin/billing.
export interface AdminBillingResponse {
  enabled: boolean;
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
  orphans: BillingOrphan[];
  recent_events: BillingEvent[];
}
