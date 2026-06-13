export interface LobbyHost {
  id: number;
  serverName: string;
  hostIp?: string | null;
  hostPort?: number | null;
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
