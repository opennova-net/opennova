// Admin user-management API. Goes through `adminClient`, which attaches
// the bearer token from local storage and clears it on 401.
//
// Public self-signup at /api/register goes through the plain
// `apiClient` so anonymous browsers can call it.

import { adminClient, apiClient } from './client';
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

export async function deleteUser(id: number): Promise<void> {
  await adminClient.delete(`/users/${id}`);
}

export async function registerUser(payload: RegisterPayload): Promise<User> {
  const res = await apiClient.post<UserResponse>('/register', payload);
  return res.data.user;
}
