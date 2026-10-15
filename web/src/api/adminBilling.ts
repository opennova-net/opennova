// The admin's billing overview (apps/novaworld_server/billing/billing_routes.cpp):
// GET /api/admin/billing (with the hosted servers' limits hold, billing on or
// off), POST /api/admin/billing/reconcile (and its override of the hold) and
// POST /api/admin/billing/link. Each answers an AdminResult (admin.ts).

import { adminClient } from './client';
import { adminCall, adminPost, type AdminResult } from './admin';
import { pickSubscription } from './users';
import type { AdminBillingResponse, BillingEvent, BillingOrphan, BillingSubscriptionRow } from '../types/billing';

const str = (v: unknown): string => (typeof v === 'string' ? v : '');
const strOrNull = (v: unknown): string | null => (typeof v === 'string' && v !== '' ? v : null);
const count = (v: unknown): number => (Number.isInteger(v) && (v as number) >= 0 ? (v as number) : 0);

function pickRow(value: unknown): BillingSubscriptionRow | null {
  const sub = pickSubscription(value);
  const v = value as Record<string, unknown>;
  if (!sub || typeof v.user_id !== 'number') return null;
  return { ...sub, user_id: v.user_id, username: str(v.username), stripe_customer_id: str(v.stripe_customer_id) };
}

function pickOrphan(value: unknown): BillingOrphan | null {
  const v = value as Record<string, unknown> | null;
  if (typeof v !== 'object' || v === null || typeof v.stripe_customer_id !== 'string') return null;
  return {
    stripe_customer_id: v.stripe_customer_id,
    stripe_subscription_id: str(v.stripe_subscription_id),
    username_hint: str(v.username_hint),
    status: str(v.status),
  };
}

function pickEvent(value: unknown): BillingEvent | null {
  const v = value as Record<string, unknown> | null;
  if (typeof v !== 'object' || v === null || typeof v.event_id !== 'string') return null;
  return { event_id: v.event_id, type: str(v.type), received_at: str(v.received_at), outcome: str(v.outcome) };
}

function list<T>(value: unknown, pick: (v: unknown) => T | null): T[] | null {
  if (!Array.isArray(value)) return null;
  const out = value.map(pick);
  return out.some((x) => x === null) ? null : (out as T[]);
}

export function parseAdminBilling(data: unknown): AdminBillingResponse | null {
  const v = data as Record<string, unknown> | null;
  if (typeof v !== 'object' || v === null || typeof v.enabled !== 'boolean') return null;
  const subscriptions = list(v.subscriptions ?? [], pickRow);
  const orphans = list(v.orphans ?? [], pickOrphan);
  const events = list(v.recent_events ?? [], pickEvent);
  if (!subscriptions || !orphans || !events) return null;
  return {
    enabled: v.enabled,
    limits_hold: v.limits_hold === true,
    limits_hold_reason: strOrNull(v.limits_hold_reason),
    limits_hold_owners: count(v.limits_hold_owners),
    limits_hold_servers: count(v.limits_hold_servers),
    mode: v.mode === 'live' ? 'live' : 'test',
    prices_ok: v.prices_ok === true,
    last_reconcile_at: strOrNull(v.last_reconcile_at),
    last_reconcile_error: strOrNull(v.last_reconcile_error),
    pending_events: Number.isInteger(v.pending_events) ? (v.pending_events as number) : 0,
    subscriptions,
    duplicates: Array.isArray(v.duplicates) ? v.duplicates.filter((d): d is number => typeof d === 'number') : [],
    orphans,
    recent_events: events,
  };
}

// GET /api/admin/billing.
export function fetchAdminBilling(signal?: AbortSignal): Promise<AdminResult<AdminBillingResponse>> {
  return adminCall(() => adminClient.get<unknown>('/billing', { signal }), parseAdminBilling);
}

// POST /api/admin/billing/reconcile: a full read of Stripe now (202; it runs
// in the background).
export function reconcileBilling(): Promise<AdminResult<void>> {
  return adminPost('/billing/reconcile');
}

// POST /api/admin/billing/reconcile {"force_limits": true}: the read, and the
// supervisor's next pass applies the stops the limits hold keeps back (once).
// With billing off only the stops apply.
export function applyLimitStops(): Promise<AdminResult<void>> {
  return adminPost('/billing/reconcile', { force_limits: true });
}

// "3 servers of 2 owners": the stops the limits hold keeps back.
export function limitsHoldSummary(billing: Pick<AdminBillingResponse, 'limits_hold_owners' | 'limits_hold_servers'>): string {
  const servers = billing.limits_hold_servers;
  const owners = billing.limits_hold_owners;
  return `${servers} server${servers === 1 ? '' : 's'} of ${owners} owner${owners === 1 ? '' : 's'}`;
}

// POST /api/admin/billing/link: a Stripe customer (an orphan's) linked to an
// account.
export function linkBillingCustomer(userId: number, stripeCustomerId: string): Promise<AdminResult<void>> {
  return adminPost('/billing/link', { user_id: userId, stripe_customer_id: stripeCustomerId });
}

// The Stripe dashboard's page of a customer or subscription, in the key's
// mode.
export function stripeDashboardUrl(kind: 'customers' | 'subscriptions', id: string, live: boolean): string {
  return `https://dashboard.stripe.com/${live ? '' : 'test/'}${kind}/${encodeURIComponent(id)}`;
}
