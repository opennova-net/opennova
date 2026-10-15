// What the support tier pages show of the account's billing (types/billing.ts):
// the status in words, whether a tier is live, and what a checkout that came
// back has to wait for.

import type { BillingItem, MyBilling, SubscriptionStatus } from '../types/billing';

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

// The statuses after which a subscription is over: a new checkout may start.
const ENDED: ReadonlySet<string> = new Set(['canceled', 'incomplete_expired']);

// The account has a support tier subscription that is not over (it may be
// past due): the tiers are managed in the billing portal, not bought again.
export function hasLiveTier(billing: MyBilling | null): boolean {
  const sub = billing?.subscription;
  return !!sub && !ENDED.has(sub.status);
}

// A checkout of `item` that came back successful is confirmed once Stripe's
// word has reached NovaWorld: the tier grants hosting, or the own-machine
// add-on is active.
export function checkoutConfirmed(billing: MyBilling, item: BillingItem): boolean {
  if (item === 'own_machine') {
    const own = billing.own_machine_subscription;
    return !!own && (own.status === 'active' || own.status === 'trialing') && billing.own_machines > 0;
  }
  return billing.subscription?.entitled === true;
}

// The item a checkout went to Stripe for, kept in this tab while the visitor
// is away (Stripe's return URL carries only its session id). A per-tab
// convenience: without it, a tier is assumed.
const CHECKOUT_ITEM_KEY = 'opennova_checkout_item';

export function rememberCheckoutItem(item: BillingItem): void {
  try {
    window.sessionStorage.setItem(CHECKOUT_ITEM_KEY, item);
  } catch {
    // Storage refused: the return assumes a tier.
  }
}

export function recallCheckoutItem(): BillingItem {
  try {
    const item = window.sessionStorage.getItem(CHECKOUT_ITEM_KEY);
    window.sessionStorage.removeItem(CHECKOUT_ITEM_KEY);
    if (item === 'tier1' || item === 'tier2' || item === 'tier3' || item === 'own_machine') return item;
  } catch {
    // Storage refused: a tier is assumed.
  }
  return 'tier1';
}
