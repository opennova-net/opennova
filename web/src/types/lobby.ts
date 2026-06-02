export interface LobbyHost {
  id: number;
  serverName: string;
  region?: string | null;
  players: number;
  maxPlayers: number;
  message?: string | null;
  game?: string | null;
}

export interface LobbyGroup {
  slug: string;
  displayName: string;
  hosts: LobbyHost[];
}

export interface LobbyResponse {
  games: LobbyGroup[];
}
