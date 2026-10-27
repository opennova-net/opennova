// What a game, a server and a game type are called, the same on every page:
// one place for the names the site shows for them and their fallbacks.

import { stripGameText } from './gameText';

// The titles the service speaks on the wire, by their short code.
const TITLE_NAMES: Record<string, string> = { jo: 'Joint Operations', dfx2: 'Delta Force Xtreme 2' };

// A title's name ("jo" -> "Joint Operations"); an unknown code upper-cased.
export function titleName(code: string): string {
  return TITLE_NAMES[code] ?? code.toUpperCase();
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
