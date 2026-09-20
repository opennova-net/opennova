import { apiClient } from './client';

export interface StatsResponse {
  stats: {
    games: number;
    lobbies: number;
    players: number;
    updatedAt: string;
  };
}

export async function fetchStats(signal?: AbortSignal): Promise<StatsResponse['stats']> {
  const response = await apiClient.get<StatsResponse>('/stats', { signal });
  return response.data.stats;
}
