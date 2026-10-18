import { AxiosError, AxiosHeaders, type AxiosResponse } from 'axios';
import { beforeEach, describe, expect, it, vi } from 'vitest';
import { adminClient } from './client';
import { addCompAddress, fetchAdminAddresses, releaseAddress, renameAddress } from './adminAddresses';
import { adminAddress, adminAddressesResponse } from '../test/addressFixtures';

vi.mock('./client', () => ({
  apiClient: { get: vi.fn(), post: vi.fn() },
  adminClient: { get: vi.fn(), post: vi.fn(), put: vi.fn() },
}));

const get = vi.mocked(adminClient.get);
const post = vi.mocked(adminClient.post);
const put = vi.mocked(adminClient.put);

function reply(data: unknown, status = 200) {
  return { status, data } as AxiosResponse;
}

function refusal(status: number, data: unknown): AxiosError {
  const config = { headers: new AxiosHeaders() };
  return new AxiosError('refused', 'ERR_BAD_REQUEST', config, null, {
    status,
    statusText: '',
    data,
    headers: {},
    config,
  } as AxiosResponse);
}

describe('the admin address routes', () => {
  beforeEach(() => vi.clearAllMocks());

  it('reads every address with its owner, and the orphans', async () => {
    const body = adminAddressesResponse([adminAddress(1, 'smoke'), adminAddress(2, 'arena', { owner: null, lapse_cause: 'admin' })], {
      orphans: [{ allocation_id: 'eipalloc-0dead', public_ip: '203.0.113.50', address_id: null, seen_at: '2026-10-18 09:00:00' }],
    });
    // The sweep's "no tag" is 0.
    get.mockResolvedValue(reply({ ...body, orphans: [{ ...body.orphans[0], address_id: 0 }] }));
    expect(await fetchAdminAddresses()).toEqual({ ok: true, value: body });
    expect(get).toHaveBeenCalledWith('/addresses', expect.anything());
    get.mockResolvedValue(reply({ ...body, held: 'two' }));
    expect((await fetchAdminAddresses()).ok).toBe(false);
  });

  it('renames, releases and grants, mapping the refusals', async () => {
    put.mockResolvedValue(reply({ address: adminAddress(1, 'blaze') }));
    const renamed = await renameAddress(1, 'blaze');
    expect(renamed.ok && renamed.value.name).toBe('blaze');
    expect(put).toHaveBeenCalledWith('/addresses/1', { name: 'blaze' });

    post.mockResolvedValueOnce(reply({ address: adminAddress(1, 'smoke', { state: 'lapsed', lapse_cause: 'admin' }) }, 202));
    const released = await releaseAddress(1, 'smoke');
    expect(released.ok && released.value.state).toBe('lapsed');
    expect(post).toHaveBeenLastCalledWith('/addresses/1/release', { confirm: 'smoke' });

    post.mockResolvedValueOnce(reply({ address: adminAddress(3, 'arena', { source: 'comp' }) }, 201));
    const comp = await addCompAddress(7, 'arena');
    expect(comp.ok && comp.value.source).toBe('comp');
    expect(post).toHaveBeenLastCalledWith('/users/7/addresses', { name: 'arena' });

    const cases: [number, string, string][] = [
      [400, 'invalid_name', '3 to 32 lower-case letters'],
      [409, 'name_taken', 'Another static address already has that name'],
      [400, 'confirm_mismatch', "not the address's"],
      [409, 'not_releasable', 'on its way out already'],
      [409, 'address_limit', 'a static address for every own machine'],
      [503, 'addresses_disabled', 'sells no static address'],
    ];
    for (const [status, code, text] of cases) {
      post.mockRejectedValueOnce(refusal(status, { error: code, message: 'raw server text' }));
      const result = await releaseAddress(1, 'smoke');
      expect(!result.ok && result.error.code).toBe(code);
      expect(!result.ok && result.error.message).toContain(text);
      expect(!result.ok && result.error.message).not.toContain('raw server text');
    }
  });
});
