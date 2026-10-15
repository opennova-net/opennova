import { describe, expect, it, vi } from 'vitest';
import { adminClient } from './client';
import { applyLimitStops, limitsHoldSummary, parseAdminBilling, reconcileBilling } from './adminBilling';

vi.mock('./client', () => ({
  apiClient: { get: vi.fn(), post: vi.fn() },
  adminClient: { get: vi.fn(), post: vi.fn() },
}));

const post = vi.mocked(adminClient.post);

describe('parseAdminBilling', () => {
  it('reads the limits hold', () => {
    const parsed = parseAdminBilling({
      enabled: true,
      mode: 'live',
      prices_ok: true,
      limits_hold: true,
      limits_hold_reason: 'a pass would stop 4 server(s) of 3 of 5 owners',
      limits_hold_owners: 3,
      limits_hold_servers: 4,
      subscriptions: [],
      orphans: [],
      recent_events: [],
    });
    expect(parsed).toMatchObject({
      limits_hold: true,
      limits_hold_reason: 'a pass would stop 4 server(s) of 3 of 5 owners',
      limits_hold_owners: 3,
      limits_hold_servers: 4,
    });
  });

  it('reads billing off: the hold alone, the rest empty', () => {
    const parsed = parseAdminBilling({
      enabled: false,
      limits_hold: false,
      limits_hold_reason: null,
      limits_hold_owners: 0,
      limits_hold_servers: 0,
    });
    expect(parsed).toMatchObject({ enabled: false, limits_hold: false, limits_hold_reason: null, subscriptions: [] });
  });
});

describe('the limits hold', () => {
  it('says what it keeps back', () => {
    expect(limitsHoldSummary({ limits_hold_owners: 1, limits_hold_servers: 1 })).toBe('1 server of 1 owner');
    expect(limitsHoldSummary({ limits_hold_owners: 3, limits_hold_servers: 4 })).toBe('4 servers of 3 owners');
  });

  it('applies its stops through the reconcile with force_limits; a plain reconcile carries none', async () => {
    post.mockResolvedValue({ status: 202, data: {} });
    expect(await applyLimitStops()).toEqual({ ok: true, value: undefined });
    expect(post).toHaveBeenLastCalledWith('/billing/reconcile', { force_limits: true });
    await reconcileBilling();
    expect(post).toHaveBeenLastCalledWith('/billing/reconcile', undefined);
  });
});
