// The logged-in account's listed servers and its pushes to them
// (apps/novaworld_server/README.md, "Listed servers and owners"):
// GET /api/me/hosts, POST /api/me/hosts/<rid>/command and POST
// /api/me/hosts/<rid>/stop. The session cookie authenticates them and
// `apiClient` sends the CSRF header every POST needs.
//
// Each call answers a HostResult instead of throwing, with the server's
// `error` code mapped to a message the owner can act on.

import { isAxiosError } from 'axios';
import { apiClient } from './client';
import { formatRetryAfter, parseRetryAfter } from './session';
import type {
  HostedStopQueued,
  MyServer,
  MyServersResponse,
  PushQueued,
  ServerCommandBody,
} from '../types/myServers';

// The server's `error` codes these routes answer with, plus `network` (no
// reply at all) and `unknown` (a reply this client does not recognise).
export type HostErrorCode =
  | 'unauthorized'
  | 'csrf_header_required'
  | 'rate_limited'
  | 'invalid_json'
  | 'invalid_command'
  | 'verb_not_allowed'
  | 'unknown_rid'
  | 'not_hosting'
  | 'server_stopping'
  | 'no_session_listener'
  | 'db_error'
  | 'network'
  | 'unknown';

export interface HostError {
  code: HostErrorCode;
  // The HTTP status, or null when no reply arrived.
  status: number | null;
  // What to show the owner.
  message: string;
  // A 429's Retry-After, in seconds.
  retryAfterSeconds?: number;
}

export type HostResult<T> = { ok: true; value: T } | { ok: false; error: HostError };

const KNOWN_CODES: ReadonlySet<string> = new Set<HostErrorCode>([
  'unauthorized',
  'csrf_header_required',
  'rate_limited',
  'invalid_json',
  'invalid_command',
  'verb_not_allowed',
  'unknown_rid',
  'not_hosting',
  'server_stopping',
  'no_session_listener',
  'db_error',
]);

function messageFor(code: HostErrorCode, serverMessage: string, retryAfterSeconds?: number): string {
  switch (code) {
    case 'unauthorized':
      return 'Your session has ended. Log in again to manage your servers.';
    case 'csrf_header_required':
      return 'The server refused the request as not coming from this site. Reload the page and try again.';
    case 'rate_limited':
      return retryAfterSeconds !== undefined
        ? `Too many commands. Try again in ${formatRetryAfter(retryAfterSeconds)}.`
        : 'Too many commands. Try again in a little while.';
    case 'invalid_json':
      return 'The server could not read the request. Reload the page and try again.';
    case 'invalid_command':
      return serverMessage
        ? `The server refused the command: ${serverMessage}.`
        : 'The server refused the command.';
    case 'verb_not_allowed':
      return serverMessage || 'That command is not available to server owners.';
    case 'unknown_rid':
      return 'This server is no longer listed under your account. Refresh the list.';
    case 'not_hosting':
      return 'This server is not hosting right now. Refresh the list.';
    case 'server_stopping':
      return 'This hosted server is being stopped, so it takes no more commands.';
    case 'no_session_listener':
      return 'NovaWorld cannot reach game servers right now. Try again shortly.';
    case 'db_error':
      return 'The server hit a database error. Try again shortly.';
    case 'network':
      return 'Could not reach the server. Check your connection and try again.';
    case 'unknown':
      return 'Something went wrong. Try again shortly.';
  }
}

// Any failure of these calls, as a HostError.
export function describeHostError(err: unknown): HostError {
  if (isAxiosError(err)) {
    const response = err.response;
    if (!response) {
      return { code: 'network', status: null, message: messageFor('network', '') };
    }
    const data = response.data as { error?: unknown; message?: unknown } | undefined;
    const serverCode = typeof data?.error === 'string' ? data.error : '';
    const code: HostErrorCode = KNOWN_CODES.has(serverCode)
      ? (serverCode as HostErrorCode)
      : response.status === 429
        ? 'rate_limited'
        : response.status === 401
          ? 'unauthorized'
          : 'unknown';
    const serverMessage = typeof data?.message === 'string' ? data.message : '';
    const retryAfterSeconds =
      code === 'rate_limited' ? parseRetryAfter(response.headers?.['retry-after']) : undefined;
    return {
      code,
      status: response.status,
      message:
        code === 'unknown'
          ? `${messageFor('unknown', '')} (HTTP ${response.status})`
          : messageFor(code, serverMessage, retryAfterSeconds),
      ...(retryAfterSeconds !== undefined ? { retryAfterSeconds } : {}),
    };
  }
  return { code: 'unknown', status: null, message: messageFor('unknown', '') };
}

function isMyServersResponse(value: unknown): value is MyServersResponse {
  const hosts = (value as Partial<MyServersResponse> | null)?.hosts;
  return Array.isArray(hosts) && hosts.every((h) => typeof h?.rid === 'number' && Array.isArray(h?.roster));
}

// GET /api/me/hosts: the account's listed servers, oldest first.
export async function listMyServers(signal?: AbortSignal): Promise<HostResult<MyServer[]>> {
  try {
    const res = await apiClient.get<unknown>('/me/hosts', { signal });
    if (!isMyServersResponse(res.data)) {
      return { ok: false, error: { code: 'unknown', status: res.status, message: messageFor('unknown', '') } };
    }
    return { ok: true, value: res.data.hosts };
  } catch (err) {
    return { ok: false, error: describeHostError(err) };
  }
}

// POST /api/me/hosts/<rid>/command: a ServerCommand to one of the account's
// servers, queued for its NovaWorld session (202).
export async function sendServerCommand(
  rid: number,
  body: ServerCommandBody,
): Promise<HostResult<PushQueued>> {
  try {
    const res = await apiClient.post<PushQueued>(`/me/hosts/${rid}/command`, body);
    return { ok: true, value: res.data };
  } catch (err) {
    return { ok: false, error: describeHostError(err) };
  }
}

// POST /api/me/hosts/<rid>/stop: the sysop punt (ServerStopHosting, MsgCode
// 7). A stock host ends its running match for everyone on the server, goes to
// its main menu and leaves NovaWorld.
// On a hosted server's listing it is the hosted server's own stop instead
// (HostedStopQueued): its supervisor ends the match and the machine.
export async function stopHosting(rid: number): Promise<HostResult<PushQueued | HostedStopQueued>> {
  try {
    const res = await apiClient.post<PushQueued | HostedStopQueued>(`/me/hosts/${rid}/stop`);
    return { ok: true, value: res.data };
  } catch (err) {
    return { ok: false, error: describeHostError(err) };
  }
}
