import axios from 'axios';
import type { ExpansionsResponse, ExpansionSummary } from '../types/expansions';

const fallbackBaseUrl =
  typeof window !== 'undefined'
    ? '/api'
    : 'http://localhost:8080/api';

const API_BASE_URL = import.meta.env.VITE_API_BASE_URL || fallbackBaseUrl;

export async function fetchExpansions(signal?: AbortSignal): Promise<ExpansionSummary[]> {
  const response = await axios.get<ExpansionsResponse>(`${API_BASE_URL}/expansions`, { signal });
  return response.data.expansions;
}
