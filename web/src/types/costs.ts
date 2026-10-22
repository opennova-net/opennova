// The configured AWS accounts' bill this month as GET /api/admin/costs answers
// it (apps/novaworld_web/infra/cost_routes.h), read at most every 6 hours.
// Amounts are cents of `currency`; dates are YYYY-MM-DD in UTC.

export interface CostLine {
  // AWS's name for the service ("Amazon Elastic Compute Cloud - Compute").
  service: string;
  cents: number;
}

export interface CostDay {
  date: string;
  cents: number;
}

export interface CostBudget {
  name: string;
  currency: string;
  limit_cents: number;
  // AWS Budgets' own figures for the month; null until it has computed them.
  actual_cents: number | null;
  forecast_cents: number | null;
}

export interface CostAccount {
  id: string;
  label: string;
  currency: string;
  // A failed account has no usable amounts; never display its zero as spend.
  error: string | null;
  month_to_date_cents: number;
  forecast_cents: number | null;
  forecast_error: string | null;
  budget: CostBudget | null;
  budget_error: string | null;
}

export interface Costs {
  // False where AWS is not read (the local stack); `unavailable` says why.
  available: boolean;
  unavailable: string | null;
  // Why the combined read failed; null only when every account was read.
  error: string | null;
  // When web read AWS (ISO 8601); null when it never did.
  read_at: string | null;
  // ISO 4217, upper case as AWS writes it ('USD').
  currency: string;
  // The month read: its first day and the next month's (exclusive).
  month: { start: string; end: string } | null;
  // The last day read (today when it was read).
  through: string | null;
  // Cost Explorer's figures are still open (the month is not closed).
  estimated: boolean;
  month_to_date_cents: number;
  // The month's expected total; null when AWS gave no forecast.
  forecast_cents: number | null;
  forecast_error: string | null;
  // Largest first.
  by_service: CostLine[];
  // Every day of the month so far, oldest first.
  days: CostDay[];
  budget: CostBudget | null;
  budget_error: string | null;
  // Account budgets are separate; the combined bill has no aggregate budget.
  // Empty for older servers and the local stack.
  accounts: CostAccount[];
}
