// The logged-in account's hosted server (apps/novaworld_server/README.md,
// "Hosted servers"; hosted/hosted_routes.cpp): GET /api/me/server, POST
// /api/me/server/start and POST /api/me/server/stop. The session cookie
// authenticates them and `apiClient` sends the CSRF header both POSTs need.
//
// Each call answers a HostedResult instead of throwing, with the server's
// `error` code mapped to a message the owner can act on. The server's own
// `message` text is never shown.

import { isAxiosError } from 'axios';
import { apiClient } from './client';
import { formatRetryAfter, parseRetryAfter } from './session';
import type {
  HostedServer,
  HostedServerReply,
  HostedSettings,
  MyHostedServerResponse,
  StartSettings,
} from '../types/hostedServer';
import { FIELD_RULES, isSettingsField, type SettingsField } from '../utils/hostedSettings';

// The server's `error` codes these routes answer with, plus `network` (no
// reply at all) and `unknown` (a reply this client does not recognise).
export type HostedErrorCode =
  | 'unauthorized'
  | 'csrf_header_required'
  | 'proxy_required'
  | 'hosting_disabled'
  | 'rate_limited'
  | 'invalid_json'
  | 'invalid_field'
  | 'invalid_settings'
  | 'not_approved'
  | 'already_live'
  | 'at_capacity'
  | 'no_live_server'
  | 'db_error'
  | 'network'
  | 'unknown';

export interface HostedError {
  code: HostedErrorCode;
  // The HTTP status, or null when no reply arrived.
  status: number | null;
  // What to show the owner.
  message: string;
  // A 429's Retry-After, in seconds.
  retryAfterSeconds?: number;
  // invalid_settings: the setting the server refused.
  field?: SettingsField;
}

export type HostedResult<T> = { ok: true; value: T } | { ok: false; error: HostedError };

const KNOWN_CODES: ReadonlySet<string> = new Set<HostedErrorCode>([
  'unauthorized',
  'csrf_header_required',
  'proxy_required',
  'hosting_disabled',
  'rate_limited',
  'invalid_json',
  'invalid_field',
  'invalid_settings',
  'not_approved',
  'already_live',
  'at_capacity',
  'no_live_server',
  'db_error',
]);

function messageFor(code: HostedErrorCode, field?: SettingsField, retryAfterSeconds?: number): string {
  switch (code) {
    case 'unauthorized':
      return 'Your session has ended. Log in again to manage your hosted server.';
    case 'csrf_header_required':
      return 'The server refused the request as not coming from this site. Reload the page and try again.';
    case 'proxy_required':
      return 'The server refused a request that did not come through this site. Reload the page and try again.';
    case 'hosting_disabled':
      return 'This NovaWorld service is not running hosted servers right now.';
    case 'rate_limited':
      return retryAfterSeconds !== undefined
        ? `Too many starts and stops. Try again in ${formatRetryAfter(retryAfterSeconds)}.`
        : 'Too many starts and stops. Try again in a little while.';
    case 'invalid_json':
      return 'The server could not read the request. Reload the page and try again.';
    case 'invalid_field':
      return 'The server could not read one of the settings. Reload the page and try again.';
    case 'invalid_settings':
      return field
        ? `The server refused the settings. ${FIELD_RULES[field]}`
        : 'The server refused the settings. Check them and try again.';
    case 'not_approved':
      return 'Your account is not approved to host a server, or is no longer active.';
    case 'already_live':
      return 'You already have a hosted server. It is shown below.';
    case 'at_capacity':
      return 'Every hosted server place on this service is taken. Try again later.';
    case 'no_live_server':
      return 'You have no hosted server running to stop.';
    case 'db_error':
      return 'The server hit a database error. Try again shortly.';
    case 'network':
      return 'Could not reach the server. Check your connection and try again.';
    case 'unknown':
      return 'Something went wrong. Try again shortly.';
  }
}

// Any failure of these calls, as a HostedError.
export function describeHostedError(err: unknown): HostedError {
  if (isAxiosError(err)) {
    const response = err.response;
    if (!response) {
      return { code: 'network', status: null, message: messageFor('network') };
    }
    const data = response.data as { error?: unknown; field?: unknown } | undefined;
    const serverCode = typeof data?.error === 'string' ? data.error : '';
    const code: HostedErrorCode = KNOWN_CODES.has(serverCode)
      ? (serverCode as HostedErrorCode)
      : response.status === 429
        ? 'rate_limited'
        : response.status === 401
          ? 'unauthorized'
          : 'unknown';
    const named = data?.field;
    const field = code === 'invalid_settings' && isSettingsField(named) ? named : undefined;
    const retryAfterSeconds =
      code === 'rate_limited' ? parseRetryAfter(response.headers?.['retry-after']) : undefined;
    return {
      code,
      status: response.status,
      message:
        code === 'unknown'
          ? `${messageFor('unknown')} (HTTP ${response.status})`
          : messageFor(code, field, retryAfterSeconds),
      ...(field !== undefined ? { field } : {}),
      ...(retryAfterSeconds !== undefined ? { retryAfterSeconds } : {}),
    };
  }
  return { code: 'unknown', status: null, message: messageFor('unknown') };
}

function malformed(status: number): HostedResult<never> {
  return { ok: false, error: { code: 'unknown', status, message: messageFor('unknown') } };
}

function isHostedServer(value: unknown): value is HostedServer {
  const v = value as Partial<HostedServer> | null;
  return (
    typeof v === 'object' &&
    v !== null &&
    typeof v.id === 'number' &&
    typeof v.state === 'string' &&
    typeof v.live === 'boolean' &&
    typeof v.settings === 'object' &&
    v.settings !== null &&
    Array.isArray((v.settings as Partial<HostedSettings>).missions)
  );
}

function serverOrNull(value: unknown): HostedServer | null | undefined {
  if (value === null) return null;
  return isHostedServer(value) ? value : undefined;
}

// GET /api/me/server: whether hosting is on, the account's approval, the
// service's rotation and the account's server (live, else its last one).
export async function fetchMyHostedServer(signal?: AbortSignal): Promise<HostedResult<MyHostedServerResponse>> {
  try {
    const res = await apiClient.get<unknown>('/me/server', { signal });
    const data = res.data as Partial<MyHostedServerResponse> | null;
    const server = serverOrNull(data?.server);
    if (typeof data?.enabled !== 'boolean' || typeof data.can_host !== 'boolean' || server === undefined) {
      return malformed(res.status);
    }
    const defaults = Array.isArray(data.default_missions)
      ? data.default_missions.filter((m): m is string => typeof m === 'string')
      : [];
    return {
      ok: true,
      value: { enabled: data.enabled, can_host: data.can_host, default_missions: defaults, server },
    };
  } catch (err) {
    return { ok: false, error: describeHostedError(err) };
  }
}

async function postForServer(path: string, body?: StartSettings): Promise<HostedResult<HostedServer | null>> {
  try {
    const res = await apiClient.post<unknown>(path, body);
    const server = serverOrNull((res.data as Partial<HostedServerReply> | null)?.server);
    if (server === undefined) return malformed(res.status);
    return { ok: true, value: server };
  } catch (err) {
    return { ok: false, error: describeHostedError(err) };
  }
}

// POST /api/me/server/start: the account's server, requested (202), with
// these settings. The service's supervisor takes it from there.
export function startHostedServer(settings: StartSettings): Promise<HostedResult<HostedServer | null>> {
  return postForServer('/me/server/start', settings);
}

// POST /api/me/server/stop: the account's live server to stopping (202; also
// when it already was), or stopped when the supervisor finished it meanwhile.
export function stopHostedServer(): Promise<HostedResult<HostedServer | null>> {
  return postForServer('/me/server/stop');
}
