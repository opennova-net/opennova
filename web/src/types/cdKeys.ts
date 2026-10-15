// The CD keys' admin routes (apps/novaworld_server/cd_key_routes.h), in the
// server's snake_case. Times are the service's UTC 'YYYY-MM-DD HH:MM:SS'
// (utils/format.ts), null until set.

export type CdKeyState = 'unassigned' | 'assigned' | 'revoked';

export interface CdKeyAccount {
  id: number;
  username: string;
}

export interface CdKey {
  id: number;
  // The game's code ('dfx2').
  title: string;
  // The plaintext the key carries, what an admin hands a player.
  serial: string;
  // The CDKIID an activated copy keeps and a LAN host checks.
  wire_key: string;
  state: CdKeyState;
  account: CdKeyAccount | null;
  // "admin <username>", "token", or "auto" (minted at the account's first activation).
  created_by: string;
  created_at: string;
  assigned_at: string | null;
  revoked_at: string | null;
  // How many machines it activated.
  activations: number;
}

export interface CdKeyActivation {
  id: number;
  key_id: number;
  title: string;
  serial: string;
  // The machine's fingerprint, as the game posts it.
  nwpssk: string;
  // The value the machine's registry CDKIID took.
  nwact: string;
  created_at: string;
}

export interface CdKeyListResponse {
  total: number;
  limit: number;
  offset: number;
  // The games keys are issued for.
  titles: string[];
  // Whether an account's first activation mints its key (ONNET_CD_KEY_AUTO_MINT).
  auto_mint: boolean;
  keys: CdKey[];
}

export interface CdKeyFilter {
  title?: string;
  state?: CdKeyState | '';
  q?: string;
  limit?: number;
  offset?: number;
}

export interface CdKeyAccountResponse {
  account: CdKeyAccount;
  keys: CdKey[];
  activations: CdKeyActivation[];
}

export interface CdKeyActivationResult {
  nwact: string;
  reactivated: boolean;
  key: CdKey;
}
