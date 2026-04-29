import axios from 'axios';
import type { GamesResponse } from '../types/games';

const fallbackBaseUrl =
  typeof window !== 'undefined'
    ? '/api'
    : 'http://localhost:8080/api';

const API_BASE_URL = import.meta.env.VITE_API_BASE_URL || fallbackBaseUrl;

export async function fetchGames(signal?: AbortSignal): Promise<GamesResponse> {
  const response = await axios.get<GamesResponse>(`${API_BASE_URL}/games`, { signal });
  return response.data;
}
