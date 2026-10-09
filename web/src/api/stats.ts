import { apiClient } from './client';

export interface StatsResponse {
  stats: {
    games: number;
    lobbies: number;
    players: number;
    // Milliseconds since the Unix epoch, as /api/stats sends it.
    updatedAtMs: number;
  };
}

export async function fetchStats(signal?: AbortSignal): Promise<StatsResponse['stats']> {
  const response = await apiClient.get<StatsResponse>('/stats', { signal });
  return response.data.stats;
}
