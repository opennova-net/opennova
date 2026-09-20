import { apiClient } from './client';
import type { ExpansionsResponse, ExpansionSummary } from '../types/expansions';

export async function fetchExpansions(signal?: AbortSignal): Promise<ExpansionSummary[]> {
  const response = await apiClient.get<ExpansionsResponse>('/expansions', { signal });
  return response.data.expansions;
}
