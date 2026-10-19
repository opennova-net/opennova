// Hosted servers, as the NovaWorld service's routes answer them (snake_case;
// apps/novaworld_web/hosted/hosted_routes.cpp, README "Hosted servers").

import type { ServerAddress } from './addresses';
import type { TierNumber } from './billing';

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

// The owner's settings, as stored (hosted_settings.h HostedSettings). The
// backend range-checks each one (check_settings) against the data sets it
// offers; utils/hostedSettings.ts mirrors those rules.
export interface HostedSettings {
  // The data set the server runs (HostedDataSet.id): a base game the service
  // offers.
  data_set_id: number;
  // One of that data set's expansions (HostedExpansionOption.id), or null for
  // the base game alone.
  expansion_id: number | null;
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
  // The rotation in play order: the first is the map the server starts on.
  missions: string[];
}

// The settings in POST /api/me/servers's body: every field but game_name may
// be left out (the backend's default): data_set_id defaults to the first data
// set the service offers, expansion_id to none, and missions to every mission
// of the base and the picked expansion.
export type StartSettings = Pick<HostedSettings, 'game_name'> & Partial<Omit<HostedSettings, 'game_name'>>;

// POST /api/me/servers's body: the settings, whether the server runs on one
// of the account's own machines (the own-machine add-on) rather than a shared
// one, and on an own machine the static address it runs at (one of the
// account's ready ones; absent or null for none).
export type StartRequest = StartSettings & { own_machine?: boolean; address_id?: number | null };

// The whole-number settings, each with its range per data set.
export type HostedIntField = 'max_players' | 'time_limit' | 'kill_limit' | 'max_score' | 'respawn' | 'start_delay';

// A whole-number setting's range, both ends included.
export interface SettingRange {
  min: number;
  max: number;
}

// One multiplayer mission a data set's current tree holds: its .bms file and
// the title the game shows for it ('' when the archive names none).
export interface HostedMission {
  file: string;
  title: string;
}

// One expansion a base data set offers (an expansion data set with a ready
// tree): its id (the value of HostedSettings.expansion_id), its /exp code
// ('jox01'), the name the page shows, the version of its current tree and
// the missions that tree adds.
export interface HostedExpansionOption {
  id: number;
  code: string;
  display_name: string;
  version: string;
  missions: HostedMission[];
}

// A base data set the service offers hosted servers of (one with a ready
// current tree; ADR 0064): the title it speaks, the version of its current
// tree, the missions that tree holds, its expansions and the host screen's
// ranges.
export interface HostedDataSet {
  // The value of HostedSettings.data_set_id.
  id: number;
  display_name: string;
  // The title it speaks ('jo', 'dfx2').
  title: string;
  // Its current tree's version.txt.
  version: string;
  // The missions a rotation may hold (with an expansion's), in catalog order;
  // a start naming none takes all of them.
  missions: HostedMission[];
  // The host screen's ranges, per setting.
  limits: Record<HostedIntField, SettingRange>;
  expansions: HostedExpansionOption[];
}

// A data set a server runs, as its row names it.
export interface HostedServerDataSet {
  id: number;
  display_name: string;
}

export interface HostedServerExpansion {
  id: number;
  code: string;
  display_name: string;
}

// One tree a server is pinned to: the tree and its version.
export interface PinnedTree {
  tree_id: number;
  version: string;
}

// The data a server runs on: the trees it was pinned to when it started
// (null before placement, or for the base game alone).
export interface HostedServerData {
  base: PinnedTree | null;
  expansion: PinnedTree | null;
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

// Where a start stands, in its owner's words (hosted/model/server_phase.h): the
// states themselves, and within them what the fleet does for it.
export type ServerPhaseName =
  | 'queued'
  | 'restarting'
  | 'moving'
  | 'data_unavailable'
  | 'launches_held'
  | 'launching_machine'
  | 'waiting_for_slot'
  | 'waiting_for_address'
  | 'downloading_data'
  | 'downloading_server'
  | 'starting_server'
  | 'signing_in'
  | 'running'
  | 'stopping'
  | 'stopped'
  | 'failed';

export interface ServerPhase {
  name: ServerPhaseName | (string & {});
  // downloading_data: its trees' bytes fetched so far, of their manifests'.
  bytes_done?: number;
  bytes_total?: number;
  // restarting: restart `restart` of `restart_limit`, after a crash or a start
  // that timed out (`cause`).
  restart?: number;
  restart_limit?: number;
  cause?: 'crash' | 'timeout' | (string & {});
  // data_unavailable, on an admin's page alone: the machine's agent's reason
  // (the fetch's error).
  line?: string;
}

// Why a stopped or failed server's last run ended: its cause, how its
// container ended, and the server's own last line ('' for none).
export type StopCause =
  | 'crash'
  | 'timeout'
  | 'fleet'
  | 'stop'
  | 'update'
  | 'limits'
  | 'no_data'
  | 'link_lost'
  | 'data_unavailable';

export interface StoppedBecause {
  cause: StopCause | (string & {});
  exit_code: number | null;
  // It ran out of memory.
  oom: boolean;
  line: string;
}

// A hosted server, as its owner sees it. Times are UTC 'YYYY-MM-DD HH:MM:SS'.
export interface HostedServer {
  id: number;
  state: HostedState;
  // Any state but stopped and failed.
  live: boolean;
  // It runs on a machine of its own (the own-machine add-on), not a shared one.
  own_machine: boolean;
  // The static address it runs at (an own machine's), or null.
  address: ServerAddress | null;
  settings: HostedSettings;
  // The names the service gives the server's game and expansion ("Joint
  // Operations", "Escalation"); the expansion's '' for the base game alone.
  game_display_name: string;
  expansion_display_name: string;
  // The data set and expansion it runs, as their rows name them; null when
  // a row is gone.
  data_set: HostedServerDataSet | null;
  expansion: HostedServerExpansion | null;
  // The trees it runs on.
  data: HostedServerData;
  // Its data set (or expansion) has newer data than the trees it runs on: the
  // owner may update it (POST /api/me/servers/<id>/update), which restarts it.
  update_available: boolean;
  // An update was asked for: the supervisor restarts it on the current data
  // shortly.
  update_requested: boolean;
  // Crash restarts counted toward the limit (3 in the restart window).
  restarts: number;
  // Why it failed, in the service's words; null unless it did.
  failure_reason: string | null;
  created_at: string;
  updated_at: string;
  running_at: string | null;
  ended_at: string | null;
  listing: HostedListing | null;
  // Where its start stands; null where the reply carries none (the admin's
  // fleet list).
  phase: ServerPhase | null;
  // Why its last run ended, for a stopped or failed server; else null.
  stopped_because: StoppedBecause | null;
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
  // Whether a new server may start: false while a payment of the account's (a
  // tier's or an add-on's) is past due (the live ones keep running).
  can_start: boolean;
  source: EntitlementSource;
  // Its granting support tier, null for none.
  tier: TierNumber | null;
  // An admin grants the account hosting: it buys no support tier or add-on,
  // so the pages offer none.
  granted: boolean;
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
// none) and whether an admin grants them hosting (both may hold).
export interface FleetOwner {
  id: number;
  username: string;
  tier: 1 | 2 | 3 | null;
  granted: boolean;
}

// A machine's kind: a shared machine packs several servers, an own machine
// runs exactly one (the own-machine add-on).
export type MachineKind = 'shared' | 'own';

// launching -> booting -> ready -> draining -> ending -> ended | failed; lost
// when its reports stop.
export type MachineState = 'launching' | 'booting' | 'ready' | 'draining' | 'ending' | 'ended' | 'failed' | 'lost';

// What a machine's agent last said the machine uses: its CPU (0 to 100 of all
// its CPUs), memory, the trees' disk, and its network (counters since boot, and
// the rate between its two newest minutes; null for none yet).
export interface MachineStats {
  cpu_percent: number | null;
  memory_used_bytes: number | null;
  memory_total_bytes: number | null;
  disk_free_bytes: number | null;
  disk_total_bytes: number | null;
  net_in_bytes: number | null;
  net_out_bytes: number | null;
  net_in_bps: number | null;
  net_out_bps: number | null;
}

// A slot of a machine that holds a server (machine_json's slot_holders, from
// the slot assignments): its number (it answers UDP 32768+slot), the server
// and the server's state. On the machine's own page (GET
// /api/admin/fleet/machines/<id>) each also names its server ('' for a server
// with no name), its game port and its last minute of ticks (as FleetServer's
// tick), so the page reads no fleet list; the fleet view's leave them out.
export interface SlotHolder {
  slot: number;
  server_id: number;
  state: HostedState | (string & {});
  name?: string;
  game_port?: number | null;
  tick?: FleetServer['tick'];
}

// A machine of the fleet (GET /api/admin/hosted's `machines`, machine_json).
// Times are UTC; '' or null where none is known.
export interface FleetMachine {
  // The service's fleet_machines row (what machine_ref and the routes name).
  id: string;
  // The cloud's handle of it (an EC2 instance id); '' before its launch answered.
  cloud_id: string;
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
  // Its agent: hostd's version, the protocol its last sync spoke (null before
  // one), the image its ECS task runs ('' off ECS), its own log stream, and
  // since when its syncs speak another protocol than the service's (held, not
  // drained, for 10 minutes).
  agent_version: string;
  agent_protocol: number | null;
  agent_image: string;
  agent_log_stream: string | null;
  protocol_mismatch_since: string | null;
  // Lost, and kept from its end by the supervisor's mass-lost brake.
  lost_held: boolean;
  // The machine's image (an AMI id) as its agent reported it.
  image_id: string | null;
  last_report_at: string | null;
  launched_at: string | null;
  ready_at: string | null;
  ended_at: string | null;
  end_reason: string | null;
  // Its use at its last report (stats_at); null before one.
  stats: MachineStats | null;
  stats_at: string | null;
  // What the release pins: the machines' image (ONNET_FLEET_IMAGE_ID) and
  // hostd's; null where none or unknown.
  release: { image_id: string | null; agent_image: string | null };
  // Its agent's image or its machine's differs from the release's; null when
  // that is not known.
  outdated: boolean | null;
  // The static address it carries (an own machine's), or null.
  address: { id: number; name: string } | null;
  // Its slots that hold a server, one each (slots_used of them); whatever the
  // fleet list shows.
  slot_holders: SlotHolder[];
}

// One of a machine's moves (fleet_machine_events): from a state to another,
// or a note with the two the same ("log stream <name>"); `from` null at the
// row's making.
export interface MachineEvent {
  from: string | null;
  to: string;
  detail: string;
  at: string;
}

// A tree a machine's agent holds or fetches, as it last said.
export interface MachineTree {
  tree_id: number;
  state: 'fetching' | 'ready' | 'failed' | (string & {});
  // Fetched so far, of the tree's manifest's (null when unknown).
  bytes: number;
  total_bytes: number | null;
  tries: number;
  error: string | null;
  reported_at: string | null;
}

// GET /api/admin/fleet/machines/<id>: the machine, its newest 100 events
// oldest first, and its trees.
export interface MachineDetail {
  machine: FleetMachine;
  events: MachineEvent[];
  trees: MachineTree[];
}

// One machine in a fleet roll's preview (GET /api/admin/fleet/roll): whether
// the roll replaces it (another image than the launch templates', or one not
// known) or keeps it, and why.
export interface RollStep {
  id: string;
  kind: MachineKind;
  state: string;
  image_id: string | null;
  servers: number;
  action: 'replace' | 'keep';
  why: string;
}

export interface RollPlan {
  // The image the launch templates pin; null for none.
  image_id: string | null;
  machines: RollStep[];
  // The ids the preview would replace.
  replace: string[];
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
  // The trees it is pinned to; null before placement (the expansion's for
  // the base game alone).
  base_tree_id: number | null;
  expansion_tree_id: number | null;
  // It runs on an older tree than its data set's current one.
  outdated: boolean;
  // Its starts so far (each start a new generation; 0 before its first).
  generation: number;
  // Its tick health in its slot's latest report: its frames' 99th percentile
  // time and the late ticks of the last minute; null with no report or no
  // tick in it.
  tick: { p99_ms: number | null; late: number | null } | null;
  // GET /api/admin/hosted/<id> only: its machine.
  machine?: FleetMachine | null;
}

// The live servers of one machine kind, and the most the fleet's machines of
// that kind hold.
export interface FleetRoom {
  live: number;
  room: number;
}

// GET /api/admin/hosted.
export interface FleetResponse {
  enabled: boolean;
  // ONNET_HOSTED_CAP: the most servers live at once.
  cap: number;
  // How many are live now.
  live: number;
  shared: FleetRoom;
  own: FleetRoom;
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

// The console routes (apps/novaworld_web/hosted/console_routes.cpp, README
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
