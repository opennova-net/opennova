// The admin's Servers table: every server on our NovaWorld as one row, a
// hosted one (the fleet's) or one a player hosts (a listing no hosted server
// holds), and the filters it takes from the URL.

import type { LocationQuery } from 'vue-router';
import { gameName, gameTypeName } from './games';
import type { ListedHost } from '../types/admin';
import type { FleetQuery, FleetServer } from '../types/hostedServer';
import type { ProductsResponse } from '../types/products';

export type ServerKindFilter = 'all' | 'hosted' | 'player';

// The table's filters, as the URL's query holds them: state (live by
// default), kind, product (a product id) and owner (a username).
export interface ServersFilter {
  state: NonNullable<FleetQuery['state']>;
  kind: ServerKindFilter;
  product: number | null;
  owner: string;
}

export function filterOf(query: LocationQuery): ServersFilter {
  const one = (v: unknown) => (typeof v === 'string' ? v : '');
  const state = one(query.state);
  const kind = one(query.kind);
  const product = one(query.product);
  return {
    state: state === 'ended' || state === 'all' ? state : 'live',
    kind: kind === 'hosted' || kind === 'player' ? kind : 'all',
    product: /^\d+$/.test(product) ? Number(product) : null,
    owner: one(query.owner).trim(),
  };
}

export interface ServerRow {
  key: string;
  // The hosted server, or the listing of one a player hosts.
  server: FleetServer | null;
  host: ListedHost | null;
  name: string;
  game: string;
  gameType: string;
  map: string;
  // "5/32"; '' when not listed.
  players: string;
  // Since when it has been in its state (UTC).
  since: string;
}

// Since when a hosted server has been in its state: running since it first
// ran, an ended one since it ended, any other since its last change.
export function sinceOf(s: FleetServer): string {
  if (s.state === 'running' && s.running_at) return s.running_at;
  if (!s.live && s.ended_at) return s.ended_at;
  return s.updated_at;
}

export function hostedRow(s: FleetServer): ServerRow {
  const listing = s.listing;
  return {
    key: `hosted-${s.id}`,
    server: s,
    host: null,
    name: listing?.server_name || s.settings.game_name,
    game: gameName(s),
    gameType: gameTypeName(listing?.game_type),
    map: listing?.mission_name ?? '',
    players: s.players !== null ? `${s.players}/${listing?.max_players ?? s.settings.max_players}` : '',
    since: sinceOf(s),
  };
}

// A lobby's game in words: the service's games row, else the product that
// lists there, else the slug itself.
export function lobbyGameName(slug: string, products: ProductsResponse | null): string {
  return (
    products?.games.find((g) => g.slug === slug)?.display_name ||
    products?.products.find((p) => p.lobby === slug)?.display_name ||
    slug
  );
}

export function listedRow(h: ListedHost, products: ProductsResponse | null): ServerRow {
  return {
    key: `listed-${h.rid}`,
    server: null,
    host: h,
    name: h.server_name,
    game: lobbyGameName(h.game, products),
    gameType: gameTypeName(h.game_type),
    map: h.mission_name,
    players: `${h.players}/${h.max_players}`,
    since: h.created_at,
  };
}

// Whether a hosted server runs the product (its game or its expansion).
export function runsProduct(s: FleetServer, product: number): boolean {
  return s.product?.id === product || s.expansion?.id === product;
}

// The rows the filter shows: the live hosted servers, then the ones players
// host (listings no hosted server holds, live by nature), then the ended
// hosted ones. The fleet read already took the state and owner; `hidden` are
// the hosted rows the history toggle keeps out.
export function serverRows(
  servers: readonly FleetServer[],
  hosts: readonly ListedHost[],
  filter: ServersFilter,
  products: ProductsResponse | null,
  hidden: ReadonlySet<number> = new Set(),
): ServerRow[] {
  const hosted = filter.kind === 'player' ? [] : servers.filter((s) => !hidden.has(s.id) && (filter.product === null || runsProduct(s, filter.product)));
  const lobby = filter.product === null ? null : products?.products.find((p) => p.id === filter.product)?.lobby ?? '';
  const owner = filter.owner.toLowerCase();
  const listed =
    filter.kind === 'hosted' || filter.state === 'ended'
      ? []
      : hosts.filter(
          (h) =>
            h.hosted_server_id === null &&
            (lobby === null || (lobby !== '' && h.game === lobby)) &&
            (!owner || h.owner?.username.toLowerCase() === owner),
        );
  return [
    ...hosted.filter((s) => s.live).map(hostedRow),
    ...listed.map((h) => listedRow(h, products)),
    ...hosted.filter((s) => !s.live).map(hostedRow),
  ];
}
