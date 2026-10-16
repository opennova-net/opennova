// Admin endpoints: an admin-role session or the bearer token (see `client.ts`
// for the bearer-token interceptor). Every call answers an AdminResult instead
// of throwing, with the server's `error` code mapped to a message: the
// server's own `message` text is never shown. The users' calls are in
// users.ts, the CD keys' in cdKeys.ts, the billing overview's in
// adminBilling.ts; here are the NovaWorld service's status and sessions, and
// the hosted fleet (/api/admin/hosted*, /api/admin/fleet/*).

import { isAxiosError } from 'axios';
import { adminClient } from './client';
import { isHostedServer } from './hostedServer';
import type { AdminConnection, ConnectionsResponse, ServerStatus } from '../types/admin';
import type { FleetMachine, FleetQuery, FleetResponse, FleetServer } from '../types/hostedServer';

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
  | 'username_exists'
  | 'pcid_exists'
  | 'active_subscription'
  | 'revoked'
  | 'already_revoked'
  | 'already_assigned'
  | 'account_has_key'
  | 'billing_disabled'
  | 'stripe_unavailable'
  | 'rate_limited'
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
  'username_exists',
  'pcid_exists',
  'active_subscription',
  'revoked',
  'already_revoked',
  'already_assigned',
  'account_has_key',
  'billing_disabled',
  'stripe_unavailable',
  'rate_limited',
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
      return 'The server could not read the request. Reload the page and try again.';
    case 'invalid_field':
    case 'missing_field':
      return 'The server refused one of the values sent. Check them and try again.';
    case 'not_found':
      return 'It no longer exists. Refresh the list.';
    case 'not_live':
      return 'That hosted server has already ended. Refresh the list.';
    case 'no_such_account':
      return 'No player account has that name.';
    case 'username_exists':
      return 'Another account already has that username.';
    case 'pcid_exists':
      return 'Another account already has that PCID.';
    case 'active_subscription':
      return 'This account has a support tier that still grants hosting. Cancel it in Stripe first; the account can be deleted once it ends.';
    case 'revoked':
    case 'already_revoked':
      return 'That key is revoked. Refresh the list.';
    case 'already_assigned':
      return 'That key is already on an account. Refresh the list.';
    case 'account_has_key':
      return 'That account already holds a key for this game. Revoke it first.';
    case 'billing_disabled':
      return 'Billing is off on this NovaWorld service.';
    case 'stripe_unavailable':
      return 'Stripe could not take the request (the server log says why). Try again in a moment.';
    case 'rate_limited':
      return 'Too many requests. Try again in a little while.';
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
        : response.status === 429
          ? 'rate_limited'
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

// One admin request whose reply `pick` reads (null: malformed).
export async function adminCall<T>(
  request: () => Promise<{ status: number; data: unknown }>,
  pick: (data: unknown) => T | null,
): Promise<AdminResult<T>> {
  try {
    const res = await request();
    const value = pick(res.data);
    if (value === null) return malformedAdminReply(res.status);
    return { ok: true, value };
  } catch (err) {
    return { ok: false, error: describeAdminError(err) };
  }
}

// ---------------------------------------------------------------------------
// The NovaWorld service: maintenance mode, and the sessions signed in.

function pickStatus(data: unknown): ServerStatus | null {
  const v = data as Partial<ServerStatus> | null;
  return typeof v?.maintenance_enabled === 'boolean' && typeof v.message === 'string'
    ? { maintenance_enabled: v.maintenance_enabled, message: v.message }
    : null;
}

// GET /api/admin/server-status.
export function fetchServerStatus(signal?: AbortSignal): Promise<AdminResult<ServerStatus>> {
  return adminCall(() => adminClient.get<unknown>('/server-status', { signal }), pickStatus);
}

// PUT /api/admin/server-status.
export function updateServerStatus(payload: ServerStatus): Promise<AdminResult<ServerStatus>> {
  return adminCall(() => adminClient.put<unknown>('/server-status', payload), pickStatus);
}

function pickConnection(value: unknown): AdminConnection | null {
  const v = value as Record<string, unknown> | null;
  if (typeof v !== 'object' || v === null || typeof v.id !== 'number') return null;
  return {
    id: v.id,
    state: typeof v.state === 'string' ? v.state : '',
    pn: typeof v.pn === 'string' ? v.pn : typeof v.pn === 'number' ? String(v.pn) : '',
    identity: typeof v.identity === 'string' ? v.identity : '',
    created_ms: typeof v.created_ms === 'number' ? v.created_ms : 0,
    last_seen_ms: typeof v.last_seen_ms === 'number' ? v.last_seen_ms : 0,
    addr: typeof v.addr === 'string' ? v.addr : '',
  };
}

// GET /api/admin/connections: the NovaWorld sessions signed in now.
export function fetchConnections(signal?: AbortSignal): Promise<AdminResult<ConnectionsResponse>> {
  return adminCall(
    () => adminClient.get<unknown>('/connections', { signal }),
    (data) => {
      const v = data as Partial<Record<keyof ConnectionsResponse, unknown>> | null;
      if (typeof v !== 'object' || v === null || !Array.isArray(v.connections)) return null;
      const connections = v.connections.map(pickConnection);
      if (connections.some((c) => c === null)) return null;
      return {
        count: typeof v.count === 'number' ? v.count : connections.length,
        heartbeat_timeout_ms: typeof v.heartbeat_timeout_ms === 'number' ? v.heartbeat_timeout_ms : 0,
        connections: connections as AdminConnection[],
      };
    },
  );
}

// ---------------------------------------------------------------------------
// The hosted fleet.

const isCount = (v: unknown): v is number => Number.isInteger(v) && (v as number) >= 0;

function pickMachine(value: unknown): FleetMachine | null {
  const v = value as Record<string, unknown> | null;
  if (typeof v !== 'object' || v === null) return null;
  if ((typeof v.id !== 'string' && typeof v.id !== 'number') || (v.kind !== 'shared' && v.kind !== 'own')) return null;
  if (typeof v.state !== 'string' || !isCount(v.slots) || !isCount(v.slots_used)) return null;
  return {
    id: String(v.id),
    cloud_id: typeof v.cloud_id === 'string' ? v.cloud_id : '',
    kind: v.kind,
    state: v.state,
    instance_type: typeof v.instance_type === 'string' ? v.instance_type : '',
    slots: v.slots,
    slots_used: v.slots_used,
    public_address: typeof v.public_address === 'string' ? v.public_address : '',
    egress_bytes_month: typeof v.egress_bytes_month === 'number' ? v.egress_bytes_month : 0,
  };
}

function pickFleetServer(value: unknown): FleetServer | null {
  if (!isHostedServer(value)) return null;
  const v = value as unknown as Record<string, unknown>;
  const owner = v.owner as Record<string, unknown> | null | undefined;
  const tier = owner?.tier;
  return {
    ...(value as FleetServer),
    owner:
      owner && typeof owner.id === 'number' && typeof owner.username === 'string'
        ? { id: owner.id, username: owner.username, tier: tier === 1 || tier === 2 || tier === 3 ? tier : null }
        : null,
    machine_ref: typeof v.machine_ref === 'string' || typeof v.machine_ref === 'number' ? String(v.machine_ref) : null,
    slot: isCount(v.slot) ? v.slot : null,
    game_port: isCount(v.game_port) ? v.game_port : null,
    players: isCount(v.players) ? v.players : null,
  };
}

function pickFleet(data: unknown): FleetResponse | null {
  const v = data as Record<string, unknown> | null;
  if (typeof v !== 'object' || v === null) return null;
  if (typeof v.enabled !== 'boolean' || !isCount(v.cap) || !isCount(v.live)) return null;
  if (!Array.isArray(v.servers) || !Array.isArray(v.machines)) return null;
  const servers = v.servers.map(pickFleetServer);
  const machines = v.machines.map(pickMachine);
  if (servers.some((s) => s === null) || machines.some((m) => m === null)) return null;
  return {
    enabled: v.enabled,
    cap: v.cap,
    live: v.live,
    machines: machines as FleetMachine[],
    servers: servers as FleetServer[],
  };
}

// GET /api/admin/hosted?owner=&state=: the machines and the servers the
// filter selects, live ones first.
export function fetchHostedFleet(query: FleetQuery = {}, signal?: AbortSignal): Promise<AdminResult<FleetResponse>> {
  const params: Record<string, string> = {};
  if (query.owner) params.owner = query.owner;
  if (query.state) params.state = query.state;
  return adminCall(() => adminClient.get<unknown>('/hosted', { params, signal }), pickFleet);
}

// GET /api/admin/hosted/<id>: one hosted server with its events, its owner
// and its machine (the reply's `machine`, set on the server here).
export function fetchFleetServer(id: number, signal?: AbortSignal): Promise<AdminResult<FleetServer>> {
  return adminCall(
    () => adminClient.get<unknown>(`/hosted/${id}`, { signal }),
    (data) => {
      const v = data as { server?: unknown; machine?: unknown } | null;
      const server = pickFleetServer(v?.server);
      if (!server) return null;
      const machineIn = v?.machine ?? (v?.server as { machine?: unknown } | undefined)?.machine ?? null;
      return { ...server, machine: machineIn === null ? null : pickMachine(machineIn) };
    },
  );
}

// A POST whose reply body the page does not need (it reads again after).
export async function adminPost(path: string, body?: object): Promise<AdminResult<void>> {
  try {
    await adminClient.post<unknown>(path, body);
    return { ok: true, value: undefined };
  } catch (err) {
    return { ok: false, error: describeAdminError(err) };
  }
}

// POST /api/admin/hosted/<id>/stop: one live server to stopping (202); the
// server as the reply has it, null when it named none it could read.
export async function stopFleetServer(id: number): Promise<AdminResult<FleetServer | null>> {
  try {
    const res = await adminClient.post<unknown>(`/hosted/${id}/stop`);
    const server = (res.data as { server?: unknown } | null)?.server;
    return { ok: true, value: server == null ? null : pickFleetServer(server) };
  } catch (err) {
    return { ok: false, error: describeAdminError(err) };
  }
}

// POST /api/admin/hosted/stop-all: the kill switch, every live server to
// stopping; how many were asked.
export function stopAllFleetServers(): Promise<AdminResult<number>> {
  return adminCall(
    () => adminClient.post<unknown>('/hosted/stop-all'),
    (data) => {
      const stopping = (data as { stopping?: unknown } | null)?.stopping;
      return typeof stopping === 'number' ? stopping : null;
    },
  );
}

// POST /api/admin/fleet/machines/<id>/drain or /end: a machine takes no new
// server and its servers move to other machines (drain: one at a time, then it
// ends; end: all at once, now), no restart counted. Any 2xx is taken; a
// machine that has ended already answers 409 not_live, told as a machine's.
export async function machineAction(id: string, action: 'drain' | 'end'): Promise<AdminResult<void>> {
  const result = await adminPost(`/fleet/machines/${encodeURIComponent(id)}/${action}`);
  if (!result.ok && result.error.code === 'not_live') {
    return { ok: false, error: { ...result.error, message: 'That machine has already ended. Refresh the list.' } };
  }
  return result;
}

// POST /api/admin/fleet/roll: every shared machine drains and its servers move
// to fresh machines, without counting a restart.
export function rollFleet(): Promise<AdminResult<void>> {
  return adminPost('/fleet/roll');
}
