// Admin endpoints — token-gated. See `client.ts` for the bearer-token
// interceptor; the call sites here look like plain GETs/POSTs.

import { adminClient } from './client';
import type {
  AdminExpansionsResponse,
  AdminReleasesResponse,
  ConnectionsResponse,
  CreateReleaseRequest,
  CreateReleaseResponse,
  ServerStatus,
} from '../types/admin';

export async function fetchAdminExpansions(signal?: AbortSignal) {
  const response = await adminClient.get<AdminExpansionsResponse>('/expansions', { signal });
  return response.data.expansions;
}

export async function fetchAdminReleases(limit = 20, signal?: AbortSignal) {
  const response = await adminClient.get<AdminReleasesResponse>('/releases', {
    params: { limit },
    signal,
  });
  return response.data.releases;
}

export async function createExpansionRelease(
  slug: string,
  payload: CreateReleaseRequest,
): Promise<CreateReleaseResponse> {
  const response = await adminClient.post<CreateReleaseResponse>(
    `/expansions/${encodeURIComponent(slug)}/release`,
    payload,
  );
  return response.data;
}

export async function fetchServerStatus(signal?: AbortSignal): Promise<ServerStatus> {
  const response = await adminClient.get<ServerStatus>('/server-status', { signal });
  return response.data;
}

export async function updateServerStatus(payload: ServerStatus): Promise<ServerStatus> {
  const response = await adminClient.put<ServerStatus>('/server-status', payload);
  return response.data;
}

export async function fetchConnections(signal?: AbortSignal): Promise<ConnectionsResponse> {
  const response = await adminClient.get<ConnectionsResponse>('/connections', { signal });
  return response.data;
}
