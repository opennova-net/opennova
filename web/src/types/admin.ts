// The NovaWorld service's own admin routes (snake_case;
// apps/novaworld_server/http_listener.cpp).

// GET/PUT /api/admin/server-status: maintenance mode and the message game
// clients see when they sign in.
export interface ServerStatus {
  maintenance_enabled: boolean;
  message: string;
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
  identity: string;
  // Epoch milliseconds.
  created_ms: number;
  last_seen_ms: number;
  addr: string;
}

export interface ConnectionsResponse {
  count: number;
  // How long a session lasts without a keepalive.
  heartbeat_timeout_ms: number;
  connections: AdminConnection[];
}
