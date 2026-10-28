// Who is on NovaWorld now, as GET /api/admin/online answers it
// (apps/novaworld_web/connection_routes.cpp): one row per person, a signed-in
// client of ours (in the lobby, or playing on a listed server) or a player a
// listed server reports who came through NovaLogic's NovaWorld instead.

export interface OnlineAccount {
  id: number;
  username: string;
  // The in-game name, '' when the account has none.
  nwhandle: string;
}

export interface OnlineServer {
  // The listing's id.
  rid: number;
  name: string;
  mission: string;
  game_type: string;
  // The hosted server's id, null for one a player hosts.
  hosted_server_id: number | null;
}

export interface OnlinePlayer {
  // As the game shows them: the account's in-game name, else the server's.
  name: string;
  account: OnlineAccount | null;
  game: { slug: string; display_name: string } | null;
  state: 'lobby' | 'playing';
  server: OnlineServer | null;
  // Since when they play on `server`, else since they signed in (ms).
  since_ms: number;
  via: 'opennova' | 'novalogic';
  // Their lobby session; null for a NovaLogic player.
  session_id: number | null;
  // The client's executable ("Jointops.exe", "opennova.exe"); null when unknown.
  client: string | null;
}

export interface OnlineResponse {
  count: number;
  players: OnlinePlayer[];
}
