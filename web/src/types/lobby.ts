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

export interface LobbyGroup {
  slug: string;
  displayName: string;
  hosts: LobbyHost[];
}

export interface LobbyResponse {
  games: LobbyGroup[];
}
