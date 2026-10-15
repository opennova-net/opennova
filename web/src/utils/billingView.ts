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

// What a checkout went to Stripe for: the item, and for the add-on the own
// machines the account had before it left (each own machine is a
// subscription of its own, so the return waits for one more).
export interface PendingCheckout {
  item: BillingItem;
  ownMachinesBefore: number;
}

// A checkout that came back successful is confirmed once Stripe's word has
// reached NovaWorld: the tier grants hosting, or the account has more own
// machines than before, its add-on active.
export function checkoutConfirmed(billing: MyBilling, pending: PendingCheckout): boolean {
  if (pending.item === 'own_machine') {
    const own = billing.own_machine_subscription;
    return (
      !!own && (own.status === 'active' || own.status === 'trialing') && billing.own_machines > pending.ownMachinesBefore
    );
  }
  return billing.subscription?.entitled === true;
}

// The checkout, kept in this tab while the visitor is away (Stripe's return
// URL carries only its session id). A per-tab convenience: without it, a tier
// is assumed.
const CHECKOUT_KEY = 'opennova_checkout';

const ITEMS: ReadonlySet<unknown> = new Set<BillingItem>(['tier1', 'tier2', 'tier3', 'own_machine']);

export function rememberCheckout(item: BillingItem, ownMachinesBefore: number): void {
  try {
    window.sessionStorage.setItem(CHECKOUT_KEY, JSON.stringify({ item, own_machines_before: ownMachinesBefore }));
  } catch {
    // Storage refused: the return assumes a tier.
  }
}

export function recallCheckout(): PendingCheckout {
  const fallback: PendingCheckout = { item: 'tier1', ownMachinesBefore: 0 };
  try {
    const text = window.sessionStorage.getItem(CHECKOUT_KEY);
    window.sessionStorage.removeItem(CHECKOUT_KEY);
    if (text === null) return fallback;
    const v = JSON.parse(text) as { item?: unknown; own_machines_before?: unknown } | null;
    if (!v || !ITEMS.has(v.item)) return fallback;
    const before = Number.isInteger(v.own_machines_before) ? Math.max(0, v.own_machines_before as number) : 0;
    return { item: v.item as BillingItem, ownMachinesBefore: before };
  } catch {
    // Storage refused, or a value this page did not write: a tier is assumed.
    return fallback;
  }
}
