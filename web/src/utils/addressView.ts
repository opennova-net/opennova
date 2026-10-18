// What the pages show of static addresses (types/addresses.ts): a name's rule
// and its check before the request, each state in plain words, the address
// players join, and which addresses a new own-machine server may take.

import type { Address, AddressState, ServerAddress } from '../types/addresses';
import type { HostedServer } from '../types/hostedServer';

// The game port an own machine's servers list at (its one slot, slot 0;
// hosted/protocol/slots.h slot_game_port): a server JSON's address names no port.
export const ADDRESS_GAME_PORT = 32768;

// hosted/model/address_names.h: 3 to 32 of lower-case letters, digits and '-', a
// letter or digit first and last. The reserved names are the service's to
// judge (invalid_name).
const NAME_PATTERN = /^[a-z0-9][a-z0-9-]{1,30}[a-z0-9]$/;

export const ADDRESS_NAME_RULE =
  "A name is 3 to 32 lower-case letters, digits and '-', starts and ends with a letter or digit, has no '--', is not all digits, and is not one the service keeps for itself.";

// '' when `name` may name an address as far as this page can tell, else the
// rule.
export function addressNameProblem(name: string): string {
  if (!NAME_PATTERN.test(name) || name.includes('--') || /^\d+$/.test(name)) return ADDRESS_NAME_RULE;
  return '';
}

const STATE_LABELS: Record<AddressState, string> = {
  allocating: 'Being set up',
  ready: 'Ready',
  lapsed: 'Ending: its subscription ended',
  releasing: 'Being released',
  released: 'Released',
  failed: 'Could not be set up',
};

// An address state in words; one this site does not know shows as it is.
export function addressStateLabel(state: string): string {
  return (STATE_LABELS as Record<string, string>)[state] ?? state;
}

// The badge colour of a state: ready is good, failed is bad, the rest move.
export function addressStateClass(state: string): string {
  if (state === 'ready') return 'text-online';
  if (state === 'failed') return 'text-danger';
  if (state === 'released') return 'text-ink-muted';
  return 'text-warn';
}

// The states a worker moves on its own: a page watching one reads again soon.
export function isAddressMoving(state: string): boolean {
  return state === 'allocating' || state === 'lapsed' || state === 'releasing';
}

// "203.0.113.9:32768", the address players join at; '' before it has an IP.
export function addressEndpoint(address: Pick<Address, 'public_ip' | 'port'>): string {
  if (!address.public_ip) return '';
  return `${address.public_ip}:${address.port ?? ADDRESS_GAME_PORT}`;
}

// The same for a server's address, whose JSON names no port.
export function serverAddressEndpoint(address: Pick<ServerAddress, 'public_ip'>): string {
  return address.public_ip ? `${address.public_ip}:${ADDRESS_GAME_PORT}` : '';
}

// The addresses a new own-machine server may run at: the ready ones no live
// server uses.
export function freeAddresses(addresses: readonly Address[]): Address[] {
  return addresses.filter((a) => a.state === 'ready' && a.server === null);
}

// The address a new server's picker starts on: the one the server it starts
// again from used, else the one the account's newest server used, when that
// is free; else the first free one; null with none free.
export function preferredAddress(
  free: readonly Address[],
  servers: readonly HostedServer[],
  from: HostedServer | null = null,
): number | null {
  const newest = servers.filter((s) => s.address !== null).sort((a, b) => b.id - a.id)[0];
  const wanted = from?.address?.id ?? newest?.address?.id;
  if (wanted !== undefined && free.some((a) => a.id === wanted)) return wanted;
  return free[0]?.id ?? null;
}
