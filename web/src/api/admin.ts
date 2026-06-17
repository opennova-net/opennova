// Admin endpoints — token-gated. See `client.ts` for the bearer-token
// interceptor; the call sites here look like plain GETs/POSTs.

import { adminClient } from './client';
import type {
  AdminExpansionsResponse,
  AdminReleasesResponse,
  CreateReleaseRequest,
  CreateReleaseResponse,
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
