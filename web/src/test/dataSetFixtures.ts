// Data sets and trees for the game data admin tests, shaped as
// GET /api/admin/data-sets answers them: JO (id 1) with a ready tree and a
// live server, Escalation (id 2) under it with an uploading tree, DFX2 (id 3)
// with no data.

import type { DataSet, DataSetsResponse, Tree } from '../types/dataSets';

export function tree(id: number, overrides: Partial<Tree> = {}): Tree {
  const state = overrides.state ?? 'ready';
  return {
    id,
    state,
    version: '1.7.5.5',
    bytes: 1_250_000_000,
    files: 7,
    missions: 54,
    pinned_by: 0,
    error: '',
    created_by: 'admin root',
    created_at: '2026-10-16 10:00:00',
    ready_at: state === 'ready' ? '2026-10-16 10:12:00' : null,
    ...overrides,
  };
}

export function dataSet(id: number, overrides: Partial<DataSet> = {}): DataSet {
  return {
    id,
    display_name: `Data set ${id}`,
    kind: 'base',
    parent_id: null,
    expansion_code: '',
    title: 'jo',
    game: 'jop_2_consumer',
    current_tree: null,
    servers_live: 0,
    servers_outdated: 0,
    trees: [],
    ...overrides,
  };
}

export const JO_SET: DataSet = dataSet(1, {
  display_name: 'Joint Operations',
  current_tree: { id: 10, version: '1.7.5.5', ready_at: '2026-10-16 10:12:00' },
  servers_live: 3,
  servers_outdated: 1,
  trees: [tree(10, { pinned_by: 2 }), tree(11, { state: 'failed', version: '', error: 'version.txt is missing', bytes: 12, files: 1, missions: 0, created_at: '2026-10-15 09:00:00' })],
});

export const ESCALATION_SET: DataSet = dataSet(2, {
  display_name: 'Escalation',
  kind: 'expansion',
  parent_id: 1,
  expansion_code: 'jox01',
  title: '',
  game: '',
  trees: [tree(12, { state: 'uploading', version: '', bytes: 0, files: 0, missions: 0, ready_at: null })],
});

export const DFX2_SET: DataSet = dataSet(3, { display_name: 'Delta Force Xtreme 2', title: 'dfx2', game: 'dfx2_consumer' });

export function dataSetsResponse(overrides: Partial<DataSetsResponse> = {}): DataSetsResponse {
  return {
    store: { kind: 'local' },
    titles: ['jo', 'dfx2'],
    games: [
      { slug: 'jop_2_consumer', display_name: 'Joint Operations' },
      { slug: 'dfx2_consumer', display_name: 'Delta Force Xtreme 2' },
      { slug: 'opennova_0_consumer', display_name: 'OpenNova' },
    ],
    data_sets: [JO_SET, ESCALATION_SET, DFX2_SET],
    ...overrides,
  };
}
