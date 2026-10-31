// Who sees the admin's money: the Billing section (subscriptions, static
// addresses, AWS costs), the Overview's Subscribers and AWS tiles and its
// limits-hold notice, a player's Stripe customer and subscriptions, the
// accounts' Subscription column, and what needs an admin that opens the
// Billing section. Only the admin session of the account `biggy` does; any
// other admin, and the Bearer admin token without that session, sees none of
// it. A site-only hide: the /api/admin routes still answer every admin.

import type { SessionUser } from '../api/session';
import type { Attention } from '../types/attention';
import { alarmDestination } from './alarmLinks';

export const BILLING_ADMIN = 'biggy';

export function seesBilling(user: SessionUser | null | undefined): boolean {
  return user?.role === 'admin' && user.username === BILLING_ADMIN;
}

// Whether `path` (a query or hash allowed) is a page of the Billing section.
export function isBillingPath(path: string): boolean {
  const clean = path.split(/[?#]/)[0].replace(/\/$/, '');
  return clean === '/admin/billing' || clean.startsWith('/admin/billing/');
}

// What needs an admin, less what opens the Billing section: its findings
// (the static addresses', the oversold capacity's) and its alarms (the
// billing sync's, the webhook's, the limits hold's).
export function withoutBilling(a: Attention): Attention {
  return {
    ...a,
    findings: a.findings.filter((f) => !f.link || !isBillingPath(f.link)),
    alarms: a.alarms.filter((alarm) => !isBillingPath(alarmDestination(alarm.name).to)),
  };
}
