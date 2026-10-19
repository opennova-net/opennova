// The admin billing page's monthly total (an estimate): the subscriptions
// that grant hosting now, by tier, at the tiers' prices today, plus the
// dedicated machine add-on by quantity and, where the deployment sells them, the
// static addresses. Not what Stripe pays out (no fees, no tax, no proration).

import type { AdminSubscription, TiersResponse } from '../types/billing';

export interface RevenueRow {
  label: string;
  // Subscribers (or dedicated machines, for the add-on's row).
  count: number;
  // The price a month, null when the tiers did not name one.
  price: number | null;
  total: number;
}

export interface Revenue {
  rows: RevenueRow[];
  // Accounts on a tier.
  subscribers: number;
  total: number;
}

export function billingRevenue(subscriptions: readonly AdminSubscription[], tiers: TiersResponse | null): Revenue {
  const granting = subscriptions.filter((s) => s.entitled);
  const rows: RevenueRow[] = ([1, 2, 3] as const).map((tier) => {
    const count = granting.filter((s) => s.item === 'tier' && s.tier === tier).length;
    const price = tiers?.tiers.find((t) => t.tier === tier)?.amount ?? null;
    return { label: `Tier ${tier}`, count, price, total: price === null ? 0 : price * count };
  });
  const machines = granting.filter((s) => s.item === 'own_machine').reduce((n, s) => n + s.quantity, 0);
  const machinePrice = tiers?.own_machine?.amount ?? null;
  rows.push({
    label: 'Dedicated machines',
    count: machines,
    price: machinePrice,
    total: machinePrice === null ? 0 : machinePrice * machines,
  });
  const addresses = granting.filter((s) => s.item === 'static_address').length;
  if (tiers?.static_address || addresses > 0) {
    const addressPrice = tiers?.static_address?.amount ?? null;
    rows.push({
      label: 'Static addresses',
      count: addresses,
      price: addressPrice,
      total: addressPrice === null ? 0 : addressPrice * addresses,
    });
  }
  return {
    rows,
    subscribers: rows.slice(0, 3).reduce((n, r) => n + r.count, 0),
    total: rows.reduce((n, r) => n + r.total, 0),
  };
}
