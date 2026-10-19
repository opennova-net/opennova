// The deployment's AWS bill this month as GET /api/admin/costs answers it
// (apps/novaworld_web/infra/cost_routes.h): what its account is charged, from
// Cost Explorer and its monthly budget, read by web at most every 6 hours.
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

export interface Costs {
  // False where AWS is not read (the local stack); `unavailable` says why.
  available: boolean;
  unavailable: string | null;
  // Why Cost Explorer's read failed; null when it did not.
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
}
