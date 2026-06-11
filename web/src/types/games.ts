import type { ExpansionSummary } from './expansions';

export interface GameSummary {
  slug: string;
  displayName: string;
  executableName: string;
  expansions: ExpansionSummary[];
}

export interface GamesResponse {
  games: GameSummary[];
}
