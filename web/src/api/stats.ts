import axios from 'axios';

const fallbackBaseUrl =
  typeof window !== 'undefined'
    ? '/api'
    : 'http://localhost:8080/api';

const API_BASE_URL = import.meta.env.VITE_API_BASE_URL || fallbackBaseUrl;

export interface StatsResponse {
  stats: {
    games: number;
    lobbies: number;
    players: number;
    updatedAt: string;
  };
}

export async function fetchStats(signal?: AbortSignal): Promise<StatsResponse['stats']> {
  const response = await axios.get<StatsResponse>(`${API_BASE_URL}/stats`, { signal });
  return response.data.stats;
}
