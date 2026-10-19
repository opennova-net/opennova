import { describe, expect, it, vi } from 'vitest';
import { adminClient } from './client';
import { fetchCosts, pickCosts } from './costs';
import { costs, localCosts } from '../test/costFixtures';

vi.mock('./client', () => ({
  apiClient: { get: vi.fn(), post: vi.fn() },
  adminClient: { get: vi.fn(), post: vi.fn() },
}));

const get = vi.mocked(adminClient.get);

const wire = (value: unknown): unknown => JSON.parse(JSON.stringify(value));

describe('the costs client', () => {
  it('reads the month as the server sends it', async () => {
    expect(pickCosts(wire(costs()))).toEqual(costs());
    expect(pickCosts(wire(localCosts()))).toEqual(localCosts());
    get.mockResolvedValue({ status: 200, data: wire(costs()) });
    const result = await fetchCosts();
    expect(result.ok).toBe(true);
    expect(get).toHaveBeenCalledWith('/costs', { signal: undefined });
  });

  it('reads a missing forecast and budget as none, never as a number', () => {
    const read = pickCosts(wire(costs({ forecast_cents: null, forecast_error: 'ce answered 400', budget: null, budget_error: 'refused' })));
    expect(read?.forecast_cents).toBeNull();
    expect(read?.forecast_error).toBe('ce answered 400');
    expect(read?.budget).toBeNull();
    expect(read?.budget_error).toBe('refused');
  });

  it('refuses a reply without the month or with a malformed line', () => {
    expect(pickCosts({ available: true })).toBeNull();
    expect(pickCosts({ available: true, month_to_date_cents: 'lots' })).toBeNull();
    expect(pickCosts({ ...(wire(costs()) as object), by_service: [{ service: 'EC2' }] })).toBeNull();
    expect(pickCosts({ ...(wire(costs()) as object), days: [{ cents: 5 }] })).toBeNull();
  });

  it("answers the server's refusal as an error", async () => {
    get.mockRejectedValue({ isAxiosError: true, response: { status: 401, data: { error: 'unauthorized' } } });
    const result = await fetchCosts();
    expect(result.ok).toBe(false);
  });
});
