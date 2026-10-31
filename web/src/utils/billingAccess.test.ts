import { describe, expect, it } from 'vitest';
import { isBillingPath, seesBilling, withoutBilling } from './billingAccess';
import { attention, finding } from '../test/usageFixtures';

describe('seesBilling', () => {
  it('is biggy as an admin, and no one else', () => {
    expect(seesBilling({ id: 1, username: 'biggy', role: 'admin' })).toBe(true);
    expect(seesBilling({ id: 1, username: 'biggy', role: 'player' })).toBe(false);
    expect(seesBilling({ id: 2, username: 'ghost', role: 'admin' })).toBe(false);
    expect(seesBilling({ id: 3, username: 'Biggy', role: 'admin' })).toBe(false);
    expect(seesBilling(null)).toBe(false);
  });
});

describe('isBillingPath', () => {
  it('is the Billing section and the pages under it, never by a shared prefix alone', () => {
    expect(isBillingPath('/admin/billing')).toBe(true);
    expect(isBillingPath('/admin/billing/')).toBe(true);
    expect(isBillingPath('/admin/billing/costs')).toBe(true);
    expect(isBillingPath('/admin/billing/addresses?state=failed#top')).toBe(true);
    expect(isBillingPath('/admin/billingx')).toBe(false);
    expect(isBillingPath('/me/billing')).toBe(false);
    expect(isBillingPath('/admin')).toBe(false);
  });
});

describe('withoutBilling', () => {
  it('drops the findings and alarms that open the Billing section, and keeps the rest', () => {
    const fleet = finding('warn', 'Machine 4 runs an older hostd than the release.', { subject: 'machine 4', link: '/admin/machines/4' });
    const plain = finding('info', 'Launches are held.', { link: null });
    const lost = { name: 'opennova-prod-machines-lost', state_since: null, description: '' };
    const kept = withoutBilling(
      attention({
        findings: [finding('danger', 'failed.', { link: '/admin/billing/addresses' }), fleet, finding('warn', 'is oversold.', { link: '/admin/billing' }), plain],
        alarms: [lost, { name: 'opennova-prod-billing-sync-stale', state_since: null, description: '' }, { name: 'opennova-prod-limits-held', state_since: null, description: '' }],
      }),
    );
    expect(kept.findings).toEqual([fleet, plain]);
    expect(kept.alarms).toEqual([lost]);
  });
});
