// Admin user-management API. Goes through `adminClient`: an admin-role
// session's cookie authorizes it, or the bearer token from local storage,
// which the client attaches when one is stored and drops when refused.
//
// Public self-signup at /api/register goes through the plain
// `apiClient` so anonymous browsers can call it.

import { adminClient, apiClient } from './client';
import { describeAdminError, malformedAdminReply, type AdminResult } from './admin';
import type {
  CreateUserPayload,
  RegisterPayload,
  UpdateUserPayload,
  User,
  UserResponse,
  UsersListResponse,
} from '../types/users';

export async function listUsers(signal?: AbortSignal): Promise<User[]> {
  const res = await adminClient.get<UsersListResponse>('/users', { signal });
  return res.data.users;
}

export async function createUserAdmin(payload: CreateUserPayload): Promise<User> {
  const res = await adminClient.post<UserResponse>('/users', payload);
  return res.data.user;
}

export async function updateUser(id: number, payload: UpdateUserPayload): Promise<User> {
  const res = await adminClient.put<UserResponse>(`/users/${id}`, payload);
  return res.data.user;
}

// PUT /api/admin/users/<id>/can-host: the account's approval for a hosted
// server. Withdrawing it makes the service stop the account's live hosted
// server, as a ban does. 404 (`not_found`) for an unknown account.
export async function setCanHost(id: number, canHost: boolean): Promise<AdminResult<User>> {
  try {
    const res = await adminClient.put<UserResponse>(`/users/${id}/can-host`, { can_host: canHost });
    const user = res.data?.user;
    if (typeof user?.id !== 'number' || typeof user.can_host !== 'boolean') return malformedAdminReply(res.status);
    return { ok: true, value: user };
  } catch (err) {
    return { ok: false, error: describeAdminError(err) };
  }
}

export async function deleteUser(id: number): Promise<void> {
  await adminClient.delete(`/users/${id}`);
}

export async function registerUser(payload: RegisterPayload): Promise<User> {
  const res = await apiClient.post<UserResponse>('/register', payload);
  return res.data.user;
}
