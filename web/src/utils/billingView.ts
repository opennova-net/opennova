// What the support tier pages show of the account's billing (types/billing.ts):
// the status in words, whether a tier is live, what a sold-out item says, and
// what a checkout that came back has to wait for.

import type { AdminSubscription, BillingItem, MyBilling, SubscriptionStatus } from '../types/billing';

// An item the fleet has sold all it holds of (its offer's `available` false):
// what its card says instead of selling it.
export const HOSTING_FULL = 'Hosting is full: new subscriptions are paused.';

const STATUS_LABELS: Record<string, string> = {
  active: 'Active',
  trialing: 'Trial',
  past_due: 'Payment past due',
  canceled: 'Canceled',
  unpaid: 'Unpaid',
  incomplete: 'Waiting for the first payment',
  incomplete_expired: 'First payment never arrived',
  paused: 'Paused',
};

// A Stripe subscription status in words; one this site does not know shows as
// it is.
export function subscriptionStatusLabel(status: SubscriptionStatus): string {
  return STATUS_LABELS[status] ?? status;
}

// What a subscription is for, as the admin pages list it: "Tier 2", "Own
// machines: 2", "Static address".
export function subscriptionItemLabel(s: Pick<AdminSubscription, 'item' | 'tier' | 'quantity'>): string {
  if (s.item === 'own_machine') return `Dedicated machines: ${s.quantity}`;
  if (s.item === 'static_address') return 'Static address';
  return `Tier ${s.tier ?? '?'}`;
}

// The statuses after which a subscription is over: a new checkout may start.
const ENDED: ReadonlySet<string> = new Set(['canceled', 'incomplete_expired']);

// The account has a support tier subscription that is not over (it may be
// past due): the tiers are managed in the billing portal, not bought again.
export function hasLiveTier(billing: MyBilling | null): boolean {
  const sub = billing?.subscription;
  return !!sub && !ENDED.has(sub.status);
}

// What a checkout went to Stripe for: the item, and for an add-on the own
// machines or static addresses the account had before it left (each is a
// subscription of its own, so the return waits for one more).
export interface PendingCheckout {
  item: BillingItem;
  ownMachinesBefore: number;
  addressesBefore: number;
}

const granting = (status: SubscriptionStatus) => status === 'active' || status === 'trialing';

// The account's static address subscriptions that are active.
export function activeAddressSubscriptions(billing: MyBilling): number {
  return billing.address_subscriptions.filter((s) => granting(s.status)).length;
}

// A checkout that came back successful is confirmed once Stripe's word has
// reached NovaWorld: the tier grants hosting, or the account has more own
// machines (or active static addresses) than before, its add-on active.
export function checkoutConfirmed(billing: MyBilling, pending: PendingCheckout): boolean {
  if (pending.item === 'own_machine') {
    const own = billing.own_machine_subscription;
    return !!own && granting(own.status) && billing.own_machines > pending.ownMachinesBefore;
  }
  if (pending.item === 'static_address') return activeAddressSubscriptions(billing) > pending.addressesBefore;
  return billing.subscription?.entitled === true;
}

// The checkout, kept in this tab while the visitor is away (Stripe's return
// URL carries only its session id). A per-tab convenience: without it, a tier
// is assumed.
const CHECKOUT_KEY = 'opennova_checkout';

const ITEMS: ReadonlySet<unknown> = new Set<BillingItem>(['tier1', 'tier2', 'tier3', 'own_machine', 'static_address']);

export function rememberCheckout(item: BillingItem, ownMachinesBefore: number, addressesBefore: number): void {
  try {
    window.sessionStorage.setItem(
      CHECKOUT_KEY,
      JSON.stringify({ item, own_machines_before: ownMachinesBefore, addresses_before: addressesBefore }),
    );
  } catch {
    // Storage refused: the return assumes a tier.
  }
}

const countOr0 = (v: unknown): number => (Number.isInteger(v) ? Math.max(0, v as number) : 0);

export function recallCheckout(): PendingCheckout {
  const fallback: PendingCheckout = { item: 'tier1', ownMachinesBefore: 0, addressesBefore: 0 };
  try {
    const text = window.sessionStorage.getItem(CHECKOUT_KEY);
    window.sessionStorage.removeItem(CHECKOUT_KEY);
    if (text === null) return fallback;
    const v = JSON.parse(text) as { item?: unknown; own_machines_before?: unknown; addresses_before?: unknown } | null;
    if (!v || !ITEMS.has(v.item)) return fallback;
    return {
      item: v.item as BillingItem,
      ownMachinesBefore: countOr0(v.own_machines_before),
      addressesBefore: countOr0(v.addresses_before),
    };
  } catch {
    // Storage refused, or a value this page did not write: a tier is assumed.
    return fallback;
  }
}
