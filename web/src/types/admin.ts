// The NovaWorld service's own admin routes (snake_case;
// apps/novaworld_web/http_listener.cpp).

// GET/PUT /api/admin/server-status: maintenance mode and the message game
// clients see when they sign in.
export interface ServerStatus {
  maintenance_enabled: boolean;
  message: string;
}

// The account a NovaWorld session is: the owner its listing's host key names
// ('host_key'; a hosted server's owner for its listing), or the account it
// signed in as, which the game login its requests carried named ('login'; it
// stays when that login ends).
export interface AdminConnectionAccount {
  id: number;
  username: string;
  via: 'host_key' | 'login';
}

// The game a session speaks: its games row (a server's by its listing's lobby
// name, else by the game tag it signed in with).
export interface AdminConnectionTitle {
  slug: string;
  display_name: string;
}

// What a listing is: one of the fleet's hosted servers ('hosted'), a server
// whose host key names an account ('owned'), or one it names none for
// ('anonymous').
export type ServerKind = 'hosted' | 'owned' | 'anonymous';

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
  server_kind: ServerKind;
  // Its listing's Dedicated column: true for a Serve Only host, false for a
  // listen host (it plays on its own server too); null while it is not listed.
  dedicated: boolean | null;
}

// The server a session plays on: its latest play request's address, and the
// listing there.
export interface AdminConnectionPlaying {
  // The listing at that address when it asked; null for none (a LAN server,
  // or one no lobby lists).
  rid: number | null;
  addr: string;
  // Epoch milliseconds.
  since_ms: number;
  // Whether that listing is still in the browser.
  listed: boolean;
  // The listing's name while it is listed, else null.
  server_name: string | null;
  // The hosted server the listing is, else null.
  hosted_server_id: number | null;
}

// One NovaWorld session from GET /api/admin/connections (the lobby instances'
// session rows).
export interface AdminConnection {
  // The session's row: unique across the lobby instances, what ending it names.
  session_id: number;
  // The lobby instance holding it, and the task that instance runs as ('' for
  // none: the key of its program's usage).
  lobby_id: number;
  lobby_task_id: string;
  // The lobby's registry id (unique within its instance alone).
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
  // 'server' for a session that hosts, 'client' for any other.
  kind: 'client' | 'server';
  // Null for a game tag no game of the service's answers to.
  title: AdminConnectionTitle | null;
  // Null while no host key or game login named one.
  account: AdminConnectionAccount | null;
  // Null unless it hosts.
  host: AdminConnectionHost | null;
  // Null unless it plays.
  playing: AdminConnectionPlaying | null;
}

export interface ConnectionsResponse {
  count: number;
  // How long a session lasts without a keepalive.
  heartbeat_timeout_ms: number;
  connections: AdminConnection[];
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
