// Server status / maintenance. Matches GET/PUT /api/admin/server-status, which
// emits snake_case (apps/novaworld_server/http_listener.cpp get/update_server_status).
export interface ServerStatus {
  maintenance_enabled: boolean;
  message: string;
}

// A live connection from GET /api/admin/connections (snake_case, straight from
// the in-memory connection registry snapshot).
export interface AdminConnection {
  id: number;
  state: string;
  pn: number;
  identity: string;
  created_ms: number;
  last_seen_ms: number;
  addr: string;
}

export interface ConnectionsResponse {
  count: number;
  heartbeat_timeout_ms: number;
  connections: AdminConnection[];
}
