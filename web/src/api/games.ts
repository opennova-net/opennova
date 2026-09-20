import { apiClient } from './client';
import type { GamesResponse } from '../types/games';

export async function fetchGames(signal?: AbortSignal): Promise<GamesResponse> {
  const response = await apiClient.get<GamesResponse>('/games', { signal });
  return response.data;
}
