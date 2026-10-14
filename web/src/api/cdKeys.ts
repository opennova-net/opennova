// The CD keys' admin routes (/api/admin/cd-keys*, /api/admin/users/<id>/cd-keys).
// Like the hosted servers' admin calls, each answers an AdminResult with the
// server's `error` code mapped to a message (admin.ts), never the server's text.

import { adminClient } from './client';
import { describeAdminError, malformedAdminReply, type AdminResult } from './admin';
import type {
  CdKey,
  CdKeyAccountResponse,
  CdKeyActivation,
  CdKeyActivationResult,
  CdKeyFilter,
  CdKeyListResponse,
} from '../types/cdKeys';

function isKey(value: unknown): value is CdKey {
  const k = value as Partial<CdKey> | null;
  return (
    typeof k === 'object' &&
    k !== null &&
    typeof k.id === 'number' &&
    typeof k.serial === 'string' &&
    typeof k.state === 'string'
  );
}

function isKeyList(value: unknown): value is CdKey[] {
  return Array.isArray(value) && value.every(isKey);
}

function isListResponse(value: unknown): value is CdKeyListResponse {
  const v = value as Partial<CdKeyListResponse> | null;
  return (
    typeof v === 'object' &&
    v !== null &&
    typeof v.total === 'number' &&
    typeof v.auto_mint === 'boolean' &&
    Array.isArray(v.titles) &&
    isKeyList(v.keys)
  );
}

async function call<T>(request: () => Promise<{ status: number; data: unknown }>, pick: (data: unknown) => T | null): Promise<AdminResult<T>> {
  try {
    const res = await request();
    const value = pick(res.data);
    if (value === null) return malformedAdminReply(res.status);
    return { ok: true, value };
  } catch (err) {
    return { ok: false, error: describeAdminError(err) };
  }
}

// GET /api/admin/cd-keys: the keys the filter selects, newest first.
export function fetchCdKeys(filter: CdKeyFilter, signal?: AbortSignal): Promise<AdminResult<CdKeyListResponse>> {
  const params: Record<string, string | number> = {};
  if (filter.title) params.title = filter.title;
  if (filter.state) params.state = filter.state;
  if (filter.q) params.q = filter.q;
  if (filter.limit !== undefined) params.limit = filter.limit;
  if (filter.offset !== undefined) params.offset = filter.offset;
  return call(
    () => adminClient.get<unknown>('/cd-keys', { params, signal }),
    (data) => (isListResponse(data) ? data : null),
  );
}

// POST /api/admin/cd-keys: `count` new unassigned keys for the game.
export function generateCdKeys(title: string, count: number): Promise<AdminResult<CdKey[]>> {
  return call(
    () => adminClient.post<unknown>('/cd-keys', { title, count }),
    (data) => {
      const keys = (data as { keys?: unknown } | null)?.keys;
      return isKeyList(keys) ? keys : null;
    },
  );
}

function pickKey(data: unknown): CdKey | null {
  const key = (data as { key?: unknown } | null)?.key;
  return isKey(key) ? key : null;
}

// POST /api/admin/cd-keys/<id>/assign: the key onto the player account of that name.
export function assignCdKey(id: number, username: string): Promise<AdminResult<CdKey>> {
  return call(() => adminClient.post<unknown>(`/cd-keys/${id}/assign`, { username }), pickKey);
}

// POST /api/admin/cd-keys/<id>/revoke: the key activates no machine again.
export function revokeCdKey(id: number): Promise<AdminResult<CdKey>> {
  return call(() => adminClient.post<unknown>(`/cd-keys/${id}/revoke`), pickKey);
}

// GET /api/admin/cd-keys/<id>/activations: the machines a key activated.
export function fetchKeyActivations(id: number, signal?: AbortSignal): Promise<AdminResult<CdKeyActivation[]>> {
  return call(
    () => adminClient.get<unknown>(`/cd-keys/${id}/activations`, { signal }),
    (data) => {
      const activations = (data as { activations?: unknown } | null)?.activations;
      return Array.isArray(activations) ? (activations as CdKeyActivation[]) : null;
    },
  );
}

// POST /api/admin/cd-keys/<id>/activations: a manual activation, the NWACT for the
// machine the NWPSSK names.
export function activateCdKey(id: number, nwpssk: string): Promise<AdminResult<CdKeyActivationResult>> {
  return call(
    () => adminClient.post<unknown>(`/cd-keys/${id}/activations`, { nwpssk }),
    (data) => {
      const v = data as Partial<CdKeyActivationResult> | null;
      return typeof v?.nwact === 'string' && typeof v.reactivated === 'boolean' && isKey(v.key)
        ? (v as CdKeyActivationResult)
        : null;
    },
  );
}

// GET /api/admin/users/<id>/cd-keys: an account's keys and every machine they activated.
export function fetchAccountCdKeys(userId: number, signal?: AbortSignal): Promise<AdminResult<CdKeyAccountResponse>> {
  return call(
    () => adminClient.get<unknown>(`/users/${userId}/cd-keys`, { signal }),
    (data) => {
      const v = data as Partial<CdKeyAccountResponse> | null;
      return typeof v?.account?.id === 'number' && isKeyList(v.keys) && Array.isArray(v.activations)
        ? (v as CdKeyAccountResponse)
        : null;
    },
  );
}
