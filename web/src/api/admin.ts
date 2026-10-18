// Admin endpoints: an admin-role session or the bearer token (see `client.ts`
// for the bearer-token interceptor). Every call answers an AdminResult instead
// of throwing, with the server's `error` code mapped to a message: the
// server's own `message` text is never shown. The users' calls are in
// users.ts, the CD keys' in cdKeys.ts, the billing overview's in
// adminBilling.ts, the background jobs' in jobs.ts, the infrastructure view's in infra.ts; here are the
// NovaWorld service's status and sessions, and
// the hosted fleet (/api/admin/hosted*, /api/admin/fleet/*).

import { isAxiosError } from 'axios';
import { adminClient } from './client';
import { parseHostedServer } from './hostedServer';
import type {
  AdminConnection,
  AdminConnectionAccount,
  AdminConnectionHost,
  ConnectionsResponse,
  ServerStatus,
} from '../types/admin';
import type { FleetMachine, FleetQuery, FleetResponse, FleetRoom, FleetServer } from '../types/hostedServer';

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
  | 'hosting'
  | 'not_hosting'
  | 'unknown_rid'
  | 'not_failed'
  | 'not_queued'
  | 'secret_dropped'
  | 'already_pending'
  // The game data routes (dataSets.ts).
  | 'invalid_files'
  | 'upload_in_flight'
  | 'store_unavailable'
  | 'store_error'
  | 'not_uploading'
  | 'too_large'
  | 'duplicate_login'
  | 'duplicate_expansion'
  | 'no_such_parent'
  | 'in_use'
  | 'current'
  | 'baseline'
  | 'pinned'
  | 'not_draft'
  | 'not_published'
  | 'published'
  | 'up_to_date'
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
  'hosting',
  'not_hosting',
  'unknown_rid',
  'not_failed',
  'not_queued',
  'secret_dropped',
  'already_pending',
  'invalid_files',
  'upload_in_flight',
  'store_unavailable',
  'store_error',
  'not_uploading',
  'too_large',
  'duplicate_login',
  'duplicate_expansion',
  'no_such_parent',
  'in_use',
  'current',
  'baseline',
  'pinned',
  'not_draft',
  'not_published',
  'published',
  'up_to_date',
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
    case 'hosting':
      return 'That session hosts a server now: stop the server instead. Refresh the list.';
    case 'not_hosting':
    case 'unknown_rid':
      return 'That server is no longer listed. Refresh the list.';
    case 'not_failed':
      return 'That job has not failed: it is waiting, running or done. Refresh the page.';
    case 'not_queued':
      return 'That job is no longer waiting: a worker took it, or it ended. Refresh the page.';
    case 'secret_dropped':
      return 'That job carried a secret (a mail, a machine launch token) that was dropped when it ended, so it cannot run again.';
    case 'already_pending':
      return 'A job of that kind, or with the same key, is already waiting or running.';
    case 'invalid_files':
      return 'The files picked do not make a tree of that kind. Check the list and pick the folder again.';
    case 'upload_in_flight':
      return 'An upload to this data set is already in progress. Wait for it to finish, or delete the uploading tree first.';
    case 'store_unavailable':
      return 'The object store is not reachable right now. Try again shortly.';
    case 'store_error':
      return 'The object store refused the request (the server log says why). Try again shortly.';
    case 'not_uploading':
      return 'That tree is no longer taking uploads. Refresh the list.';
    case 'too_large':
      return 'The server refused an upload part as too large. Reload the page and upload again.';
    case 'duplicate_login':
      return 'Another data set already uses that updater login.';
    case 'duplicate_expansion':
      return 'That base already has an expansion with that code.';
    case 'no_such_parent':
      return 'The base that expansion would belong to no longer exists. Refresh the list.';
    case 'in_use':
      return 'That data set is in use: a live server runs it, or an expansion belongs to it. Stop the servers or delete the expansion first.';
    case 'current':
      return "That tree is the data set's current data, so it stays. Upload or publish newer data first.";
    case 'baseline':
      return 'That tree is the baseline a release applies to, so it stays.';
    case 'pinned':
      return 'Live servers run on that tree, so it stays until they update or stop.';
    case 'not_draft':
      return 'Only a draft release can be published. Refresh the list.';
    case 'not_published':
      return 'That release is not published. Refresh the list.';
    case 'published':
      return 'A published release cannot be deleted. Retire it first.';
    case 'up_to_date':
      return 'That server already runs the current data.';
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

function pickConnectionAccount(value: unknown): AdminConnectionAccount | null {
  const v = value as Record<string, unknown> | null;
  if (typeof v !== 'object' || v === null || typeof v.id !== 'number' || typeof v.username !== 'string') return null;
  return { id: v.id, username: v.username, via: v.via === 'host_key' ? 'host_key' : 'login' };
}

function pickConnectionHost(value: unknown): AdminConnectionHost | null {
  const v = value as Record<string, unknown> | null;
  if (typeof v !== 'object' || v === null || typeof v.rid !== 'number') return null;
  return {
    rid: v.rid,
    server_name: typeof v.server_name === 'string' ? v.server_name : '',
    mission_name: typeof v.mission_name === 'string' ? v.mission_name : '',
    players: typeof v.players === 'number' ? v.players : 0,
    max_players: typeof v.max_players === 'number' ? v.max_players : 0,
    listed: v.listed !== false,
    hosted_server_id: typeof v.hosted_server_id === 'number' ? v.hosted_server_id : null,
  };
}

function pickConnection(value: unknown): AdminConnection | null {
  const v = value as Record<string, unknown> | null;
  if (typeof v !== 'object' || v === null || typeof v.id !== 'number' || typeof v.session_id !== 'number') return null;
  return {
    session_id: v.session_id,
    lobby_id: typeof v.lobby_id === 'number' ? v.lobby_id : 0,
    id: v.id,
    state: typeof v.state === 'string' ? v.state : '',
    pn: typeof v.pn === 'string' ? v.pn : typeof v.pn === 'number' ? String(v.pn) : '',
    identity: typeof v.identity === 'string' ? v.identity : '',
    created_ms: typeof v.created_ms === 'number' ? v.created_ms : 0,
    last_seen_ms: typeof v.last_seen_ms === 'number' ? v.last_seen_ms : 0,
    addr: typeof v.addr === 'string' ? v.addr : '',
    account: pickConnectionAccount(v.account),
    host: pickConnectionHost(v.host),
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

// POST /api/admin/connections/<session id>/end: the NovaWorld service ends one
// session (202), named by its row's id, which no other session ever takes (a
// registry id and an address are one lobby instance's alone). A hosting session
// is refused ('hosting'): its server stops through stopListedServer.
export function endConnection(c: Pick<AdminConnection, 'session_id'>): Promise<AdminResult<void>> {
  return adminPost(`/connections/${c.session_id}/end`);
}

// POST /api/admin/hosts/<rid>/stop: a listed server stops (202): a hosted
// server as its owner's stop would, any other with the sysop punt, which ends
// its match and takes it out of NovaWorld.
export function stopListedServer(rid: number): Promise<AdminResult<void>> {
  return adminPost(`/hosts/${rid}/stop`);
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
  const server = parseHostedServer(value);
  if (!server) return null;
  const v = value as Record<string, unknown>;
  const owner = v.owner as Record<string, unknown> | null | undefined;
  const tier = owner?.tier;
  return {
    ...server,
    owner:
      owner && typeof owner.id === 'number' && typeof owner.username === 'string'
        ? { id: owner.id, username: owner.username, tier: tier === 1 || tier === 2 || tier === 3 ? tier : null }
        : null,
    machine_ref: typeof v.machine_ref === 'string' || typeof v.machine_ref === 'number' ? String(v.machine_ref) : null,
    slot: isCount(v.slot) ? v.slot : null,
    game_port: isCount(v.game_port) ? v.game_port : null,
    players: isCount(v.players) ? v.players : null,
    base_tree_id: isCount(v.base_tree_id) ? v.base_tree_id : null,
    expansion_tree_id: isCount(v.expansion_tree_id) ? v.expansion_tree_id : null,
    outdated: v.outdated === true,
  };
}

function pickRoom(data: unknown): FleetRoom | null {
  const v = data as Record<string, unknown> | null;
  if (typeof v !== 'object' || v === null || !isCount(v.live) || !isCount(v.room)) return null;
  return { live: v.live, room: v.room };
}

function pickFleet(data: unknown): FleetResponse | null {
  const v = data as Record<string, unknown> | null;
  if (typeof v !== 'object' || v === null) return null;
  if (typeof v.enabled !== 'boolean' || !isCount(v.cap) || !isCount(v.live)) return null;
  const shared = pickRoom(v.shared);
  const own = pickRoom(v.own);
  if (!shared || !own) return null;
  if (!Array.isArray(v.servers) || !Array.isArray(v.machines)) return null;
  const servers = v.servers.map(pickFleetServer);
  const machines = v.machines.map(pickMachine);
  if (servers.some((s) => s === null) || machines.some((m) => m === null)) return null;
  return {
    enabled: v.enabled,
    cap: v.cap,
    live: v.live,
    shared,
    own,
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
