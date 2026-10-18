// Static addresses for the address tests, shaped as GET /api/me/addresses and
// GET /api/admin/addresses answer them: a ready one named `name` in the
// play.opennova.net zone, paid by its own subscription, unless the overrides
// say otherwise.

import type { Address, AddressesResponse, AdminAddress, AdminAddressesResponse } from '../types/addresses';

export const ZONE = 'play.opennova.net';

export function address(id: number, name = `addr${id}`, overrides: Partial<Address> = {}): Address {
  return {
    id,
    name,
    hostname: `${name}.${ZONE}`,
    state: 'ready',
    public_ip: `203.0.113.${id}`,
    port: 32768,
    source: 'subscription',
    stripe_subscription_id: `sub_address_${id}`,
    server: null,
    reason: null,
    created_at: '2026-10-14 12:00:00',
    ready_at: '2026-10-14 12:01:00',
    lapsed_at: null,
    released_at: null,
    ...overrides,
  };
}

export function addressesResponse(addresses: Address[], overrides: Partial<AddressesResponse> = {}): AddressesResponse {
  return { enabled: true, zone: ZONE, unnamed: 0, addresses, ...overrides };
}

export function adminAddress(id: number, name = `addr${id}`, overrides: Partial<AdminAddress> = {}): AdminAddress {
  return {
    ...address(id, name),
    owner: { id: 100 + id, username: `owner${id}` },
    allocation_id: `eipalloc-0abc${id}`,
    dns_name: `${name}.${ZONE}`,
    dns_state: 'insync',
    lapse_cause: null,
    ...overrides,
  };
}

export function adminAddressesResponse(
  addresses: AdminAddress[],
  overrides: Partial<AdminAddressesResponse> = {},
): AdminAddressesResponse {
  return {
    enabled: true,
    zone: ZONE,
    held: addresses.filter((a) => a.state !== 'released' && a.state !== 'failed').length,
    sellable: 10,
    addresses,
    orphans: [],
    ...overrides,
  };
}
