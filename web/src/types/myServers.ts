// The logged-in account's listed servers, as GET /api/me/hosts answers them
// (snake_case; apps/novaworld_server/host_routes.cpp, README "Listed servers
// and owners").

// One player slot of the host's PlayerList. `slot` is what a player verb's
// ByIndex target names.
export interface RosterSlot {
  slot: number;
  player_name: string;
  team: string;
  type: string;
}

export interface MyServer {
  rid: number;
  gsid: string;
  game: string;
  server_name: string;
  mission_name: string;
  game_type: string;
  players: number;
  max_players: number;
  // Where joiners are sent.
  host_ip: string;
  host_port: number;
  // The address the server's NovaWorld session comes from.
  peer_ip: string;
  // UTC, 'YYYY-MM-DD HH:MM:SS'.
  created_at: string;
  updated_at: string;
  // The server's message.
  msg: string;
  roster: RosterSlot[];
}

export interface MyServersResponse {
  hosts: MyServer[];
}

// The ServerCommand verbs an owner may send (every verb but SetMPReset, which
// stays an admin's).
export type OwnerVerb =
  | 'TextChatServer'
  | 'TextChatPlayer'
  | 'CmdEchoPlayer'
  | 'PuntPlayer'
  | 'KillPlayer'
  | 'ChangeTeam'
  | 'SwapTeam'
  | 'Cycle'
  | 'EndMission'
  | 'GameOver'
  | 'Earthquake'
  | 'Lightning'
  | 'TimeOfDay'
  | 'SetServerName'
  | 'SetServerMsg'
  | 'ReloadPlayer'
  | 'DisarmPlayer';

export type CommandTarget = 'None' | 'ByIndex' | 'ByIpAndPort' | 'ByName' | 'ByPCID';

// POST /api/me/hosts/<rid>/command's body.
export interface ServerCommandBody {
  verb: OwnerVerb;
  target?: CommandTarget;
  args?: string[];
}

// A push the service queued for the server's NovaWorld session (202).
export interface PushQueued {
  rid: number;
  status: 'queued';
  statement: 'ServerCommand' | 'ServerStopHosting';
  // The composed line, for a ServerCommand.
  cmd?: string;
  // The stop's MsgCode (7, the sysop punt).
  msg_code?: number;
}
