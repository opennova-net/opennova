// The deployment's AWS bill this month (GET /api/admin/costs, cost_routes.h).
// Answers an AdminResult (admin.ts); a reply missing what the page needs reads
// as malformed rather than as made-up numbers.

import { adminClient } from './client';
import { adminCall, type AdminResult } from './admin';
import type { CostBudget, CostDay, CostLine, Costs } from '../types/costs';

type Fields = Record<string, unknown>;

const isObject = (v: unknown): v is Fields => typeof v === 'object' && v !== null && !Array.isArray(v);
const isCents = (v: unknown): v is number => typeof v === 'number' && Number.isFinite(v);
const textOrNull = (v: unknown): string | null => (typeof v === 'string' && v !== '' ? v : null);
const centsOrNull = (v: unknown): number | null => (isCents(v) ? v : null);

function pickLine(v: unknown): CostLine | null {
  if (!isObject(v) || typeof v.service !== 'string' || !isCents(v.cents)) return null;
  return { service: v.service, cents: v.cents };
}

function pickDay(v: unknown): CostDay | null {
  if (!isObject(v) || typeof v.date !== 'string' || !isCents(v.cents)) return null;
  return { date: v.date, cents: v.cents };
}

function pickBudget(v: unknown): CostBudget | null {
  if (!isObject(v) || typeof v.name !== 'string' || !isCents(v.limit_cents)) return null;
  return {
    name: v.name,
    currency: typeof v.currency === 'string' && v.currency ? v.currency : 'USD',
    limit_cents: v.limit_cents,
    actual_cents: centsOrNull(v.actual_cents),
    forecast_cents: centsOrNull(v.forecast_cents),
  };
}

function pickList<T>(v: unknown, pick: (item: unknown) => T | null): T[] | null {
  if (!Array.isArray(v)) return [];
  const out: T[] = [];
  for (const item of v) {
    const picked = pick(item);
    if (picked === null) return null;
    out.push(picked);
  }
  return out;
}

export function pickCosts(v: unknown): Costs | null {
  if (!isObject(v) || typeof v.available !== 'boolean' || !isCents(v.month_to_date_cents)) return null;
  const byService = pickList(v.by_service, pickLine);
  const days = pickList(v.days, pickDay);
  if (byService === null || days === null) return null;
  const month =
    isObject(v.month) && typeof v.month.start === 'string' && typeof v.month.end === 'string'
      ? { start: v.month.start, end: v.month.end }
      : null;
  return {
    available: v.available,
    unavailable: textOrNull(v.unavailable),
    error: textOrNull(v.error),
    read_at: textOrNull(v.read_at),
    currency: typeof v.currency === 'string' && v.currency ? v.currency : 'USD',
    month,
    through: textOrNull(v.through),
    estimated: v.estimated === true,
    month_to_date_cents: v.month_to_date_cents,
    forecast_cents: centsOrNull(v.forecast_cents),
    forecast_error: textOrNull(v.forecast_error),
    by_service: byService,
    days,
    budget: v.budget === null || v.budget === undefined ? null : pickBudget(v.budget),
    budget_error: textOrNull(v.budget_error),
  };
}

// GET /api/admin/costs.
export function fetchCosts(signal?: AbortSignal): Promise<AdminResult<Costs>> {
  return adminCall(() => adminClient.get<unknown>('/costs', { signal }), pickCosts);
}
