import { mount } from '@vue/test-utils';
import { describe, expect, it } from 'vitest';
import CostSummary from './CostSummary.vue';
import { combinedCosts, costs, localCosts } from '../test/costFixtures';

function render(value = costs(), shown?: number) {
  return mount(CostSummary, { props: { costs: value, now: Date.now(), ...(shown ? { shown } : {}) } });
}

describe('CostSummary', () => {
  it('puts where the month stands against the budget first, in words beside the colour', () => {
    const page = render();
    const state = page.get('.cost-budget-state');
    expect(state.classes()).toContain('border-online/40');
    expect(state.text()).toContain('Within the budget');
    expect(state.text()).toContain("About $117 by the month's end, 47% of the $250 budget.");
    // The meter says what it shows to a screen reader, and names its marks in words.
    expect(page.get('.cost-meter').attributes('aria-label')).toBe(
      'Within the budget: $41.25 spent, about $117 forecast, against a budget of $250',
    );
    expect(state.text()).toContain('Solid: spent so far');
    expect(state.text()).toContain('Line: the budget');
  });

  it('shows the month so far, the forecast and the budget as tiles', () => {
    const tiles = render().findAll('.cost-tiles > div').map((t) => t.text());
    expect(tiles).toHaveLength(3);
    expect(tiles[0]).toContain('Spent so far');
    expect(tiles[0]).toContain('$41.25');
    expect(tiles[0]).toContain('October 1 to 19, UTC');
    expect(tiles[1]).toContain('Forecast for October');
    expect(tiles[1]).toContain('about $117');
    expect(tiles[2]).toContain('$250');
    expect(tiles[2]).toContain('AWS Budgets counts $41.00 spent');
  });

  it('shows a combined total and charts with separate production and CI budgets', () => {
    const page = render(combinedCosts());
    expect(page.get('.cost-scope').text()).toContain('Production + CI');
    const tiles = page.findAll('.cost-tiles > div').map((tile) => tile.text());
    expect(tiles).toHaveLength(2);
    expect(tiles[0]).toContain('$66.25');
    expect(tiles[1]).toContain('about $192');
    expect(page.find('.cost-budget-state').exists()).toBe(false);
    expect(page.findAll('.cost-service')).toHaveLength(6);
    expect(page.findAll('svg')).toHaveLength(1);
    const accounts = page.findAll('.cost-account');
    expect(accounts).toHaveLength(2);
    expect(accounts[0].text()).toContain('Production');
    expect(accounts[0].text()).toContain('47% of the $250 budget');
    expect(accounts[1].text()).toContain('CI');
    expect(accounts[1].text()).toContain('75% of the $100 budget');
    expect(accounts[1].text()).toContain('$25.00');
  });

  it.each([0, 1])('keeps successful account details when account %i cannot be read', (failed) => {
    const value = combinedCosts({ error: 'An account could not be read' });
    value.accounts[failed].error = 'AccessDeniedException';
    const page = render(value);
    expect(page.get('.cost-error').text()).toContain('Combined total unavailable');
    expect(page.find('.cost-tiles').exists()).toBe(false);
    expect(page.find('.cost-services').exists()).toBe(false);
    expect(page.find('svg').exists()).toBe(false);
    const accounts = page.findAll('.cost-account');
    expect(accounts[failed].get('.cost-account-error').text()).toContain('AccessDeniedException');
    expect(accounts[failed].find('dl').exists()).toBe(false);
    expect(accounts[1 - failed].find('dl').exists()).toBe(true);
  });

  it('shows both account errors without invented amounts when neither can be read', () => {
    const value = combinedCosts({ error: 'Production and CI could not be read' });
    for (const account of value.accounts) account.error = 'AccessDeniedException';
    const page = render(value);
    expect(page.findAll('.cost-account-error')).toHaveLength(2);
    expect(page.findAll('dl')).toHaveLength(0);
  });

  it('keeps an unavailable combined forecast separate from account budget forecasts', () => {
    const value = combinedCosts({ forecast_cents: null, forecast_error: 'CI: too little history' });
    value.accounts[1].forecast_cents = null;
    value.accounts[1].forecast_error = 'too little history';
    const page = render(value);
    expect(page.findAll('.cost-tiles > div')[1].text()).toContain('None yet');
    expect(page.findAll('.cost-tiles > div')[1].text()).toContain('CI: too little history');
    expect(page.findAll('.cost-account')[1].get('.cost-account-budget-state').text()).toContain('75% of the $100 budget');
  });

  it('formats each account budget in its own currency without a currency comparison', () => {
    const value = combinedCosts();
    value.accounts[1].budget!.currency = 'EUR';
    const account = render(value).findAll('.cost-account')[1];
    expect(account.find('.cost-account-budget-state').exists()).toBe(false);
    expect(account.text()).toContain('€100');
    expect(account.text()).toContain('different currencies');
  });

  it('preserves the single-account presentation when source metadata is present', () => {
    const value = costs({ accounts: combinedCosts().accounts.slice(0, 1) });
    const page = render(value);
    expect(page.findAll('.cost-tiles > div')).toHaveLength(3);
    expect(page.find('.cost-accounts').exists()).toBe(false);
    expect(page.get('.cost-budget-state').text()).toContain('Within the budget');
  });

  it('lists the largest services with a bar each and adds up the rest', () => {
    const page = render(costs(), 3);
    const lines = page.findAll('.cost-service').map((l) => l.text());
    expect(lines).toEqual(['Elastic Container Service$22.00', 'Elastic Load Balancing$9.00', 'Relational Database Service$6.00']);
    expect(page.get('.cost-services-rest').text()).toBe('And 2 more services: $4.25 together.');
    expect(render().find('.cost-services-rest').exists()).toBe(false);
  });

  it('shows the spend per day with its highest day', () => {
    const page = render();
    expect(page.get('svg').attributes('aria-label')).toBe('Spend per day, October 1 to 19: $0.75 now, peak $6.50');
    expect(page.text()).toContain('Highest $6.50 on October 5');
  });

  it('warns while the month is on course to pass the budget, and says when it is over', () => {
    const warn = render(costs({ forecast_cents: 26000 })).get('.cost-budget-state');
    expect(warn.classes()).toContain('border-warn/40');
    expect(warn.text()).toContain('On course to pass the budget');
    const over = render(costs({ month_to_date_cents: 25100, forecast_cents: 30000 })).get('.cost-budget-state');
    expect(over.classes()).toContain('border-danger/40');
    expect(over.text()).toContain('Over the budget');
  });

  it("says why there is no forecast or budget rather than inventing one", () => {
    const page = render(
      costs({ forecast_cents: null, forecast_error: 'ce answered 400: DataUnavailableException', budget: null, budget_error: 'budgets answered 400: AccessDeniedException' }),
    );
    expect(page.find('.cost-budget-state').exists()).toBe(false);
    expect(page.get('.cost-budget-error').text()).toBe('The budget could not be read: budgets answered 400: AccessDeniedException');
    const tiles = page.findAll('.cost-tiles > div').map((t) => t.text());
    expect(tiles[1]).toContain('None yet');
    expect(tiles[1]).toContain('AWS gave none: ce answered 400: DataUnavailableException');
    expect(tiles[2]).toContain('None read');
  });

  it('says nothing was spent yet rather than showing an empty list', () => {
    const page = render(costs({ month_to_date_cents: 0, by_service: [], days: [{ date: '2026-10-01', cents: 0 }] }));
    expect(page.get('.cost-no-spend').text()).toContain('No spend recorded yet this month');
    expect(page.find('svg').exists()).toBe(false);
  });

  it('shows no number where AWS is not read, or its read failed', () => {
    const local = render(localCosts());
    expect(local.get('.cost-unavailable').text()).toContain('AWS costs are not read on this stack.');
    expect(local.get('.cost-unavailable').text()).toContain('ONNET_INFRA_REGION is no region name.');
    expect(local.find('.cost-tiles').exists()).toBe(false);
    const failed = render(costs({ error: 'ce answered 400: AccessDeniedException: not authorized' }));
    expect(failed.get('.cost-error').text()).toBe('AWS costs could not be read: ce answered 400: AccessDeniedException: not authorized');
    expect(failed.find('.cost-tiles').exists()).toBe(false);
  });
});
