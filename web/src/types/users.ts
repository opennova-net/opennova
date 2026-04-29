export interface User {
  id: number;
  username: string;
  pcid: string;
  nwh: string;
  nwhandle: string;
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
}

export interface RegisterPayload {
  username: string;
  password: string;
  nwhandle?: string;
}
