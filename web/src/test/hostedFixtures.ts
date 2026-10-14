// Data sets and settings for the hosted server tests, shaped as GET
// /api/me/server answers them. DFX2's limits differ from JO's here on purpose,
// so a test can tell whose ranges a form applies.

import type { HostedDataSet, HostedSettings } from '../types/hostedServer';

export const JO: HostedDataSet = {
  game: 'jop_2_consumer',
  display_name: 'Joint Operations',
  expansions: [
    { code: '', name: 'Joint Operations (base game)' },
    { code: 'jox01', name: 'Escalation (jox01)' },
  ],
  default_missions: ['jo_one.bms', 'jo_two.bms'],
  limits: {
    max_players: { min: 2, max: 64 },
    time_limit: { min: 1, max: 300 },
    kill_limit: { min: 1, max: 500 },
    max_score: { min: 1, max: 500 },
    respawn: { min: 0, max: 300 },
    start_delay: { min: 0, max: 300 },
  },
};

export const DFX2: HostedDataSet = {
  game: 'dfx2_consumer',
  display_name: 'Delta Force Xtreme 2',
  expansions: [{ code: '', name: 'Delta Force Xtreme 2 (base game)' }],
  default_missions: ['dfx_one.bms'],
  limits: {
    max_players: { min: 2, max: 50 },
    time_limit: { min: 1, max: 120 },
    kill_limit: { min: 1, max: 250 },
    max_score: { min: 1, max: 250 },
    respawn: { min: 0, max: 60 },
    start_delay: { min: 0, max: 60 },
  },
};

export function settings(overrides: Partial<HostedSettings> = {}): HostedSettings {
  return {
    game: JO.game,
    game_name: 'Test Server',
    max_players: 32,
    game_password: '',
    time_limit: 30,
    kill_limit: 50,
    max_score: 5,
    respawn: 5,
    start_delay: 0,
    team_ff: false,
    expansion: '',
    missions: ['jo_one.bms'],
    ...overrides,
  };
}
