import type { RouteLocationRaw } from 'vue-router';

export interface LobbyHost {
  id: number;
  serverName: string;
  hostIp?: string | null;
  hostPort?: number | null;
  region?: string | null;
  players: number;
  maxPlayers: number;
  // The mission playing and its game type ("AS - Kombang Valley", "AAS").
  mission?: string | null;
  gameType?: string | null;
  // The expansion a joiner needs installed ("Escalation"); empty for the base game.
  expansion?: string | null;
  // A game password guards it.
  locked?: boolean;
}

// One player on a server NovaWorld hosts, as its admin link reports them.
export interface LobbyScore {
  name: string;
  team: number;
  kills: number;
  deaths: number;
}

// GET /api/lobbies/<id>: a listed server's page.
export interface LobbyServer extends LobbyHost {
  game: { slug: string; displayName: string };
  // Minutes left in the round; null when its host gave none.
  timeLeft: number | null;
  timeOfDay: string;
  message: string;
  // The names of the players who joined it through NovaWorld.
  roster: string[];
  // Scores, on a server NovaWorld hosts; null for a player's own host.
  scoreboard: LobbyScore[] | null;
}

export interface LobbyGroup {
  slug: string;
  displayName: string;
  hosts: LobbyHost[];
}

export interface LobbyResponse {
  games: LobbyGroup[];
}

// One Joint Operations server on NovaLogic's NovaWorld, as GET /api/lobbies/novalogic
// lists it (no address: it is joined from the game's own browser there). Its
// text keeps the game's inline markup (utils/gameText.ts).
export interface NovaLogicServer {
  id: number;
  serverName: string;
  mission: string;
  gameType: string;
  players: number;
  maxPlayers: number;
  // A game password guards it, or it is locked.
  locked: boolean;
  // A game password guards it.
  password: boolean;
  message: string;
  country: string;
  region: string;
  // Minutes left in the round as its host wrote them ("29"), or "NA".
  timeLeft: string;
  timeOfDay: string;
  // The expansion's /exp code ("jox01"); empty for the base game.
  expansion: string;
  punkBuster: boolean;
  dedicated: boolean;
  tracers: boolean;
  // The names of its players.
  roster: string[];
}

// GET /api/lobbies/novalogic: NovaLogic's list as the service last fetched it.
export interface NovaLogicLobbies {
  // The service lists NovaLogic's servers at all.
  enabled: boolean;
  servers: NovaLogicServer[];
  // When the list was fetched (ISO 8601 UTC) and how long ago; null before the first.
  fetchedAt: string | null;
  ageSeconds: number | null;
  // The last fetch failed: the list is the one before it, or none.
  failing: boolean;
}

// One row of the Lobbies page's server table (components/ServerTable.vue): a
// server our NovaWorld lists, or one on NovaLogic's. Its text keeps the game's
// inline markup.
export interface ServerRow {
  key: string;
  // Its page.
  to: RouteLocationRaw;
  serverName: string;
  mission: string;
  gameType: string;
  players: number;
  maxPlayers: number;
  country?: string;
  // The expansion a joiner needs: its name, or its /exp code on NovaLogic's.
  expansion?: string;
  // A game password guards it.
  password?: boolean;
  // It is locked (with or without a password).
  locked?: boolean;
  punkBuster?: boolean;
}
