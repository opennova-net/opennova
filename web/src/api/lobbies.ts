import { apiClient } from './client';
import type { LobbyResponse } from '../types/lobby';

export async function fetchLobbies(signal?: AbortSignal): Promise<LobbyResponse> {
  const response = await apiClient.get<LobbyResponse>('/lobbies', { signal });
  return response.data;
}
