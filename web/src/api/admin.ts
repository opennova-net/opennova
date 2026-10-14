// Admin endpoints: an admin-role session or the bearer token. See
// `client.ts` for the bearer-token interceptor; the call sites here look like
// plain GETs/POSTs.

import { isAxiosError } from 'axios';
import { adminClient } from './client';
import type { ConnectionsResponse, ServerStatus } from '../types/admin';
import type { FleetResponse, FleetServer, FleetServerReply, StopAllReply } from '../types/hostedServer';

export async function fetchServerStatus(signal?: AbortSignal): Promise<ServerStatus> {
  const response = await adminClient.get<ServerStatus>('/server-status', { signal });
  return response.data;
}

export async function updateServerStatus(payload: ServerStatus): Promise<ServerStatus> {
  const response = await adminClient.put<ServerStatus>('/server-status', payload);
  return response.data;
}

export async function fetchConnections(signal?: AbortSignal): Promise<ConnectionsResponse> {
  const response = await adminClient.get<ConnectionsResponse>('/connections', { signal });
  return response.data;
}

// The hosted servers' admin routes (/api/admin/hosted*), the CD keys' (cdKeys.ts)
// and the hosting approval (users.ts setCanHost) answer an AdminResult instead of throwing,
// with the server's `error` code mapped to a message: the server's own
// `message` text is never shown.

// The `error` codes these routes answer with, plus `network` (no reply at
// all) and `unknown` (a reply this client does not recognise).
export type AdminErrorCode =
  | 'unauthorized'
  | 'forbidden'
  | 'csrf_header_required'
  | 'proxy_required'
  | 'invalid_json'
  | 'invalid_field'
  | 'missing_field'
  | 'not_found'
  | 'not_live'
  | 'no_such_account'
  | 'revoked'
  | 'already_revoked'
  | 'already_assigned'
  | 'account_has_key'
  | 'db_error'
  | 'network'
  | 'unknown';

export interface AdminError {
  code: AdminErrorCode;
  // The HTTP status, or null when no reply arrived.
  status: number | null;
  // What to show the admin.
  message: string;
}

export type AdminResult<T> = { ok: true; value: T } | { ok: false; error: AdminError };

const KNOWN_ADMIN_CODES: ReadonlySet<string> = new Set<AdminErrorCode>([
  'unauthorized',
  'forbidden',
  'csrf_header_required',
  'proxy_required',
  'invalid_json',
  'invalid_field',
  'missing_field',
  'not_found',
  'not_live',
  'no_such_account',
  'revoked',
  'already_revoked',
  'already_assigned',
  'account_has_key',
  'db_error',
]);

function adminMessageFor(code: AdminErrorCode): string {
  switch (code) {
    case 'unauthorized':
      return 'Your admin credentials were refused. Log in again or enter the admin token.';
    case 'forbidden':
      return 'This account is not an admin.';
    case 'csrf_header_required':
      return 'The server refused the request as not coming from this site. Reload the page and try again.';
    case 'proxy_required':
      return 'The server refused a request that did not come through this site. Reload the page and try again.';
    case 'invalid_json':
    case 'invalid_field':
    case 'missing_field':
      return 'The server could not read the request. Reload the page and try again.';
    case 'not_found':
      return 'It no longer exists. Refresh the list.';
    case 'not_live':
      return 'That hosted server has already ended. Refresh the list.';
    case 'no_such_account':
      return 'No player account has that name.';
    case 'revoked':
    case 'already_revoked':
      return 'That key is revoked. Refresh the list.';
    case 'already_assigned':
      return 'That key is already on an account. Refresh the list.';
    case 'account_has_key':
      return 'That account already holds a key for this game. Revoke it first.';
    case 'db_error':
      return 'The server hit a database error. Try again shortly.';
    case 'network':
      return 'Could not reach the server. Check your connection and try again.';
    case 'unknown':
      return 'Something went wrong. Try again shortly.';
  }
}

// Any failure of an admin call, as an AdminError.
export function describeAdminError(err: unknown): AdminError {
  if (isAxiosError(err)) {
    const response = err.response;
    if (!response) return { code: 'network', status: null, message: adminMessageFor('network') };
    const data = response.data as { error?: unknown } | undefined;
    const serverCode = typeof data?.error === 'string' ? data.error : '';
    const code: AdminErrorCode = KNOWN_ADMIN_CODES.has(serverCode)
      ? (serverCode as AdminErrorCode)
      : response.status === 401
        ? 'unauthorized'
        : 'unknown';
    return {
      code,
      status: response.status,
      message:
        code === 'unknown'
          ? `${adminMessageFor('unknown')} (HTTP ${response.status})`
          : adminMessageFor(code),
    };
  }
  return { code: 'unknown', status: null, message: adminMessageFor('unknown') };
}

export function malformedAdminReply(status: number): AdminResult<never> {
  return { ok: false, error: { code: 'unknown', status, message: adminMessageFor('unknown') } };
}

function isFleetResponse(value: unknown): value is FleetResponse {
  const v = value as Partial<FleetResponse> | null;
  return (
    typeof v === 'object' &&
    v !== null &&
    typeof v.enabled === 'boolean' &&
    typeof v.cap === 'number' &&
    typeof v.live === 'number' &&
    Array.isArray(v.servers) &&
    v.servers.every((s) => typeof s?.id === 'number' && typeof s?.state === 'string')
  );
}

// GET /api/admin/hosted: the fleet, the newest 100 servers, live ones first.
export async function fetchHostedFleet(signal?: AbortSignal): Promise<AdminResult<FleetResponse>> {
  try {
    const res = await adminClient.get<unknown>('/hosted', { signal });
    if (!isFleetResponse(res.data)) return malformedAdminReply(res.status);
    return { ok: true, value: res.data };
  } catch (err) {
    return { ok: false, error: describeAdminError(err) };
  }
}

// POST /api/admin/hosted/<id>/stop: one live server to stopping (202).
export async function stopFleetServer(id: number): Promise<AdminResult<FleetServer | null>> {
  try {
    const res = await adminClient.post<FleetServerReply>(`/hosted/${id}/stop`);
    return { ok: true, value: res.data?.server ?? null };
  } catch (err) {
    return { ok: false, error: describeAdminError(err) };
  }
}

// POST /api/admin/hosted/stop-all: the kill switch, every live server to
// stopping; how many were asked.
export async function stopAllFleetServers(): Promise<AdminResult<number>> {
  try {
    const res = await adminClient.post<StopAllReply>('/hosted/stop-all');
    if (typeof res.data?.stopping !== 'number') return malformedAdminReply(res.status);
    return { ok: true, value: res.data.stopping };
  } catch (err) {
    return { ok: false, error: describeAdminError(err) };
  }
}
