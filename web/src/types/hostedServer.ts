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

// An expansion's code, as the server's /exp takes it: '' for the data set's
// base game ('jox01', Escalation, for Joint Operations). The data set names
// the ones it allows (HostedDataSet.expansions).
export type HostedExpansion = string;

// The owner's settings, as stored (hosted_settings.h HostedSettings). The
// backend range-checks each one (check_settings) against the server's data
// set; utils/hostedSettings.ts mirrors those rules.
export interface HostedSettings {
  // The data set the server runs: its games slug, the lobby it lists in
  // ('jop_2_consumer', 'dfx2_consumer', 'opennova_0_consumer').
  game: string;
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

// The settings in POST /api/me/servers's body: every field but game_name may
// be left out (the backend's default): game defaults to the first data set the
// service offers, and missions to that data set's rotation (its "missions" in
// ONNET_HOSTED_DATA_SETS).
export type StartSettings = Pick<HostedSettings, 'game_name'> & Partial<Omit<HostedSettings, 'game_name'>>;

// POST /api/me/servers's body: the settings, and whether the server runs on
// one of the account's own machines (the own-machine add-on) rather than a
// shared one.
export type StartRequest = StartSettings & { own_machine?: boolean };

// The whole-number settings, each with its range per data set.
export type HostedIntField = 'max_players' | 'time_limit' | 'kill_limit' | 'max_score' | 'respawn' | 'start_delay';

// A whole-number setting's range, both ends included.
export interface SettingRange {
  min: number;
  max: number;
}

// One expansion a data set allows: its code ('' for the base game) and the
// name the page shows.
export interface HostedExpansionOption {
  code: HostedExpansion;
  name: string;
}

// A data set the service offers hosted servers of (hosted/data_sets.h): the
// game folder a server mounts, the title it speaks and the lobby it lists in.
export interface HostedDataSet {
  // Its games slug, the value of HostedSettings.game.
  game: string;
  display_name: string;
  // The base game ('') first.
  expansions: HostedExpansionOption[];
  // The deployment's rotation for it (its "missions" in ONNET_HOSTED_DATA_SETS),
  // which a start naming no missions takes, and the missions the rotation
  // picker offers; empty when none is configured, and a start must then name
  // its own.
  default_missions: string[];
  // The host screen's ranges, per setting.
  limits: Record<HostedIntField, SettingRange>;
}

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
  // It runs on a machine of its own (the own-machine add-on), not a shared one.
  own_machine: boolean;
  settings: HostedSettings;
  // The names the service gives the server's game and expansion ("Joint
  // Operations", "Escalation (jox01)" or "Joint Operations (base game)").
  game_display_name: string;
  expansion_display_name: string;
  // Crash restarts counted toward the limit (3 in the restart window).
  restarts: number;
  // Why it failed, in the service's words; null unless it did.
  failure_reason: string | null;
  created_at: string;
  updated_at: string;
  running_at: string | null;
  ended_at: string | null;
  listing: HostedListing | null;
  // The newest 50 transitions, oldest first: GET /api/me/servers/<id> only
  // (the list, the start and the stop replies leave them out).
  events?: HostedEvent[];
}

// Where an account's hosting entitlement comes from: nothing, its support
// tier, or an admin's comp grant.
export type EntitlementSource = 'none' | 'tier' | 'comp';

// The account's hosting entitlement, as GET /api/me/servers counts it.
export interface HostingEntitlement {
  // How many hosted servers the account may have live at once; 0 locks
  // hosting.
  max_servers: number;
  // How many it has live now, stopping ones included (a stopping server holds
  // its place until it has stopped).
  used: number;
  // The own-machine add-ons it holds, and how many of its live servers run on
  // one.
  own_machines: number;
  own_used: number;
  // Whether a new server may start: false while the support tier's payment is
  // past due (the live ones keep running).
  can_start: boolean;
  source: EntitlementSource;
}

// GET /api/me/servers.
export interface HostedServersResponse {
  // The service runs hosted servers (it has a fleet and offers at least one
  // data set).
  enabled: boolean;
  // Every hosted server place on the service is taken: a start now would be
  // refused (at_capacity).
  full: boolean;
  entitlement: HostingEntitlement;
  // The data sets a start may pick, in the service's order (the first is a
  // start's default); only the ones this deployment configured.
  data_sets: HostedDataSet[];
  // The live servers by id, oldest first, then the newest 10 ended ones,
  // newest first; without their events.
  servers: HostedServer[];
}

// GET /api/me/servers/<id>, and the start and stop replies.
export interface HostedServerReply {
  server: HostedServer;
}

// The owner named in the fleet view, with their support tier (null for
// none: a comp grant, or no entitlement left).
export interface FleetOwner {
  id: number;
  username: string;
  tier: 1 | 2 | 3 | null;
}

// A machine's kind: a shared machine packs several servers, an own machine
// runs exactly one (the own-machine add-on).
export type MachineKind = 'shared' | 'own';

// launching -> booting -> ready -> draining -> ending -> ended | failed; lost
// when its reports stop.
export type MachineState = 'launching' | 'booting' | 'ready' | 'draining' | 'ending' | 'ended' | 'failed' | 'lost';

// A machine of the fleet (GET /api/admin/hosted's `machines`).
export interface FleetMachine {
  id: string;
  kind: MachineKind;
  state: MachineState | (string & {});
  // The EC2 instance type ('t3a.large'), or the local driver's name.
  instance_type: string;
  // Its server positions, and how many hold a live server.
  slots: number;
  slots_used: number;
  public_address: string;
  // Bytes sent to players this calendar month, all its servers together.
  egress_bytes_month: number;
}

// A hosted server in the admin's fleet view: the owner's view plus who owns
// it and where it runs. Never its admin password.
export interface FleetServer extends HostedServer {
  owner: FleetOwner | null;
  // The machine it is placed on (its id), its slot there and its game port;
  // null before placement.
  machine_ref: string | null;
  slot: number | null;
  game_port: number | null;
  // Players on it now, as its listing says; null while not listed.
  players: number | null;
  // GET /api/admin/hosted/<id> only: its machine.
  machine?: FleetMachine | null;
}

// GET /api/admin/hosted.
export interface FleetResponse {
  enabled: boolean;
  // ONNET_HOSTED_CAP: the most servers live at once.
  cap: number;
  // How many are live now.
  live: number;
  machines: FleetMachine[];
  // The filter's servers, live ones first.
  servers: FleetServer[];
}

// GET /api/admin/hosted's filter.
export interface FleetQuery {
  // An owner's username.
  owner?: string;
  state?: 'live' | 'ended' | 'all';
}

// POST /api/admin/hosted/stop-all.
export interface StopAllReply {
  // How many live servers were asked to stop.
  stopping: number;
}

// The console routes (apps/novaworld_server/hosted/console_routes.cpp, README
// "Hosted servers", the console routes' table): the owner's on
// /api/me/servers/<id>/..., the admin's on /api/admin/hosted/<id>/....

// One PLAYER LIST row (the server's own slot-0 row left out). `team` and
// `class` are the numbers the server prints.
export interface ConsolePlayer {
  // 1..255: what punt, ban, swap and kill name.
  slot: number;
  name: string;
  team: number;
  class: number;
  kills: number;
  deaths: number;
  ping: number;
}

// One MISSION AVAILABLE row: a mission the server has, which an add names by
// its `file`.
export interface ConsoleMission {
  index: number;
  file: string;
  title: string;
}

// One MISSION LIST entry, in play order. `index` is what remove and next name.
export interface RotationEntry {
  index: number;
  file: string;
  // Sides swapped at the half (the add's swap_sides).
  swap_sides: boolean;
  // The current map is in its second half.
  flipped: boolean;
  one_shot: boolean;
  current: boolean;
  next: boolean;
}

// The service's admin link to the server, as GET .../console reports it.
export interface ConsoleLink {
  state: 'connecting' | 'up' | 'down' | 'stopped';
  // The link's last poll's scene: 'Game' (a match), 'Menus' (between
  // matches) or 'Unknown'; '' before one.
  game_state: string;
  // UTC 'YYYY-MM-DD HH:MM:SS', '' before one.
  last_poll_at: string;
  // UTC, the session's login; '' while not up.
  up_since: string;
  // Why the last login or session failed; '' while up.
  error: string;
}

// One line of the server's console ring (its newest 500): each command the
// site sent that changes the server (with its sender), each reply, and the
// link's own status lines. The reads the pages poll are not recorded.
export interface ConsoleLine {
  id: number;
  kind: 'command' | 'reply' | 'status';
  // A command's sender: 'owner', 'admin' or 'service'; '' for the rest.
  actor: string;
  text: string;
  // UTC 'YYYY-MM-DD HH:MM:SS'.
  at: string;
}

// GET .../console (readable on an ended server too).
export interface ConsoleLog {
  server_id: number;
  // Null while the server has no link (not running).
  link: ConsoleLink | null;
  // The lines past `after`, oldest first.
  lines: ConsoleLine[];
}

// The player commands, by the route's last segment.
export type PlayerAction = 'punt' | 'ban' | 'swap' | 'kill';

// POST .../players/<slot>/<action>.
export interface PlayerActionReply {
  slot: number;
  result: string;
  // A ban: whether the server listed the player's PCID (a NovaWorld host's
  // ban) before it punted them.
  listed?: boolean;
}

// The rotation entry commands, by the route's last segment.
export type RotationEntryAction = 'remove' | 'next';
