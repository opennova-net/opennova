// Admin endpoints — token-gated. See `client.ts` for the bearer-token
// interceptor; the call sites here look like plain GETs/POSTs.

import { adminClient } from './client';
import type { ConnectionsResponse, ServerStatus } from '../types/admin';

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
