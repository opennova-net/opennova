// What a game, a server and a game type are called, the same on every page:
// one place for the names the site shows for them and their fallbacks.

import { stripGameText } from './gameText';
import type { LobbyOption, Protocol } from '../types/products';

// The titles' names (the retail game a game runs as), by their short code.
const TITLE_NAMES: Record<string, string> = {
  jo: 'Joint Operations',
  dfx2: 'Delta Force Xtreme 2',
  acidgambit: 'OP: Acid Gambit',
};

// A title's name ("jo" -> "Joint Operations"); an unknown code upper-cased.
export function titleName(code: string): string {
  return TITLE_NAMES[code] ?? code.toUpperCase();
}

// A protocol in words: "JO protocol 16"; '' for none.
export function protocolWords(protocol: Protocol | null | undefined): string {
  if (!protocol?.name) return '';
  return protocol.version ? `${protocol.name} protocol ${protocol.version}` : `${protocol.name} protocol`;
}

// The code of the protocol title `title` speaks, from a reply's `protocols` (by title code); the
// title's own code where the reply names none (JO's and DFX2's protocols share their titles' codes).
export function titleProtocolCode(title: string, protocols: Record<string, Protocol> | undefined): string {
  return protocols?.[title]?.code ?? title;
}

// The titles that speak the protocol `title` speaks, from a reply's `protocols` (by title code; OP:
// Acid Gambit speaks JO's), by name, joined "and": "Joint Operations and OP: Acid Gambit"; the
// title's own name where the reply names no protocol for it.
export function playsWith(title: string, protocols: Record<string, Protocol> | undefined): string {
  const code = protocols?.[title]?.code;
  const names = code && protocols ? Object.keys(protocols).filter((t) => protocols[t].code === code).map(titleName) : [];
  if (names.length === 0) return titleName(title);
  return names.length === 1 ? names[0] : `${names.slice(0, -1).join(', ')} and ${names[names.length - 1]}`;
}

// The lobbies a game of title `title` may list in: its protocol's.
export function lobbiesForTitle(
  lobbies: readonly LobbyOption[],
  title: string,
  protocols: Record<string, Protocol> | undefined,
): LobbyOption[] {
  const code = titleProtocolCode(title, protocols);
  return lobbies.filter((lobby) => lobby.protocol?.code === code);
}

// A release by its game version and the ONE that built it: "0.2.0 (ONE 0.0.22)"; the version
// alone where the service names no ONE.
export function releaseVersionWords(version: string, oneVersion: string | null | undefined): string {
  return oneVersion ? `${version} (ONE ${oneVersion})` : version;
}

// The game a hosted server runs, in words: its product's name, else the name
// the server row kept, else its title's.
export function gameName(server: {
  product?: { display_name: string; title?: string } | null;
  game_display_name?: string;
}): string {
  const product = server.product;
  if (product?.display_name) return product.display_name;
  if (server.game_display_name) return server.game_display_name;
  if (product?.title) return titleName(product.title);
  return 'Unknown game';
}

// A server's name as the game shows it, or "Unnamed server" when it shows
// nothing (an empty name, or one of markup alone).
export function serverName(name: string | null | undefined): string {
  return name && stripGameText(name).trim() ? name : 'Unnamed server';
}

// The game type a host reports (it may carry the game's markup), '' for none.
export function gameTypeName(gameType: string | null | undefined): string {
  return gameType && stripGameText(gameType).trim() ? gameType : '';
}
