// A website role (players.role): every account is a `player`; `admin` opens
// the /api/admin/* routes to the account's session.
export type Role = 'player' | 'admin';

export interface User {
  id: number;
  username: string;
  pcid: string;
  nwh: string;
  nwhandle: string;
  account_status: string;
  role: Role;
  // An admin approved the account for a hosted server (players.can_host).
  can_host: boolean;
}

export interface UsersListResponse {
  users: User[];
}

export interface UserResponse {
  user: User;
}

export interface CreateUserPayload {
  username: string;
  password: string;
  pcid: string;
  nwhandle: string;
  nwh?: string;
}

export interface UpdateUserPayload {
  username?: string;
  password?: string;
  pcid?: string;
  nwhandle?: string;
  nwh?: string;
  role?: Role;
}

export interface RegisterPayload {
  username: string;
  password: string;
  nwhandle?: string;
}
