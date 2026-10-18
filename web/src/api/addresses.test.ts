import { AxiosError, AxiosHeaders, type AxiosResponse } from 'axios';
import { beforeEach, describe, expect, it, vi } from 'vitest';
import { apiClient } from './client';
import { fetchMyAddresses, nameAddress, parseAddress } from './addresses';
import { address, addressesResponse } from '../test/addressFixtures';

vi.mock('./client', () => ({
  apiClient: { get: vi.fn(), post: vi.fn() },
  adminClient: { get: vi.fn(), post: vi.fn() },
}));

const get = vi.mocked(apiClient.get);
const post = vi.mocked(apiClient.post);

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

describe('parseAddress', () => {
  it('reads an address, with defaults for what a reply leaves out', () => {
    expect(parseAddress(address(1, 'smoke'))).toEqual(address(1, 'smoke'));
    expect(parseAddress({ id: 2, name: 'arena', state: 'allocating', source: 'comp' })).toEqual({
      id: 2,
      name: 'arena',
      hostname: 'arena',
      state: 'allocating',
      public_ip: null,
      port: null,
      source: 'comp',
      stripe_subscription_id: null,
      server: null,
      reason: null,
      created_at: null,
      ready_at: null,
      lapsed_at: null,
      released_at: null,
    });
    expect(parseAddress({ ...address(1), id: '1' })).toBeNull();
    expect(parseAddress({ ...address(1), state: undefined })).toBeNull();
  });
});

describe('the routes', () => {
  beforeEach(() => vi.clearAllMocks());

  it("reads the account's addresses and refuses a malformed one whole", async () => {
    const body = addressesResponse([address(1, 'smoke')], { unnamed: 1 });
    get.mockResolvedValue(reply(body));
    expect(await fetchMyAddresses()).toEqual({ ok: true, value: body });
    expect(get).toHaveBeenCalledWith('/me/addresses', expect.anything());
    get.mockResolvedValue(reply({ ...body, addresses: [address(1), { id: 'x' }] }));
    expect((await fetchMyAddresses()).ok).toBe(false);
    get.mockResolvedValue(reply({ ...body, unnamed: -1 }));
    expect((await fetchMyAddresses()).ok).toBe(false);
  });

  it('names the next bought address, and maps the refusals to words, never the server text', async () => {
    post.mockResolvedValue(reply({ address: address(3, 'arena', { state: 'allocating', public_ip: null }) }, 201));
    const named = await nameAddress('arena');
    expect(named.ok && named.value.hostname).toBe('arena.play.opennova.net');
    expect(post).toHaveBeenCalledWith('/me/addresses', { name: 'arena' });

    const cases: [number, string, string][] = [
      [400, 'invalid_name', '3 to 32 lower-case letters'],
      [409, 'name_taken', 'Another static address already has that name'],
      [409, 'no_address', 'no bought static address waiting'],
      [503, 'addresses_disabled', 'does not offer static addresses'],
      [401, 'unauthorized', 'Log in again'],
    ];
    for (const [status, code, text] of cases) {
      post.mockRejectedValueOnce(refusal(status, { error: code, message: 'raw server text' }));
      const result = await nameAddress('arena');
      expect(!result.ok && result.error.code).toBe(code);
      expect(!result.ok && result.error.message).toContain(text);
      expect(!result.ok && result.error.message).not.toContain('raw server text');
    }
    post.mockRejectedValueOnce(refusal(502, '<html>'));
    const proxy = await nameAddress('arena');
    expect(!proxy.ok && proxy.error).toMatchObject({ code: 'unknown', status: 502 });
  });
});
