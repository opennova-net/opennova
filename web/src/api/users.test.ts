import { type AxiosResponse } from 'axios';
import { beforeEach, describe, expect, it, vi } from 'vitest';
import { adminClient } from './client';
import { fetchUser, listUsers, pickUser } from './users';
import { user } from '../test/adminFixtures';

vi.mock('./client', () => ({
  apiClient: { post: vi.fn() },
  adminClient: { get: vi.fn(), post: vi.fn(), put: vi.fn(), delete: vi.fn() },
}));

const get = vi.mocked(adminClient.get);

function reply(data: unknown, status = 200) {
  return { status, data } as AxiosResponse;
}

// An account as the service writes it, with its list row's fields.
function row(id: number, extra: Record<string, unknown> = {}) {
  const account: Record<string, unknown> = { ...user(id) };
  delete account.created_at;
  delete account.last_login;
  return {
    ...account,
    entitlement: { max_servers: 0, shared_servers: 0, own_machines: 0, comp_own_machines: 0, source: 'none', tier: null, comp_servers: 0 },
    live_servers: 0,
    subscription_status: null,
    ...extra,
  };
}

describe('the users routes', () => {
  beforeEach(() => vi.clearAllMocks());

  it('reads when an account was made and last signed in', () => {
    const parsed = pickUser({ ...row(7), created_at: '2026-09-14T12:00:00Z', last_login: '2026-10-27T08:30:00Z' });
    expect(parsed).toMatchObject({ id: 7, created_at: '2026-09-14T12:00:00Z', last_login: '2026-10-27T08:30:00Z' });
  });

  it('takes a missing or null time as none', () => {
    expect(pickUser(row(7))).toMatchObject({ created_at: null, last_login: null });
    expect(pickUser({ ...row(7), created_at: null, last_login: null })).toMatchObject({ created_at: null, last_login: null });
    expect(pickUser({ ...row(7), created_at: 12, last_login: '' })).toMatchObject({ created_at: null, last_login: null });
  });

  it('lists the accounts with their times, and refuses a malformed row', async () => {
    get.mockResolvedValue(reply({ users: [row(1, { created_at: '2026-09-01T00:00:00Z', last_login: null, live_servers: 2 }), row(2)] }));
    const result = await listUsers();
    expect(get).toHaveBeenCalledWith('/users', expect.anything());
    expect(result.ok && result.value.map((u) => [u.id, u.created_at, u.last_login, u.live_servers])).toEqual([
      [1, '2026-09-01T00:00:00Z', null, 2],
      [2, null, null, 0],
    ]);
    get.mockResolvedValue(reply({ users: [row(1), { id: 2 }] }));
    expect((await listUsers()).ok).toBe(false);
  });

  it("reads one account's times on its page", async () => {
    get.mockResolvedValue(reply({ user: { ...row(7), created_at: '2026-09-14T12:00:00Z', last_login: '2026-10-27T08:30:00Z' }, entitlement: {}, servers: [], addresses: [], billing: {} }));
    const result = await fetchUser(7);
    expect(get).toHaveBeenCalledWith('/users/7', expect.anything());
    expect(result.ok && result.value.user).toMatchObject({ created_at: '2026-09-14T12:00:00Z', last_login: '2026-10-27T08:30:00Z' });
  });
});
