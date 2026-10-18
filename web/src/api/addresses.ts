// The logged-in account's static addresses (apps/novaworld_web/hosted/
// address_routes.cpp; hosted README "Static addresses"): GET /api/me/addresses
// (its addresses, and how many bought ones wait for a name) and POST
// /api/me/addresses (the next bought one named; a worker then allocates it).
// An address is bought on the support tier page (api/billing.ts, the
// static_address checkout) and canceled in Stripe's billing portal (openPortal
// with its subscription). The admin's routes are in adminAddresses.ts.
//
// Each call answers an AddressResult instead of throwing, with the server's
// `error` code mapped to a message: the server's own `message` text is never
// shown.

import { isAxiosError } from 'axios';
import { apiClient } from './client';
import type { Address, AddressServer, AddressesResponse, AddressSource, ServerAddress } from '../types/addresses';
import { ADDRESS_NAME_RULE } from '../utils/addressView';

// The server's `error` codes these routes answer with, plus `network` (no
// reply at all) and `unknown` (a reply this client does not recognise).
export type AddressErrorCode =
  | 'unauthorized'
  | 'csrf_header_required'
  | 'proxy_required'
  | 'addresses_disabled'
  | 'invalid_json'
  | 'invalid_field'
  | 'invalid_name'
  | 'name_taken'
  | 'no_address'
  | 'not_found'
  | 'db_error'
  | 'network'
  | 'unknown';

export interface AddressError {
  code: AddressErrorCode;
  // The HTTP status, or null when no reply arrived.
  status: number | null;
  message: string;
}

export type AddressResult<T> = { ok: true; value: T } | { ok: false; error: AddressError };

const KNOWN_CODES: ReadonlySet<string> = new Set<AddressErrorCode>([
  'unauthorized',
  'csrf_header_required',
  'proxy_required',
  'addresses_disabled',
  'invalid_json',
  'invalid_field',
  'invalid_name',
  'name_taken',
  'no_address',
  'not_found',
  'db_error',
]);

function messageFor(code: AddressErrorCode): string {
  switch (code) {
    case 'unauthorized':
      return 'Your session has ended. Log in again to manage your static addresses.';
    case 'csrf_header_required':
      return 'The server refused the request as not coming from this site. Reload the page and try again.';
    case 'proxy_required':
      return 'The server refused a request that did not come through this site. Reload the page and try again.';
    case 'addresses_disabled':
      return 'NovaWorld does not offer static addresses right now.';
    case 'invalid_json':
    case 'invalid_field':
      return 'The server could not read the request. Reload the page and try again.';
    case 'invalid_name':
      return `The server refused that name. ${ADDRESS_NAME_RULE}`;
    case 'name_taken':
      return 'Another static address already has that name. Pick another.';
    case 'no_address':
      return 'You have no bought static address waiting for a name. Add one on your Support tier page.';
    case 'not_found':
      return 'Your account is not active, so it cannot name a static address.';
    case 'db_error':
      return 'The server hit a database error. Try again shortly.';
    case 'network':
      return 'Could not reach the server. Check your connection and try again.';
    case 'unknown':
      return 'Something went wrong. Try again shortly.';
  }
}

// Any failure of these calls, as an AddressError.
export function describeAddressError(err: unknown): AddressError {
  if (isAxiosError(err)) {
    const response = err.response;
    if (!response) return { code: 'network', status: null, message: messageFor('network') };
    const data = response.data as { error?: unknown } | undefined;
    const serverCode = typeof data?.error === 'string' ? data.error : '';
    const code: AddressErrorCode = KNOWN_CODES.has(serverCode)
      ? (serverCode as AddressErrorCode)
      : response.status === 401
        ? 'unauthorized'
        : 'unknown';
    return {
      code,
      status: response.status,
      message: code === 'unknown' ? `${messageFor('unknown')} (HTTP ${response.status})` : messageFor(code),
    };
  }
  return { code: 'unknown', status: null, message: messageFor('unknown') };
}

const isCount = (v: unknown): v is number => Number.isInteger(v) && (v as number) >= 0;
const textOrNull = (v: unknown): string | null => (typeof v === 'string' && v !== '' ? v : null);

function parseAddressServer(value: unknown): AddressServer | null {
  const v = value as Record<string, unknown> | null;
  if (typeof v !== 'object' || v === null || !isCount(v.id)) return null;
  return {
    id: v.id,
    game_name: typeof v.game_name === 'string' ? v.game_name : '',
    state: typeof v.state === 'string' ? v.state : '',
  };
}

// An address as the routes answer it, or null when it lacks its id, name or
// state; the rest read with defaults.
export function parseAddress(value: unknown): Address | null {
  const v = value as Record<string, unknown> | null;
  if (typeof v !== 'object' || v === null) return null;
  if (!isCount(v.id) || typeof v.name !== 'string' || typeof v.state !== 'string') return null;
  return {
    id: v.id,
    name: v.name,
    hostname: typeof v.hostname === 'string' ? v.hostname : v.name,
    state: v.state,
    public_ip: textOrNull(v.public_ip),
    port: isCount(v.port) ? v.port : null,
    source: (v.source === 'comp' ? 'comp' : 'subscription') as AddressSource,
    stripe_subscription_id: textOrNull(v.stripe_subscription_id),
    server: parseAddressServer(v.server),
    reason: textOrNull(v.reason),
    created_at: textOrNull(v.created_at),
    ready_at: textOrNull(v.ready_at),
    lapsed_at: textOrNull(v.lapsed_at),
    released_at: textOrNull(v.released_at),
  };
}

// A hosted server's `address` ({id, name, hostname, public_ip, state}), or
// null for none.
export function parseServerAddress(value: unknown): ServerAddress | null {
  const v = value as Record<string, unknown> | null;
  if (typeof v !== 'object' || v === null) return null;
  if (!isCount(v.id) || typeof v.name !== 'string' || typeof v.state !== 'string') return null;
  return {
    id: v.id,
    name: v.name,
    hostname: textOrNull(v.hostname),
    public_ip: textOrNull(v.public_ip),
    state: v.state,
  };
}

function malformed(status: number): AddressResult<never> {
  return { ok: false, error: { code: 'unknown', status, message: messageFor('unknown') } };
}

// GET /api/me/addresses: whether the service sells addresses, their zone, how
// many bought ones wait for a name, and the account's addresses. A reply with
// a malformed address is refused whole.
export async function fetchMyAddresses(signal?: AbortSignal): Promise<AddressResult<AddressesResponse>> {
  try {
    const res = await apiClient.get<unknown>('/me/addresses', { signal });
    const v = res.data as Record<string, unknown> | null;
    if (typeof v !== 'object' || v === null) return malformed(res.status);
    if (typeof v.enabled !== 'boolean' || typeof v.zone !== 'string' || !isCount(v.unnamed) || !Array.isArray(v.addresses)) {
      return malformed(res.status);
    }
    const addresses = v.addresses.map(parseAddress);
    if (addresses.some((a) => a === null)) return malformed(res.status);
    return {
      ok: true,
      value: { enabled: v.enabled, zone: v.zone, unnamed: v.unnamed, addresses: addresses as Address[] },
    };
  } catch (err) {
    return { ok: false, error: describeAddressError(err) };
  }
}

// POST /api/me/addresses {"name"}: the account's oldest unnamed address named
// (201, allocating; a worker gives it its IP). 400 invalid_name, 409
// name_taken, 409 no_address with none waiting, 503 addresses_disabled.
export async function nameAddress(name: string): Promise<AddressResult<Address>> {
  try {
    const res = await apiClient.post<unknown>('/me/addresses', { name });
    const address = parseAddress((res.data as { address?: unknown } | null)?.address);
    if (!address) return malformed(res.status);
    return { ok: true, value: address };
  } catch (err) {
    return { ok: false, error: describeAddressError(err) };
  }
}
