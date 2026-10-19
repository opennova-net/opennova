// The AWS costs as GET /api/admin/costs answers them (types/costs.ts): a
// deployment's October to the 19th, $41.25 so far, a $117.20 forecast against a
// $250 budget.

import type { Costs } from '../types/costs';

export function costs(overrides: Partial<Costs> = {}): Costs {
  return {
    available: true,
    unavailable: null,
    error: null,
    read_at: new Date(Date.now() - 2 * 3600_000).toISOString(),
    currency: 'USD',
    month: { start: '2026-10-01', end: '2026-11-01' },
    through: '2026-10-19',
    estimated: true,
    month_to_date_cents: 4125,
    forecast_cents: 11720,
    forecast_error: null,
    by_service: [
      { service: 'Amazon Elastic Container Service', cents: 2200 },
      { service: 'Amazon Elastic Load Balancing', cents: 900 },
      { service: 'Amazon Relational Database Service', cents: 600 },
      { service: 'AmazonCloudWatch', cents: 300 },
      { service: 'Amazon Simple Storage Service', cents: 125 },
    ],
    days: Array.from({ length: 19 }, (_, i) => ({
      date: `2026-10-${String(i + 1).padStart(2, '0')}`,
      cents: i === 4 ? 650 : i === 18 ? 75 : 200,
    })),
    budget: { name: 'opennova-prod-monthly-cost', currency: 'USD', limit_cents: 25000, actual_cents: 4100, forecast_cents: 11800 },
    budget_error: null,
    ...overrides,
  };
}

// The local stack's: nothing read.
export function localCosts(): Costs {
  return costs({
    available: false,
    unavailable: 'AWS costs are not read here: ONNET_INFRA_REGION is no region name',
    read_at: null,
    month: null,
    through: null,
    estimated: false,
    month_to_date_cents: 0,
    forecast_cents: null,
    by_service: [],
    days: [],
    budget: null,
  });
}
