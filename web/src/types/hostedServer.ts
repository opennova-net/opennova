// Hosted servers, as the NovaWorld service's routes answer them (snake_case;
// apps/novaworld_server/hosted/hosted_routes.cpp, README "Hosted servers").

// requested -> provisioning -> booting -> listing -> running -> stopping ->
// stopped | failed. A crash restart goes back to provisioning, and a stop takes
// any live state to stopping.
export type HostedState =
  | 'requested'
  | 'provisioning'
  | 'booting'
  | 'listing'
  | 'running'
  | 'stopping'
  | 'stopped'
  | 'failed';

// '' is base Joint Operations, 'jox01' Escalation (the server runs /exp jox01).
export type HostedExpansion = '' | 'jox01';

// The owner's settings, as stored (hosted_settings.h HostedSettings). The
// backend range-checks each one (check_settings); utils/hostedSettings.ts
// mirrors those rules.
export interface HostedSettings {
  game_name: string;
  max_players: number;
  // '' for none.
  game_password: string;
  // Minutes.
  time_limit: number;
  kill_limit: number;
  max_score: number;
  respawn: number;
  start_delay: number;
  team_ff: boolean;
  expansion: HostedExpansion;
  // The rotation in play order: the first is the map the server starts on.
  missions: string[];
}

// POST /api/me/server/start's body: every field but game_name may be left out
// (the backend's default), and missions defaults to the service's
// ONNET_HOSTED_MISSIONS.
export type StartSettings = Pick<HostedSettings, 'game_name'> & Partial<Omit<HostedSettings, 'game_name'>>;

// The listing the server's service account holds now.
export interface HostedListing {
  rid: number;
  server_name: string;
  mission_name: string;
  players: number;
  max_players: number;
  // Where joiners are sent.
  host_ip: string;
  host_port: number;
}

// One state transition. `from` is '' for the request itself; `detail` is the
// service's own note on what caused it ("picked up", "the launch failed").
export interface HostedEvent {
  from: HostedState | '';
  to: HostedState;
  detail: string;
  // UTC, 'YYYY-MM-DD HH:MM:SS'.
  at: string;
}

// A hosted server, as its owner sees it. Times are UTC 'YYYY-MM-DD HH:MM:SS'.
export interface HostedServer {
  id: number;
  state: HostedState;
  // Any state but stopped and failed.
  live: boolean;
  settings: HostedSettings;
  // Crash restarts counted toward the limit (3 in the restart window).
  restarts: number;
  // Why it failed, in the service's words; null unless it did.
  failure_reason: string | null;
  created_at: string;
  updated_at: string;
  running_at: string | null;
  ended_at: string | null;
  listing: HostedListing | null;
  // The newest 50 transitions, oldest first: GET /api/me/server only (the
  // start and stop replies leave them out).
  events?: HostedEvent[];
}

// GET /api/me/server.
export interface MyHostedServerResponse {
  // The service runs hosted servers (ONNET_HOSTED_PROVIDER is not none).
  enabled: boolean;
  // An admin approved this account for a hosted server.
  can_host: boolean;
  // The service's configured rotation (ONNET_HOSTED_MISSIONS), which a start
  // naming no missions takes; empty when none is configured, and a start must
  // then name its own.
  default_missions: string[];
  // The account's live server, else its newest ended one, else null.
  server: HostedServer | null;
}

// The start and stop replies.
export interface HostedServerReply {
  server: HostedServer | null;
}

// An account named in the fleet view.
export interface AccountRef {
  id: number;
  username: string;
}

// A hosted server in the admin's fleet view: the owner's view plus who owns
// it, its service account (srv-<id>) and its machine. Never its admin
// password.
export interface FleetServer extends HostedServer {
  owner: AccountRef | null;
  service_account: AccountRef | null;
  provider: string;
  machine_id: string;
  machine_address: string;
  admin_port: number;
}

// GET /api/admin/hosted.
export interface FleetResponse {
  enabled: boolean;
  provider: string;
  // ONNET_HOSTED_CAP: the most servers live at once.
  cap: number;
  // How many are live now.
  live: number;
  // The newest 100, live ones first.
  servers: FleetServer[];
}

// POST /api/admin/hosted/<id>/stop.
export interface FleetServerReply {
  server: FleetServer | null;
}

// POST /api/admin/hosted/stop-all.
export interface StopAllReply {
  // How many live servers were asked to stop.
  stopping: number;
}
