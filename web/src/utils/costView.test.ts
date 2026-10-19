import { describe, expect, it } from 'vitest';
import { budgetState, costMoney, monthName, periodLabel, serviceLabel, topServices } from './costView';
import { costs } from '../test/costFixtures';

describe('the costs view', () => {
  it('writes amounts as a bill reads them, whole dollars from a hundred', () => {
    expect(costMoney(4125, 'USD')).toBe('$41.25');
    expect(costMoney(8, 'USD')).toBe('$0.08');
    expect(costMoney(11720, 'USD')).toBe('$117');
    expect(costMoney(120450, 'usd')).toBe('$1,205');
    expect(costMoney(-150, 'USD')).toBe('-$1.50');
  });

  it('names the month and the days read', () => {
    expect(monthName('2026-10-01')).toBe('October');
    expect(monthName(null)).toBe('');
    expect(periodLabel({ month: { start: '2026-10-01', end: '2026-11-01' }, through: '2026-10-19' })).toBe('October 1 to 19');
    expect(periodLabel({ month: { start: '2026-12-01', end: '2027-01-01' }, through: '2026-12-01' })).toBe('December 1');
    expect(periodLabel({ month: null, through: null })).toBe('');
  });

  it("shortens AWS's service names and adds up the rest", () => {
    expect(serviceLabel('Amazon Elastic Compute Cloud - Compute')).toBe('Elastic Compute Cloud - Compute');
    expect(serviceLabel('AWS Cost Explorer')).toBe('Cost Explorer');
    expect(serviceLabel('Tax')).toBe('Tax');
    const { top, rest } = topServices(costs().by_service, 3);
    expect(top.map((l) => l.cents)).toEqual([2200, 900, 600]);
    expect(rest).toEqual({ count: 2, cents: 425 });
    expect(topServices(costs().by_service, 8).rest).toBeNull();
  });

  it('says where the month stands against the budget, at the thresholds AWS emails at', () => {
    expect(budgetState(costs())).toMatchObject({ tone: 'ok', label: 'Within the budget', detail: "About $117 by the month's end, 47% of the $250 budget." });
    // No forecast from Cost Explorer: AWS Budgets' own.
    expect(budgetState(costs({ forecast_cents: null }))).toMatchObject({ tone: 'ok', detail: "About $118 by the month's end, 47% of the $250 budget." });
    expect(budgetState(costs({ forecast_cents: 26000 }))).toMatchObject({ tone: 'warn', label: 'On course to pass the budget' });
    expect(budgetState(costs({ month_to_date_cents: 20500, forecast_cents: 24000 }))).toMatchObject({ tone: 'warn', label: '80% of the budget spent' });
    expect(budgetState(costs({ month_to_date_cents: 25100, forecast_cents: 30000 }))).toMatchObject({
      tone: 'bad',
      label: 'Over the budget',
      detail: '$251 spent, 100% of the $250 budget.',
    });
    expect(budgetState(costs({ budget: null }))).toBeNull();
  });
});
