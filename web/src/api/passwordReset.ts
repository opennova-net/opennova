// Password resets by email (apps/novaworld_server/README.md, "Account email
// and password resets"): POST /api/auth/forgot mails a reset link to an
// account's address, POST /api/auth/reset sets the new password with the
// link's token. Both go through the plain `apiClient` (anonymous visitors,
// the CSRF header). Each answers a ResetResult instead of throwing, the
// server's `error` code mapped to a message a visitor can act on.

import { isAxiosError } from 'axios';
import { apiClient } from './client';
import { formatRetryAfter, parseRetryAfter } from './session';

export type ResetErrorCode =
  | 'invalid_token'
  | 'invalid_field'
  | 'missing_field'
  | 'invalid_json'
  | 'rate_limited'
  | 'csrf_header_required'
  | 'db_error'
  | 'network'
  | 'unknown';

export interface ResetError {
  code: ResetErrorCode;
  // The HTTP status, or null when no reply arrived.
  status: number | null;
  message: string;
  // A 429's Retry-After, in seconds.
  retryAfterSeconds?: number;
}

export type ResetResult = { ok: true } | { ok: false; error: ResetError };

const KNOWN_CODES: ReadonlySet<string> = new Set<ResetErrorCode>([
  'invalid_token',
  'invalid_field',
  'missing_field',
  'invalid_json',
  'rate_limited',
  'csrf_header_required',
  'db_error',
]);

function messageFor(code: ResetErrorCode, retryAfterSeconds?: number): string {
  switch (code) {
    case 'invalid_token':
      return 'This link has expired or was used already. Ask for a new one.';
    case 'invalid_field':
      return 'That is not an email address.';
    case 'missing_field':
      return 'Fill in every field.';
    case 'invalid_json':
      return 'The server could not read the request. Reload the page and try again.';
    case 'rate_limited':
      return retryAfterSeconds !== undefined
        ? `Too many requests. Try again in ${formatRetryAfter(retryAfterSeconds)}.`
        : 'Too many requests. Try again in a little while.';
    case 'csrf_header_required':
      return 'The server refused the request as not coming from this site. Reload the page and try again.';
    case 'db_error':
      return 'The server hit a database error. Try again shortly.';
    case 'network':
      return 'Could not reach the server. Check your connection and try again.';
    case 'unknown':
      return 'Something went wrong. Try again shortly.';
  }
}

export function describeResetError(err: unknown): ResetError {
  if (isAxiosError(err)) {
    const response = err.response;
    if (!response) return { code: 'network', status: null, message: messageFor('network') };
    const data = response.data as { error?: unknown } | undefined;
    const serverCode = typeof data?.error === 'string' ? data.error : '';
    const code: ResetErrorCode = KNOWN_CODES.has(serverCode)
      ? (serverCode as ResetErrorCode)
      : response.status === 429
        ? 'rate_limited'
        : 'unknown';
    const retryAfterSeconds = code === 'rate_limited' ? parseRetryAfter(response.headers?.['retry-after']) : undefined;
    return {
      code,
      status: response.status,
      message: code === 'unknown' ? `${messageFor('unknown')} (HTTP ${response.status})` : messageFor(code, retryAfterSeconds),
      ...(retryAfterSeconds !== undefined ? { retryAfterSeconds } : {}),
    };
  }
  return { code: 'unknown', status: null, message: messageFor('unknown') };
}

// POST /api/auth/forgot: 202 for every address, whether or not an account has
// it (the server tells nobody which).
export async function requestPasswordReset(email: string): Promise<ResetResult> {
  try {
    await apiClient.post('/auth/forgot', { email });
    return { ok: true };
  } catch (err) {
    return { ok: false, error: describeResetError(err) };
  }
}

// POST /api/auth/reset: 204 once the password is set (the account's sessions
// end with it); 400 invalid_token for a link that is wrong, spent or past its
// hour.
export async function resetPassword(token: string, password: string): Promise<ResetResult> {
  try {
    await apiClient.post('/auth/reset', { token, password });
    return { ok: true };
  } catch (err) {
    return { ok: false, error: describeResetError(err) };
  }
}

// The form a reset link's token takes: 64 lower-case hex digits.
export function isResetToken(value: unknown): value is string {
  return typeof value === 'string' && /^[0-9a-f]{64}$/.test(value);
}
