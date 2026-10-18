// The logged-in account's hosted servers (apps/novaworld_common/hosted/README.md;
// hosted/hosted_routes.cpp): GET /api/me/servers (the
// hosting entitlement, the data sets and the servers), GET
// /api/me/servers/<id>, POST /api/me/servers (a start), POST
// /api/me/servers/<id>/stop and POST /api/me/servers/<id>/update (a restart
// on the data set's current data). The session cookie authenticates them and
// `apiClient` sends the CSRF header the POSTs need. Below them, the console
// routes (hosted/console_routes.cpp), the owner's and the admin's.
//
// Each call answers a HostedResult (a ConsoleResult) instead of throwing, with
// the server's `error` code mapped to a message the owner can act on. The
// server's own `message` text is never shown.

import { isAxiosError, type AxiosInstance } from 'axios';
import { adminClient, apiClient } from './client';
import { formatRetryAfter, parseRetryAfter } from './session';
import type {
  ConsoleLine,
  ConsoleLink,
  ConsoleLog,
  ConsoleMission,
  ConsolePlayer,
  EntitlementSource,
  HostedDataSet,
  HostedExpansionOption,
  HostedMission,
  HostedServer,
  HostedServerData,
  HostedServersResponse,
  HostedSettings,
  HostingEntitlement,
  PinnedTree,
  PlayerAction,
  PlayerActionReply,
  RotationEntry,
  RotationEntryAction,
  StartSettings,
} from '../types/hostedServer';
import { FIELD_RULES, INT_FIELDS, MISSIONS_MAX, isSettingsField, type SettingsField } from '../utils/hostedSettings';

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
  | 'not_entitled'
  | 'payment_past_due'
  | 'server_limit'
  | 'no_own_machine'
  | 'at_capacity'
  | 'not_found'
  | 'not_live'
  | 'up_to_date'
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
  // server_limit: the account's entitlement and how many of its servers are
  // live, when the reply said.
  maxServers?: number;
  used?: number;
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
  'not_entitled',
  'payment_past_due',
  'server_limit',
  'no_own_machine',
  'at_capacity',
  'not_found',
  'not_live',
  'up_to_date',
  'db_error',
]);

// The refusals a support tier (or its add-on) answers: the page shows a link
// to the account's support tier page beside their message.
const SUPPORT_TIER_CODES: ReadonlySet<HostedErrorCode> = new Set<HostedErrorCode>([
  'not_entitled',
  'payment_past_due',
  'server_limit',
  'no_own_machine',
]);

export function linksToSupportTier(code: HostedErrorCode): boolean {
  return SUPPORT_TIER_CODES.has(code);
}

// Why a stop does not free a place at once (the service counts a stopping
// server as live until its machine reports it gone).
export const STOPPING_STILL_COUNTS =
  'A stopping server still counts toward your limit until it has stopped, in about a minute.';

// "1 server", "3 servers".
function servers(n: number): string {
  return `${n} server${n === 1 ? '' : 's'}`;
}

function messageFor(
  code: HostedErrorCode,
  extra: { field?: SettingsField; retryAfterSeconds?: number; maxServers?: number } = {},
): string {
  switch (code) {
    case 'unauthorized':
      return 'Your session has ended. Log in again to manage your hosted servers.';
    case 'csrf_header_required':
      return 'The server refused the request as not coming from this site. Reload the page and try again.';
    case 'proxy_required':
      return 'The server refused a request that did not come through this site. Reload the page and try again.';
    case 'hosting_disabled':
      return 'NovaWorld is not running hosted servers right now.';
    case 'rate_limited':
      return extra.retryAfterSeconds !== undefined
        ? `Too many starts and stops. Try again in ${formatRetryAfter(extra.retryAfterSeconds)}.`
        : 'Too many starts and stops. Try again in a little while.';
    case 'invalid_json':
      return 'The server could not read the request. Reload the page and try again.';
    case 'invalid_field':
      return 'The server could not read one of the settings. Reload the page and try again.';
    case 'invalid_settings':
      return extra.field
        ? `The server refused the settings. ${FIELD_RULES[extra.field]}`
        : 'The server refused the settings. Check them and try again.';
    case 'not_entitled':
      return 'Hosting a server needs a support tier.';
    case 'payment_past_due':
      return "Your support tier's last payment did not go through, so new servers can't start. Your running servers keep running while Stripe retries the card.";
    case 'server_limit':
      return `${
        extra.maxServers !== undefined
          ? `You may run ${servers(extra.maxServers)} at once, and all of them are live.`
          : 'All the servers you may run at once are live.'
      } Stop one, or move to a bigger support tier. ${STOPPING_STILL_COUNTS}`;
    case 'no_own_machine':
      return 'Each of your own machines already runs a server. Start this one on a shared machine, or add another own machine.';
    case 'at_capacity':
      return 'Every hosted server place on NovaWorld is taken right now. Try again later.';
    case 'not_found':
      return 'That hosted server no longer exists. Refresh the list.';
    case 'not_live':
      return 'That hosted server has already ended.';
    case 'up_to_date':
      return 'Your server already runs the current data.';
    case 'db_error':
      return 'The server hit a database error. Try again shortly.';
    case 'network':
      return 'Could not reach the server. Check your connection and try again.';
    case 'unknown':
      return 'Something went wrong. Try again shortly.';
  }
}

const isCount = (v: unknown): v is number => Number.isInteger(v) && (v as number) >= 0;

// Any failure of these calls, as a HostedError.
export function describeHostedError(err: unknown): HostedError {
  if (isAxiosError(err)) {
    const response = err.response;
    if (!response) {
      return { code: 'network', status: null, message: messageFor('network') };
    }
    const data = response.data as { error?: unknown; field?: unknown; max_servers?: unknown; used?: unknown } | undefined;
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
    const maxServers = code === 'server_limit' && isCount(data?.max_servers) ? data.max_servers : undefined;
    const used = code === 'server_limit' && isCount(data?.used) ? data.used : undefined;
    return {
      code,
      status: response.status,
      message:
        code === 'unknown'
          ? `${messageFor('unknown')} (HTTP ${response.status})`
          : messageFor(code, { field, retryAfterSeconds, maxServers }),
      ...(field !== undefined ? { field } : {}),
      ...(retryAfterSeconds !== undefined ? { retryAfterSeconds } : {}),
      ...(maxServers !== undefined ? { maxServers } : {}),
      ...(used !== undefined ? { used } : {}),
    };
  }
  return { code: 'unknown', status: null, message: messageFor('unknown') };
}

function malformed(status: number): HostedResult<never> {
  return { ok: false, error: { code: 'unknown', status, message: messageFor('unknown') } };
}

function isHostedServerShape(value: unknown): value is HostedServer {
  const v = value as Partial<HostedServer> | null;
  return (
    typeof v === 'object' &&
    v !== null &&
    typeof v.id === 'number' &&
    typeof v.state === 'string' &&
    typeof v.live === 'boolean' &&
    typeof v.own_machine === 'boolean' &&
    typeof v.settings === 'object' &&
    v.settings !== null &&
    Array.isArray((v.settings as Partial<HostedSettings>).missions)
  );
}

// A data set or expansion a server names ({id, display_name}, with an
// expansion's code), or null.
function parseNamed<T extends { id: number; display_name: string }>(value: unknown, extra: (v: Record<string, unknown>) => Omit<T, 'id' | 'display_name'>): T | null {
  const v = value as Record<string, unknown> | null;
  if (typeof v !== 'object' || v === null || !isCount(v.id)) return null;
  return { id: v.id, display_name: typeof v.display_name === 'string' ? v.display_name : '', ...extra(v) } as T;
}

function parsePinnedTree(value: unknown): PinnedTree | null {
  const v = value as Record<string, unknown> | null;
  if (typeof v !== 'object' || v === null || !isCount(v.tree_id)) return null;
  return { tree_id: v.tree_id, version: typeof v.version === 'string' ? v.version : '' };
}

function parseServerData(value: unknown): HostedServerData {
  const v = value as Record<string, unknown> | null;
  if (typeof v !== 'object' || v === null) return { base: null, expansion: null };
  return { base: parsePinnedTree(v.base), expansion: parsePinnedTree(v.expansion) };
}

// A hosted server as the routes answer it, or null when it lacks what every
// page needs (its id, state, liveness and settings). The data it runs on
// (its data set, expansion, trees and update flags) is read with defaults,
// so a reply that leaves them out still shows.
export function parseHostedServer(value: unknown): HostedServer | null {
  if (!isHostedServerShape(value)) return null;
  const v = value as unknown as Record<string, unknown>;
  return {
    ...value,
    game_display_name: typeof v.game_display_name === 'string' ? v.game_display_name : '',
    expansion_display_name: typeof v.expansion_display_name === 'string' ? v.expansion_display_name : '',
    data_set: parseNamed(v.data_set, () => ({})),
    expansion: parseNamed(v.expansion, (e) => ({ code: typeof e.code === 'string' ? e.code : '' })),
    data: parseServerData(v.data),
    update_available: v.update_available === true,
    update_requested: v.update_requested === true,
  };
}

const SOURCES: ReadonlySet<unknown> = new Set<EntitlementSource>(['none', 'tier', 'comp']);

// The entitlement, or null when any count is missing or not a whole number
// (the page never offers a start it could not count).
function parseEntitlement(value: unknown): HostingEntitlement | null {
  const v = value as Partial<Record<keyof HostingEntitlement, unknown>> | null;
  if (typeof v !== 'object' || v === null) return null;
  const { max_servers: max, used, own_machines: own, own_used: ownUsed, can_start: canStart, source } = v;
  if (!isCount(max) || !isCount(used) || !isCount(own) || !isCount(ownUsed)) return null;
  if (typeof canStart !== 'boolean' || !SOURCES.has(source)) return null;
  return {
    max_servers: max,
    used,
    own_machines: own,
    own_used: ownUsed,
    can_start: canStart,
    source: source as EntitlementSource,
  };
}

// The missions a tree lists ({file, title}), the malformed entries left out.
function parseMissions(value: unknown): HostedMission[] {
  if (!Array.isArray(value)) return [];
  const out: HostedMission[] = [];
  for (const m of value as (Record<string, unknown> | null)[]) {
    if (typeof m?.file !== 'string' || m.file === '') continue;
    out.push({ file: m.file, title: typeof m.title === 'string' ? m.title : '' });
  }
  return out;
}

// One expansion of a data set, or null when the entry is malformed.
function parseExpansion(value: unknown): HostedExpansionOption | null {
  const v = value as Partial<Record<keyof HostedExpansionOption, unknown>> | null;
  if (typeof v !== 'object' || v === null || !isCount(v.id) || typeof v.code !== 'string') return null;
  return {
    id: v.id,
    code: v.code,
    display_name: typeof v.display_name === 'string' ? v.display_name : v.code,
    version: typeof v.version === 'string' ? v.version : '',
    missions: parseMissions(v.missions),
  };
}

// One offered data set, or null when the entry is malformed (no id or name,
// a malformed expansion, a range that is no pair of whole numbers): the form
// never offers a game it could not check.
function parseDataSet(value: unknown): HostedDataSet | null {
  const v = value as Partial<Record<keyof HostedDataSet, unknown>> | null;
  if (typeof v !== 'object' || v === null) return null;
  if (!isCount(v.id) || typeof v.display_name !== 'string') return null;
  const expansions = Array.isArray(v.expansions) ? v.expansions.map(parseExpansion) : [];
  if (expansions.some((e) => e === null)) return null;
  const limitsIn = v.limits as Record<string, { min?: unknown; max?: unknown } | null> | undefined;
  if (typeof limitsIn !== 'object' || limitsIn === null) return null;
  const limits = {} as HostedDataSet['limits'];
  for (const field of INT_FIELDS) {
    const range = limitsIn[field];
    const min = range?.min;
    const max = range?.max;
    if (!Number.isInteger(min) || !Number.isInteger(max) || (min as number) > (max as number)) return null;
    limits[field] = { min: min as number, max: max as number };
  }
  return {
    id: v.id,
    display_name: v.display_name,
    title: typeof v.title === 'string' ? v.title : '',
    version: typeof v.version === 'string' ? v.version : '',
    missions: parseMissions(v.missions),
    limits,
    expansions: expansions as HostedExpansionOption[],
  };
}

// GET /api/me/servers: whether hosting is on (and full), the account's
// hosting entitlement, the data sets a start may pick and the account's
// servers (the live ones, then its newest ended ones). A reply whose
// entitlement or any server is malformed is refused whole: the page never
// counts servers it could not read.
export async function fetchMyServers(signal?: AbortSignal): Promise<HostedResult<HostedServersResponse>> {
  try {
    const res = await apiClient.get<unknown>('/me/servers', { signal });
    const data = res.data as Partial<Record<keyof HostedServersResponse, unknown>> | null;
    if (typeof data !== 'object' || data === null) return malformed(res.status);
    const entitlement = parseEntitlement(data.entitlement);
    const list = data.servers;
    if (typeof data.enabled !== 'boolean' || typeof data.full !== 'boolean' || entitlement === null || !Array.isArray(list)) {
      return malformed(res.status);
    }
    const servers = list.map(parseHostedServer);
    if (servers.some((s) => s === null)) return malformed(res.status);
    const dataSets = Array.isArray(data.data_sets)
      ? data.data_sets.map(parseDataSet).filter((d): d is HostedDataSet => d !== null)
      : [];
    return {
      ok: true,
      value: { enabled: data.enabled, full: data.full, entitlement, data_sets: dataSets, servers: servers as HostedServer[] },
    };
  } catch (err) {
    return { ok: false, error: describeHostedError(err) };
  }
}

async function forServer(request: () => Promise<{ status: number; data: unknown }>): Promise<HostedResult<HostedServer>> {
  try {
    const res = await request();
    const server = parseHostedServer((res.data as { server?: unknown } | null)?.server);
    if (!server) return malformed(res.status);
    return { ok: true, value: server };
  } catch (err) {
    return { ok: false, error: describeHostedError(err) };
  }
}

// GET /api/me/servers/<id>: one of the account's servers, with its events;
// 404 not_found for any id that is not the account's.
export function fetchMyServer(id: number, signal?: AbortSignal): Promise<HostedResult<HostedServer>> {
  return forServer(() => apiClient.get<unknown>(`/me/servers/${id}`, { signal }));
}

// POST /api/me/servers: a new server, requested (202), with these settings,
// on one of the account's own machines when `ownMachine`. The service's
// supervisor takes it from there.
export function startHostedServer(settings: StartSettings, ownMachine = false): Promise<HostedResult<HostedServer>> {
  return forServer(() => apiClient.post<unknown>('/me/servers', { ...settings, own_machine: ownMachine }));
}

// POST /api/me/servers/<id>/stop: the server to stopping (202; also when it
// already was), or stopped when the supervisor finished it meanwhile; 409
// not_live once it has ended.
export function stopHostedServer(id: number): Promise<HostedResult<HostedServer>> {
  return forServer(() => apiClient.post<unknown>(`/me/servers/${id}/stop`));
}

// POST /api/me/servers/<id>/update: the server restarts on its data set's
// current data (202; again when it was asked already): its match ends and
// its players drop. 409 not_live once it has ended, up_to_date when it runs
// the current data already.
export function updateHostedServer(id: number): Promise<HostedResult<HostedServer>> {
  return forServer(() => apiClient.post<unknown>(`/me/servers/${id}/update`));
}

// ---------------------------------------------------------------------------
// The console routes (hosted/console_routes.cpp; README "Hosted servers", the
// console routes). The owner's act on one of the account's own servers, by id
// (`apiClient`, which sends the CSRF header every POST needs; another
// account's id answers 404 not_found); the admin's on any server
// (`adminClient`: an admin session or the stored Bearer token). The service
// runs at most one console request per account at once (an admin session's
// account; the Bearer token counts as one account) and answers a second 429
// console_busy at once, so a page sends every one of these calls the service
// admits through its one ConsoleQueue (utils/consoleQueue.ts); the console
// log's read, which it does not admit, keeps out of it.

// Where a console call goes: server `id`, through the owner's routes or the
// admin's.
export interface ConsoleTarget {
  kind: 'owner' | 'admin';
  id: number;
}

// The chat text a server line keeps: cp1252 characters, after the text is
// trimmed (console_commands.h kChatTextMaxBytes).
export const CHAT_TEXT_MAX = 59;
// The console ring's length, the most lines one read returns
// (hosted_repository.h kConsoleKeep).
export const CONSOLE_KEEP = 500;

// The `error` codes the console routes answer with, plus `network` (no reply
// at all) and `unknown` (a reply this client does not recognise).
export type ConsoleErrorCode =
  | 'unauthorized'
  | 'forbidden'
  | 'csrf_header_required'
  | 'proxy_required'
  | 'not_found'
  | 'not_live'
  | 'server_stopping'
  | 'server_not_running'
  | 'admin_unavailable'
  | 'admin_busy'
  | 'console_busy'
  | 'console_full'
  | 'rotation_busy'
  | 'rate_limited'
  | 'admin_timeout'
  | 'admin_error'
  | 'not_in_game'
  | 'rotation_full'
  | 'last_mission'
  | 'no_such_entry'
  | 'mission_not_found'
  | 'player_not_found'
  | 'no_pcid'
  | 'not_connected'
  | 'invalid_slot'
  | 'invalid_index'
  | 'invalid_mission'
  | 'invalid_text'
  | 'invalid_json'
  | 'invalid_field'
  | 'missing_field'
  | 'internal_error'
  | 'db_error'
  | 'network'
  | 'unknown';

export interface ConsoleError {
  code: ConsoleErrorCode;
  // The HTTP status, or null when no reply arrived.
  status: number | null;
  // What to show.
  message: string;
  // A 429's or a busy 503's Retry-After, in seconds.
  retryAfterSeconds?: number;
  // admin_timeout: whether the command went out before the wait ran out (it
  // may then still take effect). Absent when the reply did not say.
  sent?: boolean;
}

export type ConsoleResult<T> = { ok: true; value: T } | { ok: false; error: ConsoleError };

const KNOWN_CONSOLE_CODES: ReadonlySet<string> = new Set<ConsoleErrorCode>([
  'unauthorized',
  'forbidden',
  'csrf_header_required',
  'proxy_required',
  'not_found',
  'not_live',
  'server_stopping',
  'server_not_running',
  'admin_unavailable',
  'admin_busy',
  'console_busy',
  'console_full',
  'rotation_busy',
  'rate_limited',
  'admin_timeout',
  'admin_error',
  'not_in_game',
  'rotation_full',
  'last_mission',
  'no_such_entry',
  'mission_not_found',
  'player_not_found',
  'no_pcid',
  'not_connected',
  'invalid_slot',
  'invalid_index',
  'invalid_mission',
  'invalid_text',
  'invalid_json',
  'invalid_field',
  'missing_field',
  'internal_error',
  'db_error',
]);

// The refusals that mean the server is not running (any more): a page reads
// its server again rather than keep asking its console.
export function isConsoleGone(code: ConsoleErrorCode): boolean {
  return (
    code === 'not_found' ||
    code === 'not_live' ||
    code === 'server_stopping' ||
    code === 'server_not_running'
  );
}

// `read`: the call only reads (a poll), so nothing it asked can have taken
// effect.
function consoleMessageFor(
  code: ConsoleErrorCode,
  target: ConsoleTarget,
  extra: { retryAfterSeconds?: number; sent?: boolean; read?: boolean } = {},
): string {
  switch (code) {
    case 'unauthorized':
      return target.kind === 'admin'
        ? 'Your admin credentials were refused. Log in again or enter the admin token.'
        : 'Your session has ended. Log in again to manage your hosted servers.';
    case 'forbidden':
      return 'This account is not an admin.';
    case 'csrf_header_required':
      return 'The server refused the request as not coming from this site. Reload the page and try again.';
    case 'proxy_required':
      return 'The server refused a request that did not come through this site. Reload the page and try again.';
    case 'not_found':
      return 'That hosted server no longer exists. Refresh the list.';
    case 'not_live':
      return 'That hosted server has ended.';
    case 'server_stopping':
      return 'The server is stopping, so its console is closed.';
    case 'server_not_running':
      return 'The server is not running yet, so its console is not open.';
    case 'admin_unavailable':
      return "The service's console link to the game server is down right now. It reconnects by itself; try again shortly.";
    case 'admin_busy':
      return "The game server's console is busy with other commands. Try again shortly.";
    case 'console_busy':
      return 'Your previous console request is still running. Try again in a moment.';
    case 'console_full':
      return "The service's console is busy with other servers' requests. Try again shortly.";
    case 'rotation_busy':
      return "Another change to this server's rotation is running. Try again in a moment.";
    case 'rate_limited':
      return extra.retryAfterSeconds !== undefined
        ? `Too many console requests. Try again in ${formatRetryAfter(extra.retryAfterSeconds)}.`
        : 'Too many console requests. Try again in a little while.';
    case 'admin_timeout':
      if (extra.read) {
        return 'The game server did not answer in time (a map load holds its console up for a few seconds). The console reads it again shortly.';
      }
      if (extra.sent === true) {
        return 'The game server did not answer in time. The command went out, so it may still take effect: check before sending it again.';
      }
      if (extra.sent === false) {
        return 'The game server did not answer in time, and the command was not sent. Try again shortly.';
      }
      return 'The game server did not answer in time. Try again shortly.';
    case 'admin_error':
      return 'The game server answered the command with something this console does not expect.';
    case 'not_in_game':
      return 'The server is between matches. Try again once the next map has loaded.';
    case 'rotation_full':
      return `The rotation holds ${MISSIONS_MAX} maps, all this console builds. Remove one first.`;
    case 'last_mission':
      return "The rotation's last map stays: without it the server's session would end.";
    case 'no_such_entry':
      return 'That rotation entry is gone: the rotation changed. It is read again.';
    case 'mission_not_found':
      return 'The game server has no such mission.';
    case 'player_not_found':
      return 'No player holds that slot any more: they left. The player list is read again.';
    case 'no_pcid':
      return 'That player has no NovaWorld PCID to ban (a LAN player). Kick them instead.';
    case 'not_connected':
      return 'That player has no network connection to ban.';
    case 'invalid_slot':
      return "The server's own slot cannot be acted on.";
    case 'invalid_index':
      return `A rotation entry is numbered 0 to ${MISSIONS_MAX - 1}.`;
    case 'invalid_mission':
      return 'That is not a map name. Pick a map from the list.';
    case 'invalid_text':
      return `A chat line is 1 to ${CHAT_TEXT_MAX} characters. Use only standard letters, numbers and punctuation.`;
    case 'invalid_json':
    case 'invalid_field':
    case 'missing_field':
      return 'The server could not read the request. Reload the page and try again.';
    case 'internal_error':
    case 'db_error':
      return 'The service hit an error of its own. Try again shortly.';
    case 'network':
      return 'Could not reach the server. Check your connection and try again.';
    case 'unknown':
      return 'Something went wrong. Try again shortly.';
  }
}

// Any failure of a console call, as a ConsoleError; `call` says whether it
// was a read or a command (which words a timeout).
export function describeConsoleError(
  err: unknown,
  target: ConsoleTarget,
  call: 'read' | 'command' = 'command',
): ConsoleError {
  if (!isAxiosError(err)) {
    return { code: 'unknown', status: null, message: consoleMessageFor('unknown', target) };
  }
  const response = err.response;
  if (!response) {
    return { code: 'network', status: null, message: consoleMessageFor('network', target) };
  }
  const data = response.data as { error?: unknown; sent?: unknown } | undefined;
  const serverCode = typeof data?.error === 'string' ? data.error : '';
  // A reply with no code of ours (a proxy's own page) is read by its status.
  const code: ConsoleErrorCode = KNOWN_CONSOLE_CODES.has(serverCode)
    ? (serverCode as ConsoleErrorCode)
    : response.status === 429
      ? 'rate_limited'
      : response.status === 401
        ? 'unauthorized'
        : response.status === 504
          ? 'admin_timeout'
          : 'unknown';
  const retryAfterSeconds =
    response.status === 429 || response.status === 503
      ? parseRetryAfter(response.headers?.['retry-after'])
      : undefined;
  const sent = code === 'admin_timeout' && typeof data?.sent === 'boolean' ? data.sent : undefined;
  return {
    code,
    status: response.status,
    message:
      code === 'unknown'
        ? `${consoleMessageFor('unknown', target)} (HTTP ${response.status})`
        : consoleMessageFor(code, target, { retryAfterSeconds, sent, read: call === 'read' }),
    ...(retryAfterSeconds !== undefined ? { retryAfterSeconds } : {}),
    ...(sent !== undefined ? { sent } : {}),
  };
}

function consoleClient(target: ConsoleTarget): AxiosInstance {
  return target.kind === 'admin' ? adminClient : apiClient;
}

// A route under the target's server: `players`, `rotation/3/next`, ...
// (relative to the client's base: /api for the owner's, /api/admin for the
// admin's).
export function consolePath(target: ConsoleTarget, rest: string): string {
  return target.kind === 'admin' ? `/hosted/${target.id}/${rest}` : `/me/servers/${target.id}/${rest}`;
}

function malformedConsole(status: number, target: ConsoleTarget): ConsoleResult<never> {
  return { ok: false, error: { code: 'unknown', status, message: consoleMessageFor('unknown', target) } };
}

const isNumber = (v: unknown): v is number => typeof v === 'number';
const isString = (v: unknown): v is string => typeof v === 'string';
const isBoolean = (v: unknown): v is boolean => typeof v === 'boolean';

function isConsolePlayer(value: unknown): value is ConsolePlayer {
  const v = value as Record<string, unknown> | null;
  return (
    typeof v === 'object' &&
    v !== null &&
    isNumber(v.slot) &&
    isString(v.name) &&
    isNumber(v.team) &&
    isNumber(v.class) &&
    isNumber(v.kills) &&
    isNumber(v.deaths) &&
    isNumber(v.ping)
  );
}

function isConsoleMission(value: unknown): value is ConsoleMission {
  const v = value as Record<string, unknown> | null;
  return typeof v === 'object' && v !== null && isNumber(v.index) && isString(v.file) && isString(v.title);
}

function isRotationEntry(value: unknown): value is RotationEntry {
  const v = value as Record<string, unknown> | null;
  return (
    typeof v === 'object' &&
    v !== null &&
    isNumber(v.index) &&
    isString(v.file) &&
    isBoolean(v.swap_sides) &&
    isBoolean(v.flipped) &&
    isBoolean(v.one_shot) &&
    isBoolean(v.current) &&
    isBoolean(v.next)
  );
}

const LINK_STATES: ReadonlySet<unknown> = new Set<ConsoleLink['state']>(['connecting', 'up', 'down', 'stopped']);
const LINE_KINDS: ReadonlySet<unknown> = new Set<ConsoleLine['kind']>(['command', 'reply', 'status']);

function isConsoleLink(value: unknown): value is ConsoleLink {
  const v = value as Record<string, unknown> | null;
  return (
    typeof v === 'object' &&
    v !== null &&
    LINK_STATES.has(v.state) &&
    isString(v.game_state) &&
    isString(v.last_poll_at) &&
    isString(v.up_since) &&
    isString(v.error)
  );
}

function isConsoleLine(value: unknown): value is ConsoleLine {
  const v = value as Record<string, unknown> | null;
  return (
    typeof v === 'object' &&
    v !== null &&
    isNumber(v.id) &&
    LINE_KINDS.has(v.kind) &&
    isString(v.actor) &&
    isString(v.text) &&
    isString(v.at)
  );
}

// One GET whose reply carries the list `field` of items `isItem` accepts.
async function getList<T>(
  target: ConsoleTarget,
  rest: string,
  field: string,
  isItem: (value: unknown) => value is T,
  signal?: AbortSignal,
): Promise<ConsoleResult<T[]>> {
  try {
    const res = await consoleClient(target).get<unknown>(consolePath(target, rest), { signal });
    const list = (res.data as Record<string, unknown> | null)?.[field];
    if (!Array.isArray(list) || !list.every(isItem)) return malformedConsole(res.status, target);
    return { ok: true, value: list };
  } catch (err) {
    return { ok: false, error: describeConsoleError(err, target, 'read') };
  }
}

// One POST; the reply's `result`.
async function postCommand(target: ConsoleTarget, rest: string, body?: object): Promise<ConsoleResult<string>> {
  try {
    const res = await consoleClient(target).post<unknown>(consolePath(target, rest), body ?? {});
    const result = (res.data as { result?: unknown } | null)?.result;
    if (!isString(result)) return malformedConsole(res.status, target);
    return { ok: true, value: result };
  } catch (err) {
    return { ok: false, error: describeConsoleError(err, target) };
  }
}

// GET .../players (PLAYER LIST): the players; 409 not_in_game between matches.
export function fetchConsolePlayers(target: ConsoleTarget, signal?: AbortSignal): Promise<ConsoleResult<ConsolePlayer[]>> {
  return getList(target, 'players', 'players', isConsolePlayer, signal);
}

// GET .../missions (MISSION AVAILABLE): the missions the server has.
export function fetchConsoleMissions(target: ConsoleTarget, signal?: AbortSignal): Promise<ConsoleResult<ConsoleMission[]>> {
  return getList(target, 'missions', 'missions', isConsoleMission, signal);
}

// GET .../rotation (MISSION LIST): the rotation in play order; 409
// not_in_game between matches.
export function fetchConsoleRotation(target: ConsoleTarget, signal?: AbortSignal): Promise<ConsoleResult<RotationEntry[]>> {
  return getList(target, 'rotation', 'rotation', isRotationEntry, signal);
}

// GET .../chat (CHAT GET): the server's chat window, oldest first.
export function fetchConsoleChat(target: ConsoleTarget, signal?: AbortSignal): Promise<ConsoleResult<string[]>> {
  return getList(target, 'chat', 'lines', isString, signal);
}

// GET .../console: the console ring's lines with an id past `after`, the
// newest `limit` of them (at most CONSOLE_KEEP), oldest first, and the link's
// status. It reads the service's database alone: no command reaches the
// server (an ended server's log reads too), and the service admits it outside
// the one-request-per-account rule, so it needs no turn in the page's console
// queue.
export async function fetchConsoleLog(
  target: ConsoleTarget,
  query: { after?: number; limit?: number },
  signal?: AbortSignal,
): Promise<ConsoleResult<ConsoleLog>> {
  try {
    const res = await consoleClient(target).get<unknown>(consolePath(target, 'console'), {
      params: { after: query.after ?? 0, limit: query.limit ?? CONSOLE_KEEP },
      signal,
    });
    const data = res.data as Record<string, unknown> | null;
    if (typeof data !== 'object' || data === null) return malformedConsole(res.status, target);
    const { server_id: serverId, link, lines } = data;
    if (
      !isNumber(serverId) ||
      !(link === null || isConsoleLink(link)) ||
      !Array.isArray(lines) ||
      !lines.every(isConsoleLine)
    ) {
      return malformedConsole(res.status, target);
    }
    return { ok: true, value: { server_id: serverId, link, lines } };
  } catch (err) {
    return { ok: false, error: describeConsoleError(err, target, 'read') };
  }
}

// POST .../players/<slot>/punt, /ban, /swap or /kill.
export async function consolePlayerAction(
  target: ConsoleTarget,
  slot: number,
  action: PlayerAction,
): Promise<ConsoleResult<PlayerActionReply>> {
  try {
    const res = await consoleClient(target).post<unknown>(consolePath(target, `players/${slot}/${action}`), {});
    const data = res.data as Record<string, unknown> | null;
    if (typeof data !== 'object' || data === null) return malformedConsole(res.status, target);
    const { slot: replySlot, result, listed } = data;
    if (!isNumber(replySlot) || !isString(result)) return malformedConsole(res.status, target);
    return {
      ok: true,
      value: { slot: replySlot, result, ...(isBoolean(listed) ? { listed } : {}) },
    };
  } catch (err) {
    return { ok: false, error: describeConsoleError(err, target) };
  }
}

// POST .../rotation: `mission` (a MISSION AVAILABLE file) appended to the
// rotation, its sides swapped at the half or not; 409 rotation_full at
// MISSIONS_MAX maps.
export function addRotationMission(
  target: ConsoleTarget,
  mission: string,
  swapSides: boolean,
): Promise<ConsoleResult<string>> {
  return postCommand(target, 'rotation', { mission, swap_sides: swapSides });
}

// POST .../rotation/<index>/remove or /next: an entry removed (never the
// last), or made the next map.
export function rotationEntryAction(
  target: ConsoleTarget,
  index: number,
  action: RotationEntryAction,
): Promise<ConsoleResult<string>> {
  return postCommand(target, `rotation/${index}/${action}`);
}

// POST .../rotation/cycle (MISSION CYCLE): the round ends now and the next
// map loads.
export function cycleRotation(target: ConsoleTarget): Promise<ConsoleResult<string>> {
  return postCommand(target, 'rotation/cycle');
}

// POST .../chat (CHAT SEND): a server chat line, up to CHAT_TEXT_MAX
// characters of cp1252.
export function sendConsoleChat(target: ConsoleTarget, text: string): Promise<ConsoleResult<string>> {
  return postCommand(target, 'chat', { text });
}
