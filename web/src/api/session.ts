// The website session: POST /api/login, POST /api/logout and GET /api/me
// (apps/novaworld_common/README.md, "Website sessions and roles"). The
// server keeps the session in an HttpOnly cookie (`opennova_session`, or
// `__Host-opennova_session` behind https), which the browser sends on these
// same-origin calls by itself; nothing here touches it.
//
// Each call answers a SessionResult instead of throwing, with the server's
// `error` code mapped to a message a visitor can act on.

import { isAxiosError } from 'axios';
import { apiClient } from './client';
import type { Role } from '../types/users';

// The account a session acts as: what /api/login and /api/me answer.
export interface SessionUser {
  id: number;
  username: string;
  role: Role;
}

// The server's `error` codes these routes answer with, plus `network` (no
// reply at all) and `unknown` (a reply this client does not recognise).
export type SessionErrorCode =
  | 'invalid_credentials'
  | 'account_banned'
  | 'account_restricted'
  | 'rate_limited'
  | 'missing_field'
  | 'invalid_field'
  | 'invalid_json'
  | 'csrf_header_required'
  | 'db_error'
  | 'cookie_not_kept'
  | 'network'
  | 'unknown';

export interface SessionError {
  code: SessionErrorCode;
  // The HTTP status, or null when no reply arrived.
  status: number | null;
  // What to show the visitor.
  message: string;
  // A 429's Retry-After, in seconds.
  retryAfterSeconds?: number;
}

export type SessionResult<T> = { ok: true; value: T } | { ok: false; error: SessionError };

const KNOWN_CODES: ReadonlySet<string> = new Set<SessionErrorCode>([
  'invalid_credentials',
  'account_banned',
  'account_restricted',
  'rate_limited',
  'missing_field',
  'invalid_field',
  'invalid_json',
  'csrf_header_required',
  'db_error',
]);

// "30 seconds", "2 minutes", "1 hour": a Retry-After for a sentence.
export function formatRetryAfter(seconds: number): string {
  const plural = (n: number, unit: string) => `${n} ${unit}${n === 1 ? '' : 's'}`;
  if (seconds < 60) return plural(Math.max(1, Math.ceil(seconds)), 'second');
  if (seconds < 3600) return plural(Math.ceil(seconds / 60), 'minute');
  return plural(Math.ceil(seconds / 3600), 'hour');
}

// Retry-After as seconds: the server sends delta-seconds; an HTTP-date is
// read too. Undefined when absent or unreadable.
export function parseRetryAfter(value: unknown, now: number = Date.now()): number | undefined {
  if (typeof value !== 'string' && typeof value !== 'number') return undefined;
  const text = String(value).trim();
  if (/^\d+$/.test(text)) return Number(text);
  const date = Date.parse(text);
  if (Number.isNaN(date)) return undefined;
  return Math.max(0, Math.ceil((date - now) / 1000));
}

function messageFor(code: SessionErrorCode, retryAfterSeconds?: number): string {
  switch (code) {
    case 'invalid_credentials':
      return 'Wrong username or password.';
    case 'account_banned':
      return 'This account is banned.';
    case 'account_restricted':
      return 'This account is not active, so it cannot log in.';
    case 'rate_limited':
      return retryAfterSeconds !== undefined
        ? `Too many login attempts. Try again in ${formatRetryAfter(retryAfterSeconds)}.`
        : 'Too many login attempts. Try again in a little while.';
    case 'missing_field':
      return 'Enter your username and password.';
    case 'invalid_field':
      return 'That username is not valid (64 bytes at most).';
    case 'invalid_json':
      return 'The server could not read the request. Reload the page and try again.';
    case 'csrf_header_required':
      return 'The server refused the request as not coming from this site. Reload the page and try again.';
    case 'db_error':
      return 'The server hit a database error. Try again shortly.';
    case 'cookie_not_kept':
      return 'The server accepted the login, but your browser did not keep the session cookie. Check that this site may set cookies.';
    case 'network':
      return 'Could not reach the server. Check your connection and try again.';
    case 'unknown':
      return 'Something went wrong. Try again shortly.';
  }
}

// Any failure of a session call, as a SessionError.
export function describeSessionError(err: unknown): SessionError {
  if (isAxiosError(err)) {
    const response = err.response;
    if (!response) {
      return { code: 'network', status: null, message: messageFor('network') };
    }
    const data = response.data as { error?: unknown; message?: unknown } | undefined;
    const serverCode = typeof data?.error === 'string' ? data.error : '';
    const code: SessionErrorCode = KNOWN_CODES.has(serverCode)
      ? (serverCode as SessionErrorCode)
      : response.status === 429
        ? 'rate_limited'
        : 'unknown';
    const retryAfterSeconds =
      code === 'rate_limited' ? parseRetryAfter(response.headers?.['retry-after']) : undefined;
    const known = code !== 'unknown';
    const serverMessage = typeof data?.message === 'string' ? data.message : '';
    return {
      code,
      status: response.status,
      message: known
        ? messageFor(code, retryAfterSeconds)
        : serverMessage || `${messageFor('unknown')} (HTTP ${response.status})`,
      ...(retryAfterSeconds !== undefined ? { retryAfterSeconds } : {}),
    };
  }
  return { code: 'unknown', status: null, message: messageFor('unknown') };
}

export function sessionError(code: SessionErrorCode, status: number | null = null): SessionError {
  return { code, status, message: messageFor(code) };
}

function isSessionUser(value: unknown): value is SessionUser {
  const v = value as Partial<SessionUser> | null;
  return (
    typeof v === 'object' &&
    v !== null &&
    typeof v.id === 'number' &&
    typeof v.username === 'string' &&
    (v.role === 'player' || v.role === 'admin')
  );
}

// POST /api/login: the account on success (the server set the cookie).
export async function login(username: string, password: string): Promise<SessionResult<SessionUser>> {
  try {
    const res = await apiClient.post<unknown>('/login', { username, password });
    if (!isSessionUser(res.data)) return { ok: false, error: sessionError('unknown', res.status) };
    return { ok: true, value: res.data };
  } catch (err) {
    return { ok: false, error: describeSessionError(err) };
  }
}

// POST /api/logout: 204, also when no session was live.
export async function logout(): Promise<SessionResult<void>> {
  try {
    await apiClient.post('/logout');
    return { ok: true, value: undefined };
  } catch (err) {
    return { ok: false, error: describeSessionError(err) };
  }
}

// How long GET /api/me may take before it counts as no answer (`network`):
// the pages wait on it before they show who is logged in.
export const ME_TIMEOUT_MS = 10_000;

// GET /api/me: the signed-in account, or null when the browser holds no live
// session (401).
export async function me(signal?: AbortSignal): Promise<SessionResult<SessionUser | null>> {
  try {
    const res = await apiClient.get<unknown>('/me', { signal, timeout: ME_TIMEOUT_MS });
    if (!isSessionUser(res.data)) return { ok: false, error: sessionError('unknown', res.status) };
    return { ok: true, value: res.data };
  } catch (err) {
    if (isAxiosError(err) && err.response?.status === 401) return { ok: true, value: null };
    return { ok: false, error: describeSessionError(err) };
  }
}
