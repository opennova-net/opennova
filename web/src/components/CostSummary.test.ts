import { mount } from '@vue/test-utils';
import { describe, expect, it } from 'vitest';
import CostSummary from './CostSummary.vue';
import { costs, localCosts } from '../test/costFixtures';

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
