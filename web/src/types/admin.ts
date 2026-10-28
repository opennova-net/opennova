// The NovaWorld service's own admin routes (snake_case;
// apps/novaworld_web/http_listener.cpp).

// GET/PUT /api/admin/server-status: maintenance mode and the message game
// clients see when they sign in.
export interface ServerStatus {
  maintenance_enabled: boolean;
  message: string;
}

// One lobby instance from GET /api/admin/lobbies (lobby_instances).
export interface AdminLobby {
  id: number;
  // The lobby address every instance announces.
  endpoint: string;
  // 'active' (the gates count it as taking clients) or 'draining'.
  state: string;
  // The task its lobby runs as ('' for none): its program usage's key.
  task_id: string;
  // Epoch milliseconds.
  started_ms: number;
  last_seen_ms: number;
  sessions: number;
}

export interface LobbiesResponse {
  lobbies: AdminLobby[];
}

// One server NovaWorld lists, as GET /api/admin/hosts answers it
// (host_routes.cpp listed_host_json): its browser columns, where joiners are
// sent, the account whose host key listed it (a hosted server's owner for its
// listing; null for an anonymous one) and the live hosted server it is, if any.
export interface ListedHost {
  rid: number;
  gsid: string;
  // Its lobby's games slug ('jop_2_consumer').
  game: string;
  server_name: string;
  mission_name: string;
  game_type: string;
  players: number;
  max_players: number;
  host_ip: string;
  host_port: number;
  peer_ip: string;
  // UTC 'YYYY-MM-DD HH:MM:SS'.
  created_at: string;
  updated_at: string;
  hosted_server_id: number | null;
  owner: { id: number; username: string } | null;
}

export interface ListedHostsResponse {
  count: number;
  hosts: ListedHost[];
}
