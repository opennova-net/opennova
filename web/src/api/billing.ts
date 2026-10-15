// Support tiers and the account's billing (apps/novaworld_server/billing/,
// README "Support tiers"): GET /api/billing/tiers (public), GET
// /api/me/billing, POST /api/billing/checkout, POST /api/billing/portal and
// POST /api/me/billing/sync. The service creates the Stripe Checkout and
// billing portal sessions and answers their `{url}`; the page then leaves for
// Stripe through utils/navigate.ts. No Stripe key or Stripe.js reaches the
// browser.
//
// Each call answers a BillingResult instead of throwing, with the server's
// `error` code mapped to a message: the server's own `message` text is never
// shown.

import { isAxiosError } from 'axios';
import { apiClient } from './client';
import { formatRetryAfter, parseRetryAfter } from './session';
import type {
  BillingItem,
  MyBilling,
  OwnMachineOffer,
  OwnMachineSubscription,
  TierNumber,
  TierOffer,
  TierSubscription,
  TiersResponse,
} from '../types/billing';

// The server's `error` codes these routes answer with, plus `network` (no
// reply at all) and `unknown` (a reply this client does not recognise).
export type BillingErrorCode =
  | 'unauthorized'
  | 'csrf_header_required'
  | 'proxy_required'
  | 'billing_disabled'
  | 'billing_misconfigured'
  | 'rate_limited'
  | 'invalid_json'
  | 'invalid_item'
  | 'not_eligible'
  | 'already_subscribed'
  | 'own_machine_limit'
  | 'no_billing_account'
  | 'stripe_unavailable'
  | 'db_error'
  | 'network'
  | 'unknown';

export interface BillingError {
  code: BillingErrorCode;
  // The HTTP status, or null when no reply arrived.
  status: number | null;
  message: string;
  // A 429's Retry-After, in seconds.
  retryAfterSeconds?: number;
}

export type BillingResult<T> = { ok: true; value: T } | { ok: false; error: BillingError };

const KNOWN_CODES: ReadonlySet<string> = new Set<BillingErrorCode>([
  'unauthorized',
  'csrf_header_required',
  'proxy_required',
  'billing_disabled',
  'billing_misconfigured',
  'rate_limited',
  'invalid_json',
  'invalid_item',
  'not_eligible',
  'already_subscribed',
  'own_machine_limit',
  'no_billing_account',
  'stripe_unavailable',
  'db_error',
]);

function messageFor(code: BillingErrorCode, retryAfterSeconds?: number): string {
  switch (code) {
    case 'unauthorized':
      return 'Your session has ended. Log in again to manage your support tier.';
    case 'csrf_header_required':
      return 'The server refused the request as not coming from this site. Reload the page and try again.';
    case 'proxy_required':
      return 'The server refused a request that did not come through this site. Reload the page and try again.';
    case 'billing_disabled':
      return 'Support tiers are not open on NovaWorld yet.';
    case 'billing_misconfigured':
      return 'Support tiers are paused while their prices are checked. Try again later; no payment was taken.';
    case 'rate_limited':
      return retryAfterSeconds !== undefined
        ? `Too many requests. Try again in ${formatRetryAfter(retryAfterSeconds)}.`
        : 'Too many requests. Try again in a little while.';
    case 'invalid_json':
    case 'invalid_item':
      return 'The server could not read the request. Reload the page and try again.';
    case 'not_eligible':
      return 'Your account cannot buy that right now: an own machine needs a support tier first, and an account that is not active cannot subscribe.';
    case 'already_subscribed':
      return 'You already have a support tier. Change it in the billing portal.';
    case 'own_machine_limit':
      return 'You already have an own machine for every server your support tier covers. A higher tier makes room for more.';
    case 'no_billing_account':
      return 'You have no billing account yet: subscribe to a support tier first.';
    case 'stripe_unavailable':
      return 'Stripe did not answer. Try again in a moment; no payment was taken.';
    case 'db_error':
      return 'The server hit a database error. Try again shortly.';
    case 'network':
      return 'Could not reach the server. Check your connection and try again.';
    case 'unknown':
      return 'Something went wrong. Try again shortly.';
  }
}

// Any failure of these calls, as a BillingError.
export function describeBillingError(err: unknown): BillingError {
  if (isAxiosError(err)) {
    const response = err.response;
    if (!response) return { code: 'network', status: null, message: messageFor('network') };
    const data = response.data as { error?: unknown } | undefined;
    const serverCode = typeof data?.error === 'string' ? data.error : '';
    const code: BillingErrorCode = KNOWN_CODES.has(serverCode)
      ? (serverCode as BillingErrorCode)
      : response.status === 429
        ? 'rate_limited'
        : response.status === 401
          ? 'unauthorized'
          : 'unknown';
    const retryAfterSeconds =
      code === 'rate_limited' ? parseRetryAfter(response.headers?.['retry-after']) : undefined;
    return {
      code,
      status: response.status,
      message:
        code === 'unknown' ? `${messageFor('unknown')} (HTTP ${response.status})` : messageFor(code, retryAfterSeconds),
      ...(retryAfterSeconds !== undefined ? { retryAfterSeconds } : {}),
    };
  }
  return { code: 'unknown', status: null, message: messageFor('unknown') };
}

function malformed(status: number): BillingResult<never> {
  return { ok: false, error: { code: 'unknown', status, message: messageFor('unknown') } };
}

const isCount = (v: unknown): v is number => Number.isInteger(v) && (v as number) >= 0;
const isAmount = (v: unknown): v is number | null => v === null || isCount(v);
const isTier = (v: unknown): v is TierNumber => v === 1 || v === 2 || v === 3;

function parseTier(value: unknown): TierOffer | null {
  const v = value as Record<string, unknown> | null;
  if (typeof v !== 'object' || v === null) return null;
  if (!isTier(v.tier) || !isCount(v.max_servers) || !isAmount(v.amount) || v.interval !== 'month') return null;
  return { tier: v.tier, max_servers: v.max_servers, amount: v.amount, interval: 'month' };
}

function parseOwnMachineOffer(value: unknown): OwnMachineOffer | null | undefined {
  if (value === null || value === undefined) return null;
  const v = value as Record<string, unknown>;
  if (typeof v !== 'object' || !isAmount(v.amount) || typeof v.interval !== 'string') return undefined;
  return { amount: v.amount, interval: v.interval };
}

// The tiers, in order, each once; a reply naming a malformed tier is refused
// whole (the page never shows a price it could not read).
export function parseTiers(value: unknown): TiersResponse | null {
  const v = value as Record<string, unknown> | null;
  if (typeof v !== 'object' || v === null) return null;
  if (typeof v.enabled !== 'boolean' || typeof v.currency !== 'string' || !Array.isArray(v.tiers)) return null;
  const tiers = v.tiers.map(parseTier);
  if (tiers.some((t) => t === null)) return null;
  const ownMachine = parseOwnMachineOffer(v.own_machine);
  if (ownMachine === undefined) return null;
  const sorted = (tiers as TierOffer[]).slice().sort((a, b) => a.tier - b.tier);
  if (sorted.some((t, i) => i > 0 && sorted[i - 1].tier === t.tier)) return null;
  return { enabled: v.enabled, currency: v.currency, tiers: sorted, own_machine: ownMachine };
}

const isTime = (v: unknown): v is string => typeof v === 'string' && !Number.isNaN(Date.parse(v));

function parseSubscription(value: unknown): TierSubscription | null | undefined {
  if (value === null) return null;
  const v = value as Record<string, unknown> | undefined;
  if (typeof v !== 'object' || v === null) return undefined;
  if (!isTier(v.tier) || typeof v.status !== 'string' || typeof v.entitled !== 'boolean') return undefined;
  if (!isTime(v.current_period_end) || typeof v.cancel_at_period_end !== 'boolean') return undefined;
  return {
    tier: v.tier,
    status: v.status,
    entitled: v.entitled,
    current_period_end: v.current_period_end,
    cancel_at_period_end: v.cancel_at_period_end,
  };
}

function parseOwnMachineSubscription(value: unknown): OwnMachineSubscription | null | undefined {
  if (value === null || value === undefined) return null;
  const v = value as Record<string, unknown>;
  if (typeof v !== 'object' || !isCount(v.quantity) || typeof v.status !== 'string') return undefined;
  if (!isTime(v.current_period_end) || typeof v.cancel_at_period_end !== 'boolean') return undefined;
  return {
    quantity: v.quantity,
    status: v.status,
    current_period_end: v.current_period_end,
    cancel_at_period_end: v.cancel_at_period_end,
  };
}

export function parseMyBilling(value: unknown): MyBilling | null {
  const v = value as Record<string, unknown> | null;
  if (typeof v !== 'object' || v === null) return null;
  const { enabled, max_servers: max, comp_servers: comp, own_machines: own, used, stopping } = v;
  if (typeof enabled !== 'boolean' || !isCount(max) || !isCount(comp) || !isCount(own) || !isCount(used)) return null;
  if (!isCount(stopping)) return null;
  if (typeof v.can_start !== 'boolean' || typeof v.can_manage !== 'boolean' || typeof v.can_subscribe !== 'boolean') {
    return null;
  }
  const subscription = parseSubscription(v.subscription);
  const ownMachine = parseOwnMachineSubscription(v.own_machine_subscription);
  if (subscription === undefined || ownMachine === undefined) return null;
  return {
    enabled,
    max_servers: max,
    comp_servers: comp,
    own_machines: own,
    used,
    stopping,
    can_start: v.can_start,
    subscription,
    own_machine_subscription: ownMachine,
    can_manage: v.can_manage,
    can_subscribe: v.can_subscribe,
  };
}

async function call<T>(
  request: () => Promise<{ status: number; data: unknown }>,
  pick: (data: unknown) => T | null,
): Promise<BillingResult<T>> {
  try {
    const res = await request();
    const value = pick(res.data);
    if (value === null) return malformed(res.status);
    return { ok: true, value };
  } catch (err) {
    return { ok: false, error: describeBillingError(err) };
  }
}

function pickUrl(data: unknown): string | null {
  const url = (data as { url?: unknown } | null)?.url;
  return typeof url === 'string' && url !== '' ? url : null;
}

// GET /api/billing/tiers: the tiers and the own-machine add-on, with their
// prices; public.
export function fetchTiers(signal?: AbortSignal): Promise<BillingResult<TiersResponse>> {
  return call(() => apiClient.get<unknown>('/billing/tiers', { signal }), parseTiers);
}

// GET /api/me/billing: the account's entitlement and subscriptions.
export function fetchMyBilling(signal?: AbortSignal): Promise<BillingResult<MyBilling>> {
  return call(() => apiClient.get<unknown>('/me/billing', { signal }), parseMyBilling);
}

// POST /api/me/billing/sync: the account's subscriptions read from Stripe
// now (after a checkout), then as GET /api/me/billing.
export function syncMyBilling(): Promise<BillingResult<MyBilling>> {
  return call(() => apiClient.post<unknown>('/me/billing/sync'), parseMyBilling);
}

// POST /api/billing/checkout: a Stripe Checkout session for `item`; its URL.
// 409 already_subscribed while a tier is live (change it in the portal), 409
// own_machine_limit for the add-on once every server the account may run has
// an own machine.
export function startCheckout(item: BillingItem): Promise<BillingResult<string>> {
  return call(() => apiClient.post<unknown>('/billing/checkout', { item }), pickUrl);
}

// POST /api/billing/portal: a Stripe billing portal session; its URL.
export function openPortal(): Promise<BillingResult<string>> {
  return call(() => apiClient.post<unknown>('/billing/portal'), pickUrl);
}

// The checkout item for a tier.
export function tierItem(tier: TierNumber): BillingItem {
  return `tier${tier}` as BillingItem;
}
