// The AWS costs' words (components/CostSummary.vue, pages/admin/CostsSection.vue,
// the Overview): amounts as a bill reads them, the month and the days read, AWS's
// service names shortened, and where the month stands against the budget in a
// tone and words, never a colour alone.

import type { CostLine, Costs } from '../types/costs';
import type { Tone } from './infraView';

// "$41.52", "$0.08", "$1,204": cents as money, whole units from 100 up (a
// bill of hundreds reads in dollars).
export function costMoney(cents: number, currency: string): string {
  const whole = Math.abs(cents) >= 10_000;
  const code = currency.toUpperCase();
  try {
    return new Intl.NumberFormat('en-US', {
      style: 'currency',
      currency: code,
      minimumFractionDigits: whole ? 0 : 2,
      maximumFractionDigits: whole ? 0 : 2,
    }).format(whole ? Math.round(cents / 100) : cents / 100);
  } catch {
    return `${(cents / 100).toFixed(whole ? 0 : 2)} ${code}`;
  }
}

const MONTHS = [
  'January',
  'February',
  'March',
  'April',
  'May',
  'June',
  'July',
  'August',
  'September',
  'October',
  'November',
  'December',
];

// "October": a YYYY-MM-DD date's month; '' when it reads as none.
export function monthName(date: string | null): string {
  const m = /^\d{4}-(\d{2})-\d{2}$/.exec(date ?? '');
  const index = m ? Number(m[1]) - 1 : -1;
  return MONTHS[index] ?? '';
}

// "October 1 to 19", "October 1": the days the month so far covers.
export function periodLabel(costs: Pick<Costs, 'month' | 'through'>): string {
  const month = monthName(costs.month?.start ?? null);
  const through = /^\d{4}-\d{2}-(\d{2})$/.exec(costs.through ?? '');
  if (!month || !through) return '';
  const last = Number(through[1]);
  return last === 1 ? `${month} 1` : `${month} 1 to ${last}`;
}

// "Elastic Compute Cloud - Compute": AWS's name without the vendor's word.
export function serviceLabel(service: string): string {
  return service.replace(/^(Amazon|AWS)\s+/, '') || service;
}

// The largest `shown` services, and the rest added up (null when none is left).
export function topServices(lines: readonly CostLine[], shown: number): { top: CostLine[]; rest: { count: number; cents: number } | null } {
  const top = lines.slice(0, shown);
  const others = lines.slice(shown);
  return {
    top,
    rest: others.length ? { count: others.length, cents: others.reduce((n, l) => n + l.cents, 0) } : null,
  };
}

export interface BudgetState {
  tone: Tone;
  // What it is, in two or three words: "Within the budget".
  label: string;
  // The numbers behind it, in a sentence.
  detail: string;
  // The month so far and the forecast as shares of the budget (0 to 1, past 1 over it).
  spentShare: number;
  forecastShare: number | null;
}

function share(cents: number, limit: number): number {
  return limit > 0 ? cents / limit : 0;
}

function percent(fraction: number): string {
  return `${Math.round(fraction * 100)}%`;
}

// Where the month stands against the budget, at the thresholds AWS emails at
// (80% spent, a forecast past 100%): over it (danger), on course to pass it or
// 80% spent (warn), else within it. Null without a budget to hold it against.
export function budgetState(costs: Pick<Costs, 'month_to_date_cents' | 'forecast_cents' | 'budget' | 'currency'>): BudgetState | null {
  const budget = costs.budget;
  if (!budget || budget.limit_cents <= 0) return null;
  const limit = budget.limit_cents;
  const spent = costs.month_to_date_cents;
  const forecast = costs.forecast_cents ?? budget.forecast_cents;
  const money = (cents: number) => costMoney(cents, costs.currency);
  const spentShare = share(spent, limit);
  const forecastShare = forecast === null ? null : share(forecast, limit);
  const of = `of the ${money(limit)} budget`;
  if (spent >= limit) {
    return { tone: 'bad', label: 'Over the budget', detail: `${money(spent)} spent, ${percent(spentShare)} ${of}.`, spentShare, forecastShare };
  }
  if (forecast !== null && forecast > limit) {
    return {
      tone: 'warn',
      label: 'On course to pass the budget',
      detail: `About ${money(forecast)} by the month's end, ${percent(forecastShare!)} ${of}.`,
      spentShare,
      forecastShare,
    };
  }
  if (spentShare >= 0.8) {
    return { tone: 'warn', label: '80% of the budget spent', detail: `${money(spent)} spent, ${percent(spentShare)} ${of}.`, spentShare, forecastShare };
  }
  return {
    tone: 'ok',
    label: 'Within the budget',
    detail:
      forecast === null
        ? `${money(spent)} spent so far, ${percent(spentShare)} ${of}.`
        : `About ${money(forecast)} by the month's end, ${percent(forecastShare!)} ${of}.`,
    spentShare,
    forecastShare,
  };
}
