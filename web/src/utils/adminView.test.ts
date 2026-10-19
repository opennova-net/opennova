import { describe, expect, it } from 'vitest';
import { entitlementLabel, ownerHostingLabel, serverPlacesLabel, soldCapacityText } from './adminView';

describe('entitlementLabel', () => {
  it('names each source an account has, the one that gives its servers first', () => {
    expect(entitlementLabel({ source: 'tier', tier: 2, comp_servers: 0 })).toBe('Tier 2');
    expect(entitlementLabel({ source: 'comp', tier: null, comp_servers: 3 })).toBe('Admin grant of 3');
    // A grant bigger than the tier still billing beside it, and one no bigger.
    expect(entitlementLabel({ source: 'comp', tier: 2, comp_servers: 11 })).toBe('Admin grant of 11, Tier 2');
    expect(entitlementLabel({ source: 'tier', tier: 2, comp_servers: 2 })).toBe('Tier 2, Admin grant of 2');
    expect(entitlementLabel({ source: 'none', tier: null, comp_servers: 0 })).toBe('None');
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
    ).toBe('Sold: 9 of 12 shared places, 2 of 2 own machines, 0 of 1 static addresses.');
  });
});

describe('serverPlacesLabel', () => {
  it('counts an own machine as one of the servers, adding none', () => {
    expect(serverPlacesLabel({ max_servers: 0, own_machines: 2 })).toBe('None');
    expect(serverPlacesLabel({ max_servers: 3, own_machines: 0 })).toBe('All on shared machines');
    expect(serverPlacesLabel({ max_servers: 11, own_machines: 2 })).toBe('9 on shared machines, 2 on own machines');
    expect(serverPlacesLabel({ max_servers: 3, own_machines: 1 })).toBe('2 on shared machines, 1 on own machine');
    // More own machines than servers: every server on one.
    expect(serverPlacesLabel({ max_servers: 2, own_machines: 5 })).toBe('All on own machines');
  });
});
