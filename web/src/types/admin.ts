// The NovaWorld service's own admin routes (snake_case;
// apps/novaworld_server/http_listener.cpp).

// GET/PUT /api/admin/server-status: maintenance mode and the message game
// clients see when they sign in.
export interface ServerStatus {
  maintenance_enabled: boolean;
  message: string;
}

// The account a NovaWorld session is: the owner its listing's host key names
// ('host_key'; a hosted server's owner for its listing), or the account whose
// live game login its requests carried ('login').
export interface AdminConnectionAccount {
  id: number;
  username: string;
  via: 'host_key' | 'login';
}

// The server a hosting session lists (its lobby state).
export interface AdminConnectionHost {
  rid: number;
  server_name: string;
  mission_name: string;
  players: number;
  max_players: number;
  // False while it waits out of the browser for a host key that names an account.
  listed: boolean;
  // The hosted server the listing is, else null.
  hosted_server_id: number | null;
}

// One NovaWorld session from GET /api/admin/connections (the in-memory
// connection registry's snapshot).
export interface AdminConnection {
  id: number;
  // 'handshaking', 'active' or 'closing'.
  state: string;
  // The client hello's protocol name: NOVAWORLDUDP for a game client's
  // NovaWorld session, a title's own for a game host's ('JOINTOPERATIONS').
  pn: string;
  // The game tag its sign-in named ('jop:cus2').
  identity: string;
  // Epoch milliseconds.
  created_ms: number;
  last_seen_ms: number;
  addr: string;
  // Null while no host key or game login named one.
  account: AdminConnectionAccount | null;
  // Null unless it hosts.
  host: AdminConnectionHost | null;
}

export interface ConnectionsResponse {
  count: number;
  // How long a session lasts without a keepalive.
  heartbeat_timeout_ms: number;
  connections: AdminConnection[];
}
