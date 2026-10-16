import { AxiosError, AxiosHeaders, type AxiosResponse } from 'axios';
import { beforeEach, describe, expect, it, vi } from 'vitest';
import { apiClient } from './client';
import { isResetToken, requestPasswordReset, resetPassword } from './passwordReset';

vi.mock('./client', () => ({
  apiClient: { get: vi.fn(), post: vi.fn() },
  adminClient: { get: vi.fn(), post: vi.fn() },
}));

const post = vi.mocked(apiClient.post);

function reply(status: number) {
  return { status, data: '' } as AxiosResponse;
}

function refusal(status: number, data: unknown, headers: Record<string, string> = {}): AxiosError {
  const config = { headers: new AxiosHeaders() };
  return new AxiosError('refused', 'ERR_BAD_REQUEST', config, null, {
    status,
    statusText: '',
    data,
    headers,
    config,
  } as AxiosResponse);
}

const TOKEN = 'ab'.repeat(32);

describe('password reset routes', () => {
  beforeEach(() => vi.clearAllMocks());

  it('asks for a link with the address alone', async () => {
    post.mockResolvedValueOnce(reply(202));
    expect(await requestPasswordReset('player@example.com')).toEqual({ ok: true });
    expect(post).toHaveBeenLastCalledWith('/auth/forgot', { email: 'player@example.com' });
  });

  it('sets the password with the token', async () => {
    post.mockResolvedValueOnce(reply(204));
    expect(await resetPassword(TOKEN, 'new-secret')).toEqual({ ok: true });
    expect(post).toHaveBeenLastCalledWith('/auth/reset', { token: TOKEN, password: 'new-secret' });
  });

  it('names a dead link, the brake and its wait, a refused address and no answer', async () => {
    post.mockRejectedValueOnce(refusal(400, { error: 'invalid_token' }));
    const dead = await resetPassword(TOKEN, 'x');
    expect(!dead.ok && dead.error.code).toBe('invalid_token');
    expect(!dead.ok && dead.error.message).toContain('expired or was used already');

    post.mockRejectedValueOnce(refusal(429, { error: 'rate_limited' }, { 'retry-after': '120' }));
    const braked = await requestPasswordReset('player@example.com');
    expect(!braked.ok && braked.error).toMatchObject({ code: 'rate_limited', status: 429, retryAfterSeconds: 120 });
    expect(!braked.ok && braked.error.message).toBe('Too many requests. Try again in 2 minutes.');

    post.mockRejectedValueOnce(refusal(400, { error: 'invalid_field', message: 'email is not an email address' }));
    const refused = await requestPasswordReset('nope');
    expect(!refused.ok && refused.error.message).toBe('That is not an email address.');

    post.mockRejectedValueOnce(new AxiosError('down', 'ERR_NETWORK'));
    const down = await requestPasswordReset('player@example.com');
    expect(!down.ok && down.error.code).toBe('network');

    post.mockRejectedValueOnce(refusal(503, { error: 'teapot' }));
    const odd = await requestPasswordReset('player@example.com');
    expect(!odd.ok && odd.error.message).toContain('(HTTP 503)');
  });

  it('knows a token by its form', () => {
    expect(isResetToken(TOKEN)).toBe(true);
    expect(isResetToken(TOKEN.toUpperCase())).toBe(false);
    expect(isResetToken(TOKEN.slice(1))).toBe(false);
    expect(isResetToken([TOKEN])).toBe(false);
    expect(isResetToken(undefined)).toBe(false);
  });
});
