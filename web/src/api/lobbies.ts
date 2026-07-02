import axios from 'axios';
import type { LobbyResponse } from '../types/lobby';

const fallbackBaseUrl =
  typeof window !== 'undefined'
    ? '/api'
    : 'http://localhost:8080/api';

const API_BASE_URL = import.meta.env.VITE_API_BASE_URL || fallbackBaseUrl;

export async function fetchLobbies(signal?: AbortSignal): Promise<LobbyResponse> {
  const response = await axios.get<LobbyResponse>(`${API_BASE_URL}/lobbies`, { signal });
  return response.data;
}
