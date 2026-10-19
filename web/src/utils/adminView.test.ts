import { describe, expect, it } from 'vitest';
import { entitlementLabel, ownerHostingLabel, serverPlacesLabel, soldCapacityText } from './adminView';

describe('entitlementLabel', () => {
  const none = { source: 'none' as const, tier: null, comp_servers: 0, comp_own_machines: 0, own_machines: 0 };

  it('names each source an account has, the one that gives its servers on shared machines first', () => {
    expect(entitlementLabel({ ...none, source: 'tier', tier: 2 })).toBe('Tier 2');
    expect(entitlementLabel({ ...none, source: 'comp', comp_servers: 3 })).toBe('Admin grant of 3 shared');
    // A grant bigger than the tier still billing beside it, and one no bigger.
    expect(entitlementLabel({ ...none, source: 'comp', tier: 2, comp_servers: 11 })).toBe('Admin grant of 11 shared, Tier 2');
    expect(entitlementLabel({ ...none, source: 'tier', tier: 2, comp_servers: 2 })).toBe('Tier 2, Admin grant of 2 shared');
    expect(entitlementLabel(none)).toBe('None');
  });

  it('adds the dedicated machines, granted or paid for', () => {
    expect(
      entitlementLabel({ ...none, source: 'comp', comp_servers: 9, comp_own_machines: 2, own_machines: 2 }),
    ).toBe('Admin grant of 9 shared + 2 dedicated');
    expect(entitlementLabel({ ...none, comp_own_machines: 1, own_machines: 1 })).toBe('Admin grant of 1 dedicated');
    // Paid for, beside a tier or alone (no tier needed).
    expect(entitlementLabel({ ...none, source: 'tier', tier: 2, own_machines: 1 })).toBe('Tier 2, 1 dedicated');
    expect(entitlementLabel({ ...none, own_machines: 2 })).toBe('2 dedicated');
  });
});

describe('ownerHostingLabel', () => {
  it("says an admin's grant where the fleet said no tier, and both when both", () => {
    expect(ownerHostingLabel({ tier: 2, granted: false })).toBe('Tier 2');
    expect(ownerHostingLabel({ tier: null, granted: true })).toBe('Admin grant');
    expect(ownerHostingLabel({ tier: 1, granted: true })).toBe('Admin grant, Tier 1');
    expect(ownerHostingLabel({ tier: null, granted: false })).toBe('No tier');
  });
});

describe('soldCapacityText', () => {
  it('says each item sold against what the fleet sells', () => {
    expect(
      soldCapacityText({
        shared: { sold: 9, sellable: 12 },
        own_machines: { sold: 2, sellable: 2 },
        addresses: { sold: 0, sellable: 1 },
        oversold: false,
      }),
    ).toBe('Sold: 9 of 12 shared places, 2 of 2 dedicated machines, 0 of 1 static addresses.');
  });
});

describe('serverPlacesLabel', () => {
  it('counts each dedicated machine as one more server beside the shared ones', () => {
    expect(serverPlacesLabel({ shared_servers: 0, own_machines: 0 })).toBe('None');
    expect(serverPlacesLabel({ shared_servers: 3, own_machines: 0 })).toBe('3 on shared machines');
    expect(serverPlacesLabel({ shared_servers: 9, own_machines: 2 })).toBe('9 on shared machines, 2 on dedicated machines');
    expect(serverPlacesLabel({ shared_servers: 2, own_machines: 1 })).toBe('2 on shared machines, 1 on a dedicated machine');
    // Dedicated machines alone, no tier.
    expect(serverPlacesLabel({ shared_servers: 0, own_machines: 2 })).toBe('2 on dedicated machines');
  });
});
