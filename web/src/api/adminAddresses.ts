// The admin's static addresses (apps/novaworld_web/hosted/address_routes.cpp):
// GET /api/admin/addresses (every address, what the cloud holds against what
// the deployment sells, and the addresses the cloud holds that no row names),
// PUT /api/admin/addresses/<id> (a rename), POST
// /api/admin/addresses/<id>/release (with the name typed to confirm), POST
// /api/admin/addresses/<id>/retry (one the cloud refused, tried again) and POST
// /api/admin/users/<id>/addresses (an admin's comp address). Each answers an
// AdminResult (admin.ts).

import { adminClient } from './client';
import { adminCall, type AdminResult } from './admin';
import { parseAddress } from './addresses';
import type { AddressOrphan, AdminAddress, AdminAddressesResponse } from '../types/addresses';

const isCount = (v: unknown): v is number => Number.isInteger(v) && (v as number) >= 0;
const textOrNull = (v: unknown): string | null => (typeof v === 'string' && v !== '' ? v : null);

// An address with the admin's fields, or null when malformed.
export function parseAdminAddress(value: unknown): AdminAddress | null {
  const address = parseAddress(value);
  if (!address) return null;
  const v = value as Record<string, unknown>;
  const owner = v.owner as Record<string, unknown> | null | undefined;
  return {
    ...address,
    owner: owner && isCount(owner.id) && typeof owner.username === 'string' ? { id: owner.id, username: owner.username } : null,
    allocation_id: textOrNull(v.allocation_id),
    dns_name: textOrNull(v.dns_name),
    dns_state: typeof v.dns_state === 'string' ? v.dns_state : '',
    retry_at: textOrNull(v.retry_at),
  };
}

function parseOrphan(value: unknown): AddressOrphan | null {
  const v = value as Record<string, unknown> | null;
  if (typeof v !== 'object' || v === null || typeof v.allocation_id !== 'string') return null;
  return {
    allocation_id: v.allocation_id,
    public_ip: typeof v.public_ip === 'string' ? v.public_ip : '',
    // 0 is the sweep's "no tag".
    address_id: isCount(v.address_id) && v.address_id > 0 ? v.address_id : null,
    seen_at: typeof v.seen_at === 'string' ? v.seen_at : '',
  };
}

function parseAdminAddresses(data: unknown): AdminAddressesResponse | null {
  const v = data as Record<string, unknown> | null;
  if (typeof v !== 'object' || v === null) return null;
  if (typeof v.enabled !== 'boolean' || typeof v.zone !== 'string' || !isCount(v.held) || !isCount(v.sellable)) return null;
  if (!Array.isArray(v.addresses) || !Array.isArray(v.orphans)) return null;
  const addresses = v.addresses.map(parseAdminAddress);
  const orphans = v.orphans.map(parseOrphan);
  if (addresses.some((a) => a === null) || orphans.some((o) => o === null)) return null;
  return {
    enabled: v.enabled,
    zone: v.zone,
    held: v.held,
    sellable: v.sellable,
    addresses: addresses as AdminAddress[],
    orphans: orphans as AddressOrphan[],
  };
}

function pickAddressReply(data: unknown): AdminAddress | null {
  return parseAdminAddress((data as { address?: unknown } | null)?.address);
}

// GET /api/admin/addresses.
export function fetchAdminAddresses(signal?: AbortSignal): Promise<AdminResult<AdminAddressesResponse>> {
  return adminCall(() => adminClient.get<unknown>('/addresses', { signal }), parseAdminAddresses);
}

// PUT /api/admin/addresses/<id> {"name"}: the address renamed (a released
// one's old name is free again); 400 invalid_name, 409 name_taken.
export function renameAddress(id: number, name: string): Promise<AdminResult<AdminAddress>> {
  return adminCall(() => adminClient.put<unknown>(`/addresses/${id}`, { name }), pickAddressReply);
}

// POST /api/admin/addresses/<id>/release {"confirm": its name}: the address
// lapsed (its server placed again without it), then released (202). A
// subscription that pays for it is canceled in Stripe, not here. 400
// confirm_mismatch, 409 not_releasable when it is on its way out already.
export function releaseAddress(id: number, confirm: string): Promise<AdminResult<AdminAddress>> {
  return adminCall(() => adminClient.post<unknown>(`/addresses/${id}/release`, { confirm }), pickAddressReply);
}

// POST /api/admin/addresses/<id>/retry: an address the cloud refused tried
// again (allocating, 202); 409 not_retryable when it is not failed, 503
// addresses_disabled when the service sells none.
export function retryAddress(id: number): Promise<AdminResult<AdminAddress>> {
  return adminCall(() => adminClient.post<unknown>(`/addresses/${id}/retry`, {}), pickAddressReply);
}

// POST /api/admin/users/<id>/addresses {"name"}: an admin's comp address for
// the account (201), at most one for each of its dedicated machines; 409
// address_limit past them, 503 addresses_disabled when the service sells
// none.
export function addCompAddress(userId: number, name: string): Promise<AdminResult<AdminAddress>> {
  return adminCall(() => adminClient.post<unknown>(`/users/${userId}/addresses`, { name }), pickAddressReply);
}
