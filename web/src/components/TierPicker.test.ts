import { mount } from '@vue/test-utils';
import { describe, expect, it } from 'vitest';
import TierPicker from './TierPicker.vue';
import { tiersResponse } from '../test/billingFixtures';

const { tiers, currency } = tiersResponse();

describe('TierPicker', () => {
  it('shows each tier with its price and servers, and subscribes', async () => {
    const wrapper = mount(TierPicker, { props: { tiers, currency, mode: 'subscribe' } });
    const cards = wrapper.findAll('article');
    expect(cards).toHaveLength(3);
    expect(cards[0].text()).toContain('$5 a month');
    expect(cards[0].text()).toContain('Up to 1 hosted server at once');
    expect(cards[1].text()).toContain('$10 a month');
    expect(cards[2].text()).toContain('Up to 3 hosted servers at once');
    expect(cards.every((c) => c.attributes('aria-current') === undefined)).toBe(true);
    expect(cards.map((c) => c.get('button').text())).toEqual([
      'Subscribe: Tier 1',
      'Subscribe: Tier 2',
      'Subscribe: Tier 3',
    ]);
    await cards[1].get('button').trigger('click');
    expect(wrapper.emitted('choose')).toEqual([[2]]);
  });

  it("marks the account's tier and warns what a move down would stop", () => {
    const wrapper = mount(TierPicker, { props: { tiers, currency, mode: 'manage', currentTier: 3, used: 3 } });
    const cards = wrapper.findAll('article');
    expect(cards[2].attributes('aria-current')).toBe('true');
    expect(cards[2].text()).toContain('Your tier');
    expect(cards[2].get('button').text()).toBe('Manage: Tier 3');
    expect(cards[0].get('button').text()).toBe('Change: Tier 1');
    expect(cards[0].text()).toContain('Your 2 newest servers will be stopped when the change takes effect.');
    expect(cards[1].text()).toContain('Your newest server will be stopped when the change takes effect.');
    expect(cards[1].get('button').attributes('aria-describedby')).toBe('support-tier-2-note');
    // Nothing to stop when the live servers fit.
    const fits = mount(TierPicker, { props: { tiers, currency, mode: 'manage', currentTier: 3, used: 1 } });
    expect(fits.text()).not.toContain('will be stopped');
  });

  it('pauses a tier the fleet is sold out of, but keeps Manage on the account\'s own', async () => {
    const full = tiers.map((t) => ({ ...t, available: t.tier === 1 }));
    const wrapper = mount(TierPicker, { props: { tiers: full, currency, mode: 'subscribe' } });
    const cards = wrapper.findAll('article');
    expect(cards[0].get('button').attributes('disabled')).toBeUndefined();
    expect(cards[0].text()).not.toContain('Hosting is full');
    expect(cards[1].get('button').attributes('disabled')).toBeDefined();
    expect(cards[1].text()).toContain('Hosting is full: new subscriptions are paused.');
    expect(cards[1].get('button').attributes('aria-describedby')).toBe('support-tier-2-full');
    await cards[1].get('button').trigger('click');
    expect(wrapper.emitted('choose')).toBeUndefined();

    const manage = mount(TierPicker, { props: { tiers: full, currency, mode: 'manage', currentTier: 3, used: 3 } });
    const mine = manage.findAll('article');
    expect(mine[2].get('button').attributes('disabled')).toBeUndefined();
    expect(mine[2].text()).not.toContain('Hosting is full');
    // A move down that would stop servers says both.
    expect(mine[1].get('button').attributes('aria-describedby')).toBe('support-tier-2-note support-tier-2-full');
  });

  it('cannot pick a tier whose price is not set', () => {
    const wrapper = mount(TierPicker, {
      props: { tiers: [{ ...tiers[0], amount: null }], currency, mode: 'subscribe' },
    });
    expect(wrapper.text()).toContain('Price not set yet');
    expect(wrapper.get('button').attributes('disabled')).toBeDefined();
  });
});
