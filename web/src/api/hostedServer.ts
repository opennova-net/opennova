// The logged-in account's hosted server (apps/novaworld_server/README.md,
// "Hosted servers"; hosted/hosted_routes.cpp): GET /api/me/server, POST
// /api/me/server/start and POST /api/me/server/stop. The session cookie
// authenticates them and `apiClient` sends the CSRF header both POSTs need.
// Below them, the console routes (hosted/console_routes.cpp), the owner's and
// the admin's.
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
  HostedServer,
  HostedServerReply,
  HostedSettings,
  MyHostedServerResponse,
  PlayerAction,
  PlayerActionReply,
  RotationEntry,
  RotationEntryAction,
  StartSettings,
} from '../types/hostedServer';
import { FIELD_RULES, MISSIONS_MAX, isSettingsField, type SettingsField } from '../utils/hostedSettings';

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
      return 'You already have a hosted server running.';
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

// ---------------------------------------------------------------------------
// The console routes (hosted/console_routes.cpp; README "Hosted servers", the
// console routes). The owner's act on the account's own running server, which
// its session resolves (`apiClient`, which sends the CSRF header every POST
// needs); the admin's name the server (`adminClient`: an admin session or the
// stored Bearer token). The service runs at most one console request per
// account at once (an admin session's account; the Bearer token counts as one
// account) and answers a second 429 console_busy at once, so a page sends
// every one of these calls the service admits through its one ConsoleQueue
// (utils/consoleQueue.ts); the console log's read, which it does not admit,
// keeps out of it.

// Where a console call goes: the owner's own server, or server `id` for an
// admin.
export type ConsoleTarget = { kind: 'owner' } | { kind: 'admin'; id: number };

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
  | 'no_live_server'
  | 'no_server'
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
  'no_live_server',
  'no_server',
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
    code === 'no_live_server' ||
    code === 'no_server' ||
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
        : 'Your session has ended. Log in again to manage your hosted server.';
    case 'forbidden':
      return 'This account is not an admin.';
    case 'csrf_header_required':
      return 'The server refused the request as not coming from this site. Reload the page and try again.';
    case 'proxy_required':
      return 'The server refused a request that did not come through this site. Reload the page and try again.';
    case 'no_live_server':
      return 'You have no hosted server running.';
    case 'no_server':
      return 'You have no hosted server.';
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
function consolePath(target: ConsoleTarget, rest: string): string {
  return target.kind === 'admin' ? `/hosted/${target.id}/${rest}` : `/me/server/${rest}`;
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
// server, and the service admits it outside the one-request-per-account rule,
// so it needs no turn in the page's console queue.
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
