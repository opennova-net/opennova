import { isAxiosError } from 'axios';
import { apiClient } from './client';
import type { LobbyResponse, LobbyServer, NovaLogicLobbies } from '../types/lobby';

export async function fetchLobbies(signal?: AbortSignal): Promise<LobbyResponse> {
  const response = await apiClient.get<LobbyResponse>('/lobbies', { signal });
  return response.data;
}

// GET /api/lobbies/novalogic: the servers on NovaLogic's NovaWorld, from the
// service's own copy of the list (the page never reaches NovaLogic itself).
export async function fetchNovaLogicLobbies(signal?: AbortSignal): Promise<NovaLogicLobbies> {
  const response = await apiClient.get<NovaLogicLobbies>('/lobbies/novalogic', { signal });
  return response.data;
}

// The listing is gone (404): its page says so and stops reading.
export class LobbyGone extends Error {}

// GET /api/lobbies/<id>: one listed server's page.
export async function fetchLobbyServer(id: number, signal?: AbortSignal): Promise<LobbyServer> {
  try {
    const response = await apiClient.get<LobbyServer>(`/lobbies/${id}`, { signal });
    return response.data;
  } catch (e) {
    if (isAxiosError(e) && e.response?.status === 404) throw new LobbyGone();
    throw e;
  }
}
